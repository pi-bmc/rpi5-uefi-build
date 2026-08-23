/** @file

  ActiveCoolerScmiDxe - closed-loop fan control for the Raspberry Pi 5
  active cooler during the firmware phase, as an SCMI agent.

  Successor to ActiveCoolerDxe (which drives the RP1 PWM directly and
  remains in the tree as the reference for builds without OP-TEE): this
  platform builds ONLY this variant, because OP-TEE owns the fan PWM
  exclusively and every agent must go through its SCMI server.

  Nothing regulates the fan between the VPU's handoff and the OS coming up,
  so a long UEFI session (Setup, netboot retries, the BMC holding the host
  at the firmware menu) runs the SoC with whatever airflow the bootloader
  last commanded. This driver closes the loop: a 1 s timer reads the SoC
  temperature and maps it onto the cooling policy - by default the exact
  curve the VPU DTB ships for Linux's pwm-fan:

    level  duty/255  trip (up)   release (down, 5 C hysteresis)
      0        0        -            < 45.0 C
      1       75      >= 50.0 C      < 55.0 C
      2      125      >= 60.0 C      < 62.5 C
      3      175      >= 67.5 C      < 70.0 C
      4      250      >= 75.0 C        -

  Control precedence, decided fresh every tick:

    1. A volatile override staged through RPI_FAN_PROTOCOL.SetOverride
       (the BMC steering via RpiRedfishSyncDxe, or any in-firmware tool).
    2. The persistent FanPolicy variable (FanConfigDxe's Setup page or a
       BMC-side variable write): FIXED pins a level, CUSTOM replaces the
       trip temperatures (duties and hysteresis stay), AUTO is the table
       above. The variable is re-read every tick, so Setup changes apply
       live without a reset; a missing or invalid variable means AUTO.
    3. The automatic loop.

  HARDWARE ACCESS IS ALL SCMI. OP-TEE owns the RP1 fan PWM exclusively
  (plat-rpi5 rp1_pwm.c, including the GPIO45 mux) and the AVS temperature
  read, and serves both over the SCMI SMT channel that Linux's arm,scmi-smc
  uses at runtime: sensor protocol 0x15 (sensor 0 "soc", milli-C) and
  performance-domain protocol 0x13 (domain 0 "fan", levels 0..4). This
  driver speaks the same wire - a 128-byte SMT message in the shared page
  rung by the SiP fast SMC - so there is exactly one owner of the PWM
  registers in the system and the firmware agent simply stops calling at
  ExitBootServices, floored at level >= 1 so an OS without an SCMI stack
  inherits real airflow. (ArmPkg's ArmScmiDxe was considered and skipped:
  it brings the Base/Clock/Performance protocols over an ArmMtlLib port
  but no Sensor protocol, so a custom client is needed either way.)

  The SMT + doorbell wire lives in RpiScmiLib (shared with
  RpiScmiConfigDxe); see RpiScmiLib.h for the cross-component contract.

  The fan set path returns SCMI errors until EDK2's RpiOpteeSensorDxe
  delivers the RP1 BAR to OP-TEE (the PCIe late-init handshake); the loop
  just retries next tick, exactly as it used to wait for Rp1BusDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/RpiScmiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/RpiFanPolicy.h>
#include <Guid/RpiScmiConfig.h>
#include <Protocol/RpiFan.h>

//
// --- SCMI access (RpiScmiLib does the SMT + doorbell wire) ---------------
//

/**
  Read the SoC temperature over SCMI. Returns FALSE while the transport or
  the sensor is not answering.
**/
STATIC
BOOLEAN
ScmiReadMilliCelsius (
  OUT INT32  *MilliC
  )
{
  UINT32  In[2];
  UINT32  Out[3];

  In[0] = RPI_SCMI_SENSOR_ID_SOC_TEMP;
  In[1] = 0;                                       // synchronous read

  if (EFI_ERROR (
        RpiScmiCall (
          RPI_SCMI_PROTOCOL_SENSOR,
          RPI_SCMI_SENSOR_READING_GET,
          In,
          2,
          Out,
          3
          )
        ) ||
      ((INT32)Out[0] != 0))
  {
    return FALSE;
  }

  //
  // Sensor 0 reports milli-Celsius (scale -3) in the 64-bit value; the low
  // word carries the whole range this SoC can produce.
  //
  *MilliC = (INT32)Out[1];
  return TRUE;
}

/**
  Command a fan level over SCMI (perf domain 0, level == cooling level).
  Fails (and is retried next tick) until OP-TEE has the RP1 BAR.
**/
STATIC
BOOLEAN
ScmiFanSetLevel (
  IN UINTN  Level
  )
{
  UINT32  In[2];
  UINT32  Out[1];

  In[0] = RPI_SCMI_PERF_DOMAIN_FAN;
  In[1] = (UINT32)Level;

  if (EFI_ERROR (
        RpiScmiCall (
          RPI_SCMI_PROTOCOL_PERF,
          RPI_SCMI_PERF_LEVEL_SET,
          In,
          2,
          Out,
          1
          )
        ) ||
      ((INT32)Out[0] != 0))
  {
    return FALSE;
  }

  return TRUE;
}

//
// --- Cooling policy (unchanged from the direct-PWM incarnation) ------------
//

//
// Duty values are OP-TEE's (rp1_pwm.c fan_duty255), mirrored here only for
// RPI_FAN_PROTOCOL.GetInfo reporting.
//
typedef struct {
  INT32     TripMilliC;   // enter this level at or above
  UINT32    Duty255;      // pwm-fan cooling-level, out of 255
} FAN_LEVEL;

STATIC CONST FAN_LEVEL  mFanLevels[] = {
  { 0,     0   },
  { 50000, 75  },
  { 60000, 125 },
  { 67500, 175 },
  { 75000, 250 },
};

#define FAN_LEVEL_COUNT  (sizeof (mFanLevels) / sizeof (mFanLevels[0]))
#define FAN_LEVEL_MAX    (FAN_LEVEL_COUNT - 1)
#define FAN_LEVEL_SAFE   2    // commanded while the sensor reads invalid

//
// Custom-trip sanity window, in whole C (matches the Setup page's numeric
// limits; a BMC-written variable gets the same treatment).
//
#define FAN_TRIP_MIN_C  30
#define FAN_TRIP_MAX_C  90

#define FAN_POLL_INTERVAL  10000000ULL         // 1 s in 100 ns units
#define FAN_HYST_MILLIC    5000

STATIC EFI_EVENT       mPollTimer;
STATIC EFI_EVENT       mExitBootServicesEvent;
STATIC INTN            mLevel    = -1;         // -1 until the first SCMI set lands
STATIC INTN            mOverride = -1;         // -1 = no protocol override
STATIC RPI_FAN_POLICY  mPolicy;                // sanitized, refreshed every tick

/**
  Refresh mPolicy from the FanPolicy variable, sanitized. Anything absent,
  short, out of range or with non-ascending custom trips resolves to AUTO
  defaults - the fan must keep regulating no matter what was written.
**/
STATIC
VOID
ReadPolicy (
  VOID
  )
{
  RPI_FAN_POLICY     Var;
  RPI_POWER_PROFILE  Profile;
  UINTN              Size;
  EFI_STATUS         Status;

  mPolicy.Mode       = RPI_FAN_MODE_AUTO;
  mPolicy.FixedLevel = FAN_LEVEL_SAFE;
  mPolicy.Trip1C     = 50;
  mPolicy.Trip2C     = 60;
  mPolicy.Trip3C     = 68;
  mPolicy.Trip4C     = 75;

  //
  // The high-level PowerProfile (RpiScmiConfigDxe) decides the fan curve
  // for every profile but Manual. A non-Manual profile is resolved to a
  // CUSTOM trip set here and wins over FanPolicy; Manual (or an absent /
  // malformed PowerProfile) falls through to the detailed FanPolicy below.
  //
  Size   = sizeof (Profile);
  Status = gRT->GetVariable (
                  RPI_POWER_PROFILE_VARIABLE_NAME,
                  &gRpiScmiConfigFormSetGuid,
                  NULL,
                  &Size,
                  &Profile
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Profile)) &&
      (Profile.Profile != RPI_POWER_PROFILE_MANUAL))
  {
    STATIC CONST UINT8  ProfileTrips[][4] = {
      [RPI_POWER_PROFILE_BALANCED] = { 50, 60, 68, 75 },
      [RPI_POWER_PROFILE_QUIET]    = { 58, 68, 76, 84 },
      [RPI_POWER_PROFILE_COOL]     = { 42, 50, 58, 66 },
    };

    if (Profile.Profile < ARRAY_SIZE (ProfileTrips)) {
      mPolicy.Mode   = RPI_FAN_MODE_CUSTOM;
      mPolicy.Trip1C = ProfileTrips[Profile.Profile][0];
      mPolicy.Trip2C = ProfileTrips[Profile.Profile][1];
      mPolicy.Trip3C = ProfileTrips[Profile.Profile][2];
      mPolicy.Trip4C = ProfileTrips[Profile.Profile][3];
      return;
    }
  }

  Size   = sizeof (Var);
  Status = gRT->GetVariable (
                  RPI_FAN_POLICY_VARIABLE_NAME,
                  &gRpiFanConfigFormSetGuid,
                  NULL,
                  &Size,
                  &Var
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Var))) {
    return;
  }

  if (Var.Mode > RPI_FAN_MODE_CUSTOM) {
    return;
  }

  if (Var.Mode == RPI_FAN_MODE_CUSTOM) {
    if ((Var.Trip1C < FAN_TRIP_MIN_C) || (Var.Trip4C > FAN_TRIP_MAX_C) ||
        (Var.Trip1C >= Var.Trip2C) || (Var.Trip2C >= Var.Trip3C) ||
        (Var.Trip3C >= Var.Trip4C))
    {
      return;                     // unusable trips: stay on AUTO defaults
    }
  }

  mPolicy = Var;
  if (mPolicy.FixedLevel > FAN_LEVEL_MAX) {
    mPolicy.FixedLevel = FAN_LEVEL_SAFE;
  }
}

/**
  Trip temperature for Level under the active policy, in milli-C.
**/
STATIC
INT32
TripMilliC (
  IN UINTN  Level
  )
{
  UINT8  TripC;

  if (mPolicy.Mode != RPI_FAN_MODE_CUSTOM) {
    return mFanLevels[Level].TripMilliC;
  }

  switch (Level) {
    case 1:  TripC = mPolicy.Trip1C;
      break;
    case 2:  TripC = mPolicy.Trip2C;
      break;
    case 3:  TripC = mPolicy.Trip3C;
      break;
    default: TripC = mPolicy.Trip4C;
      break;
  }

  return (INT32)TripC * 1000;
}

/**
  Command a cooling level through SCMI; mLevel tracks only levels the
  server acknowledged, so a failed set is retried on the next tick.
**/
STATIC
VOID
FanSetLevel (
  IN UINTN  Level
  )
{
  if (!ScmiFanSetLevel (Level)) {
    //
    // Expected until the RP1 BAR handshake completes; stay quiet and keep
    // the loop retrying.
    //
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "ActiveCoolerScmiDxe: level %d -> %d (SCMI)\n",
    (INT32)mLevel,
    (INT32)Level
    ));

  mLevel = (INTN)Level;
}

/**
  1 s poll: bring up the SCMI channel on first sight, refresh the policy,
  then command the fan per the precedence chain (override, fixed policy,
  closed loop). In the loop, ramp-ups are immediate; ramp-downs release one
  level per poll after the temperature clears the current trip minus 5 C.
**/
STATIC
VOID
EFIAPI
FanPollTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  INT32  MilliC;
  UINTN  Target;
  UINTN  Index;

  if (!RpiScmiReady ()) {
    return;   // CPU arch protocol not up yet; try again next tick
  }

  ReadPolicy ();

  //
  // Pinned levels bypass the loop and the sensor entirely: the operator
  // (or the BMC) asked for exactly this.
  //
  if (mOverride >= 0) {
    if (mLevel != mOverride) {
      FanSetLevel ((UINTN)mOverride);
    }

    return;
  }

  if (mPolicy.Mode == RPI_FAN_MODE_FIXED) {
    if (mLevel != (INTN)mPolicy.FixedLevel) {
      FanSetLevel (mPolicy.FixedLevel);
    }

    return;
  }

  if (!ScmiReadMilliCelsius (&MilliC)) {
    if (mLevel < FAN_LEVEL_SAFE) {
      DEBUG ((
        DEBUG_WARN,
        "ActiveCoolerScmiDxe: SCMI temperature unavailable, forcing level %d\n",
        FAN_LEVEL_SAFE
        ));
      FanSetLevel (FAN_LEVEL_SAFE);
    }

    return;
  }

  Target = 0;
  for (Index = FAN_LEVEL_COUNT; Index-- > 1; ) {
    if (MilliC >= TripMilliC (Index)) {
      Target = Index;
      break;
    }
  }

  if ((mLevel >= 0) && (Target < (UINTN)mLevel)) {
    if (MilliC >= TripMilliC ((UINTN)mLevel) - FAN_HYST_MILLIC) {
      Target = (UINTN)mLevel;         // inside the hysteresis band: hold
    } else {
      Target = (UINTN)mLevel - 1;     // step down gently
    }
  }

  if ((INTN)Target != mLevel) {
    FanSetLevel (Target);
  }
}

/**
  Stop regulating at ExitBootServices and never hand off a parked fan: on
  DT boots Linux's SCMI stack takes over, and an OS without one inherits
  level >= 1 airflow instead of silence. OP-TEE holds the level between.
**/
STATIC
VOID
EFIAPI
FanOnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->SetTimer (mPollTimer, TimerCancel, 0);

  if (RpiScmiReady () && (mLevel < 1)) {
    ScmiFanSetLevel (1);
  }
}

//
// RPI_FAN_PROTOCOL implementation.
//

STATIC
EFI_STATUS
EFIAPI
FanGetInfo (
  IN  RPI_FAN_PROTOCOL  *This,
  OUT RPI_FAN_INFO      *Info
  )
{
  UINTN  Level;

  if (Info == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Info->TemperatureValid = ScmiReadMilliCelsius (&Info->TemperatureMilliCelsius);
  if (!Info->TemperatureValid) {
    Info->TemperatureMilliCelsius = 0;
  }

  Info->MaxLevel       = (UINT8)FAN_LEVEL_MAX;
  Info->OverrideActive = (BOOLEAN)(mOverride >= 0);

  if (!RpiScmiReady () || (mLevel < 0)) {
    Info->Level   = 0;
    Info->Duty255 = 0;
    return EFI_NOT_READY;
  }

  Level         = (UINTN)mLevel;
  Info->Level   = (UINT8)Level;
  Info->Duty255 = mFanLevels[Level].Duty255;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FanSetOverride (
  IN RPI_FAN_PROTOCOL  *This,
  IN UINT8             Level
  )
{
  if (Level > FAN_LEVEL_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  mOverride = (INTN)Level;
  DEBUG ((DEBUG_INFO, "ActiveCoolerScmiDxe: override staged, level %d\n", Level));

  //
  // Apply now rather than waiting out the poll interval; the timer keeps
  // it asserted afterwards.
  //
  if (RpiScmiReady () && (mLevel != mOverride)) {
    FanSetLevel ((UINTN)mOverride);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FanClearOverride (
  IN RPI_FAN_PROTOCOL  *This
  )
{
  if (mOverride >= 0) {
    DEBUG ((DEBUG_INFO, "ActiveCoolerScmiDxe: override cleared\n"));
  }

  mOverride = -1;
  return EFI_SUCCESS;                 // loop resumes on the next tick
}

STATIC RPI_FAN_PROTOCOL  mFanProtocol = {
  FanGetInfo,
  FanSetOverride,
  FanClearOverride
};

EFI_STATUS
EFIAPI
ActiveCoolerScmiEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  FanPollTick,
                  NULL,
                  &mPollTimer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  FanOnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mPollTimer);
    return Status;
  }

  Status = gBS->SetTimer (mPollTimer, TimerPeriodic, FAN_POLL_INTERVAL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mExitBootServicesEvent);
    gBS->CloseEvent (mPollTimer);
    return Status;
  }

  //
  // The control surface is best-effort: fan regulation must run even if
  // the protocol cannot be published.
  //
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gRpiFanProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mFanProtocol
                  );
  DEBUG ((DEBUG_INFO, "ActiveCoolerScmiDxe: install RpiFanProtocol - %r\n", Status));

  return EFI_SUCCESS;
}

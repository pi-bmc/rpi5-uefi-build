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

  Wire contract, kept in step across four places (TF-A 0002 rpi_scmi_svc.c,
  OP-TEE plat-rpi5 scmi_server.c, the bcm2712-scmi DTB overlay, here):
  doorbell SMC 0x82000010; SMT slot = the top 4 KB page of the OP-TEE
  reserved SHM window (derived from the carve-out PCDs below = 0x1F3FF000);
  message layout per the SCMI platform shared-memory spec, matching OP-TEE
  core/drivers/scmi-msg/smt.c's struct smt_header. The page must be mapped
  uncached here because OP-TEE (MEM_AREA_IO_NSEC) and Linux (no-map +
  ioremap) both use device mappings - a cached view would be incoherent.

  The fan set path returns SCMI errors until EDK2's RpiOpteeSensorDxe
  delivers the RP1 BAR to OP-TEE (the PCIe late-init handshake); the loop
  just retries next tick, exactly as it used to wait for Rp1BusDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/RpiFanPolicy.h>
#include <Protocol/RpiFan.h>

//
// --- SCMI-lite agent -------------------------------------------------------
//

//
// SiP fast SMC the SMT channel is rung with (RPI_SIP_SCMI_AGENT0).
//
#define SCMI_DOORBELL_SMC_FID  0x82000010

//
// SMT slot layout (SCMI platform design document shared-memory transfer;
// field-for-field OP-TEE's core/drivers/scmi-msg/smt.c struct smt_header).
//
#pragma pack(1)
typedef struct {
  UINT32    Reserved0;
  UINT32    Status;         // bit0 FREE, bit1 ERROR
  UINT64    Reserved1;
  UINT32    Flags;          // bit1 interrupt completion; 0 = polled
  UINT32    Length;         // MessageHeader + payload, in bytes
  UINT32    MessageHeader;  // msg id [7:0], type [9:8], protocol [17:10]
  UINT32    Payload[30];    // rest of the 128-byte slot
} SCMI_SMT_SLOT;
#pragma pack()

#define SMT_STATUS_FREE   BIT0
#define SMT_STATUS_ERROR  BIT1

#define SCMI_MSG_HEADER(Protocol, MsgId)  \
  (((UINT32)(Protocol) << 10) | (UINT32)(MsgId))

//
// Protocols and messages used (ids per the SCMI spec, verified against
// OP-TEE's scmi-msg sensor.h / perf_domain.h).
//
#define SCMI_PROTOCOL_SENSOR      0x15
#define SCMI_SENSOR_READING_GET   0x6

#define SCMI_PROTOCOL_PERF        0x13
#define SCMI_PERF_LEVEL_SET       0x7

#define SCMI_SENSOR_ID_SOC_TEMP   0
#define SCMI_PERF_DOMAIN_FAN      0

STATIC volatile SCMI_SMT_SLOT  *mSmtSlot;   // NULL until the page is UC-mapped

/**
  The SMT slot address: the top 4 KB page of the OP-TEE reserved SHM window,
  derived from the same carve-out PCDs RaspberryPiMem.c reserves it with.
**/
STATIC
EFI_PHYSICAL_ADDRESS
ScmiSmtBase (
  VOID
  )
{
  return PcdGet64 (PcdOpteeTzdramBase) + PcdGet32 (PcdOpteeTzdramSize) +
         PcdGet32 (PcdOpteeShmSize) - SIZE_4KB;
}

/**
  One synchronous SCMI command over the SMT slot: build the message, ring
  the doorbell, return the payload. The fastcall is served synchronously
  inside the SMC, so the response is in place when ArmCallSmc returns.

  @param[in]  Protocol  SCMI protocol id.
  @param[in]  MsgId     Message id within the protocol.
  @param[in]  In        Payload words to send.
  @param[in]  InCount   Number of payload words to send.
  @param[out] Out       Response payload words (first is the SCMI status).
  @param[in]  OutCount  Number of response words to read back.

  @retval EFI_SUCCESS       Transport round trip completed; Out[0] carries
                            the SCMI status.
  @retval EFI_NOT_READY     Channel not free (server never released it).
  @retval EFI_DEVICE_ERROR  Doorbell SMC or channel-level error.
**/
STATIC
EFI_STATUS
ScmiCall (
  IN  UINT8         Protocol,
  IN  UINT8         MsgId,
  IN  CONST UINT32  *In,
  IN  UINTN         InCount,
  OUT UINT32        *Out,
  IN  UINTN         OutCount
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Index;

  ASSERT (mSmtSlot != NULL);
  ASSERT (InCount <= ARRAY_SIZE (mSmtSlot->Payload));
  ASSERT (OutCount <= ARRAY_SIZE (mSmtSlot->Payload));

  //
  // The channel must be free: the platform sets FREE after every message,
  // and this driver is the only agent before the OS. Anything else means
  // the server side is wedged - do not write over an in-flight slot.
  //
  if ((mSmtSlot->Status & SMT_STATUS_FREE) == 0) {
    return EFI_NOT_READY;
  }

  for (Index = 0; Index < InCount; Index++) {
    mSmtSlot->Payload[Index] = In[Index];
  }

  mSmtSlot->Flags         = 0;
  mSmtSlot->Length        = (UINT32)(sizeof (UINT32) * (1 + InCount));
  mSmtSlot->MessageHeader = SCMI_MSG_HEADER (Protocol, MsgId);
  mSmtSlot->Status        = 0;                     // claim the channel

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = SCMI_DOORBELL_SMC_FID;
  ArmCallSmc (&Args);

  if (Args.Arg0 != 0) {
    //
    // TF-A refused the forward (OP-TEE not up?): release our claim so the
    // next tick can try again.
    //
    mSmtSlot->Status = SMT_STATUS_FREE;
    return EFI_DEVICE_ERROR;
  }

  if (((mSmtSlot->Status & SMT_STATUS_FREE) == 0) ||
      ((mSmtSlot->Status & SMT_STATUS_ERROR) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < OutCount; Index++) {
    Out[Index] = mSmtSlot->Payload[Index];
  }

  return EFI_SUCCESS;
}

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

  if (mSmtSlot == NULL) {
    return FALSE;
  }

  In[0] = SCMI_SENSOR_ID_SOC_TEMP;
  In[1] = 0;                                       // synchronous read

  if (EFI_ERROR (ScmiCall (
                   SCMI_PROTOCOL_SENSOR,
                   SCMI_SENSOR_READING_GET,
                   In,
                   2,
                   Out,
                   3
                   )) ||
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

  if (mSmtSlot == NULL) {
    return FALSE;
  }

  In[0] = SCMI_PERF_DOMAIN_FAN;
  In[1] = (UINT32)Level;

  if (EFI_ERROR (ScmiCall (
                   SCMI_PROTOCOL_PERF,
                   SCMI_PERF_LEVEL_SET,
                   In,
                   2,
                   Out,
                   1
                   )) ||
      ((INT32)Out[0] != 0))
  {
    return FALSE;
  }

  return TRUE;
}

/**
  Map the SMT page uncached and arm the client. Retried from the poll tick
  until it succeeds (SetMemorySpaceAttributes needs the CPU arch protocol,
  which may dispatch after this driver).
**/
STATIC
BOOLEAN
ScmiChannelInit (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  if (mSmtSlot != NULL) {
    return TRUE;
  }

  Base = ScmiSmtBase ();

  //
  // Uncached, or nothing: a write-back view of a page OP-TEE and Linux
  // access through device mappings corrupts messages via stale lines.
  //
  Status = gDS->SetMemorySpaceAttributes (Base, SIZE_4KB, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  mSmtSlot = (volatile SCMI_SMT_SLOT *)(UINTN)Base;
  DEBUG ((DEBUG_INFO, "ActiveCoolerScmiDxe: SCMI SMT channel at 0x%lx\n", Base));

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
  RPI_FAN_POLICY  Var;
  UINTN           Size;
  EFI_STATUS      Status;

  mPolicy.Mode       = RPI_FAN_MODE_AUTO;
  mPolicy.FixedLevel = FAN_LEVEL_SAFE;
  mPolicy.Trip1C     = 50;
  mPolicy.Trip2C     = 60;
  mPolicy.Trip3C     = 68;
  mPolicy.Trip4C     = 75;

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

  if (!ScmiChannelInit ()) {
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

  if ((mSmtSlot != NULL) && (mLevel < 1)) {
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

  if ((mSmtSlot == NULL) || (mLevel < 0)) {
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
  if ((mSmtSlot != NULL) && (mLevel != mOverride)) {
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

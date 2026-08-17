/** @file

  ActiveCoolerDxe - closed-loop fan control for the Raspberry Pi 5 active
  cooler during the firmware phase.

  Nothing drives the fan header once the VPU hands off to the armstub, so
  a long UEFI session (Setup, netboot retries, the BMC holding the host at
  the firmware menu) runs the SoC with whatever airflow the bootloader
  last commanded. This driver closes the loop: a 1 s timer reads the
  BCM2712 AVS monitor's AVS_RO_TEMP_STATUS - the same register
  SsdtThermal.asl exposes to the OS (milli-C = 450000 - 550 * raw, raw in
  bits 9:0, validity bits 16 and 10) - and maps the temperature onto the
  exact cooling policy the VPU DTB ships for Linux's pwm-fan:

    level  duty/255  trip (up)   release (down, 5 C hysteresis)
      0        0        -            < 45.0 C
      1       75      >= 50.0 C      < 55.0 C
      2      125      >= 60.0 C      < 62.5 C
      3      175      >= 67.5 C      < 70.0 C
      4      250      >= 75.0 C        -

  The fan PWM line is RP1 GPIO45 (funcsel 0 = pwm1, pull-down per the DTB),
  RP1 PWM1 channel 3, 41566 ns period, inverted polarity, clocked from
  clk_pwm1 (xosc 50 MHz / 1 = 20 ns per tick, so range = 2078). The clock
  is programmed only if it comes up disabled - if the VPU already runs it,
  its mux/divider are left alone. Steps up are immediate; steps down go
  one level per poll once the temperature clears the hysteresis band.

  RP1 base comes from Rp1BusDxe's RP1_BUS_PROTOCOL, which only exists
  after BDS connects the RP1 PCI function, so the timer idles until the
  protocol appears. At ExitBootServices the poll stops and the commanded
  level is floored at 1: an OS fan driver (pwm-fan on DT boots) reprograms
  the channel within seconds, and an OS without one inherits real airflow
  instead of a parked fan.

  Register facts (offsets, bit positions, mux/divider encoding) match the
  RP1 Peripherals datasheet, cross-checked against the downstream
  raspberrypi/linux pwm-rp1.c / clk-rp1.c / pinctrl-rp1.c and the
  cooling_fan + thermal-zones nodes of the shipped bcm2712-rpi-5-b.dtb.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Rp1GpioLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/Rp1Bus.h>
#include <Rp1.h>

//
// BCM2712 AVS monitor temperature status (SoC bus, direct CPU mapping).
//
#define AVS_RO_TEMP_STATUS      0x107D542200ULL
#define AVS_TEMP_VALID_MASK     0x10400        // bits 16 and 10
#define AVS_TEMP_RAW_MASK       0x3FF

//
// RP1 PWM block (at RP1_PWM1_BASE), per-channel stride 16 bytes.
//
#define PWM_GLOBAL_CTRL         0x000
#define PWM_GLOBAL_SET_UPDATE   BIT31          // latch shadowed config
#define PWM_GLOBAL_CHAN_EN(c)   (1u << (c))
#define PWM_CHAN_CTRL(c)        (0x014 + ((c) * 16))
#define PWM_CHAN_RANGE(c)       (0x018 + ((c) * 16))
#define PWM_CHAN_DUTY(c)        (0x020 + ((c) * 16))

#define PWM_CHAN_CTRL_MODE_TE_MS   BIT0        // trailing-edge mark/space
#define PWM_CHAN_CTRL_INVERT       BIT3
#define PWM_CHAN_CTRL_FIFO_POP     BIT8        // mask FIFO pop (unused here)

//
// clk_pwm1 in the main clock block (at RP1_CLOCKS_MAIN_BASE). All of
// clk_pwm1's parents are aux sources; index 2 is xosc (50 MHz).
//
#define CLK_PWM1_CTRL           0x084
#define CLK_PWM1_DIV_INT        0x088
#define CLK_PWM1_DIV_FRAC       0x08C
#define CLK_CTRL_ENABLE         BIT11
#define CLK_CTRL_AUXSRC_XOSC    (2u << 5)

#define FAN_PWM_CHANNEL         3
#define FAN_PWM_GPIO            45

//
// 41566 ns DTB period at 20 ns per tick (xosc 50 MHz, divide-by-1).
//
#define FAN_PWM_RANGE_TICKS     2078

#define FAN_POLL_INTERVAL       10000000ULL    // 1 s in 100 ns units
#define FAN_HYST_MILLIC         5000

typedef struct {
  INT32    TripMilliC;    // enter this level at or above
  UINT32   Duty255;       // pwm-fan cooling-level, out of 255
} FAN_LEVEL;

STATIC CONST FAN_LEVEL  mFanLevels[] = {
  { 0,      0   },
  { 50000,  75  },
  { 60000,  125 },
  { 67500,  175 },
  { 75000,  250 },
};

#define FAN_LEVEL_COUNT  (sizeof (mFanLevels) / sizeof (mFanLevels[0]))
#define FAN_LEVEL_SAFE   2    // commanded while the sensor reads invalid

STATIC EFI_EVENT             mPollTimer;
STATIC EFI_EVENT             mExitBootServicesEvent;
STATIC EFI_PHYSICAL_ADDRESS  mRp1Base;      // 0 until RP1_BUS_PROTOCOL appears
STATIC INTN                  mLevel = -1;   // -1 until first command

/**
  Read the SoC temperature. Returns FALSE while the AVS monitor's validity
  bits are clear (early after reset, or a wedged sensor).
**/
STATIC
BOOLEAN
AvsReadMilliCelsius (
  OUT INT32  *MilliC
  )
{
  UINT32  Status;

  Status = MmioRead32 (AVS_RO_TEMP_STATUS);
  if ((Status & AVS_TEMP_VALID_MASK) != AVS_TEMP_VALID_MASK) {
    return FALSE;
  }

  *MilliC = 450000 - 550 * (INT32)(Status & AVS_TEMP_RAW_MASK);
  return TRUE;
}

/**
  Latch the shadowed PWM configuration into the running generator, as the
  Linux driver does after every register change.
**/
STATIC
VOID
FanPwmLatch (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Pwm;

  Pwm = mRp1Base + RP1_PWM1_BASE;
  MmioWrite32 (Pwm + PWM_GLOBAL_CTRL,
    MmioRead32 (Pwm + PWM_GLOBAL_CTRL) | PWM_GLOBAL_SET_UPDATE);
}

/**
  Command a cooling level: convert the DTB duty/255 to range ticks and
  latch it.
**/
STATIC
VOID
FanSetLevel (
  IN UINTN  Level
  )
{
  EFI_PHYSICAL_ADDRESS  Pwm;
  UINT32                DutyTicks;

  Pwm       = mRp1Base + RP1_PWM1_BASE;
  DutyTicks = (mFanLevels[Level].Duty255 * FAN_PWM_RANGE_TICKS + 127) / 255;

  MmioWrite32 (Pwm + PWM_CHAN_DUTY (FAN_PWM_CHANNEL), DutyTicks);
  FanPwmLatch ();

  DEBUG ((DEBUG_INFO, "ActiveCoolerDxe: level %d -> %d (duty %u/%u)\n",
    (INT32)mLevel, (INT32)Level, DutyTicks, FAN_PWM_RANGE_TICKS));

  mLevel = (INTN)Level;
}

/**
  One-time hardware bring-up, run when RP1_BUS_PROTOCOL first appears:
  clock (only if disabled), pinmux, channel mode/polarity/range, enable.
**/
STATIC
VOID
FanHwInit (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Clk;
  EFI_PHYSICAL_ADDRESS  Pwm;

  Clk = mRp1Base + RP1_CLOCKS_MAIN_BASE;
  Pwm = mRp1Base + RP1_PWM1_BASE;

  //
  // A running clk_pwm1 (the VPU's own fan bring-up) is left untouched: a
  // different mux/divider only shifts the ~24 kHz carrier, never the duty
  // ratio.
  //
  if ((MmioRead32 (Clk + CLK_PWM1_CTRL) & CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Clk + CLK_PWM1_DIV_INT, 1);
    MmioWrite32 (Clk + CLK_PWM1_DIV_FRAC, 0);
    MmioWrite32 (Clk + CLK_PWM1_CTRL, CLK_CTRL_AUXSRC_XOSC | CLK_CTRL_ENABLE);
  }

  Rp1GpioSetFunction (mRp1Base, FAN_PWM_GPIO, RP1_GPIO_FUNC_ALT0);
  Rp1GpioSetPull (mRp1Base, FAN_PWM_GPIO, Rp1GpioPullDown);

  //
  // Inverted polarity per the DTB's PWM_POLARITY_INVERTED: the line sits
  // low for DUTY ticks of each RANGE window, so duty 0 parks it high
  // (fan off) even while the channel stays enabled.
  //
  MmioWrite32 (Pwm + PWM_CHAN_CTRL (FAN_PWM_CHANNEL),
    PWM_CHAN_CTRL_MODE_TE_MS | PWM_CHAN_CTRL_INVERT | PWM_CHAN_CTRL_FIFO_POP);
  MmioWrite32 (Pwm + PWM_CHAN_RANGE (FAN_PWM_CHANNEL), FAN_PWM_RANGE_TICKS);
  MmioWrite32 (Pwm + PWM_CHAN_DUTY (FAN_PWM_CHANNEL), 0);
  MmioWrite32 (Pwm + PWM_GLOBAL_CTRL,
    MmioRead32 (Pwm + PWM_GLOBAL_CTRL) | PWM_GLOBAL_CHAN_EN (FAN_PWM_CHANNEL));
  FanPwmLatch ();

  DEBUG ((DEBUG_INFO, "ActiveCoolerDxe: PWM1 ch%d up at RP1 %lx\n",
    FAN_PWM_CHANNEL, (UINT64)mRp1Base));
}

/**
  1 s poll: acquire RP1 on first sight, then run the trip/hysteresis state
  machine. Ramp-ups are immediate; ramp-downs release one level per poll
  after the temperature clears the current trip minus 5 C.
**/
STATIC
VOID
EFIAPI
FanPollTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  RP1_BUS_PROTOCOL  *Rp1Bus;
  INT32             MilliC;
  UINTN             Target;
  UINTN             Index;

  if (mRp1Base == 0) {
    if (EFI_ERROR (gBS->LocateProtocol (&gRp1BusProtocolGuid, NULL,
                     (VOID **)&Rp1Bus))) {
      return;   // RP1 not connected yet; try again next tick
    }

    mRp1Base = Rp1Bus->GetPeripheralBase (Rp1Bus);
    FanHwInit ();
  }

  if (!AvsReadMilliCelsius (&MilliC)) {
    if (mLevel < FAN_LEVEL_SAFE) {
      DEBUG ((DEBUG_WARN,
        "ActiveCoolerDxe: AVS reading invalid, forcing level %d\n",
        FAN_LEVEL_SAFE));
      FanSetLevel (FAN_LEVEL_SAFE);
    }

    return;
  }

  Target = 0;
  for (Index = FAN_LEVEL_COUNT; Index-- > 1; ) {
    if (MilliC >= mFanLevels[Index].TripMilliC) {
      Target = Index;
      break;
    }
  }

  if ((mLevel >= 0) && (Target < (UINTN)mLevel)) {
    if (MilliC >= mFanLevels[mLevel].TripMilliC - FAN_HYST_MILLIC) {
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
  Stop regulating at ExitBootServices and never hand off a parked fan:
  pwm-fan takes over within seconds on DT boots, and an OS without a fan
  driver inherits level >= 1 airflow instead of silence.
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

  if ((mRp1Base != 0) && (mLevel < 1)) {
    FanSetLevel (1);
  }
}

EFI_STATUS
EFIAPI
ActiveCoolerEntryPoint (
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

  return EFI_SUCCESS;
}

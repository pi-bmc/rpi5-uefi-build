/** @file

  Stateless MMIO helpers for the RP1 GPIO banks.

  Clean-room implementation from the RP1 Peripherals datasheet sec. 3.1.4
  (io_bank / sys_rio / pads register interface). 54 GPIOs in three banks
  (0-27, 28-33, 34-53) at window offsets +0x0000 / +0x4000 / +0x8000,
  identical across the IO (RP1_IO_BANK0_BASE), RIO (RP1_SYS_RIO0_BASE) and
  PADS (RP1_PADS_BANK0_BASE) windows.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Rp1.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Rp1GpioLib.h>

//
// Per-bank window offset, identical within the IO, RIO and PADS windows
// (Rp1.h's RP1_IO_BANK1_BASE etc. are the bank-0 base plus these).
//
#define RP1_GPIO_BANK1_OFFSET  0x4000
#define RP1_GPIO_BANK2_OFFSET  0x8000

#define RP1_GPIO_BANK0_FIRST_PIN  0
#define RP1_GPIO_BANK1_FIRST_PIN  28
#define RP1_GPIO_BANK2_FIRST_PIN  34

//
// RP2040-style atomic register aliases, applied on top of the per-bank
// RIO window (write a bitmask to XOR/set/clear bits without a RMW).
//
#define RP1_ATOMIC_XOR_OFFSET  0x1000
#define RP1_ATOMIC_SET_OFFSET  0x2000
#define RP1_ATOMIC_CLR_OFFSET  0x3000

//
// IO window: per-pin STATUS/CTRL register pair, pin index local to the bank.
//
#define RP1_IO_STATUS_OFFSET(Pin)  ((Pin) * 8)
#define RP1_IO_CTRL_OFFSET(Pin)    (((Pin) * 8) + 4)

#define RP1_IO_CTRL_FUNCSEL_MASK  0x0000001f      // bits 4:0
#define RP1_IO_CTRL_OUTOVER_MASK  0x00003000      // bits 13:12, 00 = follow peripheral
#define RP1_IO_CTRL_OEOVER_MASK   0x0000c000      // bits 15:14, 00 = follow peripheral

//
// RIO window: whole-bank bitmask registers.
//
#define RP1_RIO_OUT_OFFSET  0x0
#define RP1_RIO_OE_OFFSET   0x4
#define RP1_RIO_IN_OFFSET   0x8                   // nosync input

//
// PADS window: per-pin register at 0x4 + pin*4 (pin index local to the bank).
//
#define RP1_PADS_PIN_OFFSET(Pin)  (0x4 + ((Pin) * 4))

#define RP1_PADS_SLEWFAST     BIT0
#define RP1_PADS_SCHMITT      BIT1
#define RP1_PADS_PULL_DOWN    BIT2
#define RP1_PADS_PULL_UP      BIT3
#define RP1_PADS_DRIVE_MASK   (BIT5 | BIT4)
#define RP1_PADS_IN_ENABLE    BIT6
#define RP1_PADS_OUT_DISABLE  BIT7

/**
  Split a global pin number into its bank window offset (+0x0000 / +0x4000 /
  +0x8000, valid within the IO, RIO and PADS windows alike) and the pin
  index local to that bank.
**/
STATIC
VOID
Rp1GpioLocatePin (
  IN  UINTN   Pin,
  OUT UINT32  *BankOffset,
  OUT UINT32  *LocalPin
  )
{
  ASSERT (Pin < RP1_GPIO_COUNT);

  if (Pin < RP1_GPIO_BANK1_FIRST_PIN) {
    *BankOffset = 0;
    *LocalPin   = (UINT32)Pin;
  } else if (Pin < RP1_GPIO_BANK2_FIRST_PIN) {
    *BankOffset = RP1_GPIO_BANK1_OFFSET;
    *LocalPin   = (UINT32)(Pin - RP1_GPIO_BANK1_FIRST_PIN);
  } else {
    *BankOffset = RP1_GPIO_BANK2_OFFSET;
    *LocalPin   = (UINT32)(Pin - RP1_GPIO_BANK2_FIRST_PIN);
  }
}

VOID
EFIAPI
Rp1GpioSetFunction (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN UINT32                Function
  )
{
  UINT32                Bank;
  UINT32                LocalPin;
  EFI_PHYSICAL_ADDRESS  PadReg;
  EFI_PHYSICAL_ADDRESS  CtrlReg;
  UINT32                Value;

  ASSERT (Pin < RP1_GPIO_COUNT);
  ASSERT ((Function <= RP1_GPIO_FUNC_ALT8) || (Function == RP1_GPIO_FUNC_NONE));

  Rp1GpioLocatePin (Pin, &Bank, &LocalPin);

  //
  // Pad: enable the input buffer and clear the output-disable bit, leaving
  // drive strength, schmitt, slew and the pulls untouched.
  //
  PadReg = PeripheralBase + RP1_PADS_BANK0_BASE + Bank +
           RP1_PADS_PIN_OFFSET (LocalPin);
  Value  = MmioRead32 (PadReg);
  Value |= RP1_PADS_IN_ENABLE;
  Value &= ~RP1_PADS_OUT_DISABLE;
  MmioWrite32 (PadReg, Value);

  //
  // IO CTRL: select the function and return the output / output-enable
  // overrides to peripheral control (00).
  //
  CtrlReg = PeripheralBase + RP1_IO_BANK0_BASE + Bank +
            RP1_IO_CTRL_OFFSET (LocalPin);
  Value  = MmioRead32 (CtrlReg);
  Value &= ~(RP1_IO_CTRL_FUNCSEL_MASK |
             RP1_IO_CTRL_OUTOVER_MASK |
             RP1_IO_CTRL_OEOVER_MASK);
  Value |= Function & RP1_IO_CTRL_FUNCSEL_MASK;
  MmioWrite32 (CtrlReg, Value);
}

VOID
EFIAPI
Rp1GpioSetPull (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN RP1_GPIO_PULL         Pull
  )
{
  UINT32                Bank;
  UINT32                LocalPin;
  EFI_PHYSICAL_ADDRESS  PadReg;
  UINT32                Value;

  ASSERT (Pin < RP1_GPIO_COUNT);

  Rp1GpioLocatePin (Pin, &Bank, &LocalPin);

  PadReg = PeripheralBase + RP1_PADS_BANK0_BASE + Bank +
           RP1_PADS_PIN_OFFSET (LocalPin);
  Value  = MmioRead32 (PadReg);
  Value &= ~(RP1_PADS_PULL_UP | RP1_PADS_PULL_DOWN);

  switch (Pull) {
    case Rp1GpioPullDown:
      Value |= RP1_PADS_PULL_DOWN;
      break;
    case Rp1GpioPullUp:
      Value |= RP1_PADS_PULL_UP;
      break;
    case Rp1GpioPullNone:
    default:
      break;
  }

  MmioWrite32 (PadReg, Value);
}

VOID
EFIAPI
Rp1GpioSetDirection (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN BOOLEAN               Output
  )
{
  UINT32                Bank;
  UINT32                LocalPin;
  EFI_PHYSICAL_ADDRESS  RioBank;

  ASSERT (Pin < RP1_GPIO_COUNT);

  Rp1GpioLocatePin (Pin, &Bank, &LocalPin);

  //
  // Flip the pin's OE bit through the atomic SET/CLR aliases, then hand the
  // pin to the SYS_RIO block with the full mux sequence (funcsel, pad input
  // enable / output-disable clear, overrides back to peripheral control).
  //
  RioBank = PeripheralBase + RP1_SYS_RIO0_BASE + Bank;
  MmioWrite32 (
    RioBank + RP1_RIO_OE_OFFSET +
    (Output ? RP1_ATOMIC_SET_OFFSET : RP1_ATOMIC_CLR_OFFSET),
    1u << LocalPin
    );

  Rp1GpioSetFunction (PeripheralBase, Pin, RP1_GPIO_FUNC_RIO);
}

BOOLEAN
EFIAPI
Rp1GpioRead (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin
  )
{
  UINT32  Bank;
  UINT32  LocalPin;
  UINT32  Value;

  ASSERT (Pin < RP1_GPIO_COUNT);

  Rp1GpioLocatePin (Pin, &Bank, &LocalPin);

  Value = MmioRead32 (
            PeripheralBase + RP1_SYS_RIO0_BASE + Bank + RP1_RIO_IN_OFFSET
            );

  return (Value & (1u << LocalPin)) != 0;
}

VOID
EFIAPI
Rp1GpioWrite (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN BOOLEAN               Value
  )
{
  UINT32  Bank;
  UINT32  LocalPin;

  ASSERT (Pin < RP1_GPIO_COUNT);

  Rp1GpioLocatePin (Pin, &Bank, &LocalPin);

  MmioWrite32 (
    PeripheralBase + RP1_SYS_RIO0_BASE + Bank + RP1_RIO_OUT_OFFSET +
    (Value ? RP1_ATOMIC_SET_OFFSET : RP1_ATOMIC_CLR_OFFSET),
    1u << LocalPin
    );
}

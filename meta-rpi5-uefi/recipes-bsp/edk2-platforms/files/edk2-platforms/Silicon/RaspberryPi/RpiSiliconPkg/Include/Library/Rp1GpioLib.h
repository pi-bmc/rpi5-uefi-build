/** @file

  Stateless MMIO helpers for the RP1 GPIO banks.

  Clean-room implementation from the RP1 Peripherals datasheet sec. 3.1.4
  (io_bank / sys_rio / pads register interface). 54 GPIOs in three banks
  (0-27, 28-33, 34-53) at window offsets +0x0000 / +0x4000 / +0x8000,
  identical across the IO (RP1_IO_BANK0_BASE), RIO (RP1_SYS_RIO0_BASE) and
  PADS (RP1_PADS_BANK0_BASE) windows. RIO registers use the RP2040-style
  atomic aliases (XOR +0x1000, SET +0x2000, CLR +0x3000).

  Callers pass the RP1 peripheral base (RP1_BUS_PROTOCOL.GetPeripheralBase()
  or the first MMIO resource's window base minus the block offset); this
  library holds no state, in the style of Bcm2712GpioLib.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RP1_GPIO_LIB_H_
#define RP1_GPIO_LIB_H_

#define RP1_GPIO_COUNT  54

//
// 5-bit FUNCSEL field values (IO CTRL bits 4:0). alt0..alt8 = 0..8;
// RP1_GPIO_FUNC_RIO (5) routes the pin to the SYS_RIO block for software
// GPIO. I2C on GPIO2/3 is alt3.
//
#define RP1_GPIO_FUNC_ALT0  0
#define RP1_GPIO_FUNC_ALT1  1
#define RP1_GPIO_FUNC_ALT2  2
#define RP1_GPIO_FUNC_ALT3  3
#define RP1_GPIO_FUNC_ALT4  4
#define RP1_GPIO_FUNC_RIO   5
#define RP1_GPIO_FUNC_ALT6  6
#define RP1_GPIO_FUNC_ALT7  7
#define RP1_GPIO_FUNC_ALT8  8
#define RP1_GPIO_FUNC_NONE  0x1f

typedef enum {
  Rp1GpioPullNone = 0,
  Rp1GpioPullDown,
  Rp1GpioPullUp,
} RP1_GPIO_PULL;

/**
  Route Pin to alternate function Function (0..8, or RP1_GPIO_FUNC_NONE).

  Also enables the pad input buffer, clears the pad output-disable bit, and
  returns the output/output-enable overrides to peripheral control - the
  full mux sequence a peripheral needs.
**/
VOID
EFIAPI
Rp1GpioSetFunction (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN UINT32                Function
  );

/**
  Set the pad pull (none / down / up) for Pin.
**/
VOID
EFIAPI
Rp1GpioSetPull (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN RP1_GPIO_PULL         Pull
  );

/**
  Make Pin a software GPIO (funcsel RIO) and set its direction.
**/
VOID
EFIAPI
Rp1GpioSetDirection (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN BOOLEAN               Output
  );

/**
  Read the input level of Pin (RIO IN).
**/
BOOLEAN
EFIAPI
Rp1GpioRead (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin
  );

/**
  Drive Pin (RIO OUT via the SET/CLR atomic aliases).
**/
VOID
EFIAPI
Rp1GpioWrite (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase,
  IN UINTN                 Pin,
  IN BOOLEAN               Value
  );

#endif // RP1_GPIO_LIB_H_

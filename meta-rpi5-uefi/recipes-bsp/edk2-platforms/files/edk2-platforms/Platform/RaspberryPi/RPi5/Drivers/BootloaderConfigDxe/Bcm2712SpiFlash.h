/** @file

  Read-only access to the Raspberry Pi 5 boot EEPROM (W25Q16, 2 MiB SPI
  NOR) through the BCM2712's own SPI controller ("spi10"), the same path
  Linux's rpi-eeprom-update uses via /dev/spidev10.0 and flashrom.

  This driver never writes the flash: it reads the live image so a
  modified copy can be staged as pieeprom.upd for the bootloader's
  self-update.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BCM2712_SPI_FLASH_H_
#define BCM2712_SPI_FLASH_H_

#include <Uefi.h>

typedef struct {
  UINT64     SpiBase;        // BCM2835-compatible SPI controller
  UINT64     CsBankBase;     // brcmstb GIO bank holding the CS line
  UINT32     CsMask;         // CS bit within the bank
  UINT32     CsPin;          // CS pin number on the soc GPIO controller
  BOOLEAN    CsActiveLow;
  UINT64     PinctrlBase;    // soc pinctrl mux registers; 0 = leave muxing alone
  BOOLEAN    IsD0;           // BCM2712 D0 stepping (different mux encoding)
} BCM2712_BOOT_SPI;

/**
  Locate the boot SPI controller, its chip-select GPIO and the soc
  pinctrl block in the VPU device tree.

  @retval EFI_SUCCESS    Spi filled in.
  @retval EFI_NOT_FOUND  Required nodes/properties missing.
**/
EFI_STATUS
Bcm2712BootSpiLocate (
  IN  CONST VOID        *Fdt,
  OUT BCM2712_BOOT_SPI  *Spi
  );

/**
  Read the full EEPROM image. Muxes the boot SPI pins (GPIO1 chip-select
  as GPIO output, GPIO2/3/4 to the SPI controller), probes the JEDEC ID,
  then streams Len bytes from flash offset 0.

  @retval EFI_SUCCESS       Buffer filled.
  @retval EFI_DEVICE_ERROR  JEDEC ID probe failed (bus not responding).
  @retval EFI_TIMEOUT       Transfer stalled.
**/
EFI_STATUS
Bcm2712BootSpiReadImage (
  IN  CONST BCM2712_BOOT_SPI  *Spi,
  OUT UINT8                   *Buffer,
  IN  UINTN                   Len
  );

#endif // BCM2712_SPI_FLASH_H_

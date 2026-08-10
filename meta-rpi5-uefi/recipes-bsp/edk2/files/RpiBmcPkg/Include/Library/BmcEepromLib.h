/** @file

  EEPROM region accessors for the BMC shared 24c256 on RP1 I2C1.

  All functions speak EFI_I2C_MASTER_PROTOCOL directly (2-byte big-endian
  data addressing, as both a real 24c256 and the BMC's Linux
  i2c-slave-eeprom emulation expect). Writes are chunked to the EEPROM page
  size with an ACK-poll between pages (a real part needs ~5-10 ms per page;
  the emulated slave ACKs immediately, so the poll costs nothing there).

  Offsets/lengths are bounds-checked against PcdBmcEepromCapacity.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BMC_EEPROM_LIB_H_
#define BMC_EEPROM_LIB_H_

#include <Protocol/I2cMaster.h>

/**
  Locate the (sole) I2C master and prepare it for EEPROM access.

  Convenience wrapper: LocateProtocol (gEfiI2cMasterProtocolGuid) and set
  the bus to 100 kHz.

  @param[out] I2cMaster   The located master.

  @retval EFI_SUCCESS     Master located and configured.
  @retval EFI_NOT_FOUND   No I2C master present (yet).
**/
EFI_STATUS
EFIAPI
BmcEepromLocateI2cMaster (
  OUT EFI_I2C_MASTER_PROTOCOL  **I2cMaster
  );

/**
  Read Length bytes at EEPROM Offset into Data.
**/
EFI_STATUS
EFIAPI
BmcEepromRead (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINT32                   Offset,
  IN  UINTN                    Length,
  OUT VOID                     *Data
  );

/**
  Write Length bytes from Data at EEPROM Offset, page-chunked with an
  ACK-poll after each page.
**/
EFI_STATUS
EFIAPI
BmcEepromWrite (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT32                   Offset,
  IN UINTN                    Length,
  IN CONST VOID               *Data
  );

/**
  Write only if the stored bytes differ (read + compare first). Spares
  EEPROM wear and needless BMC-slave pokes on unchanged content.

  @param[out] Wrote   Optional: set to TRUE if a write was performed.
**/
EFI_STATUS
EFIAPI
BmcEepromWriteIfChanged (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINT32                   Offset,
  IN  UINTN                    Length,
  IN  CONST VOID               *Data,
  OUT BOOLEAN                  *Wrote  OPTIONAL
  );

#endif // BMC_EEPROM_LIB_H_

/** @file

  Names of the bootloader-provenance UEFI variables shared with the BMC.

  Historical note: this header used to also carry the wire formats of the
  BMC shared-EEPROM regions (UbEfiVa variable blob, SMBIOS mirror, BLK1
  block inventory). That I2C EEPROM sync was replaced by the Redfish Host
  Interface (RpiRedfishPkg); only the variable-name contract below remains
  in use on the EDK2 side. u-boot and the BMC's nanokvm-app still speak the
  EEPROM formats between themselves.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_BMC_EEPROM_H_
#define RPI_BMC_EEPROM_H_

//
// UEFI variables published for the BMC under gRpiBmcBootloaderVendorGuid
// (attributes NV|BS|RT). Names are part of the frozen contract, mirrored
// by u-boot rpi.c and the BMC's efivars package.
//
#define BMC_VAR_BOOTLOADER_CONFIG     L"BootloaderConfig"
#define BMC_VAR_BOOTLOADER_TIMESTAMP  L"BootloaderUpdateTimestamp"

#endif // RPI_BMC_EEPROM_H_

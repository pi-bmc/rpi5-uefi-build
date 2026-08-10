/** @file

  Wire formats of the BMC shared-EEPROM regions.

  These are interoperability formats defined by the pi-bmc U-Boot port and
  parsed by the BMC's nanokvm-app; this header reimplements them from their
  on-wire description. Field-for-field compatibility is the contract - any
  change here must be mirrored on the BMC.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_BMC_EEPROM_H_
#define RPI_BMC_EEPROM_H_

//
// Region 0x0000..0x3fff: UEFI variable store.
//
// A single serialized blob ("UbEfiVa" version 1, same format as U-Boot's
// ubootefi.var file): header followed by packed variable entries. The
// header CRC32 (standard IEEE 802.3, as computed by BaseLib
// CalculateCrc32()) covers the entry area only, i.e. Length -
// sizeof (UB_EFI_VAR_FILE_HEADER) bytes starting right after the header.
// Length includes the header.
//
#define UB_EFI_VAR_FILE_MAGIC  0x0161566966456255ULL /* "UbEfiVa", v1 */

#pragma pack (1)

typedef struct {
  UINT64    Reserved; // unused, may be overwritten by memory probing
  UINT64    Magic;    // UB_EFI_VAR_FILE_MAGIC
  UINT32    Length;   // total blob length, including this header
  UINT32    Crc32;    // CRC32 of the entry area (excludes this header)
} UB_EFI_VAR_FILE_HEADER;

//
// One variable entry. Length is the DATA length in bytes - NOT the entry
// length. Data begins immediately after the name's 16-bit NUL, and the
// next entry starts at ALIGN8 (offset-of-Data + Length). This matches
// u-boot's efi_var_mem.c and the BMC's nanokvm-app efivars/blob.go, the
// two peer implementations of this wire format.
//
typedef struct {
  UINT32      Length;     // DATA length in bytes (not the entry length)
  UINT32      Attributes; // UEFI variable attributes
  UINT64      Time;       // authentication time (epoch seconds), 0 if unused
  EFI_GUID    VendorGuid;
  // CHAR16   Name[];     // NUL-terminated
  // UINT8    Data[Length];
} UB_EFI_VAR_ENTRY;

#pragma pack ()

//
// Region 0x6000..0x67ff: SMBIOS mirror.
//
// Verbatim SMBIOS 3.0 (64-bit) entry point followed by the structure table,
// which begins ALIGN (EntryPointLength, 16) bytes into the region. The
// entry point's TableAddress field is the DRAM address the tables lived at
// and is meaningless to an EEPROM reader; the blob is otherwise
// self-describing (anchor "_SM3_", TableMaximumSize).
//

//
// Region 0x6800..0x77ff: block-device inventory.
//
// 4-byte magic "BLK1", UINT16 little-endian JSON length, then JSON:
//   {"v":1,"drives":[{"if":"nvme","dev":0,"vendor":"..","product":"..",
//                     "rev":"..","removable":0,"size":<bytes>}, ...]}
//
#define BMC_BLKINFO_MAGIC       "BLK1"
#define BMC_BLKINFO_MAGIC_LEN   4
#define BMC_BLKINFO_HEADER_LEN  6 // magic + UINT16 LE JSON length

//
// UEFI variables published for the BMC under gRpiBmcBootloaderVendorGuid
// (attributes NV|BS|RT). Names are part of the frozen contract.
//
#define BMC_VAR_BOOTLOADER_CONFIG     L"BootloaderConfig"
#define BMC_VAR_BOOTLOADER_TIMESTAMP  L"BootloaderUpdateTimestamp"

#endif // RPI_BMC_EEPROM_H_

/** @file

  Raspberry Pi bootloader EEPROM image structure - a faithful C port of
  the section format handled by the rpi-eeprom project's
  rpi-eeprom-config tool (raspberrypi/rpi-eeprom).

  The image is a sequence of 8-byte-aligned sections, each starting with
  a big-endian magic and length. Modifiable-file sections (FILE_MAGIC)
  carry a 12-byte zero-padded filename, 4 opaque bytes, then the content;
  the length field covers filename + opaque bytes + content (16 + n).
  The walk ends at a 0x00000000/0xffffffff magic (erased flash).

  2 MiB BCM2712 images are laid out as a 64 KiB read-only bootcode area,
  a 988 KiB partition A holding the firmware plus bootconf.txt, an
  optional partition B mirror (A/B capable images; a 0xFF gap separates
  it, so it needs its own walk), and a reserved 4 KiB bootloader scratch
  sector at the very end that padding must never reach.

  Pure buffer logic - no MMIO, no allocation - so it can be unit-tested
  on the build host against the Python tool.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BLCFG_EEPROM_IMAGE_H_
#define BLCFG_EEPROM_IMAGE_H_

#include <Uefi.h>

#define EEPROM_IMAGE_SIZE_2712    SIZE_2MB

#define EEPROM_SECTION_MAGIC      0x55aaf00fU
#define EEPROM_SECTION_MAGIC_MASK 0xfffff00fU
#define EEPROM_FILE_MAGIC         0x55aaf11fU
#define EEPROM_PAD_MAGIC          0x55aafeefU

#define EEPROM_FILE_HDR_LEN       20
#define EEPROM_FILENAME_LEN       12

#define EEPROM_READ_ONLY_SIZE     SIZE_64KB
#define EEPROM_PARTITION_SIZE     (988 * 1024)
#define EEPROM_PARTITION_A_START  EEPROM_READ_ONLY_SIZE
#define EEPROM_PARTITION_A_END    (EEPROM_READ_ONLY_SIZE + EEPROM_PARTITION_SIZE)
#define EEPROM_PARTITION_B_START  EEPROM_PARTITION_A_END
#define EEPROM_PARTITION_B_END    (EEPROM_PARTITION_B_START + EEPROM_PARTITION_SIZE)

#define EEPROM_ERASE_ALIGN        SIZE_4KB

//
// Modifiable files must fit one erase sector (rpi-eeprom-config's
// MAX_FILE_SIZE).
//
#define EEPROM_MAX_FILE_SIZE      (EEPROM_ERASE_ALIGN - EEPROM_FILE_HDR_LEN)

typedef struct {
  UINTN      HdrOffset;      // section header (magic) offset
  UINTN      ContentOffset;  // HdrOffset + 4 + EEPROM_FILE_HDR_LEN
  UINTN      ContentLen;     // length field - filename - opaque word
  UINTN      NextOffset;     // start of the next non-pad section / window cap
  BOOLEAN    IsLast;         // last section of the walk
} EEPROM_FILE_LOC;

/**
  Check that a buffer looks like a bootloader EEPROM image: expected size
  and the read-only bootcode section magic at offset 0.
**/
BOOLEAN
EepromImageValid (
  IN CONST UINT8  *Image,
  IN UINTN        Size
  );

/**
  Find a modifiable file section by name.

  Walks the section chain from WalkStart and matches FILE_MAGIC sections
  whose header offset lies in [WinStart, WinEnd). NextOffset seeds at
  MIN(Size - EEPROM_ERASE_ALIGN, WinEnd) and lowers to the first non-pad
  section after the match, mirroring rpi-eeprom-config's find_file().

  @retval TRUE   Found; Loc filled in.
  @retval FALSE  Not found, or the section chain is corrupt.
**/
BOOLEAN
EepromFindFileIn (
  IN  CONST UINT8      *Image,
  IN  UINTN            Size,
  IN  UINTN            WalkStart,
  IN  UINTN            WinStart,
  IN  UINTN            WinEnd,
  IN  CONST CHAR8      *Name,
  OUT EEPROM_FILE_LOC  *Loc
  );

/**
  Replace a modifiable file's content in place, mirroring
  rpi-eeprom-config's update(): rewrite the length field, copy the new
  content (the 4 opaque bytes are preserved), 0xFF-fill the leftover up
  to the next section, inserting a pad section header when the gap is at
  least 8 bytes and the file is not the last section.

  @retval EFI_SUCCESS          Replaced.
  @retval EFI_NOT_FOUND        No such file in the window.
  @retval EFI_BAD_BUFFER_SIZE  Content exceeds EEPROM_MAX_FILE_SIZE.
  @retval EFI_VOLUME_FULL      Content does not fit before the next
                               section / the scratch sector.
**/
EFI_STATUS
EepromReplaceFileIn (
  IN OUT UINT8        *Image,
  IN     UINTN        Size,
  IN     UINTN        WalkStart,
  IN     UINTN        WinStart,
  IN     UINTN        WinEnd,
  IN     CONST CHAR8  *Name,
  IN     CONST UINT8  *Data,
  IN     UINTN        DataLen
  );

/**
  Set the self-update timestamp, mirroring rpi-eeprom-config's
  set_timestamp(): the "updatetime" file content is little-endian
  ~Timestamp then Timestamp, written to the read-only copy and the
  partition A copy; with no read-only copy the last 8 image bytes get
  Timestamp / ~Timestamp instead.

  @retval EFI_SUCCESS    Written.
  @retval EFI_NOT_FOUND  Read-only copy present but partition copy
                         missing (corrupt image; nothing was changed
                         beyond the read-only copy).
**/
EFI_STATUS
EepromSetTimestamp (
  IN OUT UINT8   *Image,
  IN     UINTN   Size,
  IN     UINT32  Timestamp
  );

#endif // BLCFG_EEPROM_IMAGE_H_

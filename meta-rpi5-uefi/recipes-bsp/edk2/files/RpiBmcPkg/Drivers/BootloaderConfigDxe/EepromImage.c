/** @file

  Raspberry Pi bootloader EEPROM image section walking and in-place file
  replacement. Byte-for-byte faithful to rpi-eeprom-config (see
  EepromImage.h for the layout); verified on the build host against the
  Python tool's output on a release image.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>

#include "EepromImage.h"

//
// Real images carry ~15 sections; a corrupt chain bails out long before
// this cap.
//
#define EEPROM_MAX_SECTIONS  64

typedef struct {
  UINT32    Magic;
  UINTN     Offset;
  UINT32    Length;
} EEPROM_SECTION;

STATIC
UINT32
Be32 (
  IN CONST UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
         ((UINT32)P[2] << 8) | (UINT32)P[3];
}

STATIC
VOID
PutBe32 (
  OUT UINT8   *P,
  IN  UINT32  V
  )
{
  P[0] = (UINT8)(V >> 24);
  P[1] = (UINT8)(V >> 16);
  P[2] = (UINT8)(V >> 8);
  P[3] = (UINT8)V;
}

STATIC
VOID
PutLe32 (
  OUT UINT8   *P,
  IN  UINT32  V
  )
{
  P[0] = (UINT8)V;
  P[1] = (UINT8)(V >> 8);
  P[2] = (UINT8)(V >> 16);
  P[3] = (UINT8)(V >> 24);
}

/**
  Walk the section chain from Start. Stops at erased flash (0x00000000 /
  0xffffffff magic) or the image end.

  @return Section count, or -1 on a corrupt chain.
**/
STATIC
INTN
WalkSections (
  IN  CONST UINT8     *Image,
  IN  UINTN           Size,
  IN  UINTN           Start,
  OUT EEPROM_SECTION  *Sections,
  IN  UINTN           Cap
  )
{
  UINTN   Offset;
  UINT32  Magic;
  UINT32  Length;
  INTN    Count;

  Count  = 0;
  Offset = Start;
  while (Offset + 8 <= Size) {
    Magic = Be32 (Image + Offset);
    if ((Magic == 0x0) || (Magic == 0xffffffff)) {
      break;
    }

    if ((Magic & EEPROM_SECTION_MAGIC_MASK) != EEPROM_SECTION_MAGIC) {
      return -1;
    }

    Length = Be32 (Image + Offset + 4);
    if ((UINTN)Length > Size - Offset - 8) {
      return -1;
    }

    if ((UINTN)Count == Cap) {
      return -1;
    }

    Sections[Count].Magic  = Magic;
    Sections[Count].Offset = Offset;
    Sections[Count].Length = Length;
    Count++;

    Offset = (Offset + 8 + Length + 7) & ~(UINTN)7;
  }

  return Count;
}

/**
  Match a section's 12-byte zero-padded filename field against Name.
**/
STATIC
BOOLEAN
FileNameMatches (
  IN CONST UINT8  *Field,
  IN CONST CHAR8  *Name
  )
{
  UINTN  Index;

  for (Index = 0; Index < EEPROM_FILENAME_LEN; Index++) {
    if (Name[Index] == '\0') {
      break;
    }

    if (Field[Index] != (UINT8)Name[Index]) {
      return FALSE;
    }
  }

  if ((Index == EEPROM_FILENAME_LEN) && (Name[Index] != '\0')) {
    return FALSE;
  }

  for ( ; Index < EEPROM_FILENAME_LEN; Index++) {
    if (Field[Index] != 0) {
      return FALSE;
    }
  }

  return TRUE;
}

BOOLEAN
EepromImageValid (
  IN CONST UINT8  *Image,
  IN UINTN        Size
  )
{
  if ((Image == NULL) || (Size != EEPROM_IMAGE_SIZE_2712)) {
    return FALSE;
  }

  //
  // Offset 0 is the read-only bootcode section, plain section magic.
  //
  return Be32 (Image) == EEPROM_SECTION_MAGIC;
}

BOOLEAN
EepromFindFileIn (
  IN  CONST UINT8      *Image,
  IN  UINTN            Size,
  IN  UINTN            WalkStart,
  IN  UINTN            WinStart,
  IN  UINTN            WinEnd,
  IN  CONST CHAR8      *Name,
  OUT EEPROM_FILE_LOC  *Loc
  )
{
  EEPROM_SECTION  Sections[EEPROM_MAX_SECTIONS];
  INTN            Count;
  INTN            Index;
  INTN            Found;
  UINTN           NextOffset;

  Count = WalkSections (Image, Size, WalkStart, Sections, EEPROM_MAX_SECTIONS);
  if (Count <= 0) {
    return FALSE;
  }

  Found = -1;
  for (Index = 0; Index < Count; Index++) {
    if ((Sections[Index].Magic == EEPROM_FILE_MAGIC) &&
        (Sections[Index].Offset >= WinStart) &&
        (Sections[Index].Offset < WinEnd) &&
        FileNameMatches (Image + Sections[Index].Offset + 8, Name))
    {
      Found = Index;
      break;
    }
  }

  if (Found < 0) {
    return FALSE;
  }

  //
  // Never let padding reach the bootloader scratch sector at the image
  // end, nor cross the window; stop earlier at the next real section.
  //
  NextOffset = Size - EEPROM_ERASE_ALIGN;
  if (WinEnd < NextOffset) {
    NextOffset = WinEnd;
  }

  for (Index = Found + 1; Index < Count; Index++) {
    if (Sections[Index].Magic != EEPROM_PAD_MAGIC) {
      NextOffset = Sections[Index].Offset;
      break;
    }
  }

  Loc->HdrOffset     = Sections[Found].Offset;
  Loc->ContentOffset = Sections[Found].Offset + 4 + EEPROM_FILE_HDR_LEN;
  Loc->ContentLen    = (Sections[Found].Length >= EEPROM_FILENAME_LEN + 4)
                       ? Sections[Found].Length - EEPROM_FILENAME_LEN - 4
                       : 0;
  Loc->NextOffset = NextOffset;
  Loc->IsLast     = (BOOLEAN)(Found == Count - 1);
  return TRUE;
}

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
  )
{
  EEPROM_FILE_LOC  Loc;
  UINTN            End;
  UINTN            PadStart;
  UINTN            PadBytes;

  if (DataLen > EEPROM_MAX_FILE_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!EepromFindFileIn (Image, Size, WalkStart, WinStart, WinEnd, Name, &Loc)) {
    return EFI_NOT_FOUND;
  }

  End = Loc.ContentOffset + DataLen;
  if ((End > Size - EEPROM_ERASE_ALIGN) || (End > Loc.NextOffset)) {
    return EFI_VOLUME_FULL;
  }

  //
  // Length field covers filename + opaque word + content; the 4 opaque
  // bytes between the filename and the content are preserved.
  //
  PutBe32 (Image + Loc.HdrOffset + 4, (UINT32)(DataLen + EEPROM_FILENAME_LEN + 4));
  CopyMem (Image + Loc.ContentOffset, Data, DataLen);

  PadStart = End;
  while ((PadStart & 7) != 0) {
    Image[PadStart++] = 0xFF;
  }

  PadBytes = Loc.NextOffset - PadStart;
  if ((PadBytes >= 8) && !Loc.IsLast) {
    PutBe32 (Image + PadStart, EEPROM_PAD_MAGIC);
    PutBe32 (Image + PadStart + 4, (UINT32)(PadBytes - 8));
    PadStart += 8;
    PadBytes -= 8;
  }

  SetMem (Image + PadStart, PadBytes, 0xFF);
  return EFI_SUCCESS;
}

EFI_STATUS
EepromSetTimestamp (
  IN OUT UINT8   *Image,
  IN     UINTN   Size,
  IN     UINT32  Timestamp
  )
{
  EEPROM_FILE_LOC  Loc;

  if (!EepromFindFileIn (
         Image,
         Size,
         0,
         0,
         EEPROM_READ_ONLY_SIZE,
         "updatetime",
         &Loc
         ) || (Loc.ContentLen < 8))
  {
    //
    // Old-style images keep the timestamp in the last 8 bytes instead
    // (Timestamp at the end, complement before it).
    //
    PutLe32 (Image + Size - 4, Timestamp);
    PutLe32 (Image + Size - 8, ~Timestamp);
    return EFI_SUCCESS;
  }

  PutLe32 (Image + Loc.ContentOffset, ~Timestamp);
  PutLe32 (Image + Loc.ContentOffset + 4, Timestamp);

  if (!EepromFindFileIn (
         Image,
         Size,
         0,
         EEPROM_PARTITION_A_START,
         EEPROM_PARTITION_A_END,
         "updatetime",
         &Loc
         ) || (Loc.ContentLen < 8))
  {
    return EFI_NOT_FOUND;
  }

  PutLe32 (Image + Loc.ContentOffset, ~Timestamp);
  PutLe32 (Image + Loc.ContentOffset + 4, Timestamp);
  return EFI_SUCCESS;
}

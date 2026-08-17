/** @file

  Bootloader config text handling: parsing the managed values out of
  key=value config text, canonicalizing user edits, and rebuilding the
  config text with new values while preserving every unmanaged line.

  Pure string logic over caller-provided buffers - no MMIO, no
  allocation - so it can be unit-tested on the build host.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

#include "BootloaderConfig.h"

//
// The managed config keys, their varstore order and absent defaults.
//
typedef enum {
  BlKeyBootOrder,
  BlKeyBootUart,
  BlKeyPowerOffOnHalt,
  BlKeyWakeOnGpio,
  BlKeyPsuMaxCurrent,
  BlKeyCount
} BL_KEY;

STATIC CONST CHAR8  *mBlKeyNames[BlKeyCount] = {
  "BOOT_ORDER",
  "BOOT_UART",
  "POWER_OFF_ON_HALT",
  "WAKE_ON_GPIO",
  "PSU_MAX_CURRENT"
};

#define BL_BOOT_ORDER_DEFAULT  "0xf41"

BOOLEAN
BlTextGetValue (
  IN  CONST CHAR8  *Text,
  IN  UINTN        Len,
  IN  CONST CHAR8  *Key,
  OUT CONST CHAR8  **Value,
  OUT UINTN        *ValueLen
  )
{
  UINTN    KeyLen;
  UINTN    Pos;
  UINTN    LineEnd;
  UINTN    ValEnd;
  BOOLEAN  Found;

  KeyLen = AsciiStrLen (Key);
  Found  = FALSE;

  for (Pos = 0; Pos < Len; Pos = LineEnd + 1) {
    for (LineEnd = Pos; (LineEnd < Len) && (Text[LineEnd] != '\n'); LineEnd++) {
    }

    if ((LineEnd - Pos > KeyLen) &&
        (CompareMem (&Text[Pos], Key, KeyLen) == 0) &&
        (Text[Pos + KeyLen] == '='))
    {
      ValEnd = LineEnd;
      if ((ValEnd > Pos + KeyLen + 1) && (Text[ValEnd - 1] == '\r')) {
        ValEnd--;
      }

      //
      // Last occurrence wins.
      //
      *Value    = &Text[Pos + KeyLen + 1];
      *ValueLen = ValEnd - (Pos + KeyLen + 1);
      Found     = TRUE;
    }
  }

  return Found;
}

/**
  Canonicalize a BOOT_ORDER string: optional whitespace, optional 0x/0X
  prefix, 1..8 hex digits; emits lowercase "0x...".
**/
STATIC
BOOLEAN
ParseBootOrder (
  IN  CONST CHAR8  *Str,
  IN  UINTN        Len,
  OUT CHAR8        Out[RPI_BLCFG_BOOT_ORDER_MAXLEN + 1]
  )
{
  UINTN  Pos;
  UINTN  End;
  UINTN  Digits;
  CHAR8  C;

  Pos = 0;
  End = Len;
  while ((Pos < End) && ((Str[Pos] == ' ') || (Str[Pos] == '\t'))) {
    Pos++;
  }

  while ((End > Pos) && ((Str[End - 1] == ' ') || (Str[End - 1] == '\t'))) {
    End--;
  }

  if ((End - Pos > 2) && (Str[Pos] == '0') &&
      ((Str[Pos + 1] == 'x') || (Str[Pos + 1] == 'X')))
  {
    Pos += 2;
  }

  if ((End == Pos) || (End - Pos > 8)) {
    return FALSE;
  }

  Out[0] = '0';
  Out[1] = 'x';
  Digits = 2;
  for ( ; Pos < End; Pos++) {
    C = Str[Pos];
    if ((C >= 'A') && (C <= 'F')) {
      C = (CHAR8)(C - 'A' + 'a');
    }

    if (!(((C >= '0') && (C <= '9')) || ((C >= 'a') && (C <= 'f')))) {
      return FALSE;
    }

    Out[Digits++] = C;
  }

  Out[Digits] = '\0';
  return TRUE;
}

STATIC
BOOLEAN
ParseDecimal (
  IN  CONST CHAR8  *Str,
  IN  UINTN        Len,
  OUT UINTN        *Result
  )
{
  UINTN  Pos;
  UINTN  Value;

  if (Len == 0) {
    return FALSE;
  }

  Value = 0;
  for (Pos = 0; Pos < Len; Pos++) {
    if ((Str[Pos] < '0') || (Str[Pos] > '9') || (Value > 0xFFFFFF)) {
      return FALSE;
    }

    Value = Value * 10 + (UINTN)(Str[Pos] - '0');
  }

  *Result = Value;
  return TRUE;
}

STATIC
VOID
SetDefaults (
  OUT BLCFG_VALUES  *Values
  )
{
  ZeroMem (Values, sizeof (*Values));
  AsciiStrCpyS (Values->BootOrder, sizeof (Values->BootOrder), BL_BOOT_ORDER_DEFAULT);
  Values->WakeOnGpio = 1;
}

VOID
BlValuesFromText (
  IN  CONST CHAR8   *Text,
  IN  UINTN         Len,
  OUT BLCFG_VALUES  *Values
  )
{
  CONST CHAR8  *Val;
  UINTN        ValLen;
  UINTN        Number;
  CHAR8        Order[RPI_BLCFG_BOOT_ORDER_MAXLEN + 1];

  SetDefaults (Values);

  if (BlTextGetValue (Text, Len, "BOOT_ORDER", &Val, &ValLen) &&
      ParseBootOrder (Val, ValLen, Order))
  {
    AsciiStrCpyS (Values->BootOrder, sizeof (Values->BootOrder), Order);
  }

  if (BlTextGetValue (Text, Len, "BOOT_UART", &Val, &ValLen) &&
      ParseDecimal (Val, ValLen, &Number))
  {
    Values->BootUart = (Number != 0) ? 1 : 0;
  }

  if (BlTextGetValue (Text, Len, "POWER_OFF_ON_HALT", &Val, &ValLen) &&
      ParseDecimal (Val, ValLen, &Number))
  {
    Values->PowerOffOnHalt = (Number != 0) ? 1 : 0;
  }

  if (BlTextGetValue (Text, Len, "WAKE_ON_GPIO", &Val, &ValLen) &&
      ParseDecimal (Val, ValLen, &Number))
  {
    Values->WakeOnGpio = (Number != 0) ? 1 : 0;
  }

  if (BlTextGetValue (Text, Len, "PSU_MAX_CURRENT", &Val, &ValLen) &&
      ParseDecimal (Val, ValLen, &Number))
  {
    //
    // Only 3000 and 5000 are meaningful on the Pi 5; snap odd values so
    // the oneof always has a selectable state.
    //
    if (Number >= 4000) {
      Values->PsuMaxCurrent = 5000;
    } else if (Number >= 1500) {
      Values->PsuMaxCurrent = 3000;
    }
  }
}

BOOLEAN
BlValuesFromData (
  IN  CONST RPI_BLCFG_DATA  *Data,
  OUT BLCFG_VALUES          *Values
  )
{
  CHAR8  Ascii[RPI_BLCFG_BOOT_ORDER_MAXLEN + 1];
  UINTN  Index;

  SetDefaults (Values);

  for (Index = 0; Index < RPI_BLCFG_BOOT_ORDER_MAXLEN; Index++) {
    if (Data->BootOrder[Index] == L'\0') {
      break;
    }

    if (Data->BootOrder[Index] > 0x7F) {
      return FALSE;
    }

    Ascii[Index] = (CHAR8)Data->BootOrder[Index];
  }

  Ascii[Index] = '\0';
  if (!ParseBootOrder (Ascii, Index, Values->BootOrder)) {
    return FALSE;
  }

  Values->BootUart       = (Data->BootUart != 0) ? 1 : 0;
  Values->PowerOffOnHalt = (Data->PowerOffOnHalt != 0) ? 1 : 0;
  Values->WakeOnGpio     = (Data->WakeOnGpio != 0) ? 1 : 0;

  if ((Data->PsuMaxCurrent != 0) && (Data->PsuMaxCurrent != 3000) &&
      (Data->PsuMaxCurrent != 5000))
  {
    return FALSE;
  }

  Values->PsuMaxCurrent = Data->PsuMaxCurrent;
  return TRUE;
}

VOID
BlValuesToData (
  IN  CONST BLCFG_VALUES  *Values,
  OUT RPI_BLCFG_DATA      *Data
  )
{
  UINTN  Index;

  ZeroMem (Data, sizeof (*Data));
  for (Index = 0; Values->BootOrder[Index] != '\0'; Index++) {
    Data->BootOrder[Index] = (CHAR16)Values->BootOrder[Index];
  }

  Data->BootUart       = Values->BootUart;
  Data->PowerOffOnHalt = Values->PowerOffOnHalt;
  Data->WakeOnGpio     = Values->WakeOnGpio;
  Data->PsuMaxCurrent  = Values->PsuMaxCurrent;
}

BOOLEAN
BlValuesEqual (
  IN CONST BLCFG_VALUES  *A,
  IN CONST BLCFG_VALUES  *B
  )
{
  return (BOOLEAN)((AsciiStrCmp (A->BootOrder, B->BootOrder) == 0) &&
                   (A->BootUart == B->BootUart) &&
                   (A->PowerOffOnHalt == B->PowerOffOnHalt) &&
                   (A->WakeOnGpio == B->WakeOnGpio) &&
                   (A->PsuMaxCurrent == B->PsuMaxCurrent));
}

/**
  Format "KEY=value\n" for one managed key. Returns 0 when the key should
  be omitted entirely (PSU_MAX_CURRENT auto).
**/
STATIC
UINTN
FormatKeyLine (
  IN  BL_KEY              Key,
  IN  CONST BLCFG_VALUES  *Values,
  OUT CHAR8               *Line,
  IN  UINTN               Cap
  )
{
  CHAR8  Value[16];
  UINTN  Number;
  UINTN  Pos;
  UINTN  Index;

  switch (Key) {
    case BlKeyBootOrder:
      AsciiStrCpyS (Value, sizeof (Value), Values->BootOrder);
      break;
    case BlKeyBootUart:
      Value[0] = (CHAR8)('0' + Values->BootUart);
      Value[1] = '\0';
      break;
    case BlKeyPowerOffOnHalt:
      Value[0] = (CHAR8)('0' + Values->PowerOffOnHalt);
      Value[1] = '\0';
      break;
    case BlKeyWakeOnGpio:
      Value[0] = (CHAR8)('0' + Values->WakeOnGpio);
      Value[1] = '\0';
      break;
    case BlKeyPsuMaxCurrent:
    default:
      if (Values->PsuMaxCurrent == 0) {
        return 0;
      }

      Number = Values->PsuMaxCurrent;
      Pos    = 0;
      if (Number >= 1000) {
        Value[Pos++] = (CHAR8)('0' + Number / 1000);
        Number      %= 1000;
      }

      Value[Pos++] = (CHAR8)('0' + Number / 100);
      Value[Pos++] = (CHAR8)('0' + (Number / 10) % 10);
      Value[Pos++] = (CHAR8)('0' + Number % 10);
      Value[Pos]   = '\0';
      break;
  }

  Pos = AsciiStrLen (mBlKeyNames[Key]) + 1 + AsciiStrLen (Value) + 1;
  if (Pos > Cap) {
    return 0;
  }

  Pos = 0;
  for (Index = 0; mBlKeyNames[Key][Index] != '\0'; Index++) {
    Line[Pos++] = mBlKeyNames[Key][Index];
  }

  Line[Pos++] = '=';
  for (Index = 0; Value[Index] != '\0'; Index++) {
    Line[Pos++] = Value[Index];
  }

  Line[Pos++] = '\n';
  return Pos;
}

/**
  Should an absent managed key be appended? Only when the desired value
  differs from what the bootloader does with the key absent.
**/
STATIC
BOOLEAN
AppendWanted (
  IN BL_KEY              Key,
  IN CONST BLCFG_VALUES  *Values
  )
{
  switch (Key) {
    case BlKeyBootOrder:
      return (BOOLEAN)(AsciiStrCmp (Values->BootOrder, BL_BOOT_ORDER_DEFAULT) != 0);
    case BlKeyBootUart:
      return (BOOLEAN)(Values->BootUart != 0);
    case BlKeyPowerOffOnHalt:
      return (BOOLEAN)(Values->PowerOffOnHalt != 0);
    case BlKeyWakeOnGpio:
      return (BOOLEAN)(Values->WakeOnGpio != 1);
    case BlKeyPsuMaxCurrent:
    default:
      return (BOOLEAN)(Values->PsuMaxCurrent != 0);
  }
}

EFI_STATUS
BlBuildNewConfigText (
  IN  CONST CHAR8         *Orig,
  IN  UINTN               OrigLen,
  IN  CONST BLCFG_VALUES  *Values,
  OUT CHAR8               *Out,
  IN  UINTN               OutCap,
  OUT UINTN               *OutLen
  )
{
  BOOLEAN  Seen[BlKeyCount];
  UINTN    Pos;
  UINTN    LineEnd;
  UINTN    LineLen;
  UINTN    OutPos;
  UINTN    KeyLen;
  UINTN    Emitted;
  BL_KEY   Key;
  BL_KEY   Match;

  ZeroMem (Seen, sizeof (Seen));
  OutPos = 0;

  for (Pos = 0; Pos < OrigLen; Pos = LineEnd + 1) {
    for (LineEnd = Pos; (LineEnd < OrigLen) && (Orig[LineEnd] != '\n'); LineEnd++) {
    }

    LineLen = LineEnd - Pos;

    Match = BlKeyCount;
    for (Key = 0; Key < BlKeyCount; Key++) {
      KeyLen = AsciiStrLen (mBlKeyNames[Key]);
      if ((LineLen > KeyLen) &&
          (CompareMem (&Orig[Pos], mBlKeyNames[Key], KeyLen) == 0) &&
          (Orig[Pos + KeyLen] == '='))
      {
        Match = Key;
        break;
      }
    }

    if (Match == BlKeyCount) {
      //
      // Unmanaged line (comments, [all] section headers, other keys):
      // copied through verbatim, newline included.
      //
      if (OutPos + LineLen + 1 > OutCap) {
        return EFI_BUFFER_TOO_SMALL;
      }

      CopyMem (&Out[OutPos], &Orig[Pos], LineLen);
      OutPos += LineLen;
      if (LineEnd < OrigLen) {
        Out[OutPos++] = '\n';
      }

      continue;
    }

    if (Seen[Match]) {
      //
      // Duplicate of a managed key: drop it, the first line already
      // carries the new value.
      //
      continue;
    }

    Seen[Match] = TRUE;
    if (OutPos + 64 > OutCap) {
      return EFI_BUFFER_TOO_SMALL;
    }

    OutPos += FormatKeyLine (Match, Values, &Out[OutPos], OutCap - OutPos);
  }

  //
  // Terminate a final line that had no newline before appending.
  //
  if ((OutPos > 0) && (Out[OutPos - 1] != '\n')) {
    if (OutPos + 1 > OutCap) {
      return EFI_BUFFER_TOO_SMALL;
    }

    Out[OutPos++] = '\n';
  }

  for (Key = 0; Key < BlKeyCount; Key++) {
    if (Seen[Key] || !AppendWanted (Key, Values)) {
      continue;
    }

    if (OutPos + 64 > OutCap) {
      return EFI_BUFFER_TOO_SMALL;
    }

    Emitted = FormatKeyLine (Key, Values, &Out[OutPos], OutCap - OutPos);
    OutPos += Emitted;
  }

  *OutLen = OutPos;
  return EFI_SUCCESS;
}

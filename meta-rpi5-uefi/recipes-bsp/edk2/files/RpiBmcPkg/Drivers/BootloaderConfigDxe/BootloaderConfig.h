/** @file

  BootloaderConfigDxe internal interfaces shared between the core
  (blconfig staging, value parsing, BMC mirror) and the Setup page /
  EEPROM staging half.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BOOTLOADER_CONFIG_H_
#define BOOTLOADER_CONFIG_H_

#include <Uefi.h>
#include <Guid/RpiBlCfg.h>

//
// The managed bootloader config values in parsed, canonical form
// (BootOrder as lowercase ASCII "0x..."). Field semantics and absent
// defaults match RPI_BLCFG_DATA.
//
typedef struct {
  CHAR8     BootOrder[RPI_BLCFG_BOOT_ORDER_MAXLEN + 1];
  UINT8     BootUart;
  UINT8     PowerOffOnHalt;
  UINT8     WakeOnGpio;
  UINT16    PsuMaxCurrent;
} BLCFG_VALUES;

//
// Live state cached at entry from the VPU DTB (BootloaderConfigDxe.c).
//
extern CONST VOID    *mFdt;                 // validated VPU DTB, NULL if absent
extern UINT8         *mBlconfigRaw;         // staged blconfig region bytes
extern UINTN         mBlconfigRawLen;       // staged length (may be truncated)
extern UINTN         mBlconfigTextLen;      // effective text length within Raw
extern BLCFG_VALUES  mCurrentValues;        // parsed from the text
extern UINT32        mDtbUpdateTimestamp;   // /chosen/bootloader/update-timestamp

/**
  Translate a DT node's first "reg" entry to a CPU physical address by
  walking every parent bus's "ranges" (defined in BootloaderConfigDxe.c,
  shared with the SPI locator).
**/
BOOLEAN
BlGetTranslatedRegAddress (
  IN  CONST VOID  *Fdt,
  IN  INT32       NodeOffset,
  OUT UINT64      *Address,
  OUT UINT64      *Size
  );

/**
  Parse the managed values out of bootloader config text (key=value
  lines; the last occurrence of a key wins). Missing keys take their
  documented absent defaults. Never fails: unparseable fields keep the
  defaults.
**/
VOID
BlValuesFromText (
  IN  CONST CHAR8   *Text,
  IN  UINTN         Len,
  OUT BLCFG_VALUES  *Values
  );

/**
  Canonicalize user-edited varstore data. Fails on an invalid BOOT_ORDER
  string or an unsupported PSU_MAX_CURRENT value.
**/
BOOLEAN
BlValuesFromData (
  IN  CONST RPI_BLCFG_DATA  *Data,
  OUT BLCFG_VALUES          *Values
  );

/**
  Convert parsed values to the varstore wire form.
**/
VOID
BlValuesToData (
  IN  CONST BLCFG_VALUES  *Values,
  OUT RPI_BLCFG_DATA      *Data
  );

BOOLEAN
BlValuesEqual (
  IN CONST BLCFG_VALUES  *A,
  IN CONST BLCFG_VALUES  *B
  );

/**
  Look up the last value of an arbitrary key in config text. Returns
  FALSE when absent.
**/
BOOLEAN
BlTextGetValue (
  IN  CONST CHAR8  *Text,
  IN  UINTN        Len,
  IN  CONST CHAR8  *Key,
  OUT CONST CHAR8  **Value,
  OUT UINTN        *ValueLen
  );

/**
  Rebuild config text with the managed keys set to Values: existing
  lines are replaced in place (first occurrence wins, duplicates drop),
  absent keys append at the end unless their value is the absent
  default. PSU_MAX_CURRENT=0 removes the key.

  @retval EFI_SUCCESS           Out/OutLen filled.
  @retval EFI_BUFFER_TOO_SMALL  Result exceeds OutCap.
**/
EFI_STATUS
BlBuildNewConfigText (
  IN  CONST CHAR8         *Orig,
  IN  UINTN               OrigLen,
  IN  CONST BLCFG_VALUES  *Values,
  OUT CHAR8               *Out,
  IN  UINTN               OutCap,
  OUT UINTN               *OutLen
  );

/**
  Install the Setup page (BootloaderConfigSetup.c).
**/
EFI_STATUS
BlInstallHiiPage (
  VOID
  );

/**
  ReadyToBoot follow-up for a previously staged update: if the running
  config now matches the staged values the update files are deleted from
  the boot volume; the marker variable is cleared either way
  (BootloaderConfigSetup.c).
**/
VOID
BlStagedMarkerCleanup (
  VOID
  );

#endif // BOOTLOADER_CONFIG_H_

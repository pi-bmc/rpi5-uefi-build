/** @file

  BootloaderConfigDxe - the bootloader EEPROM configuration driver.

  Two halves share the blconfig nvmem-rmem region (the live EEPROM config
  text the VPU firmware hands over in its DTB):

  1. BMC mirror (this file): at ReadyToBoot the config text is published
     as the BootloaderConfig variable (gRpiBmcBootloaderVendorGuid,
     NV|BS|RT) for the BMC, gated on /chosen/bootloader/update-timestamp
     so steady-state boots write nothing. Mirrors U-Boot's
     rpi_publish_bootloader_vars().

  2. Setup page (BootloaderConfigSetup.c): the managed config values
     (BOOT_ORDER, BOOT_UART, POWER_OFF_ON_HALT, WAKE_ON_GPIO,
     PSU_MAX_CURRENT) are parsed from the same text into the BlCfg
     efivarstore every boot - the form always shows what the running
     bootloader actually used, and the boot EEPROM is never read for
     display. Only the page's explicit "stage EEPROM update" action
     touches the SPI flash: the live 2 MiB image is read back (read-only)
     over the boot SPI, bootconf.txt is patched with the edited values,
     and the result is staged on the boot partition as
     pieeprom.upd/pieeprom.sig for the bootloader's self-update, followed
     by a reboot. The next boot detects whether the update was applied
     and cleans the staged files up.

  The blconfig region is a VPU carve-out mapped as device memory:
  unaligned/wide accesses fault, so it is staged once at entry with
  single-byte volatile reads (see the alignment-fault note in the git
  history) and every consumer works from that pool copy.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <RpiBmcEeprom.h>

#include "BootloaderConfig.h"

//
// Cap on the mirrored config text. The blconfig region is typically 2 KiB;
// anything close to the variable store's capacity would evict everything
// else, so U-Boot and this driver apply the same discipline.
//
#define BLCONFIG_VAR_MAX_LEN  SIZE_8KB

//
// A blconfig region larger than this is not believable - treat as invalid.
//
#define BLCONFIG_REGION_MAX  SIZE_64KB

#define BL_VAR_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE |\
                            EFI_VARIABLE_BOOTSERVICE_ACCESS |\
                            EFI_VARIABLE_RUNTIME_ACCESS)

CONST VOID    *mFdt;
UINT8         *mBlconfigRaw;
UINTN         mBlconfigRawLen;
UINTN         mBlconfigTextLen;
BLCFG_VALUES  mCurrentValues;
UINT32        mDtbUpdateTimestamp;

STATIC BOOLEAN    mDtbHasTimestamp;
STATIC EFI_EVENT  mReadyToBootEvent;
STATIC BOOLEAN    mPublished = FALSE;

/**
  Accumulate Count big-endian 32-bit cells into a UINT64.
**/
STATIC
UINT64
ReadCells (
  IN CONST UINT32  *Cell,
  IN INT32         Count
  )
{
  UINT64  Value;
  INT32   Index;

  Value = 0;
  for (Index = 0; Index < Count; Index++) {
    Value = LShiftU64 (Value, 32) | Fdt32ToCpu (Cell[Index]);
  }

  return Value;
}

/**
  Read a node's first "reg" entry and translate it to a CPU physical
  address by walking every parent bus's "ranges" up to the root - the
  equivalent of U-Boot's dev_remap_addr(). A bus with an empty "ranges"
  (or, leniently, none at all - the firmware DTBs always carry one) maps
  1:1.

  @param[in]  Fdt         The FDT blob.
  @param[in]  NodeOffset  Node whose "reg" to read.
  @param[out] Address     CPU physical address of the first reg entry.
  @param[out] Size        First reg entry's size (bus-invariant).

  @retval TRUE   Address/Size extracted and translated.
  @retval FALSE  Missing/short properties or unsupported cell counts.
**/
BOOLEAN
BlGetTranslatedRegAddress (
  IN  CONST VOID  *Fdt,
  IN  INT32       NodeOffset,
  OUT UINT64      *Address,
  OUT UINT64      *Size
  )
{
  INT32         Bus;
  INT32         BusParent;
  INT32         AddressCells;
  INT32         SizeCells;
  INT32         ParentCells;
  INT32         Length;
  INT32         EntryCells;
  CONST UINT32  *Cell;
  UINT64        Addr;
  UINT64        ChildStart;
  UINT64        ParentStart;
  UINT64        RangeSize;
  BOOLEAN       Matched;

  Bus = FdtParentOffset (Fdt, NodeOffset);
  if (Bus < 0) {
    return FALSE;
  }

  AddressCells = FdtAddressCells (Fdt, Bus);
  SizeCells    = FdtSizeCells (Fdt, Bus);
  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 0) || (SizeCells > 2))
  {
    return FALSE;
  }

  Cell = FdtGetProp (Fdt, NodeOffset, "reg", &Length);
  if ((Cell == NULL) ||
      (Length < (AddressCells + SizeCells) * (INT32)sizeof (UINT32)))
  {
    return FALSE;
  }

  Addr  = ReadCells (Cell, AddressCells);
  *Size = ReadCells (Cell + AddressCells, SizeCells);

  //
  // Root node offset is 0: once the bus IS the root, Addr is CPU-physical.
  //
  while (Bus > 0) {
    BusParent = FdtParentOffset (Fdt, Bus);
    if (BusParent < 0) {
      return FALSE;
    }

    ParentCells = FdtAddressCells (Fdt, BusParent);
    if ((ParentCells < 1) || (ParentCells > 2)) {
      return FALSE;
    }

    Cell = FdtGetProp (Fdt, Bus, "ranges", &Length);
    if ((Cell != NULL) && (Length > 0)) {
      EntryCells = AddressCells + ParentCells + SizeCells;
      Matched    = FALSE;
      while (Length >= EntryCells * (INT32)sizeof (UINT32)) {
        ChildStart  = ReadCells (Cell, AddressCells);
        ParentStart = ReadCells (Cell + AddressCells, ParentCells);
        RangeSize   = ReadCells (Cell + AddressCells + ParentCells, SizeCells);
        if ((Addr >= ChildStart) && (Addr - ChildStart < RangeSize)) {
          Addr    = ParentStart + (Addr - ChildStart);
          Matched = TRUE;
          break;
        }

        Cell   += EntryCells;
        Length -= EntryCells * (INT32)sizeof (UINT32);
      }

      if (!Matched) {
        return FALSE;
      }
    }

    Bus          = BusParent;
    AddressCells = ParentCells;
    SizeCells    = FdtSizeCells (Fdt, Bus);
    if ((SizeCells < 0) || (SizeCells > 2)) {
      return FALSE;
    }
  }

  *Address = Addr;
  return TRUE;
}

/**
  Locate the blconfig nvmem-rmem node in the VPU DTB: by compatible first,
  then - some firmware versions differ - through the /aliases "blconfig"
  path indirection, like U-Boot's rpi_find_blconfig().
**/
STATIC
INT32
FindBlconfigNode (
  IN CONST VOID  *Fdt
  )
{
  INT32  Node;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "raspberrypi,bootloader-config");
  if (Node >= 0) {
    return Node;
  }

  //
  // A path not starting with '/' is resolved through /aliases.
  //
  return FdtPathOffset (Fdt, "blconfig");
}

/**
  Stage the blconfig region into a pool buffer once, with single-byte
  volatile reads (device memory - see the file header), and record the
  DTB's update-timestamp.
**/
STATIC
VOID
LoadBlconfig (
  VOID
  )
{
  INT32                 Node;
  INT32                 Length;
  CONST UINT32          *Prop;
  UINT64                RegionAddress;
  UINT64                RegionSize;
  UINTN                 DataLen;
  CONST volatile UINT8  *Region;
  UINTN                 Index;

  mFdt = (CONST VOID *)(UINTN)FixedPcdGet32 (PcdFdtBaseAddress);
  if ((mFdt == NULL) || (FdtCheckHeader (mFdt) != 0)) {
    DEBUG ((DEBUG_INFO, "BootloaderConfig: no valid VPU DTB\n"));
    mFdt = NULL;
    return;
  }

  Node = FdtPathOffset (mFdt, "/chosen/bootloader");
  if (Node >= 0) {
    Prop = FdtGetProp (mFdt, Node, "update-timestamp", &Length);
    if ((Prop != NULL) && (Length == sizeof (UINT32))) {
      mDtbUpdateTimestamp = Fdt32ToCpu (*Prop);
      mDtbHasTimestamp    = TRUE;
    }
  }

  Node = FindBlconfigNode (mFdt);
  if (Node < 0) {
    return;
  }

  if (!BlGetTranslatedRegAddress (mFdt, Node, &RegionAddress, &RegionSize)) {
    return;
  }

  if ((RegionAddress == 0) || (RegionSize == 0) ||
      (RegionSize >= BLCONFIG_REGION_MAX))
  {
    return;
  }

  DataLen = MIN ((UINTN)RegionSize, (UINTN)BLCONFIG_VAR_MAX_LEN);
  if (DataLen < RegionSize) {
    DEBUG ((
      DEBUG_WARN,
      "BootloaderConfig: config truncated %Lu -> %u bytes\n",
      RegionSize,
      (UINT32)DataLen
      ));
  }

  mBlconfigRaw = AllocatePool (DataLen);
  if (mBlconfigRaw == NULL) {
    return;
  }

  Region = (CONST volatile UINT8 *)(UINTN)RegionAddress;
  for (Index = 0; Index < DataLen; Index++) {
    mBlconfigRaw[Index] = Region[Index];
  }

  mBlconfigRawLen = DataLen;

  //
  // The effective config text ends at the first NUL or erased byte of
  // the region padding.
  //
  for (Index = 0; Index < DataLen; Index++) {
    if ((mBlconfigRaw[Index] == '\0') || (mBlconfigRaw[Index] == 0xFF)) {
      break;
    }
  }

  mBlconfigTextLen = Index;
}

/**
  Keep the BlCfg efivarstore in sync with the live blconfig values so the
  Setup page always opens on what the running bootloader used. Saved-but-
  never-staged edits (and any out-of-band writes) are deliberately
  overwritten here.
**/
STATIC
VOID
SyncSetupVariable (
  VOID
  )
{
  RPI_BLCFG_DATA  Data;
  RPI_BLCFG_DATA  Existing;
  UINTN           Size;
  EFI_STATUS      Status;

  BlValuesToData (&mCurrentValues, &Data);

  Size   = sizeof (Existing);
  Status = gRT->GetVariable (
                  RPI_BLCFG_VARIABLE_NAME,
                  &gRpiBlCfgFormSetGuid,
                  NULL,
                  &Size,
                  &Existing
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Existing)) &&
      (CompareMem (&Existing, &Data, sizeof (Data)) == 0))
  {
    return;
  }

  Status = gRT->SetVariable (
                  RPI_BLCFG_VARIABLE_NAME,
                  &gRpiBlCfgFormSetGuid,
                  BL_VAR_ATTRIBUTES,
                  sizeof (Data),
                  &Data
                  );
  DEBUG ((DEBUG_INFO, "BootloaderConfig: BlCfg synced - %r\n", Status));
}

/**
  Mirror the blconfig region into BootloaderConfig for the BMC, gated on
  update-timestamp, and follow up on a previously staged EEPROM update.
  One attempt per boot; every silent return matches U-Boot's "nothing to
  gate on / nothing to publish" behavior.
**/
STATIC
VOID
EFIAPI
BootloaderConfigOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  UINT32      StoredTimestamp;
  UINTN       VarSize;

  //
  // Run once per boot: ReadyToBoot can be signaled again on a later boot
  // attempt, but one publish attempt per boot is the contract (a failure
  // retries on the next boot via the timestamp gate).
  //
  if (mPublished) {
    return;
  }

  mPublished = TRUE;
  gBS->CloseEvent (Event);

  //
  // Staged-update follow-up needs the filesystem stack, which exists
  // only after BDS connected the boot devices - hence here and not at
  // entry.
  //
  BlStagedMarkerCleanup ();

  if (!mDtbHasTimestamp) {
    //
    // Nothing to gate on; leave the variables alone (matches U-Boot).
    //
    return;
  }

  VarSize = sizeof (StoredTimestamp);
  Status  = gRT->GetVariable (
                   BMC_VAR_BOOTLOADER_TIMESTAMP,
                   &gRpiBmcBootloaderVendorGuid,
                   NULL,
                   &VarSize,
                   &StoredTimestamp
                   );
  if (!EFI_ERROR (Status) &&
      (VarSize == sizeof (StoredTimestamp)) &&
      (StoredTimestamp >= mDtbUpdateTimestamp))
  {
    //
    // EEPROM unchanged since the last publish: steady-state boots write
    // nothing (the 16 KiB BMC-facing variable budget is shared; wear
    // discipline matters).
    //
    return;
  }

  if (mBlconfigRaw == NULL) {
    return;
  }

  Status = gRT->SetVariable (
                  BMC_VAR_BOOTLOADER_CONFIG,
                  &gRpiBmcBootloaderVendorGuid,
                  BL_VAR_ATTRIBUTES,
                  mBlconfigRawLen,
                  mBlconfigRaw
                  );
  if (EFI_ERROR (Status)) {
    //
    // Do not advance the timestamp: the gate stays open and the config
    // write retries next boot.
    //
    DEBUG ((
      DEBUG_WARN,
      "BootloaderConfig: config variable write failed: %r\n",
      Status
      ));
    return;
  }

  //
  // Advance the stored timestamp LAST, so the next unchanged boot returns
  // early above.
  //
  Status = gRT->SetVariable (
                  BMC_VAR_BOOTLOADER_TIMESTAMP,
                  &gRpiBmcBootloaderVendorGuid,
                  BL_VAR_ATTRIBUTES,
                  sizeof (mDtbUpdateTimestamp),
                  &mDtbUpdateTimestamp
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "BootloaderConfig: timestamp variable write failed: %r\n",
      Status
      ));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootloaderConfig: published %u bytes, timestamp %u\n",
    (UINT32)mBlconfigRawLen,
    mDtbUpdateTimestamp
    ));
}

/**
  Entry point: stage the blconfig text, seed the Setup varstore, publish
  the Setup page and register the ReadyToBoot handler.
**/
EFI_STATUS
EFIAPI
BootloaderConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  LoadBlconfig ();
  BlValuesFromText ((CONST CHAR8 *)mBlconfigRaw, mBlconfigTextLen, &mCurrentValues);
  SyncSetupVariable ();

  Status = BlInstallHiiPage ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BootloaderConfig: Setup page install failed: %r\n", Status));
  }

  return EfiCreateEventReadyToBootEx (
           TPL_CALLBACK,
           BootloaderConfigOnReadyToBoot,
           NULL,
           &mReadyToBootEvent
           );
}

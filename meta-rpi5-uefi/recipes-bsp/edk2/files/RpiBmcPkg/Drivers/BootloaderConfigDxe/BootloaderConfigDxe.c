/** @file

  BootloaderConfigDxe - publish the VPU bootloader EEPROM config as a UEFI
  variable for the BMC.

  The Pi 5 VPU firmware records where the running bootloader came from in
  the DT it hands over: the flash time at /chosen/bootloader/update-timestamp
  and the live EEPROM config text in the blconfig nvmem-rmem region under
  /reserved-memory. At ReadyToBoot this driver mirrors that config into the
  BootloaderConfig variable (vendor gRpiBmcBootloaderVendorGuid, NV|BS|RT)
  so the BMC can read it out-of-band from the EEPROM-backed variable store.

  update-timestamp gates every write: the firmware only rewrites that node
  when the SPI EEPROM is reflashed, so a stored timestamp >= the DT's means
  there is nothing to do and a steady-state boot writes nothing (the 16 KiB
  EEPROM variable store is shared; wear discipline matters). The timestamp
  variable is advanced LAST, so a failed config write leaves the gate open
  and the write retries next boot.

  Ordering vs EepromVarStoreDxe's restore of the variable store from the
  EEPROM is eventually-consistent by design: if a restore races (or lands
  after) this write, the timestamp gate simply reconverges on the next
  boot - either the restored timestamp is stale and the config is written
  again, or it is current and nothing happens.

  Mirrors U-Boot's rpi_publish_bootloader_vars() behavior (same variable
  names, GUID, gating and ordering).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <RpiBmcEeprom.h>

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

STATIC EFI_EVENT  mReadyToBootEvent;
STATIC BOOLEAN    mPublished = FALSE;

/**
  Read a "reg" address/size pair honoring the parent bus's
  #address-cells/#size-cells (2/1 or 2/2 on bcm2712).

  @param[in]  Fdt         The FDT blob.
  @param[in]  NodeOffset  Node whose "reg" to read.
  @param[out] Address     First reg entry's address.
  @param[out] Size        First reg entry's size.

  @retval TRUE   Address/Size extracted.
  @retval FALSE  Missing/short "reg" or unsupported cell counts.
**/
STATIC
BOOLEAN
GetRegAddressSize (
  IN  CONST VOID  *Fdt,
  IN  INT32       NodeOffset,
  OUT UINT64      *Address,
  OUT UINT64      *Size
  )
{
  INT32         Parent;
  INT32         AddressCells;
  INT32         SizeCells;
  INT32         Length;
  CONST UINT32  *Cell;
  UINT64        Value;
  INT32         Index;

  Parent = FdtParentOffset (Fdt, NodeOffset);
  if (Parent < 0) {
    return FALSE;
  }

  //
  // FdtAddressCells()/FdtSizeCells() return the node's own
  // #address-cells/#size-cells, i.e. the ones governing its children -
  // so they are queried on the parent.
  //
  AddressCells = FdtAddressCells (Fdt, Parent);
  SizeCells    = FdtSizeCells (Fdt, Parent);
  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2))
  {
    return FALSE;
  }

  Cell = FdtGetProp (Fdt, NodeOffset, "reg", &Length);
  if ((Cell == NULL) ||
      (Length < (AddressCells + SizeCells) * (INT32)sizeof (UINT32)))
  {
    return FALSE;
  }

  Value = 0;
  for (Index = 0; Index < AddressCells; Index++) {
    Value = LShiftU64 (Value, 32) | Fdt32ToCpu (*Cell);
    Cell++;
  }

  *Address = Value;

  Value = 0;
  for (Index = 0; Index < SizeCells; Index++) {
    Value = LShiftU64 (Value, 32) | Fdt32ToCpu (*Cell);
    Cell++;
  }

  *Size = Value;
  return TRUE;
}

/**
  Locate the blconfig nvmem-rmem node in the VPU DTB: by compatible first,
  then - some firmware versions differ - through the /aliases "blconfig"
  path indirection, like U-Boot's rpi_find_blconfig().

  @param[in] Fdt  The FDT blob.

  @return Node offset, or negative if not found.
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
  Mirror the blconfig region into BootloaderConfig, gated on
  update-timestamp. One attempt per boot; every silent return matches
  U-Boot's "nothing to gate on / nothing to publish" behavior.

  @param[in] Event    The ReadyToBoot event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
BootloaderConfigOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS    Status;
  CONST VOID    *Fdt;
  INT32         Node;
  INT32         Length;
  CONST UINT32  *Prop;
  UINT32        DtbTimestamp;
  UINT32        StoredTimestamp;
  UINTN         VarSize;
  UINT64                 RegionAddress;
  UINT64                 RegionSize;
  UINTN                  DataLen;
  UINT8                  *Data;
  CONST volatile UINT8   *Region;
  UINTN                  Index;

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

  Fdt = (CONST VOID *)(UINTN)FixedPcdGet32 (PcdFdtBaseAddress);
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    DEBUG ((DEBUG_INFO, "BootloaderConfig: no valid VPU DTB\n"));
    return;
  }

  Node = FdtPathOffset (Fdt, "/chosen/bootloader");
  if (Node < 0) {
    return;
  }

  Prop = FdtGetProp (Fdt, Node, "update-timestamp", &Length);
  if ((Prop == NULL) || (Length != sizeof (UINT32))) {
    //
    // Nothing to gate on; leave the variables alone (matches U-Boot).
    //
    return;
  }

  DtbTimestamp = Fdt32ToCpu (*Prop);

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
      (StoredTimestamp >= DtbTimestamp))
  {
    //
    // EEPROM unchanged since the last publish: steady-state boots write
    // nothing.
    //
    return;
  }

  Node = FindBlconfigNode (Fdt);
  if (Node < 0) {
    return;
  }

  if (!GetRegAddressSize (Fdt, Node, &RegionAddress, &RegionSize)) {
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

  //
  // The blconfig region is a VPU carve-out outside the UEFI system-memory
  // map, mapped as device memory -- unaligned/wide accesses to it take an
  // alignment fault (seen in the field as a data abort inside
  // VariableRuntimeDxe's CopyMem when this pointer was passed straight to
  // SetVariable). Stage the bytes into a pool buffer with single-byte
  // reads, which are always aligned and therefore legal on device memory.
  //
  Data = AllocatePool (DataLen);
  if (Data == NULL) {
    DEBUG ((DEBUG_WARN, "BootloaderConfig: out of memory staging config\n"));
    return;
  }

  Region = (CONST volatile UINT8 *)(UINTN)RegionAddress;
  for (Index = 0; Index < DataLen; Index++) {
    Data[Index] = Region[Index];
  }

  Status = gRT->SetVariable (
                  BMC_VAR_BOOTLOADER_CONFIG,
                  &gRpiBmcBootloaderVendorGuid,
                  BL_VAR_ATTRIBUTES,
                  DataLen,
                  Data
                  );
  FreePool (Data);
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
                  sizeof (DtbTimestamp),
                  &DtbTimestamp
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
    (UINT32)DataLen,
    DtbTimestamp
    ));
}

/**
  Entry point: register the ReadyToBoot handler.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  Event registered.
  @return              Error from CreateEventEx otherwise.
**/
EFI_STATUS
EFIAPI
BootloaderConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiCreateEventReadyToBootEx (
           TPL_CALLBACK,
           BootloaderConfigOnReadyToBoot,
           NULL,
           &mReadyToBootEvent
           );
}

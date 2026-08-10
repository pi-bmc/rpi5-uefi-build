/** @file

  Mirror the SMBIOS tables into the BMC shared EEPROM.

  At ReadyToBoot, the SMBIOS 3.0 (64-bit) entry point and the structure
  table it points at are serialized into a single blob and written to the
  EEPROM's SMBIOS region (PcdBmcEepromSmbiosOffset), where the BMC's
  nanokvm-app parses them to present the host's inventory out-of-band.

  Blob layout (frozen contract, see Include/RpiBmcEeprom.h and the pi-bmc
  U-Boot port's SMBIOS I2C store):

    +0x0000                              SMBIOS 3.0 entry point, verbatim
                                         (EntryPointLength bytes)
    ...zero padding...
    +ALIGN (EntryPointLength, 16)        structure table
                                         (TableMaximumSize bytes)

  The entry point is kept byte-for-byte as published in DRAM; in
  particular its TableAddress still holds the meaningless DRAM address.
  EEPROM readers must instead locate the table at the aligned offset
  above - the blob is otherwise self-describing ("_SM3_" anchor,
  TableMaximumSize).

  If the blob does not fit the region, a single warning is logged and
  nothing is written (same semantics as U-Boot's ENOSPC path).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <IndustryStandard/SmBios.h>
#include <Guid/SmBios.h>

#include <Library/BaseMemoryLib.h>
#include <Library/BmcEepromLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

/**
  ReadyToBoot notification: serialize the SMBIOS 3.0 entry point and
  structure table and mirror them into the BMC EEPROM.

  Runs once - the event is closed on entry, so a retried boot option does
  not re-mirror (BmcEepromWriteIfChanged would make that a no-op anyway).

  @param[in] Event    The ReadyToBoot event.
  @param[in] Context  Unused.

**/
STATIC
VOID
EFIAPI
SmbiosEepromMirrorOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                    Status;
  SMBIOS_TABLE_3_0_ENTRY_POINT  *EntryPoint;
  EFI_I2C_MASTER_PROTOCOL       *I2cMaster;
  UINT8                         *Blob;
  UINTN                         TableOffset;
  UINTN                         TotalSize;
  BOOLEAN                       Wrote;

  gBS->CloseEvent (Event);

  Status = EfiGetSystemConfigurationTable (
             &gEfiSmbios3TableGuid,
             (VOID **)&EntryPoint
             );
  if (EFI_ERROR (Status) || (EntryPoint == NULL)) {
    DEBUG ((DEBUG_INFO,
      "%a: no SMBIOS 3.0 entry point published - nothing to mirror\n",
      __func__));
    return;
  }

  //
  // One blob: entry point verbatim, zero padding up to a 16-byte
  // boundary, then the structure table.
  //
  TableOffset = ALIGN_VALUE ((UINTN)EntryPoint->EntryPointLength, 16);
  TotalSize   = TableOffset + EntryPoint->TableMaximumSize;

  if (TotalSize > FixedPcdGet32 (PcdBmcEepromSmbiosSize)) {
    DEBUG ((DEBUG_WARN,
      "%a: SMBIOS blob (%u bytes) exceeds EEPROM region (%u bytes) - not mirrored\n",
      __func__, (UINT32)TotalSize, FixedPcdGet32 (PcdBmcEepromSmbiosSize)));
    return;
  }

  Blob = AllocateZeroPool (TotalSize);
  if (Blob == NULL) {
    DEBUG ((DEBUG_WARN, "%a: failed to allocate %u bytes\n",
      __func__, (UINT32)TotalSize));
    return;
  }

  CopyMem (Blob, EntryPoint, EntryPoint->EntryPointLength);
  CopyMem (
    Blob + TableOffset,
    (VOID *)(UINTN)EntryPoint->TableAddress,
    EntryPoint->TableMaximumSize
    );

  Status = BmcEepromLocateI2cMaster (&I2cMaster);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: no I2C master available (%r) - not mirrored\n",
      __func__, Status));
    FreePool (Blob);
    return;
  }

  Wrote  = FALSE;
  Status = BmcEepromWriteIfChanged (
             I2cMaster,
             FixedPcdGet32 (PcdBmcEepromSmbiosOffset),
             TotalSize,
             Blob,
             &Wrote
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: EEPROM write failed: %r\n", __func__, Status));
  } else {
    DEBUG ((DEBUG_INFO, "%a: %u-byte SMBIOS blob %a at EEPROM offset 0x%x\n",
      __func__, (UINT32)TotalSize,
      Wrote ? "written" : "unchanged",
      FixedPcdGet32 (PcdBmcEepromSmbiosOffset)));
  }

  FreePool (Blob);
}

/**
  Driver entry point: register the ReadyToBoot notification.

  @param[in] ImageHandle  The firmware allocated handle for this driver.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS  The notification was registered.
  @retval other        Event creation failed.

**/
EFI_STATUS
EFIAPI
SmbiosEepromMirrorEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             SmbiosEepromMirrorOnReadyToBoot,
             NULL,
             &Event
             );
  ASSERT_EFI_ERROR (Status);

  return Status;
}

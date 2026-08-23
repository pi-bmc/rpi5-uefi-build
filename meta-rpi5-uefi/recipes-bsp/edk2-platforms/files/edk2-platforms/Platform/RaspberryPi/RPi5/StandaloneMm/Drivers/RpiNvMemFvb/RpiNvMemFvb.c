/** @file

  Memory-window FVB for the RPi5 StandaloneMM variable store.

  The store is the RPi5.fdf NV region of the VPU-loaded FD (armstub8-2712.bin
  / RPI_EFI.fd, loaded at PA 0 before any ARM code runs), which OP-TEE maps
  into this secure partition non-secure + cached at a fixed VA
  (CFG_STMM_VARSTORE_* in optee_os plat-rpi5; PcdFlashNvStorageVariableBase64
  here). Every FVB operation is a plain memory access -- no device, no
  OP-TEE storage-service round trips, nothing to synchronize. Persisting the
  window back to the file on the boot FAT is the normal world's job
  (MmCommunicationOpteeDxe, VarBlockServiceDxe-style).

  Layout served (one FVB spanning the whole window, 4KB blocks, matching the
  FV header the FDF bakes into the FD):

    +0x0000  variable store FV header + authenticated varstore   (0xe000)
    +0xe000  NS event log page (not touched from here)           (0x1000)
    +0xf000  FTW working block                                   (0x1000)
    +0x10000 FTW spare                                           (0x10000)

  Derived from Drivers/OpTee/OpteeRpmbPkg/OpTeeRpmbFvb.c.

  Copyright (c) 2020, Linaro Ltd. All rights reserved.
  Copyright (c) 2026, pi-bmc contributors

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiMm.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MmServicesTableLib.h>
#include <Library/PcdLib.h>

#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/SmmFirmwareVolumeBlock.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>

#define NV_BLOCK_SIZE  SIZE_4KB

STATIC EFI_HANDLE  mHandle;
STATIC UINTN       mWindowBase;
STATIC UINTN       mWindowSize;
STATIC UINTN       mNBlocks;

/**
  The GetAttributes() function retrieves the attributes and current settings
  of the block device.

  @param[in]  This        FVB protocol instance.
  @param[out] Attributes  Attributes and current settings.

  @retval EFI_SUCCESS     Attributes returned.
**/
STATIC
EFI_STATUS
EFIAPI
FvbGetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT      EFI_FVB_ATTRIBUTES_2                *Attributes
  )
{
  *Attributes = EFI_FVB2_READ_ENABLED_CAP   |
                EFI_FVB2_READ_STATUS        |
                EFI_FVB2_WRITE_STATUS       |
                EFI_FVB2_WRITE_ENABLED_CAP  |
                EFI_FVB2_STICKY_WRITE       |
                EFI_FVB2_MEMORY_MAPPED      |
                EFI_FVB2_ERASE_POLARITY;

  return EFI_SUCCESS;
}

/**
  The SetAttributes() function sets configurable firmware volume attributes.

  @param[in]     This        FVB protocol instance.
  @param[in,out] Attributes  Desired settings.

  @retval EFI_UNSUPPORTED    Not supported.
**/
STATIC
EFI_STATUS
EFIAPI
FvbSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN OUT   EFI_FVB_ATTRIBUTES_2                *Attributes
  )
{
  return EFI_UNSUPPORTED;
}

/**
  The GetPhysicalAddress() function retrieves the base address of the
  memory-mapped firmware volume: the SP VA of the mapped FD NV window.

  @param[in]  This     FVB protocol instance.
  @param[out] Address  Base address.

  @retval EFI_SUCCESS  Address returned.
**/
STATIC
EFI_STATUS
EFIAPI
FvbGetPhysicalAddress (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT      EFI_PHYSICAL_ADDRESS                *Address
  )
{
  *Address = mWindowBase;
  return EFI_SUCCESS;
}

/**
  The GetBlockSize() function retrieves the size of the requested block and
  the number of consecutive identically-sized blocks that follow it.

  @param[in]  This            FVB protocol instance.
  @param[in]  Lba             Block index queried.
  @param[out] BlockSize       Size of the block.
  @param[out] NumberOfBlocks  Consecutive blocks of that size from Lba.

  @retval EFI_SUCCESS            Returned.
  @retval EFI_INVALID_PARAMETER  Lba out of range.
**/
STATIC
EFI_STATUS
EFIAPI
FvbGetBlockSize (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN       EFI_LBA                             Lba,
  OUT      UINTN                               *BlockSize,
  OUT      UINTN                               *NumberOfBlocks
  )
{
  if (Lba >= mNBlocks) {
    return EFI_INVALID_PARAMETER;
  }

  *BlockSize      = NV_BLOCK_SIZE;
  *NumberOfBlocks = mNBlocks - (UINTN)Lba;

  return EFI_SUCCESS;
}

/**
  Bounds-check one block-relative access and return its window offset.

  @param[in]     Lba       Block index.
  @param[in]     Offset    Byte offset into the block.
  @param[in,out] NumBytes  Requested size; clamped to the block end.
  @param[out]    Pos       Byte offset into the window.

  @retval EFI_SUCCESS          Access is within the volume (possibly clamped).
  @retval EFI_BAD_BUFFER_SIZE  Request was clamped at the block boundary.
  @retval EFI_INVALID_PARAMETER  Lba/Offset outside the volume.
**/
STATIC
EFI_STATUS
FvbCheckRange (
  IN     EFI_LBA  Lba,
  IN     UINTN    Offset,
  IN OUT UINTN    *NumBytes,
  OUT    UINTN    *Pos
  )
{
  if ((Lba >= mNBlocks) || (Offset >= NV_BLOCK_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }

  *Pos = (UINTN)Lba * NV_BLOCK_SIZE + Offset;
  if (*NumBytes > NV_BLOCK_SIZE - Offset) {
    *NumBytes = NV_BLOCK_SIZE - Offset;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

/**
  Read from the window.

  @param[in]     This      FVB protocol instance.
  @param[in]     Lba       Starting block.
  @param[in]     Offset    Byte offset into the block.
  @param[in,out] NumBytes  Bytes to read / bytes read.
  @param[in,out] Buffer    Destination.

  @retval EFI_SUCCESS          Read.
  @retval EFI_BAD_BUFFER_SIZE  Read clamped at the block boundary.
**/
STATIC
EFI_STATUS
EFIAPI
FvbRead (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN       EFI_LBA                             Lba,
  IN       UINTN                               Offset,
  IN OUT   UINTN                               *NumBytes,
  IN OUT   UINT8                               *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Pos;

  Status = FvbCheckRange (Lba, Offset, NumBytes, &Pos);
  if (Status == EFI_INVALID_PARAMETER) {
    return Status;
  }

  CopyMem (Buffer, (VOID *)(mWindowBase + Pos), *NumBytes);
  return Status;
}

/**
  Write to the window. The write is complete when the CopyMem returns: the
  backing is ordinary cacheable RAM shared coherently with the normal world
  (both sides map it non-secure), so there is nothing to flush here.

  @param[in]     This      FVB protocol instance.
  @param[in]     Lba       Starting block.
  @param[in]     Offset    Byte offset into the block.
  @param[in,out] NumBytes  Bytes to write / bytes written.
  @param[in]     Buffer    Source.

  @retval EFI_SUCCESS          Written.
  @retval EFI_BAD_BUFFER_SIZE  Write clamped at the block boundary.
**/
STATIC
EFI_STATUS
EFIAPI
FvbWrite (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN       EFI_LBA                             Lba,
  IN       UINTN                               Offset,
  IN OUT   UINTN                               *NumBytes,
  IN       UINT8                               *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Pos;

  Status = FvbCheckRange (Lba, Offset, NumBytes, &Pos);
  if (Status == EFI_INVALID_PARAMETER) {
    return Status;
  }

  CopyMem ((VOID *)(mWindowBase + Pos), Buffer, *NumBytes);
  return Status;
}

/**
  Erase blocks: set them to the erase polarity (all 0xFF).

  @param[in] This  FVB protocol instance.
  @param[in] ...   (Lba, NumBlocks) pairs, EFI_LBA_LIST_TERMINATOR-ended.

  @retval EFI_SUCCESS            Erased.
  @retval EFI_INVALID_PARAMETER  A range is outside the volume.
**/
STATIC
EFI_STATUS
EFIAPI
FvbEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  ...
  )
{
  VA_LIST  Args;
  EFI_LBA  Start;
  UINTN    NumLba;

  //
  // Validate the whole list before touching anything, as the PI spec
  // requires.
  //
  VA_START (Args, This);
  for (Start = VA_ARG (Args, EFI_LBA);
       Start != EFI_LBA_LIST_TERMINATOR;
       Start = VA_ARG (Args, EFI_LBA))
  {
    NumLba = VA_ARG (Args, UINTN);
    if ((NumLba == 0) || (Start + NumLba > mNBlocks)) {
      VA_END (Args);
      return EFI_INVALID_PARAMETER;
    }
  }

  VA_END (Args);

  VA_START (Args, This);
  for (Start = VA_ARG (Args, EFI_LBA);
       Start != EFI_LBA_LIST_TERMINATOR;
       Start = VA_ARG (Args, EFI_LBA))
  {
    NumLba = VA_ARG (Args, UINTN);
    SetMem64 (
      (VOID *)(mWindowBase + (UINTN)Start * NV_BLOCK_SIZE),
      NumLba * NV_BLOCK_SIZE,
      ~0UL
      );
  }

  VA_END (Args);

  return EFI_SUCCESS;
}

STATIC EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  mFvbProtocol = {
  FvbGetAttributes,
  FvbSetAttributes,
  FvbGetPhysicalAddress,
  FvbGetBlockSize,
  FvbRead,
  FvbWrite,
  FvbEraseBlocks,
  NULL
};

/**
  Validate the FV + variable store headers the FDF bakes into the FD window.

  @param[in] FwVolHeader  Header at the window base.

  @retval EFI_SUCCESS           Valid.
  @retval EFI_VOLUME_CORRUPTED  Not our NV store.
**/
STATIC
EFI_STATUS
ValidateFvHeader (
  IN EFI_FIRMWARE_VOLUME_HEADER  *FwVolHeader
  )
{
  VARIABLE_STORE_HEADER  *VariableStoreHeader;

  if ((FwVolHeader->Revision != EFI_FVH_REVISION) ||
      (FwVolHeader->Signature != EFI_FVH_SIGNATURE) ||
      (FwVolHeader->FvLength != mWindowSize) ||
      (FwVolHeader->HeaderLength >= NV_BLOCK_SIZE) ||
      ((FwVolHeader->HeaderLength & 1) != 0))
  {
    return EFI_VOLUME_CORRUPTED;
  }

  if (!CompareGuid (&FwVolHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid)) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (CalculateSum16 ((UINT16 *)FwVolHeader, FwVolHeader->HeaderLength) != 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  VariableStoreHeader = (VARIABLE_STORE_HEADER *)((UINTN)FwVolHeader +
                                                  FwVolHeader->HeaderLength);
  if (!CompareGuid (&VariableStoreHeader->Signature, &gEfiVariableGuid) &&
      !CompareGuid (&VariableStoreHeader->Signature, &gEfiAuthenticatedVariableGuid))
  {
    return EFI_VOLUME_CORRUPTED;
  }

  if (VariableStoreHeader->Size !=
      PcdGet32 (PcdFlashNvStorageVariableSize) - FwVolHeader->HeaderLength)
  {
    return EFI_VOLUME_CORRUPTED;
  }

  return EFI_SUCCESS;
}

/**
  Reformat the window in place: erase it and write fresh FV + authenticated
  variable store headers matching what RPi5.fdf bakes in. The FTW working
  block header is deliberately left erased -- FaultTolerantWriteStandaloneMm
  detects the invalid working block and reinitializes it itself.

  Normally never runs (the VPU-loaded FD always carries the baked headers);
  it exists so a corrupted card degrades to a fresh store instead of a
  non-functional variable service.
**/
STATIC
VOID
InitializeFvAndVariableStoreHeaders (
  VOID
  )
{
  EFI_FIRMWARE_VOLUME_HEADER  *FwVolHeader;
  VARIABLE_STORE_HEADER       *VariableStoreHeader;

  SetMem64 ((VOID *)mWindowBase, mWindowSize, ~0UL);

  FwVolHeader = (EFI_FIRMWARE_VOLUME_HEADER *)mWindowBase;
  ZeroMem (FwVolHeader, sizeof (*FwVolHeader) + sizeof (EFI_FV_BLOCK_MAP_ENTRY));
  CopyGuid (&FwVolHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid);
  FwVolHeader->FvLength   = mWindowSize;
  FwVolHeader->Signature  = EFI_FVH_SIGNATURE;
  FwVolHeader->Attributes = EFI_FVB2_READ_ENABLED_CAP |
                            EFI_FVB2_READ_STATUS |
                            EFI_FVB2_STICKY_WRITE |
                            EFI_FVB2_MEMORY_MAPPED |
                            EFI_FVB2_ERASE_POLARITY |
                            EFI_FVB2_WRITE_STATUS |
                            EFI_FVB2_WRITE_ENABLED_CAP;
  FwVolHeader->HeaderLength = sizeof (EFI_FIRMWARE_VOLUME_HEADER) +
                              sizeof (EFI_FV_BLOCK_MAP_ENTRY);
  FwVolHeader->Revision              = EFI_FVH_REVISION;
  FwVolHeader->BlockMap[0].NumBlocks = (UINT32)mNBlocks;
  FwVolHeader->BlockMap[0].Length    = NV_BLOCK_SIZE;
  FwVolHeader->BlockMap[1].NumBlocks = 0;
  FwVolHeader->BlockMap[1].Length    = 0;
  FwVolHeader->Checksum              = CalculateCheckSum16 (
                                         (UINT16 *)FwVolHeader,
                                         FwVolHeader->HeaderLength
                                         );

  VariableStoreHeader = (VARIABLE_STORE_HEADER *)((UINTN)FwVolHeader +
                                                  FwVolHeader->HeaderLength);
  ZeroMem (VariableStoreHeader, sizeof (*VariableStoreHeader));
  CopyGuid (&VariableStoreHeader->Signature, &gEfiAuthenticatedVariableGuid);
  VariableStoreHeader->Size = PcdGet32 (PcdFlashNvStorageVariableSize) -
                              FwVolHeader->HeaderLength;
  VariableStoreHeader->Format = VARIABLE_STORE_FORMATTED;
  VariableStoreHeader->State  = VARIABLE_STORE_HEALTHY;
}

/**
  Driver entry: locate the mapped FD NV window through the NV storage PCDs,
  validate (or restore) its headers and install the MM FVB protocol that
  FaultTolerantWriteStandaloneMm and VariableStandaloneMm dispatch on.

  @param[in] ImageHandle  Image handle.
  @param[in] SystemTable  MM system table.

  @retval EFI_SUCCESS  FVB installed.
**/
EFI_STATUS
EFIAPI
RpiNvMemFvbInit (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_MM_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  mWindowBase = (UINTN)PcdGet64 (PcdFlashNvStorageVariableBase64);
  mWindowSize = (UINTN)(PcdGet64 (PcdFlashNvStorageFtwSpareBase64) +
                        PcdGet32 (PcdFlashNvStorageFtwSpareSize)) -
                mWindowBase;
  mNBlocks = mWindowSize / NV_BLOCK_SIZE;

  ASSERT (mWindowBase != 0);
  ASSERT ((mWindowBase % NV_BLOCK_SIZE) == 0);
  ASSERT ((mWindowSize % NV_BLOCK_SIZE) == 0);
  ASSERT ((PcdGet64 (PcdFlashNvStorageVariableBase64) % NV_BLOCK_SIZE) == 0);
  ASSERT ((PcdGet64 (PcdFlashNvStorageFtwWorkingBase64) % NV_BLOCK_SIZE) == 0);
  ASSERT ((PcdGet64 (PcdFlashNvStorageFtwSpareBase64) % NV_BLOCK_SIZE) == 0);

  Status = ValidateFvHeader ((EFI_FIRMWARE_VOLUME_HEADER *)mWindowBase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no valid NV store in the FD window, reformatting\n", __func__));
    InitializeFvAndVariableStoreHeaders ();
  }

  Status = gMmst->MmInstallProtocolInterface (
                    &mHandle,
                    &gEfiSmmFirmwareVolumeBlockProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &mFvbProtocol
                    );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((
    DEBUG_INFO,
    "%a: FVB over FD NV window at 0x%lx (%u blocks)\n",
    __func__,
    (UINT64)mWindowBase,
    (UINT32)mNBlocks
    ));

  return Status;
}

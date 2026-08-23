/** @file
  Persistence engine for the StMM-owned variable store.

  StMM (via OP-TEE's CFG_STMM_VARSTORE_* mapping and the RpiNvMemFvb driver)
  reads and writes UEFI variables directly in the RPi5.fdf NV window of the
  VPU-loaded FD -- plain RAM at PcdNvStorageVariableBase, loaded from
  armstub8-2712.bin / RPI_EFI.fd before any ARM code runs. This file writes
  that window back into the file on the boot FAT so the next boot sees the
  updated store. It is Platform/RaspberryPi/Drivers/VarBlockServiceDxe's
  persistence model with one difference: dirtiness is signalled by the MM
  transport (VarStoreSyncMarkDirty(), called for every successful
  SetVariable communicate) because the writes happen inside StMM, not
  through an NS FVB this driver could observe.

  Flush points:
    - volume adoption (SimpleFileSystem protocol notify): one unconditional
      dump, healing a file torn by an earlier power loss;
    - ReadyToBoot, then every LoadedImage install after it (boot apps write
      Boot#### etc.): dump if dirty;
    - reset notification (armed as soon as ResetSystemRuntimeDxe appears, so
      a "save & reset" from Setup persists): dump if dirty.
  ResetSystemRuntimeDxe only runs reset notifications before ExitBootServices,
  and all events here are boot-services events, so nothing in this file runs
  at OS runtime.

  Copyright (c) 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
  Copyright (c) 2026, pi-bmc contributors

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/EventGroup.h>
#include <Guid/SystemNvDataGuid.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ResetNotification.h>
#include <Protocol/SimpleFileSystem.h>

#include "MmCommunicationOptee.h"

//
// The firmware file doubles as the NV variable store. Which name this card
// uses is a property of the SD layout (see VarBlockServiceDxe/FileIo.c for
// the full story): armstub8-2712.bin is the BCM2712 default the VPU
// auto-loads; RPI_EFI.FD is the upstream rpi5-uefi install flow's explicit
// armstub= name. Probe both, latch whichever carries our NV store.
//
STATIC CHAR16  *CONST  mMappedFileNames[] = {
  L"armstub8-2712.bin",
  L"RPI_EFI.FD"
};

STATIC UINTN                     mWindowBase;    // PA of the NV window
STATIC UINTN                     mWindowSize;    // variable store..FTW spare end
STATIC UINTN                     mFileOffset;    // window offset inside the FD file
STATIC EFI_DEVICE_PATH_PROTOCOL  *mDevice;       // latched volume, NULL until adopted
STATIC CHAR16                    *mMappedFile;   // latched name on that volume
STATIC BOOLEAN                   mDirty;
STATIC BOOLEAN                   mResetNotifyArmed;
STATIC VOID                      *mSfsRegistration;
STATIC VOID                      *mResetRegistration;

/**
  Open a file in the root of the volume identified by Device.

  @param[in]  Device    Device path of the volume.
  @param[in]  Name      File name in the volume root.
  @param[out] File      Opened file on success.
  @param[in]  OpenMode  EFI_FILE_MODE_* bits.

  @retval EFI_SUCCESS  Opened.
**/
STATIC
EFI_STATUS
FileOpen (
  IN  EFI_DEVICE_PATH_PROTOCOL  *Device,
  IN  CHAR16                    *Name,
  OUT EFI_FILE_PROTOCOL         **File,
  IN  UINT64                    OpenMode
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Volume;
  EFI_FILE_PROTOCOL                *Root;
  EFI_HANDLE                       Handle;
  EFI_STATUS                       Status;

  *File = NULL;

  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Device, &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Volume);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Root   = NULL;
  Status = Volume->OpenVolume (Volume, &Root);
  if (EFI_ERROR (Status) || (Root == NULL)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Root->Open (Root, File, Name, OpenMode, 0);
  if (EFI_ERROR (Status)) {
    *File = NULL;
  }

  Root->Close (Root);
  return Status;
}

/**
  Write Size bytes from Buffer at Offset in File.
**/
STATIC
EFI_STATUS
FileWrite (
  IN EFI_FILE_PROTOCOL  *File,
  IN UINTN              Offset,
  IN VOID               *Buffer,
  IN UINTN              Size
  )
{
  EFI_STATUS  Status;

  Status = File->SetPosition (File, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return File->Write (File, &Size, Buffer);
}

/**
  Probe helper: read Size bytes at Offset. Short/absent files are expected
  answers, not bugs.
**/
STATIC
EFI_STATUS
FileRead (
  IN     EFI_FILE_PROTOCOL  *File,
  IN     UINTN              Offset,
  OUT    VOID               *Buffer,
  IN OUT UINTN              *Size
  )
{
  EFI_STATUS  Status;

  Status = File->SetPosition (File, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return File->Read (File, Size, Buffer);
}

STATIC
VOID
FileClose (
  IN EFI_FILE_PROTOCOL  *File
  )
{
  File->Flush (File);
  File->Close (File);
}

/**
  Test one candidate name on one volume: the file must actually carry this
  firmware's NV store (an EFI_FIRMWARE_VOLUME_HEADER tagged with
  gEfiSystemNvDataFvGuid at exactly the offset the dump writes to) --
  opening alone is not enough, armstub8-2712.bin on some other card's layout
  is somebody else's bootloader and a 128KB write into it would corrupt it.

  @retval EFI_SUCCESS    The file holds our variable store.
  @retval EFI_NOT_FOUND  It does not; keep looking.
**/
STATIC
EFI_STATUS
CheckStoreFile (
  IN EFI_DEVICE_PATH_PROTOCOL  *Device,
  IN CHAR16                    *Name
  )
{
  EFI_FIRMWARE_VOLUME_HEADER  FwVolHeader;
  EFI_FILE_PROTOCOL           *File;
  EFI_STATUS                  Status;
  UINTN                       Size;

  Status = FileOpen (Device, Name, &File, EFI_FILE_MODE_READ);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Size   = sizeof (FwVolHeader);
  Status = FileRead (File, mFileOffset, &FwVolHeader, &Size);
  FileClose (File);

  if (EFI_ERROR (Status) || (Size != sizeof (FwVolHeader))) {
    return EFI_NOT_FOUND;
  }

  if ((FwVolHeader.Signature != EFI_FVH_SIGNATURE) ||
      !CompareGuid (&FwVolHeader.FileSystemGuid, &gEfiSystemNvDataFvGuid))
  {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Check whether the volume on SimpleFileSystemHandle carries our store:
  writable media plus one of the candidate file names passing CheckStoreFile.

  @param[in]  SimpleFileSystemHandle  Volume handle to probe.
  @param[out] Device                  Duplicated device path on success.

  @retval EFI_SUCCESS  Store found; mMappedFile latched.
**/
STATIC
EFI_STATUS
CheckStore (
  IN  EFI_HANDLE                SimpleFileSystemHandle,
  OUT EFI_DEVICE_PATH_PROTOCOL  **Device
  )
{
  EFI_BLOCK_IO_PROTOCOL  *BlkIo;
  EFI_STATUS             Status;
  UINTN                  Index;

  *Device = NULL;
  Status  = gBS->HandleProtocol (
                   SimpleFileSystemHandle,
                   &gEfiBlockIoProtocolGuid,
                   (VOID **)&BlkIo
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!BlkIo->Media->MediaPresent || BlkIo->Media->ReadOnly) {
    return EFI_ACCESS_DENIED;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < ARRAY_SIZE (mMappedFileNames); Index++) {
    Status = CheckStoreFile (
               DevicePathFromHandle (SimpleFileSystemHandle),
               mMappedFileNames[Index]
               );
    if (!EFI_ERROR (Status)) {
      mMappedFile = mMappedFileNames[Index];
      break;
    }
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Device = DuplicateDevicePath (DevicePathFromHandle (SimpleFileSystemHandle));
  if (*Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

/**
  TRUE while the latched volume is still present.
**/
STATIC
BOOLEAN
CheckStoreExists (
  IN EFI_DEVICE_PATH_PROTOCOL  *Device
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Volume;
  EFI_HANDLE                       Handle;

  if (EFI_ERROR (gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Device, &Handle))) {
    return FALSE;
  }

  return !EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Volume));
}

/**
  Write the whole NV window (variable store + event log + FTW regions, one
  consistent snapshot -- events are serialized behind the MM calls, so no
  FTW transaction is ever in flight here) into the latched file.
**/
STATIC
EFI_STATUS
DoDump (
  IN EFI_DEVICE_PATH_PROTOCOL  *Device
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS         Status;

  Status = FileOpen (
             Device,
             mMappedFile,
             &File,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FileWrite (File, mFileOffset, (VOID *)mWindowBase, mWindowSize);
  FileClose (File);
  return Status;
}

STATIC
VOID
DumpIfDirty (
  VOID
  )
{
  EFI_STATUS  Status;

  if ((mDevice == NULL) || !mDirty) {
    return;
  }

  Status = DoDump (mDevice);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: dumping the variable store to '%s' failed - %r\n",
      __func__,
      mMappedFile,
      Status
      ));
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: variable store persisted to '%s'\n", __func__, mMappedFile));
  mDirty = FALSE;
}

STATIC
VOID
EFIAPI
DumpOnEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DumpIfDirty ();
}

STATIC
VOID
EFIAPI
DumpOnReset (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  DumpIfDirty ();
}

/**
  SimpleFileSystem protocol notify: adopt the volume that carries our store.
  The adoption dump is unconditional so a file torn by a power loss mid-dump
  is repaired from the (valid, VPU-loaded + StMM-maintained) RAM window.
**/
STATIC
VOID
EFIAPI
OnSimpleFileSystemInstall (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Device;
  EFI_HANDLE                Handle;
  EFI_STATUS                Status;
  UINTN                     HandleSize;

  if ((mDevice != NULL) && CheckStoreExists (mDevice)) {
    return;
  }

  while (TRUE) {
    HandleSize = sizeof (EFI_HANDLE);
    Status     = gBS->LocateHandle (
                        ByRegisterNotify,
                        NULL,
                        mSfsRegistration,
                        &HandleSize,
                        &Handle
                        );
    if (EFI_ERROR (Status)) {
      break;
    }

    Status = CheckStore (Handle, &Device);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = DoDump (Device);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: adoption dump to '%s' failed - %r\n",
        __func__,
        mMappedFile,
        Status
        ));
      gBS->FreePool (Device);
      continue;
    }

    if (mDevice != NULL) {
      gBS->FreePool (mDevice);
    }

    mDevice = Device;
    mDirty  = FALSE;
    DEBUG ((DEBUG_INFO, "%a: variable store file found ('%s')\n", __func__, mMappedFile));
    break;
  }
}

/**
  ResetNotification protocol notify: arm the reset-time dump the moment
  ResetSystemRuntimeDxe shows up (this driver dispatches long before it, so
  a plain LocateProtocol at entry would miss it -- and a "save & reset" from
  Setup never passes through ReadyToBoot).
**/
STATIC
VOID
EFIAPI
OnResetNotifyInstall (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotify;
  EFI_STATUS                       Status;

  if (mResetNotifyArmed) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiResetNotificationProtocolGuid, NULL, (VOID **)&ResetNotify);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = ResetNotify->RegisterResetNotify (ResetNotify, DumpOnReset);
  if (!EFI_ERROR (Status)) {
    mResetNotifyArmed = TRUE;
    gBS->CloseEvent (Event);
  }
}

/**
  ReadyToBoot: dump, then keep dumping after every LoadedImage install --
  boot options and boot apps write variables (Boot####, BootOrder) after
  ReadyToBoot and before ExitBootServices.
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_EVENT   ImageInstallEvent;
  VOID        *ImageRegistration;
  EFI_STATUS  Status;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  DumpOnEvent,
                  NULL,
                  &ImageInstallEvent
                  );
  if (!EFI_ERROR (Status)) {
    gBS->RegisterProtocolNotify (
           &gEfiLoadedImageProtocolGuid,
           ImageInstallEvent,
           &ImageRegistration
           );
  }

  DumpIfDirty ();
  gBS->CloseEvent (Event);
}

/**
  Mark the store dirty. Called by the MM transport for every successful
  SetVariable communicate -- the only path that changes the NV window.
**/
VOID
VarStoreSyncMarkDirty (
  VOID
  )
{
  mDirty = TRUE;
}

/**
  Set up the persistence engine: geometry from the same PCDs RPi5.fdf sets
  for the NV regions, then the protocol notifies and events listed in the
  file header.

  @retval EFI_SUCCESS  Armed.
**/
EFI_STATUS
VarStoreSyncInit (
  VOID
  )
{
  EFI_EVENT   Event;
  EFI_STATUS  Status;

  mWindowBase = PcdGet32 (PcdNvStorageVariableBase);
  mWindowSize = (PcdGet32 (PcdNvStorageFtwSpareBase) +
                 PcdGet32 (PcdFlashNvStorageFtwSpareSize)) -
                mWindowBase;
  mFileOffset = mWindowBase - (UINTN)FixedPcdGet64 (PcdFdBaseAddress);

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnSimpleFileSystemInstall,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiSimpleFileSystemProtocolGuid,
                  Event,
                  &mSfsRegistration
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnResetNotifyInstall,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiResetNotificationProtocolGuid,
                  Event,
                  &mResetRegistration
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // ResetSystemRuntimeDxe may already be up (protocol notify only fires on
  // future installs) -- probe once now.
  //
  OnResetNotifyInstall (Event, NULL);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: NV window 0x%lx+0x%lx -> file offset 0x%lx\n",
    __func__,
    (UINT64)mWindowBase,
    (UINT64)mWindowSize,
    (UINT64)mFileOffset
    ));

  return EFI_SUCCESS;
}

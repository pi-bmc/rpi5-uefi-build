/** @file

  FmpDeviceLib for the Raspberry Pi 5's own UEFI firmware image.

  The Pi 5 has no SPI flash of its own to update: the VPU bootloader loads the
  firmware from a file on the FAT boot partition, so "writing the firmware" here
  means writing that file. This is the same object VarBlockServiceDxe persists
  EFI variables into, found the same way and under the same two names, because
  it is literally the same file.

  Which leads to the one thing this library exists to get right:

      The firmware image and the NV variable store share one file.

  RPi5.fdf lays the FD out as [0, 0x3b0000) firmware, then the variable store,
  the event log and the fault-tolerant-write blocks out to 0x3d0000. A capsule
  that wrote the whole file would take the boot entries, the Secure Boot keys
  and every BIOS setting with it -- silently, and only visibly on the next boot.
  So the updatable region stops at PcdNvStorageVariableBase and the tail is left
  exactly as it was found. That is also why FmpDeviceGetSize() reports the
  firmware region rather than the file size: it is what a capsule may replace.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>

#include <Guid/FileInfo.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/SystemResourceTable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FmpDeviceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

//
// The firmware file, under both names it is known by. Kept in step with
// VarBlockServiceDxe's candidate list (patch 0013): this platform deploys the
// FD as armstub8-2712.bin, the default BCM2712 stub name the VPU bootloader
// auto-loads, while the upstream Raspberry Pi layout keeps it as RPI_EFI.fd.
// A card written either way is updatable.
//
STATIC CHAR16 *CONST  mFirmwareFileNames[] = {
  L"armstub8-2712.bin",
  L"RPI_EFI.FD"
};

//
// Byte range of the file a capsule may replace: everything before the NV store.
//
#define RPI5_FMP_UPDATABLE_SIZE \
  ((UINTN)(FixedPcdGet32 (PcdNvStorageVariableBase) - FixedPcdGet64 (PcdFdBaseAddress)))

/**
  Test whether an open file is this platform's firmware image.

  Opening a file called armstub8-2712.bin is not enough to start writing to it:
  that name is the BCM2712 default, and the sibling u-boot image in this project
  uses it for an entirely different payload. Require the NV firmware volume
  header to sit at exactly the offset this build's FD layout puts it -- the same
  identity check VarBlockServiceDxe makes before it writes variables there.

  @param  File[in]  Open file to test.

  @retval EFI_SUCCESS    The file carries this firmware's layout.
  @retval EFI_NOT_FOUND  It does not.

**/
STATIC
EFI_STATUS
Rpi5FmpCheckFirmwareFile (
  IN EFI_FILE_PROTOCOL  *File
  )
{
  EFI_STATUS                  Status;
  EFI_FIRMWARE_VOLUME_HEADER  FwVolHeader;
  UINTN                       Size;

  Status = File->SetPosition (File, RPI5_FMP_UPDATABLE_SIZE);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Size   = sizeof (FwVolHeader);
  Status = File->Read (File, &Size, &FwVolHeader);
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
  Find the firmware file and open it.

  @param  OpenMode[in]  EFI_FILE_MODE_* bits to open with.
  @param  Root[out]     Receives the open volume root; caller closes it.
  @param  File[out]     Receives the open file; caller closes it.

  @retval EFI_SUCCESS    Found and opened.
  @retval EFI_NOT_FOUND  No writable volume carries this firmware.

**/
STATIC
EFI_STATUS
Rpi5FmpOpenFirmwareFile (
  IN  UINT64             OpenMode,
  OUT EFI_FILE_PROTOCOL  **Root,
  OUT EFI_FILE_PROTOCOL  **File
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_BLOCK_IO_PROTOCOL            *BlockIo;
  EFI_FILE_PROTOCOL                *Volume;
  EFI_FILE_PROTOCOL                *Candidate;
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            NameIndex;

  *Root = NULL;
  *File = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    //
    // A read-only volume cannot be the one we booted from in any useful sense,
    // and trying to open for write on one only produces a confusing error.
    //
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status) &&
        (!BlockIo->Media->MediaPresent ||
         (BlockIo->Media->ReadOnly && ((OpenMode & EFI_FILE_MODE_WRITE) != 0))))
    {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Volume = NULL;
    Status = Fs->OpenVolume (Fs, &Volume);
    if (EFI_ERROR (Status)) {
      continue;
    }

    for (NameIndex = 0; NameIndex < ARRAY_SIZE (mFirmwareFileNames); NameIndex++) {
      Candidate = NULL;
      Status    = Volume->Open (
                            Volume,
                            &Candidate,
                            mFirmwareFileNames[NameIndex],
                            OpenMode,
                            0
                            );
      if (EFI_ERROR (Status)) {
        continue;
      }

      if (EFI_ERROR (Rpi5FmpCheckFirmwareFile (Candidate))) {
        DEBUG ((
          DEBUG_INFO,
          "%a: '%s' is not this firmware's image, skipping\n",
          __func__,
          mFirmwareFileNames[NameIndex]
          ));
        Candidate->Close (Candidate);
        continue;
      }

      *Root = Volume;
      *File = Candidate;
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    Volume->Close (Volume);
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/**
  Test whether a capsule payload is a firmware image this platform can take.

  @param  Image[in]      Payload.
  @param  ImageSize[in]  Payload size.

  @retval EFI_SUCCESS            Usable.
  @retval EFI_INVALID_PARAMETER  Wrong size, or not built for this layout.

**/
STATIC
EFI_STATUS
Rpi5FmpValidatePayload (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize
  )
{
  CONST EFI_FIRMWARE_VOLUME_HEADER  *FwVolHeader;

  if ((Image == NULL) || (ImageSize < RPI5_FMP_UPDATABLE_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: payload is 0x%lx bytes, need at least 0x%lx\n",
      __func__,
      (UINT64)ImageSize,
      (UINT64)RPI5_FMP_UPDATABLE_SIZE
      ));
    return EFI_INVALID_PARAMETER;
  }

  //
  // A payload longer than the updatable region is a whole FD, tail included.
  // Accept it -- that is what the build produces and what an operator will have
  // to hand -- but only after confirming the tail really is the NV store, so a
  // payload built for a different flash layout cannot be written at an offset
  // that means something else here. The tail itself is never copied.
  //
  if (ImageSize > RPI5_FMP_UPDATABLE_SIZE) {
    if (ImageSize < RPI5_FMP_UPDATABLE_SIZE + sizeof (*FwVolHeader)) {
      return EFI_INVALID_PARAMETER;
    }

    FwVolHeader = (CONST EFI_FIRMWARE_VOLUME_HEADER *)
                  ((CONST UINT8 *)Image + RPI5_FMP_UPDATABLE_SIZE);

    if ((FwVolHeader->Signature != EFI_FVH_SIGNATURE) ||
        !CompareGuid (&FwVolHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid))
    {
      DEBUG ((
        DEBUG_ERROR,
        "%a: no NV store at 0x%lx -- payload is not an RPi5 firmware image\n",
        __func__,
        (UINT64)RPI5_FMP_UPDATABLE_SIZE
        ));
      return EFI_INVALID_PARAMETER;
    }
  }

  return EFI_SUCCESS;
}

/**
  The three hooks a UEFI-Driver-Model device would use to have its FMP instance
  installed onto a controller handle as it is bound and unbound.

  There is no controller here: the firmware image is a file, not a device this
  or any driver enumerates, so FmpDxe installs its protocol on its own image
  handle at entry and that is the whole lifecycle. EFI_UNSUPPORTED is how the
  library says exactly that -- FmpDxe reads it as "not driver-model managed" and
  proceeds, which is the intended path, not a degraded one.

**/
EFI_STATUS
EFIAPI
RegisterFmpInstaller (
  IN FMP_DEVICE_LIB_REGISTER_FMP_INSTALLER  FmpInstaller
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
RegisterFmpUninstaller (
  IN FMP_DEVICE_LIB_REGISTER_FMP_UNINSTALLER  FmpUninstaller
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FmpDeviceSetContext (
  IN EFI_HANDLE  Handle,
  IN OUT VOID    **Context
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FmpDeviceGetSize (
  OUT UINTN  *Size
  )
{
  if (Size == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The region a capsule may replace, not the size of the file: the NV store
  // past this point belongs to the board, not to the image.
  //
  *Size = RPI5_FMP_UPDATABLE_SIZE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceGetImageTypeIdGuidPtr (
  OUT EFI_GUID  **Guid
  )
{
  //
  // EFI_UNSUPPORTED tells FmpDxe to use PcdFmpDeviceImageTypeIdGuid, which the
  // platform sets. Keeping the GUID in one place stops the ESRT entry and the
  // capsule header from ever disagreeing about what this device is.
  //
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FmpDeviceGetAttributes (
  OUT UINT64  *Supported,
  OUT UINT64  *Setting
  )
{
  if ((Supported == NULL) || (Setting == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Supported = IMAGE_ATTRIBUTE_IMAGE_UPDATABLE |
               IMAGE_ATTRIBUTE_RESET_REQUIRED |
               IMAGE_ATTRIBUTE_IN_USE;

  *Setting = IMAGE_ATTRIBUTE_RESET_REQUIRED | IMAGE_ATTRIBUTE_IN_USE;

  //
  // Writing the image is a build-time choice, so a node can be made visible in
  // ESRT and Redfish without being remotely writable.
  //
  if (FeaturePcdGet (PcdRpi5FmpUpdateSupported)) {
    *Setting |= IMAGE_ATTRIBUTE_IMAGE_UPDATABLE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceGetLowestSupportedVersion (
  OUT UINT32  *LowestSupportedVersion
  )
{
  //
  // Deferred to PcdFmpDeviceBuildTimeLowestSupportedVersion: there is nowhere
  // on this platform to record a device-side floor, and inventing one in a
  // variable would let a downgrade that cleared it walk the floor backwards.
  //
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FmpDeviceGetVersionString (
  OUT CHAR16  **VersionString
  )
{
  CHAR16  *Version;

  if (VersionString == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The same string SMBIOS type 0 and the Redfish BiosVersion carry: the recipe
  // sets PcdFirmwareVersionString once, from RPI5_FW_VERSION.
  //
  Version = (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString);
  if ((Version == NULL) || (Version[0] == L'\0')) {
    return EFI_UNSUPPORTED;
  }

  *VersionString = AllocateCopyPool (StrSize (Version), Version);
  if (*VersionString == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceGetVersion (
  OUT UINT32  *Version
  )
{
  if (Version == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Version = FixedPcdGet32 (PcdRpi5FirmwareVersion);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceGetHardwareInstance (
  OUT UINT64  *HardwareInstance
  )
{
  //
  // One firmware image per board; no instance to distinguish.
  //
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
FmpDeviceGetImage (
  OUT VOID      *Image,
  IN OUT UINTN  *ImageSize
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS         Status;
  UINTN              ReadSize;

  if (ImageSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*ImageSize < RPI5_FMP_UPDATABLE_SIZE) || (Image == NULL)) {
    *ImageSize = RPI5_FMP_UPDATABLE_SIZE;
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = Rpi5FmpOpenFirmwareFile (EFI_FILE_MODE_READ, &Root, &File);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = File->SetPosition (File, 0);
  if (!EFI_ERROR (Status)) {
    ReadSize = RPI5_FMP_UPDATABLE_SIZE;
    Status   = File->Read (File, &ReadSize, Image);
    if (!EFI_ERROR (Status) && (ReadSize != RPI5_FMP_UPDATABLE_SIZE)) {
      Status = EFI_DEVICE_ERROR;
    }
  }

  File->Close (File);
  Root->Close (Root);

  if (!EFI_ERROR (Status)) {
    *ImageSize = RPI5_FMP_UPDATABLE_SIZE;
  }

  return Status;
}

EFI_STATUS
EFIAPI
FmpDeviceCheckImageWithStatus (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize,
  OUT UINT32     *ImageUpdatable,
  OUT UINT32     *LastAttemptStatus
  )
{
  EFI_STATUS  Status;

  if ((ImageUpdatable == NULL) || (LastAttemptStatus == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *LastAttemptStatus = LAST_ATTEMPT_STATUS_SUCCESS;

  if (!FeaturePcdGet (PcdRpi5FmpUpdateSupported)) {
    *ImageUpdatable    = IMAGE_UPDATABLE_INVALID;
    *LastAttemptStatus = LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL;
    return EFI_SUCCESS;
  }

  Status = Rpi5FmpValidatePayload (Image, ImageSize);
  if (EFI_ERROR (Status)) {
    *ImageUpdatable    = IMAGE_UPDATABLE_INVALID;
    *LastAttemptStatus = LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT;
    return EFI_SUCCESS;
  }

  *ImageUpdatable = IMAGE_UPDATABLE_VALID;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceCheckImage (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize,
  OUT UINT32     *ImageUpdatable
  )
{
  UINT32  LastAttemptStatus;

  return FmpDeviceCheckImageWithStatus (Image, ImageSize, ImageUpdatable, &LastAttemptStatus);
}

EFI_STATUS
EFIAPI
FmpDeviceSetImageWithStatus (
  IN CONST VOID                                     *Image,
  IN UINTN                                          ImageSize,
  IN CONST VOID                                     *VendorCode        OPTIONAL,
  IN EFI_FIRMWARE_MANAGEMENT_UPDATE_IMAGE_PROGRESS  Progress           OPTIONAL,
  IN UINT32                                         CapsuleFwVersion,
  OUT CHAR16                                        **AbortReason,
  OUT UINT32                                        *LastAttemptStatus
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS         Status;
  UINTN              WriteSize;

  if (LastAttemptStatus == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *LastAttemptStatus = LAST_ATTEMPT_STATUS_ERROR_UNSUCCESSFUL;

  if (!FeaturePcdGet (PcdRpi5FmpUpdateSupported)) {
    return EFI_WRITE_PROTECTED;
  }

  Status = Rpi5FmpValidatePayload (Image, ImageSize);
  if (EFI_ERROR (Status)) {
    *LastAttemptStatus = LAST_ATTEMPT_STATUS_ERROR_INVALID_FORMAT;
    return Status;
  }

  Status = Rpi5FmpOpenFirmwareFile (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, &Root, &File);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no writable firmware image found\n", __func__));
    return EFI_NOT_FOUND;
  }

  if (Progress != NULL) {
    Progress (5);
  }

  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  //
  // One write of the firmware region, and nothing past it. Everything from
  // PcdNvStorageVariableBase on -- the variable store, the event log, both
  // fault-tolerant-write blocks -- stays exactly as found, which is what keeps
  // boot entries, enrolled keys and BIOS settings across a firmware update.
  //
  WriteSize = RPI5_FMP_UPDATABLE_SIZE;
  Status    = File->Write (File, &WriteSize, (VOID *)Image);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (WriteSize != RPI5_FMP_UPDATABLE_SIZE) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  //
  // Flush before returning: FmpDxe's caller resets the board shortly after, and
  // a write still sitting in the FAT driver's cache is a bricked card.
  //
  Status = File->Flush (File);

Done:
  File->Close (File);
  Root->Close (Root);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: write failed: %r\n", __func__, Status));
    return Status;
  }

  if (Progress != NULL) {
    Progress (100);
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: wrote 0x%lx bytes of firmware, NV store preserved\n",
    __func__,
    (UINT64)RPI5_FMP_UPDATABLE_SIZE
    ));

  *LastAttemptStatus = LAST_ATTEMPT_STATUS_SUCCESS;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FmpDeviceSetImage (
  IN CONST VOID                                     *Image,
  IN UINTN                                          ImageSize,
  IN CONST VOID                                     *VendorCode        OPTIONAL,
  IN EFI_FIRMWARE_MANAGEMENT_UPDATE_IMAGE_PROGRESS  Progress           OPTIONAL,
  IN UINT32                                         CapsuleFwVersion,
  OUT CHAR16                                        **AbortReason
  )
{
  UINT32  LastAttemptStatus;

  return FmpDeviceSetImageWithStatus (
           Image,
           ImageSize,
           VendorCode,
           Progress,
           CapsuleFwVersion,
           AbortReason,
           &LastAttemptStatus
           );
}

EFI_STATUS
EFIAPI
FmpDeviceLock (
  VOID
  )
{
  //
  // Nothing to latch: the image is a file on removable media, so any lock here
  // would be a claim this library cannot keep.
  //
  return EFI_UNSUPPORTED;
}

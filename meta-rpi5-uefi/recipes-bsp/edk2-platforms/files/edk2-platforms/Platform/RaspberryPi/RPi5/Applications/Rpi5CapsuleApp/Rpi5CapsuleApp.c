/** @file

  Self-applying firmware update: scan the volume this application booted
  from for staged UEFI FMP capsules and apply them.

  Ships as \EFI\BOOT\BOOTAA64.EFI on the capsule volume, next to the
  \EFI\UpdateCapsule\ drop box (UEFI 2.10 8.5.5) that nanokvm-app stages
  capsules into. Booting the volume -- through the BMC's boot override,
  the firmware's boot menu, or plain removable-media fallback -- IS the
  update. The firmware also carries RpiCapsuleOnDiskLib (linked into
  BdsDxe), which applies staged capsules at ReadyToBoot on ordinary
  boots; this application remains the fallback for boots where the
  scanner cannot reach the firmware volume, and for firmware built
  before the scanner existed.

  The application context is also the safest this platform has for the
  in-place FD rewrite: ReadyToBoot has fired before any boot option
  launches, and the Redfish quiesce hangs off it, so no BMC exchange can
  stage a BootNext-and-cold-reset while the write is in flight.

  A flagless capsule is applied synchronously inside gRT->UpdateCapsule()
  (DxeCapsuleLibFmp -> FmpDxe), under FmpDxe's PKCS#7 authentication and
  lowest-supported-version gates -- a capsule signed with the wrong key is
  rejected in the call, whatever volume offered it. An applied capsule is
  deleted from the drop box, which is how the BMC tells applied from
  pending; a failed one is left there, and the next boot's firmware
  inventory report carries the LastAttemptStatus that says why.

  "Applied" on this console is a proven claim, not an echo of
  UpdateCapsule()'s status. Before applying, the capsule's FMP payload
  version is compared against the running firmware's: an equal version is
  skipped outright (FmpDxe compares an incoming image only against the
  lowest supported version, never the running one, so a stale capsule
  volume would otherwise "apply" the same bytes forever and read as a
  broken update path); an older one applies with a DOWNGRADING banner.
  After applying, the firmware file is re-located read-only and the
  firmware region is read back off the media and byte-compared against
  the capsule payload -- only a verified write counts, is deleted, and
  earns the reset.

  One escape hatch: firmware built before the FMP lock event was
  disarmed (RPi5.dsc, PcdFmpDeviceLockEventGuid) refuses every
  SetImage from EndOfDxe on, and UpdateCapsule() swallows the refusal
  into success. That state is provable -- the apply "succeeds", the
  media does not change, and the FMP last-attempt record stays empty --
  and when proven, this application writes the firmware region to the
  armstub directly, with Rpi5FmpDeviceLib's exact discipline (region
  only, variable-store tail untouched), then re-verifies. The PKCS#7
  signature is NOT checked on that path -- the structure, platform
  identity and version gate still are -- and it says so on the console.
  One reboot later the unlocked FmpDxe makes every future update flow
  through the real, authenticated path.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Guid/FileInfo.h>
#include <Guid/FmpCapsule.h>
#include <Guid/SystemNvDataGuid.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FileHandleLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/BootManagerPolicy.h>
#include <Protocol/FirmwareManagement.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#define CAPSULE_DIR_NAME  L"\\EFI\\UpdateCapsule"

//
// Bound on one capsule file. Matches the BMC's HttpPushUri cap; the whole
// capsule volume is smaller than this in practice.
//
#define MAX_CAPSULE_BYTES  SIZE_128MB

//
// Bound on drop-box entries processed in one boot. The BMC stages one or
// two; the bound only guards the name array.
//
#define MAX_CAPSULES  8

//
// How long the summary stays readable on a console before the reset that
// boots the freshly written firmware.
//
#define SUMMARY_STALL_US  (5 * 1000 * 1000)

//
// FMP_PAYLOAD_HEADER, as GenerateCapsule emits it ahead of the firmware
// image. Upstream keeps this struct private to FmpDevicePkg; the layout is
// stable and pinned by the "MSS1" signature.
//
#pragma pack(1)
typedef struct {
  UINT32    Signature;
  UINT32    HeaderSize;
  UINT32    FwVersion;
  UINT32    LowestSupportedVersion;
} RPI5_FMP_PAYLOAD_HEADER;
#pragma pack()

#define RPI5_FMP_PAYLOAD_SIGNATURE  SIGNATURE_32 ('M', 'S', 'S', '1')

//
// How deep into a capsule the payload header search goes. The real offset
// is a few KB of capsule/FMP/PKCS7 headers; anything past this is not a
// capsule this platform built.
//
#define RPI5_FMP_HEADER_SEARCH_LIMIT  SIZE_64KB

//
// Byte range of the firmware file a capsule replaces -- everything before
// the NV store. The same two PCDs Rpi5FmpDeviceLib subtracts, so the
// readback verify measures exactly what the writer wrote.
//
#define RPI5_FMP_UPDATABLE_SIZE \
  ((UINTN)(FixedPcdGet32 (PcdNvStorageVariableBase) - FixedPcdGet64 (PcdFdBaseAddress)))

//
// The firmware file's two deployment names, kept in step with
// Rpi5FmpDeviceLib and VarBlockServiceDxe.
//
STATIC CHAR16 *CONST  mFirmwareFileNames[] = {
  L"armstub8-2712.bin",
  L"RPI_EFI.FD"
};

//
// The build's config.txt rides the capsule volume as a sidecar in the
// drop box (staged by rpi5-capsule-image.bb). It is not a capsule --
// the collectors skip it by name -- and it is firmware-owned state,
// coupled to the FD layout through device_tree_address: once the
// firmware on the media is proven to match the capsule build, the
// sidecar is converged onto every volume carrying the firmware file.
// User overrides belong in uefi-cfg.txt, which config.txt includes
// last and which no update path ever touches.
//
#define SIDECAR_CONFIG_NAME  L"config.txt"

//
// Set only at the points that prove the firmware on the media is the
// capsule's build: a verified apply, or a capsule already running.
// A blind (unverifiable) apply does not count -- pairing a config.txt
// with firmware it was not built against is how device_tree_address
// breaks.
//
STATIC BOOLEAN  mMediaFirmwareCurrent = FALSE;

/**
  Locate the FMP payload header and image-type GUID inside a capsule.

  @param[in]  Capsule      The capsule, fully read into memory.
  @param[in]  CapsuleSize  Its size.
  @param[out] TypeId       Receives UpdateImageTypeId when parseable, else
                           left untouched.
  @param[out] FwVersion    Receives the payload's declared version.
  @param[out] Payload      Receives a pointer to the firmware image bytes.
  @param[out] PayloadSize  Receives the byte count from there to the end of
                           the capsule image.

  @retval TRUE   Parsed; outputs are valid.
  @retval FALSE  Not an FMP capsule this platform built.
**/
STATIC
BOOLEAN
ParseFmpCapsule (
  IN  CONST EFI_CAPSULE_HEADER  *Capsule,
  IN  UINTN                     CapsuleSize,
  OUT EFI_GUID                  *TypeId,
  OUT UINT32                    *FwVersion,
  OUT CONST UINT8               **Payload,
  OUT UINTN                     *PayloadSize
  )
{
  CONST UINT8                                         *Bytes;
  CONST EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER        *FmpHeader;
  CONST UINT64                                        *ItemOffsets;
  CONST EFI_FIRMWARE_MANAGEMENT_CAPSULE_IMAGE_HEADER  *ImageHeader;
  CONST RPI5_FMP_PAYLOAD_HEADER                       *PayloadHeader;
  UINTN                                               Offset;
  UINTN                                               Limit;

  if (!CompareGuid (&Capsule->CapsuleGuid, &gEfiFmpCapsuleGuid)) {
    return FALSE;
  }

  Bytes = (CONST UINT8 *)Capsule;

  //
  // The image-type GUID, through the proper structures: capsule header ->
  // FMP capsule header -> first payload item's image header. Offsets past
  // the image header are version-dependent, so the payload itself is found
  // by its signature instead.
  //
  if ((UINTN)Capsule->HeaderSize + sizeof (*FmpHeader) <= CapsuleSize) {
    FmpHeader   = (CONST VOID *)(Bytes + Capsule->HeaderSize);
    ItemOffsets = (CONST UINT64 *)(FmpHeader + 1);
    if ((FmpHeader->PayloadItemCount > 0) &&
        ((UINTN)Capsule->HeaderSize + sizeof (*FmpHeader) +
         ((UINTN)FmpHeader->EmbeddedDriverCount + FmpHeader->PayloadItemCount) * sizeof (UINT64) <= CapsuleSize))
    {
      Offset = (UINTN)ItemOffsets[FmpHeader->EmbeddedDriverCount];
      if ((UINTN)Capsule->HeaderSize + Offset + sizeof (*ImageHeader) <= CapsuleSize) {
        ImageHeader = (CONST VOID *)(Bytes + Capsule->HeaderSize + Offset);
        CopyGuid (TypeId, &ImageHeader->UpdateImageTypeId);
      }
    }
  }

  Limit = MIN (CapsuleSize, RPI5_FMP_HEADER_SEARCH_LIMIT);
  for (Offset = 0; Offset + sizeof (*PayloadHeader) <= Limit; Offset++) {
    PayloadHeader = (CONST VOID *)(Bytes + Offset);
    if ((PayloadHeader->Signature == RPI5_FMP_PAYLOAD_SIGNATURE) &&
        (PayloadHeader->HeaderSize >= sizeof (*PayloadHeader)) &&
        (PayloadHeader->HeaderSize < SIZE_4KB) &&
        (Offset + PayloadHeader->HeaderSize < CapsuleSize))
    {
      *FwVersion   = PayloadHeader->FwVersion;
      *Payload     = Bytes + Offset + PayloadHeader->HeaderSize;
      *PayloadSize = CapsuleSize - (Offset + PayloadHeader->HeaderSize);
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Decode a LastAttemptStatus into the UEFI spec's name for it.

  @param[in] LastAttemptStatus  The value FmpDxe recorded.

  @return A static string; never NULL.
**/
STATIC
CONST CHAR16 *
LastAttemptStatusName (
  IN UINT32  LastAttemptStatus
  )
{
  switch (LastAttemptStatus) {
    case 0: return L"success";
    case 1: return L"unsuccessful";
    case 2: return L"insufficient resources";
    case 3: return L"incorrect version";
    case 4: return L"invalid image format";
    case 5: return L"authentication error (capsule not signed with the certificate this firmware trusts)";
    case 6: return L"AC power not connected";
    case 7: return L"insufficient battery";
    default: return L"vendor/device specific";
  }
}

/**
  Read the running firmware's version and last-attempt record from the
  platform FMP.

  FmpDxe updates LastAttemptStatus/LastAttemptVersion synchronously while
  SetTheImage runs, so calling this right after a failed apply names the
  real reason -- which UpdateCapsule() itself never surfaces: upstream
  ProcessFmpCapsuleImage returns success for any processed capsule and
  only records the SetImage status.

  @param[in]  TypeId       Image type to match; a zero GUID matches the
                           first descriptor found.
  @param[out] Version      Receives the running version.
  @param[out] LastStatus   Receives LastAttemptStatus; optional.
  @param[out] LastVersion  Receives LastAttemptVersion; optional.

  @retval TRUE   Found.
  @retval FALSE  No matching FMP descriptor (firmware predating FMP).
**/
STATIC
BOOLEAN
GetRunningFmpVersion (
  IN  CONST EFI_GUID  *TypeId,
  OUT UINT32          *Version,
  OUT UINT32          *LastStatus   OPTIONAL,
  OUT UINT32          *LastVersion  OPTIONAL
  )
{
  EFI_STATUS                        Status;
  EFI_HANDLE                        *Handles;
  UINTN                             HandleCount;
  UINTN                             Index;
  EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *Fmp;
  EFI_FIRMWARE_IMAGE_DESCRIPTOR     *Info;
  UINTN                             InfoSize;
  UINT32                            Ver;
  UINT32                            PackageVer;
  CHAR16                            *PackageVerName;
  UINT8                             Count;
  UINTN                             DescSize;
  EFI_GUID                          ZeroGuid;
  BOOLEAN                           Found;

  ZeroMem (&ZeroGuid, sizeof (ZeroGuid));
  Found  = FALSE;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareManagementProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  for (Index = 0; (Index < HandleCount) && !Found; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareManagementProtocolGuid,
                    (VOID **)&Fmp
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    InfoSize = 0;
    Status   = Fmp->GetImageInfo (Fmp, &InfoSize, NULL, NULL, NULL, NULL, NULL, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      continue;
    }

    Info = AllocateZeroPool (InfoSize);
    if (Info == NULL) {
      continue;
    }

    PackageVerName = NULL;
    Status         = Fmp->GetImageInfo (
                            Fmp,
                            &InfoSize,
                            Info,
                            &Ver,
                            &Count,
                            &DescSize,
                            &PackageVer,
                            &PackageVerName
                            );
    if (!EFI_ERROR (Status) && (Count > 0)) {
      if (CompareGuid (TypeId, &ZeroGuid) ||
          CompareGuid (TypeId, &Info->ImageTypeId))
      {
        *Version = Info->Version;
        if ((LastStatus != NULL) && (LastVersion != NULL)) {
          *LastStatus  = 0;
          *LastVersion = 0;
          if (Ver >= 3) {
            *LastStatus  = Info->LastAttemptStatus;
            *LastVersion = Info->LastAttemptVersion;
          }
        }

        Found = TRUE;
      }
    }

    if (PackageVerName != NULL) {
      FreePool (PackageVerName);
    }

    FreePool (Info);
  }

  FreePool (Handles);
  return Found;
}

/**
  Is this open file this platform's firmware image?

  The same identity test Rpi5FmpDeviceLib and VarBlockServiceDxe make: the
  NV firmware volume header must sit at exactly the offset this build's FD
  layout puts it. A u-boot armstub, or any other impostor under the
  firmware's filename, fails it.

  @param[in] File  Open file to test.

  @retval TRUE   It is.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
FileIsThisFirmware (
  IN EFI_FILE_HANDLE  File
  )
{
  EFI_STATUS                  Status;
  EFI_FIRMWARE_VOLUME_HEADER  Header;
  UINTN                       Size;

  Status = File->SetPosition (File, RPI5_FMP_UPDATABLE_SIZE);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Size   = sizeof (Header);
  Status = File->Read (File, &Size, &Header);

  return !EFI_ERROR (Status) &&
         (Size == sizeof (Header)) &&
         (Header.Signature == EFI_FVH_SIGNATURE) &&
         CompareGuid (&Header.FileSystemGuid, &gEfiSystemNvDataFvGuid);
}

/**
  Read the firmware region back off the media and compare it against what
  the capsule carried.

  Walks every SimpleFileSystem volume for the firmware file under either
  deployment name, identity-checked, and byte-compares the updatable
  region. More than one genuine firmware file is reported as such: on a
  multi-FD system a match on one volume says nothing about the one the VPU
  boots.

  @param[in] Payload      The capsule's firmware image bytes.
  @param[in] PayloadSize  Its size; must cover the updatable region.

  @retval TRUE   At least one firmware file matches, and none mismatch.
  @retval FALSE  No firmware file found, or any copy differs.
**/
STATIC
BOOLEAN
VerifyFirmwareOnDisk (
  IN CONST UINT8  *Payload,
  IN UINTN        PayloadSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            NameIndex;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_HANDLE                  Root;
  EFI_FILE_HANDLE                  File;
  UINT8                            *OnDisk;
  UINTN                            ReadSize;
  UINTN                            Matched;
  UINTN                            Mismatched;

  if (PayloadSize < RPI5_FMP_UPDATABLE_SIZE) {
    Print (L"Rpi5CapsuleApp: payload too small to verify (%u bytes)\n", (UINT32)PayloadSize);
    return FALSE;
  }

  OnDisk = AllocatePool (RPI5_FMP_UPDATABLE_SIZE);
  if (OnDisk == NULL) {
    Print (L"Rpi5CapsuleApp: out of memory for readback verify\n");
    return FALSE;
  }

  Matched    = 0;
  Mismatched = 0;
  Status     = gBS->LocateHandleBuffer (
                      ByProtocol,
                      &gEfiSimpleFileSystemProtocolGuid,
                      NULL,
                      &HandleCount,
                      &Handles
                      );
  if (EFI_ERROR (Status)) {
    FreePool (OnDisk);
    return FALSE;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    for (NameIndex = 0; NameIndex < ARRAY_SIZE (mFirmwareFileNames); NameIndex++) {
      if (EFI_ERROR (Root->Open (Root, &File, mFirmwareFileNames[NameIndex], EFI_FILE_MODE_READ, 0))) {
        continue;
      }

      if (FileIsThisFirmware (File)) {
        ReadSize = RPI5_FMP_UPDATABLE_SIZE;
        if (!EFI_ERROR (File->SetPosition (File, 0)) &&
            !EFI_ERROR (File->Read (File, &ReadSize, OnDisk)) &&
            (ReadSize == RPI5_FMP_UPDATABLE_SIZE) &&
            (CompareMem (OnDisk, Payload, RPI5_FMP_UPDATABLE_SIZE) == 0))
        {
          Matched++;
          Print (L"Rpi5CapsuleApp:   %s on volume %u matches the capsule payload\n", mFirmwareFileNames[NameIndex], (UINT32)Index);
        } else {
          Mismatched++;
          Print (L"Rpi5CapsuleApp:   %s on volume %u DIFFERS from the capsule payload\n", mFirmwareFileNames[NameIndex], (UINT32)Index);
        }
      }

      File->Close (File);
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  FreePool (OnDisk);

  if ((Matched + Mismatched) > 1) {
    Print (L"Rpi5CapsuleApp: warning: %u firmware files exist across volumes; the VPU boots only one of them\n", (UINT32)(Matched + Mismatched));
  }

  return (Matched > 0) && (Mismatched == 0);
}

/**
  Write the capsule's firmware region directly into the firmware file.

  The legacy-bootstrap path: bypasses FmpDxe entirely, for running
  firmware whose FMP is lock-refused. Same discipline as
  Rpi5FmpDeviceLib -- the first identity-passing writable candidate gets
  exactly the updatable region, the variable-store tail is left as
  found, and the write is flushed before success is claimed.

  @param[in] Payload      The capsule's firmware image bytes.
  @param[in] PayloadSize  Its size; must cover the updatable region.

  @retval TRUE   Written and flushed.
  @retval FALSE  No writable firmware file, or the write failed.
**/
STATIC
BOOLEAN
DirectWriteFirmware (
  IN CONST UINT8  *Payload,
  IN UINTN        PayloadSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            NameIndex;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_HANDLE                  Root;
  EFI_FILE_HANDLE                  File;
  UINTN                            WriteSize;
  BOOLEAN                          Written;

  if (PayloadSize < RPI5_FMP_UPDATABLE_SIZE) {
    return FALSE;
  }

  Written = FALSE;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  for (Index = 0; (Index < HandleCount) && !Written; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    for (NameIndex = 0; (NameIndex < ARRAY_SIZE (mFirmwareFileNames)) && !Written; NameIndex++) {
      if (EFI_ERROR (
            Root->Open (
                    Root,
                    &File,
                    mFirmwareFileNames[NameIndex],
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    )
            ))
      {
        continue;
      }

      if (FileIsThisFirmware (File)) {
        WriteSize = RPI5_FMP_UPDATABLE_SIZE;
        if (!EFI_ERROR (File->SetPosition (File, 0)) &&
            !EFI_ERROR (File->Write (File, &WriteSize, (VOID *)Payload)) &&
            (WriteSize == RPI5_FMP_UPDATABLE_SIZE) &&
            !EFI_ERROR (File->Flush (File)))
        {
          Written = TRUE;
          Print (
            L"Rpi5CapsuleApp:   wrote %u bytes to %s on volume %u, tail preserved\n",
            (UINT32)RPI5_FMP_UPDATABLE_SIZE,
            mFirmwareFileNames[NameIndex],
            (UINT32)Index
            );
        } else {
          Print (
            L"Rpi5CapsuleApp:   writing %s on volume %u failed partway\n",
            mFirmwareFileNames[NameIndex],
            (UINT32)Index
            );
        }
      }

      File->Close (File);
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Written;
}

/**
  Connect every device so the firmware file's volume exists.

  This application boots from the capsule volume, but the file the capsule
  REWRITES lives on the boot medium's FAT partition (SD or NVMe), and
  Rpi5FmpDeviceLib only searches volumes that are already connected. A
  boot-menu launch happens to connect everything; a BootNext launch -- the
  BMC's "Usb" boot override -- connects only this volume's own device path
  under BDP_CONNECT_MINIMAL, and without this call the apply would fail
  with the firmware file nowhere to be found. Same ConnectDeviceClass
  plumbing RpiRedfishSyncDxe uses; in a one-shot updater the cost of
  connect-all is irrelevant.

**/
STATIC
VOID
ConnectFirmwareTargetVolumes (
  VOID
  )
{
  EFI_STATUS                        Status;
  EFI_BOOT_MANAGER_POLICY_PROTOCOL  *Policy;

  Status = gBS->LocateProtocol (
                  &gEfiBootManagerPolicyProtocolGuid,
                  NULL,
                  (VOID **)&Policy
                  );
  if (!EFI_ERROR (Status)) {
    Status = Policy->ConnectDeviceClass (
                       Policy,
                       (EFI_GUID *)&gEfiBootManagerPolicyConnectAllGuid
                       );
  }

  if (EFI_ERROR (Status)) {
    //
    // Not fatal by itself: on a boot path that already connected storage
    // the apply still works. Say so, in case a later failure needs the
    // hint.
    //
    Print (L"Rpi5CapsuleApp: warning: connect-all failed (%r); the firmware volume may be absent\n", Status);
  }
}

/**
  Print the FMP's freshly recorded last-attempt status for an image type.

  @param[in] TypeId  The image type the apply targeted.
**/
STATIC
VOID
ReportLastAttempt (
  IN CONST EFI_GUID  *TypeId
  )
{
  UINT32  Version;
  UINT32  LastStatus;
  UINT32  LastVersion;

  if (GetRunningFmpVersion (TypeId, &Version, &LastStatus, &LastVersion)) {
    Print (
      L"Rpi5CapsuleApp:   FMP recorded: LastAttemptVersion %u, LastAttemptStatus %u (%s)\n",
      LastVersion,
      LastStatus,
      LastAttemptStatusName (LastStatus)
      );

    if ((LastVersion == 0) && (LastStatus == 0)) {
      //
      // Not a failure record -- the absence of one. On this platform that
      // means the running FmpDxe refused before doing any work, which is
      // the EndOfDxe device lock in firmware built before the lock event
      // was disarmed (RPi5.dsc, PcdFmpDeviceLockEventGuid). No capsule can
      // get past it, because the refusal comes from the firmware doing the
      // applying.
      //
      Print (L"Rpi5CapsuleApp:   an empty record means the running firmware's FMP refused before starting --\n");
      Print (L"Rpi5CapsuleApp:   its FmpDxe locks at EndOfDxe (fixed in builds after 1788173827). A capsule\n");
      Print (L"Rpi5CapsuleApp:   cannot update this firmware; flash the boot medium directly once.\n");
    }
  } else {
    Print (L"Rpi5CapsuleApp:   no FMP descriptor to read a last-attempt record from\n");
  }
}

/**
  Apply one capsule file from the drop box.

  @param[in] Dir   Open handle on \EFI\UpdateCapsule.
  @param[in] Name  File name within Dir.

  @retval TRUE   The capsule was applied (and the file deleted).
  @retval FALSE  It was not; the file is left in place.
**/
STATIC
BOOLEAN
ApplyOneCapsule (
  IN EFI_FILE_HANDLE  Dir,
  IN CONST CHAR16     *Name
  )
{
  EFI_STATUS          Status;
  EFI_FILE_HANDLE     File;
  BOOLEAN             CanDelete;
  UINT64              FileSize;
  UINTN               ReadSize;
  EFI_CAPSULE_HEADER  *Capsule;
  EFI_CAPSULE_HEADER  *HeaderArray[1];
  EFI_GUID            TypeId;
  UINT32              CapsuleVersion;
  UINT32              RunningVersion;
  BOOLEAN             HaveCapsuleVersion;
  BOOLEAN             HaveRunningVersion;
  BOOLEAN             LockedLegacy;
  UINT32              BsLastStatus;
  UINT32              BsLastVersion;
  CONST UINT8         *Payload;
  UINTN               PayloadSize;

  //
  // Read-write so a successful apply can delete the file. A physically
  // write-protected stick still gets its capsule applied; it just cannot
  // be marked consumed.
  //
  CanDelete = TRUE;
  Status    = Dir->Open (
                     Dir,
                     &File,
                     (CHAR16 *)Name,
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                     0
                     );
  if (EFI_ERROR (Status)) {
    CanDelete = FALSE;
    Status    = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  }

  if (EFI_ERROR (Status)) {
    Print (L"Rpi5CapsuleApp: cannot open %s: %r\n", Name, Status);
    return FALSE;
  }

  Status = FileHandleGetSize (File, &FileSize);
  if (EFI_ERROR (Status) ||
      (FileSize < sizeof (EFI_CAPSULE_HEADER)) ||
      (FileSize > MAX_CAPSULE_BYTES))
  {
    Print (L"Rpi5CapsuleApp: %s has no plausible capsule size (%Lu bytes)\n", Name, FileSize);
    FileHandleClose (File);
    return FALSE;
  }

  Capsule = AllocatePool ((UINTN)FileSize);
  if (Capsule == NULL) {
    Print (L"Rpi5CapsuleApp: out of memory for %s (%Lu bytes)\n", Name, FileSize);
    FileHandleClose (File);
    return FALSE;
  }

  ReadSize = (UINTN)FileSize;
  Status   = FileHandleRead (File, &ReadSize, Capsule);
  if (EFI_ERROR (Status) || (ReadSize != (UINTN)FileSize)) {
    Print (L"Rpi5CapsuleApp: reading %s failed: %r\n", Name, Status);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  if ((Capsule->HeaderSize < sizeof (EFI_CAPSULE_HEADER)) ||
      (Capsule->CapsuleImageSize > FileSize) ||
      (Capsule->CapsuleImageSize < Capsule->HeaderSize))
  {
    Print (L"Rpi5CapsuleApp: %s carries an inconsistent capsule header\n", Name);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  if ((Capsule->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET) != 0) {
    //
    // This platform deliberately builds none of the reset-persistence
    // machinery (see Rpi5FmpDeviceLib's README); such a capsule would be
    // refused by CapsuleRuntimeDxe with a status that reads like a bug.
    // Name the real problem instead.
    //
    Print (
      L"Rpi5CapsuleApp: %s requests PersistAcrossReset, which this platform does not support; rebuild the capsule flagless\n",
      Name
      );
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  //
  // Version gate. FmpDxe compares an incoming image only against the
  // lowest supported version, never the running one, so without this a
  // stale capsule volume would "apply" identical bytes forever and read
  // as a broken update path.
  //
  ZeroMem (&TypeId, sizeof (TypeId));
  HaveCapsuleVersion = ParseFmpCapsule (
                         Capsule,
                         (UINTN)FileSize,
                         &TypeId,
                         &CapsuleVersion,
                         &Payload,
                         &PayloadSize
                         );
  HaveRunningVersion = GetRunningFmpVersion (&TypeId, &RunningVersion, NULL, NULL);

  if (HaveCapsuleVersion && HaveRunningVersion) {
    Print (
      L"Rpi5CapsuleApp: %s carries version %u; running firmware is %u\n",
      Name,
      CapsuleVersion,
      RunningVersion
      );

    if (CapsuleVersion == RunningVersion) {
      Print (L"Rpi5CapsuleApp: %s is the running version already, skipping\n", Name);
      mMediaFirmwareCurrent = TRUE;
      if (CanDelete) {
        //
        // Consumed in every sense that matters: delete it so the BMC sees
        // it handled and later boots stop re-reading it.
        //
        if (EFI_ERROR (FileHandleDelete (File))) {
          FileHandleClose (File);
        }
      } else {
        FileHandleClose (File);
      }

      FreePool (Capsule);
      return FALSE;
    }

    if (CapsuleVersion < RunningVersion) {
      Print (L"Rpi5CapsuleApp: *** DOWNGRADING from %u to %u ***\n", RunningVersion, CapsuleVersion);
    }
  } else {
    Print (
      L"Rpi5CapsuleApp: warning: cannot compare versions (capsule %a, running %a); applying blind\n",
      HaveCapsuleVersion ? "parsed" : "unparsed",
      HaveRunningVersion ? "known" : "unknown"
      );
  }

  Print (
    L"Rpi5CapsuleApp: applying %s (%u bytes, %g)...\n",
    Name,
    Capsule->CapsuleImageSize,
    &Capsule->CapsuleGuid
    );

  //
  // Flagless capsule: applied inside the call, under boot services --
  // authentication, version gates, progress display and the FD write all
  // happen before this returns.
  //
  HeaderArray[0] = Capsule;
  Status         = gRT->UpdateCapsule (HeaderArray, 1, 0);

  if (EFI_ERROR (Status)) {
    Print (L"Rpi5CapsuleApp: %s NOT applied: %r\n", Name, Status);
    ReportLastAttempt (&TypeId);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  //
  // UpdateCapsule() returning success is a claim; the readback is the
  // proof. Anything from a wrong write target to a lost write shows up
  // here as a mismatch, and a mismatch is a failure: the capsule stays,
  // and no reset happens on its account.
  //
  if (HaveCapsuleVersion) {
    if (!VerifyFirmwareOnDisk (Payload, PayloadSize)) {
      Print (L"Rpi5CapsuleApp: %s MISMATCH: the firmware file still carries the OLD bytes -- the write never happened\n", Name);

      //
      // UpdateCapsule() said success, the media says otherwise -- upstream
      // working as built: ProcessFmpCapsuleImage returns success for any
      // processed capsule and only records the SetImage status. Read the
      // record: an EMPTY one (no version, no status) is the signature of
      // pre-fix firmware whose FmpDxe lock-refused before doing anything,
      // and that exact state has an escape hatch -- the lock only guards
      // FmpDxe, never the file.
      //
      LockedLegacy = FALSE;
      if (GetRunningFmpVersion (&TypeId, &RunningVersion, &BsLastStatus, &BsLastVersion)) {
        Print (
          L"Rpi5CapsuleApp:   FMP recorded: LastAttemptVersion %u, LastAttemptStatus %u (%s)\n",
          BsLastVersion,
          BsLastStatus,
          LastAttemptStatusName (BsLastStatus)
          );
        LockedLegacy = (BOOLEAN)((BsLastVersion == 0) && (BsLastStatus == 0));
      }

      if (LockedLegacy) {
        Print (L"Rpi5CapsuleApp:   empty record = this firmware's FmpDxe lock-refused the update (fixed in builds after 1788173827)\n");
        Print (L"Rpi5CapsuleApp:   legacy bootstrap: writing the firmware region directly -- signature NOT verified by this path\n");

        if (DirectWriteFirmware (Payload, PayloadSize) &&
            VerifyFirmwareOnDisk (Payload, PayloadSize))
        {
          Print (L"Rpi5CapsuleApp: %s applied and VERIFIED on disk (legacy bootstrap)\n", Name);
          mMediaFirmwareCurrent = TRUE;
          FreePool (Capsule);

          if (CanDelete) {
            if (EFI_ERROR (FileHandleDelete (File))) {
              Print (L"Rpi5CapsuleApp: warning: could not delete %s\n", Name);
            }
          } else {
            Print (L"Rpi5CapsuleApp: warning: %s is on read-only media, left in place\n", Name);
            FileHandleClose (File);
          }

          return TRUE;
        }

        Print (L"Rpi5CapsuleApp:   legacy bootstrap failed; flash the boot medium directly once\n");
      }

      FreePool (Capsule);
      FileHandleClose (File);
      return FALSE;
    }

    Print (L"Rpi5CapsuleApp: %s applied and VERIFIED on disk\n", Name);
    mMediaFirmwareCurrent = TRUE;
  } else {
    Print (L"Rpi5CapsuleApp: %s applied (unverified: payload location unknown)\n", Name);
  }

  FreePool (Capsule);

  if (CanDelete) {
    //
    // Deleting the consumed capsule is the applied-vs-pending signal the
    // BMC reads back from the volume. FileHandleDelete releases the handle
    // whatever it returns.
    //
    Status = FileHandleDelete (File);
    if (EFI_ERROR (Status)) {
      Print (L"Rpi5CapsuleApp: warning: could not delete %s: %r\n", Name, Status);
    }
  } else {
    Print (L"Rpi5CapsuleApp: warning: %s is on read-only media, left in place\n", Name);
    FileHandleClose (File);
  }

  return TRUE;
}

/**
  Converge config.txt on every firmware-carrying volume to the sidecar
  copy shipped beside the capsule. Compare-first and quiet work when
  already current; loud on failure but never fatal to the update that
  already applied -- the next capsule event retries. The write is in
  place with the truncate last, so config.txt never transits through
  nonexistence and a mid-write power cut garbles a tail rather than
  losing the file.

  @param[in] Dir  Open handle on the capsule volume's drop box.
**/
STATIC
VOID
InstallSidecarConfig (
  IN EFI_FILE_HANDLE  Dir
  )
{
  EFI_STATUS       Status;
  EFI_FILE_HANDLE  File;
  UINT64           FileSize;
  UINTN            ReadSize;
  UINTN            WriteLen;
  UINT8            *Wanted;
  UINTN            WantedLen;
  EFI_HANDLE       *Handles;
  UINTN            HandleCount;
  UINTN            Index;
  UINTN            NameIndex;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_HANDLE  Root;
  EFI_FILE_HANDLE  Target;
  BOOLEAN          IsFirmwareVolume;
  UINT8            *Current;
  UINT64           CurrentSize;
  UINTN            CurrentRead;
  BOOLEAN          UpToDate;
  UINTN            Targets;
  BOOLEAN          AnyFailed;

  Status = Dir->Open (Dir, &File, SIDECAR_CONFIG_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    //
    // A capsule volume from before the sidecar existed; nothing to do.
    //
    return;
  }

  Wanted    = NULL;
  WantedLen = 0;
  Status    = FileHandleGetSize (File, &FileSize);
  if (!EFI_ERROR (Status) && (FileSize > 0) && (FileSize <= SIZE_64KB)) {
    Wanted = AllocatePool ((UINTN)FileSize);
  }

  if (Wanted != NULL) {
    ReadSize = (UINTN)FileSize;
    Status   = FileHandleRead (File, &ReadSize, Wanted);
    if (EFI_ERROR (Status) || (ReadSize != (UINTN)FileSize)) {
      FreePool (Wanted);
      Wanted = NULL;
    } else {
      WantedLen = ReadSize;
    }
  }

  FileHandleClose (File);
  if (Wanted == NULL) {
    Print (L"Rpi5CapsuleApp: warning: could not read the sidecar config.txt; boot config left alone\n");
    return;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Wanted);
    return;
  }

  Targets   = 0;
  AnyFailed = FALSE;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    IsFirmwareVolume = FALSE;
    for (NameIndex = 0; NameIndex < ARRAY_SIZE (mFirmwareFileNames); NameIndex++) {
      if (!EFI_ERROR (Root->Open (Root, &Target, (CHAR16 *)mFirmwareFileNames[NameIndex], EFI_FILE_MODE_READ, 0))) {
        FileHandleClose (Target);
        IsFirmwareVolume = TRUE;
        break;
      }
    }

    if (!IsFirmwareVolume) {
      Root->Close (Root);
      continue;
    }

    Targets++;

    //
    // Compare before writing: the steady state is a match and no write.
    //
    UpToDate = FALSE;
    if (!EFI_ERROR (Root->Open (Root, &Target, SIDECAR_CONFIG_NAME, EFI_FILE_MODE_READ, 0))) {
      if (!EFI_ERROR (FileHandleGetSize (Target, &CurrentSize)) &&
          (CurrentSize == WantedLen))
      {
        Current = AllocatePool (WantedLen);
        if (Current != NULL) {
          CurrentRead = WantedLen;
          if (!EFI_ERROR (FileHandleRead (Target, &CurrentRead, Current)) &&
              (CurrentRead == WantedLen) &&
              (CompareMem (Current, Wanted, WantedLen) == 0))
          {
            UpToDate = TRUE;
          }

          FreePool (Current);
        }
      }

      FileHandleClose (Target);
    }

    if (UpToDate) {
      Print (L"Rpi5CapsuleApp: config.txt on volume %u is current\n", (UINT32)Index);
      Root->Close (Root);
      continue;
    }

    Status = Root->Open (
                     Root,
                     &Target,
                     SIDECAR_CONFIG_NAME,
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                     0
                     );
    if (EFI_ERROR (Status)) {
      Status = Root->Open (
                       Root,
                       &Target,
                       SIDECAR_CONFIG_NAME,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                       0
                       );
    }

    if (EFI_ERROR (Status)) {
      Print (L"Rpi5CapsuleApp: warning: cannot open config.txt on volume %u: %r\n", (UINT32)Index, Status);
      AnyFailed = TRUE;
      Root->Close (Root);
      continue;
    }

    WriteLen = WantedLen;
    Status   = Target->Write (Target, &WriteLen, Wanted);
    if (!EFI_ERROR (Status) && (WriteLen == WantedLen)) {
      //
      // Truncate any stale tail only after the data is down; a leftover
      // tail would feed the VPU's last-assignment-wins parse stale lines.
      //
      Status = FileHandleSetSize (Target, WantedLen);
      if (!EFI_ERROR (Status)) {
        Target->Flush (Target);
        Print (L"Rpi5CapsuleApp: config.txt updated on volume %u (read at the next boot)\n", (UINT32)Index);
      } else {
        Print (L"Rpi5CapsuleApp: warning: truncating config.txt on volume %u failed: %r\n", (UINT32)Index, Status);
        AnyFailed = TRUE;
      }
    } else {
      Print (L"Rpi5CapsuleApp: warning: writing config.txt on volume %u failed: %r\n", (UINT32)Index, Status);
      AnyFailed = TRUE;
    }

    FileHandleClose (Target);
    Root->Close (Root);
  }

  FreePool (Handles);

  //
  // Consumed like a capsule once every reachable target is converged:
  // deletion is the applied-vs-pending signal the BMC reads back from
  // the drop box (a lingering file would list as staged forever).
  // Read-only media keeps it, matching the capsule behavior; a failed
  // or unreachable target keeps it too, so the next capsule event
  // retries.
  //
  if ((Targets > 0) && !AnyFailed) {
    Status = Dir->Open (
                    Dir,
                    &File,
                    SIDECAR_CONFIG_NAME,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    );
    if (!EFI_ERROR (Status)) {
      //
      // FileHandleDelete releases the handle whatever it returns.
      //
      FileHandleDelete (File);
    }
  }

  FreePool (Wanted);
}

/**
  Application entry point: apply every capsule in the boot volume's drop
  box, then reset if anything was applied.

  @param[in] ImageHandle  This application's image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  Nothing was applied; BDS continues its boot order.
  @return              Other status when the boot volume cannot be read.
**/
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_HANDLE                  Root;
  EFI_FILE_HANDLE                  Dir;
  EFI_FILE_INFO                    *Info;
  BOOLEAN                          NoFile;
  CHAR16                           *Names[MAX_CAPSULES];
  UINTN                            NameCount;
  UINTN                            Index;
  UINTN                            Applied;

  Print (L"\nRpi5CapsuleApp: firmware capsule updater\n");

  ConnectFirmwareTargetVolumes ();

  //
  // The volume this application was loaded from is the capsule volume;
  // BDS connected it in order to boot it, so no device connection work is
  // needed here at all.
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (
                    LoadedImage->DeviceHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
  }

  if (EFI_ERROR (Status)) {
    Print (L"Rpi5CapsuleApp: cannot reach my own boot volume: %r\n", Status);
    return Status;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    Print (L"Rpi5CapsuleApp: cannot open the boot volume: %r\n", Status);
    return Status;
  }

  Status = Root->Open (Root, &Dir, CAPSULE_DIR_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (
      L"Rpi5CapsuleApp: no %s directory on this volume (%r); nothing to do\n",
      CAPSULE_DIR_NAME,
      Status
      );
    Root->Close (Root);
    return EFI_SUCCESS;
  }

  //
  // Collect the names first, apply second: deleting entries out of a
  // directory that is being iterated is exactly the kind of FAT behavior
  // not worth depending on.
  //
  NameCount = 0;
  for (Status = FileHandleFindFirstFile (Dir, &Info), NoFile = FALSE;
       !EFI_ERROR (Status) && !NoFile && (Info != NULL);
       Status = FileHandleFindNextFile (Dir, Info, &NoFile))
  {
    if (((Info->Attribute & EFI_FILE_DIRECTORY) == 0) &&
        (Info->FileSize > 0) &&
        (StrCmp (Info->FileName, SIDECAR_CONFIG_NAME) != 0))
    {
      if (NameCount < MAX_CAPSULES) {
        Names[NameCount] = AllocateCopyPool (StrSize (Info->FileName), Info->FileName);
        if (Names[NameCount] != NULL) {
          NameCount++;
        }
      } else {
        Print (L"Rpi5CapsuleApp: more than %d entries, ignoring %s this boot\n", MAX_CAPSULES, Info->FileName);
      }
    }
  }

  if (NameCount == 0) {
    Print (L"Rpi5CapsuleApp: the drop box is empty; nothing to do\n");
    FileHandleClose (Dir);
    Root->Close (Root);
    return EFI_SUCCESS;
  }

  Applied = 0;
  for (Index = 0; Index < NameCount; Index++) {
    if (ApplyOneCapsule (Dir, Names[Index])) {
      Applied++;
    }

    FreePool (Names[Index]);
  }

  //
  // With the media's firmware proven current, bring its config.txt to
  // this build too -- delivered beside the capsule for exactly this.
  //
  if (mMediaFirmwareCurrent) {
    InstallSidecarConfig (Dir);
  }

  FileHandleClose (Dir);
  Root->Close (Root);

  Print (
    L"Rpi5CapsuleApp: %d of %d capsule(s) applied\n",
    (UINT32)Applied,
    (UINT32)NameCount
    );

  if (Applied > 0) {
    //
    // The firmware file on the boot medium has been rewritten; only a
    // fresh cold boot runs it. The stall keeps the summary readable.
    //
    Print (L"Rpi5CapsuleApp: resetting to boot the new firmware...\n");
    gBS->Stall (SUMMARY_STALL_US);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

  //
  // Nothing applied (empty box was handled above, so this is the failure
  // path): give a console reader a moment, then return -- BDS carries on
  // with the normal boot order, and the BMC still sees the capsule
  // pending plus, after the next sync, the LastAttemptStatus saying why.
  //
  gBS->Stall (SUMMARY_STALL_US);
  return EFI_SUCCESS;
}

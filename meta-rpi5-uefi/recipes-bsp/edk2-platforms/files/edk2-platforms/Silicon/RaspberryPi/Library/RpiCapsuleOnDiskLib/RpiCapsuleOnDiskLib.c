/** @file

  Boot-time scanner for staged UEFI FMP capsules: the firmware half of
  capsule-on-disk, for a platform whose capsules apply synchronously.

  Linked NULL into BdsDxe. At ReadyToBoot -- once per boot, on the first
  boot attempt -- this walks the \EFI\UpdateCapsule drop box (UEFI 2.10
  8.5.5) of every attached FAT volume and applies whatever it finds
  through gRT->UpdateCapsule(), exactly as Rpi5CapsuleApp does when the
  capsule volume itself is booted. Before scanning it connects USB
  mass-storage interfaces, so the LUN the BMC's gadget exposes is among
  the volumes searched even on a boot that never chose it. The two
  consumers share one contract: an applied capsule is deleted (how the
  BMC tells applied from pending), a failed one is left in place with
  LastAttemptStatus to say why, and any success ends in a cold reset
  into the freshly written firmware.

  Upstream's Capsule-on-Disk machinery (PcdCapsuleOnDiskSupport,
  CoDRelocateCapsule, CapsuleOnDiskLoadPei) is deliberately not used: it
  parks capsules across a reset for PEI to coalesce, and this platform
  has no PEI phase and no persist-across-reset support. A flagless
  capsule applied under boot services needs no relocation -- the scan IS
  the processing. PcdCapsuleOnDiskSupport must stay FALSE so BdsEntry's
  relocate-and-reset path never runs; the OsIndicationsSupported bit it
  would have advertised is OR'd back in here instead, after BdsDxe has
  recomputed the variable, so an OS-side deliverer (fwupd's
  capsule-on-disk mode) can discover that a file dropped in
  \EFI\UpdateCapsule will be picked up.

  Why ReadyToBoot, and why the work stays inside the TPL_CALLBACK
  notification: RpiRedfishSyncDxe executes BMC boot overrides -- stage
  BootNext, cold reset -- from network callbacks at TPL_CALLBACK, and
  only latches them off at ReadyToBoot. Same-TPL callbacks cannot
  preempt this notification, so no BMC exchange can cold-reset the
  machine while the in-place FD rewrite is in flight; that is the same
  protection Rpi5CapsuleApp gets from running after the latch.
  ConnectController from a TPL_CALLBACK notification follows UsbBusDxe's
  own hot-plug enumeration precedent.

  What this deliberately does not do: connect-all. A pending BMC-side
  capsule is reachable through the targeted USB mass-storage connect,
  and a capsule dropped on the boot ESP sits on a volume BDS already
  connected. Exotic splits (booting NVMe while the firmware file lives
  on an unconnected SD card) defer to Rpi5CapsuleApp, whose one-shot
  context can afford ConnectDeviceClass(ConnectAll); doing that here on
  every normal boot would re-open the NCM connect stall this platform
  works around.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Guid/FmpCapsule.h>
#include <Guid/SystemNvDataGuid.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareManagement.h>

#include <IndustryStandard/Usb.h>

#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>

#include <Library/DebugLib.h>
#include <Library/FileHandleLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>

#define CAPSULE_DIR_NAME  L"\\EFI\\UpdateCapsule"

//
// Bound on one capsule file. Matches Rpi5CapsuleApp and the BMC's
// HttpPushUri cap.
//
#define MAX_CAPSULE_BYTES  SIZE_128MB

//
// Bound on drop-box entries processed per volume in one boot. The BMC
// stages one or two; the bound only guards the name array.
//
#define MAX_CAPSULES  8

//
// How long a summary stays readable on a console -- before the reset
// that boots the new firmware, or before a failed boot carries on.
//
#define SUMMARY_STALL_US  (5 * 1000 * 1000)

//
// The names Rpi5FmpDeviceLib will try when FmpDxe asks it to write the
// firmware file -- kept in step with its candidate list. Used here only
// as a cheap "is the write target even reachable" gate before applying:
// a false positive (u-boot's identically named armstub8) costs one
// failed apply that FmpDxe reports, so the NV-FV identity check is not
// repeated.
//
STATIC CONST CHAR16  *mFirmwareFileNames[] = {
  L"armstub8-2712.bin",
  L"RPI_EFI.FD",
};

STATIC BOOLEAN  mRpiCodScanDone = FALSE;

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
  Advertise EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED.

  BdsDxe recomputes OsIndicationsSupported at BdsEntry from
  PcdCapsuleOnDiskSupport, which this platform keeps FALSE (see the file
  header); ReadyToBoot is after that write, so OR'ing the bit in here
  sticks for the OS session.
**/
STATIC
VOID
AdvertiseFileCapsuleDelivery (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      Supported;
  UINTN       Size;

  Supported = 0;
  Size      = sizeof (Supported);
  Status    = gRT->GetVariable (
                     EFI_OS_INDICATIONS_SUPPORT_VARIABLE_NAME,
                     &gEfiGlobalVariableGuid,
                     NULL,
                     &Size,
                     &Supported
                     );
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    return;
  }

  if ((Supported & EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED) != 0) {
    return;
  }

  Supported |= EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED;
  gRT->SetVariable (
         EFI_OS_INDICATIONS_SUPPORT_VARIABLE_NAME,
         &gEfiGlobalVariableGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
         sizeof (Supported),
         &Supported
         );
}

/**
  Consume EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED from
  OsIndications, if an OS set it. The scan runs regardless -- the BMC
  cannot set UEFI variables -- but a set bit left in place would ask
  every subsequent boot to process a delivery that already happened.
**/
STATIC
VOID
ClearFileCapsuleIndication (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      OsIndications;
  UINTN       Size;

  Size   = sizeof (OsIndications);
  Status = gRT->GetVariable (
                  EFI_OS_INDICATIONS_VARIABLE_NAME,
                  &gEfiGlobalVariableGuid,
                  NULL,
                  &Size,
                  &OsIndications
                  );
  if (EFI_ERROR (Status) ||
      ((OsIndications & EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED) == 0))
  {
    return;
  }

  OsIndications &= ~EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED;
  gRT->SetVariable (
         EFI_OS_INDICATIONS_VARIABLE_NAME,
         &gEfiGlobalVariableGuid,
         EFI_VARIABLE_NON_VOLATILE |
         EFI_VARIABLE_BOOTSERVICE_ACCESS |
         EFI_VARIABLE_RUNTIME_ACCESS,
         sizeof (OsIndications),
         &OsIndications
         );
}

/**
  Connect USB mass-storage interfaces to their filesystems, and nothing
  else on the bus.

  Two passes. The host-controller pass binds UsbBusDxe wherever a USB
  host controller driver already runs (non-recursive, so enumeration
  creates interface child handles without connecting them); the
  interface pass then recursively connects only interfaces whose class
  is mass storage, producing BlockIo -> partition -> FAT. The CDC-NCM
  interface of the same BMC gadget stays exactly as the Redfish stack
  left it.
**/
STATIC
VOID
ConnectUsbMassStorage (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;
  EFI_USB_IO_PROTOCOL           *UsbIo;
  EFI_USB_INTERFACE_DESCRIPTOR  Interface;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiUsb2HcProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }

    FreePool (Handles);
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiUsbIoProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiUsbIoProtocolGuid,
                    (VOID **)&UsbIo
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &Interface);
    if (EFI_ERROR (Status) || (Interface.InterfaceClass != USB_MASS_STORE_CLASS)) {
      continue;
    }

    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

/**
  Open a volume's root directory.

  @param[in]  FsHandle  Handle carrying SimpleFileSystem.
  @param[out] Root      The opened root on success.

  @retval TRUE   Root is open.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
OpenVolumeRoot (
  IN  EFI_HANDLE       FsHandle,
  OUT EFI_FILE_HANDLE  *Root
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;

  Status = gBS->HandleProtocol (
                  FsHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = Fs->OpenVolume (Fs, Root);
  return !EFI_ERROR (Status);
}

/**
  Does this volume's drop box hold at least one non-empty file?

  @param[in] Root  Open root directory of the volume.

  @retval TRUE   It does.
  @retval FALSE  No drop box, or nothing in it.
**/
STATIC
BOOLEAN
DropBoxHasCapsules (
  IN EFI_FILE_HANDLE  Root
  )
{
  EFI_STATUS       Status;
  EFI_FILE_HANDLE  Dir;
  EFI_FILE_INFO    *Info;
  BOOLEAN          NoFile;
  BOOLEAN          Found;

  Status = Root->Open (Root, &Dir, CAPSULE_DIR_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Found = FALSE;
  for (Status = FileHandleFindFirstFile (Dir, &Info), NoFile = FALSE;
       !EFI_ERROR (Status) && !NoFile && (Info != NULL) && !Found;
       Status = FileHandleFindNextFile (Dir, Info, &NoFile))
  {
    if (((Info->Attribute & EFI_FILE_DIRECTORY) == 0) &&
        (Info->FileSize > 0) &&
        (StrCmp (Info->FileName, SIDECAR_CONFIG_NAME) != 0))
    {
      Found = TRUE;
    }
  }

  FileHandleClose (Dir);
  return Found;
}

/**
  Is the firmware file Rpi5FmpDeviceLib rewrites reachable on any
  connected volume?

  @param[in] Handles  SimpleFileSystem handles to search.
  @param[in] Count    How many.

  @retval TRUE   A candidate firmware file exists somewhere.
  @retval FALSE  It does not; an apply would fail with the target absent.
**/
STATIC
BOOLEAN
FirmwareTargetPresent (
  IN EFI_HANDLE  *Handles,
  IN UINTN       Count
  )
{
  EFI_STATUS       Status;
  EFI_FILE_HANDLE  Root;
  EFI_FILE_HANDLE  File;
  UINTN            Index;
  UINTN            Name;

  for (Index = 0; Index < Count; Index++) {
    if (!OpenVolumeRoot (Handles[Index], &Root)) {
      continue;
    }

    for (Name = 0; Name < ARRAY_SIZE (mFirmwareFileNames); Name++) {
      Status = Root->Open (
                       Root,
                       &File,
                       (CHAR16 *)mFirmwareFileNames[Name],
                       EFI_FILE_MODE_READ,
                       0
                       );
      if (!EFI_ERROR (Status)) {
        FileHandleClose (File);
        Root->Close (Root);
        return TRUE;
      }
    }

    Root->Close (Root);
  }

  return FALSE;
}

//
// FMP_PAYLOAD_HEADER, as GenerateCapsule emits it ahead of the firmware
// image. Upstream keeps this struct private to FmpDevicePkg; the layout
// is stable and pinned by the "MSS1" signature.
//
#pragma pack(1)
typedef struct {
  UINT32    Signature;
  UINT32    HeaderSize;
  UINT32    FwVersion;
  UINT32    LowestSupportedVersion;
} RPI_COD_FMP_PAYLOAD_HEADER;
#pragma pack()

#define RPI_COD_FMP_PAYLOAD_SIGNATURE  SIGNATURE_32 ('M', 'S', 'S', '1')
#define RPI_COD_HEADER_SEARCH_LIMIT    SIZE_64KB

//
// Byte range of the firmware file a capsule replaces -- the same two PCDs
// Rpi5FmpDeviceLib subtracts, so the readback verify measures exactly
// what the writer wrote.
//
#define RPI_COD_UPDATABLE_SIZE \
  ((UINTN)(FixedPcdGet32 (PcdNvStorageVariableBase) - FixedPcdGet64 (PcdFdBaseAddress)))

/**
  Locate the FMP payload header and image-type GUID inside a capsule.

  @param[in]  Capsule      The capsule, fully read into memory.
  @param[in]  CapsuleSize  Its size.
  @param[out] TypeId       Receives UpdateImageTypeId when parseable.
  @param[out] FwVersion    Receives the payload's declared version.
  @param[out] Payload      Receives a pointer to the firmware image bytes.
  @param[out] PayloadSize  Receives the byte count from there to the end.

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
  CONST RPI_COD_FMP_PAYLOAD_HEADER                    *PayloadHeader;
  UINTN                                               Offset;
  UINTN                                               Limit;

  if (!CompareGuid (&Capsule->CapsuleGuid, &gEfiFmpCapsuleGuid)) {
    return FALSE;
  }

  Bytes = (CONST UINT8 *)Capsule;
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

  Limit = MIN (CapsuleSize, RPI_COD_HEADER_SEARCH_LIMIT);
  for (Offset = 0; Offset + sizeof (*PayloadHeader) <= Limit; Offset++) {
    PayloadHeader = (CONST VOID *)(Bytes + Offset);
    if ((PayloadHeader->Signature == RPI_COD_FMP_PAYLOAD_SIGNATURE) &&
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
  platform FMP. FmpDxe updates the record synchronously while SetTheImage
  runs; UpdateCapsule() itself never surfaces the failure (upstream
  ProcessFmpCapsuleImage returns success for any processed capsule).

  @param[in]  TypeId       Image type to match; zero GUID matches first.
  @param[out] Version      Receives the running version.
  @param[out] LastStatus   Receives LastAttemptStatus; optional.
  @param[out] LastVersion  Receives LastAttemptVersion; optional.

  @retval TRUE   Found.
  @retval FALSE  No matching FMP descriptor.
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
  UINT8                             DescCount;
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
    if (Fmp->GetImageInfo (Fmp, &InfoSize, NULL, NULL, NULL, NULL, NULL, NULL) != EFI_BUFFER_TOO_SMALL) {
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
                            &DescCount,
                            &DescSize,
                            &PackageVer,
                            &PackageVerName
                            );
    if (!EFI_ERROR (Status) && (DescCount > 0)) {
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
  Print the FMP's freshly recorded last-attempt status.

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
      L"RpiCapsuleOnDisk:   FMP recorded: LastAttemptVersion %u, LastAttemptStatus %u (%s)\n",
      LastVersion,
      LastStatus,
      LastAttemptStatusName (LastStatus)
      );
  }
}

/**
  Is this open file this platform's firmware image? Same identity test
  Rpi5FmpDeviceLib and VarBlockServiceDxe make.

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

  Status = File->SetPosition (File, RPI_COD_UPDATABLE_SIZE);
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
  the capsule carried. Only a verified write counts as applied.

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
  EFI_STATUS       Status;
  EFI_HANDLE       *Handles;
  UINTN            HandleCount;
  UINTN            Index;
  UINTN            NameIndex;
  EFI_FILE_HANDLE  Root;
  EFI_FILE_HANDLE  File;
  UINT8            *OnDisk;
  UINTN            ReadSize;
  UINTN            Matched;
  UINTN            Mismatched;

  if (PayloadSize < RPI_COD_UPDATABLE_SIZE) {
    return FALSE;
  }

  OnDisk = AllocatePool (RPI_COD_UPDATABLE_SIZE);
  if (OnDisk == NULL) {
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
    if (!OpenVolumeRoot (Handles[Index], &Root)) {
      continue;
    }

    for (NameIndex = 0; NameIndex < ARRAY_SIZE (mFirmwareFileNames); NameIndex++) {
      if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)mFirmwareFileNames[NameIndex], EFI_FILE_MODE_READ, 0))) {
        continue;
      }

      if (FileIsThisFirmware (File)) {
        ReadSize = RPI_COD_UPDATABLE_SIZE;
        if (!EFI_ERROR (File->SetPosition (File, 0)) &&
            !EFI_ERROR (File->Read (File, &ReadSize, OnDisk)) &&
            (ReadSize == RPI_COD_UPDATABLE_SIZE) &&
            (CompareMem (OnDisk, Payload, RPI_COD_UPDATABLE_SIZE) == 0))
        {
          Matched++;
          Print (L"RpiCapsuleOnDisk:   %s on volume %u matches the capsule payload\n", mFirmwareFileNames[NameIndex], (UINT32)Index);
        } else {
          Mismatched++;
          Print (L"RpiCapsuleOnDisk:   %s on volume %u DIFFERS from the capsule payload\n", mFirmwareFileNames[NameIndex], (UINT32)Index);
        }
      }

      FileHandleClose (File);
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  FreePool (OnDisk);

  if ((Matched + Mismatched) > 1) {
    Print (L"RpiCapsuleOnDisk: warning: %u firmware files exist across volumes; the VPU boots only one of them\n", (UINT32)(Matched + Mismatched));
  }

  return (Matched > 0) && (Mismatched == 0);
}

/**
  Apply one capsule file from a drop box. Mirrors Rpi5CapsuleApp's
  checks and contract exactly.

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
  CONST UINT8         *Payload;
  UINTN               PayloadSize;

  //
  // Read-write so a successful apply can delete the file; a physically
  // write-protected volume still gets its capsule applied, it just
  // cannot be marked consumed.
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
    Print (L"RpiCapsuleOnDisk: cannot open %s: %r\n", Name, Status);
    return FALSE;
  }

  Status = FileHandleGetSize (File, &FileSize);
  if (EFI_ERROR (Status) ||
      (FileSize < sizeof (EFI_CAPSULE_HEADER)) ||
      (FileSize > MAX_CAPSULE_BYTES))
  {
    Print (L"RpiCapsuleOnDisk: %s has no plausible capsule size (%Lu bytes)\n", Name, FileSize);
    FileHandleClose (File);
    return FALSE;
  }

  Capsule = AllocatePool ((UINTN)FileSize);
  if (Capsule == NULL) {
    Print (L"RpiCapsuleOnDisk: out of memory for %s (%Lu bytes)\n", Name, FileSize);
    FileHandleClose (File);
    return FALSE;
  }

  ReadSize = (UINTN)FileSize;
  Status   = FileHandleRead (File, &ReadSize, Capsule);
  if (EFI_ERROR (Status) || (ReadSize != (UINTN)FileSize)) {
    Print (L"RpiCapsuleOnDisk: reading %s failed: %r\n", Name, Status);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  if ((Capsule->HeaderSize < sizeof (EFI_CAPSULE_HEADER)) ||
      (Capsule->CapsuleImageSize > FileSize) ||
      (Capsule->CapsuleImageSize < Capsule->HeaderSize))
  {
    Print (L"RpiCapsuleOnDisk: %s carries an inconsistent capsule header\n", Name);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  if ((Capsule->Flags & CAPSULE_FLAGS_PERSIST_ACROSS_RESET) != 0) {
    //
    // This platform deliberately builds none of the reset-persistence
    // machinery (see Rpi5FmpDeviceLib's README); such a capsule would
    // be refused by CapsuleRuntimeDxe with a status that reads like a
    // bug. Name the real problem instead.
    //
    Print (
      L"RpiCapsuleOnDisk: %s requests PersistAcrossReset, which this platform does not support; rebuild the capsule flagless\n",
      Name
      );
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  //
  // Version gate. FmpDxe compares an incoming image only against the
  // lowest supported version, never the running one -- without this an
  // undeletable capsule (read-only stick) re-applies identical bytes and
  // cold-resets on every boot, forever.
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
      L"RpiCapsuleOnDisk: %s carries version %u; running firmware is %u\n",
      Name,
      CapsuleVersion,
      RunningVersion
      );

    if (CapsuleVersion == RunningVersion) {
      Print (L"RpiCapsuleOnDisk: %s is the running version already, skipping\n", Name);
      mMediaFirmwareCurrent = TRUE;
      if (CanDelete) {
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
      Print (L"RpiCapsuleOnDisk: *** DOWNGRADING from %u to %u ***\n", RunningVersion, CapsuleVersion);
    }
  } else {
    Print (L"RpiCapsuleOnDisk: warning: cannot compare versions; applying blind\n");
  }

  Print (
    L"RpiCapsuleOnDisk: applying %s (%u bytes, %g)...\n",
    Name,
    Capsule->CapsuleImageSize,
    &Capsule->CapsuleGuid
    );

  //
  // Flagless capsule: applied inside the call, under boot services --
  // authentication, version gates, progress display and the FD write
  // all happen before this returns.
  //
  HeaderArray[0] = Capsule;
  Status         = gRT->UpdateCapsule (HeaderArray, 1, 0);

  if (EFI_ERROR (Status)) {
    Print (L"RpiCapsuleOnDisk: %s NOT applied: %r\n", Name, Status);
    ReportLastAttempt (&TypeId);
    FreePool (Capsule);
    FileHandleClose (File);
    return FALSE;
  }

  //
  // UpdateCapsule() success means "processed", never "applied" --
  // upstream records the SetImage status and returns success either way.
  // Only the media's own bytes prove the write. No legacy bootstrap here,
  // deliberately: any firmware carrying this library postdates the FMP
  // lock disarm, so the one state that needed it cannot occur.
  //
  if (HaveCapsuleVersion) {
    if (!VerifyFirmwareOnDisk (Payload, PayloadSize)) {
      Print (L"RpiCapsuleOnDisk: %s MISMATCH: the firmware file still carries the OLD bytes -- the write never happened\n", Name);
      ReportLastAttempt (&TypeId);
      FreePool (Capsule);
      FileHandleClose (File);
      return FALSE;
    }

    Print (L"RpiCapsuleOnDisk: %s applied and VERIFIED on disk\n", Name);
    mMediaFirmwareCurrent = TRUE;
  } else {
    Print (L"RpiCapsuleOnDisk: %s applied (unverified: payload location unknown)\n", Name);
  }

  FreePool (Capsule);

  if (CanDelete) {
    //
    // Deleting the consumed capsule is the applied-vs-pending signal
    // the BMC reads back from the volume. FileHandleDelete releases the
    // handle whatever it returns.
    //
    Status = FileHandleDelete (File);
    if (EFI_ERROR (Status)) {
      Print (L"RpiCapsuleOnDisk: warning: could not delete %s: %r\n", Name, Status);
    }
  } else {
    Print (L"RpiCapsuleOnDisk: warning: %s is on read-only media, left in place\n", Name);
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
    Print (L"RpiCapsuleOnDisk: warning: could not read the sidecar config.txt; boot config left alone\n");
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
    if (!OpenVolumeRoot (Handles[Index], &Root)) {
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
      Print (L"RpiCapsuleOnDisk: config.txt on volume %u is current\n", (UINT32)Index);
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
      Print (L"RpiCapsuleOnDisk: warning: cannot open config.txt on volume %u: %r\n", (UINT32)Index, Status);
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
        Print (L"RpiCapsuleOnDisk: config.txt updated on volume %u (read at the next boot)\n", (UINT32)Index);
      } else {
        Print (L"RpiCapsuleOnDisk: warning: truncating config.txt on volume %u failed: %r\n", (UINT32)Index, Status);
        AnyFailed = TRUE;
      }
    } else {
      Print (L"RpiCapsuleOnDisk: warning: writing config.txt on volume %u failed: %r\n", (UINT32)Index, Status);
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
  Apply every capsule in one volume's drop box.

  @param[in]     Root     Open root directory of the volume.
  @param[in,out] Applied  Incremented per capsule applied.
  @param[in,out] Failed   Incremented per capsule left in place.
**/
STATIC
VOID
ProcessDropBox (
  IN     EFI_FILE_HANDLE  Root,
  IN OUT UINTN            *Applied,
  IN OUT UINTN            *Failed
  )
{
  EFI_STATUS       Status;
  EFI_FILE_HANDLE  Dir;
  EFI_FILE_INFO    *Info;
  BOOLEAN          NoFile;
  CHAR16           *Names[MAX_CAPSULES];
  UINTN            NameCount;
  UINTN            Index;

  Status = Root->Open (Root, &Dir, CAPSULE_DIR_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return;
  }

  mMediaFirmwareCurrent = FALSE;

  //
  // Collect the names first, apply second: deleting entries out of a
  // directory that is being iterated is exactly the kind of FAT
  // behavior not worth depending on.
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
        Print (L"RpiCapsuleOnDisk: more than %d entries, ignoring %s this boot\n", MAX_CAPSULES, Info->FileName);
      }
    }
  }

  for (Index = 0; Index < NameCount; Index++) {
    if (ApplyOneCapsule (Dir, Names[Index])) {
      (*Applied)++;
    } else {
      (*Failed)++;
    }

    FreePool (Names[Index]);
  }

  //
  // With this media's firmware proven current, bring its config.txt to
  // this build too -- delivered beside the capsule for exactly this.
  //
  if (mMediaFirmwareCurrent) {
    InstallSidecarConfig (Dir);
  }

  FileHandleClose (Dir);
}

/**
  ReadyToBoot: scan every attached volume's drop box and apply what is
  staged. Runs once per boot; later boot attempts in the same BDS pass
  find the latch set.

  @param[in] Event    The ReadyToBoot event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS       Status;
  EFI_HANDLE       *Handles;
  UINTN            Count;
  UINTN            Index;
  EFI_FILE_HANDLE  Root;
  BOOLEAN          AnyStaged;
  UINTN            Applied;
  UINTN            Failed;

  if (mRpiCodScanDone) {
    return;
  }

  mRpiCodScanDone = TRUE;

  AdvertiseFileCapsuleDelivery ();
  ConnectUsbMassStorage ();

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    ClearFileCapsuleIndication ();
    return;
  }

  //
  // First pass: is anything staged anywhere? The common boot must stay
  // silent and near-free.
  //
  AnyStaged = FALSE;
  for (Index = 0; Index < Count && !AnyStaged; Index++) {
    if (OpenVolumeRoot (Handles[Index], &Root)) {
      AnyStaged = DropBoxHasCapsules (Root);
      Root->Close (Root);
    }
  }

  if (!AnyStaged) {
    ClearFileCapsuleIndication ();
    FreePool (Handles);
    return;
  }

  Print (L"\nRpiCapsuleOnDisk: staged firmware capsule(s) found\n");

  if (!FirmwareTargetPresent (Handles, Count)) {
    //
    // The write target is on a volume this boot never connected (see
    // the file header on why there is no connect-all here). Leave
    // everything -- the capsule, and the OsIndications bit -- for a
    // boot that can reach it, or for Rpi5CapsuleApp.
    //
    Print (L"RpiCapsuleOnDisk: the firmware file's volume is not connected; leaving capsule(s) staged\n");
    FreePool (Handles);
    return;
  }

  Applied = 0;
  Failed  = 0;
  for (Index = 0; Index < Count; Index++) {
    if (OpenVolumeRoot (Handles[Index], &Root)) {
      ProcessDropBox (Root, &Applied, &Failed);
      Root->Close (Root);
    }
  }

  FreePool (Handles);
  ClearFileCapsuleIndication ();

  Print (
    L"RpiCapsuleOnDisk: %d of %d capsule(s) applied\n",
    (UINT32)Applied,
    (UINT32)(Applied + Failed)
    );

  if (Applied > 0) {
    //
    // The firmware file on the boot medium has been rewritten; only a
    // fresh cold boot runs it. The stall keeps the summary readable.
    //
    Print (L"RpiCapsuleOnDisk: resetting to boot the new firmware...\n");
    gBS->Stall (SUMMARY_STALL_US);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

  if (Failed > 0) {
    //
    // Nothing applied: give a console reader a moment, then let BDS
    // carry on. The capsule stays pending and the next firmware
    // inventory report carries the LastAttemptStatus that says why.
    //
    gBS->Stall (SUMMARY_STALL_US);
  }
}

/**
  Register the ReadyToBoot scan. Never fails BdsDxe's load: a firmware
  that cannot scan for capsules must still boot.

  @param[in] ImageHandle  BdsDxe's image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
RpiCapsuleOnDiskLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RpiCapsuleOnDisk: ReadyToBoot registration - %r\n", Status));
  }

  return EFI_SUCCESS;
}

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

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Guid/FileInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FileHandleLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BootManagerPolicy.h>
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

  FreePool (Capsule);

  if (EFI_ERROR (Status)) {
    Print (L"Rpi5CapsuleApp: %s NOT applied: %r\n", Name, Status);
    Print (L"  (a security violation here means the capsule is not signed with the running firmware's certificate)\n");
    FileHandleClose (File);
    return FALSE;
  }

  Print (L"Rpi5CapsuleApp: %s applied\n", Name);

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
    if (((Info->Attribute & EFI_FILE_DIRECTORY) == 0) && (Info->FileSize > 0)) {
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

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
    if (((Info->Attribute & EFI_FILE_DIRECTORY) == 0) && (Info->FileSize > 0)) {
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

  FreePool (Capsule);

  if (EFI_ERROR (Status)) {
    Print (L"RpiCapsuleOnDisk: %s NOT applied: %r\n", Name, Status);
    Print (L"  (a security violation here means the capsule is not signed with the running firmware's certificate)\n");
    FileHandleClose (File);
    return FALSE;
  }

  Print (L"RpiCapsuleOnDisk: %s applied\n", Name);

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
    if (((Info->Attribute & EFI_FILE_DIRECTORY) == 0) && (Info->FileSize > 0)) {
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

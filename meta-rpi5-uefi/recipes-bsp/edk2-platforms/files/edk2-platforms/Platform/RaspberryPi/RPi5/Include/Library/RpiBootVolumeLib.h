/** @file

  RpiBootVolumeLib - file operations on the VPU boot volume.

  The boot volume is the FAT filesystem the VPU bootloader loaded the
  firmware from: the one carrying armstub8-2712.bin (falling back to any
  volume with a config.txt). It is where staged bootloader updates
  (pieeprom.upd), the firmware file itself and config.txt all live, so
  several Setup pages and boot-time services need the same locator and
  the same careful file-replacement discipline; this library is that
  shared code (lifted from BootloaderConfigDxe, which now consumes it).

  All of this requires SimpleFileSystem instances, which exist only after
  BDS has connected the boot medium: callable from Setup-page callbacks
  and ReadyToBoot notifications, NOT from DXE entry points.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_BOOT_VOLUME_LIB_H_
#define RPI_BOOT_VOLUME_LIB_H_

#include <Protocol/SimpleFileSystem.h>

/**
  Open the boot volume's root: the filesystem carrying our firmware
  (armstub8-2712.bin), falling back to any volume with a config.txt.

  @param[out] Root  The opened root directory; caller closes it.

  @retval EFI_SUCCESS    A boot volume was found and opened.
  @retval EFI_NOT_FOUND  No candidate volume is connected.
**/
EFI_STATUS
RpiOpenBootVolume (
  OUT EFI_FILE_PROTOCOL  **Root
  );

/**
  Read a whole file into a freshly allocated, NUL-terminated buffer.
  The terminator is for ASCII-text convenience and is not counted in Len.

  @param[in]  Root  Directory to open Name under.
  @param[in]  Name  File to read.
  @param[out] Data  Allocated buffer of Len + 1 bytes; caller frees.
  @param[out] Len   File size in bytes.
**/
EFI_STATUS
RpiReadFileContent (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CHAR16             *Name,
  OUT VOID               **Data,
  OUT UINTN              *Len
  );

/**
  Create Name with exactly the given content, replacing any previous
  file (delete + recreate, so no stale tail can survive). The file does
  not exist during the replacement: use only for files whose absence is
  benign (staged updates), never for files the platform needs to boot.
**/
EFI_STATUS
RpiReplaceFileContent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name,
  IN CONST VOID         *Data,
  IN UINTN              Len
  );

/**
  Rewrite an existing file's content in place: write from offset zero,
  then truncate any old tail. The file always exists and keeps its head
  bytes through a mid-write power cut - the failure mode is a stale or
  garbled tail, not a missing file - so this is the writer for files the
  VPU must find on the next boot (config.txt).

  @retval EFI_NOT_FOUND  Name does not exist (nothing is created).
**/
EFI_STATUS
RpiRewriteFileInPlace (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name,
  IN CONST VOID         *Data,
  IN UINTN              Len
  );

/**
  Delete Name if it exists; quiet in every outcome.
**/
VOID
RpiDeleteFileIfPresent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name
  );

#endif // RPI_BOOT_VOLUME_LIB_H_

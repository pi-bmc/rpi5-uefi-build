/** @file

  RpiBootVolumeLib - file operations on the VPU boot volume.

  See Include/Library/RpiBootVolumeLib.h. The locator and the
  delete+recreate writer are lifted verbatim from BootloaderConfigDxe's
  Setup page, which now links this library.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RpiBootVolumeLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/FileInfo.h>

EFI_STATUS
RpiOpenBootVolume (
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Candidate;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_PROTOCOL                *Best;
  UINTN                            BestScore;
  UINTN                            Score;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Best      = NULL;
  BestScore = 0;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (EFI_ERROR (Sfs->OpenVolume (Sfs, &Candidate))) {
      continue;
    }

    Score = 0;
    if (!EFI_ERROR (
           Candidate->Open (
                        Candidate,
                        &File,
                        L"armstub8-2712.bin",
                        EFI_FILE_MODE_READ,
                        0
                        )
           ))
    {
      File->Close (File);
      Score = 2;
    } else if (!EFI_ERROR (
                  Candidate->Open (
                               Candidate,
                               &File,
                               L"config.txt",
                               EFI_FILE_MODE_READ,
                               0
                               )
                  ))
    {
      File->Close (File);
      Score = 1;
    }

    if (Score > BestScore) {
      if (Best != NULL) {
        Best->Close (Best);
      }

      Best      = Candidate;
      BestScore = Score;
      if (Score == 2) {
        break;
      }
    } else {
      Candidate->Close (Candidate);
    }
  }

  FreePool (Handles);

  if (Best == NULL) {
    return EFI_NOT_FOUND;
  }

  *Root = Best;
  return EFI_SUCCESS;
}

/**
  GetInfo(EFI_FILE_INFO) with the buffer dance; caller frees *Info.
**/
STATIC
EFI_STATUS
GetFileInfo (
  IN  EFI_FILE_PROTOCOL  *File,
  OUT EFI_FILE_INFO      **Info
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size   = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  *Info = AllocatePool (Size);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, *Info);
  if (EFI_ERROR (Status)) {
    FreePool (*Info);
    *Info = NULL;
  }

  return Status;
}

EFI_STATUS
RpiReadFileContent (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CHAR16             *Name,
  OUT VOID               **Data,
  OUT UINTN              *Len
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINT8              *Buffer;
  UINTN              ReadLen;

  Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GetFileInfo (File, &Info);
  if (EFI_ERROR (Status)) {
    File->Close (File);
    return Status;
  }

  ReadLen = (UINTN)Info->FileSize;
  FreePool (Info);

  Buffer = AllocatePool (ReadLen + 1);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &ReadLen, Buffer);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return Status;
  }

  Buffer[ReadLen] = '\0';
  *Data           = Buffer;
  *Len            = ReadLen;
  return EFI_SUCCESS;
}

VOID
RpiDeleteFileIfPresent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (!EFI_ERROR (
         Root->Open (
                 Root,
                 &File,
                 Name,
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                 0
                 )
         ))
  {
    //
    // Delete() closes the handle regardless of the outcome.
    //
    File->Delete (File);
  }
}

EFI_STATUS
RpiReplaceFileContent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name,
  IN CONST VOID         *Data,
  IN UINTN              Len
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              WriteLen;

  RpiDeleteFileIfPresent (Root, Name);

  Status = Root->Open (
                   Root,
                   &File,
                   Name,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  WriteLen = Len;
  Status   = File->Write (File, &WriteLen, (VOID *)Data);
  if (!EFI_ERROR (Status) && (WriteLen != Len)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);

  if (EFI_ERROR (Status)) {
    RpiDeleteFileIfPresent (Root, Name);
  }

  return Status;
}

EFI_STATUS
RpiRewriteFileInPlace (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name,
  IN CONST VOID         *Data,
  IN UINTN              Len
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              WriteLen;
  UINTN              InfoSize;

  Status = Root->Open (
                   Root,
                   &File,
                   Name,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  WriteLen = Len;
  Status   = File->Write (File, &WriteLen, (VOID *)Data);
  if (!EFI_ERROR (Status) && (WriteLen != Len)) {
    Status = EFI_DEVICE_ERROR;
  }

  //
  // Truncate a shrinking file's old tail. Ordered after the data write:
  // a power cut in the window leaves head + stale tail, which for the
  // line-oriented files this writer serves is still parseable.
  //
  if (!EFI_ERROR (Status)) {
    Status = GetFileInfo (File, &Info);
    if (!EFI_ERROR (Status)) {
      if (Info->FileSize > Len) {
        Info->FileSize = Len;
        InfoSize       = (UINTN)Info->Size;
        Status         = File->SetInfo (File, &gEfiFileInfoGuid, InfoSize, Info);
      }

      FreePool (Info);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);
  return Status;
}

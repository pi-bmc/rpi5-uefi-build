/** @file

  Firmware inventory: what this board runs, reported to the BMC as a Redfish
  SoftwareInventory, and the pull side of a remote update.

  The source is EFI_FIRMWARE_MANAGEMENT_PROTOCOL rather than the ESRT, even
  though the ESRT is what an OS reads. FMP carries the same numbers plus the
  two things a human-facing inventory wants and ESRT has no room for: the
  version string and the image's name. Walking FMP also means an image that
  produced no ESRT entry -- because EsrtFmpDxe ran before it -- is still
  reported.

  Everything here is fail-open, like the rest of RpiRedfishSyncDxe: a BMC with
  no UpdateService, or none that knows this resource, 404s and the host carries
  on booting.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "RpiRedfishSyncDxe.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/FirmwareManagement.h>

/**
  Render a GUID as the lowercase 8-4-4-4-12 text Redfish resources use.

  @param  Guid[in]     GUID to render.
  @param  Buffer[out]  Receives the text.
  @param  BufferSize[in]  Capacity of Buffer, at least 37 bytes.

**/
STATIC
VOID
RenderGuid (
  IN  CONST EFI_GUID  *Guid,
  OUT CHAR8           *Buffer,
  IN  UINTN           BufferSize
  )
{
  AsciiSPrint (
    Buffer,
    BufferSize,
    "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    Guid->Data1,
    Guid->Data2,
    Guid->Data3,
    Guid->Data4[0],
    Guid->Data4[1],
    Guid->Data4[2],
    Guid->Data4[3],
    Guid->Data4[4],
    Guid->Data4[5],
    Guid->Data4[6],
    Guid->Data4[7]
    );
}

/**
  Copy a UCS-2 descriptor string into the ASCII the JSON body carries.

  Truncation is silent and deliberate: a version string longer than the field is
  still more useful reported short than not reported.

**/
STATIC
VOID
CopyDescriptorString (
  IN  CONST CHAR16  *Source,
  OUT CHAR8         *Dest,
  IN  UINTN         DestSize
  )
{
  UINTN  Index;

  Dest[0] = '\0';
  if (Source == NULL) {
    return;
  }

  for (Index = 0; (Index < DestSize - 1) && (Source[Index] != L'\0'); Index++) {
    //
    // Anything outside 7-bit ASCII becomes '?': the body is ASCII JSON, and a
    // stray high byte would make it invalid rather than merely ugly.
    //
    Dest[Index] = (Source[Index] < 0x80) ? (CHAR8)Source[Index] : '?';
  }

  Dest[Index] = '\0';
}

/**
  Collect every updatable firmware image this platform publishes.

  @param  Images[out]  Receives the collected images.
  @param  Max[in]      Capacity of Images.
  @param  Count[out]   Receives the number written.

  @retval EFI_SUCCESS    Zero or more images were collected.
  @retval EFI_NOT_FOUND  No Firmware Management Protocol is installed.

**/
EFI_STATUS
RpiRedfishCollectFirmware (
  OUT RPI_REDFISH_FIRMWARE_IMAGE  *Images,
  IN  UINTN                       Max,
  OUT UINTN                       *Count
  )
{
  EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *Fmp;
  EFI_FIRMWARE_IMAGE_DESCRIPTOR     *Descriptors;
  EFI_FIRMWARE_IMAGE_DESCRIPTOR     *Descriptor;
  RPI_REDFISH_FIRMWARE_IMAGE        *Image;
  EFI_STATUS                        Status;
  EFI_HANDLE                        *Handles;
  UINTN                             HandleCount;
  UINTN                             HandleIndex;
  UINTN                             DescriptorIndex;
  UINTN                             InfoSize;
  UINT32                            DescriptorVersion;
  UINT8                             DescriptorCount;
  UINTN                             DescriptorSize;
  UINT32                            PackageVersion;
  CHAR16                            *PackageVersionName;

  *Count = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareManagementProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (HandleIndex = 0; (HandleIndex < HandleCount) && (*Count < Max); HandleIndex++) {
    Status = gBS->HandleProtocol (
                    Handles[HandleIndex],
                    &gEfiFirmwareManagementProtocolGuid,
                    (VOID **)&Fmp
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Size query first: GetImageInfo reports what it needs through InfoSize and
    // returns EFI_BUFFER_TOO_SMALL. Anything else means this instance has
    // nothing to say.
    //
    InfoSize = 0;
    Status   = Fmp->GetImageInfo (
                      Fmp,
                      &InfoSize,
                      NULL,
                      &DescriptorVersion,
                      &DescriptorCount,
                      &DescriptorSize,
                      &PackageVersion,
                      &PackageVersionName
                      );
    if (Status != EFI_BUFFER_TOO_SMALL) {
      continue;
    }

    Descriptors = AllocateZeroPool (InfoSize);
    if (Descriptors == NULL) {
      continue;
    }

    Status = Fmp->GetImageInfo (
                    Fmp,
                    &InfoSize,
                    Descriptors,
                    &DescriptorVersion,
                    &DescriptorCount,
                    &DescriptorSize,
                    &PackageVersion,
                    &PackageVersionName
                    );
    if (EFI_ERROR (Status)) {
      FreePool (Descriptors);
      continue;
    }

    for (DescriptorIndex = 0;
         (DescriptorIndex < DescriptorCount) && (*Count < Max);
         DescriptorIndex++)
    {
      //
      // Stride by the reported DescriptorSize, not by sizeof(): the structure
      // has grown across descriptor versions and a producer may be newer than
      // the header this was built against.
      //
      Descriptor = (EFI_FIRMWARE_IMAGE_DESCRIPTOR *)
                   ((UINT8 *)Descriptors + (DescriptorIndex * DescriptorSize));

      Image = &Images[*Count];
      ZeroMem (Image, sizeof (*Image));

      RenderGuid (&Descriptor->ImageTypeId, Image->ImageTypeId, sizeof (Image->ImageTypeId));
      CopyDescriptorString (Descriptor->ImageIdName, Image->Name, sizeof (Image->Name));
      CopyDescriptorString (Descriptor->VersionName, Image->Version, sizeof (Image->Version));

      Image->VersionNumber          = Descriptor->Version;
      Image->LowestSupportedVersion = Descriptor->LowestSupportedImageVersion;
      Image->Updateable             =
        (BOOLEAN)((Descriptor->AttributesSetting & IMAGE_ATTRIBUTE_IMAGE_UPDATABLE) != 0);

      //
      // LastAttemptVersion/Status only exist from descriptor version 3 on. On
      // an older producer they are absent rather than zero, so say so instead
      // of reporting a successful attempt that never happened.
      //
      if (DescriptorVersion >= 3) {
        Image->LastAttemptVersion = Descriptor->LastAttemptVersion;
        Image->LastAttemptStatus  = Descriptor->LastAttemptStatus;
        Image->LastAttemptValid   = TRUE;
      }

      (*Count)++;
    }

    FreePool (Descriptors);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

/**
  Build the SoftwareInventory PATCH body for one firmware image.

  @param  Image[in]  Image to describe.
  @param  Json[out]  Receives an allocated ASCII JSON body. Caller frees with
                     FreePool().

  @retval EFI_SUCCESS           Body was built.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.

**/
EFI_STATUS
RpiRedfishBuildFirmwareInventoryPatch (
  IN  RPI_REDFISH_FIRMWARE_IMAGE  *Image,
  OUT CHAR8                       **Json
  )
{
  CHAR8  *Body;
  UINTN  Size;
  UINTN  Offset;

  if ((Image == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Size = 1024;
  Body = AllocateZeroPool (Size);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // SoftwareInventory v1_2_x. SoftwareId carries the ESRT firmware class GUID:
  // it is the identifier a capsule is built against, so an operator comparing
  // "what is installed" with "what this capsule is for" has both in one place.
  //
  Offset = AsciiSPrint (
             Body,
             Size,
             "{\"@odata.type\":\"#SoftwareInventory.v1_2_3.SoftwareInventory\","
             "\"Id\":\"%a\","
             "\"Name\":\"%a\","
             "\"Version\":\"%a\","
             "\"SoftwareId\":\"%a\","
             "\"Updateable\":%a,"
             "\"Status\":{\"State\":\"Enabled\",\"Health\":\"OK\"}",
             RPI_REDFISH_FIRMWARE_INVENTORY_ID,
             (Image->Name[0] != '\0') ? Image->Name : "Raspberry Pi 5 UEFI Firmware",
             (Image->Version[0] != '\0') ? Image->Version : "unknown",
             Image->ImageTypeId,
             Image->Updateable ? "true" : "false"
             );

  //
  // The integers ESRT publishes, under Oem because SoftwareInventory has no
  // schema property for them, and they are exactly what a BMC needs to decide
  // whether an update is worth staging: FmpDxe compares VersionNumber against
  // the capsule's, and refuses anything below LowestSupportedVersion.
  //
  Offset += AsciiSPrint (
              Body + Offset,
              Size - Offset,
              ",\"Oem\":{\"PiBmc\":{"
              "\"FirmwareVersion\":%u,"
              "\"LowestSupportedVersion\":%u",
              Image->VersionNumber,
              Image->LowestSupportedVersion
              );

  if (Image->LastAttemptValid) {
    Offset += AsciiSPrint (
                Body + Offset,
                Size - Offset,
                ",\"LastAttemptVersion\":%u,\"LastAttemptStatus\":%u",
                Image->LastAttemptVersion,
                Image->LastAttemptStatus
                );
  }

  AsciiSPrint (Body + Offset, Size - Offset, "}}}");

  *Json = Body;
  return EFI_SUCCESS;
}

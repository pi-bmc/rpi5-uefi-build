/** @file

  BlkInfoMirrorDxe - mirror the probed block-device inventory to the BMC's
  shared EEPROM.

  At ReadyToBoot (after BDS ConnectAll, so NVMe/USB/SD are probed) every
  physical EFI_BLOCK_IO_PROTOCOL device with media present is serialized to
  the "BLK1" region of the shared 24c256 at PcdBmcEepromBlkInfoOffset:

    "BLK1" + UINT16 LE JSON length +
    {"v":1,"drives":[{"if":"nvme","dev":0,"vendor":"..","product":"..",
                      "rev":"..","removable":0,"size":<bytes>}, ...]}

  The schema and the string sanitizer are byte-compatible with the pi-bmc
  U-Boot port's blkinfo region (same magic, same JSON shape), so the BMC
  parses one format regardless of which bootloader wrote it. "dev" is a
  per-interface ordinal in UEFI discovery order; it may differ from
  U-Boot's devnum for the same drive - the BMC correlates loosely (by
  interface/product/size), not by exact numbering.

  The write goes through BmcEepromWriteIfChanged, so a steady-state boot
  costs one EEPROM read and no wear.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <IndustryStandard/Nvme.h>
#include <IndustryStandard/Scsi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BmcEepromLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskInfo.h>

#include <RpiBmcEeprom.h>

//
// SCSI standard INQUIRY data: byte offsets/lengths of the id strings.
//
#define SCSI_INQ_VENDOR_OFFSET   8
#define SCSI_INQ_VENDOR_LEN      8
#define SCSI_INQ_PRODUCT_OFFSET  16
#define SCSI_INQ_PRODUCT_LEN     16
#define SCSI_INQ_REV_OFFSET      32
#define SCSI_INQ_REV_LEN         4
#define SCSI_INQ_MIN_LEN         36

//
// NVMe Identify Controller data: byte offsets/lengths of the id strings
// (Sn @4, Mn @24, Fr @64 - same layout as NVME_ADMIN_CONTROLLER_DATA).
//
#define NVME_ID_SN_OFFSET  4
#define NVME_ID_SN_LEN     20
#define NVME_ID_MN_OFFSET  24
#define NVME_ID_MN_LEN     40
#define NVME_ID_FR_OFFSET  64
#define NVME_ID_FR_LEN     8

//
// Generous string buffers: the largest source field is NVMe Mn (40 bytes).
//
#define DRIVE_STR_MAX  48

//
// One serialized JSON drive entry; sized like U-Boot's (worst case is
// ~166 bytes with all fields at maximum length).
//
#define DRIVE_ENTRY_MAX  192

STATIC EFI_EVENT  mReadyToBootEvent;

/**
  Copy a fixed-length identify/inquiry field into Dst as JSON-safe text,
  exactly like U-Boot's json_str(): printable ASCII only, '"' '\\' and
  control/high bytes replaced by ' ', trailing spaces trimmed (ATA/SCSI
  identify data is space-padded). Stops early at a NUL in the source.

  @param[out] Dst      Destination buffer, always NUL-terminated.
  @param[in]  DstSize  Destination size in bytes, including the NUL.
  @param[in]  Src      Source field (not necessarily NUL-terminated).
  @param[in]  SrcLen   Source field length in bytes.
**/
STATIC
VOID
JsonStrFromField (
  OUT CHAR8        *Dst,
  IN  UINTN        DstSize,
  IN  CONST UINT8  *Src,
  IN  UINTN        SrcLen
  )
{
  UINTN  Index;
  UINTN  N;
  UINT8  C;

  ASSERT (DstSize > 0);

  N = 0;
  for (Index = 0; Index < SrcLen && N + 1 < DstSize; Index++) {
    C = Src[Index];
    if (C == 0) {
      break;
    }

    if ((C < 0x20) || (C > 0x7e) || (C == '"') || (C == '\\')) {
      C = ' ';
    }

    Dst[N++] = (CHAR8)C;
  }

  while ((N > 0) && (Dst[N - 1] == ' ')) {
    N--;
  }

  Dst[N] = '\0';
}

/**
  Check that a fixed-length identify field looks like the ASCII string the
  spec mandates: every byte NUL or printable (0x20..0x7e).

  Used to gate the NVMe parse: the NVMe spec requires Sn/Mn/Fr to be
  space-padded ASCII, but some EFI_DISK_INFO_PROTOCOL producers (notably
  MdeModulePkg's NvmExpressDxe) return Identify *Namespace* data - binary -
  from Identify(). Parsing that blindly at the controller-data offsets
  would leak garbage into the JSON, so anything non-ASCII means "this is
  not controller data" and the strings are left empty (best-effort).

  @param[in] Field  Field bytes.
  @param[in] Len    Field length.

  @retval TRUE   All bytes are NUL or printable ASCII.
  @retval FALSE  At least one binary byte present.
**/
STATIC
BOOLEAN
IsAsciiField (
  IN CONST UINT8  *Field,
  IN UINTN        Len
  )
{
  UINTN  Index;

  for (Index = 0; Index < Len; Index++) {
    if ((Field[Index] != 0) &&
        ((Field[Index] < 0x20) || (Field[Index] > 0x7e)))
    {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Classify a block device by its device path: the tag the BMC shows as the
  drive's interface.

  The whole path is walked and the most specific transport wins: an NVMe
  namespace node or an SD/eMMC slot node is the leaf itself, while USB
  nodes also appear as mere hops (hubs, bridge enclosures), so USB is only
  reported when nothing more specific was seen.

  @param[in] DevicePath  The handle's device path (may be NULL).

  @return Static interface tag: "nvme", "mmc", "sata", "usb" or "blk".
**/
STATIC
CONST CHAR8 *
InterfaceTagFromDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  BOOLEAN                   HasNvme;
  BOOLEAN                   HasMmc;
  BOOLEAN                   HasSata;
  BOOLEAN                   HasUsb;

  if (DevicePath == NULL) {
    return "blk";
  }

  HasNvme = FALSE;
  HasMmc  = FALSE;
  HasSata = FALSE;
  HasUsb  = FALSE;

  for (Node = DevicePath;
       !IsDevicePathEnd (Node);
       Node = NextDevicePathNode (Node))
  {
    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    switch (DevicePathSubType (Node)) {
      case MSG_NVME_NAMESPACE_DP:
        HasNvme = TRUE;
        break;
      case MSG_SD_DP:
      case MSG_EMMC_DP:
        HasMmc = TRUE;
        break;
      case MSG_SATA_DP:
        HasSata = TRUE;
        break;
      case MSG_USB_DP:
        HasUsb = TRUE;
        break;
      default:
        break;
    }
  }

  if (HasNvme) {
    return "nvme";
  }

  if (HasMmc) {
    return "mmc";
  }

  if (HasSata) {
    return "sata";
  }

  if (HasUsb) {
    return "usb";
  }

  return "blk";
}

/**
  Best-effort vendor/product/rev strings from the handle's
  EFI_DISK_INFO_PROTOCOL. Missing protocol, unsupported command or
  unrecognized data all leave the strings empty - the JSON schema keeps
  the fields either way.

  @param[in]  Handle   The block device handle.
  @param[out] Vendor   Vendor string, DRIVE_STR_MAX bytes.
  @param[out] Product  Product string, DRIVE_STR_MAX bytes.
  @param[out] Rev      Firmware revision string, DRIVE_STR_MAX bytes.
**/
STATIC
VOID
GetDriveStrings (
  IN  EFI_HANDLE  Handle,
  OUT CHAR8       *Vendor,
  OUT CHAR8       *Product,
  OUT CHAR8       *Rev
  )
{
  EFI_STATUS              Status;
  EFI_DISK_INFO_PROTOCOL  *DiskInfo;
  UINT8                   *Data;
  UINT32                  DataSize;

  Vendor[0]  = '\0';
  Product[0] = '\0';
  Rev[0]     = '\0';

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiDiskInfoProtocolGuid,
                  (VOID **)&DiskInfo
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoNvmeInterfaceGuid)) {
    //
    // NVMe: vendor = model name (Mn), product = serial (Sn), matching the
    // field mapping u-boot's nvme.c puts in the blob ("Carry the Identify
    // controller model name in the vendor field ... product stays the
    // serial") - the BMC's inventory view was built against that layout.
    // The parse is gated on the fields being ASCII (mandated for real
    // Sn/Mn/Fr) because NvmExpressDxe's DiskInfo may return Identify
    // Namespace data instead of Controller data - see IsAsciiField().
    //
    Data = AllocateZeroPool (sizeof (NVME_ADMIN_CONTROLLER_DATA));
    if (Data == NULL) {
      return;
    }

    DataSize = sizeof (NVME_ADMIN_CONTROLLER_DATA);
    Status   = DiskInfo->Identify (DiskInfo, Data, &DataSize);
    if (!EFI_ERROR (Status) &&
        (DataSize >= NVME_ID_FR_OFFSET + NVME_ID_FR_LEN) &&
        IsAsciiField (Data + NVME_ID_SN_OFFSET, NVME_ID_SN_LEN) &&
        IsAsciiField (Data + NVME_ID_MN_OFFSET, NVME_ID_MN_LEN) &&
        IsAsciiField (Data + NVME_ID_FR_OFFSET, NVME_ID_FR_LEN))
    {
      JsonStrFromField (
        Vendor,
        DRIVE_STR_MAX,
        Data + NVME_ID_MN_OFFSET,
        NVME_ID_MN_LEN
        );
      JsonStrFromField (
        Product,
        DRIVE_STR_MAX,
        Data + NVME_ID_SN_OFFSET,
        NVME_ID_SN_LEN
        );
      JsonStrFromField (
        Rev,
        DRIVE_STR_MAX,
        Data + NVME_ID_FR_OFFSET,
        NVME_ID_FR_LEN
        );
    }

    FreePool (Data);
    return;
  }

  if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoUsbInterfaceGuid) ||
      CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoScsiInterfaceGuid))
  {
    //
    // SCSI-shaped transports: standard INQUIRY data (vendor @8 len 8,
    // product @16 len 16, revision @32 len 4).
    //
    Data = AllocateZeroPool (sizeof (EFI_SCSI_INQUIRY_DATA));
    if (Data == NULL) {
      return;
    }

    DataSize = sizeof (EFI_SCSI_INQUIRY_DATA);
    Status   = DiskInfo->Inquiry (DiskInfo, Data, &DataSize);
    if (!EFI_ERROR (Status) && (DataSize >= SCSI_INQ_MIN_LEN)) {
      JsonStrFromField (
        Vendor,
        DRIVE_STR_MAX,
        Data + SCSI_INQ_VENDOR_OFFSET,
        SCSI_INQ_VENDOR_LEN
        );
      JsonStrFromField (
        Product,
        DRIVE_STR_MAX,
        Data + SCSI_INQ_PRODUCT_OFFSET,
        SCSI_INQ_PRODUCT_LEN
        );
      JsonStrFromField (
        Rev,
        DRIVE_STR_MAX,
        Data + SCSI_INQ_REV_OFFSET,
        SCSI_INQ_REV_LEN
        );
    }

    FreePool (Data);
    return;
  }

  //
  // SD/MMC (and anything else): the SdMmc DiskInfo has no INQUIRY-style
  // id strings; leave everything empty.
  //
}

/**
  Build the BLK1 blob from every physical block device with media present
  and mirror it to the EEPROM.

  @param[in] Event    The ReadyToBoot event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
BlkInfoOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                Status;
  EFI_I2C_MASTER_PROTOCOL   *I2cMaster;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_BLOCK_IO_MEDIA        *Media;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CONST CHAR8               *IfTag;
  UINT8                     *Buf;
  UINT32                    BufSize;
  UINTN                     Pos;
  UINTN                     EntryLen;
  UINTN                     JsonLen;
  UINT32                    Count;
  UINT32                    Dropped;
  UINT32                    NvmeDev;
  UINT32                    UsbDev;
  UINT32                    MmcDev;
  UINT32                    SataDev;
  UINT32                    BlkDev;
  UINT32                    *DevCounter;
  UINT64                    DriveSize;
  BOOLEAN                   Wrote;
  CHAR8                     Vendor[DRIVE_STR_MAX];
  CHAR8                     Product[DRIVE_STR_MAX];
  CHAR8                     Rev[DRIVE_STR_MAX];
  CHAR8                     Entry[DRIVE_ENTRY_MAX];

  //
  // ReadyToBoot can be signaled again on a later boot attempt; rebuilding
  // is cheap and WriteIfChanged makes the repeat a no-op, so the event is
  // deliberately left registered - a drive hot-plugged between attempts
  // still gets reported.
  //

  BufSize = PcdGet32 (PcdBmcEepromBlkInfoSize);
  if (BufSize <= BMC_BLKINFO_HEADER_LEN + 2) {
    return;
  }

  Buf = AllocateZeroPool (BufSize);
  if (Buf == NULL) {
    return;
  }

  Pos  = BMC_BLKINFO_HEADER_LEN;
  Pos += AsciiSPrint (
           (CHAR8 *)Buf + Pos,
           BufSize - Pos,
           "{\"v\":1,\"drives\":["
           );

  Count   = 0;
  Dropped = 0;
  NvmeDev = 0;
  UsbDev  = 0;
  MmcDev  = 0;
  SataDev = 0;
  BlkDev  = 0;

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiBlockIoProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status)) {
    HandleCount = 0;
    Handles     = NULL;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status) || (BlockIo->Media == NULL)) {
      continue;
    }

    Media = BlockIo->Media;

    //
    // Physical devices only, and only ones whose media is actually there:
    // U-Boot lists probed block devices, so an empty removable slot (SD
    // reader with no card) is not part of the inventory.
    //
    if (Media->LogicalPartition || !Media->MediaPresent) {
      continue;
    }

    DevicePath = NULL;
    Status     = gBS->HandleProtocol (
                        Handles[Index],
                        &gEfiDevicePathProtocolGuid,
                        (VOID **)&DevicePath
                        );
    if (EFI_ERROR (Status)) {
      DevicePath = NULL;
    }

    IfTag = InterfaceTagFromDevicePath (DevicePath);

    //
    // Per-interface ordinal in discovery order. May differ from U-Boot's
    // devnum for the same drive; the BMC correlates loosely.
    //
    if (IfTag[0] == 'n') {
      DevCounter = &NvmeDev;
    } else if (IfTag[0] == 'm') {
      DevCounter = &MmcDev;
    } else if (IfTag[0] == 's') {
      DevCounter = &SataDev;
    } else if (IfTag[0] == 'u') {
      DevCounter = &UsbDev;
    } else {
      DevCounter = &BlkDev;
    }

    GetDriveStrings (Handles[Index], Vendor, Product, Rev);

    DriveSize = MultU64x32 (Media->LastBlock + 1, Media->BlockSize);

    EntryLen = AsciiSPrint (
                 Entry,
                 sizeof (Entry),
                 "%a{\"if\":\"%a\",\"dev\":%u,\"vendor\":\"%a\","
                 "\"product\":\"%a\",\"rev\":\"%a\","
                 "\"removable\":%u,\"size\":%Lu}",
                 (Count > 0) ? "," : "",
                 IfTag,
                 *DevCounter,
                 Vendor,
                 Product,
                 Rev,
                 Media->RemovableMedia ? 1u : 0u,
                 DriveSize
                 );

    *DevCounter += 1;

    //
    // +2 keeps room for the closing "]}" (U-Boot's drop-overflowing
    // semantics: the entry is dropped, later - smaller - ones may fit).
    //
    if (Pos + EntryLen + 2 >= BufSize) {
      Dropped++;
      continue;
    }

    CopyMem (Buf + Pos, Entry, EntryLen);
    Pos += EntryLen;
    Count++;
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  if (Dropped > 0) {
    DEBUG ((
      DEBUG_WARN,
      "BlkInfoMirror: region full, %u drive(s) dropped\n",
      Dropped
      ));
  }

  Pos += AsciiSPrint ((CHAR8 *)Buf + Pos, BufSize - Pos, "]}");

  JsonLen = Pos - BMC_BLKINFO_HEADER_LEN;
  CopyMem (Buf, BMC_BLKINFO_MAGIC, BMC_BLKINFO_MAGIC_LEN);
  Buf[4] = (UINT8)(JsonLen & 0xff);
  Buf[5] = (UINT8)((JsonLen >> 8) & 0xff);

  Status = BmcEepromLocateI2cMaster (&I2cMaster);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "BlkInfoMirror: no I2C master, inventory not mirrored (%r)\n",
      Status
      ));
    FreePool (Buf);
    return;
  }

  Wrote  = FALSE;
  Status = BmcEepromWriteIfChanged (
             I2cMaster,
             PcdGet32 (PcdBmcEepromBlkInfoOffset),
             Pos,
             Buf,
             &Wrote
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BlkInfoMirror: EEPROM write failed: %r\n", Status));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "BlkInfoMirror: %u drive(s), %u bytes at 0x%x%a\n",
      Count,
      (UINT32)Pos,
      PcdGet32 (PcdBmcEepromBlkInfoOffset),
      Wrote ? "" : " (unchanged)"
      ));
  }

  FreePool (Buf);
}

/**
  Entry point: register the ReadyToBoot handler.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  Event registered.
  @return              Error from CreateEventEx otherwise.
**/
EFI_STATUS
EFIAPI
BlkInfoMirrorEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiCreateEventReadyToBootEx (
           TPL_CALLBACK,
           BlkInfoOnReadyToBoot,
           NULL,
           &mReadyToBootEvent
           );
}

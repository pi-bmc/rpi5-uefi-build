/** @file
  RpiRedfishSyncDxe - definitions for the host-side Redfish Host Interface
  client.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RPI_REDFISH_SYNC_DXE_H_
#define RPI_REDFISH_SYNC_DXE_H_

#include <Uefi.h>

#include <IndustryStandard/Atapi.h>
#include <IndustryStandard/Nvme.h>
#include <IndustryStandard/SmBios.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/RedfishHttpLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskInfo.h>
#include <Protocol/EdkIIRedfishConfigHandler.h>
#include <Protocol/NvmExpressPassthru.h>
#include <Protocol/Smbios.h>

#include <RedfishServiceData.h>

//
// The managed system this firmware reports itself as. The BMC (nanokvm-app)
// models exactly one ComputerSystem ("1"), so the URIs are fixed rather than
// walked from the service root - one fewer round trip on the boot path, and
// the BMC is a known peer on a point-to-point link.
//
#define RPI_REDFISH_SERVICE_ROOT_URI  L"/redfish/v1/"
#define RPI_REDFISH_SYSTEM_URI        L"/redfish/v1/Systems/1"
#define RPI_REDFISH_MEMORY_URI        L"/redfish/v1/Systems/1/Memory"
#define RPI_REDFISH_PROCESSORS_URI    L"/redfish/v1/Systems/1/Processors"
#define RPI_REDFISH_DRIVES_URI        L"/redfish/v1/Systems/1/Storage/1/Drives"
#define RPI_REDFISH_THERMAL_URI       L"/redfish/v1/Chassis/1/Thermal"

//
// Thermal telemetry cadence while the firmware phase lasts (BDS wait, Setup,
// the shell). 10 s keeps the BMC's view fresh without loading the link.
//
#define RPI_REDFISH_THERMAL_PERIOD    100000000ULL   // 10 s, in 100 ns units

//
// Boot progress state reported at the point the config handler runs: DXE is
// complete and BDS is selecting a boot option. This is the DSP2046
// BootProgressTypes value for that moment.
//
#define RPI_REDFISH_BOOT_PROGRESS  "SystemHardwareInitializationComplete"

//
// Upper bound on the JSON body we build. The payload is a fixed set of short
// SMBIOS-derived strings; 1 KiB leaves generous headroom and keeps the
// allocation off the boot path's critical size budget.
//
#define RPI_REDFISH_JSON_MAX  1024

//
// The first request over the host interface routinely fails with EFI_NO_MEDIA:
// the USB-net stack only learns the link is up by catching a CDC
// NETWORK_CONNECTION notification, and the one the BMC's gadget sends during
// enumeration predates this driver. A handful of retries a couple of seconds
// apart meets the BMC's periodic link re-announcement, while bounding the
// delay this adds to a boot where the BMC never answers. (The 0100 UsbNetwork
// patch makes CableDetect sticky for the same reason; the retry remains as
// belt and braces for a slow BMC HTTP listener.)
//
#define RPI_REDFISH_MEDIA_RETRIES      8
#define RPI_REDFISH_MEDIA_RETRY_STALL  2000000   // 2 s, in microseconds

//
// Maximum length kept for any single SMBIOS-sourced string. SMBIOS strings are
// unbounded in principle; truncating defensively keeps a malformed table from
// overflowing the JSON buffer.
//
#define RPI_REDFISH_STR_MAX  64

typedef struct {
  CHAR8      BiosVersion[RPI_REDFISH_STR_MAX];
  CHAR8      Manufacturer[RPI_REDFISH_STR_MAX];
  CHAR8      Model[RPI_REDFISH_STR_MAX];
  CHAR8      SerialNumber[RPI_REDFISH_STR_MAX];
  CHAR8      Uuid[37];                           // 36 chars + NUL
  BOOLEAN    UuidValid;
} RPI_REDFISH_HOST_INVENTORY;

/**
  Collect host inventory (BIOS version, system manufacturer/model/serial/UUID)
  from the SMBIOS tables this firmware published.

  @param[out] Inventory  Receives the collected inventory. Fields that cannot be
                         resolved are left as empty strings.

  @retval EFI_SUCCESS    Inventory was collected (possibly partially).
  @retval EFI_NOT_FOUND  The SMBIOS protocol is not available.
**/
EFI_STATUS
RpiRedfishCollectInventory (
  OUT RPI_REDFISH_HOST_INVENTORY  *Inventory
  );

/**
  Build the ComputerSystem PATCH body reporting this host to the BMC.

  @param[in]  Inventory  Host inventory to report.
  @param[out] Json       Receives an allocated ASCII JSON body. Caller frees with
                         FreePool().

  @retval EFI_SUCCESS           Body was built.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RpiRedfishBuildSystemPatch (
  IN  RPI_REDFISH_HOST_INVENTORY  *Inventory,
  OUT CHAR8                       **Json
  );

//
// Memory devices reported. The Pi 5's LPDDR4X is soldered and shows up as a
// single SMBIOS type 17 record; the bound is generous so a table with more
// entries is truncated rather than overrunning.
//
#define RPI_REDFISH_MEMORY_MAX  8

//
// One populated memory device, as SMBIOS type 17 describes it, reduced to the
// Redfish Memory v1_7_1 properties the BMC stores.
//
typedef struct {
  CHAR8          DeviceLocator[RPI_REDFISH_STR_MAX];
  CHAR8          BankLocator[RPI_REDFISH_STR_MAX];
  CHAR8          Manufacturer[RPI_REDFISH_STR_MAX];
  CHAR8          SerialNumber[RPI_REDFISH_STR_MAX];
  CHAR8          PartNumber[RPI_REDFISH_STR_MAX];
  CONST CHAR8    *MemoryDeviceType;               // "DDR4", "LPDDR4_SDRAM", ... or NULL
  CONST CHAR8    *BaseModuleType;                 // "SO_DIMM", "UDIMM", ... or NULL
  UINT32         CapacityMiB;
  UINT32         OperatingSpeedMhz;               // configured speed
  UINT32         RatedSpeedMhz;                   // the module's own rating
  UINT16         DataWidthBits;
  UINT16         BusWidthBits;
} RPI_REDFISH_MEMORY_MODULE;

/**
  Collect populated memory devices from the SMBIOS type 17 records this
  firmware published.

  Unpopulated slots (Size == 0) and slots of unknown size are skipped: SMBIOS
  emits a record per socket whether or not it is filled, and reporting an empty
  socket as a Memory resource would claim hardware that is not there.

  @param[out] Modules  Receives the populated modules.
  @param[in]  Max      Capacity of Modules.
  @param[out] Count    Receives the number written.

  @retval EFI_SUCCESS    Zero or more modules were collected.
  @retval EFI_NOT_FOUND  The SMBIOS protocol is not available.
**/
EFI_STATUS
RpiRedfishCollectMemory (
  OUT RPI_REDFISH_MEMORY_MODULE  *Modules,
  IN  UINTN                      Max,
  OUT UINTN                      *Count
  );

/**
  Build the Memory POST body for one module.

  @param[in]  Module  Module to describe.
  @param[out] Json    Receives an allocated ASCII JSON body. Caller frees with
                      FreePool().

  @retval EFI_SUCCESS           Body was built.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RpiRedfishBuildMemoryPost (
  IN  RPI_REDFISH_MEMORY_MODULE  *Module,
  OUT CHAR8                      **Json
  );

//
// Processor sockets reported. The Pi 5 is single-socket; the bound leaves
// headroom and keeps a malformed table from overrunning.
//
#define RPI_REDFISH_PROCESSOR_MAX  4

//
// One populated processor socket, as SMBIOS type 4 describes it, reduced to
// the Redfish Processor v1_16_0 properties the BMC stores.
//
typedef struct {
  CHAR8          Socket[RPI_REDFISH_STR_MAX];
  CHAR8          Manufacturer[RPI_REDFISH_STR_MAX];
  CHAR8          Model[RPI_REDFISH_STR_MAX];
  CHAR8          SerialNumber[RPI_REDFISH_STR_MAX];
  CHAR8          PartNumber[RPI_REDFISH_STR_MAX];
  CHAR8          IdRegisters[19];                  // "0x" + 16 hex digits + NUL
  CONST CHAR8    *ProcessorType;                   // "CPU", "GPU", ... or NULL
  UINT32         MaxSpeedMhz;
  UINT32         OperatingSpeedMhz;                // current, not rated
  UINT32         TotalCores;
  UINT32         TotalEnabledCores;
  UINT32         TotalThreads;
  BOOLEAN        Enabled;                          // type 4 CPU Status says enabled
} RPI_REDFISH_PROCESSOR;

/**
  Collect populated processor sockets from the SMBIOS type 4 records this
  firmware published.

  Unpopulated sockets are skipped on the same reasoning as memory: SMBIOS
  emits a record per socket whether or not it is filled, and reporting an
  empty one would claim hardware that is not there.

  @param[out] Processors  Receives the populated sockets.
  @param[in]  Max         Capacity of Processors.
  @param[out] Count       Receives the number written.

  @retval EFI_SUCCESS    Zero or more processors were collected.
  @retval EFI_NOT_FOUND  The SMBIOS protocol is not available.
**/
EFI_STATUS
RpiRedfishCollectProcessors (
  OUT RPI_REDFISH_PROCESSOR  *Processors,
  IN  UINTN                  Max,
  OUT UINTN                  *Count
  );

/**
  Build the Processor POST body for one socket.

  @param[in]  Processor  Processor to describe.
  @param[out] Json       Receives an allocated ASCII JSON body. Caller frees
                         with FreePool().

  @retval EFI_SUCCESS           Body was built.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RpiRedfishBuildProcessorPost (
  IN  RPI_REDFISH_PROCESSOR  *Processor,
  OUT CHAR8                  **Json
  );

//
// Physical drives reported. One NVMe SSD on the FPC PCIe slot is the expected
// population; the bound leaves headroom.
//
#define RPI_REDFISH_DRIVE_MAX  8

//
// One physical drive, reduced to the Redfish Drive properties the BMC stores.
//
// This cannot come from SMBIOS: DSP0134 defines no structure type for a disk,
// so the boot-services protocol stack (DiskInfo / NVMe pass-thru) is the only
// firmware-native source. The same data used to reach the BMC as the blkinfo
// EEPROM region (BlkInfoMirrorDxe); these POSTs are that mirror's successor.
//
typedef struct {
  CHAR8          Model[RPI_REDFISH_STR_MAX];
  CHAR8          SerialNumber[RPI_REDFISH_STR_MAX];
  CHAR8          Revision[RPI_REDFISH_STR_MAX];    // firmware revision
  CONST CHAR8    *Protocol;                        // "NVMe" or "SATA"
  CONST CHAR8    *MediaType;                       // "SSD", "HDD", or NULL when unknown
  UINT64         CapacityBytes;                    // 0 = unknown
} RPI_REDFISH_DRIVE;

/**
  Collect the local drives BDS has connected, via EFI_DISK_INFO_PROTOCOL.

  Only reports what is already connected: this runs at TPL_CALLBACK, where
  ConnectController is not permitted, so a drive BDS has not brought up is
  invisible here. The exchange runs during BdsWait, after ConnectAll, so that
  is every drive.

  @param[out] Drives  Receives the drives.
  @param[in]  Max     Capacity of Drives.
  @param[out] Count   Receives the number written.

  @retval EFI_SUCCESS  Zero or more drives were collected.
**/
EFI_STATUS
RpiRedfishCollectDrives (
  OUT RPI_REDFISH_DRIVE  *Drives,
  IN  UINTN              Max,
  OUT UINTN              *Count
  );

/**
  Build the Drive POST body for one drive.

  @param[in]  Drive  Drive to describe.
  @param[out] Json   Receives an allocated ASCII JSON body. Caller frees with
                     FreePool().

  @retval EFI_SUCCESS           Body was built.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RpiRedfishBuildDrivePost (
  IN  RPI_REDFISH_DRIVE  *Drive,
  OUT CHAR8              **Json
  );

#endif // RPI_REDFISH_SYNC_DXE_H_

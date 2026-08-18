/** @file
  RpiRedfishSyncDxe - the host-side client of the Redfish Host Interface.

  RedfishPkg gets the host as far as *discovering* the BMC: RedfishHostInterfaceDxe
  publishes the SMBIOS type 42 record, RedfishDiscoverDxe correlates it with the
  CDC-NCM NIC and configures REST EX, and RedfishConfigHandlerDriver signals that
  a service is available. Nothing then talks to it - the drivers that would
  (edk2-redfish-client's feature layer) need a newer edk2 than the pinned
  NumberOneGit fork, and the JetKVM/NUC deployment proved a small purpose-built
  client covers the whole point-to-point exchange anyway.

  This driver is that consumer, ported from nuc-bios-build's NucRedfishSyncDxe.
  It produces EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL, which
  RedfishConfigHandlerDriver invokes with the discovered service, and then
  performs the exchange that replaces the old I2C shared-EEPROM sync:

    1. GET  /redfish/v1/            -- proves the link and the service are live.
    2. PATCH /redfish/v1/Systems/1  -- reports this host's identity (SMBIOS type
                                       0/1) and boot progress to the BMC
                                       (successor of SmbiosEepromMirrorDxe).
    2b-2d. POST Processors /        -- inventory the BMC cannot see in band
           Memory / Drives             (successor of BlkInfoMirrorDxe).
    2e. PATCH+GET Chassis/1/Thermal -- SoC temperature + fan state up, BMC fan
                                       steering down (RPI_FAN_PROTOCOL); then
                                       every 10 s for as long as BDS lasts.
    3. GET  /redfish/v1/Systems/1   -- reads back the BMC's requested one-time
                                       boot override, acknowledges it, and boots
                                       the matching option in this same boot
                                       (successor of the EEPROM BootNext writes).

  Everything here is fail-open: an unreachable or unhappy BMC must never stop the
  host from booting, so every failure is logged and returns EFI_SUCCESS.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RpiRedfishSyncDxe.h"

#include <Library/DevicePathLib.h>
#include <Library/JsonLib.h>
#include <Library/UefiBootManagerLib.h>

#include <Protocol/RpiFan.h>

STATIC EFI_HANDLE       mImageHandle    = NULL;
STATIC BOOLEAN          mSyncDone       = FALSE;
STATIC REDFISH_SERVICE  mThermalService = NULL;
STATIC EFI_EVENT        mThermalTimer   = NULL;

/**
  Log an HTTP status code alongside the URI it came from.

  Redfish traffic is invisible from the OS once BDS hands over, so the serial
  log is the only record of what happened. These lines are DEBUG_ERROR rather
  than DEBUG_MANAGEABILITY deliberately: they are the host-side evidence that
  the exchange took place, and must survive whatever PcdDebugPrintErrorLevel
  is set to.

  @param[in] What      Short label for the operation.
  @param[in] Uri       URI operated on.
  @param[in] Status    EFI status of the call.
  @param[in] Response  Response whose status code should be reported, may be NULL.
**/
STATIC
VOID
LogResult (
  IN CONST CHAR8       *What,
  IN EFI_STRING        Uri,
  IN EFI_STATUS        Status,
  IN REDFISH_RESPONSE  *Response
  )
{
  UINTN  Code;

  Code = 0;
  if ((Response != NULL) && (Response->StatusCode != NULL)) {
    Code = (UINTN)*Response->StatusCode;
  }

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: %a %s -> %r (HTTP status enum %d)\n",
    What,
    Uri,
    Status,
    Code
    ));
}

/**
  Report whether a boot option's device path contains a node of the given type.

  @param[in] Option   Boot option to inspect.
  @param[in] Type     Device path type to look for.
  @param[in] SubType  Device path sub-type to look for.

  @retval TRUE   A matching node is present.
  @retval FALSE  No matching node, or the option has no device path.
**/
STATIC
BOOLEAN
OptionHasNode (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option,
  IN UINT8                         Type,
  IN UINT8                         SubType
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  if ((Option == NULL) || (Option->FilePath == NULL)) {
    return FALSE;
  }

  for (Node = Option->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == Type) && (DevicePathSubType (Node) == SubType)) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Report whether a boot option boots from a real network interface.

  Requires a MAC node, which is what makes it a network option, and rejects
  anything reached through USB. On this board the only USB NIC is the BMC's
  CDC-NCM host interface -- a DSP0270 management link that must never be
  selected as a boot target. The RP1 GEM onboard RJ45 (Rp1GemDxe's PXE
  option) is what a "Pxe" override should land on.

  @param[in] Option  Boot option to inspect.

  @retval TRUE   The option boots from a non-USB network interface.
  @retval FALSE  It does not.
**/
STATIC
BOOLEAN
OptionIsNetworkBoot (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  BOOLEAN                   HasMac;

  if ((Option == NULL) || (Option->FilePath == NULL)) {
    return FALSE;
  }

  HasMac = FALSE;

  for (Node = Option->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    if (DevicePathSubType (Node) == MSG_USB_DP) {
      return FALSE;
    }

    if (DevicePathSubType (Node) == MSG_MAC_ADDR_DP) {
      HasMac = TRUE;
    }
  }

  return HasMac;
}

/**
  Report whether a boot option boots from local block storage.

  "Hdd" cannot simply look for an HD() partition node. The boot options visible
  at the point this driver runs predate EfiBootManagerRefreshAllBootOption(),
  so auto-created disk candidates may name the device (an NVMe namespace, the
  SD card) with no partition node - BDS expands them to find the ESP later.

  So match either shape: an HD() node when the option is partition-anchored, or
  a storage messaging node when it names the device. Deliberately no bare
  MSG_USB_DP - that would also match the BMC's own CDC-NCM NIC; real USB mass
  storage carries HD() once enumerated.

  @param[in] Option  Boot option to inspect.

  @retval TRUE   The option boots from local block storage.
  @retval FALSE  It does not.
**/
STATIC
BOOLEAN
OptionIsDiskBoot (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  if ((Option == NULL) || (Option->FilePath == NULL)) {
    return FALSE;
  }

  if (OptionHasNode (Option, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP)) {
    return TRUE;
  }

  for (Node = Option->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    switch (DevicePathSubType (Node)) {
      case MSG_NVME_NAMESPACE_DP:
      case MSG_SATA_DP:
      case MSG_ATAPI_DP:
      case MSG_SCSI_DP:
      case MSG_EMMC_DP:
      case MSG_SD_DP:
      case MSG_UFS_DP:
        return TRUE;
      default:
        break;
    }
  }

  return FALSE;
}

/**
  Find the boot option matching a Redfish BootSourceOverrideTarget.

  Redfish names boot *classes* ("Pxe", "Hdd", "BiosSetup"); UEFI has numbered
  Boot#### options. The mapping is done by scanning the boot options this
  firmware already built and matching on the attributes BDS itself uses --
  LOAD_OPTION_CATEGORY_APP for the setup UI, MAC-node device paths for network
  boot, and storage device path nodes for local disks -- rather than on
  description text, which is localised and unstable.

  This only matches; it has no side effects, so the caller can acknowledge the
  request to the BMC before doing anything that might not return. See
  ApplyMatchedOption().

  On success the caller owns *Options and must release it with
  EfiBootManagerFreeLoadOptions().

  @param[in]  Target       Redfish BootSourceOverrideTarget value.
  @param[out] Options      Boot option array the match indexes into.
  @param[out] OptionCount  Number of entries in *Options.
  @param[out] Match        Index of the matching option.

  @retval EFI_SUCCESS    A boot option matched.
  @retval EFI_NOT_FOUND  No boot option matched the requested class.
**/
STATIC
EFI_STATUS
FindBootOverrideOption (
  IN  CONST CHAR8                   *Target,
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  **Options,
  OUT UINTN                         *OptionCount,
  OUT UINTN                         *Match
  )
{
  UINTN    Index;
  BOOLEAN  Found;

  *Options     = NULL;
  *OptionCount = 0;
  *Match       = 0;

  if ((Target == NULL) || (AsciiStrCmp (Target, "None") == 0)) {
    return EFI_NOT_FOUND;
  }

  *Options = EfiBootManagerGetLoadOptions (OptionCount, LoadOptionTypeBoot);
  if ((*Options == NULL) || (*OptionCount == 0)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no boot options to override\n"));
    return EFI_NOT_FOUND;
  }

  Found = FALSE;

  //
  // Dump the candidates. An override that does not apply is otherwise
  // indistinguishable from one that was never staged, and the boot option set
  // this early in BDS is not what `efibootmgr` shows from the OS -- it predates
  // EfiBootManagerRefreshAllBootOption(). Cheap: this only runs when the BMC
  // has actually staged a target.
  //
  for (Index = 0; Index < *OptionCount; Index++) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync:   Boot%04x \"%s\" attr=0x%x%a%a\n",
      (*Options)[Index].OptionNumber,
      ((*Options)[Index].Description != NULL) ? (*Options)[Index].Description : L"",
      (*Options)[Index].Attributes,
      OptionHasNode (&(*Options)[Index], MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP) ? " [HD]" : "",
      OptionHasNode (&(*Options)[Index], MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP) ? " [FvFile]" : ""
      ));
  }

  for (Index = 0; Index < *OptionCount; Index++) {
    if (AsciiStrCmp (Target, "BiosSetup") == 0) {
      //
      // The setup UI (UiApp) is the boot option BDS tags as an application.
      //
      Found = (((*Options)[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_APP);
    } else if (AsciiStrCmp (Target, "Pxe") == 0) {
      //
      // Network boot is BDS's own "PXEv4 (MAC:...)" option for the onboard
      // RJ45, auto-created once Rp1GemDxe publishes SNP for it. Match the
      // device path rather than the description, which is localised.
      //
      Found = OptionIsNetworkBoot (&(*Options)[Index]);
    } else if (AsciiStrCmp (Target, "Hdd") == 0) {
      //
      // Local block storage, in whichever shape BDS has it at this point.
      // See OptionIsDiskBoot().
      //
      Found = OptionIsDiskBoot (&(*Options)[Index]);
    }

    if (Found) {
      *Match = Index;
      break;
    }
  }

  if (!Found) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no boot option matches target '%a'\n", Target));
    EfiBootManagerFreeLoadOptions (*Options, *OptionCount);
    *Options     = NULL;
    *OptionCount = 0;
    return EFI_NOT_FOUND;
  }

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: boot override '%a' -> Boot%04x \"%s\"\n",
    Target,
    (*Options)[*Match].OptionNumber,
    (*Options)[*Match].Description != NULL ? (*Options)[*Match].Description : L"(no description)"
    ));

  return EFI_SUCCESS;
}

/**
  Act on a matched boot override by staging BootNext and resetting.

  Deliberately NOT EfiBootManagerBoot() in this boot, although the NUC
  original did that when TPL allowed. EfiBootManagerBoot() signals
  ReadyToBoot before StartImage (BmBoot.c), and this driver runs inside
  RedfishConfigHandlerDriver's service-discovered callback -- so a
  same-boot boot fires ReadyToBoot on this callback's own stack while
  Redfish discovery is still in flight and the edk2-redfish-client feature
  drivers begin provisioning over the same interface. Observed on hardware
  2026-08-17 with a "BiosSetup" override: the nested client exchange
  called through a Redfish service freed underneath it -- PC alignment
  fault at 0xAFAFAFAFAFAFAFAF (the DEBUG freed-pool fill), with
  ComputerSystemDxe -> RedfishHttpDxe innermost and Ip6Dxe still inside
  RedfishDiscoverDxe -- a black screen before the setup UI ever drew.
  (The NUC's same-boot path predates its client being compiled in.)

  Staging BootNext and resetting serializes everything: BdsEntry consumes
  BootNext at the top of the next boot, long before discovery or the
  feature layer exist, and boots the target with consoles ready. Costs one
  reboot; the Pi resets in seconds.

  Safe to loop on: the BMC's override was cleared and acknowledged before
  this point (HandleBootOverride refuses to get here otherwise), so the
  next boot finds nothing staged and consumes BootNext normally.

  Going through gRT->ResetSystem (never a bare reset library call) lets
  reset notifications run first. EfiResetCold reaches PSCI SYSTEM_RESET in
  TF-A, which the Pi 5 handles cleanly.

  @param[in] Option  The matched boot option. Not freed here.

  @retval EFI_SUCCESS  Never actually returned: the reset does not come back.
  @return              Error from SetVariable if BootNext could not be staged.
**/
STATIC
EFI_STATUS
ApplyMatchedOption (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  EFI_STATUS  Status;
  UINT16      BootNext;

  BootNext = (UINT16)Option->OptionNumber;
  Status   = gRT->SetVariable (
                    L"BootNext",
                    &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (BootNext),
                    &BootNext
                    );

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: staged BootNext=Boot%04x - %r\n",
    BootNext,
    Status
    ));

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: resetting to consume BootNext\n"));
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);

  //
  // Not reached.
  //
  CpuDeadLoop ();
  return EFI_SUCCESS;
}

/**
  Read the BMC's requested boot override from a ComputerSystem payload and apply
  it, then tell the BMC it has been consumed.

  Only "Once" overrides are honoured. A "Continuous" override would re-apply on
  every boot with no way for the host to clear it, which is a boot loop waiting
  to happen on a link whose only operator is the BMC.

  @param[in] Service   Redfish service to acknowledge through.
  @param[in] Response  Response holding the ComputerSystem payload.
**/
STATIC
VOID
HandleBootOverride (
  IN REDFISH_SERVICE   Service,
  IN REDFISH_RESPONSE  *Response
  )
{
  EDKII_JSON_VALUE              Root;
  EDKII_JSON_VALUE              Boot;
  EDKII_JSON_VALUE              Value;
  CONST CHAR8                   *Target;
  CONST CHAR8                   *Enabled;
  EFI_STATUS                    Status;
  REDFISH_RESPONSE              AckResponse;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         OptionCount;
  UINTN                         Match;

  if ((Response == NULL) || (Response->Payload == NULL)) {
    return;
  }

  Root = RedfishJsonInPayload (Response->Payload);
  if ((Root == NULL) || !JsonValueIsObject (Root)) {
    return;
  }

  Boot = JsonObjectGetValue (JsonValueGetObject (Root), "Boot");
  if ((Boot == NULL) || !JsonValueIsObject (Boot)) {
    return;
  }

  Value  = JsonObjectGetValue (JsonValueGetObject (Boot), "BootSourceOverrideTarget");
  Target = (Value == NULL) ? NULL : JsonValueGetAsciiString (Value);

  Value   = JsonObjectGetValue (JsonValueGetObject (Boot), "BootSourceOverrideEnabled");
  Enabled = (Value == NULL) ? NULL : JsonValueGetAsciiString (Value);

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: BMC boot override target='%a' enabled='%a'\n",
    (Target == NULL) ? "(unset)" : Target,
    (Enabled == NULL) ? "(unset)" : Enabled
    ));

  if ((Enabled == NULL) || (AsciiStrCmp (Enabled, "Once") != 0)) {
    return;
  }

  if (EFI_ERROR (FindBootOverrideOption (Target, &Options, &OptionCount, &Match))) {
    return;
  }

  //
  // Acknowledge BEFORE booting, not after. ApplyMatchedOption() boots the target
  // in this boot where it can, and a successful boot never returns -- so an
  // acknowledgement placed after it would never be sent on exactly the runs that
  // worked. The BMC would still show the override staged, the next boot would
  // apply it again, and the host would be pinned to that target forever.
  //
  // Ordering it this way also keeps the override genuinely one-shot if the host
  // resets between here and the boot attempt.
  //
  ZeroMem (&AckResponse, sizeof (AckResponse));
  Status = RedfishHttpPatchResource (
             Service,
             RPI_REDFISH_SYSTEM_URI,
             "{\"Boot\":{\"BootSourceOverrideEnabled\":\"Disabled\"}}",
             &AckResponse
             );
  LogResult ("PATCH(clear-override)", RPI_REDFISH_SYSTEM_URI, Status, &AckResponse);
  RedfishHttpFreeResponse (&AckResponse);

  if (EFI_ERROR (Status)) {
    //
    // Refuse to boot a request we could not acknowledge: the BMC still has it
    // staged, so honouring it now would repeat on every subsequent boot.
    //
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: not applying override, BMC did not accept the clear - %r\n",
      Status
      ));
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
    return;
  }

  ApplyMatchedOption (&Options[Match]);

  EfiBootManagerFreeLoadOptions (Options, OptionCount);
}

/**
  Report the host's processors to the BMC's Processor collection.

  One POST per populated socket, keyed on the SMBIOS socket designation so a
  later boot updates the existing member rather than accumulating duplicates.
  Everything reported comes from SMBIOS type 4, including the clock rates
  PlatformSmbiosDxe reads from the VPU mailbox -- the BMC has no in-band way
  to learn any of it.

  The BMC (nanokvm-app) serves Processors read-only today and has no POST
  handler, so until one lands these log an HTTP error and move on, exactly as
  the memory and drive POSTs did before their handlers arrived. Fail-open by
  design.

  @param[in] Service  The Redfish service to report to.
**/
STATIC
VOID
ReportProcessors (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_REDFISH_PROCESSOR  Processors[RPI_REDFISH_PROCESSOR_MAX];
  REDFISH_RESPONSE       Response;
  EFI_STATUS             Status;
  UINTN                  Count;
  UINTN                  Index;
  CHAR8                  *Body;

  Status = RpiRedfishCollectProcessors (Processors, RPI_REDFISH_PROCESSOR_MAX, &Count);
  if (EFI_ERROR (Status) || (Count == 0)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no processors to report - %r\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Body   = NULL;
    Status = RpiRedfishBuildProcessorPost (&Processors[Index], &Body);
    if (EFI_ERROR (Status) || (Body == NULL)) {
      continue;
    }

    ZeroMem (&Response, sizeof (Response));
    Status = RedfishHttpPostResource (Service, RPI_REDFISH_PROCESSORS_URI, Body, &Response);
    LogResult ("POST", RPI_REDFISH_PROCESSORS_URI, Status, &Response);
    RedfishHttpFreeResponse (&Response);

    FreePool (Body);
  }

  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: reported %d processor(s)\n", Count));
}

/**
  Report the host's memory devices to the BMC's Memory collection.

  One POST per module, keyed on DeviceLocator so a later boot updates the
  existing member rather than accumulating duplicates. The BMC (nanokvm-app)
  does not accept these POSTs yet - it currently serves Memory from its own
  sources - so until it grows the handler these log an HTTP error and move
  on. Fail-open by design.

  @param[in] Service  The Redfish service to report to.
**/
STATIC
VOID
ReportMemory (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_REDFISH_MEMORY_MODULE  Modules[RPI_REDFISH_MEMORY_MAX];
  REDFISH_RESPONSE           Response;
  EFI_STATUS                 Status;
  UINTN                      Count;
  UINTN                      Index;
  CHAR8                      *Body;

  Status = RpiRedfishCollectMemory (Modules, RPI_REDFISH_MEMORY_MAX, &Count);
  if (EFI_ERROR (Status) || (Count == 0)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no memory devices to report - %r\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Body   = NULL;
    Status = RpiRedfishBuildMemoryPost (&Modules[Index], &Body);
    if (EFI_ERROR (Status) || (Body == NULL)) {
      continue;
    }

    ZeroMem (&Response, sizeof (Response));
    Status = RedfishHttpPostResource (Service, RPI_REDFISH_MEMORY_URI, Body, &Response);
    LogResult ("POST", RPI_REDFISH_MEMORY_URI, Status, &Response);
    RedfishHttpFreeResponse (&Response);

    FreePool (Body);
  }

  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: reported %d memory device(s)\n", Count));
}

/**
  Report the host's local drives to the BMC's Storage subsystem.

  Same shape as ReportMemory -- one POST per drive, keyed on SerialNumber --
  but sourced from the boot-services stack (DiskInfo / NVMe pass-thru)
  instead of SMBIOS, which has no structure type for a disk. This is the
  Redfish successor of the blkinfo EEPROM region BlkInfoMirrorDxe used to
  write; the BMC's storage.go serves host drives and will source from these
  POSTs once its handler lands.

  @param[in] Service  The Redfish service to report to.
**/
STATIC
VOID
ReportDrives (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_REDFISH_DRIVE  Drives[RPI_REDFISH_DRIVE_MAX];
  REDFISH_RESPONSE   Response;
  EFI_STATUS         Status;
  UINTN              Count;
  UINTN              Index;
  CHAR8              *Body;

  Status = RpiRedfishCollectDrives (Drives, RPI_REDFISH_DRIVE_MAX, &Count);
  if (EFI_ERROR (Status) || (Count == 0)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no drives to report - %r\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Body   = NULL;
    Status = RpiRedfishBuildDrivePost (&Drives[Index], &Body);
    if (EFI_ERROR (Status) || (Body == NULL)) {
      continue;
    }

    ZeroMem (&Response, sizeof (Response));
    Status = RedfishHttpPostResource (Service, RPI_REDFISH_DRIVES_URI, Body, &Response);
    LogResult ("POST", RPI_REDFISH_DRIVES_URI, Status, &Response);
    RedfishHttpFreeResponse (&Response);

    FreePool (Body);
  }

  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: reported %d drive(s)\n", Count));
}

/**
  Publish the SoC temperature and fan state to the BMC's Thermal resource.

  Redfish puts thermal under Chassis, so this PATCHes Chassis/1/Thermal in
  the Thermal-schema shape (Temperatures + Fans arrays) with the fan's
  commanded level in an Oem block. The BMC (nanokvm-app) has no Chassis
  handler yet; until it lands these log an HTTP error and move on,
  fail-open like the Memory/Drives POSTs.

  @param[in] Service  The Redfish service to report to.

  @retval EFI_SUCCESS    The PATCH was accepted.
  @retval EFI_NOT_FOUND  No fan protocol (ActiveCoolerDxe absent).
  @return                Transport or HTTP error from the PATCH.
**/
STATIC
EFI_STATUS
ReportThermal (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_FAN_PROTOCOL  *Fan;
  RPI_FAN_INFO      Info;
  REDFISH_RESPONSE  Response;
  EFI_STATUS        Status;
  CHAR8             Body[RPI_REDFISH_JSON_MAX];
  CHAR8             TempField[64];
  UINT32            Percent;
  INT32             Whole;
  INT32             Milli;

  Status = gBS->LocateProtocol (&gRpiFanProtocolGuid, NULL, (VOID **)&Fan);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = Fan->GetInfo (Fan, &Info);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_READY)) {
    return Status;
  }

  //
  // An invalid sensor omits the Temperatures array rather than reporting a
  // made-up number the BMC would chart.
  //
  TempField[0] = '\0';
  if (Info.TemperatureValid) {
    Whole = Info.TemperatureMilliCelsius / 1000;
    Milli = Info.TemperatureMilliCelsius % 1000;
    if (Milli < 0) {
      Milli = -Milli;
    }

    AsciiSPrint (
      TempField,
      sizeof (TempField),
      "\"Temperatures\":[{\"MemberId\":\"SoC\",\"ReadingCelsius\":%a%d.%03d}],",
      ((Info.TemperatureMilliCelsius < 0) && (Whole == 0)) ? "-" : "",
      Whole,
      Milli
      );
  }

  Percent = (Info.Duty255 * 100) / 255;
  AsciiSPrint (
    Body,
    sizeof (Body),
    "{%a\"Fans\":[{\"MemberId\":\"ActiveCooler\",\"Reading\":%d,"
    "\"ReadingUnits\":\"Percent\","
    "\"Oem\":{\"PiBmc\":{\"Level\":%d,\"MaxLevel\":%d,\"OverrideActive\":%a}}}]}",
    TempField,
    Percent,
    Info.Level,
    Info.MaxLevel,
    Info.OverrideActive ? "true" : "false"
    );

  ZeroMem (&Response, sizeof (Response));
  Status = RedfishHttpPatchResource (Service, RPI_REDFISH_THERMAL_URI, Body, &Response);
  LogResult ("PATCH", RPI_REDFISH_THERMAL_URI, Status, &Response);
  RedfishHttpFreeResponse (&Response);

  return Status;
}

/**
  Read back the BMC's fan steering from the Thermal resource and apply it.

  Wire contract: the BMC stages Oem.PiBmc.FanOverrideLevel (integer,
  0..MaxLevel) in its Chassis/1/Thermal representation to pin the fan, and
  removes the property (or sets it to a non-integer, e.g. null or "Auto")
  to release it. The override is applied through RPI_FAN_PROTOCOL, so it
  is volatile and loses to nothing except a newer override.

  A failed GET changes nothing: a comms error is indistinguishable from a
  BMC that has no opinion, and dropping an active override on a glitch
  would let the fan loop fight the BMC.

  @param[in] Service  The Redfish service to poll.

  @retval EFI_SUCCESS  The GET succeeded (override applied or released).
  @return              Transport or HTTP error from the GET.
**/
STATIC
EFI_STATUS
PollFanOverride (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_FAN_PROTOCOL  *Fan;
  REDFISH_RESPONSE  Response;
  EFI_STATUS        Status;
  EDKII_JSON_VALUE  Root;
  EDKII_JSON_VALUE  Value;
  INT64             Level;

  Status = gBS->LocateProtocol (&gRpiFanProtocolGuid, NULL, (VOID **)&Fan);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  ZeroMem (&Response, sizeof (Response));
  Status = RedfishHttpGetResource (Service, RPI_REDFISH_THERMAL_URI, NULL, &Response, FALSE);
  if (EFI_ERROR (Status)) {
    RedfishHttpFreeResponse (&Response);
    return Status;
  }

  Root = (Response.Payload == NULL) ? NULL : RedfishJsonInPayload (Response.Payload);

  Value = NULL;
  if ((Root != NULL) && JsonValueIsObject (Root)) {
    Value = JsonObjectGetValue (JsonValueGetObject (Root), "Oem");
    if ((Value != NULL) && JsonValueIsObject (Value)) {
      Value = JsonObjectGetValue (JsonValueGetObject (Value), "PiBmc");
      if ((Value != NULL) && JsonValueIsObject (Value)) {
        Value = JsonObjectGetValue (JsonValueGetObject (Value), "FanOverrideLevel");
      }
    }
  }

  if ((Value != NULL) && JsonValueIsInteger (Value)) {
    Level = JsonValueGetInteger (Value);
    if ((Level >= 0) && (Level <= 0xFF)) {
      Status = Fan->SetOverride (Fan, (UINT8)Level);
      DEBUG ((
        DEBUG_ERROR,
        "RpiRedfishSync: BMC fan override level %d - %r\n",
        (INT32)Level,
        Status
        ));
    } else {
      Fan->ClearOverride (Fan);
    }
  } else {
    //
    // No integer staged: the BMC is not steering, or released a previous
    // override. Clearing when none is active is a no-op.
    //
    Fan->ClearOverride (Fan);
  }

  RedfishHttpFreeResponse (&Response);
  return EFI_SUCCESS;
}

/**
  Periodic thermal telemetry: keep the BMC's temperature/fan view fresh
  and honour its steering while the firmware phase lasts (BDS wait, the
  Setup browser, the shell). Stops itself the first time BOTH legs fail:
  the service handle this rides was created for the one-shot exchange, and
  a link that has gone away is not coming back within this boot phase --
  better silent than banging a dead (or freed) service every 10 s.

  The event dies with boot services, so a successful OS handoff needs no
  cleanup here.

  @param[in] Event    The timer event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
ThermalTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  PatchStatus;
  EFI_STATUS  PollStatus;

  if (mThermalService == NULL) {
    return;
  }

  PatchStatus = ReportThermal (mThermalService);
  PollStatus  = PollFanOverride (mThermalService);

  if (EFI_ERROR (PatchStatus) && EFI_ERROR (PollStatus)) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: thermal telemetry stopped (%r / %r)\n",
      PatchStatus,
      PollStatus
      ));
    gBS->SetTimer (mThermalTimer, TimerCancel, 0);
    mThermalService = NULL;
  }
}

/**
  Perform the host-interface exchange against the discovered Redfish service.

  @param[in] ServiceInfo  Discovered Redfish service information.
**/
STATIC
VOID
RpiRedfishSync (
  IN REDFISH_CONFIG_SERVICE_INFORMATION  *ServiceInfo
  )
{
  REDFISH_SERVICE             Service;
  REDFISH_RESPONSE            Response;
  EFI_STATUS                  Status;
  RPI_REDFISH_HOST_INVENTORY  Inventory;
  CHAR8                       *Patch;
  UINTN                       Attempt;

  Service = RedfishCreateService (ServiceInfo);
  if (Service == NULL) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: RedfishCreateService failed\n"));
    return;
  }

  //
  // 1. Service root. Reaching this at all is the proof that the type 42 record,
  //    the NCM link and REST EX all line up.
  //
  // Retried: even with the sticky CableDetect patch, the BMC's HTTP listener
  // may not be up the instant the link is, and the first request after an
  // idle gap can catch a closed TCP connection. Bounded so a boot where the
  // BMC never answers is delayed, not blocked.
  //
  for (Attempt = 0; Attempt < RPI_REDFISH_MEDIA_RETRIES; Attempt++) {
    ZeroMem (&Response, sizeof (Response));
    Status = RedfishHttpGetResource (Service, RPI_REDFISH_SERVICE_ROOT_URI, NULL, &Response, FALSE);
    if ((Status != EFI_NO_MEDIA) || (Attempt == RPI_REDFISH_MEDIA_RETRIES - 1)) {
      LogResult ("GET", RPI_REDFISH_SERVICE_ROOT_URI, Status, &Response);
      RedfishHttpFreeResponse (&Response);
      break;
    }

    RedfishHttpFreeResponse (&Response);
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: no media on the host interface, retry %d/%d\n",
      Attempt + 1,
      RPI_REDFISH_MEDIA_RETRIES
      ));
    gBS->Stall (RPI_REDFISH_MEDIA_RETRY_STALL);
  }

  if (EFI_ERROR (Status)) {
    //
    // No point attempting the rest: the service is not answering. Leave the
    // service alone regardless -- see the note at the end of this function.
    //
    return;
  }

  //
  // 2. Report who this host is and how far it has booted.
  //
  Patch = NULL;
  RpiRedfishCollectInventory (&Inventory);
  Status = RpiRedfishBuildSystemPatch (&Inventory, &Patch);
  if (!EFI_ERROR (Status) && (Patch != NULL)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: PATCH body %a\n", Patch));

    ZeroMem (&Response, sizeof (Response));
    Status = RedfishHttpPatchResource (Service, RPI_REDFISH_SYSTEM_URI, Patch, &Response);
    LogResult ("PATCH", RPI_REDFISH_SYSTEM_URI, Status, &Response);
    RedfishHttpFreeResponse (&Response);

    FreePool (Patch);
  }

  //
  // 2b. Report the processors.
  //
  ReportProcessors (Service);

  //
  // 2c. Report the memory devices.
  //
  ReportMemory (Service);

  //
  // 2d. Report the drives.
  //
  ReportDrives (Service);

  //
  // 2e. First thermal sample + any fan steering the BMC already staged.
  //    Before the boot-override step on purpose: an override reboots the
  //    host, and the BMC should still get one thermal reading out of this
  //    boot.
  //
  ReportThermal (Service);
  PollFanOverride (Service);

  //
  // 3. Read back the system, including any boot override the BMC wants applied.
  //
  ZeroMem (&Response, sizeof (Response));
  Status = RedfishHttpGetResource (Service, RPI_REDFISH_SYSTEM_URI, NULL, &Response, FALSE);
  LogResult ("GET", RPI_REDFISH_SYSTEM_URI, Status, &Response);
  if (!EFI_ERROR (Status)) {
    HandleBootOverride (Service, &Response);
  }

  RedfishHttpFreeResponse (&Response);

  //
  // Keep the thermal view fresh from here on. Only reached when no boot
  // override fired (ApplyMatchedOption resets and never returns), i.e. the
  // host is proceeding into BDS wait / Setup / a normal boot.
  //
  mThermalService = Service;
  Status          = gBS->CreateEvent (
                           EVT_TIMER | EVT_NOTIFY_SIGNAL,
                           TPL_CALLBACK,
                           ThermalTick,
                           NULL,
                           &mThermalTimer
                           );
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mThermalTimer, TimerPeriodic, RPI_REDFISH_THERMAL_PERIOD);
  }

  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: thermal telemetry timer - %r\n", Status));

  //
  // Deliberately no RedfishCleanupService (Service) here.
  //
  // This driver does not own the service. RedfishConfigHandlerDriver creates it
  // once from what RedfishDiscoverDxe found and hands the same instance to every
  // registered config handler in turn; destroying it here would tear down the
  // REST EX child underneath any handler that has not run yet.
  //
  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: host interface exchange complete\n"));
}

/**
  EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL.Init.

  Called by RedfishConfigHandlerDriver once a Redfish service has been
  discovered. Runs the exchange inline: the caller is already on the BDS path
  with the network stack up, and the work is a handful of requests against a
  point-to-point link.

  @param[in] This         This protocol instance.
  @param[in] ServiceInfo  Discovered Redfish service information.

  @retval EFI_SUCCESS    The exchange ran (or was already done this boot).
  @retval EFI_NOT_READY  Called before discovery filled in ServiceInfo.
**/
EFI_STATUS
EFIAPI
RpiRedfishConfigHandlerInit (
  IN EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL  *This,
  IN REDFISH_CONFIG_SERVICE_INFORMATION     *ServiceInfo
  )
{
  if (ServiceInfo == NULL) {
    return EFI_SUCCESS;
  }

  //
  // Init is called on two different occasions, and only the second one is
  // usable. RedfishConfigHandlerDriver registers a protocol notify on
  // EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL, so installing ours in the entry point
  // makes it fire immediately -- before any service has been discovered, with
  // the service info still zeroed. The call that matters comes later, from
  // RedfishServiceDiscoveredCallback, once that structure has been filled in.
  //
  // Returning an error here is load-bearing, not just informative:
  // RedfishConfigHandlerInitialization() marks (with gEfiCallerIdGuid) every
  // config handler whose Init returned success as already-initialized and
  // never calls it again -- so answering EFI_SUCCESS to the pre-discovery
  // call would suppress the real invocation. EFI_NOT_READY leaves the handle
  // unmarked and eligible for the call that has a service attached. (Observed
  // on the NUC 2026-07-30.)
  //
  if (ServiceInfo->RedfishServiceRestExHandle == NULL) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: init before discovery (no REST EX yet), waiting\n"));
    return EFI_NOT_READY;
  }

  //
  // Discovery can signal more than once (for example if another interface is
  // acquired later). The inventory report is idempotent, but repeating it would
  // add avoidable delay to every boot.
  //
  if (mSyncDone) {
    return EFI_SUCCESS;
  }

  mSyncDone = TRUE;

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: config handler init, service at %s (uuid %s)\n",
    (ServiceInfo->RedfishServiceLocation != NULL) ? ServiceInfo->RedfishServiceLocation : L"(unknown)",
    (ServiceInfo->RedfishServiceUuid != NULL) ? ServiceInfo->RedfishServiceUuid : L"(unknown)"
    ));

  RpiRedfishSync (ServiceInfo);

  return EFI_SUCCESS;
}

/**
  EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL.Stop.

  @param[in] This  This protocol instance.

  @retval EFI_SUCCESS  Nothing to tear down.
**/
EFI_STATUS
EFIAPI
RpiRedfishConfigHandlerStop (
  IN EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL  mRpiRedfishConfigHandler = {
  RpiRedfishConfigHandlerInit,
  RpiRedfishConfigHandlerStop
};

/**
  Driver entry point. Installs the config handler protocol; everything else is
  driven by RedfishConfigHandlerDriver when a service is discovered.

  @param[in] ImageHandle  Image handle.
  @param[in] SystemTable  System table.

  @retval EFI_SUCCESS  Protocol installed.
  @retval Others       Installation failed.
**/
EFI_STATUS
EFIAPI
RpiRedfishSyncDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  mImageHandle = ImageHandle;

  Status = gBS->InstallProtocolInterface (
                  &mImageHandle,
                  &gEdkIIRedfishConfigHandlerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mRpiRedfishConfigHandler
                  );

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSyncDxe: install EdkIIRedfishConfigHandler protocol - %r\n",
    Status
    ));

  return Status;
}

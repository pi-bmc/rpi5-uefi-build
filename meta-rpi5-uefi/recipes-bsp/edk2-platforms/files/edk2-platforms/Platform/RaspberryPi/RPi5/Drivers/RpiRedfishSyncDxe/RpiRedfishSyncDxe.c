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
    2b-2e. POST Processors /        -- inventory the BMC cannot see in band
           Memory / Drives /           (successor of BlkInfoMirrorDxe; the NICs
           EthernetInterfaces          succeed the U-Boot env "ethaddr").
    2g. PATCH+GET Chassis/1/Thermal -- SoC temperature + fan state up, BMC fan
                                       steering down (RPI_FAN_PROTOCOL). Once,
                                       as part of this exchange: see below.
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

#include <Protocol/BootManagerPolicy.h>
#include <Protocol/RpiFan.h>

STATIC EFI_HANDLE  mImageHandle = NULL;
STATIC BOOLEAN     mSyncDone    = FALSE;
STATIC BOOLEAN     mSettled     = FALSE;
STATIC BOOLEAN     mReadyToBoot = FALSE;

//
// Installed (bare, no interface) once the first sync attempt has run to
// completion -- whatever its outcome. PlatformBootManagerLib waits a bounded
// time for this before letting BDS proceed to the boot selection, so a boot
// override staged on the BMC is consumed BEFORE the default option boots
// rather than racing it: since the USB NIC gate moved the Redfish stack's
// bring-up out of ConnectAll (edk2-platforms patch 0034 / edk2 patch 0108),
// nothing else serializes the exchange against the boot. The GUID literal is
// duplicated in PlatformBm.c; keep the two in sync.
//
STATIC CONST EFI_GUID  mRpiRedfishSyncSettledGuid = {
  0x47a80c52, 0x948f, 0x4972, { 0x81, 0xe1, 0x6b, 0x32, 0x32, 0x4b, 0x06, 0x08 }
};

//
// NV breadcrumb (vendor GUID: the settled GUID above) marking that the
// previous boot staged BootNext for a "Continuous" override and cold-reset.
// Its presence means THIS boot is the one launching that target: BdsEntry
// caches BootNext before any platform hook runs (BdsEntry.c, "Cache the
// BootNext NV variable before calling any PlatformBootManagerLib APIs"),
// so if the sync staged and reset again here, the cached target would be
// thrown away every cycle and the machine would reset forever without ever
// reaching it. The sync deletes the breadcrumb and stands aside for that
// one boot instead.
//
#define RPI_REDFISH_CONTINUOUS_MARK_VAR  L"RpiSyncContinuousApplied"

/**
  Mark the sync as settled for whoever is waiting on it.

  Idempotent; installs mRpiRedfishSyncSettledGuid on a fresh handle. Never
  fails the caller -- a failed install only costs the waiter its full bound.
**/
STATIC
VOID
SignalSyncSettled (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  if (mSettled) {
    return;
  }

  mSettled = TRUE;
  Handle   = NULL;
  Status   = gBS->InstallProtocolInterface (
                    &Handle,
                    (EFI_GUID *)&mRpiRedfishSyncSettledGuid,
                    EFI_NATIVE_INTERFACE,
                    NULL
                    );
  DEBUG ((DEBUG_ERROR, "RpiRedfishSync: settled - %r\n", Status));
}

/**
  Latch ReadyToBoot. Past this point a boot override must not be applied:
  staging BootNext and cold-resetting under a loaded OS loader would yank the
  machine out from under it. HandleBootOverride() checks the latch and leaves
  the override staged on the BMC instead; the next boot's sync -- which runs
  long before ReadyToBoot -- picks it up.

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
  mReadyToBoot = TRUE;
}

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
  Does one boot option satisfy one Redfish BootSourceOverrideTarget?

  @param[in] Target  Redfish BootSourceOverrideTarget value.
  @param[in] Option  Boot option to test.

  @retval TRUE   The option satisfies the target.
  @retval FALSE  It does not.
**/
STATIC
BOOLEAN
OptionMatchesTarget (
  IN CONST CHAR8                   *Target,
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  if (AsciiStrCmp (Target, "BiosSetup") == 0) {
    //
    // The setup UI (UiApp) is the boot option BDS tags as an application.
    //
    return ((Option->Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_APP);
  }

  if (AsciiStrCmp (Target, "Pxe") == 0) {
    //
    // Network boot is BDS's own "PXEv4 (MAC:...)" option for the onboard
    // RJ45, auto-created once Rp1GemDxe publishes SNP for it. Match the
    // device path rather than the description, which is localised.
    //
    return OptionIsNetworkBoot (Option);
  }

  if (AsciiStrCmp (Target, "Usb") == 0) {
    //
    // USB mass storage -- the capsule volume the BMC presents on its
    // gadget, or a stick. A USB node without a MAC node: the MAC test
    // keeps the RHI NIC out (OptionIsNetworkBoot's concern, mirrored),
    // and Hdd below deliberately does not match bare USB.
    //
    return (OptionHasNode (Option, MESSAGING_DEVICE_PATH, MSG_USB_DP) &&
            !OptionHasNode (Option, MESSAGING_DEVICE_PATH, MSG_MAC_ADDR_DP));
  }

  if (AsciiStrCmp (Target, "Hdd") == 0) {
    //
    // Local block storage, in whichever shape BDS has it at this point.
    // See OptionIsDiskBoot().
    //
    return OptionIsDiskBoot (Option);
  }

  return FALSE;
}

/**
  Connect the device class a boot override target implies.

  Under BDP_CONNECT_MINIMAL -- this platform's default Boot Discovery
  Policy -- nothing connects the onboard NIC or extra storage during an
  ordinary boot, so the boot option a "Pxe" or "Hdd" override needs may not
  exist yet. Same ConnectDeviceClass plumbing BootDiscoveryPolicyHandler
  uses, driven by the override instead of the policy variable.

  @param[in] Target  Redfish BootSourceOverrideTarget value.
**/
STATIC
VOID
ConnectBootClassDevices (
  IN CONST CHAR8  *Target
  )
{
  EFI_STATUS                        Status;
  EFI_BOOT_MANAGER_POLICY_PROTOCOL  *Policy;
  EFI_GUID                          *Class;

  Status = gBS->LocateProtocol (
                  &gEfiBootManagerPolicyProtocolGuid,
                  NULL,
                  (VOID **)&Policy
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no boot manager policy - %r\n", Status));
    return;
  }

  if (AsciiStrCmp (Target, "Pxe") == 0) {
    Class = &gEfiBootManagerPolicyNetworkGuid;
  } else {
    Class = &gEfiBootManagerPolicyConnectAllGuid;
  }

  Status = Policy->ConnectDeviceClass (Policy, Class);
  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: connected device class for '%a' - %r\n",
    Target,
    Status
    ));
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
  UINTN    Pass;
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

  for (Pass = 0; Pass < 2; Pass++) {
    for (Index = 0; Index < *OptionCount; Index++) {
      if (OptionMatchesTarget (Target, &(*Options)[Index])) {
        Found  = TRUE;
        *Match = Index;
        break;
      }
    }

    if (Found || (Pass == 1)) {
      break;
    }

    //
    // Nothing matched the options as they stand. Under BDP_CONNECT_MINIMAL
    // (this platform's default) the device class the target implies may
    // never have been connected this boot, and its boot option never
    // created -- a PXE option only exists once the onboard NIC's network
    // stack has published a load file. Connect the class, refresh the
    // options, scan once more. (The RHI's own USB NIC can never satisfy
    // "Pxe": OptionIsNetworkBoot rejects USB device paths, and patch 0103
    // keeps USB NICs out of enumeration entirely.)
    //
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: no option matches '%a' yet, connecting its device class\n",
      Target
      ));
    EfiBootManagerFreeLoadOptions (*Options, *OptionCount);
    *Options     = NULL;
    *OptionCount = 0;

    ConnectBootClassDevices (Target);
    EfiBootManagerRefreshAllBootOption ();

    *Options = EfiBootManagerGetLoadOptions (OptionCount, LoadOptionTypeBoot);
    if ((*Options == NULL) || (*OptionCount == 0)) {
      return EFI_NOT_FOUND;
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
  Read the BMC's requested boot override from a ComputerSystem payload and act
  on it.

  A "Once" override is acknowledged to the BMC (cleared) and then applied via
  BootNext plus a cold reset -- consumed exactly once, and the target gets a
  clean full-reset boot.

  A "Continuous" override is applied exactly the same way, minus the
  acknowledgement: it stays staged on the BMC until the operator clears it,
  and re-applies on every subsequent boot cycle. What breaks the
  otherwise-infinite stage/reset loop is RPI_REDFISH_CONTINUOUS_MARK_VAR
  (see its comment): the boot that stages BootNext sets it, and the next
  boot's sync sees it, deletes it and stands aside while BDS launches the
  target it cached at entry.

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
  BOOLEAN                       IsOnce;
  BOOLEAN                       IsContinuous;
  UINTN                         DataSize;
  UINT16                        Mark;

  if (mReadyToBoot) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: past ReadyToBoot, leaving any boot override staged for the next boot\n"
      ));
    return;
  }

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

  IsOnce       = ((Enabled != NULL) && (AsciiStrCmp (Enabled, "Once") == 0));
  IsContinuous = ((Enabled != NULL) && (AsciiStrCmp (Enabled, "Continuous") == 0));

  if (!IsOnce && !IsContinuous) {
    return;
  }

  if (IsContinuous) {
    DataSize = 0;
    Status   = gRT->GetVariable (
                      RPI_REDFISH_CONTINUOUS_MARK_VAR,
                      (EFI_GUID *)&mRpiRedfishSyncSettledGuid,
                      NULL,
                      &DataSize,
                      NULL
                      );
    if (Status == EFI_BUFFER_TOO_SMALL) {
      //
      // This boot exists to launch the BootNext the previous boot staged;
      // BdsEntry cached it at entry. Stand aside.
      //
      gRT->SetVariable (
             RPI_REDFISH_CONTINUOUS_MARK_VAR,
             (EFI_GUID *)&mRpiRedfishSyncSettledGuid,
             0,
             0,
             NULL
             );
      DEBUG ((
        DEBUG_ERROR,
        "RpiRedfishSync: continuous override boot in progress, standing aside\n"
        ));
      return;
    }
  }

  if (EFI_ERROR (FindBootOverrideOption (Target, &Options, &OptionCount, &Match))) {
    return;
  }

  if (IsContinuous) {
    //
    // Same apply as "Once", minus the acknowledgement -- Continuous stays
    // staged on the BMC by definition. Mark the cycle first: without the
    // breadcrumb the next boot would stage and reset again forever, so a
    // mark that cannot be written means the override must not be applied.
    //
    Mark   = (UINT16)Options[Match].OptionNumber;
    Status = gRT->SetVariable (
                    RPI_REDFISH_CONTINUOUS_MARK_VAR,
                    (EFI_GUID *)&mRpiRedfishSyncSettledGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                    sizeof (Mark),
                    &Mark
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RpiRedfishSync: cannot mark the continuous cycle (%r), not applying\n",
        Status
        ));
      EfiBootManagerFreeLoadOptions (Options, OptionCount);
      return;
    }

    ApplyMatchedOption (&Options[Match]);

    EfiBootManagerFreeLoadOptions (Options, OptionCount);
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
  Report what firmware this board runs, as a Redfish SoftwareInventory.

  This is the inventory half of remote updates: it is what tells an operator --
  or an automated fleet check -- which version a node is on, whether it can be
  updated at all, and how the last attempt went. The BMC needs those to decide
  whether to stage a capsule; without them the only way to know a node's
  firmware version is to boot it and look.

  PATCH rather than POST: the resource is fixed and per-node, so re-reporting
  the same node updates it rather than accumulating duplicates. Fail-open like
  everything else here -- a BMC with no UpdateService just 404s.

  @param[in] Service  The Redfish service to report to.
**/
STATIC
VOID
ReportFirmwareInventory (
  IN REDFISH_SERVICE  Service
  )
{
  RPI_REDFISH_FIRMWARE_IMAGE  Images[RPI_REDFISH_FIRMWARE_MAX];
  REDFISH_RESPONSE            Response;
  EFI_STATUS                  Status;
  UINTN                       Count;
  CHAR8                       *Body;

  Status = RpiRedfishCollectFirmware (Images, RPI_REDFISH_FIRMWARE_MAX, &Count);
  if (EFI_ERROR (Status) || (Count == 0)) {
    //
    // Expected on a build without RPI5_FMP: there is no Firmware Management
    // Protocol to ask, so there is no inventory to report.
    //
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: no firmware inventory to report - %r\n", Status));
    return;
  }

  //
  // Only the first image is reported. The platform publishes exactly one
  // updatable firmware, and the inventory URI names it; a second producer would
  // need a resource of its own rather than overwriting this one.
  //
  Body   = NULL;
  Status = RpiRedfishBuildFirmwareInventoryPatch (&Images[0], &Body);
  if (EFI_ERROR (Status) || (Body == NULL)) {
    return;
  }

  ZeroMem (&Response, sizeof (Response));
  Status = RedfishHttpPatchResource (Service, RPI_REDFISH_FIRMWARE_INVENTORY_URI, Body, &Response);
  LogResult ("PATCH", RPI_REDFISH_FIRMWARE_INVENTORY_URI, Status, &Response);
  RedfishHttpFreeResponse (&Response);

  FreePool (Body);

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: reported firmware '%a' version %a (%u), updateable %a\n",
    Images[0].Name,
    Images[0].Version,
    Images[0].VersionNumber,
    Images[0].Updateable ? "yes" : "no"
    ));
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
  // 2e. Report the firmware inventory.
  //
  ReportFirmwareInventory (Service);

  //
  // 2f. Apply any fan steering the BMC already staged. The thermal reading
  //    itself is no longer PATCHed from here: OP-TEE pushes SoC temperature
  //    and fan state to the BMC over I2C (the bmc_sensor record) and the BMC
  //    renders Chassis/1/Thermal from that. Before the boot-override step on
  //    purpose: an override reboots the host.
  //
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

  //
  // The exchange ran to completion (an applied boot override never returns
  // here -- ApplyMatchedOption resets). Release anyone holding the boot for
  // its outcome.
  //
  SignalSyncSettled ();

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
  EFI_EVENT   ReadyToBootEvent;

  mImageHandle = ImageHandle;

  //
  // Latch ReadyToBoot so a sync that loses the race against the boot cannot
  // cold-reset the machine under a running OS loader. Failure to create the
  // event is survivable -- it only re-opens that (rare) window.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &ReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: ReadyToBoot latch - %r\n", Status));
  }

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

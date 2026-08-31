/** @file

  EthConfigDxe - BMC-managed IPv4 policy for the onboard NIC.

  Pure efivarstore HII in the FanConfigDxe idiom: the formset
  (EthConfigHii.vfr) binds its questions straight to the EthCfg variable,
  so the form browser and the Redfish Bios pipeline read and write the
  variable itself and this driver needs no ConfigAccess callbacks. What it
  does add over FanConfigDxe:

  * The HII packages are published on a fresh handle whose device path is
    the NIC's own path plus one vendor node (the Ip4Dxe trick from
    Ip4Config2FormInit). DeviceManagerUiLib groups formsets by a MAC node
    in the owning handle's device path, so the page appears under
    "Network Device List" next to the NIC's native IPv4 form instead of at
    the Device Manager top level.

  * At boot, once Ip4Dxe binds the NIC and installs EFI_IP4_CONFIG2_PROTOCOL
    on it, the stored policy is applied: Unmanaged touches nothing, DHCP
    forces the DHCP policy, Static parses the dotted-quad strings and pushes
    Policy/ManualAddress/Gateway/DnsServer the same way the native form's
    save path does. A BMC write over Redfish therefore takes effect on the
    NEXT boot (the Redfish provisioning runs long after this apply), which
    is why every question is RESET_REQUIRED.

  The BMC's own USB CDC network gadget - the Redfish host interface - is
  excluded by the same USB-node-plus-MAC-node device path test the 0103 and
  0109 edk2 patches use: this driver must never publish on it, and above
  all must never rewrite the IP configuration of the link the BMC is
  reached over. Only the first non-USB NIC is claimed (single-NIC board;
  the attribute namespace is flat).

  Everything is fail-open: a malformed variable or a SetData failure keeps
  the boot on the NIC's existing configuration.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/Ip4Config2.h>

#include <Guid/RpiEthConfig.h>

//
// AutoGen emits these from EthConfigHii.vfr and the .uni files.
//
extern UINT8  EthConfigHiiBin[];
extern UINT8  EthConfigDxeStrings[];

STATIC EFI_EVENT  mIp4Config2Notify;
STATIC VOID       *mIp4Config2Registration;
STATIC BOOLEAN    mNicClaimed;

/**
  Create EthCfg with Unmanaged defaults when it is absent or has been
  written with the wrong size (an old layout, a corrupt BMC write): the
  browser's efivarstore reads and the Redfish attribute reads both need a
  well-formed variable, and the apply path treats a malformed one as
  Unmanaged anyway.
**/
STATIC
VOID
EnsureConfigVariable (
  VOID
  )
{
  RPI_ETH_CONFIG  Config;
  UINTN           Size;
  EFI_STATUS      Status;

  Size   = sizeof (Config);
  Status = gRT->GetVariable (
                  RPI_ETH_CONFIG_VARIABLE_NAME,
                  &gRpiEthConfigFormSetGuid,
                  NULL,
                  &Size,
                  &Config
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Config))) {
    return;
  }

  ZeroMem (&Config, sizeof (Config));
  Config.Ip4Mode = RPI_ETH_IP4_MODE_UNMANAGED;

  Status = gRT->SetVariable (
                  RPI_ETH_CONFIG_VARIABLE_NAME,
                  &gRpiEthConfigFormSetGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (Config),
                  &Config
                  );
  DEBUG ((DEBUG_INFO, "EthConfigDxe: created default EthCfg - %r\n", Status));
}

/**
  Read EthCfg, insisting on the exact layout size and forcing NUL
  termination on every string so a garbage BMC write cannot run a later
  StrLen off the end.

  @param[out] Config  Receives the variable content.

  @retval EFI_SUCCESS    Config is valid.
  @retval EFI_NOT_FOUND  The variable is absent or malformed.
**/
STATIC
EFI_STATUS
ReadConfigVariable (
  OUT RPI_ETH_CONFIG  *Config
  )
{
  UINTN       Size;
  EFI_STATUS  Status;

  Size   = sizeof (*Config);
  Status = gRT->GetVariable (
                  RPI_ETH_CONFIG_VARIABLE_NAME,
                  &gRpiEthConfigFormSetGuid,
                  NULL,
                  &Size,
                  Config
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (*Config))) {
    return EFI_NOT_FOUND;
  }

  Config->Ip4Address[RPI_ETH_IP4_STR_SIZE - 1]    = L'\0';
  Config->Ip4SubnetMask[RPI_ETH_IP4_STR_SIZE - 1] = L'\0';
  Config->Ip4Gateway[RPI_ETH_IP4_STR_SIZE - 1]    = L'\0';
  Config->Ip4Dns1[RPI_ETH_IP4_STR_SIZE - 1]       = L'\0';
  Config->Ip4Dns2[RPI_ETH_IP4_STR_SIZE - 1]       = L'\0';
  return EFI_SUCCESS;
}

/**
  Classify a NIC device path: does it contain a MAC node, and does it run
  over USB anywhere along the way?

  @param[in]  DevicePath  The path to walk.
  @param[out] HasMac      TRUE when a MAC node is present.
  @param[out] OverUsb     TRUE when a USB node is present.
**/
STATIC
VOID
ClassifyNicDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  OUT BOOLEAN                   *HasMac,
  OUT BOOLEAN                   *OverUsb
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  *HasMac  = FALSE;
  *OverUsb = FALSE;
  for (Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    if (DevicePathSubType (Node) == MSG_USB_DP) {
      *OverUsb = TRUE;
    } else if (DevicePathSubType (Node) == MSG_MAC_ADDR_DP) {
      *HasMac = TRUE;
    }
  }
}

/**
  Parse an optional dotted-quad string field.

  @param[in]  String  The field content (NUL terminated).
  @param[out] Ip      Receives the parsed address.

  @retval EFI_SUCCESS            Parsed.
  @retval EFI_NOT_FOUND          The field is empty (legitimate for the
                                 optional fields).
  @retval EFI_INVALID_PARAMETER  Non-empty but not a valid address.
**/
STATIC
EFI_STATUS
ParseIp4Field (
  IN  CONST CHAR16      *String,
  OUT EFI_IPv4_ADDRESS  *Ip
  )
{
  if (String[0] == L'\0') {
    return EFI_NOT_FOUND;
  }

  if (EFI_ERROR (NetLibStrToIp4 (String, Ip))) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Callback for the manual-address data notify: Ip4Config2 signals it when
  an asynchronous SetData (ManualAddress) completes.

  @param[in] Event    The signaled event.
  @param[in] Context  Points at the BOOLEAN to raise.
**/
STATIC
VOID
EFIAPI
ManualAddressNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  *((BOOLEAN *)Context) = TRUE;
}

/**
  Push the static settings into Ip4Config2, mirroring the native form's
  save path (Ip4Config2ConvertIfrNvDataToConfigNvData): Policy first, then
  ManualAddress with the asynchronous EFI_NOT_READY wait, then the
  optional Gateway and DnsServer lists.

  @param[in] Ip4Cfg2  The NIC's config protocol.
  @param[in] Config   The validated EthCfg content (STATIC mode).

  @retval EFI_SUCCESS  The address was applied (gateway/DNS best-effort).
  @return  Others      Parse or SetData failure; the NIC keeps whatever
                       configuration it had.
**/
STATIC
EFI_STATUS
ApplyStaticConfig (
  IN EFI_IP4_CONFIG2_PROTOCOL  *Ip4Cfg2,
  IN RPI_ETH_CONFIG            *Config
  )
{
  EFI_STATUS                      Status;
  EFI_IP4_CONFIG2_POLICY          Policy;
  EFI_IP4_CONFIG2_MANUAL_ADDRESS  ManualAddress;
  EFI_IPv4_ADDRESS                Gateway;
  EFI_IPv4_ADDRESS                Dns[2];
  UINTN                           DnsCount;
  IP4_ADDR                        Ip;
  IP4_ADDR                        Mask;
  EFI_EVENT                       TimeoutEvent;
  EFI_EVENT                       SetAddressEvent;
  BOOLEAN                         IsAddressOk;
  INTN                            MaskLength;

  if (EFI_ERROR (ParseIp4Field (Config->Ip4Address, &ManualAddress.Address)) ||
      EFI_ERROR (ParseIp4Field (Config->Ip4SubnetMask, &ManualAddress.SubnetMask)))
  {
    DEBUG ((DEBUG_WARN, "EthConfigDxe: static mode with unparseable address/mask, not applied\n"));
    return EFI_INVALID_PARAMETER;
  }

  //
  // NetGetMaskLength returns IP4_MASK_NUM for a non-contiguous mask; a
  // zero-length mask (0.0.0.0) is contiguous but useless for a static
  // address, so both ends of the range are rejected.
  //
  CopyMem (&Ip, &ManualAddress.Address, sizeof (IP4_ADDR));
  CopyMem (&Mask, &ManualAddress.SubnetMask, sizeof (IP4_ADDR));
  MaskLength = NetGetMaskLength (NTOHL (Mask));
  if ((MaskLength == 0) || (MaskLength >= IP4_MASK_NUM) ||
      !NetIp4IsUnicast (NTOHL (Ip), NTOHL (Mask)))
  {
    DEBUG ((DEBUG_WARN, "EthConfigDxe: invalid static address/mask, not applied\n"));
    return EFI_INVALID_PARAMETER;
  }

  Policy = Ip4Config2PolicyStatic;
  Status = Ip4Cfg2->SetData (
                      Ip4Cfg2,
                      Ip4Config2DataTypePolicy,
                      sizeof (Policy),
                      &Policy
                      );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  TimeoutEvent    = NULL;
  SetAddressEvent = NULL;
  IsAddressOk     = FALSE;

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimeoutEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  ManualAddressNotify,
                  &IsAddressOk,
                  &SetAddressEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = Ip4Cfg2->RegisterDataNotify (
                      Ip4Cfg2,
                      Ip4Config2DataTypeManualAddress,
                      SetAddressEvent
                      );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = Ip4Cfg2->SetData (
                      Ip4Cfg2,
                      Ip4Config2DataTypeManualAddress,
                      sizeof (ManualAddress),
                      &ManualAddress
                      );
  if (Status == EFI_NOT_READY) {
    //
    // Completes asynchronously; the native form waits up to 5 s the same
    // way. This runs once per boot and only in STATIC mode.
    //
    gBS->SetTimer (TimeoutEvent, TimerRelative, 50000000);
    while (EFI_ERROR (gBS->CheckEvent (TimeoutEvent))) {
      if (IsAddressOk) {
        Status = EFI_SUCCESS;
        break;
      }
    }
  }

  Ip4Cfg2->UnregisterDataNotify (
             Ip4Cfg2,
             Ip4Config2DataTypeManualAddress,
             SetAddressEvent
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  //
  // Gateway and DNS are optional and best-effort: a parse failure or a
  // SetData error on them does not undo the address.
  //
  if (ParseIp4Field (Config->Ip4Gateway, &Gateway) == EFI_SUCCESS) {
    CopyMem (&Ip, &Gateway, sizeof (IP4_ADDR));
    if (NetIp4IsUnicast (NTOHL (Ip), NTOHL (Mask))) {
      Ip4Cfg2->SetData (
                 Ip4Cfg2,
                 Ip4Config2DataTypeGateway,
                 sizeof (Gateway),
                 &Gateway
                 );
    }
  }

  DnsCount = 0;
  if (ParseIp4Field (Config->Ip4Dns1, &Dns[DnsCount]) == EFI_SUCCESS) {
    DnsCount++;
  }

  if (ParseIp4Field (Config->Ip4Dns2, &Dns[DnsCount]) == EFI_SUCCESS) {
    DnsCount++;
  }

  if (DnsCount > 0) {
    Ip4Cfg2->SetData (
               Ip4Cfg2,
               Ip4Config2DataTypeDnsServer,
               DnsCount * sizeof (EFI_IPv4_ADDRESS),
               Dns
               );
  }

Exit:
  if (SetAddressEvent != NULL) {
    gBS->CloseEvent (SetAddressEvent);
  }

  if (TimeoutEvent != NULL) {
    gBS->CloseEvent (TimeoutEvent);
  }

  return Status;
}

/**
  Apply the stored EthCfg policy to a freshly bound NIC.

  @param[in] NicHandle  The handle carrying EFI_IP4_CONFIG2_PROTOCOL.
**/
STATIC
VOID
ApplyConfig (
  IN EFI_HANDLE  NicHandle
  )
{
  EFI_STATUS                Status;
  EFI_IP4_CONFIG2_PROTOCOL  *Ip4Cfg2;
  EFI_IP4_CONFIG2_POLICY    Policy;
  RPI_ETH_CONFIG            Config;

  if (EFI_ERROR (ReadConfigVariable (&Config))) {
    return;
  }

  if (Config.Ip4Mode == RPI_ETH_IP4_MODE_UNMANAGED) {
    return;
  }

  Status = gBS->HandleProtocol (
                  NicHandle,
                  &gEfiIp4Config2ProtocolGuid,
                  (VOID **)&Ip4Cfg2
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  switch (Config.Ip4Mode) {
    case RPI_ETH_IP4_MODE_DHCP:
      Policy = Ip4Config2PolicyDhcp;
      Status = Ip4Cfg2->SetData (
                          Ip4Cfg2,
                          Ip4Config2DataTypePolicy,
                          sizeof (Policy),
                          &Policy
                          );
      break;

    case RPI_ETH_IP4_MODE_STATIC:
      Status = ApplyStaticConfig (Ip4Cfg2, &Config);
      break;

    default:
      //
      // An unknown mode (a corrupt or future-layout write) is Unmanaged.
      //
      return;
  }

  DEBUG ((
    DEBUG_INFO,
    "EthConfigDxe: applied IPv4 mode %u - %r\n",
    Config.Ip4Mode,
    Status
    ));
}

/**
  Publish the formset on a fresh handle whose device path is the NIC's
  path plus one vendor node, so DeviceManagerUiLib's MAC-node grouping
  places the page under "Network Device List".

  @param[in] NicDevicePath  The NIC's own device path.
**/
STATIC
VOID
PublishFormset (
  IN EFI_DEVICE_PATH_PROTOCOL  *NicDevicePath
  )
{
  EFI_STATUS                Status;
  VENDOR_DEVICE_PATH        VendorNode;
  EFI_DEVICE_PATH_PROTOCOL  *ChildDevicePath;
  EFI_HANDLE                ChildHandle;
  EFI_HII_HANDLE            HiiHandle;

  ZeroMem (&VendorNode, sizeof (VendorNode));
  VendorNode.Header.Type    = HARDWARE_DEVICE_PATH;
  VendorNode.Header.SubType = HW_VENDOR_DP;
  CopyGuid (&VendorNode.Guid, &gRpiEthConfigFormSetGuid);
  SetDevicePathNodeLength (&VendorNode.Header, sizeof (VendorNode));

  ChildDevicePath = AppendDevicePathNode (
                      NicDevicePath,
                      (EFI_DEVICE_PATH_PROTOCOL *)&VendorNode
                      );
  if (ChildDevicePath == NULL) {
    return;
  }

  ChildHandle = NULL;
  Status      = gBS->InstallMultipleProtocolInterfaces (
                       &ChildHandle,
                       &gEfiDevicePathProtocolGuid,
                       ChildDevicePath,
                       NULL
                       );
  if (EFI_ERROR (Status)) {
    FreePool (ChildDevicePath);
    return;
  }

  HiiHandle = HiiAddPackages (
                &gRpiEthConfigFormSetGuid,
                ChildHandle,
                EthConfigDxeStrings,
                EthConfigHiiBin,
                NULL
                );
  if (HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           ChildHandle,
           &gEfiDevicePathProtocolGuid,
           ChildDevicePath,
           NULL
           );
    FreePool (ChildDevicePath);
    return;
  }

  DEBUG ((DEBUG_INFO, "EthConfigDxe: Setup page published on the NIC handle\n"));
}

/**
  Protocol notify: Ip4Dxe installed EFI_IP4_CONFIG2_PROTOCOL on a NIC.
  Claim the first one that is not the BMC's USB gadget: publish the
  formset under its device path and apply the stored policy.

  @param[in] Event    The notify event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
OnIp4Config2Installed (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                NicHandle;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     BufferSize;
  BOOLEAN                   HasMac;
  BOOLEAN                   OverUsb;

  for ( ; ;) {
    BufferSize = sizeof (NicHandle);
    Status     = gBS->LocateHandle (
                        ByRegisterNotify,
                        NULL,
                        mIp4Config2Registration,
                        &BufferSize,
                        &NicHandle
                        );
    if (EFI_ERROR (Status)) {
      break;
    }

    if (mNicClaimed) {
      //
      // Keep draining the registration queue, but the page and the apply
      // went to the first NIC.
      //
      continue;
    }

    DevicePath = DevicePathFromHandle (NicHandle);
    if (DevicePath == NULL) {
      continue;
    }

    ClassifyNicDevicePath (DevicePath, &HasMac, &OverUsb);
    if (OverUsb || !HasMac) {
      //
      // The only USB NIC on this board is the BMC's CDC network gadget,
      // the link the BMC manages us over - never republish or reconfigure
      // that one. No MAC node would defeat the menu grouping anyway.
      //
      continue;
    }

    mNicClaimed = TRUE;
    PublishFormset (DevicePath);
    ApplyConfig (NicHandle);
  }
}

EFI_STATUS
EFIAPI
EthConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  EnsureConfigVariable ();

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnIp4Config2Installed,
                  NULL,
                  &mIp4Config2Notify
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiIp4Config2ProtocolGuid,
                  mIp4Config2Notify,
                  &mIp4Config2Registration
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mIp4Config2Notify);
    return Status;
  }

  return EFI_SUCCESS;
}

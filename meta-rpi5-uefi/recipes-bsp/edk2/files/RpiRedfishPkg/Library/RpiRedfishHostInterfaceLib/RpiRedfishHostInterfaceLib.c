/** @file
  RpiRedfishHostInterfaceLib

  RedfishPlatformHostInterfaceLib instance for the Pi 5 <-> BMC USB CDC-NCM
  link. Reports a single static "Redfish over IP" protocol record plus a USB
  V2 device descriptor (SMBIOS Type 42h) describing the BMC Redfish service
  reachable over the USB Ethernet gadget. Everything comes from build-time
  PCDs, so no NV variables and no platform HII are required.

  Ported from nuc-bios-build's NucRedfishHostInterfaceLib (the JetKVM
  CDC-ECM link); the transport here is the nanokvm BMC's CDC-NCM gadget,
  and the fixed values live in RpiRedfishPkg.dec / RpiRedfish.dsc.inc
  rather than compile-time #defines.

  The MAC is the contract's linchpin: RedfishDiscoverDxe rejects the
  interface unless the type 42 MAC byte-matches the real NIC MAC, so the
  BMC's gadget must present a FIXED host_addr equal to
  PcdRpiRedfishGadgetMac - which must also be encoded in the
  PcdRedfishRestExServiceDevicePath MAC node (RpiRedfish.dsc.inc renders
  both from one recipe variable).

  No service UUID is published (the field stays zero): this RedfishPkg
  generation does not correlate on it, and the nanokvm service root does
  not advertise one.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/RedfishHostInterfaceLib.h>
#include <Library/UefiLib.h>

//
// "255.255.255.255" + NUL.
//
#define RPI_REDFISH_HOSTNAME_MAX  16

#define RPI_REDFISH_MAC_LEN  6

/**
  Get platform Redfish host interface device descriptor.

  Returns a USB Network Interface V2 (device type 0x04) descriptor for the
  BMC's NCM gadget.

  @param[out] DeviceType        Pointer to retrieve device type.
  @param[out] DeviceDescriptor  Pointer to retrieve REDFISH_INTERFACE_DATA. Caller
                                frees with FreePool().

  @retval EFI_SUCCESS           Descriptor returned.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RedfishPlatformHostInterfaceDeviceDescriptor (
  OUT UINT8                   *DeviceType,
  OUT REDFISH_INTERFACE_DATA  **DeviceDescriptor
  )
{
  REDFISH_INTERFACE_DATA              *InterfaceData;
  USB_INTERFACE_DEVICE_DESCRIPTOR_V2  *Usb;
  CONST UINT8                         *Mac;

  if ((DeviceType == NULL) || (DeviceDescriptor == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (PcdGetSize (PcdRpiRedfishGadgetMac) != RPI_REDFISH_MAC_LEN) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishHostInterface: PcdRpiRedfishGadgetMac is not 6 bytes\n"));
    return EFI_UNSUPPORTED;
  }

  InterfaceData = AllocateZeroPool (sizeof (REDFISH_INTERFACE_DATA));
  if (InterfaceData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  InterfaceData->DeviceType = REDFISH_HOST_INTERFACE_DEVICE_TYPE_USB_V2;

  Mac = (CONST UINT8 *)PcdGetPtr (PcdRpiRedfishGadgetMac);

  Usb                  = &InterfaceData->DeviceDescriptor.UsbDeviceV2;
  Usb->Length          = USB_INTERFACE_DEVICE_DESCRIPTOR_V2_SIZE_1_3;  // 0x11
  Usb->IdVendor        = PcdGet16 (PcdRpiRedfishGadgetVendorId);
  Usb->IdProduct       = PcdGet16 (PcdRpiRedfishGadgetProductId);
  Usb->SerialNumberStr = 0;   // no serial-number string
  CopyMem (Usb->MacAddress, Mac, RPI_REDFISH_MAC_LEN);
  Usb->Characteristics               = 0;
  Usb->CredentialBootstrappingHandle = 0;

  *DeviceType       = REDFISH_HOST_INTERFACE_DEVICE_TYPE_USB_V2;
  *DeviceDescriptor = InterfaceData;

  return EFI_SUCCESS;
}

/**
  Get platform Redfish host interface protocol data.

  Produces exactly one "Redfish over IP" protocol record (index 0). Any other
  index returns EFI_NOT_FOUND to terminate the caller's enumeration.

  @param[in,out] ProtocolRecord     Pointer to retrieve the protocol record.
                                     Caller frees with FreePool().
  @param[in]     IndexOfProtocolData Index of the protocol data to return.

  @retval EFI_SUCCESS           Record returned.
  @retval EFI_NOT_FOUND         No more records.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
RedfishPlatformHostInterfaceProtocolData (
  IN OUT MC_HOST_INTERFACE_PROTOCOL_RECORD  **ProtocolRecord,
  IN UINT8                                  IndexOfProtocolData
  )
{
  MC_HOST_INTERFACE_PROTOCOL_RECORD  *Record;
  REDFISH_OVER_IP_PROTOCOL_DATA      *Data;
  CONST UINT8                        *HostIp;
  CONST UINT8                        *ServiceIp;
  CONST UINT8                        *Mask;
  CHAR8                              Hostname[RPI_REDFISH_HOSTNAME_MAX];
  UINT8                              HostNameSize;
  UINT8                              ProtocolDataSize;
  UINTN                              RecordSize;

  if (ProtocolRecord == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (IndexOfProtocolData != 0) {
    return EFI_NOT_FOUND;
  }

  if ((PcdGetSize (PcdRpiRedfishHostIp) != 4) ||
      (PcdGetSize (PcdRpiRedfishServiceIp) != 4) ||
      (PcdGetSize (PcdRpiRedfishIpMask) != 4))
  {
    DEBUG ((DEBUG_ERROR, "RpiRedfishHostInterface: IPv4 PCDs must be 4 bytes\n"));
    return EFI_UNSUPPORTED;
  }

  HostIp    = (CONST UINT8 *)PcdGetPtr (PcdRpiRedfishHostIp);
  ServiceIp = (CONST UINT8 *)PcdGetPtr (PcdRpiRedfishServiceIp);
  Mask      = (CONST UINT8 *)PcdGetPtr (PcdRpiRedfishIpMask);

  //
  // Hostname advertised in the Type 42h record. Kept identical to the
  // service IP so an HTTP client that skips DNS still gets a usable
  // Host: header. Include the terminating NUL in the length, matching the
  // reference implementations.
  //
  AsciiSPrint (
    Hostname,
    sizeof (Hostname),
    "%d.%d.%d.%d",
    ServiceIp[0],
    ServiceIp[1],
    ServiceIp[2],
    ServiceIp[3]
    );

  HostNameSize     = (UINT8)(AsciiStrLen (Hostname) + 1);
  ProtocolDataSize = (UINT8)(sizeof (REDFISH_OVER_IP_PROTOCOL_DATA) - 1 + HostNameSize);

  RecordSize = sizeof (MC_HOST_INTERFACE_PROTOCOL_RECORD) - 1 + ProtocolDataSize;
  Record     = AllocateZeroPool (RecordSize);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Record->ProtocolType        = MCHostInterfaceProtocolTypeRedfishOverIP;
  Record->ProtocolTypeDataLen = ProtocolDataSize;

  Data = (REDFISH_OVER_IP_PROTOCOL_DATA *)Record->ProtocolTypeData;

  //
  // ServiceUuid stays zero - no UUID contract on this link (see the file
  // header).
  //
  Data->HostIpAssignmentType = REDFISH_HOST_INTERFACE_HOST_IP_ASSIGNMENT_TYPE_STATIC; // 0x01
  Data->HostIpAddressFormat  = REDFISH_HOST_INTERFACE_HOST_IP_ADDRESS_FORMAT_IP4;     // 0x01
  CopyMem (Data->HostIpAddress, HostIp, 4);
  CopyMem (Data->HostIpMask, Mask, 4);

  Data->RedfishServiceIpDiscoveryType = REDFISH_HOST_INTERFACE_HOST_IP_ASSIGNMENT_TYPE_STATIC; // 0x01
  Data->RedfishServiceIpAddressFormat = REDFISH_HOST_INTERFACE_HOST_IP_ADDRESS_FORMAT_IP4;     // 0x01
  CopyMem (Data->RedfishServiceIpAddress, ServiceIp, 4);
  CopyMem (Data->RedfishServiceIpMask, Mask, 4);

  Data->RedfishServiceIpPort         = PcdGet16 (PcdRedfishServicePort);
  Data->RedfishServiceVlanId         = 0xFFFFFFFF;
  Data->RedfishServiceHostnameLength = HostNameSize;
  AsciiStrCpyS (
    (CHAR8 *)Data->RedfishServiceHostname,
    HostNameSize,
    Hostname
    );

  *ProtocolRecord = Record;
  return EFI_SUCCESS;
}

/**
  Notification GUID is not used: the SMBIOS Type 42h record can be built
  immediately because all data is static.

  @param[out] InformationReadinessGuid  Unused.

  @retval EFI_UNSUPPORTED  Notification not required.
**/
EFI_STATUS
RedfishPlatformHostInterfaceNotification (
  OUT EFI_GUID  **InformationReadinessGuid
  )
{
  return EFI_UNSUPPORTED;
}

/**
  No USB serial-number string is exposed.

  @param[out] SerialNumber  Unused.

  @retval EFI_UNSUPPORTED  Serial number not available.
**/
EFI_STATUS
RedfishPlatformHostInterfaceSerialNumber (
  OUT CHAR8  **SerialNumber
  )
{
  return EFI_UNSUPPORTED;
}

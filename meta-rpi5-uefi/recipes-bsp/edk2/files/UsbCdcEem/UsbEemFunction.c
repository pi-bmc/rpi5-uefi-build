/** @file
  This file contains code for the USB CDC EEM descriptor and bulk-transfer
  implementation of EDKII_USB_ETHERNET_PROTOCOL.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbCdcEem.h"

/**
  Load the entire configuration descriptor for the device.

  @param[in]      UsbIo       A pointer to the EFI_USB_IO_PROTOCOL instance.
  @param[in, out] ConfigDesc  On output, the allocated configuration descriptor.
                              Caller frees it.

  @retval EFI_SUCCESS           The request executed successfully.
  @retval EFI_OUT_OF_RESOURCES  The configuration descriptor could not be
                                allocated.
  @retval EFI_TIMEOUT           A timeout occurred executing the request.
  @retval EFI_DEVICE_ERROR      The request failed due to a device error.

**/
EFI_STATUS
LoadAllDescriptor (
  IN     EFI_USB_IO_PROTOCOL        *UsbIo,
  IN OUT EFI_USB_CONFIG_DESCRIPTOR  **ConfigDesc
  )
{
  EFI_STATUS                 Status;
  UINT32                     TransStatus;
  EFI_USB_CONFIG_DESCRIPTOR  Tmp;

  Status = UsbIo->UsbGetConfigDescriptor (UsbIo, &Tmp);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->AllocatePool (EfiBootServicesData, Tmp.TotalLength, (VOID **)ConfigDesc);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UsbGetDescriptor (
             UsbIo,
             USB_DESC_TYPE_CONFIG << 8 | (Tmp.ConfigurationValue - 1), // zero based
             0,
             Tmp.TotalLength,
             *ConfigDesc,
             &TransStatus
             );
  return Status;
}

/**
  Locate the two bulk endpoints of the single CDC EEM interface.

  EEM has no interrupt endpoint and no alternate settings to probe, unlike
  UsbCdcEcm's GetEndpoint: this interface always presents both of its
  endpoints on the current (and only) alternate setting.

  @param[in]      UsbIo         A pointer to the EFI_USB_IO_PROTOCOL instance.
  @param[in, out] UsbEthDriver  The driver context to fill in with endpoint
                                addresses.

**/
VOID
GetEndpoint (
  IN     EFI_USB_IO_PROTOCOL  *UsbIo,
  IN OUT USB_ETHERNET_DRIVER  *UsbEthDriver
  )
{
  EFI_STATUS                    Status;
  UINT8                         Index;
  EFI_USB_INTERFACE_DESCRIPTOR  Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR   Endpoint;

  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &Interface);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Interface.NumEndpoints; Index++) {
    Status = UsbIo->UsbGetEndpointDescriptor (UsbIo, Index, &Endpoint);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((Endpoint.Attributes & (BIT0 | BIT1)) != USB_ENDPOINT_BULK) {
      continue;
    }

    if ((Endpoint.EndpointAddress & BIT7) != 0) {
      UsbEthDriver->BulkInEndpoint = Endpoint.EndpointAddress;
    } else {
      UsbEthDriver->BulkOutEndpoint = Endpoint.EndpointAddress;
    }
  }
}

/**
  Receive one Ethernet frame.

  One bulk transfer may carry several EEM data packets bundled together, and
  NetworkCommon wants exactly one Ethernet frame per call, so the staged
  transfer is walked with a byte cursor (UsbEthDriver->RxOffset) and refilled
  with a new bulk transfer only once it is exhausted.

  @param[in]      Cdb           A pointer to the command descriptor block.
  @param[in]      This          A pointer to the EDKII_USB_ETHERNET_PROTOCOL
                                instance.
  @param[in, out] Packet        On output, one Ethernet frame.
  @param[in, out] PacketLength  On input, the size of Packet. On output, the
                                length of the frame written to Packet.

  @retval EFI_SUCCESS      One frame was copied to Packet.
  @retval EFI_NOT_READY    No frame is available right now.
  @retval other            The underlying bulk transfer failed.

**/
EFI_STATUS
EFIAPI
UsbEthEemReceive (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN OUT VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  )
{
  EFI_STATUS           Status;
  USB_ETHERNET_DRIVER  *UsbEthDriver;
  UINT32               TransStatus;
  UINTN                BulkDataLength;
  CONST UINT8          *Frame;
  UINTN                FrameLen;

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);

  if (UsbEthDriver->BulkInEndpoint == 0) {
    GetEndpoint (UsbEthDriver->UsbIo, UsbEthDriver);
  }

  //
  // Refill only when the previous transfer is walked out.
  //
  if (UsbEthDriver->RxOffset >= UsbEthDriver->RxLength) {
    BulkDataLength         = USB_EEM_MAX_BULK_SIZE;
    UsbEthDriver->RxOffset = 0;
    UsbEthDriver->RxLength = 0;

    Status = UsbEthDriver->UsbIo->UsbBulkTransfer (
                                    UsbEthDriver->UsbIo,
                                    UsbEthDriver->BulkInEndpoint,
                                    UsbEthDriver->RxBuffer,
                                    &BulkDataLength,
                                    USB_ETHERNET_BULK_TIMEOUT,
                                    &TransStatus
                                    );
    if (EFI_ERROR (Status)) {
      //
      // An empty poll is the normal case on an idle link, not a fault.
      //
      return (Status == EFI_TIMEOUT) ? EFI_NOT_READY : Status;
    }

    UsbEthDriver->RxLength = BulkDataLength;
  }

  Status = UsbEemNextRxFrame (
             UsbEthDriver->RxBuffer,
             UsbEthDriver->RxLength,
             &UsbEthDriver->RxOffset,
             &Frame,
             &FrameLen
             );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_READY;
  }

  if (FrameLen > *PacketLength) {
    return EFI_NOT_READY;
  }

  CopyMem (Packet, Frame, FrameLen);
  *PacketLength = FrameLen;

  return EFI_SUCCESS;
}

/**
  Transmit one Ethernet frame, wrapped in an EEM data packet.

  @param[in]      Cdb           A pointer to the command descriptor block.
  @param[in]      This          A pointer to the EDKII_USB_ETHERNET_PROTOCOL
                                instance.
  @param[in]      Packet        The Ethernet frame to send.
  @param[in, out] PacketLength  The length of Packet.

  @retval EFI_SUCCESS      The frame was wrapped and transmitted.
  @retval other            UsbEemFrameTx or the underlying bulk transfer
                           failed.

**/
EFI_STATUS
EFIAPI
UsbEthEemTransmit (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN     VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  )
{
  EFI_STATUS           Status;
  USB_ETHERNET_DRIVER  *UsbEthDriver;
  UINT32               TransStatus;
  UINTN                WrappedLength;

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);

  if (UsbEthDriver->BulkOutEndpoint == 0) {
    GetEndpoint (UsbEthDriver->UsbIo, UsbEthDriver);
  }

  Status = UsbEemFrameTx (
             UsbEthDriver->TxBuffer,
             USB_EEM_MAX_BULK_SIZE,
             (CONST UINT8 *)Packet,
             *PacketLength,
             &WrappedLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UsbEthDriver->UsbIo->UsbBulkTransfer (
                                  UsbEthDriver->UsbIo,
                                  UsbEthDriver->BulkOutEndpoint,
                                  UsbEthDriver->TxBuffer,
                                  &WrappedLength,
                                  USB_ETHERNET_TRANSFER_TIMEOUT,
                                  &TransStatus
                                  );
  return Status;
}

/**
  EEM has no notification interface, so there is no interrupt endpoint to
  arm and no link-state or speed change will ever be reported. Succeed
  silently: NetworkCommon calls this under ASSERT_EFI_ERROR, so returning
  EFI_UNSUPPORTED would assert every DEBUG boot.

  This is also how the contract's two consequences are satisfied, and both
  are satisfied by inaction rather than by code:

    * Link-up is assumed on enumeration -- NetworkCommon patch 0100 already
      defaults CableDetect to 1 and makes it sticky, so nothing here waits
      for a NETWORK_CONNECTION notification that can never arrive.
    * The link speed is fixed -- ECM and NCM only ever change the reported
      speed on a CONNECTION_SPEED_CHANGE notification, so a driver that
      never delivers one reports a constant speed for the life of the link.
**/
EFI_STATUS
EFIAPI
UsbEthEemInterrupt (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN BOOLEAN                      IsNewTransfer,
  IN UINTN                        PollingInterval,
  IN EFI_USB_DEVICE_REQUEST       *Request
  )
{
  return EFI_SUCCESS;
}

/**
  The station address comes from a build-time PCD: EEM carries no Ethernet
  functional descriptor, so unlike ECM/NCM there is no iMACAddress to read.
  The BMC identifies the Redfish host interface by this address, so it is a
  contract value, not a discovered one.
**/
EFI_STATUS
EFIAPI
GetUsbEthMacAddress (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS              *MacAddress
  )
{
  USB_ETHERNET_DRIVER  *UsbEthDriver;

  if ((This == NULL) || (MacAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);
  CopyMem (MacAddress, &UsbEthDriver->MacAddress, sizeof (EFI_MAC_ADDRESS));

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UsbEthEemBulkSize (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT UINTN                        *BulkSize
  )
{
  if (BulkSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *BulkSize = USB_EEM_MAX_FRAME;
  return EFI_SUCCESS;
}

/**
  Synthesized: EEM sends no CDC Ethernet functional descriptor at all.

  NumberMcFilters is reported as 0 deliberately. PxeFunction.c branches on
  it, and zero selects the RECEIVE_FILTER_ALL_MULTICAST path, which never
  calls SetUsbEthMcastFilter. A point-to-point link has no use for hardware
  multicast filtering.
**/
EFI_STATUS
EFIAPI
GetUsbEthFunDescriptor (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT USB_ETHERNET_FUN_DESCRIPTOR  *UsbEthFunDescriptor
  )
{
  if (UsbEthFunDescriptor == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (UsbEthFunDescriptor, sizeof (USB_ETHERNET_FUN_DESCRIPTOR));

  UsbEthFunDescriptor->FunctionLength = sizeof (USB_ETHERNET_FUN_DESCRIPTOR);
  //
  // 0x24 is CS_INTERFACE. The pinned tree defines ETHERNET_FUN_DESCRIPTOR
  // (0x0F) but has no CS_INTERFACE macro, and nothing re-parses this as a
  // real USB descriptor -- NetworkCommon reads only NumberMcFilters -- so the
  // literal is honest rather than inventing a macro upstream does not have.
  //
  UsbEthFunDescriptor->DescriptorType     = 0x24;
  UsbEthFunDescriptor->DescriptorSubtype  = ETHERNET_FUN_DESCRIPTOR;
  UsbEthFunDescriptor->MaxSegmentSize     = USB_EEM_MAX_FRAME;
  UsbEthFunDescriptor->NumberMcFilters    = 0;
  UsbEthFunDescriptor->NumberPowerFilters = 0;

  return EFI_SUCCESS;
}

/**
  There is no control interface to send SetEthernetPacketFilter to, and the
  peer is a point-to-point gadget that filters nothing. Accept and ignore.
**/
EFI_STATUS
EFIAPI
SetUsbEthPacketFilter (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN UINT16                       Value
  )
{
  return EFI_SUCCESS;
}

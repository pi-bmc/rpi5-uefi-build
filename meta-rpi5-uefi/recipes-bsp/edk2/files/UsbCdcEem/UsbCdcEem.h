/** @file
  Header for the USB CDC Ethernet Emulation Model (EEM) driver.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiUsbLib.h>
#include <Protocol/UsbIo.h>
#include <Protocol/UsbEthernetProtocol.h>

#include "UsbEemFraming.h"

//
// CDC EEM is a single interface carrying two bulk endpoints: no control
// interface, no CDC functional descriptors, no notification endpoint and no
// alternate settings. That is the whole reason it is used here -- it costs
// one device IN endpoint where ECM/NCM/RNDIS cost two, and the BMC's dwc2
// core has exactly six to spend.
//
#define USB_CDC_EEM_SUBCLASS  0x0C
#define USB_CDC_EEM_PROTOCOL  0x07

#define USB_EEM_DRIVER_VERSION         1
#define USB_ETHERNET_BULK_TIMEOUT      1
#define USB_ETHERNET_TRANSFER_TIMEOUT  200

//
// Six, spelled locally rather than as NetLib's NET_ETHER_ADDR_LEN: NetLib is
// a NetworkPkg library class and none of the other class drivers in this
// directory depend on it.
//
#define USB_EEM_MAC_LEN  6

typedef struct {
  UINTN                          Signature;
  EDKII_USB_ETHERNET_PROTOCOL    UsbEth;
  EFI_USB_IO_PROTOCOL            *UsbIo;
  EFI_USB_CONFIG_DESCRIPTOR      *Config;
  UINT8                          NumOfInterface;
  UINT8                          BulkInEndpoint;
  UINT8                          BulkOutEndpoint;
  EFI_MAC_ADDRESS                MacAddress;

  //
  // Receive staging. One bulk transfer may carry several EEM packets, and
  // NetworkCommon wants exactly one Ethernet frame per call, so the buffer
  // is walked with a byte cursor across calls.
  //
  // A cursor rather than a datagram index pair on purpose: the index
  // arithmetic UsbCdcNcm uses is what produced its underflow-to-255 bug.
  //
  UINT8    *RxBuffer;
  UINTN    RxLength;
  UINTN    RxOffset;

  //
  // Transmit staging, so a frame can be wrapped without allocating per send.
  //
  UINT8    *TxBuffer;
} USB_ETHERNET_DRIVER;

#define USB_EEM_SIGNATURE  SIGNATURE_32('u','e','e','m')
#define USB_EEM_DEV_FROM_THIS(a)  CR (a, USB_ETHERNET_DRIVER, UsbEth, USB_EEM_SIGNATURE)

extern EFI_COMPONENT_NAME2_PROTOCOL  gUsbEemComponentName2;

EFI_STATUS
EFIAPI
UsbEemDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UsbEemDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UsbEemDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  );

VOID
GetEndpoint (
  IN     EFI_USB_IO_PROTOCOL  *UsbIo,
  IN OUT USB_ETHERNET_DRIVER  *UsbEthDriver
  );

EFI_STATUS
LoadAllDescriptor (
  IN     EFI_USB_IO_PROTOCOL        *UsbIo,
  IN OUT EFI_USB_CONFIG_DESCRIPTOR  **ConfigDesc
  );

EFI_STATUS
EFIAPI
UsbEthEemReceive (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN OUT VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  );

EFI_STATUS
EFIAPI
UsbEthEemTransmit (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN     VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  );

EFI_STATUS
EFIAPI
UsbEthEemInterrupt (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN BOOLEAN                      IsNewTransfer,
  IN UINTN                        PollingInterval,
  IN EFI_USB_DEVICE_REQUEST       *Request
  );

EFI_STATUS
EFIAPI
GetUsbEthMacAddress (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS              *MacAddress
  );

EFI_STATUS
EFIAPI
UsbEthEemBulkSize (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT UINTN                        *BulkSize
  );

EFI_STATUS
EFIAPI
GetUsbEthFunDescriptor (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT USB_ETHERNET_FUN_DESCRIPTOR  *UsbEthFunDescriptor
  );

EFI_STATUS
EFIAPI
SetUsbEthPacketFilter (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN UINT16                       Value
  );

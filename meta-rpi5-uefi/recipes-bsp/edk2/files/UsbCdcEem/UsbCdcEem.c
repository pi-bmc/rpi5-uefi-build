/** @file
  This file contains code for the USB CDC Ethernet Emulation Model (EEM)
  driver binding.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "UsbCdcEem.h"

EFI_DRIVER_BINDING_PROTOCOL  gUsbEemDriverBinding = {
  UsbEemDriverSupported,
  UsbEemDriverStart,
  UsbEemDriverStop,
  USB_EEM_DRIVER_VERSION,
  NULL,
  NULL
};

/**
  Check if this interface is the USB CDC EEM SubClass/Protocol.

  Unlike ECM/NCM, EEM has no companion CDC Data interface to pair with: the
  single interface tested here carries both bulk endpoints, so a positive
  match is sufficient to bind.

  @param[in]  UsbIo     A pointer to the EFI_USB_IO_PROTOCOL instance.

  @retval TRUE          USB CDC EEM SubClass/Protocol.
  @retval FALSE         Not USB CDC EEM SubClass/Protocol.

**/
BOOLEAN
IsSupportedDevice (
  IN EFI_USB_IO_PROTOCOL  *UsbIo
  )
{
  EFI_STATUS                    Status;
  EFI_USB_INTERFACE_DESCRIPTOR  InterfaceDescriptor;

  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &InterfaceDescriptor);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if ((InterfaceDescriptor.InterfaceClass == USB_CDC_CLASS) &&
      (InterfaceDescriptor.InterfaceSubClass == USB_CDC_EEM_SUBCLASS) &&
      (InterfaceDescriptor.InterfaceProtocol == USB_CDC_EEM_PROTOCOL))
  {
    return TRUE;
  }

  return FALSE;
}

/**
  USB CDC EEM Driver Binding Support.

  @param[in]  This                  Protocol instance pointer.
  @param[in]  ControllerHandle      Handle of device to test.
  @param[in]  RemainingDevicePath   Optional parameter use to pick a specific child
                                    device to start.

  @retval EFI_SUCCESS               This driver supports this device.
  @retval EFI_ALREADY_STARTED       This driver is already running on this device.
  @retval other                     This driver does not support this device.

**/
EFI_STATUS
EFIAPI
UsbEemDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_USB_IO_PROTOCOL  *UsbIo;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&UsbIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = IsSupportedDevice (UsbIo) ? EFI_SUCCESS : EFI_UNSUPPORTED;

  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );
  return Status;
}

/**
  USB CDC EEM Driver Binding Start.

  There is no companion CDC Data interface to wait for, so unlike UsbCdcEcm
  this never defers via RegisterProtocolNotify: the single interface tested
  by UsbEemDriverSupported already carries both bulk endpoints, so binding
  either succeeds now or fails outright.

  @param[in]  This                    Protocol instance pointer.
  @param[in]  ControllerHandle        Handle of device to bind driver to.
  @param[in]  RemainingDevicePath     Optional parameter use to pick a specific child
                                      device to start.

  @retval EFI_SUCCESS                 This driver is added to ControllerHandle
  @retval EFI_OUT_OF_RESOURCES        The driver could not install successfully due to a lack of resources.
  @retval EFI_UNSUPPORTED             The station address PCD is unset, zero-length, or all zero.
  @retval other                       This driver does not support this device

**/
EFI_STATUS
EFIAPI
UsbEemDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                    Status;
  USB_ETHERNET_DRIVER           *UsbEthDriver;
  EFI_USB_IO_PROTOCOL           *UsbIo;
  EFI_USB_INTERFACE_DESCRIPTOR  Interface;
  UINT8                         *MacPcd;
  UINTN                         MacLen;

  UsbEthDriver = NULL;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&UsbIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UsbEthDriver = AllocateZeroPool (sizeof (USB_ETHERNET_DRIVER));
  if (UsbEthDriver == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  Status = LoadAllDescriptor (UsbIo, &UsbEthDriver->Config);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  GetEndpoint (UsbIo, UsbEthDriver);

  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &Interface);
  ASSERT_EFI_ERROR (Status);

  UsbEthDriver->RxBuffer = AllocatePool (USB_EEM_MAX_BULK_SIZE);
  if (UsbEthDriver->RxBuffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  UsbEthDriver->TxBuffer = AllocatePool (USB_EEM_MAX_BULK_SIZE);
  if (UsbEthDriver->TxBuffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  MacPcd = (UINT8 *)PcdGetPtr (PcdUsbCdcEemMacAddress);
  MacLen = PcdGetSize (PcdUsbCdcEemMacAddress);

  //
  // An all-zero or wrong-sized PCD means the platform never set the station
  // address. A NIC with an invalid MAC would fail Redfish discovery anyway,
  // so decline to bind: a missing interface is diagnosable, a mystery MAC
  // is not.
  //
  if ((MacPcd == NULL) || (MacLen < USB_EEM_MAC_LEN)) {
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }

  ZeroMem (&UsbEthDriver->MacAddress, sizeof (EFI_MAC_ADDRESS));
  CopyMem (&UsbEthDriver->MacAddress, MacPcd, USB_EEM_MAC_LEN);

  if (IsZeroBuffer (&UsbEthDriver->MacAddress, USB_EEM_MAC_LEN)) {
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }

  UsbEthDriver->Signature                    = USB_EEM_SIGNATURE;
  UsbEthDriver->NumOfInterface               = Interface.InterfaceNumber;
  UsbEthDriver->UsbIo                        = UsbIo;
  UsbEthDriver->UsbEth.UsbEthReceive         = UsbEthEemReceive;
  UsbEthDriver->UsbEth.UsbEthTransmit        = UsbEthEemTransmit;
  UsbEthDriver->UsbEth.UsbEthInterrupt       = UsbEthEemInterrupt;
  UsbEthDriver->UsbEth.UsbEthMacAddress      = GetUsbEthMacAddress;
  UsbEthDriver->UsbEth.UsbEthMaxBulkSize     = UsbEthEemBulkSize;
  UsbEthDriver->UsbEth.UsbEthFunDescriptor   = GetUsbEthFunDescriptor;
  UsbEthDriver->UsbEth.SetUsbEthPacketFilter = SetUsbEthPacketFilter;

  Status = gBS->InstallProtocolInterface (
                  &ControllerHandle,
                  &gEdkIIUsbEthProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &(UsbEthDriver->UsbEth)
                  );
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  return EFI_SUCCESS;

ErrorExit:
  if (UsbEthDriver != NULL) {
    if (UsbEthDriver->TxBuffer != NULL) {
      FreePool (UsbEthDriver->TxBuffer);
    }

    if (UsbEthDriver->RxBuffer != NULL) {
      FreePool (UsbEthDriver->RxBuffer);
    }

    if (UsbEthDriver->Config != NULL) {
      FreePool (UsbEthDriver->Config);
    }

    FreePool (UsbEthDriver);
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

/**
  USB CDC EEM Driver Binding Stop.

  @param[in]  This                Protocol instance pointer.
  @param[in]  ControllerHandle    Handle of device to stop driver on
  @param[in]  NumberOfChildren    Number of Handles in ChildHandleBuffer. If number of
                                  children is zero stop the entire bus driver.
  @param[in]  ChildHandleBuffer   List of Child Handles to Stop.

  @retval EFI_SUCCESS             This driver is removed ControllerHandle
  @retval other                   This driver was not removed from this device

**/
EFI_STATUS
EFIAPI
UsbEemDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                   Status;
  EDKII_USB_ETHERNET_PROTOCOL  *UsbEthProtocol;
  USB_ETHERNET_DRIVER          *UsbEthDriver;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkIIUsbEthProtocolGuid,
                  (VOID **)&UsbEthProtocol,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (UsbEthProtocol);

  Status = gBS->UninstallProtocolInterface (
                  ControllerHandle,
                  &gEdkIIUsbEthProtocolGuid,
                  UsbEthProtocol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gEfiUsbIoProtocolGuid,
                  This->DriverBindingHandle,
                  ControllerHandle
                  );

  if (UsbEthDriver->TxBuffer != NULL) {
    FreePool (UsbEthDriver->TxBuffer);
  }

  if (UsbEthDriver->RxBuffer != NULL) {
    FreePool (UsbEthDriver->RxBuffer);
  }

  if (UsbEthDriver->Config != NULL) {
    FreePool (UsbEthDriver->Config);
  }

  FreePool (UsbEthDriver);
  return Status;
}

/**
  Entrypoint of EEM Driver.

  This function is the entrypoint of EEM Driver. It installs Driver Binding
  Protocols together with Component Name Protocols.

  @param[in]  ImageHandle       The firmware allocated handle for the EFI image.
  @param[in]  SystemTable       A pointer to the EFI System Table.

  @retval EFI_SUCCESS           The entry point is executed successfully.

**/
EFI_STATUS
EFIAPI
UsbEemEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  gUsbEemDriverBinding.DriverBindingHandle = ImageHandle;
  gUsbEemDriverBinding.ImageHandle         = ImageHandle;

  return gBS->InstallMultipleProtocolInterfaces (
                &gUsbEemDriverBinding.DriverBindingHandle,
                &gEfiDriverBindingProtocolGuid,
                &gUsbEemDriverBinding,
                &gEfiComponentName2ProtocolGuid,
                &gUsbEemComponentName2,
                NULL
                );
}

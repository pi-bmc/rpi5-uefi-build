/** @file

  Driver binding for the RP1 GEM SNP driver.

  Binds to the vendor NON_DISCOVERABLE_DEVICE installed by Rp1BusDxe
  (Type == gRp1GemNonDiscoverableDeviceGuid), takes ownership of the two
  MMIO resources (GEM core + eth_cfg wrapper) and installs
  EFI_SIMPLE_NETWORK_PROTOCOL on a new child handle whose device path is
  the parent's VenHw path with a MAC_ADDR_DEVICE_PATH node appended (so
  BDS can create PXE/HTTP boot options).

  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1GemDxe.h"

#include <Protocol/RpiFirmware.h>

/**
  Fetch the station MAC address: primary source is the VideoCore firmware
  mailbox (GetMacAddress); fallback is a locally administered address
  derived from the board serial number; last resort is a fixed locally
  administered address.

  @param  MacAddress[out]  Station address (6 bytes valid).

**/
STATIC
VOID
Rp1GemGetMacAddress (
  OUT EFI_MAC_ADDRESS  *MacAddress
  )
{
  EFI_STATUS                      Status;
  RASPBERRY_PI_FIRMWARE_PROTOCOL  *Firmware;
  UINT8                           Mac[6];
  UINT64                          Serial;

  ZeroMem (MacAddress, sizeof (EFI_MAC_ADDRESS));

  Firmware = NULL;
  Status   = gBS->LocateProtocol (
                    &gRaspberryPiFirmwareProtocolGuid,
                    NULL,
                    (VOID **)&Firmware
                    );
  if (!EFI_ERROR (Status)) {
    Status = Firmware->GetMacAddress (Mac);
    if (!EFI_ERROR (Status) &&
        ((Mac[0] | Mac[1] | Mac[2] | Mac[3] | Mac[4] | Mac[5]) != 0) &&
        ((Mac[0] & BIT0) == 0))
    {
      CopyMem (MacAddress->Addr, Mac, sizeof (Mac));
      return;
    }

    DEBUG ((
      DEBUG_WARN,
      "Rp1GemDxe: firmware MAC unavailable (%r), deriving from serial\n",
      Status
      ));

    Serial = 0;
    Status = Firmware->GetSerial (&Serial);
    if (!EFI_ERROR (Status) && (Serial != 0)) {
      //
      // Locally administered, unicast (x2-xx-xx-xx-xx-xx).
      //
      MacAddress->Addr[0] = 0x02;
      MacAddress->Addr[1] = (UINT8)(Serial >> 32);
      MacAddress->Addr[2] = (UINT8)(Serial >> 24);
      MacAddress->Addr[3] = (UINT8)(Serial >> 16);
      MacAddress->Addr[4] = (UINT8)(Serial >> 8);
      MacAddress->Addr[5] = (UINT8)Serial;
      return;
    }
  }

  DEBUG ((
    DEBUG_WARN,
    "Rp1GemDxe: no firmware MAC source, using fixed local address\n"
    ));

  MacAddress->Addr[0] = 0x02;
  MacAddress->Addr[1] = 0x52;  // 'R'
  MacAddress->Addr[2] = 0x50;  // 'P'
  MacAddress->Addr[3] = 0x31;  // '1'
  MacAddress->Addr[4] = 0x47;  // 'G'
  MacAddress->Addr[5] = 0x4D;  // 'M'
}

/**
  Parse the two MMIO resources (GEM core, eth_cfg wrapper) out of the
  NON_DISCOVERABLE_DEVICE resource list.

  @param  Gem[in]  Driver private data with Dev already set.

  @retval EFI_SUCCESS       Both resources parsed.
  @retval EFI_UNSUPPORTED   The resource list does not match expectations.

**/
STATIC
EFI_STATUS
Rp1GemParseResources (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  UINTN                              Count;

  Gem->GemBase    = 0;
  Gem->EthCfgBase = 0;
  Count           = 0;

  for (Desc = Gem->Dev->Resources;
       Desc->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR;
       Desc = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)((UINT8 *)Desc +
                                                    Desc->Len + 3))
  {
    if (Desc->ResType != ACPI_ADDRESS_SPACE_TYPE_MEM) {
      return EFI_UNSUPPORTED;
    }

    if (Count == 0) {
      Gem->GemBase = Desc->AddrRangeMin;
    } else if (Count == 1) {
      Gem->EthCfgBase = Desc->AddrRangeMin;
    }

    Count++;
  }

  if ((Count != 2) || (Gem->GemBase == 0) || (Gem->EthCfgBase == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: unexpected resource layout (%u entries)\n",
      (UINT32)Count
      ));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Callback to quiesce the controller at ExitBootServices so the OS driver
  finds the DMA engines stopped.

  @param  Event[in]    The event that fired.
  @param  Context[in]  Driver private data.

**/
STATIC
VOID
EFIAPI
Rp1GemNotifyExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  Gem = (RP1_GEM_PRIVATE_DATA *)Context;

  MmioWrite32 ((UINTN)Gem->GemBase + GEM_NET_CTRL, 0);
}

/**
  Tests to see if this driver supports a given controller.

  @param  This[in]                 EFI_DRIVER_BINDING_PROTOCOL instance.
  @param  ControllerHandle[in]     Handle of the controller to test.
  @param  RemainingDevicePath[in]  Ignored.

  @retval EFI_SUCCESS         The controller is the RP1 GEM device.
  @retval EFI_ALREADY_STARTED The controller is already being managed.
  @retval EFI_UNSUPPORTED     The controller is not supported.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  NON_DISCOVERABLE_DEVICE  *Dev;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Dev,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!CompareGuid (Dev->Type, &gRp1GemNonDiscoverableDeviceGuid)) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

/**
  Starts the driver on the RP1 GEM controller handle.

  @param  This[in]                 EFI_DRIVER_BINDING_PROTOCOL instance.
  @param  ControllerHandle[in]     Handle of the device to start.
  @param  RemainingDevicePath[in]  Ignored.

  @retval EFI_SUCCESS           The driver was started.
  @retval EFI_OUT_OF_RESOURCES  Allocation failure.
  @retval other                 Start failure.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath  OPTIONAL
  )
{
  EFI_STATUS                Status;
  RP1_GEM_PRIVATE_DATA      *Gem;
  EFI_DEVICE_PATH_PROTOCOL  *ParentDevicePath;
  MAC_ADDR_DEVICE_PATH      MacNode;
  VOID                      *Dummy;
  BOOLEAN                   DmaAllocated;

  DmaAllocated = FALSE;

  Gem = AllocateZeroPool (sizeof (RP1_GEM_PRIVATE_DATA));
  if (Gem == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Gem->Dev,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Gem);
    return Status;
  }

  if (!CompareGuid (Gem->Dev->Type, &gRp1GemNonDiscoverableDeviceGuid)) {
    Status = EFI_UNSUPPORTED;
    goto CloseProtocol;
  }

  Status = Rp1GemParseResources (Gem);
  if (EFI_ERROR (Status)) {
    goto CloseProtocol;
  }

  Gem->Signature           = RP1_GEM_SIGNATURE;
  Gem->ControllerHandle    = ControllerHandle;
  Gem->DriverBindingHandle = This->DriverBindingHandle;

  //
  // Quiesce the controller right away: a previous bootloader stage may
  // have left the DMA engines running on its own rings.
  //
  GemMacReset (Gem);

  Status = GemDmaAlloc (Gem);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: DMA buffer allocation failed: %r\n",
      Status
      ));
    goto CloseProtocol;
  }

  DmaAllocated = TRUE;

  EfiInitializeLock (&Gem->Lock, TPL_CALLBACK);

  CopyMem (&Gem->Snp, &gRp1GemSimpleNetworkTemplate, sizeof (Gem->Snp));
  Gem->Snp.Mode = &Gem->SnpMode;

  Gem->SnpMode.State             = EfiSimpleNetworkStopped;
  Gem->SnpMode.HwAddressSize     = GEM_ETHER_ADDR_LEN;
  Gem->SnpMode.MediaHeaderSize   = GEM_ETHER_HEADER_SIZE;
  Gem->SnpMode.MaxPacketSize     = GEM_ETHER_MTU;
  Gem->SnpMode.NvRamSize         = 0;
  Gem->SnpMode.NvRamAccessSize   = 0;
  Gem->SnpMode.ReceiveFilterMask = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                                   EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST |
                                   EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
                                   EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS |
                                   EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST;
  Gem->SnpMode.ReceiveFilterSetting = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                                      EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
  Gem->SnpMode.MaxMCastFilterCount   = MAX_MCAST_FILTER_CNT;
  Gem->SnpMode.MCastFilterCount      = 0;
  Gem->SnpMode.IfType                = 1;   // Ethernet
  Gem->SnpMode.MacAddressChangeable  = TRUE;
  Gem->SnpMode.MultipleTxSupported   = TRUE;
  Gem->SnpMode.MediaPresentSupported = TRUE;
  Gem->SnpMode.MediaPresent          = FALSE;

  SetMem (&Gem->SnpMode.BroadcastAddress, sizeof (EFI_MAC_ADDRESS), 0xFF);

  Rp1GemGetMacAddress (&Gem->SnpMode.PermanentAddress);
  CopyMem (
    &Gem->SnpMode.CurrentAddress,
    &Gem->SnpMode.PermanentAddress,
    sizeof (EFI_MAC_ADDRESS)
    );

  DEBUG ((
    DEBUG_INFO,
    "Rp1GemDxe: GEM at 0x%lx (module ID 0x%x), MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
    Gem->GemBase,
    MmioRead32 ((UINTN)Gem->GemBase + GEM_MODULE_ID),
    Gem->SnpMode.CurrentAddress.Addr[0],
    Gem->SnpMode.CurrentAddress.Addr[1],
    Gem->SnpMode.CurrentAddress.Addr[2],
    Gem->SnpMode.CurrentAddress.Addr[3],
    Gem->SnpMode.CurrentAddress.Addr[4],
    Gem->SnpMode.CurrentAddress.Addr[5]
    ));

  //
  // Build the child device path: parent VenHw path + MAC node.
  //
  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&ParentDevicePath
                  );
  if (EFI_ERROR (Status)) {
    goto CloseProtocol;
  }

  ZeroMem (&MacNode, sizeof (MacNode));
  MacNode.Header.Type    = MESSAGING_DEVICE_PATH;
  MacNode.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&MacNode.Header, sizeof (MAC_ADDR_DEVICE_PATH));
  CopyMem (
    &MacNode.MacAddress,
    &Gem->SnpMode.CurrentAddress,
    sizeof (EFI_MAC_ADDRESS)
    );
  MacNode.IfType = Gem->SnpMode.IfType;

  Gem->DevicePath = AppendDevicePathNode (ParentDevicePath, &MacNode.Header);
  if (Gem->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseProtocol;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_CALLBACK,
                  Rp1GemWaitForPacketNotify,
                  Gem,
                  &Gem->Snp.WaitForPacket
                  );
  if (EFI_ERROR (Status)) {
    goto FreeDevicePath;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  Rp1GemNotifyExitBootServices,
                  Gem,
                  &gEfiEventExitBootServicesGuid,
                  &Gem->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    goto CloseWaitEvent;
  }

  Gem->ChildHandle = NULL;
  Status           = gBS->InstallMultipleProtocolInterfaces (
                            &Gem->ChildHandle,
                            &gEfiDevicePathProtocolGuid,
                            Gem->DevicePath,
                            &gEfiSimpleNetworkProtocolGuid,
                            &Gem->Snp,
                            NULL
                            );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: failed to install SNP on child handle: %r\n",
      Status
      ));
    goto CloseExitBootServicesEvent;
  }

  //
  // Establish the parent-child link required by the UEFI driver model.
  //
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  &Dummy,
                  This->DriverBindingHandle,
                  Gem->ChildHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: failed to open parent by child: %r\n",
      Status
      ));
    goto UninstallProtocols;
  }

  return EFI_SUCCESS;

UninstallProtocols:
  gBS->UninstallMultipleProtocolInterfaces (
         Gem->ChildHandle,
         &gEfiDevicePathProtocolGuid,
         Gem->DevicePath,
         &gEfiSimpleNetworkProtocolGuid,
         &Gem->Snp,
         NULL
         );

CloseExitBootServicesEvent:
  gBS->CloseEvent (Gem->ExitBootServicesEvent);

CloseWaitEvent:
  gBS->CloseEvent (Gem->Snp.WaitForPacket);

FreeDevicePath:
  FreePool (Gem->DevicePath);

CloseProtocol:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  if (DmaAllocated) {
    GemDmaFree (Gem);
  }

  FreePool (Gem);

  return Status;
}

/**
  Stops the driver on ControllerHandle, destroying the SNP child handle.

  @param  This[in]               EFI_DRIVER_BINDING_PROTOCOL instance.
  @param  ControllerHandle[in]   Handle of the device being stopped.
  @param  NumberOfChildren[in]   Number of children in ChildHandleBuffer.
  @param  ChildHandleBuffer[in]  Child handles to be freed.

  @retval EFI_SUCCESS       The device was stopped.
  @retval EFI_DEVICE_ERROR  A child could not be destroyed.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer  OPTIONAL
  )
{
  EFI_STATUS                   Status;
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp;
  RP1_GEM_PRIVATE_DATA         *Gem;
  UINTN                        Index;
  VOID                         *Dummy;

  if (NumberOfChildren == 0) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gEdkiiNonDiscoverableDeviceProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < NumberOfChildren; Index++) {
    Status = gBS->OpenProtocol (
                    ChildHandleBuffer[Index],
                    &gEfiSimpleNetworkProtocolGuid,
                    (VOID **)&Snp,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (Snp);
    ASSERT (Gem->ChildHandle == ChildHandleBuffer[Index]);

    //
    // Quiesce the hardware regardless of SNP state.
    //
    GemMacDisableTxRx (Gem);
    GemMacReset (Gem);

    gBS->CloseProtocol (
           ControllerHandle,
           &gEdkiiNonDiscoverableDeviceProtocolGuid,
           This->DriverBindingHandle,
           Gem->ChildHandle
           );

    Status = gBS->UninstallMultipleProtocolInterfaces (
                    Gem->ChildHandle,
                    &gEfiDevicePathProtocolGuid,
                    Gem->DevicePath,
                    &gEfiSimpleNetworkProtocolGuid,
                    &Gem->Snp,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      //
      // Re-establish the child link and report failure.
      //
      gBS->OpenProtocol (
             ControllerHandle,
             &gEdkiiNonDiscoverableDeviceProtocolGuid,
             &Dummy,
             This->DriverBindingHandle,
             Gem->ChildHandle,
             EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
             );
      return EFI_DEVICE_ERROR;
    }

    gBS->CloseEvent (Gem->ExitBootServicesEvent);
    gBS->CloseEvent (Gem->Snp.WaitForPacket);

    GemDmaFree (Gem);
    FreePool (Gem->DevicePath);
    FreePool (Gem);
  }

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  gRp1GemDriverBinding = {
  Rp1GemDriverBindingSupported,
  Rp1GemDriverBindingStart,
  Rp1GemDriverBindingStop,
  0x10,
  NULL,
  NULL
};

/**
  The entry point of the RP1 GEM SNP driver.

  @param  ImageHandle[in]  The image handle of the driver.
  @param  SystemTable[in]  Pointer to the EFI System Table.

  @retval EFI_SUCCESS  Driver binding and component name protocols
                       installed.
  @retval other        Installation failure.

**/
EFI_STATUS
EFIAPI
Rp1GemDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &gRp1GemDriverBinding,
           ImageHandle,
           &gRp1GemComponentName,
           &gRp1GemComponentName2
           );
}

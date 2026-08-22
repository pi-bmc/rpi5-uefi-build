/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Rp1.h>

#include "Rp1BusDxe.h"

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  UINT64                      BaseAddress;
  UINT8                       ResourceType;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RP1_BUS_VENDOR_DEVICE_PATH;
#pragma pack ()

STATIC
VOID
EFIAPI
Rp1BusRegisterDwc3Controllers (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  EFI_STATUS            Status;
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  FullBase;
  EFI_HANDLE            DeviceHandle;
  RP1_BUS_PROTOCOL      *Rp1Bus;

  EFI_PHYSICAL_ADDRESS  Dwc3Addresses[] = {
    RP1_USBHOST0_BASE, RP1_USBHOST1_BASE
  };

  for (Index = 0; Index < ARRAY_SIZE (Dwc3Addresses); Index++) {
    DeviceHandle = NULL;
    FullBase     = Rp1Data->PeripheralBase + Dwc3Addresses[Index];

    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeXhci,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               &DeviceHandle,
               1,
               FullBase,
               RP1_USBHOST_SIZE
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to register DWC3 controller at 0x%lx. Status=%r\n",
        FullBase,
        Status
        ));
      continue;
    }

    Status = gBS->OpenProtocol (
                    Rp1Data->ControllerHandle,
                    &gRp1BusProtocolGuid,
                    (VOID **)&Rp1Bus,
                    Rp1Data->DriverBinding->DriverBindingHandle,
                    DeviceHandle,
                    EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to open DWC3 by controller. Status=%r\n",
        Status
        ));
      continue;
    }
  }
}

//
// Register an RP1 peripheral as a NON_DISCOVERABLE_DEVICE with a
// vendor-specific type GUID, for peripherals with no generic EDK2 class
// (GEM ethernet, DesignWare I2C). NonDiscoverablePciDeviceDxe ignores
// these GUIDs, so no PciIo is fabricated: the driver that recognizes the
// GUID does its own MMIO using the resource descriptors installed here.
//
STATIC
EFI_STATUS
EFIAPI
Rp1BusRegisterVendorMmioDevice (
  IN RP1_BUS_DATA                      *Rp1Data,
  IN CONST EFI_GUID                    *TypeGuid,
  IN NON_DISCOVERABLE_DEVICE_DMA_TYPE  DmaType,
  IN UINTN                             NumMmioResources,
  IN CONST UINT64                      *MmioOffsets,
  IN CONST UINT64                      *MmioSizes
  )
{
  NON_DISCOVERABLE_DEVICE            *Device;
  RP1_BUS_VENDOR_DEVICE_PATH         *DevicePath;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  EFI_ACPI_END_TAG_DESCRIPTOR        *End;
  EFI_HANDLE                         DeviceHandle;
  EFI_STATUS                         Status;
  UINTN                              AllocSize;
  UINTN                              Index;
  UINT64                             Base;
  RP1_BUS_PROTOCOL                   *Rp1Bus;

  AllocSize = sizeof (*Device) +
              NumMmioResources * sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) +
              sizeof (EFI_ACPI_END_TAG_DESCRIPTOR);
  Device = (NON_DISCOVERABLE_DEVICE *)AllocateZeroPool (AllocSize);
  if (Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Device->Type       = TypeGuid;
  Device->DmaType    = DmaType;
  Device->Initialize = NULL;
  Device->Resources  = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(Device + 1);

  for (Index = 0; Index < NumMmioResources; Index++) {
    Desc = &Device->Resources[Index];
    Base = Rp1Data->PeripheralBase + MmioOffsets[Index];

    Desc->Desc                  = ACPI_ADDRESS_SPACE_DESCRIPTOR;
    Desc->Len                   = sizeof (*Desc) - 3;
    Desc->AddrRangeMin          = Base;
    Desc->AddrLen               = MmioSizes[Index];
    Desc->AddrRangeMax          = Base + MmioSizes[Index] - 1;
    Desc->ResType               = ACPI_ADDRESS_SPACE_TYPE_MEM;
    Desc->AddrSpaceGranularity  = ((EFI_PHYSICAL_ADDRESS)Base + MmioSizes[Index] > SIZE_4GB) ? 64 : 32;
    Desc->AddrTranslationOffset = 0;
  }

  End           = (EFI_ACPI_END_TAG_DESCRIPTOR *)&Device->Resources[NumMmioResources];
  End->Desc     = ACPI_END_TAG_DESCRIPTOR;
  End->Checksum = 0;

  DevicePath = (RP1_BUS_VENDOR_DEVICE_PATH *)CreateDeviceNode (
                                               HARDWARE_DEVICE_PATH,
                                               HW_VENDOR_DP,
                                               sizeof (*DevicePath)
                                               );
  if (DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeDevice;
  }

  CopyGuid (&DevicePath->Vendor.Guid, TypeGuid);
  DevicePath->BaseAddress  = Device->Resources[0].AddrRangeMin;
  DevicePath->ResourceType = Device->Resources[0].ResType;

  SetDevicePathNodeLength (
    &DevicePath->Vendor,
    sizeof (*DevicePath) - sizeof (DevicePath->End)
    );
  SetDevicePathEndNode (&DevicePath->End);

  DeviceHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DeviceHandle,
                        &gEdkiiNonDiscoverableDeviceProtocolGuid,
                        Device,
                        &gEfiDevicePathProtocolGuid,
                        DevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    goto FreeDevicePath;
  }

  Status = gBS->OpenProtocol (
                  Rp1Data->ControllerHandle,
                  &gRp1BusProtocolGuid,
                  (VOID **)&Rp1Bus,
                  Rp1Data->DriverBinding->DriverBindingHandle,
                  DeviceHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "RP1: Failed to open bus protocol by child %g. Status=%r\n",
      TypeGuid,
      Status
      ));
  }

  return EFI_SUCCESS;

FreeDevicePath:
  FreePool (DevicePath);

FreeDevice:
  FreePool (Device);

  return Status;
}

STATIC
VOID
EFIAPI
Rp1BusRegisterVendorDevices (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  EFI_STATUS  Status;

  STATIC CONST UINT64  GemOffsets[]  = { RP1_ETH_BASE, RP1_ETH_CFG_BASE };
  STATIC CONST UINT64  GemSizes[]    = { RP1_ETH_SIZE, RP1_ETH_CFG_SIZE };
  STATIC CONST UINT64  I2c1Offsets[] = { RP1_I2C1_BASE };
  STATIC CONST UINT64  I2c1Sizes[]   = { RP1_I2C_SIZE };

  Status = Rp1BusRegisterVendorMmioDevice (
             Rp1Data,
             &gRp1GemNonDiscoverableDeviceGuid,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             ARRAY_SIZE (GemOffsets),
             GemOffsets,
             GemSizes
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to register GEM device. Status=%r\n", Status));
  }

  Status = Rp1BusRegisterVendorMmioDevice (
             Rp1Data,
             &gRp1DwI2cNonDiscoverableDeviceGuid,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             ARRAY_SIZE (I2c1Offsets),
             I2c1Offsets,
             I2c1Sizes
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to register I2C1 device. Status=%r\n", Status));
  }
}

STATIC
VOID
EFIAPI
Rp1BusRegisterDevices (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  Rp1BusRegisterDwc3Controllers (Rp1Data);
  Rp1BusRegisterVendorDevices (Rp1Data);
}

STATIC
VOID
EFIAPI
Rp1BusEnableInterrupts (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST0_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST1_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
}

//
// Undo Rp1BusEnableInterrupts().
//
// MSIX_CFG is RP1's own interrupt-to-MSI-X routing block, and it belongs to
// the OS the moment we leave. Linux drives these exact registers from
// drivers/misc/rp1/rp1_pci.c -- same APB base (0x108000), same set/clear
// aliases, same MSIX_CFG(hwirq) stride -- but it only ever clears ENABLE for
// one hwirq at a time, as its IRQ domain tears that mapping down. It never
// zeroes the block when it probes.
//
// So anything we leave armed stays armed behind the OS's back: it comes up
// believing all 61 RP1 sources are masked while two of them are live in
// hardware, still routed at whatever MSI-X table entries this firmware
// programmed.
//
// Symmetric with what we armed, rather than a sweep of all 61: undoing our
// own writes is the part we can be sure about, and the boot firmware may have
// its own reasons for anything else that is set.
//
STATIC
VOID
EFIAPI
Rp1BusDisableInterrupts (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_CLR + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST0_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_CLR + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST1_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
}

/**
  Quiesce RP1's interrupt routing at ExitBootServices, so the OS inherits the
  device with nothing armed that it does not know about.

  @param  Event[in]    The event that fired.
  @param  Context[in]  Driver private data.

**/
STATIC
VOID
EFIAPI
Rp1BusNotifyExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  Rp1BusDisableInterrupts ((RP1_BUS_DATA *)Context);
}

STATIC
EFI_PHYSICAL_ADDRESS
EFIAPI
Rp1BusGetPeripheralBase (
  IN RP1_BUS_PROTOCOL  *This
  )
{
  ASSERT (This != NULL);

  return (RP1_BUS_DATA_FROM_THIS (This))->PeripheralBase;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               PciId;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_VENDOR_ID_OFFSET,
                        1,
                        &PciId
                        );

  if (EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  if (((PciId & 0xffff) != PCI_VENDOR_ID_RPILTD) ||
      ((PciId >> 16) != PCI_DEVICE_ID_RP1))
  {
    Status = EFI_UNSUPPORTED;
  }

Exit:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  EFI_PCI_IO_PROTOCOL                *PciIo;
  UINT64                             Supports;
  RP1_BUS_DATA                       *Rp1Data;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *PeripheralDesc;

  Rp1Data = NULL;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationSupported,
                    0,
                    &Supports
                    );
  if (!EFI_ERROR (Status)) {
    Supports &= (UINT64)EFI_PCI_DEVICE_ENABLE;
    Status    = PciIo->Attributes (
                         PciIo,
                         EfiPciIoAttributeOperationEnable,
                         Supports,
                         NULL
                         );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to enable PCI device. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data = AllocateZeroPool (sizeof (RP1_BUS_DATA));
  if (Rp1Data == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_ERROR, "RP1: Failed to allocate device context. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data->Signature                = RP1_BUS_DATA_SIGNATURE;
  Rp1Data->ControllerHandle         = ControllerHandle;
  Rp1Data->DriverBinding            = This;
  Rp1Data->PciIo                    = PciIo;
  Rp1Data->Rp1Bus.GetPeripheralBase = Rp1BusGetPeripheralBase;

  Status = PciIo->GetBarAttributes (
                    PciIo,
                    RP1_PERIPHERAL_BAR_INDEX,
                    NULL,
                    (VOID **)&PeripheralDesc
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to get BAR attributes. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data->PeripheralBase = PeripheralDesc->AddrRangeMin;
  FreePool (PeripheralDesc);

  Rp1Data->ChipId = MmioRead32 (Rp1Data->PeripheralBase + RP1_SYSINFO_BASE);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ControllerHandle,
                  &gRp1BusProtocolGuid,
                  &Rp1Data->Rp1Bus,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to install bus protocol. Status=%r\n", Status));
    goto Fail;
  }

  DEBUG ((
    DEBUG_INFO,
    "RP1: chip id %x, peripheral base at CPU address 0x%lx\n",
    Rp1Data->ChipId,
    Rp1Data->PeripheralBase
    ));

  Rp1BusRegisterDevices (Rp1Data);
  Rp1BusEnableInterrupts (Rp1Data);

  //
  // Nothing else disarms these. The driver is not stopped on the normal boot
  // path, so without this hook RP1 reaches the OS with our USB host sources
  // still routed. Best-effort: a device that keeps working is better than a
  // failed Start, and failing here would take both xHCIs down with it.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  Rp1BusNotifyExitBootServices,
                  Rp1Data,
                  &gEfiEventExitBootServicesGuid,
                  &Rp1Data->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "RP1: Failed to register the ExitBootServices handler. Status=%r\n",
      Status
      ));
  }

  return EFI_SUCCESS;

Fail:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  gBS->UninstallMultipleProtocolInterfaces (
         ControllerHandle,
         &gRp1BusProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  if (Rp1Data != NULL) {
    FreePool (Rp1Data);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Rp1BusUnregisterNonDiscoverableDevice (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   DeviceHandle
  )
{
  EFI_STATUS                Status;
  NON_DISCOVERABLE_DEVICE   *NonDiscoverableDevice;
  EFI_DEVICE_PATH_PROTOCOL  *NonDiscoverableDevicePath;
  RP1_BUS_PROTOCOL          *Rp1Bus;

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&NonDiscoverableDevice,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&NonDiscoverableDevicePath,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gRp1BusProtocolGuid,
                  This->DriverBindingHandle,
                  DeviceHandle
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  NonDiscoverableDevice,
                  &gEfiDevicePathProtocolGuid,
                  NonDiscoverableDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->OpenProtocol (
           ControllerHandle,
           &gRp1BusProtocolGuid,
           (VOID **)&Rp1Bus,
           This->DriverBindingHandle,
           DeviceHandle,
           EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
           );
    return Status;
  }

  FreePool (NonDiscoverableDevice);
  FreePool (NonDiscoverableDevicePath);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *DeviceHandleBuffer
  )
{
  EFI_STATUS        Status;
  UINTN             Index;
  RP1_BUS_PROTOCOL  *Rp1Bus;
  RP1_BUS_DATA      *Rp1Data;
  BOOLEAN           AllChildrenStopped;

  if (NumberOfChildren == 0) {
    DEBUG ((DEBUG_INFO, "RP1: Stop bus at %p\n", ControllerHandle));

    Status = gBS->OpenProtocol (
                    ControllerHandle,
                    &gRp1BusProtocolGuid,
                    (VOID **)&Rp1Bus,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Rp1Data = RP1_BUS_DATA_FROM_THIS (Rp1Bus);

    Rp1BusDisableInterrupts (Rp1Data);

    if (Rp1Data->ExitBootServicesEvent != NULL) {
      gBS->CloseEvent (Rp1Data->ExitBootServicesEvent);
    }

    Status = gBS->UninstallMultipleProtocolInterfaces (
                    ControllerHandle,
                    &gRp1BusProtocolGuid,
                    Rp1Bus,
                    NULL
                    );
    ASSERT_EFI_ERROR (Status);

    FreePool (Rp1Data);

    Status = gBS->CloseProtocol (
                    ControllerHandle,
                    &gEfiPciIoProtocolGuid,
                    This->DriverBindingHandle,
                    ControllerHandle
                    );
    ASSERT_EFI_ERROR (Status);

    return EFI_SUCCESS;
  }

  AllChildrenStopped = TRUE;

  for (Index = 0; Index < NumberOfChildren; Index++) {
    //
    // We only register non-discoverable PCI devices so far.
    //
    Status = Rp1BusUnregisterNonDiscoverableDevice (
               This,
               ControllerHandle,
               DeviceHandleBuffer[Index]
               );
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      continue;
    }
  }

  if (!AllChildrenStopped) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  mRp1BusDriverBinding = {
  Rp1BusDriverBindingSupported,
  Rp1BusDriverBindingStart,
  Rp1BusDriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
Rp1BusDxeEntryPoint (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mRp1BusDriverBinding,
           ImageHandle,
           &mRp1BusComponentName,
           &mRp1BusComponentName2
           );
}

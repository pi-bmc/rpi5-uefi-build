/** @file

  FanConfigDxe - the "Active Cooler" page in Setup's Device Manager.

  Pure efivarstore HII: the formset (FanConfigHii.vfr) binds its questions
  straight to the FanPolicy variable, so the form browser reads and writes
  the variable itself and this driver needs no ConfigAccess callbacks. It
  only has to (a) make sure the variable exists with sane defaults before
  the browser first reads it, and (b) publish the packages under a vendor
  device path so DeviceManagerUiLib lists the page (classguid
  EFI_HII_PLATFORM_SETUP_FORMSET_GUID, the ConfigDxe idiom).

  Policy changes apply live: ActiveCoolerDxe re-reads FanPolicy on every
  1 s poll tick, so leaving the form with F10 changes the fan behavior
  without a reset.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>

#include <Guid/RpiFanPolicy.h>

//
// AutoGen emits these from FanConfigHii.vfr and FanConfigDxe.uni.
//
extern UINT8  FanConfigHiiBin[];
extern UINT8  FanConfigDxeStrings[];

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;
#pragma pack ()

STATIC HII_VENDOR_DEVICE_PATH  mVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    RPI_FAN_CONFIG_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

/**
  Create FanPolicy with AUTO defaults when it is absent or has been
  written with the wrong size (an old layout, a corrupt BMC write): the
  browser's efivarstore reads need a well-formed variable to edit, and
  ActiveCoolerDxe treats a malformed one as AUTO anyway.
**/
STATIC
VOID
EnsurePolicyVariable (
  VOID
  )
{
  RPI_FAN_POLICY  Policy;
  UINTN           Size;
  EFI_STATUS      Status;

  Size   = sizeof (Policy);
  Status = gRT->GetVariable (
                  RPI_FAN_POLICY_VARIABLE_NAME,
                  &gRpiFanConfigFormSetGuid,
                  NULL,
                  &Size,
                  &Policy
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Policy))) {
    return;
  }

  Policy.Mode       = RPI_FAN_MODE_AUTO;
  Policy.FixedLevel = 2;
  Policy.Trip1C     = 50;
  Policy.Trip2C     = 60;
  Policy.Trip3C     = 68;
  Policy.Trip4C     = 75;

  Status = gRT->SetVariable (
                  RPI_FAN_POLICY_VARIABLE_NAME,
                  &gRpiFanConfigFormSetGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (Policy),
                  &Policy
                  );
  DEBUG ((DEBUG_INFO, "FanConfigDxe: created default FanPolicy - %r\n", Status));
}

EFI_STATUS
EFIAPI
FanConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  EFI_HANDLE      DriverHandle;
  EFI_HII_HANDLE  HiiHandle;

  EnsurePolicyVariable ();

  DriverHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DriverHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mVendorDevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  HiiHandle = HiiAddPackages (
                &gRpiFanConfigFormSetGuid,
                DriverHandle,
                FanConfigDxeStrings,
                FanConfigHiiBin,
                NULL
                );
  if (HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INFO, "FanConfigDxe: Setup page published\n"));
  return EFI_SUCCESS;
}

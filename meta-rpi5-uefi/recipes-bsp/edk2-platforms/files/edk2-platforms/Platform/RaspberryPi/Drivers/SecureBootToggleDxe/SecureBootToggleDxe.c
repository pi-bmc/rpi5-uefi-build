/** @file

  SecureBootToggleDxe - a "Secure Boot" page in Setup's Device Manager that
  is also a Redfish BIOS attribute.

  SecurityPkg's own SecureBootConfigDxe already has this checkbox, but it is
  reachable only from its own page, it hides itself until a PK is enrolled,
  and it sits on a buffer varstore behind EFI_HII_CONFIG_ACCESS_PROTOCOL -
  none of which suits a Redfish BIOS attribute. This driver puts the same
  one-byte decision on a plain efivarstore so RedfishPlatformConfigDxe can
  harvest it (SecureBootToggleDxeMap.uni), while SecureBootConfigDxe keeps
  ownership of key enrollment.

  The variable is SecurityPkg's, not ours: EFI_SECURE_BOOT_ENABLE_NAME under
  gEfiSecureBootEnableDisableGuid, one UINT8 whose values are exactly what a
  HII checkbox stores. AuthVariableLib turns it into the global SecureBoot
  state during the next boot's AuthVariableLibInitialize, hence the
  RESET_REQUIRED flag on the question.

  Only built when SECURE_BOOT_ENABLE=TRUE. With AuthVariableLibNull nothing
  reads this variable, so the page would be a lie - see RpiBmc.dsc.inc.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>

#include <Guid/AuthenticatedVariableFormat.h>
#include <Guid/GlobalVariable.h>
#include <Guid/RpiSecureBootToggle.h>

//
// AutoGen emits these from SecureBootToggleHii.vfr and the two .uni files.
//
extern UINT8  SecureBootToggleHiiBin[];
extern UINT8  SecureBootToggleDxeStrings[];

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
    RPI_SECURE_BOOT_TOGGLE_FORMSET_GUID
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
  Make sure SecureBootEnable exists before the browser or
  RedfishPlatformConfigDxe first reads it.

  AuthVariableLib owns this variable's lifecycle: it creates it when the
  platform comes up in secure-boot mode and deletes it otherwise
  (AuthVariableLibInitialize). So an absent variable means secure boot is
  currently off, and seeding it from the global SecureBoot state - which is
  0 or absent in exactly that case - agrees with AuthVariableLib rather than
  fighting it. Seeding never flips the platform on: it only ever writes back
  the state that is already in force.
**/
STATIC
VOID
EnsureSecureBootEnableVariable (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Size;

  Size   = sizeof (Value);
  Status = gRT->GetVariable (
                  EFI_SECURE_BOOT_ENABLE_NAME,
                  &gEfiSecureBootEnableDisableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Value))) {
    return;
  }

  //
  // Mirror the global SecureBoot state; treat "absent" as disabled.
  //
  Size   = sizeof (Value);
  Status = gRT->GetVariable (
                  EFI_SECURE_BOOT_MODE_NAME,
                  &gEfiGlobalVariableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Value))) {
    Value = SECURE_BOOT_DISABLE;
  }

  Status = gRT->SetVariable (
                  EFI_SECURE_BOOT_ENABLE_NAME,
                  &gEfiSecureBootEnableDisableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (Value),
                  &Value
                  );
  DEBUG ((
    DEBUG_INFO,
    "SecureBootToggleDxe: seeded SecureBootEnable = %u - %r\n",
    Value,
    Status
    ));
}

EFI_STATUS
EFIAPI
SecureBootToggleEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  EFI_HANDLE      DriverHandle;
  EFI_HII_HANDLE  HiiHandle;

  EnsureSecureBootEnableVariable ();

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
                &gRpiSecureBootToggleFormSetGuid,
                DriverHandle,
                SecureBootToggleDxeStrings,
                SecureBootToggleHiiBin,
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

  DEBUG ((DEBUG_INFO, "SecureBootToggleDxe: Setup page published\n"));
  return EFI_SUCCESS;
}

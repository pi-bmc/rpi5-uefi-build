/** @file

  Component name protocol implementations for the RP1 GEM SNP driver.

  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1GemDxe.h"

STATIC EFI_UNICODE_STRING_TABLE  mRp1GemDriverNameTable[] = {
  {
    "eng;en",
    (CHAR16 *)L"RP1 GEM Ethernet Driver"
  },
  {
    NULL,
    NULL
  }
};

STATIC EFI_UNICODE_STRING_TABLE  mRp1GemControllerNameTable[] = {
  {
    "eng;en",
    (CHAR16 *)L"Cadence GEM_GXL Gigabit Ethernet (RP1)"
  },
  {
    NULL,
    NULL
  }
};

/**
  Retrieves a Unicode string that is the user readable name of the driver.

  @param  This[in]        EFI_COMPONENT_NAME_PROTOCOL instance.
  @param  Language[in]    RFC 4646 or ISO 639-2 language code.
  @param  DriverName[out] User readable driver name.

  @retval EFI_SUCCESS            The name was returned.
  @retval EFI_INVALID_PARAMETER  Language or DriverName is NULL.
  @retval EFI_UNSUPPORTED        The language is not supported.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mRp1GemDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gRp1GemComponentName)
           );
}

/**
  Retrieves a Unicode string that is the user readable name of the
  controller being managed by this driver.

  @param  This[in]              EFI_COMPONENT_NAME_PROTOCOL instance.
  @param  ControllerHandle[in]  Handle of the controller.
  @param  ChildHandle[in]       Handle of the child controller (optional).
  @param  Language[in]          RFC 4646 or ISO 639-2 language code.
  @param  ControllerName[out]   User readable controller name.

  @retval EFI_SUCCESS            The name was returned.
  @retval EFI_INVALID_PARAMETER  A required parameter is NULL.
  @retval EFI_UNSUPPORTED        The driver is not managing the handle, or
                                 the language is not supported.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle  OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  EFI_STATUS  Status;

  if (ControllerHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EfiTestManagedDevice (
             ControllerHandle,
             gRp1GemDriverBinding.DriverBindingHandle,
             &gEdkiiNonDiscoverableDeviceProtocolGuid
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ChildHandle != NULL) {
    Status = EfiTestChildHandle (
               ControllerHandle,
               ChildHandle,
               &gEdkiiNonDiscoverableDeviceProtocolGuid
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mRp1GemControllerNameTable,
           ControllerName,
           (BOOLEAN)(This == &gRp1GemComponentName)
           );
}

EFI_COMPONENT_NAME_PROTOCOL  gRp1GemComponentName = {
  Rp1GemComponentNameGetDriverName,
  Rp1GemComponentNameGetControllerName,
  "eng"
};

EFI_COMPONENT_NAME2_PROTOCOL  gRp1GemComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)Rp1GemComponentNameGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)Rp1GemComponentNameGetControllerName,
  "en"
};

/** @file

  Component name protocol implementations for the RP1 DW I2C driver.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1DwI2cDxe.h"

STATIC EFI_UNICODE_STRING_TABLE  mRp1DwI2cDriverNameTable[] = {
  { "eng;en", L"RP1 DesignWare I2C Master Driver" },
  { NULL,     NULL                                }
};

STATIC EFI_UNICODE_STRING_TABLE  mRp1DwI2cControllerNameTable[] = {
  { "eng;en", L"RP1 I2C1 Controller" },
  { NULL,     NULL                   }
};

/**
  Retrieves a string that is the user readable name of the driver.

  @param[in]  This         EFI_COMPONENT_NAME2_PROTOCOL instance.
  @param[in]  Language     The language of the driver name.
  @param[out] DriverName   The driver name in the requested language.

  @retval EFI_SUCCESS             The name was returned.
  @retval EFI_INVALID_PARAMETER   Language or DriverName is NULL.
  @retval EFI_UNSUPPORTED         The language is not supported.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cGetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mRp1DwI2cDriverNameTable,
           DriverName,
           (BOOLEAN)(This == (EFI_COMPONENT_NAME2_PROTOCOL *)&gRp1DwI2cComponentName)
           );
}

/**
  Retrieves a string that is the user readable name of the controller.

  @param[in]  This             EFI_COMPONENT_NAME2_PROTOCOL instance.
  @param[in]  ControllerHandle The controller to name.
  @param[in]  ChildHandle      Must be NULL - not a bus driver.
  @param[in]  Language         The language of the controller name.
  @param[out] ControllerName   The controller name in the requested
                               language.

  @retval EFI_SUCCESS             The name was returned.
  @retval EFI_INVALID_PARAMETER   A required parameter is NULL.
  @retval EFI_UNSUPPORTED         Not managed by this driver, ChildHandle
                                  is not NULL, or the language is not
                                  supported.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cGetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    ChildHandle  OPTIONAL,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **ControllerName
  )
{
  EFI_STATUS  Status;

  if (ChildHandle != NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = EfiTestManagedDevice (
             ControllerHandle,
             gImageHandle,
             &gEdkiiNonDiscoverableDeviceProtocolGuid
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mRp1DwI2cControllerNameTable,
           ControllerName,
           (BOOLEAN)(This == (EFI_COMPONENT_NAME2_PROTOCOL *)&gRp1DwI2cComponentName)
           );
}

EFI_COMPONENT_NAME2_PROTOCOL  gRp1DwI2cComponentName2 = {
  Rp1DwI2cGetDriverName,
  Rp1DwI2cGetControllerName,
  "en"
};

EFI_COMPONENT_NAME_PROTOCOL  gRp1DwI2cComponentName = {
  (EFI_COMPONENT_NAME_GET_DRIVER_NAME)Rp1DwI2cGetDriverName,
  (EFI_COMPONENT_NAME_GET_CONTROLLER_NAME)Rp1DwI2cGetControllerName,
  "eng"
};

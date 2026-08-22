/** @file
  RpiRedfishCredentialLib - RedfishPlatformCredentialLib instance for the
  BMC's Redfish service over the USB CDC-NCM host interface.

  Unlike the JetKVM (which exempts the RHI subnet from authentication and
  takes AuthMethodNone), the nanokvm BMC puts /redfish/v1 behind CheckAuth:
  session token or HTTP Basic, unless its authentication is disabled. So
  this instance reports AuthMethodHttpBasic with the build-time credentials
  from RpiRedfishPkg PCDs; an empty user string degrades to AuthMethodNone
  for a BMC configured with authentication off.

  RedfishPkg's PlatformCredentialLibNull is not a substitute for either
  case: its GetAuthInfo returns EFI_UNSUPPORTED, which RedfishHttpDxe
  treats as fatal (RedfishCreateService returns NULL and no request is
  ever made) - observed on the NUC 2026-07-30 with the Null instance.

  The boundary this rests on: the link is a point-to-point USB gadget
  cable between exactly one host and one BMC, carrying no other traffic
  and not routed, so build-time Basic credentials over plain HTTP are
  as exposed as the physical link itself.

  The specified alternative, DSP0270 bootstrap credentials, is delivered
  over IPMI; this board has no IPMI transport, which is why
  PcdRedfishDisableBootstrapCredentialService is set.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Protocol/EdkIIRedfishCredential.h>

/**
  Return the Redfish authentication method and credentials for this platform.

  @param[in]   This        Pointer to EDKII_REDFISH_CREDENTIAL_PROTOCOL instance.
  @param[out]  AuthMethod  Receives AuthMethodHttpBasic (or AuthMethodNone when
                           no user is configured).
  @param[out]  UserId      Receives the user name, allocated from pool; NULL
                           for AuthMethodNone. Caller frees.
  @param[out]  Password    Receives the password, allocated from pool; NULL
                           for AuthMethodNone. Caller frees.

  @retval EFI_SUCCESS            Authentication information returned.
  @retval EFI_INVALID_PARAMETER  An output pointer is NULL.
  @retval EFI_OUT_OF_RESOURCES   Allocation failed.
**/
EFI_STATUS
EFIAPI
LibCredentialGetAuthInfo (
  IN  EDKII_REDFISH_CREDENTIAL_PROTOCOL  *This,
  OUT EDKII_REDFISH_AUTH_METHOD          *AuthMethod,
  OUT CHAR8                              **UserId,
  OUT CHAR8                              **Password
  )
{
  CONST CHAR8  *User;
  CONST CHAR8  *Pass;

  if ((This == NULL) || (AuthMethod == NULL) || (UserId == NULL) || (Password == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  User = (CONST CHAR8 *)PcdGetPtr (PcdRpiRedfishUser);
  Pass = (CONST CHAR8 *)PcdGetPtr (PcdRpiRedfishPassword);

  if ((User == NULL) || (User[0] == '\0')) {
    //
    // No credentials configured: the BMC runs with authentication disabled.
    // RedfishHttpDxe only inspects UserId/Password when AuthMethod is not
    // AuthMethodNone, and skips the Authorization header entirely.
    //
    *AuthMethod = AuthMethodNone;
    *UserId     = NULL;
    *Password   = NULL;

    DEBUG ((DEBUG_ERROR, "RpiRedfishCredential: host interface is unauthenticated (AuthMethodNone)\n"));
    return EFI_SUCCESS;
  }

  *UserId = AllocateCopyPool (AsciiStrSize (User), User);
  if (*UserId == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *Password = AllocateCopyPool (AsciiStrSize (Pass), Pass);
  if (*Password == NULL) {
    FreePool (*UserId);
    *UserId = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  *AuthMethod = AuthMethodHttpBasic;

  DEBUG ((DEBUG_ERROR, "RpiRedfishCredential: HTTP Basic as '%a'\n", User));

  return EFI_SUCCESS;
}

/**
  Stop the Redfish service. There is no credential state to revoke, so nothing
  has to happen here; reporting success keeps callers from treating a clean
  shutdown as a failure.

  @param[in]  This             Pointer to EDKII_REDFISH_CREDENTIAL_PROTOCOL instance.
  @param[in]  ServiceStopType  Type of stop request.

  @retval EFI_SUCCESS            Nothing to do.
  @retval EFI_INVALID_PARAMETER  This is NULL or the stop type is out of range.
**/
EFI_STATUS
EFIAPI
LibStopRedfishService (
  IN     EDKII_REDFISH_CREDENTIAL_PROTOCOL           *This,
  IN     EDKII_REDFISH_CREDENTIAL_STOP_SERVICE_TYPE  ServiceStopType
  )
{
  if ((This == NULL) || (ServiceStopType >= ServiceStopTypeMax)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Notification of Exit Boot Service. Nothing to tear down.

  @param[in]  This  Pointer to EDKII_REDFISH_CREDENTIAL_PROTOCOL instance.
**/
VOID
EFIAPI
LibCredentialExitBootServicesNotify (
  IN  EDKII_REDFISH_CREDENTIAL_PROTOCOL  *This
  )
{
  return;
}

/**
  Notification of End of DXE. Nothing to tear down.

  @param[in]  This  Pointer to EDKII_REDFISH_CREDENTIAL_PROTOCOL instance.
**/
VOID
EFIAPI
LibCredentialEndOfDxeNotify (
  IN  EDKII_REDFISH_CREDENTIAL_PROTOCOL  *This
  )
{
  return;
}

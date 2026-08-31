/** @file

  Redfish EthernetInterface feature driver - schema-specific logic.

  Modeled on RedfishClientPkg's Memory/V1_7_1 commons, hand-written
  rather than generated because two of the managed properties defeat the
  generic helpers at this client pin:

  - IPv4StaticAddresses is a RedfishCS_Link of untyped JSON blobs in the
    v1_8_0 C structure (there is no IPAddresses CS), so the consume side
    parses the first element with JsonLib and the provision side builds
    the array by hand.
  - The generic RedfishCheckResourceCommon walks configure-language
    tails through MatchPropertyWithJsonContext, which can never match a
    node literally named "{1}" or "[1]" against a JSON array - a nested
    path would make Check fail forever and pin the driver in the
    provisioning branch. Check here tests for the managed marker
    property (DHCPv4) instead.

  Managed property set, kept deliberately small and standard:
    DHCPv4/DHCPEnabled                boolean
    IPv4StaticAddresses[0]            Address / SubnetMask / Gateway
    StaticNameServers                 up to two entries
    MACAddress                        report-only (never consumed)

  The HII questions behind these live in EthConfigDxe; a consumed change
  lands in the EthCfg variable via RedfishPlatformConfigSetValue, the
  Apply* helpers raise PcdRedfishSystemRebootRequired, the feature core
  cold-resets, and EthConfigDxe applies the new policy through
  Ip4Config2 on the way back up.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "EthernetInterfaceCommon.h"

REDFISH_RESOURCE_COMMON_PRIVATE  *mRedfishResourcePrivate             = NULL;
EFI_HANDLE                       mRedfishResourceConfigProtocolHandle = NULL;

//
// The configure-language tails this driver manages, relative to the
// instance (see EthConfigDxeMap.uni). {N}/[N] syntax follows the
// RedfishClientPkg Readme: {} is a collection member, [] an array entry.
//
#define ETH_LANG_DHCP_ENABLED  "DHCPv4/DHCPEnabled"
#define ETH_LANG_ADDRESS       "IPv4StaticAddresses/[1]/Address"
#define ETH_LANG_SUBNET_MASK   "IPv4StaticAddresses/[1]/SubnetMask"
#define ETH_LANG_GATEWAY       "IPv4StaticAddresses/[1]/Gateway"
#define ETH_LANG_DNS1          "StaticNameServers/[1]"
#define ETH_LANG_DNS2          "StaticNameServers/[2]"
#define ETH_LANG_MAC           "MACAddress"

/**
  Apply one string property to the HII database, treating a NULL value
  as "clear". Missing configure language (question not published on this
  build) is quietly skipped.

  @param[in] OdataId       The resource's odata.id, for the config-lang lookup.
  @param[in] PropertyName  Configure-language tail of the property.
  @param[in] Value         New value, or NULL to clear.
**/
STATIC
VOID
ConsumeStringProperty (
  IN CHAR8  *OdataId,
  IN CHAR8  *PropertyName,
  IN CHAR8  *Value OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_STRING  ConfigureLang;

  ConfigureLang = GetConfigureLang (OdataId, PropertyName);
  if (ConfigureLang == NULL) {
    return;
  }

  Status = ApplyFeatureSettingsStringType (
             RESOURCE_SCHEMA,
             RESOURCE_SCHEMA_VERSION,
             ConfigureLang,
             (Value == NULL) ? "" : Value
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: apply %a failed: %r\n", __func__, PropertyName, Status));
  }

  FreePool (ConfigureLang);
}

/**
  Pull an ASCII string member out of a JSON object, NULL when absent or
  not a string.

  @param[in] Object  The JSON object.
  @param[in] Key     Member name.

  @return The string value, or NULL.
**/
STATIC
CONST CHAR8 *
JsonObjectAsciiMember (
  IN EDKII_JSON_VALUE  Object,
  IN CONST CHAR8       *Key
  )
{
  EDKII_JSON_VALUE  Value;

  if (!JsonValueIsObject (Object)) {
    return NULL;
  }

  Value = JsonObjectGetValue (JsonValueGetObject (Object), Key);
  if (Value == NULL) {
    return NULL;
  }

  return JsonValueGetAsciiString (Value);
}

/**
  Consume the first IPv4StaticAddresses element. The converter hands the
  array over as a link of raw JSON blobs, so this parses element zero
  with JsonLib; an empty array clears all three questions, so a BMC
  PATCH of "IPv4StaticAddresses": [] retires a static configuration.

  @param[in] EthernetInterfaceCs  The consumed C structure.
**/
STATIC
VOID
ConsumeStaticAddresses (
  IN EFI_REDFISH_ETHERNETINTERFACE_V1_8_0_CS  *EthernetInterfaceCs
  )
{
  RedfishCS_Header          *Header;
  RedfishCS_Type_JSON_Data  *JsonData;
  EDKII_JSON_VALUE          Element;

  if (IsLinkEmpty (&EthernetInterfaceCs->IPv4StaticAddresses)) {
    ConsumeStringProperty (EthernetInterfaceCs->odata_id, ETH_LANG_ADDRESS, NULL);
    ConsumeStringProperty (EthernetInterfaceCs->odata_id, ETH_LANG_SUBNET_MASK, NULL);
    ConsumeStringProperty (EthernetInterfaceCs->odata_id, ETH_LANG_GATEWAY, NULL);
    return;
  }

  Header = (RedfishCS_Header *)GetFirstLink (&EthernetInterfaceCs->IPv4StaticAddresses);
  if (Header->ResourceType != RedfishCS_Type_JSON) {
    DEBUG ((DEBUG_ERROR, "%a: unexpected IPv4StaticAddresses element type %d\n", __func__, Header->ResourceType));
    return;
  }

  JsonData = (RedfishCS_Type_JSON_Data *)Header;
  if (JsonData->JsonText == NULL) {
    return;
  }

  Element = JsonLoadString (JsonData->JsonText, 0, NULL);
  if (Element == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: IPv4StaticAddresses[0] does not parse\n", __func__));
    return;
  }

  //
  // A JSON null member (or a missing one) clears its question - the
  // Redfish idiom for "unset this".
  //
  ConsumeStringProperty (
    EthernetInterfaceCs->odata_id,
    ETH_LANG_ADDRESS,
    (CHAR8 *)JsonObjectAsciiMember (Element, "Address")
    );
  ConsumeStringProperty (
    EthernetInterfaceCs->odata_id,
    ETH_LANG_SUBNET_MASK,
    (CHAR8 *)JsonObjectAsciiMember (Element, "SubnetMask")
    );
  ConsumeStringProperty (
    EthernetInterfaceCs->odata_id,
    ETH_LANG_GATEWAY,
    (CHAR8 *)JsonObjectAsciiMember (Element, "Gateway")
    );

  JsonValueFree (Element);
}

/**
  Consume resource from given URI.

  @param[in]   Private             Pointer to REDFISH_RESOURCE_COMMON_PRIVATE instance.
  @param[in]   Json                The JSON to consume.
  @param[in]   HeaderEtag          The Etag string returned in HTTP header.

  @retval EFI_SUCCESS              Value is returned successfully.
  @retval Others                   Some error happened.

**/
EFI_STATUS
RedfishConsumeResourceCommon (
  IN     REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN     CHAR8                            *Json,
  IN     CHAR8                            *HeaderEtag OPTIONAL
  )
{
  EFI_STATUS                               Status;
  EFI_REDFISH_ETHERNETINTERFACE_V1_8_0     *EthernetInterface;
  EFI_REDFISH_ETHERNETINTERFACE_V1_8_0_CS  *EthernetInterfaceCs;
  EFI_STRING                               ConfigureLang;
  RedfishCS_char_Array                     *NameServer;

  if ((Private == NULL) || IS_EMPTY_STRING (Json)) {
    return EFI_INVALID_PARAMETER;
  }

  EthernetInterface = NULL;

  Status = Private->JsonStructProtocol->ToStructure (
                                          Private->JsonStructProtocol,
                                          NULL,
                                          Json,
                                          (EFI_REST_JSON_STRUCTURE_HEADER **)&EthernetInterface
                                          );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: ToStructure() failed: %r\n", __func__, Status));
    return Status;
  }

  EthernetInterfaceCs = EthernetInterface->EthernetInterface;

  //
  // When the ETag is identical to what we consumed before, nothing on
  // the BMC changed and there is nothing to apply.
  //
  if (CheckEtag (Private->Uri, HeaderEtag, EthernetInterfaceCs->odata_etag)) {
    DEBUG ((REDFISH_DEBUG_TRACE, "%a: ETag identical, skip consume: %s\n", __func__, Private->Uri));
    Status = EFI_SUCCESS;
    goto ON_RELEASE;
  }

  //
  // DHCPv4/DHCPEnabled
  //
  if ((EthernetInterfaceCs->DHCPv4 != NULL) && (EthernetInterfaceCs->DHCPv4->DHCPEnabled != NULL)) {
    ConfigureLang = GetConfigureLang (EthernetInterfaceCs->odata_id, ETH_LANG_DHCP_ENABLED);
    if (ConfigureLang != NULL) {
      Status = ApplyFeatureSettingsBooleanType (
                 RESOURCE_SCHEMA,
                 RESOURCE_SCHEMA_VERSION,
                 ConfigureLang,
                 (BOOLEAN)*EthernetInterfaceCs->DHCPv4->DHCPEnabled
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: apply DHCPEnabled failed: %r\n", __func__, Status));
      }

      FreePool (ConfigureLang);
    }
  }

  //
  // IPv4StaticAddresses[0]
  //
  ConsumeStaticAddresses (EthernetInterfaceCs);

  //
  // StaticNameServers: first two entries feed the two DNS questions,
  // fewer entries clear the remainder. MACAddress is report-only and is
  // never consumed.
  //
  if (EthernetInterfaceCs->StaticNameServers != NULL) {
    NameServer = EthernetInterfaceCs->StaticNameServers;
    ConsumeStringProperty (EthernetInterfaceCs->odata_id, ETH_LANG_DNS1, NameServer->ArrayValue);
    ConsumeStringProperty (
      EthernetInterfaceCs->odata_id,
      ETH_LANG_DNS2,
      (NameServer->Next != NULL) ? NameServer->Next->ArrayValue : NULL
      );
  }

  Status = EFI_SUCCESS;

ON_RELEASE:
  Private->JsonStructProtocol->DestoryStructure (
                                 Private->JsonStructProtocol,
                                 (EFI_REST_JSON_STRUCTURE_HEADER *)EthernetInterface
                                 );

  return Status;
}

/**
  Read one HII-backed string property; empty strings collapse to NULL
  so callers can treat "unset" uniformly. Caller frees.

  @param[in] PropertyName   Configure-language tail, as CHAR16.
  @param[in] ConfigureLang  The instance configure language.

  @return The value, or NULL.
**/
STATIC
CHAR8 *
FetchStringProperty (
  IN EFI_STRING  PropertyName,
  IN EFI_STRING  ConfigureLang
  )
{
  CHAR8  *Value;

  Value = GetPropertyStringValue (RESOURCE_SCHEMA, RESOURCE_SCHEMA_VERSION, PropertyName, ConfigureLang);
  if ((Value != NULL) && (Value[0] == '\0')) {
    FreePool (Value);
    Value = NULL;
  }

  return Value;
}

/**
  Build the JSON payload carrying this driver's managed property set
  from the HII values behind the given instance configure language.

  @param[in]  ConfigureLang  Instance configure language (/EthernetInterfaces/{N}).
  @param[in]  ResourceId     Member Id to embed, or NULL to omit identity.
  @param[out] ResultJson     The payload; caller frees.

  @retval EFI_SUCCESS        Payload built.
  @retval EFI_NOT_FOUND      No managed question is published at all.
  @retval Others             Allocation or dump failure.

**/
STATIC
EFI_STATUS
BuildEthernetInterfacePayload (
  IN  EFI_STRING  ConfigureLang,
  IN  CHAR8       *ResourceId OPTIONAL,
  OUT CHAR8       **ResultJson
  )
{
  EDKII_JSON_VALUE  Payload;
  EDKII_JSON_VALUE  Dhcp;
  EDKII_JSON_VALUE  StaticAddresses;
  EDKII_JSON_VALUE  AddressEntry;
  EDKII_JSON_VALUE  NameServers;
  BOOLEAN           *DhcpEnabled;
  CHAR8             *Mac;
  CHAR8             *Address;
  CHAR8             *SubnetMask;
  CHAR8             *Gateway;
  CHAR8             *Dns1;
  CHAR8             *Dns2;
  BOOLEAN           AnyProperty;

  Payload = JsonValueInitObject ();
  if (Payload == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  AnyProperty = FALSE;

  if (ResourceId != NULL) {
    JsonObjectSetValue (JsonValueGetObject (Payload), "@odata.type", JsonValueInitAsciiString ("#EthernetInterface.v1_8_0.EthernetInterface"));
    JsonObjectSetValue (JsonValueGetObject (Payload), "Id", JsonValueInitAsciiString (ResourceId));
    JsonObjectSetValue (JsonValueGetObject (Payload), "Name", JsonValueInitAsciiString ("Ethernet Interface"));
  }

  Mac = FetchStringProperty (L"MACAddress", ConfigureLang);
  if (Mac != NULL) {
    JsonObjectSetValue (JsonValueGetObject (Payload), "MACAddress", JsonValueInitAsciiString (Mac));
    FreePool (Mac);
    AnyProperty = TRUE;
  }

  DhcpEnabled = GetPropertyBooleanValue (RESOURCE_SCHEMA, RESOURCE_SCHEMA_VERSION, L"DHCPv4/DHCPEnabled", ConfigureLang);
  if (DhcpEnabled != NULL) {
    Dhcp = JsonValueInitObject ();
    if (Dhcp != NULL) {
      JsonObjectSetValue (JsonValueGetObject (Dhcp), "DHCPEnabled", JsonValueInitBoolean (*DhcpEnabled));
      JsonObjectSetValue (JsonValueGetObject (Payload), "DHCPv4", Dhcp);
      AnyProperty = TRUE;
    }

    FreePool (DhcpEnabled);
  }

  //
  // IPv4StaticAddresses: one element when an address is configured,
  // empty array otherwise - PATCHing [] is how the host clears a
  // stale static entry off the BMC.
  //
  Address    = FetchStringProperty (L"IPv4StaticAddresses/[1]/Address", ConfigureLang);
  SubnetMask = FetchStringProperty (L"IPv4StaticAddresses/[1]/SubnetMask", ConfigureLang);
  Gateway    = FetchStringProperty (L"IPv4StaticAddresses/[1]/Gateway", ConfigureLang);

  StaticAddresses = JsonValueInitArray ();
  if (StaticAddresses != NULL) {
    if (Address != NULL) {
      AddressEntry = JsonValueInitObject ();
      if (AddressEntry != NULL) {
        JsonObjectSetValue (JsonValueGetObject (AddressEntry), "Address", JsonValueInitAsciiString (Address));
        if (SubnetMask != NULL) {
          JsonObjectSetValue (JsonValueGetObject (AddressEntry), "SubnetMask", JsonValueInitAsciiString (SubnetMask));
        }

        if (Gateway != NULL) {
          JsonObjectSetValue (JsonValueGetObject (AddressEntry), "Gateway", JsonValueInitAsciiString (Gateway));
        }

        JsonArrayAppendValue (JsonValueGetArray (StaticAddresses), AddressEntry);
      }
    }

    JsonObjectSetValue (JsonValueGetObject (Payload), "IPv4StaticAddresses", StaticAddresses);
    AnyProperty = TRUE;
  }

  if (Address != NULL) {
    FreePool (Address);
  }

  if (SubnetMask != NULL) {
    FreePool (SubnetMask);
  }

  if (Gateway != NULL) {
    FreePool (Gateway);
  }

  //
  // StaticNameServers, same clearing contract.
  //
  Dns1 = FetchStringProperty (L"StaticNameServers/[1]", ConfigureLang);
  Dns2 = FetchStringProperty (L"StaticNameServers/[2]", ConfigureLang);

  NameServers = JsonValueInitArray ();
  if (NameServers != NULL) {
    if (Dns1 != NULL) {
      JsonArrayAppendValue (JsonValueGetArray (NameServers), JsonValueInitAsciiString (Dns1));
    }

    if (Dns2 != NULL) {
      JsonArrayAppendValue (JsonValueGetArray (NameServers), JsonValueInitAsciiString (Dns2));
    }

    JsonObjectSetValue (JsonValueGetObject (Payload), "StaticNameServers", NameServers);
    AnyProperty = TRUE;
  }

  if (Dns1 != NULL) {
    FreePool (Dns1);
  }

  if (Dns2 != NULL) {
    FreePool (Dns2);
  }

  if (!AnyProperty) {
    JsonValueFree (Payload);
    return EFI_NOT_FOUND;
  }

  *ResultJson = JsonDumpString (Payload, EDKII_JSON_COMPACT);
  JsonValueFree (Payload);

  return (*ResultJson == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

/**
  POST one new collection member built from the given instance
  configure language, and record the resulting URI in the config
  language map.

  @param[in] Private        The driver context, Uri pointing at the collection.
  @param[in] Instance       The {N} instance number from the configure language.
  @param[in] ConfigureLang  The unified instance configure language.

  @retval EFI_SUCCESS       Member created.
  @retval Others            Some error happened.

**/
STATIC
EFI_STATUS
ProvisioningEthernetInterfaceResource (
  IN REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN UINTN                            Instance,
  IN EFI_STRING                       ConfigureLang
  )
{
  EFI_STATUS        Status;
  CHAR8             ResourceId[16];
  CHAR8             *Json;
  EFI_STRING        NewResourceLocation;
  REDFISH_RESPONSE  Response;

  //
  // Linux-style member ids, {1} -> eth0: the naming the BMC's tooling
  // and this platform's earlier inventory reports already use.
  //
  AsciiSPrint (ResourceId, sizeof (ResourceId), "eth%u", (UINT32)((Instance == 0) ? 0 : Instance - 1));

  Json   = NULL;
  Status = BuildEthernetInterfacePayload (ConfigureLang, ResourceId, &Json);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: build payload for %s failed: %r\n", __func__, ConfigureLang, Status));
    return Status;
  }

  ZeroMem (&Response, sizeof (Response));
  NewResourceLocation = NULL;

  Status = RedfishHttpPostResource (Private->RedfishService, Private->Uri, Json, &Response);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: POST %s failed: %r\n", __func__, Private->Uri, Status));
    goto ON_RELEASE;
  }

  //
  // The Location header seeds the config-language map; without it a
  // consume can only happen after the next boot's identify pass.
  //
  Status = GetHttpResponseLocation (&Response, &NewResourceLocation);
  if (EFI_ERROR (Status) || (NewResourceLocation == NULL)) {
    DEBUG ((DEBUG_ERROR, "%a: no Location header in POST response\n", __func__));
    Status = EFI_DEVICE_ERROR;
    goto ON_RELEASE;
  }

  Status = RedfishSetRedfishUri (ConfigureLang, NewResourceLocation);

ON_RELEASE:
  if (NewResourceLocation != NULL) {
    FreePool (NewResourceLocation);
  }

  RedfishHttpFreeResponse (&Response);
  FreePool (Json);

  return Status;
}

/**
  Create every instance the HII database publishes (POST mode).

  @param[in] Private  The driver context.

  @retval EFI_SUCCESS  All members created.
  @retval Others       Some error happened.

**/
STATIC
EFI_STATUS
ProvisioningEthernetInterfaceResources (
  IN REDFISH_RESOURCE_COMMON_PRIVATE  *Private
  )
{
  EFI_STATUS                                   Status;
  REDFISH_FEATURE_ARRAY_TYPE_CONFIG_LANG_LIST  UnifiedConfigureLangList;
  UINTN                                        Index;

  Status = RedfishFeatureGetUnifiedArrayTypeConfigureLang (
             RESOURCE_SCHEMA,
             RESOURCE_SCHEMA_VERSION,
             CONFIG_LANG_ARRAY_PATTERN,
             &UnifiedConfigureLangList
             );
  if (EFI_ERROR (Status) || (UnifiedConfigureLangList.Count == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: no EthernetInterface configure language published: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  EdkIIRedfishResourceSetConfigureLang (mRedfishResourceConfigProtocolHandle, &UnifiedConfigureLangList);

  for (Index = 0; Index < UnifiedConfigureLangList.Count; Index++) {
    Status = ProvisioningEthernetInterfaceResource (
               Private,
               UnifiedConfigureLangList.List[Index].Index,
               UnifiedConfigureLangList.List[Index].ConfigureLang
               );
    FreePool (UnifiedConfigureLangList.List[Index].ConfigureLang);
  }

  FreePool (UnifiedConfigureLangList.List);

  return Status;
}

/**
  PATCH the managed property set onto an already-existing member.

  @param[in] Private  The driver context, Uri pointing at the member.

  @retval EFI_SUCCESS  Member updated.
  @retval Others       Some error happened.

**/
STATIC
EFI_STATUS
ProvisioningEthernetInterfaceExistResource (
  IN REDFISH_RESOURCE_COMMON_PRIVATE  *Private
  )
{
  EFI_STATUS        Status;
  EFI_STRING        ConfigureLang;
  CHAR8             *Json;
  REDFISH_RESPONSE  Response;

  ConfigureLang = RedfishGetConfigLanguage (Private->Uri);
  if (ConfigureLang == NULL) {
    return EFI_NOT_FOUND;
  }

  Json   = NULL;
  Status = BuildEthernetInterfacePayload (ConfigureLang, NULL, &Json);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: build payload for %s failed: %r\n", __func__, ConfigureLang, Status));
    goto ON_RELEASE;
  }

  ZeroMem (&Response, sizeof (Response));
  Status = RedfishHttpPatchResource (Private->RedfishService, Private->Uri, Json, &Response);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: PATCH %s failed: %r\n", __func__, Private->Uri, Status));
  }

  RedfishHttpFreeResponse (&Response);

ON_RELEASE:
  if (Json != NULL) {
    FreePool (Json);
  }

  FreePool (ConfigureLang);

  return Status;
}

/**
  Provisioning redfish resource by given URI.

  @param[in]   Private             Pointer to REDFISH_RESOURCE_COMMON_PRIVATE instance.
  @param[in]   ResourceExist       TRUE if resource exists, PATCH method will be used.
                                   FALSE if resource does not exist, POST method is used.

  @retval EFI_SUCCESS              Value is returned successfully.
  @retval Others                   Some error happened.

**/
EFI_STATUS
RedfishProvisioningResourceCommon (
  IN     REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN     BOOLEAN                          ResourceExist
  )
{
  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return ResourceExist ?
         ProvisioningEthernetInterfaceExistResource (Private) :
         ProvisioningEthernetInterfaceResources (Private);
}

/**
  Check resource from given URI: does the member already carry the
  managed property set? DHCPv4 is the marker - it is always emitted by
  the provisioning path, and testing a flat property sidesteps the
  nested-array matching the generic helper cannot do.

  @param[in]   Private             Pointer to REDFISH_RESOURCE_COMMON_PRIVATE instance.
  @param[in]   Json                The JSON to check.
  @param[in]   HeaderEtag          The Etag string returned in HTTP header.

  @retval EFI_SUCCESS              The managed properties are present.
  @retval EFI_NOT_FOUND            They are not; provisioning is needed.

**/
EFI_STATUS
RedfishCheckResourceCommon (
  IN     REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN     CHAR8                            *Json,
  IN     CHAR8                            *HeaderEtag OPTIONAL
  )
{
  EDKII_JSON_VALUE  Resource;
  EDKII_JSON_VALUE  Marker;
  EFI_STATUS        Status;

  if ((Private == NULL) || IS_EMPTY_STRING (Json)) {
    return EFI_INVALID_PARAMETER;
  }

  Resource = JsonLoadString (Json, 0, NULL);
  if (Resource == NULL) {
    return EFI_DEVICE_ERROR;
  }

  Marker = JsonObjectGetValue (JsonValueGetObject (Resource), "DHCPv4");
  Status = (Marker != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;

  JsonValueFree (Resource);

  return Status;
}

/**
  Update resource to given URI: push the current HII values.

  @param[in]   Private             Pointer to REDFISH_RESOURCE_COMMON_PRIVATE instance.
  @param[in]   Json                The target resource's current JSON (unused;
                                   the full managed set is always pushed).

  @retval EFI_SUCCESS              Value is returned successfully.
  @retval Others                   Some error happened.

**/
EFI_STATUS
RedfishUpdateResourceCommon (
  IN     REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN     CHAR8                            *Json
  )
{
  if (Private == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return ProvisioningEthernetInterfaceExistResource (Private);
}

/**
  Identify resource from given URI: claim it for the single instance
  the HII database publishes and seed the config-language map, so a
  member that predates this boot's (empty) map is adopted rather than
  duplicated.

  @param[in]   Private             Pointer to REDFISH_RESOURCE_COMMON_PRIVATE instance.
  @param[in]   Json                The JSON of the resource.

  @retval EFI_SUCCESS              This is a resource we manage.
  @retval EFI_UNSUPPORTED          It is not.

**/
EFI_STATUS
RedfishIdentifyResourceCommon (
  IN     REDFISH_RESOURCE_COMMON_PRIVATE  *Private,
  IN     CHAR8                            *Json
  )
{
  BOOLEAN                                      Supported;
  EFI_STATUS                                   Status;
  REDFISH_FEATURE_ARRAY_TYPE_CONFIG_LANG_LIST  UnifiedConfigureLangList;
  UINTN                                        Index;

  Supported = RedfishIdentifyResource (Private->Uri, Json);
  if (!Supported) {
    return EFI_UNSUPPORTED;
  }

  Status = RedfishFeatureGetUnifiedArrayTypeConfigureLang (
             RESOURCE_SCHEMA,
             RESOURCE_SCHEMA_VERSION,
             CONFIG_LANG_ARRAY_PATTERN,
             &UnifiedConfigureLangList
             );
  if (EFI_ERROR (Status) || (UnifiedConfigureLangList.Count == 0)) {
    return EFI_UNSUPPORTED;
  }

  Status = RedfishSetRedfishUri (UnifiedConfigureLangList.List[0].ConfigureLang, Private->Uri);
  if (!EFI_ERROR (Status)) {
    EdkIIRedfishResourceSetConfigureLang (mRedfishResourceConfigProtocolHandle, &UnifiedConfigureLangList);
  }

  for (Index = 0; Index < UnifiedConfigureLangList.Count; Index++) {
    FreePool (UnifiedConfigureLangList.List[Index].ConfigureLang);
  }

  FreePool (UnifiedConfigureLangList.List);

  return Status;
}

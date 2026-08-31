/** @file

  Redfish Processor feature driver - schema-specific logic.

  Modeled on this platform's RedfishEthernetInterfaceDxe (itself the
  Memory/V1_7_1 shape), and much simpler: the managed set is two scalar
  properties on the Processor resource, the standard way a BMC caps a
  CPU (Processor schema v1_10_0+):

    SpeedLimitMHz   integer. On this platform 0 means "no override" -
                    Redfish null cannot round-trip through the
                    converter (a JSON null and an absent member both
                    parse to a NULL pointer), so the sentinel is what
                    clears the cap.
    SpeedLocked     boolean. TRUE pins the cores at the cap
                    (force_turbo); FALSE lets DVFS scale below it.

  The member itself is richer than this set: RpiRedfishSyncDxe POSTs
  the SMBIOS-derived inventory (model, cores, MaxSpeedMHz...) every
  boot, and unlike the NIC case that report has no HII substitute, so
  both writers deliberately coexist - the inventory POST never carries
  the two managed properties, the BMC preserves operator-staged values
  across the re-POST, and this driver's Update PATCHes only its own
  set. Check tests for the SpeedLocked marker so an inventory-only
  member still triggers provisioning of the managed properties.

  The HII questions behind these live in CpuConfigDxe; a consumed
  change lands in the CpuClockPolicy variable via
  RedfishPlatformConfigSetValue, the Apply* helpers raise
  PcdRedfishSystemRebootRequired, the feature core cold-resets, and
  CpuConfigDxe's ReadyToBoot sync writes config.txt so the VPU applies
  the cap at the reset after that.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "ProcessorCommon.h"

REDFISH_RESOURCE_COMMON_PRIVATE  *mRedfishResourcePrivate             = NULL;
EFI_HANDLE                       mRedfishResourceConfigProtocolHandle = NULL;

//
// The configure-language tails this driver manages, relative to the
// instance (see CpuConfigDxeMap.uni).
//
#define PROC_LANG_SPEED_LIMIT   "SpeedLimitMHz"
#define PROC_LANG_SPEED_LOCKED  "SpeedLocked"

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
  EFI_STATUS                        Status;
  EFI_REDFISH_PROCESSOR_V1_14_0     *Processor;
  EFI_REDFISH_PROCESSOR_V1_14_0_CS  *ProcessorCs;
  EFI_STRING                        ConfigureLang;
  INT64                             LimitMhz;

  if ((Private == NULL) || IS_EMPTY_STRING (Json)) {
    return EFI_INVALID_PARAMETER;
  }

  Processor = NULL;

  Status = Private->JsonStructProtocol->ToStructure (
                                          Private->JsonStructProtocol,
                                          NULL,
                                          Json,
                                          (EFI_REST_JSON_STRUCTURE_HEADER **)&Processor
                                          );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: ToStructure() failed: %r\n", __func__, Status));
    return Status;
  }

  ProcessorCs = Processor->Processor;

  //
  // When the ETag is identical to what we consumed before, nothing on
  // the BMC changed and there is nothing to apply.
  //
  if (CheckEtag (Private->Uri, HeaderEtag, ProcessorCs->odata_etag)) {
    DEBUG ((REDFISH_DEBUG_TRACE, "%a: ETag identical, skip consume: %s\n", __func__, Private->Uri));
    Status = EFI_SUCCESS;
    goto ON_RELEASE;
  }

  //
  // SpeedLimitMHz. Clamped into what the UINT16 HII question can hold
  // before it goes anywhere near SetValue - the HII write is
  // width-truncating, so an unclamped 65536 would wrap to 0 and
  // silently remove the cap instead of maxing it.
  //
  if (ProcessorCs->SpeedLimitMHz != NULL) {
    ConfigureLang = GetConfigureLang (ProcessorCs->odata_id, PROC_LANG_SPEED_LIMIT);
    if (ConfigureLang != NULL) {
      LimitMhz = *ProcessorCs->SpeedLimitMHz;
      if (LimitMhz < 0) {
        LimitMhz = 0;
      }

      if (LimitMhz > 3000) {
        LimitMhz = 3000;
      }

      Status = ApplyFeatureSettingsNumericType (
                 RESOURCE_SCHEMA,
                 RESOURCE_SCHEMA_VERSION,
                 ConfigureLang,
                 (INTN)LimitMhz
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: apply SpeedLimitMHz failed: %r\n", __func__, Status));
      }

      FreePool (ConfigureLang);
    }
  }

  //
  // SpeedLocked.
  //
  if (ProcessorCs->SpeedLocked != NULL) {
    ConfigureLang = GetConfigureLang (ProcessorCs->odata_id, PROC_LANG_SPEED_LOCKED);
    if (ConfigureLang != NULL) {
      Status = ApplyFeatureSettingsBooleanType (
                 RESOURCE_SCHEMA,
                 RESOURCE_SCHEMA_VERSION,
                 ConfigureLang,
                 (BOOLEAN)*ProcessorCs->SpeedLocked
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: apply SpeedLocked failed: %r\n", __func__, Status));
      }

      FreePool (ConfigureLang);
    }
  }

  Status = EFI_SUCCESS;

ON_RELEASE:
  Private->JsonStructProtocol->DestoryStructure (
                                 Private->JsonStructProtocol,
                                 (EFI_REST_JSON_STRUCTURE_HEADER *)Processor
                                 );

  return Status;
}

/**
  Build the JSON payload carrying this driver's managed property set
  from the HII values behind the given instance configure language.

  @param[in]  ConfigureLang  Instance configure language (/Processors/{N}).
  @param[in]  ResourceId     Member Id to embed, or NULL to omit identity.
  @param[out] ResultJson     The payload; caller frees.

  @retval EFI_SUCCESS        Payload built.
  @retval EFI_NOT_FOUND      No managed question is published at all.
  @retval Others             Allocation or dump failure.

**/
STATIC
EFI_STATUS
BuildProcessorPayload (
  IN  EFI_STRING  ConfigureLang,
  IN  CHAR8       *ResourceId OPTIONAL,
  OUT CHAR8       **ResultJson
  )
{
  EDKII_JSON_VALUE  Payload;
  INT64             *LimitMhz;
  BOOLEAN           *Locked;
  BOOLEAN           AnyProperty;

  Payload = JsonValueInitObject ();
  if (Payload == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  AnyProperty = FALSE;

  if (ResourceId != NULL) {
    JsonObjectSetValue (JsonValueGetObject (Payload), "@odata.type", JsonValueInitAsciiString ("#Processor.v1_14_0.Processor"));
    JsonObjectSetValue (JsonValueGetObject (Payload), "Id", JsonValueInitAsciiString (ResourceId));
    JsonObjectSetValue (JsonValueGetObject (Payload), "Name", JsonValueInitAsciiString ("Processor"));
  }

  LimitMhz = GetPropertyNumericValue (RESOURCE_SCHEMA, RESOURCE_SCHEMA_VERSION, L"SpeedLimitMHz", ConfigureLang);
  if (LimitMhz != NULL) {
    JsonObjectSetValue (JsonValueGetObject (Payload), "SpeedLimitMHz", JsonValueInitInteger (*LimitMhz));
    FreePool (LimitMhz);
    AnyProperty = TRUE;
  }

  Locked = GetPropertyBooleanValue (RESOURCE_SCHEMA, RESOURCE_SCHEMA_VERSION, L"SpeedLocked", ConfigureLang);
  if (Locked != NULL) {
    JsonObjectSetValue (JsonValueGetObject (Payload), "SpeedLocked", JsonValueInitBoolean (*Locked));
    FreePool (Locked);
    AnyProperty = TRUE;
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
  language map. In practice RpiRedfishSyncDxe's inventory POST creates
  the member long before the feature core walks the collection, so this
  runs only against a BMC with an empty Processors collection.

  @param[in] Private        The driver context, Uri pointing at the collection.
  @param[in] Instance       The {N} instance number from the configure language.
  @param[in] ConfigureLang  The unified instance configure language.

  @retval EFI_SUCCESS       Member created.
  @retval Others            Some error happened.

**/
STATIC
EFI_STATUS
ProvisioningProcessorResource (
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
  // {1} -> CPU1: the id convention the BMC's built-in placeholder
  // already uses.
  //
  AsciiSPrint (ResourceId, sizeof (ResourceId), "CPU%u", (UINT32)((Instance == 0) ? 1 : Instance));

  Json   = NULL;
  Status = BuildProcessorPayload (ConfigureLang, ResourceId, &Json);
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
ProvisioningProcessorResources (
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
    DEBUG ((DEBUG_ERROR, "%a: no Processor configure language published: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  EdkIIRedfishResourceSetConfigureLang (mRedfishResourceConfigProtocolHandle, &UnifiedConfigureLangList);

  for (Index = 0; Index < UnifiedConfigureLangList.Count; Index++) {
    Status = ProvisioningProcessorResource (
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
  PATCH the managed property set onto an already-existing member -
  which on this platform is normally the one RpiRedfishSyncDxe's
  inventory POST created; this adds/refreshes only the two managed
  properties and leaves the inventory alone.

  @param[in] Private  The driver context, Uri pointing at the member.

  @retval EFI_SUCCESS  Member updated.
  @retval Others       Some error happened.

**/
STATIC
EFI_STATUS
ProvisioningProcessorExistResource (
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
  Status = BuildProcessorPayload (ConfigureLang, NULL, &Json);
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
         ProvisioningProcessorExistResource (Private) :
         ProvisioningProcessorResources (Private);
}

/**
  Check resource from given URI: does the member already carry the
  managed property set? SpeedLocked is the marker - always emitted by
  the provisioning path, and absent from RpiRedfishSyncDxe's
  inventory-only POST, so a freshly re-POSTed member still gets the
  managed properties PATCHed back on.

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

  Marker = JsonObjectGetValue (JsonValueGetObject (Resource), "SpeedLocked");
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

  return ProvisioningProcessorExistResource (Private);
}

/**
  Identify resource from given URI: claim it for the single instance
  the HII database publishes and seed the config-language map, so the
  member RpiRedfishSyncDxe's inventory POST created is adopted rather
  than duplicated.

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

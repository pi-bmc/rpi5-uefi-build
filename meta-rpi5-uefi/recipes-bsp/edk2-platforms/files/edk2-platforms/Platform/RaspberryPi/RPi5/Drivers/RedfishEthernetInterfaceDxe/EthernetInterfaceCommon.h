/** @file

  Redfish EthernetInterface feature driver - common header.

  The schema constants every RedfishClientPkg feature driver carries.
  RESOURCE_SCHEMA_FULL must match the #langdef line in EthConfigDxeMap.uni
  byte for byte, and the configure-language paths this driver constructs
  must match that file's strings exactly - RedfishPlatformConfigDxe does
  a plain string compare, nothing more (skew reads as silent per-property
  EFI_NOT_FOUND).

  The configure language is rooted at the schema, not the service:
  /EthernetInterfaces/{1}/..., never /Systems/{1}/EthernetInterfaces/...
  (the same way the Memory sample uses /Memory/{1}/...).

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef ETHERNET_INTERFACE_COMMON_H_
#define ETHERNET_INTERFACE_COMMON_H_

#include <RedfishJsonStructure/EthernetInterface/v1_8_0/EfiEthernetInterfaceV1_8_0.h>
#include <RedfishResourceCommon.h>

#include <Library/JsonLib.h>

#define RESOURCE_SCHEMA            "EthernetInterface"
#define RESOURCE_SCHEMA_MAJOR      "1"
#define RESOURCE_SCHEMA_MINOR      "8"
#define RESOURCE_SCHEMA_ERRATA     "0"
#define RESOURCE_SCHEMA_VERSION    "v1_8_0"
#define CONFIG_LANG_ARRAY_PATTERN  L"/EthernetInterfaces/\\{.*\\}/"
#define RESOURCE_SCHEMA_FULL       "x-UEFI-redfish-EthernetInterface.v1_8_0"

#endif

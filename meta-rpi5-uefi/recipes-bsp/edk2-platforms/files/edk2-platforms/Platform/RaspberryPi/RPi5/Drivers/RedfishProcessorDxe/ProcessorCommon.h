/** @file

  Redfish Processor feature driver - schema constants.

  The version triple here, the converter paths in RPi5.dsc/.fdf, and
  the #langdef in CpuConfigDxeMap.uni must move as one: a feature
  driver looks its HII questions up by RESOURCE_SCHEMA_FULL byte for
  byte, and skew reads as silent per-property EFI_NOT_FOUND.

  Configure language is rooted at the schema, not the service:
  /Processors/{1}/..., never /Systems/{1}/Processors/....

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef EFI_REDFISH_PROCESSOR_COMMON_H_
#define EFI_REDFISH_PROCESSOR_COMMON_H_

#include <RedfishJsonStructure/Processor/v1_14_0/EfiProcessorV1_14_0.h>
#include <RedfishResourceCommon.h>

#include <Library/JsonLib.h>

//
// Schema information.
//
#define RESOURCE_SCHEMA            "Processor"
#define RESOURCE_SCHEMA_MAJOR      "1"
#define RESOURCE_SCHEMA_MINOR      "14"
#define RESOURCE_SCHEMA_ERRATA     "0"
#define RESOURCE_SCHEMA_VERSION    "v1_14_0"
#define CONFIG_LANG_ARRAY_PATTERN  L"/Processors/\\{.*\\}/"
#define RESOURCE_SCHEMA_FULL       "x-UEFI-redfish-Processor.v1_14_0"

#endif

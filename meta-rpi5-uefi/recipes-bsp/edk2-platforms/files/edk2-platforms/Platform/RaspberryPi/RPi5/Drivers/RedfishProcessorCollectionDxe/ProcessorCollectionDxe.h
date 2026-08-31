/** @file

  Redfish ProcessorCollection collection driver - header.

  REDFISH_MANAGED_URI is the feature-core registration path: the
  trailing "/{}" marks the collection node whose members this driver
  enumerates. Registered under Systems only - this platform publishes
  no chassis-scoped NICs.

  Copyright (c) 2026, the pi-bmc contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef ETHERNET_INTERFACE_COLLECTION_DXE_H_
#define ETHERNET_INTERFACE_COLLECTION_DXE_H_

#include <RedfishJsonStructure/ProcessorCollection/EfiProcessorCollection.h>
#include <RedfishCollectionCommon.h>

#define REDFISH_SCHEMA_NAME  "Processor"
#define REDFISH_MANAGED_URI  L"Systems/{}/Processors/{}"
#define MAX_URI_LENGTH       256

#endif

# edk2-redfish-client (RedfishClientPkg): the feature layer above the host
# interface - ComputerSystemDxe, BiosDxe, BootOptionDxe and their JSON
# converters. Sed-included into RPi5.dsc by the edk2-rpi5-firmware recipe
# when RPI5_REDFISH_CLIENT=1, alongside RpiRedfish.dsc.inc (the stack it
# sits on). The client tree rides PACKAGES_PATH (EDK2_REDFISH_CLIENT_PATH),
# pinned to the newest revision this edk2 fork's RedfishPkg can satisfy -
# see the SRCREV_redfishclient comment in the recipe for the exact window.
#
# The client's own RedfishClient*.dsc.inc files are deliberately not used:
# they are gated on REDFISH_CLIENT / REDFISH_CLIENT_ALL_AUTOGENED macros
# with no definition at this insertion point, and explicit lists let the
# nuc-bios-build 0100 adjustments be encoded directly instead of patching
# the client tree:
#
#   * ComputerSystemDxe gets RedfishResourceIdentifyLibNull (the global
#     mapping below) rather than the UUID-matching instance - on a DSP0270
#     point-to-point link there is one host and one ComputerSystem, nothing
#     to disambiguate.
#   * The Memory feature (MemoryDxe, MemoryCollectionDxe, HiiToRedfishMemoryDxe,
#     converters) is absent: its only data source is HII questions in the
#     x-UEFI-redfish-Memory namespace, which no platform publishes (memory
#     is inventory, not configuration), so it POSTs empty members.
#     RpiRedfishSyncDxe reports the real modules from SMBIOS type 17.
#   * HiiToRedfishBiosDxe (the BiosOption1..4 sample form) is absent: it
#     surfaces four fake attributes next to the real ones.
#   * The SecureBoot feature is absent: SECURE_BOOT_ENABLE is FALSE on this
#     platform.
#
# Same section discipline as RpiRedfish.dsc.inc: opens the sections it
# needs, MUST end by reopening [Components.common].

[LibraryClasses.common]
  # RedfishPkg classes only the client layer needs (the base stack in
  # RpiRedfish.dsc.inc deliberately leaves the HII-config path out).
  RedfishPlatformConfigLib|RedfishPkg/Library/RedfishPlatformConfigLib/RedfishPlatformConfigLib.inf
  HiiUtilityLib|RedfishPkg/Library/HiiUtilityLib/HiiUtilityLib.inf
  # Client-side classes, per RedfishClientLibs.dsc.inc at the pinned
  # revision (minus the Memory/SecureBoot schema libs, whose modules are
  # not built).
  RedfishFeatureUtilityLib|RedfishClientPkg/Library/RedfishFeatureUtilityLib/RedfishFeatureUtilityLib.inf
  ConverterCommonLib|RedfishClientPkg/ConverterLib/edk2library/ConverterCommonLib/ConverterCommonLib.inf
  RedfishResourceIdentifyLib|RedfishClientPkg/Library/RedfishResourceIdentifyLibNull/RedfishResourceIdentifyLibNull.inf
  EdkIIRedfishResourceConfigLib|RedfishClientPkg/Library/EdkIIRedfishResourceConfigLib/EdkIIRedfishResourceConfigLib.inf
  RedfishEventLib|RedfishClientPkg/Library/RedfishEventLib/RedfishEventLib.inf
  RedfishVersionLib|RedfishClientPkg/Library/RedfishVersionLib/RedfishVersionLib.inf
  RedfishAddendumLib|RedfishClientPkg/Library/RedfishAddendumLib/RedfishAddendumLib.inf
  ComputerSystemV1_5_0Lib|RedfishClientPkg/ConverterLib/edk2library/ComputerSystem/v1_5_0/Lib.inf
  ComputerSystemCollectionLib|RedfishClientPkg/ConverterLib/edk2library/ComputerSystemCollection/Lib.inf
  BiosV1_0_9Lib|RedfishClientPkg/ConverterLib/edk2library/Bios/v1_0_9/Lib.inf
  AttributeRegistryV1_3_6Lib|RedfishClientPkg/ConverterLib/edk2library/AttributeRegistry/v1_3_6/Lib.inf
  BootOptionCollectionLib|RedfishClientPkg/ConverterLib/edk2library/BootOptionCollection/Lib.inf
  BootOptionV1_0_4Lib|RedfishClientPkg/ConverterLib/edk2library/BootOption/v1_0_4/Lib.inf

[Components.common]
  # RedfishPkg/MdeModulePkg pieces the client depends on, kept out of the
  # base stack because only the feature layer consumes them:
  # RestJsonStructureDxe backs the converters, RedfishPlatformConfigDxe
  # harvests x-UEFI-redfish HII questions for BiosDxe/HiiToRedfish*, and
  # RegularExpressionDxe backs its namespace matching.
  RedfishPkg/RestJsonStructureDxe/RestJsonStructureDxe.inf
  RedfishPkg/RedfishPlatformConfigDxe/RedfishPlatformConfigDxe.inf
  MdeModulePkg/Universal/RegularExpressionDxe/RegularExpressionDxe.inf
  # Client core.
  RedfishClientPkg/RedfishFeatureCoreDxe/RedfishFeatureCoreDxe.inf
  RedfishClientPkg/RedfishETagDxe/RedfishETagDxe.inf
  RedfishClientPkg/RedfishConfigLangMapDxe/RedfishConfigLangMapDxe.inf
  RedfishClientPkg/HiiToRedfishBootDxe/HiiToRedfishBootDxe.inf
  # Features. ComputerSystemDxe deliberately has NO scoped
  # RedfishResourceIdentifyLib override: the global Null mapping above is
  # the point (see header).
  RedfishClientPkg/Features/ComputerSystem/v1_5_0/Dxe/ComputerSystemDxe.inf
  RedfishClientPkg/Features/ComputerSystemCollectionDxe/ComputerSystemCollectionDxe.inf
  RedfishClientPkg/Features/Bios/v1_0_9/Dxe/BiosDxe.inf
  RedfishClientPkg/Features/BiosAttributeRegistry/v1_3_6/BiosAttributeRegistryDxe.inf
  RedfishClientPkg/Features/BootOptionCollection/BootOptionCollectionDxe.inf
  RedfishClientPkg/Features/BootOption/v1_0_4/Dxe/BootOptionDxe.inf
  # Schema converters for exactly the features built.
  RedfishClientPkg/Converter/ComputerSystem/v1_5_0/RedfishComputerSystem_V1_5_0_Dxe.inf
  RedfishClientPkg/Converter/ComputerSystemCollection/RedfishComputerSystemCollection_Dxe.inf
  RedfishClientPkg/Converter/Bios/v1_0_9/RedfishBios_V1_0_9_Dxe.inf
  RedfishClientPkg/Converter/AttributeRegistry/v1_3_6/RedfishAttributeRegistry_V1_3_6_Dxe.inf
  RedfishClientPkg/Converter/BootOptionCollection/RedfishBootOptionCollection_Dxe.inf
  RedfishClientPkg/Converter/BootOption/v1_0_4/RedfishBootOption_V1_0_4_Dxe.inf

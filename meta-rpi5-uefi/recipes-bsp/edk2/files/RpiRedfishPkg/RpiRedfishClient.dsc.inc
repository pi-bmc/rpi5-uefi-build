# edk2-redfish-client (RedfishClientPkg): the feature layer above the host
# interface - ComputerSystemDxe, BiosDxe, BootOptionDxe and their JSON
# converters. Sed-included into RPi5.dsc by the edk2-rpi5-firmware recipe
# when RPI5_REDFISH_CLIENT=1, alongside RpiRedfish.dsc.inc (the stack it
# sits on). The client tree rides PACKAGES_PATH (EDK2_REDFISH_CLIENT_PATH),
# pinned to the newest revision this edk2 fork's RedfishPkg can satisfy -
# see the SRCREV_redfishclient comment in the recipe for the exact window.
#
# Both this list and the FV side (RpiRedfishClient.fdf.inc) are explicit
# rather than !includes of the client's own gated RedfishClient*.dsc.inc /
# RedfishClient.fdf.inc - those are keyed on REDFISH_CLIENT, which is not
# defined at the sed insertion point, and using them would also lose the
# nuc-bios-build 0100 library adjustment. The two lists MUST stay in sync:
# a module built here but missing from the .fdf.inc compiles and then never
# dispatches, which is silent.
#
#   * Every feature driver gets RedfishResourceIdentifyLibNull (the global
#     mapping below) rather than the UUID-matching instance - on a DSP0270
#     point-to-point link there is one host and one ComputerSystem, nothing
#     to disambiguate.
#
# Behavior notes for the sample/inventory modules built here:
# HiiToRedfishBiosDxe surfaces its BiosOption1..4 sample attributes next to
# the real annotated platform ones (useful as a known-good control when
# bringing the Bios path up); the Memory feature provisions from
# HiiToRedfishMemoryDxe's sample questions while RpiRedfishSyncDxe keeps
# reporting the real SMBIOS type 17 modules; and SecureBootDxe fail-opens
# against a BMC that does not serve Systems/1/SecureBoot.
#
# Same section discipline as RpiRedfish.dsc.inc: opens the sections it
# needs, MUST end by reopening [Components.common].

[LibraryClasses.common]
  # RedfishPkg classes only the client layer needs (the base stack in
  # RpiRedfish.dsc.inc deliberately leaves the HII-config path out).
  RedfishPlatformConfigLib|RedfishPkg/Library/RedfishPlatformConfigLib/RedfishPlatformConfigLib.inf
  HiiUtilityLib|RedfishPkg/Library/HiiUtilityLib/HiiUtilityLib.inf
  # Client-side classes, per RedfishClientLibs.dsc.inc at the pinned
  # revision, covering every module RedfishClient.fdf.inc names.
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
  MemoryV1_7_1Lib|RedfishClientPkg/ConverterLib/edk2library/Memory/v1_7_1/Lib.inf
  MemoryCollectionLib|RedfishClientPkg/ConverterLib/edk2library/MemoryCollection/Lib.inf
  SecureBootV1_1_0Lib|RedfishClientPkg/ConverterLib/edk2library/SecureBoot/v1_1_0/Lib.inf
  # SecureBootDxe's variable access. RPi5.dsc maps these itself, but only
  # inside its !if $(SECURE_BOOT_ENABLE) == TRUE block, so the client layer
  # has to cover the FALSE case - and must NOT redefine them in the TRUE
  # case. Their remaining deps (VariablePolicyHelperLib etc.) are global in
  # RPi5.dsc either way.
!if $(SECURE_BOOT_ENABLE) == FALSE
  SecureBootVariableLib|SecurityPkg/Library/SecureBootVariableLib/SecureBootVariableLib.inf
  PlatformPKProtectionLib|SecurityPkg/Library/PlatformPKProtectionLibVarPolicy/PlatformPKProtectionLibVarPolicy.inf
!endif

[PcdsFixedAtBuild.common]
  # RedfishPlatformConfigDxe feature bits (RedfishPlatformConfigDxe.h):
  #   0x01 REDFISH_PLATFORM_CONFIG_BUILD_MENU_PATH  - record each question's
  #        Setup menu path so BiosAttributeRegistryDxe can emit MenuPath in
  #        the registry, which is what makes the registry navigable.
  #   0x02 REDFISH_PLATFORM_CONFIG_ALLOW_SUPPRESSED - harvest questions that
  #        are currently suppressed.
  #
  # Bit 1 is not optional here. Both platform formsets hide most of their
  # questions behind suppressif on another question's value - RpiPlatformDxe
  # gates every ACPI question on SystemTableMode, FanConfigDxe gates the
  # fixed level and all four trip points on FanPolicy.Mode - so with the
  # default of 0 the BMC would see one attribute per formset and the rest
  # would appear and disappear depending on unrelated settings.
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishPlatformConfigFeatureProperty|0x03

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
  RedfishClientPkg/HiiToRedfishBiosDxe/HiiToRedfishBiosDxe.inf
  RedfishClientPkg/HiiToRedfishMemoryDxe/HiiToRedfishMemoryDxe.inf
  # Features. ComputerSystemDxe deliberately has NO scoped
  # RedfishResourceIdentifyLib override: the global Null mapping above is
  # the point (see header).
  RedfishClientPkg/Features/ComputerSystem/v1_5_0/Dxe/ComputerSystemDxe.inf
  RedfishClientPkg/Features/ComputerSystemCollectionDxe/ComputerSystemCollectionDxe.inf
  RedfishClientPkg/Features/Bios/v1_0_9/Dxe/BiosDxe.inf
  RedfishClientPkg/Features/BiosAttributeRegistry/v1_3_6/BiosAttributeRegistryDxe.inf
  RedfishClientPkg/Features/BootOptionCollection/BootOptionCollectionDxe.inf
  RedfishClientPkg/Features/BootOption/v1_0_4/Dxe/BootOptionDxe.inf
  RedfishClientPkg/Features/SecureBoot/v1_1_0/Dxe/SecureBootDxe.inf
  RedfishClientPkg/Features/Memory/V1_7_1/Dxe/MemoryDxe.inf
  RedfishClientPkg/Features/MemoryCollectionDxe/MemoryCollectionDxe.inf
  # Schema converters for exactly the features built.
  RedfishClientPkg/Converter/ComputerSystem/v1_5_0/RedfishComputerSystem_V1_5_0_Dxe.inf
  RedfishClientPkg/Converter/ComputerSystemCollection/RedfishComputerSystemCollection_Dxe.inf
  RedfishClientPkg/Converter/Bios/v1_0_9/RedfishBios_V1_0_9_Dxe.inf
  RedfishClientPkg/Converter/AttributeRegistry/v1_3_6/RedfishAttributeRegistry_V1_3_6_Dxe.inf
  RedfishClientPkg/Converter/BootOptionCollection/RedfishBootOptionCollection_Dxe.inf
  RedfishClientPkg/Converter/BootOption/v1_0_4/RedfishBootOption_V1_0_4_Dxe.inf
  RedfishClientPkg/Converter/Memory/v1_7_1/RedfishMemory_V1_7_1_Dxe.inf
  RedfishClientPkg/Converter/MemoryCollection/RedfishMemoryCollection_Dxe.inf
  RedfishClientPkg/Converter/SecureBoot/v1_1_0/RedfishSecureBoot_V1_1_0_Dxe.inf

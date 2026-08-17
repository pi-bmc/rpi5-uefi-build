# edk2-redfish-client drivers, sed-included into RPi5.fdf's FVMAIN when
# RPI5_REDFISH_CLIENT=1. Mirrors RpiRedfishClient.dsc.inc - see its header
# for what is deliberately absent (Memory feature, the sample Bios form,
# SecureBoot) and why the client's own RedfishClient.fdf.inc is not used.

  INF RedfishPkg/RestJsonStructureDxe/RestJsonStructureDxe.inf
  INF RedfishPkg/RedfishPlatformConfigDxe/RedfishPlatformConfigDxe.inf
  INF MdeModulePkg/Universal/RegularExpressionDxe/RegularExpressionDxe.inf
  INF RedfishClientPkg/RedfishFeatureCoreDxe/RedfishFeatureCoreDxe.inf
  INF RedfishClientPkg/RedfishETagDxe/RedfishETagDxe.inf
  INF RedfishClientPkg/RedfishConfigLangMapDxe/RedfishConfigLangMapDxe.inf
  INF RedfishClientPkg/HiiToRedfishBootDxe/HiiToRedfishBootDxe.inf
  INF RedfishClientPkg/Features/ComputerSystem/v1_5_0/Dxe/ComputerSystemDxe.inf
  INF RedfishClientPkg/Features/ComputerSystemCollectionDxe/ComputerSystemCollectionDxe.inf
  INF RedfishClientPkg/Features/Bios/v1_0_9/Dxe/BiosDxe.inf
  INF RedfishClientPkg/Features/BiosAttributeRegistry/v1_3_6/BiosAttributeRegistryDxe.inf
  INF RedfishClientPkg/Features/BootOptionCollection/BootOptionCollectionDxe.inf
  INF RedfishClientPkg/Features/BootOption/v1_0_4/Dxe/BootOptionDxe.inf
  INF RedfishClientPkg/Converter/ComputerSystem/v1_5_0/RedfishComputerSystem_V1_5_0_Dxe.inf
  INF RedfishClientPkg/Converter/ComputerSystemCollection/RedfishComputerSystemCollection_Dxe.inf
  INF RedfishClientPkg/Converter/Bios/v1_0_9/RedfishBios_V1_0_9_Dxe.inf
  INF RedfishClientPkg/Converter/AttributeRegistry/v1_3_6/RedfishAttributeRegistry_V1_3_6_Dxe.inf
  INF RedfishClientPkg/Converter/BootOptionCollection/RedfishBootOptionCollection_Dxe.inf
  INF RedfishClientPkg/Converter/BootOption/v1_0_4/RedfishBootOption_V1_0_4_Dxe.inf

# RpiRedfishPkg: the Redfish Host Interface (DSP0270) stack over the BMC's
# USB CDC-NCM gadget. Sed-included into RPi5.dsc right after the NetworkPkg
# include by the edk2-rpi5-firmware recipe, which first renders the
# @RPI5_REDFISH_*@ placeholders from the recipe's knobs.
#
# The insertion point is inside [Components.common], so this file opens the
# sections it needs and MUST end by reopening [Components.common] - anything
# the recipe inserted at the same anchor before this block (usbnet, RpiBmcPkg,
# Rp1GemPkg lines) follows it in the file and must land back in components.
#
# RedfishPkg's own .dsc.inc files are deliberately not used: they are gated
# on $(REDFISH_ENABLE), which has no definition at this insertion point, and
# RedfishLibs.dsc.inc resolves RedfishPlatformCredentialLib to an IPMI-driven
# instance this board (no IPMI transport) could never satisfy. The library
# set below is RedfishLibs.dsc.inc's, minus the HII-config classes only the
# unbuilt RedfishPlatformConfigDxe needs, plus this package's platform
# instances.

[LibraryClasses.common]
  RestExLib|RedfishPkg/Library/DxeRestExLib/DxeRestExLib.inf
  Ucs2Utf8Lib|RedfishPkg/Library/BaseUcs2Utf8Lib/BaseUcs2Utf8Lib.inf
  RedfishCrtLib|RedfishPkg/PrivateLibrary/RedfishCrtLib/RedfishCrtLib.inf
  JsonLib|RedfishPkg/Library/JsonLib/JsonLib.inf
  RedfishLib|RedfishPkg/PrivateLibrary/RedfishLib/RedfishLib.inf
  RedfishDebugLib|RedfishPkg/Library/RedfishDebugLib/RedfishDebugLib.inf
  RedfishHttpLib|RedfishPkg/Library/RedfishHttpLib/RedfishHttpLib.inf
  RedfishPlatformWantedDeviceLib|RedfishPkg/Library/RedfishPlatformWantedDeviceLibNull/RedfishPlatformWantedDeviceLibNull.inf
  RedfishContentCodingLib|RedfishPkg/Library/RedfishContentCodingLibNull/RedfishContentCodingLibNull.inf
  RedfishPlatformCredentialLib|RpiRedfishPkg/Library/RpiRedfishCredentialLib/RpiRedfishCredentialLib.inf
  RedfishPlatformHostInterfaceLib|RpiRedfishPkg/Library/RpiRedfishHostInterfaceLib/RpiRedfishHostInterfaceLib.inf

[PcdsFixedAtBuild.common]
  # The wire contract with the BMC - see RpiRedfishPkg/README.md. The MAC is
  # the gadget's host_addr, rendered from RPI5_REDFISH_MAC in BOTH places
  # below: RedfishDiscoverDxe requires the SMBIOS type 42 record (built from
  # PcdRpiRedfishGadgetMac) to byte-match the NIC, and REST EX binds the
  # interface selected by the device-path MAC node.
  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishGadgetMac|{@RPI5_REDFISH_MAC_BYTES@}
  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishUser|"@RPI5_REDFISH_USER@"
  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishPassword|"@RPI5_REDFISH_PASSWORD@"
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishServicePort|80
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishDisableBootstrapCredentialService|TRUE
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePathMatchMode|DEVICE_PATH_MATCH_MAC_NODE
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePathNum|1
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePath|{DEVICE_PATH("MAC(@RPI5_REDFISH_MAC_PLAIN@,0x1)")}

  # Retry every HTTP method at least once. These default to 0, which makes
  # RedfishHttpDxe give up on the first failure - and the first failure after
  # an idle gap is routine, not exceptional: the BMC's HTTP server closes the
  # TCP connection, RedfishRestExSendReceive finds it gone, resets the
  # session and returns an error, and the retry on the fresh instance is the
  # one that succeeds.
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpGetRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPutRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPatchRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPostRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpDeleteRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpRetryWaitInSecond|1

[PcdsDynamicExDefault.common]
  # NUC-0020 parity, but note what this PCD actually gates in this tree:
  # only HttpBootDxe consults it (no IPv6 HTTP-boot options get created).
  # It does NOT touch RedfishDiscoverDxe -- the IPv6 discovery leg that was
  # on the 2026-08-17 crash stack is disabled by the 0101 patch instead.
  gEfiNetworkPkgTokenSpaceGuid.PcdIPv6HttpSupport|FALSE

[Components.common]
  RedfishPkg/RedfishHostInterfaceDxe/RedfishHostInterfaceDxe.inf
  RedfishPkg/RedfishRestExDxe/RedfishRestExDxe.inf {
    <LibraryClasses>
      SortLib|MdeModulePkg/Library/BaseSortLib/BaseSortLib.inf
  }
  RedfishPkg/RedfishCredentialDxe/RedfishCredentialDxe.inf
  RedfishPkg/RedfishDiscoverDxe/RedfishDiscoverDxe.inf {
    <LibraryClasses>
      SortLib|MdeModulePkg/Library/BaseSortLib/BaseSortLib.inf
  }
  RedfishPkg/RedfishConfigHandler/RedfishConfigHandlerDriver.inf
  RedfishPkg/RedfishHttpDxe/RedfishHttpDxe.inf
  RpiRedfishPkg/RpiRedfishSyncDxe/RpiRedfishSyncDxe.inf

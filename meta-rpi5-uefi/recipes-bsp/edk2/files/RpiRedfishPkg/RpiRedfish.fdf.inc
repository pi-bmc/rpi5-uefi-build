# RpiRedfishPkg drivers, sed-included into RPi5.fdf's FVMAIN by the
# edk2-rpi5-firmware recipe (anchored on the NetworkPkg fdf include).
#
# RedfishPkg's own Redfish.fdf.inc is not used: it is gated on
# $(REDFISH_ENABLE) (undefined here) and pulls in the HII feature-layer
# drivers (RedfishPlatformConfigDxe, RegularExpressionDxe,
# RestJsonStructureDxe) that nothing in this build consumes - FVMAIN space
# is too tight to carry passengers.

  INF RedfishPkg/RedfishHostInterfaceDxe/RedfishHostInterfaceDxe.inf
  INF RedfishPkg/RedfishRestExDxe/RedfishRestExDxe.inf
  INF RedfishPkg/RedfishCredentialDxe/RedfishCredentialDxe.inf
  INF RedfishPkg/RedfishDiscoverDxe/RedfishDiscoverDxe.inf
  INF RedfishPkg/RedfishConfigHandler/RedfishConfigHandlerDriver.inf
  INF RedfishPkg/RedfishHttpDxe/RedfishHttpDxe.inf
  INF RpiRedfishPkg/RpiRedfishSyncDxe/RpiRedfishSyncDxe.inf

# FMP capsule update drivers, sed-included into RPi5.fdf's FVMAIN by the
# edk2-rpi5-firmware recipe. No RuleOverride: FmpDevicePkg's sample platform
# uses an FMP_IMAGE_DESC rule to attach a VERSION section, and RPi5.fdf defines
# no such rule. The version FMP reports comes from the library, not from an FFS
# section, so the default DXE_DRIVER rule is all this needs.

  INF FmpDevicePkg/FmpDxe/FmpDxe.inf
  INF MdeModulePkg/Universal/EsrtFmpDxe/EsrtFmpDxe.inf

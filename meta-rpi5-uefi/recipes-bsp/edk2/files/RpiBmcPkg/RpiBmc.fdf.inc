# RpiBmcPkg drivers, sed-included into RPi5.fdf's FVMAIN by the
# edk2-rpi5-firmware recipe (anchored on the NetworkPkg fdf include).

  INF RpiBmcPkg/Drivers/Rp1DwI2cDxe/Rp1DwI2cDxe.inf
  INF RpiBmcPkg/Drivers/EepromVarStoreDxe/EepromVarStoreDxe.inf
  INF RpiBmcPkg/Drivers/SmbiosEepromMirrorDxe/SmbiosEepromMirrorDxe.inf
  INF RpiBmcPkg/Drivers/BlkInfoMirrorDxe/BlkInfoMirrorDxe.inf
  INF RpiBmcPkg/Drivers/BootloaderConfigDxe/BootloaderConfigDxe.inf
  INF RpiBmcPkg/Drivers/PowerButtonDxe/PowerButtonDxe.inf
  INF RpiBmcPkg/Drivers/ActiveCoolerDxe/ActiveCoolerDxe.inf

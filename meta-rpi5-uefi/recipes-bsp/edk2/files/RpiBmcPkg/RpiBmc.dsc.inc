# RpiBmcPkg components, sed-included into RPi5.dsc's [Components.common] by
# the edk2-rpi5-firmware recipe (anchored on the NetworkPkg dsc include).
# Library instances are scoped per-module so nothing global in RPi5.dsc
# changes.

  RpiBmcPkg/Drivers/Rp1DwI2cDxe/Rp1DwI2cDxe.inf {
    <LibraryClasses>
      Rp1GpioLib|RpiBmcPkg/Library/Rp1GpioLib/Rp1GpioLib.inf
  }
  RpiBmcPkg/Drivers/EepromVarStoreDxe/EepromVarStoreDxe.inf {
    <LibraryClasses>
      BmcEepromLib|RpiBmcPkg/Library/BmcEepromLib/BmcEepromLib.inf
  }
  RpiBmcPkg/Drivers/SmbiosEepromMirrorDxe/SmbiosEepromMirrorDxe.inf {
    <LibraryClasses>
      BmcEepromLib|RpiBmcPkg/Library/BmcEepromLib/BmcEepromLib.inf
  }
  RpiBmcPkg/Drivers/BlkInfoMirrorDxe/BlkInfoMirrorDxe.inf {
    <LibraryClasses>
      BmcEepromLib|RpiBmcPkg/Library/BmcEepromLib/BmcEepromLib.inf
  }
  RpiBmcPkg/Drivers/BootloaderConfigDxe/BootloaderConfigDxe.inf

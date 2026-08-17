# RpiBmcPkg components, sed-included into RPi5.dsc's [Components.common] by
# the edk2-rpi5-firmware recipe (anchored on the NetworkPkg dsc include).
# Library instances are scoped per-module so nothing global in RPi5.dsc
# changes.
#
# The former I2C EEPROM sync drivers (Rp1DwI2cDxe, EepromVarStoreDxe,
# SmbiosEepromMirrorDxe, BlkInfoMirrorDxe) are gone: BMC data exchange now
# rides the Redfish Host Interface over the BMC's USB CDC-NCM gadget - see
# RpiRedfishPkg.

  RpiBmcPkg/Drivers/BootloaderConfigDxe/BootloaderConfigDxe.inf
  RpiBmcPkg/Drivers/PowerButtonDxe/PowerButtonDxe.inf
  RpiBmcPkg/Drivers/ActiveCoolerDxe/ActiveCoolerDxe.inf {
    <LibraryClasses>
      Rp1GpioLib|RpiBmcPkg/Library/Rp1GpioLib/Rp1GpioLib.inf
  }

# RpiBmcPkg components, sed-included into RPi5.dsc's [Components.common] by
# the edk2-rpi5-firmware recipe (anchored on the NetworkPkg dsc include).
# Driver library instances are scoped per-module; the one global change is
# the CustomizedDisplayLib remap at the end (the dark Setup theme), which
# relies on a later [LibraryClasses] definition overriding RPi5.dsc's
# stock mapping. Section discipline: ends by reopening [Components.common]
# because the insertion point sits inside that section.
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
  RpiBmcPkg/Drivers/FanConfigDxe/FanConfigDxe.inf

[LibraryClasses.common]
  # Dark Setup theme: DisplayEngineDxe paints every form through the
  # CustomizedDisplayLib class; PlatformThemeLib is the stock instance
  # with a black-background Colors.h.
  CustomizedDisplayLib|RpiBmcPkg/Library/PlatformThemeLib/PlatformThemeLib.inf

[PcdsFixedAtBuild.common]
  # The four field colors the display engine reads straight from PCDs
  # (everything else lives in PlatformThemeLib/Colors.h). Foreground
  # values are EFI_* (0x00-0x0F), the highlight background is an
  # EFI_BACKGROUND_* (high nibble). Selection = bright white on black;
  # ordinary fields stay lightgray so the highlight reads.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldTextColor|0x07                 # lightgray fields
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserSubtitleTextColor|0x0F              # white subtitles
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldTextHighlightColor|0x0F        # white text ...
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldBackgroundHighlightColor|0x00  # ... on black highlight

[PcdsDynamicHii.common]
  # Make Setup fill the screen with larger glyphs. The 8x19 EFI font does
  # not scale, so "bigger" means a smaller framebuffer the VPU scales up
  # to the display: enable DisplayDxe's scaled 800x600 GOP mode (BIT0)
  # alongside native (BIT5) - the platform default 0x20 is native-only,
  # which is why UiApp's 640x480 setup switch silently failed - and size
  # the setup text mode to cover it (100x31 at 8x19 = 800x589). UiApp
  # restores the native boot console on exit; PcdConOutColumn/Row (80x25)
  # are untouched.
  gRaspberryPiTokenSpaceGuid.PcdDisplayEnableScaledVModes|L"DisplayEnableScaledVModes"|gConfigDxeFormSetGuid|0x0|0x21
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|L"Columns"|gRaspberryPiTokenSpaceGuid|0x0|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|L"Rows"|gRaspberryPiTokenSpaceGuid|0x0|31

[PcdsDynamicDefault.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|800
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|600

[Components.common]

# Dp ("dump performance") shell command, sed-included into RPi5.dsc's
# [Components.common] by the edk2-rpi5-firmware recipe (anchored on the
# NetworkPkg dsc include).
#
# Gated on the PROFILING_ENABLED build macro rather than on whether the
# recipe inserted these lines: the recipe ALWAYS inserts them and always
# passes -D PROFILING_ENABLED=TRUE|FALSE. That matters because do_compile
# runs against a persisted WORKDIR checkout -- a conditional insertion
# could add the component but never take it away again, so turning the
# knob back off would silently keep building Dp.
#
# The platform is already instrumented, so this is the only missing piece:
# RPi5.dsc sets PcdPerformanceLibraryPropertyMask|1 (measurement enabled),
# maps DxeCorePerformanceLib on DXE_CORE -- which is what records the
# measurements and installs them as the gEdkiiFpdtExtendedFirmwarePerformance
# and gPerformanceProtocol configuration tables Dp reads -- and
# DxePerformanceLib on every other module type. Nothing else is needed:
# FirmwarePerformanceDataTableDxe only matters for the ACPI FPDT that
# OS-side tools consume, not for `dp` in the shell.
#
# (Note for anyone following older guidance: PerformancePkg was removed
# from EDK2 in 2018. Dp now ships in ShellPkg, as a dynamic shell command.)

!if $(PROFILING_ENABLED) == TRUE
  #
  # Registers itself with the Shell as the built-in `dp` command, exactly
  # like the platform's own TftpDynamicCommand entry. PcdShellLibAutoInitialize
  # MUST be FALSE for a dynamic command: the driver is a DXE_DRIVER that
  # loads long before any shell exists, and ShellLib's automatic constructor
  # would try to bind to a shell protocol that is not there yet.
  #
  # ShellLib/FileHandleLib are scoped to this module because RPi5.dsc only
  # puts them in the DXE_DRIVER scope when INCLUDE_TFTP_COMMAND is TRUE
  # (it is FALSE here, and enabling it would also pull in the tftp command).
  # Everything else Dp needs -- SortLib, DxeServicesLib,
  # PeCoffGetEntryPointLib, HiiLib -- is already mapped globally.
  #
  ShellPkg/DynamicCommand/DpDynamicCommand/DpDynamicCommand.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
    <LibraryClasses>
      ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
      FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  }
!endif

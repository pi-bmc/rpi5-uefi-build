# @file
#
#  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
#  Copyright (c) 2011 - 2020, ARM Limited. All rights reserved.
#  Copyright (c) 2017 - 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
#  Copyright (c) 2015 - 2021, Intel Corporation. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = RPi5
  PLATFORM_GUID                  = 4e8faa1b-1682-4dfc-8204-2e316a14b7ec
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x0001001A
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/RaspberryPi/$(PLATFORM_NAME)/$(PLATFORM_NAME).fdf

  #
  # Defines for default states.  These can be changed on the command line.
  # -D FLAG=VALUE
  #
  DEFINE SECURE_BOOT_ENABLE      = FALSE
  DEFINE INCLUDE_TFTP_COMMAND    = FALSE
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x8000004F

  #
  # OP-TEE (BL32) integration: embeds tee-raw.bin in the FD (see RPi5.fdf),
  # reserves the secure DRAM carve-out, and runs the EDK2->OP-TEE late
  # initialization handshake for the BMC sensor service
  # (RpiOpteeSensorDxe). Requires the TF-A build with SPD=opteed; the
  # firmware recipe keeps the two in step via its RPI5_OPTEE knob.
  #
  DEFINE RPI5_OPTEE              = TRUE

  # Store UEFI variables in OP-TEE-mediated RPMB via the StMM secure partition
  # instead of the FD-backed VarBlockServiceDxe. Requires RPI5_OPTEE=TRUE and
  # OP-TEE built with the StMM FV embedded (RPI5_OPTEE_STMM=1 in the optee-os
  # recipe). Off by default: the RPMB frame backend is not wired yet, so
  # turning this on without it leaves the variable store non-functional.
  DEFINE RPI5_OPTEE_VARS         = FALSE

!ifndef TFA_BUILD_ARTIFACTS
  #
  # Default TF-A binary checked into edk2-non-osi.
  #
  DEFINE TFA_BUILD_BL31 = Platform/RaspberryPi/$(PLATFORM_NAME)/TrustedFirmware/bl31.bin
  DEFINE TFA_BUILD_BL32 = Platform/RaspberryPi/$(PLATFORM_NAME)/TrustedFirmware/tee-raw.bin
!else
  #
  # Usually we use the checked-in binaries, but for developers working
  # on the firmware, being able to use a local TF-A build without extra copy
  # operations ends up being very helpful.
  #
  DEFINE TFA_BUILD_BL31 = $(TFA_BUILD_ARTIFACTS)/bl31.bin
  DEFINE TFA_BUILD_BL32 = $(TFA_BUILD_ARTIFACTS)/tee-raw.bin
!endif

  #
  # DEBUG_ASSERT_ENABLED       0x01
  # DEBUG_PRINT_ENABLED        0x02
  # DEBUG_CODE_ENABLED         0x04
  # CLEAR_MEMORY_ENABLED       0x08
  # ASSERT_BREAKPOINT_ENABLED  0x10
  # ASSERT_DEADLOOP_ENABLED    0x20
  #
!if $(TARGET) == RELEASE
  DEFINE DEBUG_PROPERTY_MASK             = 0x21
!else
  DEFINE DEBUG_PROPERTY_MASK             = 0x2f
!endif

################################################################################
#
# Library Class section - list of all Library Classes needed by this Platform.
#
################################################################################

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses.common]
!if $(TARGET) == RELEASE
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
!else
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
!endif
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf

  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf

  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

  #
  # Ramdisk Requirements
  #
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf

  # Allow dynamic PCDs
  #
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf

  # use the accelerated BaseMemoryLibOptDxe by default, overrides for SEC/PEI below
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf

  # ARM Architectural Libraries
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  #
  # edk2-stable202608 retired ArmPkg's own exception path: ArmExceptionLib and
  # DefaultExceptionHandlerLib are gone, and AArch64 is served by the generic
  # CpuExceptionHandlerLib class out of UefiCpuPkg -- which is what upstream
  # ArmPkg.dsc now maps, and which CpuDxe.inf now lists in [LibraryClasses].
  # The DefaultExceptionHandlerLib mapping goes with them; nothing declares
  # that class any more. (ArmMmuLib below made the same ArmPkg -> UefiCpuPkg
  # move in an earlier release.)
  #
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  OpteeLib|ArmPkg/Library/OpteeLib/OpteeLib.inf
  ArmTransferListLib|ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf

  # Dual serial port library
  PL011UartClockLib|ArmPlatformPkg/Library/PL011UartClockLib/PL011UartClockLib.inf
  PL011UartLib|ArmPlatformPkg/Library/PL011UartLib/PL011UartLib.inf
  SerialPortLib|ArmPlatformPkg/Library/PL011SerialPortLib/PL011SerialPortLib.inf

  # Cryptographic libraries
  RngLib|MdePkg/Library/DxeRngLib/DxeRngLib.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLib.inf
  TlsLib|CryptoPkg/Library/TlsLib/TlsLib.inf

  #
  # Uncomment (and comment out the next line) For RealView Debugger. The Standard IO window
  # in the debugger will show load and unload commands for symbols. You can cut and paste this
  # into the command window to load symbols. We should be able to use a script to do this, but
  # the version of RVD I have does not support scripts accessing system memory.
  #
  #PeCoffExtraActionLib|ArmPkg/Library/RvdPeCoffExtraActionLib/RvdPeCoffExtraActionLib.inf
  PeCoffExtraActionLib|ArmPkg/Library/DebugPeCoffExtraActionLib/DebugPeCoffExtraActionLib.inf
  #PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf

  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  DebugAgentTimerLib|EmbeddedPkg/Library/DebugAgentTimerLibNull/DebugAgentTimerLibNull.inf

  # Flattened Device Tree (FDT) access library
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

  # USB Libraries
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf

  #
  # Secure Boot dependencies
  #
!if $(SECURE_BOOT_ENABLE) == TRUE
  TpmMeasurementLib|SecurityPkg/Library/DxeTpmMeasurementLib/DxeTpmMeasurementLib.inf
  AuthVariableLib|SecurityPkg/Library/AuthVariableLib/AuthVariableLib.inf
  SecureBootVariableLib|SecurityPkg/Library/SecureBootVariableLib/SecureBootVariableLib.inf
  SecureBootVariableProvisionLib|SecurityPkg/Library/SecureBootVariableProvisionLib/SecureBootVariableProvisionLib.inf
  PlatformPKProtectionLib|SecurityPkg/Library/PlatformPKProtectionLibVarPolicy/PlatformPKProtectionLibVarPolicy.inf

  # re-use the UserPhysicalPresent() dummy implementation from the ovmf tree
  PlatformSecureLib|OvmfPkg/Library/PlatformSecureLib/PlatformSecureLib.inf
!else
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
!endif
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf

  FdtPlatformLib|Platform/RaspberryPi/Library/FdtPlatformLib/FdtPlatformLib.inf
  BoardInfoLib|Platform/RaspberryPi/Library/BoardInfoLib/BoardInfoLib.inf
  BoardRevisionHelperLib|Platform/RaspberryPi/Library/BoardRevisionHelperLib/BoardRevisionHelperLib.inf

  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf

  Bcm2712GpioLib|Silicon/Broadcom/Bcm27xx/Library/Bcm2712GpioLib/Bcm2712GpioLib.inf

  PciHostBridgeLib|Silicon/Broadcom/Bcm27xx/Library/Bcm2712PciHostBridgeLib/Bcm2712PciHostBridgeLib.inf
  PciSegmentLib|Silicon/Broadcom/Bcm27xx/Library/Bcm2712PciSegmentLib/PciSegmentLib.inf

[LibraryClasses.common.SEC]
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryInitPeiLib|Platform/RaspberryPi/Library/MemoryInitPeiLib/MemoryInitPeiLib.inf
  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  PerformanceLib|MdeModulePkg/Library/DxeCorePerformanceLib/DxeCorePerformanceLib.inf

[LibraryClasses.common.DXE_DRIVER]
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
!if $(INCLUDE_TFTP_COMMAND) == TRUE
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
!endif

[LibraryClasses.common.UEFI_APPLICATION]
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf

[LibraryClasses.common.UEFI_DRIVER]
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  # Runtime debug messages may crash an OS unless serial output to MMIO mapped UARTs is inhibited
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  #
  # CapsuleRuntimeDxe's half of the capsule path: UpdateCapsule() validates and
  # stages what it is handed. The runtime variant, because this one is linked
  # into a DXE_RUNTIME_DRIVER and must not drag in boot-services-only code.
  #
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibFmp/DxeRuntimeCapsuleLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

!if $(SECURE_BOOT_ENABLE) == TRUE
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/RuntimeCryptLib.inf
!endif

###################################################################################################
# BuildOptions Section - Define the module specific tool chain flags that should be used as
#                        the default flags for a module. These flags are appended to any
#                        standard flags that are defined by the build process.
###################################################################################################

[BuildOptions]
  GCC:*_*_*_CC_FLAGS          = -DRPI_MODEL=5
  GCC:*_*_*_PP_FLAGS          = -DRPI_MODEL=5
  GCC:*_*_*_ASLPP_FLAGS       = -DRPI_MODEL=5
  GCC:*_*_*_ASLCC_FLAGS       = -DRPI_MODEL=5
  GCC:*_*_*_VFRPP_FLAGS       = -DRPI_MODEL=5
  GCC:RELEASE_*_*_CC_FLAGS    = -DMDEPKG_NDEBUG -DNDEBUG

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  GCC:*_*_AARCH64_DLINK_FLAGS = -z common-page-size=0x10000

################################################################################
#
# Pcd Section - list of all EDK II PCD Entries defined by this Platform
#
################################################################################

[PcdsFeatureFlag.common]
  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdTurnOffUsbLegacySupport|TRUE

  ## If TRUE, Graphics Output Protocol will be installed on virtual handle created by ConsplitterDxe.
  #  It could be set FALSE to save size.
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport|TRUE

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdMaximumUnicodeStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumAsciiStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdSpinLockTimeout|10000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  gEfiMdePkgTokenSpaceGuid.PcdPerformanceLibraryPropertyMask|1
  gEfiMdePkgTokenSpaceGuid.PcdPostCodePropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|320
  #
  # Follows right after the FD image. (bump the size again)
  #
  gRaspberryPiTokenSpaceGuid.PcdFdtBaseAddress|0x003e0000

  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|$(DEBUG_PROPERTY_MASK)

  #  DEBUG_INIT      0x00000001  // Initialization
  #  DEBUG_WARN      0x00000002  // Warnings
  #  DEBUG_LOAD      0x00000004  // Load events
  #  DEBUG_FS        0x00000008  // EFI File system
  #  DEBUG_POOL      0x00000010  // Alloc & Free (pool)
  #  DEBUG_PAGE      0x00000020  // Alloc & Free (page)
  #  DEBUG_INFO      0x00000040  // Informational debug messages
  #  DEBUG_DISPATCH  0x00000080  // PEI/DXE/SMM Dispatchers
  #  DEBUG_VARIABLE  0x00000100  // Variable
  #  DEBUG_BM        0x00000400  // Boot Manager
  #  DEBUG_BLKIO     0x00001000  // BlkIo Driver
  #  DEBUG_NET       0x00004000  // SNP Driver
  #  DEBUG_UNDI      0x00010000  // UNDI Driver
  #  DEBUG_LOADFILE  0x00020000  // LoadFile
  #  DEBUG_EVENT     0x00080000  // Event messages
  #  DEBUG_GCD       0x00100000  // Global Coherency Database changes
  #  DEBUG_CACHE     0x00200000  // Memory range cachability changes
  #  DEBUG_VERBOSE   0x00400000  // Detailed debug messages that may
  #                              // significantly impact boot performance
  #  DEBUG_ERROR     0x80000000  // Error
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|$(DEBUG_PRINT_ERROR_LEVEL)

  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x07

  #
  # Optional feature to help prevent EFI memory map fragments
  # Turned on and off via: PcdPrePiProduceMemoryTypeInformationHob
  # Values are in EFI Pages (4K). DXE Core will make sure that
  # at least this much of each type of memory can be allocated
  # from a single memory range. This way you only end up with
  # maximum of two fragments for each type in the memory map
  # (the memory used, and the free memory that was prereserved
  # but not used).
  #
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIReclaimMemory|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIMemoryNVS|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiReservedMemoryType|0
!if $(SECURE_BOOT_ENABLE) == TRUE
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|600
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|400
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1500
!else
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|300
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|150
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1000
!endif
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesData|12000
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderCode|20
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderData|0

  gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0xc0000000
  gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xffffffff

  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"EDK2-DEV"

!if $(SECURE_BOOT_ENABLE) == TRUE
  # override the default values from SecurityPkg to ensure images from all sources are verified in secure boot
  gEfiSecurityPkgTokenSpaceGuid.PcdOptionRomImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdFixedMediaImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdRemovableMediaImageVerificationPolicy|0x04
!endif

  gEfiNetworkPkgTokenSpaceGuid.PcdAllowHttpConnections|TRUE

  # Default platform supported RFC 4646 languages: (American) English
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultPlatformLangCodes|"en-US"

[LibraryClasses.common]
  # ArmLib moved ArmPkg -> MdePkg in edk2-stable202608.
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  # PartitionDxe grew a GptLib dependency in edk2-stable202608; the GPT parsing
  # it used to do inline now lives in this library.
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmPlatformLib|Platform/RaspberryPi/RPi5/Library/PlatformLib/PlatformLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  #
  # The half that actually applies a capsule. ProcessCapsules() is already
  # called from PlatformBm both before and after EndOfDxe -- with the Null
  # instance it did nothing at all, so an UpdateCapsule() call succeeded and
  # then silently dropped the image on the next boot.
  #
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibFmp/DxeCapsuleLib.inf
  #
  # Required by DxeCapsuleLibFmp. Text rather than Graphics: the update runs
  # with the serial console up and GOP not guaranteed, and a progress bar is
  # not worth a dependency on the framebuffer being alive mid-update.
  #
  DisplayUpdateProgressLib|MdeModulePkg/Library/DisplayUpdateProgressLibText/DisplayUpdateProgressLibText.inf
  #
  # Required by FmpDxe, which authenticates every capsule payload against the
  # certificates in PcdFmpDevicePkcs7CertBufferXdr before the device library
  # ever sees it. There is no Null instance of this class by design.
  #
  FmpAuthenticationLib|SecurityPkg/Library/FmpAuthenticationLibPkcs7/FmpAuthenticationLibPkcs7.inf
  #
  # Also required by DxeCapsuleLibFmp, and only declared above under
  # INCLUDE_TFTP_COMMAND -- which is FALSE here, so BdsDxe could not resolve it
  # once PlatformBootManagerLib started pulling the real CapsuleLib in.
  #
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  PlatformBootManagerLib|Platform/RaspberryPi/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  AcpiLib|EmbeddedPkg/Library/AcpiLib/AcpiLib.inf

  #
  # RP1 GPIO block: Rp1GemDxe's PHY reset line and RpiOpteeSensorDxe's I2C1
  # pinmux. (The fan PWM is no longer a consumer: ActiveCoolerScmiDxe goes
  # through OP-TEE's SCMI server, which owns the PWM and the GPIO45 mux.)
  #
  Rp1GpioLib|Silicon/RaspberryPi/RpiSiliconPkg/Library/Rp1GpioLib/Rp1GpioLib.inf

  #
  # SCMI agent wire to OP-TEE's server, shared by ActiveCoolerScmiDxe and
  # RpiScmiConfigDxe. See RpiScmiLib.h for the cross-component contract.
  #
  RpiScmiLib|Platform/RaspberryPi/RPi5/Library/RpiScmiLib/RpiScmiLib.inf

  #
  # Setup theme for the HII browser -- colours and layout for this board
  # family's pages. Overrides MdeModulePkg's stock CustomizedDisplayLib.
  #
  CustomizedDisplayLib|Platform/RaspberryPi/Library/PlatformThemeLib/PlatformThemeLib.inf

  #
  # Redfish host interface (DSP0270) over the BMC's USB CDC-NCM gadget.
  # RedfishPkg's own .dsc.inc files are deliberately not used: they are gated
  # on $(REDFISH_ENABLE), and RedfishLibs.dsc.inc resolves
  # RedfishPlatformCredentialLib to an IPMI-driven instance this board (which
  # has no IPMI transport) could never satisfy. The two platform libraries at
  # the end of this block are this board's own.
  #
  RestExLib|RedfishPkg/Library/DxeRestExLib/DxeRestExLib.inf
  Ucs2Utf8Lib|RedfishPkg/Library/BaseUcs2Utf8Lib/BaseUcs2Utf8Lib.inf
  RedfishCrtLib|RedfishPkg/PrivateLibrary/RedfishCrtLib/RedfishCrtLib.inf
  JsonLib|RedfishPkg/Library/JsonLib/JsonLib.inf
  RedfishLib|RedfishPkg/PrivateLibrary/RedfishLib/RedfishLib.inf
  RedfishDebugLib|RedfishPkg/Library/RedfishDebugLib/RedfishDebugLib.inf
  RedfishHttpLib|RedfishPkg/Library/RedfishHttpLib/RedfishHttpLib.inf
  RedfishPlatformWantedDeviceLib|RedfishPkg/Library/RedfishPlatformWantedDeviceLibNull/RedfishPlatformWantedDeviceLibNull.inf
  RedfishContentCodingLib|RedfishPkg/Library/RedfishContentCodingLibNull/RedfishContentCodingLibNull.inf
  RedfishPlatformCredentialLib|Platform/RaspberryPi/RPi5/Library/RpiRedfishCredentialLib/RpiRedfishCredentialLib.inf
  RedfishPlatformHostInterfaceLib|Platform/RaspberryPi/RPi5/Library/RpiRedfishHostInterfaceLib/RpiRedfishHostInterfaceLib.inf

  #
  # edk2-redfish-client (RedfishClientPkg): the feature layer above the host
  # interface -- HII-to-Redfish feature drivers and their schema converters.
  #
  RedfishPlatformConfigLib|RedfishPkg/Library/RedfishPlatformConfigLib/RedfishPlatformConfigLib.inf
  HiiUtilityLib|RedfishPkg/Library/HiiUtilityLib/HiiUtilityLib.inf
  RedfishFeatureUtilityLib|RedfishClientPkg/Library/RedfishFeatureUtilityLib/RedfishFeatureUtilityLib.inf
  ConverterCommonLib|RedfishClientPkg/ConverterLib/edk2library/ConverterCommonLib/ConverterCommonLib.inf
  RedfishResourceIdentifyLib|RedfishClientPkg/Library/RedfishResourceIdentifyLibNull/RedfishResourceIdentifyLibNull.inf
  EdkIIRedfishResourceConfigLib|RedfishClientPkg/Library/EdkIIRedfishResourceConfigLib/EdkIIRedfishResourceConfigLib.inf
  RedfishEventLib|RedfishClientPkg/Library/RedfishEventLib/RedfishEventLib.inf
  RedfishVersionLib|RedfishClientPkg/Library/RedfishVersionLib/RedfishVersionLib.inf
  RedfishAddendumLib|RedfishClientPkg/Library/RedfishAddendumLib/RedfishAddendumLib.inf
  ComputerSystemV1_13_0Lib|RedfishClientPkg/ConverterLib/edk2library/ComputerSystem/v1_13_0/Lib.inf
  ComputerSystemCollectionLib|RedfishClientPkg/ConverterLib/edk2library/ComputerSystemCollection/Lib.inf
  BiosV1_1_0Lib|RedfishClientPkg/ConverterLib/edk2library/Bios/v1_1_0/Lib.inf
  AttributeRegistryV1_3_6Lib|RedfishClientPkg/ConverterLib/edk2library/AttributeRegistry/v1_3_6/Lib.inf
  BootOptionCollectionLib|RedfishClientPkg/ConverterLib/edk2library/BootOptionCollection/Lib.inf
  BootOptionV1_0_4Lib|RedfishClientPkg/ConverterLib/edk2library/BootOption/v1_0_4/Lib.inf
  MemoryV1_7_1Lib|RedfishClientPkg/ConverterLib/edk2library/Memory/v1_7_1/Lib.inf
  MemoryCollectionLib|RedfishClientPkg/ConverterLib/edk2library/MemoryCollection/Lib.inf
  SecureBootV1_1_0Lib|RedfishClientPkg/ConverterLib/edk2library/SecureBoot/v1_1_0/Lib.inf
!if $(SECURE_BOOT_ENABLE) == FALSE
  #
  # RedfishClientPkg's SecureBootDxe feature driver links these either way; with
  # SECURE_BOOT_ENABLE the platform maps them itself further up.
  #
  SecureBootVariableLib|SecurityPkg/Library/SecureBootVariableLib/SecureBootVariableLib.inf
  PlatformPKProtectionLib|SecurityPkg/Library/PlatformPKProtectionLibVarPolicy/PlatformPKProtectionLibVarPolicy.inf
!endif

[LibraryClasses.common.UEFI_DRIVER]
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

################################################################################
#
# Pcd Section - list of all EDK II PCD Entries defined by this Platform
#
################################################################################

[PcdsFeatureFlag.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE
!if $(RPI5_OPTEE_VARS) == TRUE
  # The x86-SMM-era runtime variable cache has VariableStandaloneMm writing
  # into NS buffers by raw pointer -- pointers the StMM SP has no mapping
  # for, so the first Get/SetVariable after cache init would data-abort in
  # S-EL0. All variable traffic goes through the MM communicate path instead.
  gEfiMdeModulePkgTokenSpaceGuid.PcdEnableVariableRuntimeCache|FALSE
!endif

[PcdsFixedAtBuild.common]
  gArmPlatformTokenSpaceGuid.PcdCoreCount|4

  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x4000
  # MUST stay equal to the values in PlatformStandaloneMmRpi5.dsc when
  # RPI5_OPTEE_VARS is on: the NS proxy sizes its communicate payloads from
  # these copies, StMM enforces its own.
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800

  # Size of the region used by UEFI in permanent memory (Reserved 64MB)
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x04000000
  #
  # 0x00000000 - 0x001F0000  FD (PcdFdBaseAddress, PcdFdSize)
  # 0x003E0000 - 0x00400000 DTB (PcdFdtBaseAddress, PcdFdtSize)
  # 0x00400000 - ...        RAM (PcdSystemMemoryBase, PcdSystemMemorySize)
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00400000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x3fc00000

  gRaspberryPiTokenSpaceGuid.PcdFdtSize|0x20000

!if $(RPI5_OPTEE) == TRUE
  #
  # OP-TEE secure DRAM carve-out: TZDRAM [0x1D000000, 0x1F000000) + static
  # SHM [0x1F000000, 0x1F400000), matching plat-rpi5 conf.mk and the TF-A
  # BL32 defines. RaspberryPiMem.c punches the hole out of "System RAM
  # < 1GB" (TZDRAM unmapped like the GPU carve-out, SHM mapped WB +
  # reserved for OpteeLib); FdtDxe mirrors it as /reserved-memory/optee.
  # The dec defaults (all zero) describe "no OP-TEE".
  #
  gRpiOpteeTokenSpaceGuid.PcdOpteeTzdramBase|0x1D000000
  gRpiOpteeTokenSpaceGuid.PcdOpteeTzdramSize|0x02000000
  gRpiOpteeTokenSpaceGuid.PcdOpteeShmSize|0x00400000
!endif

  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|40

  # UARTs
  gArmPlatformTokenSpaceGuid.PL011UartClkInHz|44000000
  gArmPlatformTokenSpaceGuid.PL011UartInterrupt|152
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x107d001000

  #
  # ARM General Interrupt Controller
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x107fff9000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x107fffa000
  gRaspberryPiTokenSpaceGuid.PcdGicInterruptInterfaceHBase|0x107fffc000
  gRaspberryPiTokenSpaceGuid.PcdGicInterruptInterfaceVBase|0x107fffe000
  gRaspberryPiTokenSpaceGuid.PcdGicGsivId|0x19
  gRaspberryPiTokenSpaceGuid.PcdGicPmuIrq0|0x30
  gRaspberryPiTokenSpaceGuid.PcdGicPmuIrq1|0x31
  gRaspberryPiTokenSpaceGuid.PcdGicPmuIrq2|0x32
  gRaspberryPiTokenSpaceGuid.PcdGicPmuIrq3|0x33

  #
  # Mailbox
  #
  gRaspberryPiTokenSpaceGuid.PcdFwMailboxBaseAddress|0x107c013880

  #
  # DWC2 OTG (the USB-C data port): /axi/usb@480000 in the firmware DTB,
  # identity-mapped, so the CPU address is the reg value itself.
  #
  gRaspberryPiTokenSpaceGuid.PcdDwUsbBaseAddress|0x1000480000

  #
  # RNG
  #
  gBcm283xTokenSpaceGuid.PcdBcm2838RngBaseAddress|0x107d208000

  ## Default Terminal Type
  ## 0-PCANSI, 1-VT100, 2-VT00+, 3-UTF8, 4-TTYTERM
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x21, 0xaa, 0x2c, 0x46, 0x14, 0x76, 0x03, 0x45, 0x83, 0x6e, 0x8a, 0xb6, 0xf4, 0x66, 0x23, 0x31 }

  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"EDK2"
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetNxForStack|TRUE

  #
  # Setup browser colours, paired with PlatformThemeLib above.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldTextColor|0x07                 # lightgray fields
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserSubtitleTextColor|0x0F              # white subtitles
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldTextHighlightColor|0x0F        # white text ...
  gEfiMdeModulePkgTokenSpaceGuid.PcdBrowserFieldBackgroundHighlightColor|0x00  # ... on black highlight

  #
  # Redfish host interface. The gadget MAC and the HTTP Basic credentials
  # default to the wire contract's documented values in RPi5.dec; the
  # firmware recipe appends overrides for them, and for the matching
  # RestEx device path, from its own knobs.
  #
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishServicePort|80
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishDisableBootstrapCredentialService|TRUE
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePathMatchMode|DEVICE_PATH_MATCH_MAC_NODE
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePathNum|1
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpGetRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPutRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPatchRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpPostRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpDeleteRetry|3
  gEfiRedfishPkgTokenSpaceGuid.PcdHttpRetryWaitInSecond|1

  # RedfishClientPkg: publish HII questions as Redfish attributes.
  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishPlatformConfigFeatureProperty|0x03

[PcdsDynamicHii.common.DEFAULT]
  #
  # Display-related.
  #

  #
  # Just enable native resolution by default.
  #
  gRaspberryPiTokenSpaceGuid.PcdDisplayEnableScaledVModes|L"DisplayEnableScaledVModes"|gConfigDxeFormSetGuid|0x0|0x20
  gRaspberryPiTokenSpaceGuid.PcdDisplayEnableSShot|L"DisplayEnableSShot"|gConfigDxeFormSetGuid|0x0|1

  #
  # Reset-related.
  #
  gRaspberryPiTokenSpaceGuid.PcdPlatformResetDelay|L"ResetDelay"|gRaspberryPiTokenSpaceGuid|0x0|0

  #
  # Device Tree and ACPI selection.
  #
  # 0 - SYSTEM_TABLE_MODE_ACPI
  # 1 - SYSTEM_TABLE_MODE_BOTH
  # 2 - SYSTEM_TABLE_MODE_DT (default)
  #
  # Upstream defaults to ACPI. This platform defaults to Device Tree because
  # the onboard NIC does not come up under ACPI: the RP1 GEM is a Cadence
  # macb, and macb has no acpi_match_table in any shipping kernel, so even
  # with the DSDT device from 0012 the driver never binds (and would fail at
  # devm_clk_get("pclk") if it did -- clkdev has no ACPI path). Under Device
  # Tree the firmware DTB describes ethernet@100000 the way the driver
  # expects and the NIC works. A headless node that cannot reach the network
  # is worth more than ACPI is.
  #
  # Change this back once macb grows an ACPI binding, and keep the F9
  # "Restore Defaults" value in RpiPlatformDxeHii.vfr in step with it.
  #
  gRaspberryPiTokenSpaceGuid.PcdSystemTableMode|L"SystemTableMode"|gRpiPlatformFormSetGuid|0x0|2

  #
  # Common UEFI ones.
  #
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|L"Timeout"|gEfiGlobalVariableGuid|0x0|5
  #
  # This is silly, but by pointing SetupConXXX and ConXXX PCDs to
  # the same variables, I can use the graphical configuration to
  # change the mode used by ConSplitter.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|L"Columns"|gRaspberryPiTokenSpaceGuid|0x0|80
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|L"Columns"|gRaspberryPiTokenSpaceGuid|0x0|80
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|L"Rows"|gRaspberryPiTokenSpaceGuid|0x0|25
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|L"Rows"|gRaspberryPiTokenSpaceGuid|0x0|25
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootDiscoveryPolicy|L"BootDiscoveryPolicy"|gBootDiscoveryPolicyMgrFormsetGuid|0

  gRaspberryPiTokenSpaceGuid.PcdDisplayEnableScaledVModes|L"DisplayEnableScaledVModes"|gConfigDxeFormSetGuid|0x0|0x21
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|L"Columns"|gRaspberryPiTokenSpaceGuid|0x0|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|L"Rows"|gRaspberryPiTokenSpaceGuid|0x0|31

[PcdsDynamicDefault.common]
  #
  # Set video resolution for boot options and for text setup.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|640
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|480
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################

  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|800
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|600

[PcdsDynamicExDefault.common]
  # Suppresses RedfishDiscoverDxe's IPv6 discovery leg, which it gates on this
  # PCD (IsRedfishRequiredProtocolIndexActive). The Redfish host interface is a
  # point-to-point IPv4-static link -- the SMBIOS type 42 record carries static
  # v4 addresses -- so a second discovery pass over Tcp6 on the same NIC only
  # duplicates instances and REST EX children nothing consumes. That pass was
  # also in flight during a use-after-free crash on a "BiosSetup" boot override
  # (2026-08-17). This layer carried a patch to skip it until edk2-stable202608,
  # where upstream implemented the same skip behind this PCD. It equally keeps
  # HttpBootDxe from advertising IPv6 support the board will never use.
  gEfiNetworkPkgTokenSpaceGuid.PcdIPv6HttpSupport|FALSE
[Components.common]
  #
  # PEI Phase modules
  #
  ArmPlatformPkg/PeilessSec/PeilessSec.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
      PeilessSecMeasureLib|SecurityPkg/Library/PeilessSecMeasureLib/PeilessSecMeasureLibNull.inf
  }
#
  # DXE
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
  }
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }

  #
  # Architectural Protocols
  #
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
!if $(RPI5_OPTEE_VARS) == TRUE
  #
  # UEFI variables in StMM. The store IS this FD's NV window (the same
  # 0x3b0000..0x3d0000 regions VarBlockServiceDxe serves in the !else
  # branch): OP-TEE maps it into the StMM secure partition
  # (CFG_STMM_VARSTORE_*), where RpiNvMemFvb + FaultTolerantWrite +
  # VariableStandaloneMm (BL32_AP_MM.fd, edk2-standalone-mm recipe) run the
  # authenticated variable stack on it directly -- no storage device, no
  # OP-TEE storage-service traffic. The DXE side is the MM transport plus
  # the persistence engine that writes the window back to
  # armstub8-2712.bin / RPI_EFI.fd on the boot FAT (VarStoreSync.c,
  # VarBlockServiceDxe's model).
  #
  Platform/RaspberryPi/RPi5/Drivers/MmCommunicationOpteeDxe/MmCommunicationOpteeDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableSmmRuntimeDxe.inf
!else
  Platform/RaspberryPi/Drivers/VarBlockServiceDxe/VarBlockServiceDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
  }
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
      NULL|MdeModulePkg/Library/VarCheckUefiLib/VarCheckUefiLib.inf
      DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  }
!endif
!if $(SECURE_BOOT_ENABLE) == TRUE
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
      NULL|SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.inf
  }
  SecurityPkg/VariableAuthenticated/SecureBootConfigDxe/SecureBootConfigDxe.inf
  SecurityPkg/EnrollFromDefaultKeysApp/EnrollFromDefaultKeysApp.inf
  SecurityPkg/VariableAuthenticated/SecureBootDefaultKeysDxe/SecureBootDefaultKeysDxe.inf
!else
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
!endif
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf {
    <LibraryClasses>
      RealTimeClockLib|Platform/RaspberryPi/Library/RpiRtcLib/RpiRtcLib.inf
  }
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf

  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  Platform/RaspberryPi/Drivers/DisplayDxe/DisplayDxe.inf
  EmbeddedPkg/Drivers/ConsolePrefDxe/ConsolePrefDxe.inf

  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  ArmPkg/Drivers/ArmGicDxe/ArmGicV2Dxe.inf
  Platform/RaspberryPi/Drivers/RpiFirmwareDxe/RpiFirmwareDxe.inf
  Platform/RaspberryPi/RPi5/Drivers/RpiPlatformDxe/RpiPlatformDxe.inf
  Platform/RaspberryPi/Drivers/FdtDxe/FdtDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/EbcDxe/EbcDxe.inf

  #
  # FAT filesystem + GPT/MBR partitioning
  #
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  #
  # ACPI Support
  #
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|$(DEBUG_PROPERTY_MASK) & ~0x04
  }
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
  Platform/RaspberryPi/RPi5/AcpiTables/AcpiTables.inf

  #
  # SMBIOS Support
  #
  Platform/RaspberryPi/Drivers/PlatformSmbiosDxe/PlatformSmbiosDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf

  #
  # RAM Disk Support
  #
  MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf

  #
  # Bds
  #
  MdeModulePkg/Universal/BootManagerPolicyDxe/BootManagerPolicyDxe.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  Platform/RaspberryPi/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/BootDiscoveryPolicyUiLib/BootDiscoveryPolicyUiLib.inf
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
  }

  #
  # SCSI Bus and Disk Driver
  #
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  #
  # USB Support
  #
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  #
  # DWC2 OTG host on the USB-C data port (forced to host mode - see the
  # 0006 layer patch). Scoped DMA parameters: this core masters the AXI
  # bus identity-mapped (no 0xC000_0000 legacy alias like the platform's
  # global DmaLib default assumes), and its DMA address registers are 32
  # bits wide, so buffers must sit below 4 GB - the 3 GB cap matches the
  # BAR-reclaim caution used for PCIe devices in this DSC.
  #
  Platform/RaspberryPi/Drivers/DwUsbHostDxe/DwUsbHostDxe.inf {
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0x00000000
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xbfffffff
  }
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  #
  # SD/eMMC Support
  #
  MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf
  MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
  MdeModulePkg/Bus/Pci/SdMmcPciHcDxe/SdMmcPciHcDxe.inf
  Silicon/Broadcom/Drivers/BrcmStbSdhciDxe/BrcmStbSdhciDxe.inf

  #
  # Networking stack
  #
!include NetworkPkg/Network.dsc.inc

  #
  # Onboard Gigabit Ethernet: the Cadence GEM_GXL (r1p09) MAC inside the RP1
  # southbridge, bound to the vendor NON_DISCOVERABLE_DEVICE Rp1BusDxe
  # registers. Placed and scoped exactly as RPi4.dsc places BcmGenetDxe, the
  # equivalent driver for the Pi 4 -- component entry right after the
  # NetworkPkg include, with the DMA window pinned in a <PcdsFixedAtBuild>
  # override. The values differ from RPi4's because this SoC's 32-bit BAR
  # window overlaps DRAM: identity bus mapping, buffers kept below 3 GiB,
  # matching what NonCoherentIoMmuDxe gets elsewhere in this file. DmaLib is
  # already NonCoherentDmaLib globally.
  #
  # Unconditional, as GENET is for RPi4: this is the board's only onboard NIC,
  # and a platform that cannot reach the network is no use here. Phy.c drives
  # the PHY reset line through Rp1GpioLib, mapped globally above.
  #
  Silicon/Broadcom/Drivers/Net/Rp1GemDxe/Rp1GemDxe.inf {
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0x0
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xBFFFFFFF
  }

  #
  # RNG
  #
  Silicon/Broadcom/Bcm283x/Drivers/Bcm2838RngDxe/Bcm2838RngDxe.inf

  #
  # PCI Support
  #
  ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  EmbeddedPkg/Drivers/NonCoherentIoMmuDxe/NonCoherentIoMmuDxe.inf {
    <PcdsFixedAtBuild>
      #
      # Limit DMA to bottom 3 GB to account for 32-bit BAR space.
      # We may attempt to reclaim the memory already reserved for this,
      # so without a hard limit here, devices in UEFI would start running
      # into corruption issues.
      #
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0x00000000
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xbfffffff
  }

  #
  # RP1 I/O bridge
  #
  Silicon/RaspberryPi/RpiSiliconPkg/Drivers/Rp1BusDxe/Rp1BusDxe.inf

  #
  # NVMe boot devices
  #
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf

  #
  # AHCI Support
  #
  MdeModulePkg/Bus/Pci/SataControllerDxe/SataControllerDxe.inf
  MdeModulePkg/Bus/Ata/AtaAtapiPassThru/AtaAtapiPassThru.inf
  MdeModulePkg/Bus/Ata/AtaBusDxe/AtaBusDxe.inf

  #
  # EFI Memory Attribute Protocol Manager
  #
  Platform/RaspberryPi/Drivers/MemoryAttributeManagerDxe/MemoryAttributeManagerDxe.inf

  #
  # UEFI application (Shell Embedded Boot Loader)
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellNetwork1CommandsLib/UefiShellNetwork1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf

    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8000
      gEfiShellPkgTokenSpaceGuid.PcdShellFileOperationSize|0x200000
  }
!if $(INCLUDE_TFTP_COMMAND) == TRUE
  ShellPkg/DynamicCommand/TftpDynamicCommand/TftpDynamicCommand.inf  {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }
!endif

  ArmPkg/Drivers/ArmPsciMpServicesDxe/ArmPsciMpServicesDxe.inf
  UefiCpuPkg/Test/UnitTest/EfiMpServicesPpiProtocol/EfiMpServiceProtocolShellUnitTest.inf {
    <LibraryClasses>
      UnitTestLib|UnitTestFrameworkPkg/Library/UnitTestLib/UnitTestLib.inf
      UnitTestPersistenceLib|UnitTestFrameworkPkg/Library/UnitTestPersistenceLibNull/UnitTestPersistenceLibNull.inf
      UnitTestResultReportLib|UnitTestFrameworkPkg/Library/UnitTestResultReportLib/UnitTestResultReportLibConOut.inf
  }

  #
  # Board integration: the Pi 5's power button, its Active Cooler fan and the
  # Setup page that sets the fan policy, and the bootloader-EEPROM provenance
  # variables published for the BMC.
  #
  Platform/RaspberryPi/RPi5/Drivers/BootloaderConfigDxe/BootloaderConfigDxe.inf
  Platform/RaspberryPi/RPi5/Drivers/RpiScmiConfigDxe/RpiScmiConfigDxe.inf
  Platform/RaspberryPi/RPi5/Drivers/ActiveCoolerScmiDxe/ActiveCoolerScmiDxe.inf
  Platform/RaspberryPi/RPi5/Drivers/FanConfigDxe/FanConfigDxe.inf

  #
  # BMC-managed IPv4 policy for the onboard NIC: an efivarstore Setup page
  # published under Network Device List (EthCfg variable, EthIp4* Redfish
  # attributes) applied into Ip4Config2 when Ip4Dxe binds the GEM.
  #
  Platform/RaspberryPi/RPi5/Drivers/EthConfigDxe/EthConfigDxe.inf
!if $(RPI5_OPTEE) == TRUE
  #
  # The EDK2->OP-TEE late initialization handshake: once Rp1BusDxe is up
  # (PCIe enumerated, RP1 BAR assigned) it muxes GPIO2/3 to I2C1 and
  # hands the BAR to the OP-TEE sensor pTA over the static SHM.
  #
  Platform/RaspberryPi/RPi5/Drivers/RpiOpteeSensorDxe/RpiOpteeSensorDxe.inf
!endif
!if $(SECURE_BOOT_ENABLE) == TRUE
  #
  # The Setup checkbox (and therefore the /Bios/Attributes/SecureBoot Redfish
  # attribute) only means anything with real AuthVariableLib behind it.
  #
  Platform/RaspberryPi/Drivers/SecureBootToggleDxe/SecureBootToggleDxe.inf
!endif

  #
  # Redfish host interface core, plus this board's inventory/firmware sync.
  #
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
  Platform/RaspberryPi/RPi5/Drivers/RpiRedfishSyncDxe/RpiRedfishSyncDxe.inf

  #
  # Self-applying capsule updater. Built here so it rides the same pins and
  # toolchain as the firmware, deployed onto the capsule volume as
  # \EFI\BOOT\BOOTAA64.EFI by rpi5-capsule-image -- deliberately NOT in
  # the FDF: it lives on the update media, not in the FD.
  #
  Platform/RaspberryPi/RPi5/Applications/Rpi5CapsuleApp/Rpi5CapsuleApp.inf

  #
  # edk2-redfish-client feature drivers and schema converters.
  #
  # Two of RedfishClientPkg's three HiiToRedfish* drivers are deliberately NOT
  # built. HiiToRedfishBiosDxe publishes /Bios/Attributes/BiosOption1..4 whose
  # values are "Item #1".."Item #3"; HiiToRedfishMemoryDxe publishes four
  # invented DIMMs on a board whose LPDDR4X is soldered down.
  # RedfishPlatformConfigDxe would hand both to the BMC as real, settable
  # inventory. The platform's actual attributes come from its own per-formset
  # *Map.uni files instead -- see RPi5/Drivers/RpiRedfishSyncDxe/README.md.
  #
  # HiiToRedfishBootDxe stays, despite the sample-looking name and the
  # "HII to Redfish (Boot)" Setup page it adds, because it is the ONLY backing
  # for ComputerSystem's Boot/BootOrder array: it builds an ordered list from
  # EfiBootManagerGetLoadOptions(), tags each entry Boot%04x in the
  # x-UEFI-redfish-ComputerSystem.v1_13_0 language, and rewrites the real
  # BootOrder variable on submit. ComputerSystemDxe resolves every Boot/*
  # property by configure-language lookup alone, so without this driver the
  # BMC can enumerate boot options through BootOptionCollectionDxe (which
  # reads the boot manager directly) but cannot persistently reorder them.
  #
  # That v1_13_0 tag comes from a patch in the edk2-redfish-client recipe.
  # Upstream still ships v1_5_0, which matched the v1_5_0 feature driver this
  # platform used to build. A feature driver looks its HII questions up by the
  # exact configure-language string it was compiled with, so the ComputerSystem
  # version below and HiiToRedfishBootDxe's tags have to move as a pair -- a
  # skew between them costs the boot order and reports itself only as a run of
  # per-property "No match HII statement" misses.
  #
  # Its BootSourceOverride{Enabled,Mode,Target} questions are a different
  # matter, and worth knowing about: they map to the same
  # /Systems/{1}/Boot/BootSourceOverride* paths that RpiRedfishSyncDxe reads
  # FROM the BMC to stage a one-shot BootNext, and nothing on the host
  # consumes their efivarstore copy. Two writers, one property, opposite
  # directions -- if a staged override ever comes back cleared, provisioning
  # pushing the host's defaults (Disabled/UEFI/None) is the first suspect.
  #
  RedfishPkg/RestJsonStructureDxe/RestJsonStructureDxe.inf
  RedfishPkg/RedfishPlatformConfigDxe/RedfishPlatformConfigDxe.inf
  MdeModulePkg/Universal/RegularExpressionDxe/RegularExpressionDxe.inf
  RedfishClientPkg/RedfishFeatureCoreDxe/RedfishFeatureCoreDxe.inf
  RedfishClientPkg/RedfishETagDxe/RedfishETagDxe.inf
  RedfishClientPkg/RedfishConfigLangMapDxe/RedfishConfigLangMapDxe.inf
  RedfishClientPkg/HiiToRedfishBootDxe/HiiToRedfishBootDxe.inf
  RedfishClientPkg/Features/ComputerSystem/v1_13_0/Dxe/ComputerSystemDxe.inf
  RedfishClientPkg/Features/ComputerSystemCollectionDxe/ComputerSystemCollectionDxe.inf
  RedfishClientPkg/Features/Bios/v1_1_0/Dxe/BiosDxe.inf
  RedfishClientPkg/Features/BiosAttributeRegistry/v1_3_6/BiosAttributeRegistryDxe.inf
  RedfishClientPkg/Features/BootOptionCollection/BootOptionCollectionDxe.inf
  RedfishClientPkg/Features/BootOption/v1_0_4/Dxe/BootOptionDxe.inf
  RedfishClientPkg/Features/SecureBoot/v1_1_0/Dxe/SecureBootDxe.inf
  RedfishClientPkg/Features/Memory/V1_7_1/Dxe/MemoryDxe.inf
  RedfishClientPkg/Features/MemoryCollectionDxe/MemoryCollectionDxe.inf
  RedfishClientPkg/Converter/ComputerSystem/v1_13_0/RedfishComputerSystem_V1_13_0_Dxe.inf
  RedfishClientPkg/Converter/ComputerSystemCollection/RedfishComputerSystemCollection_Dxe.inf
  RedfishClientPkg/Converter/Bios/v1_1_0/RedfishBios_V1_1_0_Dxe.inf
  RedfishClientPkg/Converter/AttributeRegistry/v1_3_6/RedfishAttributeRegistry_V1_3_6_Dxe.inf
  RedfishClientPkg/Converter/BootOptionCollection/RedfishBootOptionCollection_Dxe.inf
  RedfishClientPkg/Converter/BootOption/v1_0_4/RedfishBootOption_V1_0_4_Dxe.inf
  RedfishClientPkg/Converter/Memory/v1_7_1/RedfishMemory_V1_7_1_Dxe.inf
  RedfishClientPkg/Converter/MemoryCollection/RedfishMemoryCollection_Dxe.inf
  RedfishClientPkg/Converter/SecureBoot/v1_1_0/RedfishSecureBoot_V1_1_0_Dxe.inf

  #
  # Firmware Management Protocol + ESRT: a signed capsule through
  # UpdateCapsule() replaces the image in place. FmpDxe authenticates every
  # payload against PcdFmpDevicePkcs7CertBufferXdr, which the
  # firmware recipe appends to the end of this file from its
  # capsule signing certificate.
  #
  FmpDevicePkg/FmpDxe/FmpDxe.inf {
    #
    # FmpDxe refuses to start on its own FILE_GUID: its entry point compares
    # gEfiCallerIdGuid against the value shipped in FmpDevicePkg and returns
    # EFI_UNSUPPORTED if they match, because one FMP instance per updatable
    # image means one FILE_GUID per instance. In DEBUG that is an ASSERT; in
    # RELEASE the ASSERT compiles away but the early return does not, so the
    # driver quietly never publishes EFI_FIRMWARE_MANAGEMENT_PROTOCOL and the
    # whole capsule path is inert with nothing on the console to say so.
    #
    # Same GUID as PcdFmpDeviceImageTypeIdGuid below, deliberately: that PCD
    # is what Rpi5FmpDeviceLib defers to for the ESRT entry (it returns
    # EFI_UNSUPPORTED from FmpDeviceGetImageTypeIdGuidPtr precisely so the
    # value lives in one place), and upstream's own comment calls FILE_GUID
    # "the ESRT GUID". Keeping them equal means there is a single identity for
    # this firmware image rather than two that can drift.
    #
    # Do not change it casually once boards are in the field: FmpDxe namespaces
    # its non-volatile state -- version, lowest supported version, last attempt
    # status -- under gEfiCallerIdGuid, so a new value orphans all of it and the
    # FDF entry at RPi5.fdf must be updated in step.
    #
    <Defines>
      FILE_GUID = a3f8e2d1-5c47-4b96-8f0a-6d21b7e4c358
    <LibraryClasses>
      FmpDeviceLib|Platform/RaspberryPi/RPi5/Library/Rpi5FmpDeviceLib/Rpi5FmpDeviceLib.inf
      FmpPayloadHeaderLib|FmpDevicePkg/Library/FmpPayloadHeaderLibV1/FmpPayloadHeaderLibV1.inf
      CapsuleUpdatePolicyLib|FmpDevicePkg/Library/CapsuleUpdatePolicyLibNull/CapsuleUpdatePolicyLibNull.inf
      FmpDependencyLib|FmpDevicePkg/Library/FmpDependencyLib/FmpDependencyLib.inf
      FmpDependencyCheckLib|FmpDevicePkg/Library/FmpDependencyCheckLibNull/FmpDependencyCheckLibNull.inf
      FmpDependencyDeviceLib|FmpDevicePkg/Library/FmpDependencyDeviceLibNull/FmpDependencyDeviceLibNull.inf
    <PcdsFixedAtBuild>
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceImageTypeIdGuid|{GUID("a3f8e2d1-5c47-4b96-8f0a-6d21b7e4c358")}
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceImageIdName|L"Raspberry Pi 5 UEFI Firmware"
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceBuildTimeLowestSupportedVersion|0
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceProgressWatchdogTimeInSeconds|0
  }
  MdeModulePkg/Universal/EsrtFmpDxe/EsrtFmpDxe.inf

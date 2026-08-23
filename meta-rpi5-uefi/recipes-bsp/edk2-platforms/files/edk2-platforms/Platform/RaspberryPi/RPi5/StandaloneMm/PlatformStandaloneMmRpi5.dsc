#
#  StandaloneMM platform for the Raspberry Pi 5 (pi-bmc): UEFI variables in
#  the VPU-loaded FD NV window.
#
#  Derived from Platform/StandaloneMm/PlatformStandaloneMmPkg/
#  PlatformStandaloneMmRpmb.dsc, with the OP-TEE RPMB storage path removed.
#  There is no storage device and no OP-TEE storage-service traffic at all:
#  the variable store IS the RPi5.fdf NV region (PA 0x3b0000..0x3d0000 of
#  armstub8-2712.bin / RPI_EFI.fd, loaded to PA 0 by the VPU before anything
#  runs). OP-TEE maps that window non-secure + cached into the StMM SP at the
#  fixed VA below (CFG_STMM_VARSTORE_* in plat-rpi5), and RpiNvMemFvb serves
#  it as a plain memory FVB. Persistence is the normal world's job: the DXE
#  transport (MmCommunicationOpteeDxe) writes the window back into the file
#  on the boot FAT at ReadyToBoot/reset, VarBlockServiceDxe-style.
#
#  The VA layout MUST mirror the RPi5.fdf NV region layout offset-for-offset
#  (the window is one mapping; each PCD below = VA base + FD-relative offset):
#
#    FD offset   VA            what                       size
#    0x003b0000  0x48000000    variable store (auth)      0xe000
#    0x003be000  0x4800e000    NS event log (not ours)    0x1000
#    0x003bf000  0x4800f000    FTW working block          0x1000
#    0x003c0000  0x48010000    FTW spare                  0x10000
#
#  PcdMaxVariableSize/PcdMaxAuthVariableSize MUST stay equal to the values in
#  RPi5.dsc: VariableSmmRuntimeDxe (NS) sizes its communicate payloads from
#  its copies and VariableStandaloneMm (here) enforces its own.
#
#  Copyright (c) 2018-2024, Arm Limited. All rights reserved.
#  Copyright (c) 2020, Linaro Ltd. All rights reserved.
#  Copyright (c) 2026, pi-bmc contributors
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

[Defines]
  PLATFORM_NAME                  = MmStandaloneRpi5
  PLATFORM_GUID                  = 3c06e9bd-3a1a-4d3f-9c0e-8f1745b2a601
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x0001001C
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/RaspberryPi/RPi5/StandaloneMm/PlatformStandaloneMmRpi5.fdf
  DEFINE DEBUG_MESSAGE           = TRUE

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmSvcLib|MdePkg/Library/ArmSvcLib/ArmSvcLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmFfaLib|MdeModulePkg/Library/ArmFfaLib/ArmFfaStandaloneMmLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  HobLib|StandaloneMmPkg/Library/StandaloneMmCoreHobLib/StandaloneMmCoreHobLib.inf
  HobPrintLib|MdeModulePkg/Library/HobPrintLib/HobPrintLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemLib|StandaloneMmPkg/Library/StandaloneMmMemLib/StandaloneMmMemLib.inf
  MemoryAllocationLib|StandaloneMmPkg/Library/StandaloneMmCoreMemoryAllocationLib/StandaloneMmCoreMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  MmServicesTableLib|MdePkg/Library/StandaloneMmServicesTableLib/StandaloneMmServicesTableLib.inf

  #
  # Entry point
  #
  StandaloneMmCoreEntryPoint|ArmPkg/Library/ArmStandaloneMmCoreEntryPoint/ArmStandaloneMmCoreEntryPoint.inf
  StandaloneMmDriverEntryPoint|MdePkg/Library/StandaloneMmDriverEntryPoint/StandaloneMmDriverEntryPoint.inf

  StandaloneMmMmuLib|ArmPkg/Library/StandaloneMmMmuLib/ArmMmuStandaloneMmLib.inf
  CacheMaintenanceLib|MdePkg/Library/BaseCacheMaintenanceLibNull/BaseCacheMaintenanceLibNull.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffExtraActionLib|StandaloneMmPkg/Library/StandaloneMmPeCoffExtraActionLib/StandaloneMmPeCoffExtraActionLib.inf
  RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf

  SerialPortLib|MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf

[LibraryClasses.common.MM_CORE_STANDALONE]
  ArmFfaLib|MdeModulePkg/Library/ArmFfaLib/ArmFfaStandaloneMmCoreLib.inf
  ArmTransferListLib|ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  HobLib|StandaloneMmPkg/Library/StandaloneMmCoreHobLib/StandaloneMmCoreHobLib.inf

[LibraryClasses.common.MM_STANDALONE]
  HobLib|StandaloneMmPkg/Library/StandaloneMmHobLib/StandaloneMmHobLib.inf
  MemoryAllocationLib|StandaloneMmPkg/Library/StandaloneMmMemoryAllocationLib/StandaloneMmMemoryAllocationLib.inf

  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  MbedTlsLib|CryptoPkg/Library/MbedTlsLib/MbedTlsLib.inf
  #
  # Empty: SmmCryptLib (mbedtls) names the OpensslLib class but links zero
  # OpenSSL symbols, and real OpenSSL cannot build under the mandatory
  # -mgeneral-regs-only (double-typed OSSL_PARAM API). See the stub.
  #
  OpensslLib|Platform/RaspberryPi/RPi5/StandaloneMm/Library/OpensslLibNull/OpensslLibNull.inf
  PlatformSecureLib|SecurityPkg/Library/PlatformSecureLibNull/PlatformSecureLibNull.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  TimerLib|MdePkg/Library/BaseTimerLibNullTemplate/BaseTimerLibNullTemplate.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x800000CF
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xff
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x0f

  gEfiMdePkgTokenSpaceGuid.PcdMaximumGuidedExtractHandler|0x2

  # Secure storage limits -- MUST match RPi5.dsc (see file header).
  gEfiSecurityPkgTokenSpaceGuid.PcdUserPhysicalPresence|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800

  #
  # The FD NV window as the StMM SP sees it: fixed VAs, offsets mirroring
  # RPi5.fdf (see file header). Sizes MUST equal the RPi5.fdf region sizes --
  # the store contents, including the pre-formatted FV/varstore/FTW headers
  # baked into the FD, are shared with the non-OPTEE (VarBlockServiceDxe)
  # configuration byte-for-byte.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0x48000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x0000e000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0x4800f000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x00001000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0x48010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x00010000

  gEfiMdeModulePkgTokenSpaceGuid.PcdFfaLibConduitSmc|FALSE

  # The BFV is not located in the Flash area but is loaded in the RAM
  # by optee's stmm_sp.c instead, therefore no shadow copy is needed.
  # So disable shadow copy of boot firmware volume while loading StMM drivers.
  #
  gStandaloneMmPkgTokenSpaceGuid.PcdShadowBfv|FALSE

[Components.common]
  #
  # Standalone MM components
  #
  StandaloneMmPkg/Core/StandaloneMmCore.inf
  ArmPkg/Drivers/StandaloneMmCpu/StandaloneMmCpu.inf
  Platform/RaspberryPi/RPi5/StandaloneMm/Drivers/RpiNvMemFvb/RpiNvMemFvb.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteStandaloneMm.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableStandaloneMm.inf {
    <LibraryClasses>
      AuthVariableLib|SecurityPkg/Library/AuthVariableLib/AuthVariableLib.inf
      BaseCryptLib|CryptoPkg/Library/BaseCryptLibMbedTls/SmmCryptLib.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLibBase.inf
      VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
      NULL|MdeModulePkg/Library/VarCheckUefiLib/VarCheckUefiLib.inf
      #
      # Registers the gVarCheckPolicyLibMmiHandlerGuid MMI handler that the
      # NS side's variable policy engine (VariablePolicySmmDxe.c inside
      # VariableSmmRuntimeDxe: VariableLock/RequestToLock, capsule locks,
      # policy registration) communicates with. The upstream RPMB dsc omits
      # it -- u-boot consumers have no variable policy -- and without it
      # those communicates come back with the result field unwritten (seen
      # as pool-poison 0xAFAFAFAF status + BdsDxe capsule-lock ASSERT).
      #
      NULL|MdeModulePkg/Library/VarCheckPolicyLib/VarCheckPolicyLibStandaloneMm.inf
  }

[BuildOptions.AARCH64]
GCC:*_*_*_DLINK_FLAGS = -z common-page-size=0x1000 -march=armv8-a+nofp
# -mgeneral-regs-only: MANDATORY for every module here. This code runs in the
# StMM secure partition at S-EL0 where FP/SIMD access traps (esr EC 0x07) and
# OP-TEE kills the SP. edk2-stable202608's tools_def only applies the flag to
# XIP (SEC/PEI) module types, and gcc otherwise emits q-register spills in
# every varargs prologue (first hit: VariableSmm.c
# CheckRemainingSpaceForConsistency on the first SetVariable). The upstream
# PlatformStandaloneMmRpmb.dsc has the same gap.
# -Wno-error=*: gcc 12/13 false positives in upstream EDK2 sources (e.g.
# ArmStandaloneMmCoreEntryPoint.c), same set the edk2-standalone-mm recipe
# used to append to the upstream dsc.
GCC:*_*_*_CC_FLAGS = -mstrict-align -mgeneral-regs-only -Wno-error=maybe-uninitialized -Wno-error=uninitialized -Wno-error=stringop-overflow -Wno-error=stringop-overread -Wno-error=array-bounds -Wno-error=dangling-pointer -Wno-error=nonnull

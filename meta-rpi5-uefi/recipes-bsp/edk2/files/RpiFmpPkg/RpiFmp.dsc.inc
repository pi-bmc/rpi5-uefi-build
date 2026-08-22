# FMP capsule update components, sed-included into RPi5.dsc's
# [Components.common] by the edk2-rpi5-firmware recipe.
#
# FmpDxe produces EFI_FIRMWARE_MANAGEMENT_PROTOCOL for the platform's own
# firmware image; EsrtFmpDxe turns every FMP instance into the ESRT that fwupd,
# Windows Update and RpiRedfishSyncDxe's SoftwareInventory report all read. The
# capsule arrives through CapsuleRuntimeDxe's UpdateCapsule and is applied by
# ProcessCapsules() at BDS -- both of which RPi5 already builds, and which did
# nothing whatsoever until CapsuleLib stopped being the Null instance (0018).
#
# CAPSULES MUST BE SIGNED. FmpDxe's only FmpAuthenticationLib instances are
# Pkcs7 and Rsa2048Sha256 -- there is no Null -- and it authenticates every
# image against the keys in PcdFmpDevicePkcs7CertBufferXdr before calling the
# device library. An empty cert buffer is therefore not "no verification", it
# is "nothing can ever be applied": the loop over candidate keys simply has no
# candidates. That is a safe failure, but it is a silent one, so the recipe
# fails the build when RPI5_FMP=1 without a capsule certificate rather than
# shipping firmware whose update path cannot work.

  #
  # No FILE_GUID override. FmpDevicePkg's sample platform gives each FmpDxe
  # instance a GUID of its own because it builds several -- one per updatable
  # device -- and they would otherwise collide. There is one updatable device
  # here, so FmpDxe's own INF GUID is unambiguous, and overriding it in the DSC
  # alone breaks the build: the FDF still names the module by its INF GUID and
  # GenFv then looks for an FFS file that was written under the override.
  #
  FmpDevicePkg/FmpDxe/FmpDxe.inf {
    <LibraryClasses>
      FmpDeviceLib|RpiFmpPkg/Library/Rpi5FmpDeviceLib/Rpi5FmpDeviceLib.inf
      FmpPayloadHeaderLib|FmpDevicePkg/Library/FmpPayloadHeaderLibV1/FmpPayloadHeaderLibV1.inf
      CapsuleUpdatePolicyLib|FmpDevicePkg/Library/CapsuleUpdatePolicyLibNull/CapsuleUpdatePolicyLibNull.inf
      FmpDependencyLib|FmpDevicePkg/Library/FmpDependencyLib/FmpDependencyLib.inf
      FmpDependencyCheckLib|FmpDevicePkg/Library/FmpDependencyCheckLibNull/FmpDependencyCheckLibNull.inf
      FmpDependencyDeviceLib|FmpDevicePkg/Library/FmpDependencyDeviceLibNull/FmpDependencyDeviceLibNull.inf
    <PcdsFixedAtBuild>
      #
      # Names the device in ESRT and in every capsule built for it. Change it
      # and every previously built capsule stops applying -- which is the point:
      # it names this firmware, on this board, with this flash layout.
      #
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceImageTypeIdGuid|{GUID("a3f8e2d1-5c47-4b96-8f0a-6d21b7e4c358")}
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceImageIdName|L"Raspberry Pi 5 UEFI Firmware"
      #
      # Anti-rollback floor. 0 accepts any version on a first deployment; raise
      # it once a release exists that must never be downgraded past. Note this
      # is the BUILD-time floor, used because Rpi5FmpDeviceLib reports no
      # device-side value of its own.
      #
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceBuildTimeLowestSupportedVersion|0
      #
      # Leave the watchdog off: writing ~3.8 MB to a FAT volume on a slow card
      # can outlast any reasonable arming, and a reboot mid-write is exactly the
      # outcome the watchdog is meant to prevent.
      #
      gFmpDevicePkgTokenSpaceGuid.PcdFmpDeviceProgressWatchdogTimeInSeconds|0
  }

  MdeModulePkg/Universal/EsrtFmpDxe/EsrtFmpDxe.inf

SUMMARY = "EDK2 UEFI firmware (RPI_EFI.fd) for the Raspberry Pi 5"
DESCRIPTION = "Builds Platform/RaspberryPi/RPi5/RPi5.dsc against UPSTREAM \
               tianocore trees. The former NumberOneGit forks are fully decomposed into \
               this layer: their edk2 delta is one commit (0001-EDK2-Sd-Mmc-v4.patch on \
               the fork's exact master merge-base), and their edk2-platforms RPi5 port is \
               files/edk2-platforms/ (the port's ADDED files, copied in after unpack) \
               plus 0000-edk2-platforms-RPi5-port.patch (its changes to files that exist \
               upstream) -- reconstructed byte-identical to the fork within every package \
               tree this build compiles. TF-A's bl31.bin (see the arm-trusted-firmware \
               recipe) is embedded as the FD.RPI_EFI region-0 payload, and, if RPI5_IPXE \
               is enabled, an iPXE UNDI/SNP driver (see the ipxe-efi recipe) rides along \
               as a prebuilt DXE driver."
HOMEPAGE = "https://github.com/tianocore/edk2"

# edk2 + edk2-platforms are both BSD-2-Clause-Patent (identical License.txt).
# edk2-non-osi carries no single root license file -- it's a grab-bag of
# silicon-vendor binaries/sources, each under its own BSD-2-Clause-Patent-
# family license text embedded in the relevant .inf/source comments, so
# there's nothing meaningful to point LIC_FILES_CHKSUM at there.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

DEPENDS = "acpica-native arm-trusted-firmware util-linux-native"
DEPENDS += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi', '', d)}"
# do_compile validates the Secure Boot key files are DER X.509 before
# handing them to the FDF -- see the RPI5_SECURE_BOOT_DEFAULT_KEYS block.
DEPENDS += "${@bb.utils.contains('RPI5_SECURE_BOOT_DEFAULT_KEYS', '1', 'openssl-native', '', d)}"
# The capsule signing keypair is generated, inspected and used to sign the
# capsule with openssl -- see the RPI5_FMP block. Listed separately from the
# Secure Boot line above because the two features are independent.
DEPENDS += "${@bb.utils.contains('RPI5_FMP', '1', 'openssl-native', '', d)}"

PV = "202602+git${SRCPV}"

# Patch order matters and follows SRC_URI order:
#   edk2 tree:            0001-EDK2-Sd-Mmc-v4 (the former fork's only commit),
#                         then 0100 (UsbNetwork point-to-point media),
#                         0101 (skip IPv6 discovery leg), 0102 (quiesce the
#                         Redfish stack after provisioning -- the in-Setup
#                         gadget-detach use-after-free; NOT at ReadyToBoot,
#                         which races the client feature core and kills
#                         provisioning outright), 0103 (keep USB NICs
#                         out of BDS boot-option enumeration).
#   edk2-platforms tree:  file://edk2-platforms (the RPi5 port's ADDED files)
#                         merges INTO the git checkout at
#                         ${UNPACKDIR}/edk2-platforms during do_unpack, then
#                         0000 (the former fork's changed files), then this
#                         layer's own 0001..0007, 0010, the ACPI pair
#                         0011/0012, and 0013..0017 -- which edit files the
#                         merged tree adds. 0011 (an RP1 I2C1 device) and 0012 (an
#                         RP1 GEM Ethernet device) both append to Rp1.asi, so
#                         they must stay in that order relative to each other.
#                         0013 teaches VarBlockServiceDxe that this layer
#                         deploys the FD as armstub8-2712.bin, not RPI_EFI.fd
#                         -- without it the driver finds no file to write
#                         back to and NOTHING set in Setup survives a reboot.
#                         It is therefore coupled to the mcopy target name in
#                         recipes-bsp/images/rpi5-uefi-sdimg.bb: change one
#                         and you change the other.
#                         0014 flips the SystemTableMode default from ACPI to
#                         Device Tree, in both the PcdsDynamicHii default and
#                         the VFR's F9 "Restore Defaults" value -- the onboard
#                         NIC has no working driver path under ACPI (see the
#                         comment 0014 adds to RPi5.dsc), and a node that
#                         cannot reach the network is no use to the BMC.
#                         0015 makes FdtDxe publish EFI_DT_FIXUP_PROTOCOL, so
#                         systemd-boot can hand us the device tree out of the
#                         Talos UKI -- matched to the kernel in the same image
#                         -- and we write the board-specific values into IT,
#                         rather than shipping a tree of our own and hoping it
#                         fits a kernel we do not ship. Same split u-boot runs
#                         (its EFI_DT_APPLY_FIXUPS path calls ft_board_setup).
#                         The firmware's own tree stays installed as a fallback
#                         for anything that brings none. Paired with 0014: in
#                         ACPI mode FdtDxe never runs at all.
#                         0017 completes that: at ReadyToBoot, FdtDxe looks for
#                         \dtb\bcm2712-rpi-5-b.dtb on any attached filesystem
#                         and hands THAT to the OS. The device tree then lives
#                         with the OS install rather than on the firmware's
#                         card, so one firmware image boots any kernel and
#                         changing OS never means reflashing the board. The
#                         SD-card tree (see talos-boot-dtbs) stays as the
#                         fallback for an OS that ships none.
#                         0016 clears the RP1 MSIX_CFG routing 0001 arms for
#                         the two xHCIs, at ExitBootServices. Linux's
#                         drivers/misc/rp1/rp1_pci.c owns those same registers
#                         but only ever clears ENABLE per-IRQ as its own IRQ
#                         domain tears one down -- it never zeroes the block on
#                         probe -- so anything left armed stays armed behind
#                         the OS's back. Depends on 0001, which is where the
#                         arming lives.
#
#                         The DWC2 OTG host on the USB-C data port is 0006
#                         (put the driver on the BCM2712 core: 64-bit base
#                         PCD, forced host mode),
#                         0007 (make an absent core cheap to discover) and
#                         0010 (fix the reset ordering and the mode-switch
#                         wait); they must stay in that order. Numbering is
#                         sparse: 0008/0009 were a SET_DOMAIN_STATE mailbox
#                         experiment that no longer exists -- Linux powers
#                         USB through the OLD SET_POWER_STATE tag with
#                         device id 3, which is the call 0006 already makes.
#
# ORDERING IS LOAD-BEARING: do_unpack processes SRC_URI entries in listing
# order, and the git fetcher PRUNES its destsuffix dir before checkout --
# the file://edk2-platforms entry must therefore stay AFTER the git entry
# of the same name, or the checkout wipes the port's added files (the
# patch series then fails loudly at 0000/0001).
SRC_URI = "gitsm://github.com/tianocore/edk2.git;protocol=https;branch=master;name=edk2;destsuffix=git \
           git://github.com/tianocore/edk2-platforms.git;protocol=https;branch=master;name=platforms;destsuffix=edk2-platforms \
           git://github.com/tianocore/edk2-non-osi.git;protocol=https;branch=master;name=nonosi;destsuffix=edk2-non-osi \
           git://github.com/tianocore/edk2-redfish-client.git;protocol=https;branch=main;name=redfishclient;destsuffix=edk2-redfish-client \
           file://config.txt \
           file://ipxe-fdf-snippet.fdf.inc \
           file://edk2-platforms \
           file://0001-EDK2-Sd-Mmc-v4.patch \
           file://0000-edk2-platforms-RPi5-port.patch;patchdir=../edk2-platforms \
           file://0001-Rp1BusDxe-register-GEM-and-I2C1-vendor-devices.patch;patchdir=../edk2-platforms \
           file://0002-PlatformSmbiosDxe-deterministic-UUID-and-Type45.patch;patchdir=../edk2-platforms \
           file://0003-RPi5-AcpiTables-add-SsdtThermal.patch;patchdir=../edk2-platforms \
           file://0004-PlatformBm-return-boot-option-number-not-list-index.patch;patchdir=../edk2-platforms \
           file://0005-PlatformBm-prune-USB-NIC-network-boot-options.patch;patchdir=../edk2-platforms \
           file://0006-DwUsbHostDxe-support-the-BCM2712-DWC2-OTG-Pi-5-USB-C.patch;patchdir=../edk2-platforms \
           file://0007-DwUsbHostDxe-fail-fast-when-the-DWC2-core-is-absent.patch;patchdir=../edk2-platforms \
           file://0010-DwUsbHostDxe-fix-the-BCM2712-core-reset-and-mode-swit.patch;patchdir=../edk2-platforms \
           file://0011-Silicon-RP1-add-an-ACPI-I2C1-device.patch;patchdir=../edk2-platforms \
           file://0012-Silicon-RP1-add-an-ACPI-GEM-Ethernet-device.patch;patchdir=../edk2-platforms \
           file://0013-VarBlockServiceDxe-find-the-variable-store-under-eith.patch;patchdir=../edk2-platforms \
           file://0014-RPi5-default-SystemTableMode-to-Device-Tree.patch;patchdir=../edk2-platforms \
           file://0015-FdtDxe-publish-EFI_DT_FIXUP_PROTOCOL.patch;patchdir=../edk2-platforms \
           file://0016-Rp1BusDxe-disarm-RP1-interrupt-routing-at-handoff.patch;patchdir=../edk2-platforms \
           file://0017-FdtDxe-load-the-OS-provided-device-tree-from-its-own-.patch;patchdir=../edk2-platforms \
           file://0018-RPi5-enable-FMP-capsule-processing.patch;patchdir=../edk2-platforms \
           file://0100-UsbNetwork-assume-media-on-a-point-to-point-gadget.patch \
           file://0101-RedfishDiscoverDxe-skip-the-IPv6-discovery-leg.patch \
           file://0102-RedfishConfigHandler-quiesce-the-Redfish-stack-after.patch \
           file://0103-UefiBootManagerLib-do-not-enumerate-USB-NICs-as-boot.patch \
           file://0104-JsonLib-fix-RELEASE-build-of-lex_unget_unsave.patch \
           file://RpiBmcPkg \
           file://Rp1GemPkg \
           file://RpiRedfishPkg \
           file://RpiFmpPkg \
           file://secureboot-keys \
           file://usbnet-dsc-snippet.inc \
           file://usbnet-fdf-snippet.fdf.inc \
           file://profiling-dsc-snippet.inc \
           file://profiling-fdf-snippet.fdf.inc \
           ${SECUREBOOT_MS_CERTS} \
           "

# Microsoft's Secure Boot CA certificates, fetched rather than vendored.
#
# These are the canonical URLs behind Microsoft's fwlink redirects (321185,
# 321192, 321194 for the 2011 generation; 2239775, 2239776, 2239872 for
# 2023). Use these direct URLs, NOT the fwlinks: bitbake's decodeurl() /
# encodeurl() round trip mangles a query string, turning "?linkid=2239775"
# into "%3Flinkid%3D2239775" and breaking the fetch. The %20 in the 2023
# paths does survive the round trip -- verified against poky's own
# bitbake/lib/bb/fetch2.
#
# The sha256sum of each file IS that certificate's SHA-256 fingerprint,
# because these are raw DER blobs. So the checksums below are not just
# download integrity -- they pin the exact certificate identity, and
# bitbake refuses to build if Microsoft ever serves different bytes at
# these paths. Cross-check any change against the fingerprint table in
# files/secureboot-keys/README.md before accepting it.
#
# Only pulled in when the default key set is wanted, so a build with
# RPI5_SECURE_BOOT_DEFAULT_KEYS=0 needs no network access for this.
SECUREBOOT_MS_CERT_BASE = "https://www.microsoft.com/pkiops/certs"
SECUREBOOT_MS_CERT_URIS = "\
    ${SECUREBOOT_MS_CERT_BASE}/MicCorKEKCA2011_2011-06-24.crt;name=mskek2011;downloadfilename=MicCorKEKCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/microsoft%20corporation%20kek%202k%20ca%202023.crt;name=mskek2023;downloadfilename=MicCorKEK2KCA2023.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/MicWinProPCA2011_2011-10-19.crt;name=msdbwin2011;downloadfilename=MicWinProPCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/MicCorUEFCA2011_2011-06-27.crt;name=msdbuefi2011;downloadfilename=MicCorUEFCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/windows%20uefi%20ca%202023.crt;name=msdbwin2023;downloadfilename=WindowsUEFICA2023.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/microsoft%20uefi%20ca%202023.crt;name=msdbuefi2023;downloadfilename=MicrosoftUEFICA2023.crt;subdir=secureboot-certs \
"
# Kept as a plain variable and selected here, rather than building the list
# inside the inline Python: that keeps expansion order out of the picture.
SECUREBOOT_MS_CERTS = "${@bb.utils.contains('RPI5_SECURE_BOOT_DEFAULT_KEYS', '1', d.getVar('SECUREBOOT_MS_CERT_URIS'), '', d)}"

# www.microsoft.com refuses the wget fetcher with a bare 403, which surfaces
# as "wget ... failed with exit code 8" on do_fetch. It is the User-Agent
# alone, not TLS fingerprinting or a dead URL: the same request succeeds from
# curl, `curl -A wget/1.21` is refused, and a browser UA on wget succeeds --
# all six certificate URLs verified 200 that way (2026-08-18).
#
# So give the fetcher a browser User-Agent. Overriding FETCHCMD_wget is
# recipe-scoped and only affects HTTP(S) fetches, which here is exactly the
# Secure Boot certificate set -- every source tree comes down the git
# fetcher. The rest of the command line stays bitbake's own default
# (bitbake/lib/bb/fetch2/wget.py: "/usr/bin/env wget -t 2 -T 100"), since
# bitbake appends -O/-P/--progress itself.
#
# The pinned sha256sums above are what actually guarantee we got the right
# bytes, so relaxing how they are requested costs nothing in integrity.
FETCHCMD_wget = "/usr/bin/env wget -t 2 -T 100 --user-agent='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'"

SRC_URI[mskek2011.sha256sum]   = "a1117f516a32cefcba3f2d1ace10a87972fd6bbe8fe0d0b996e09e65d802a503"
SRC_URI[mskek2023.sha256sum]   = "3cd3f0309edae228767a976dd40d9f4affc4fbd5218f2e8cc3c9dd97e8ac6f9d"
SRC_URI[msdbwin2011.sha256sum] = "e8e95f0733a55e8bad7be0a1413ee23c51fcea64b3c8fa6a786935fddcc71961"
SRC_URI[msdbuefi2011.sha256sum] = "48e99b991f57fc52f76149599bff0a58c47154229b9f8d603ac40d3500248507"
SRC_URI[msdbwin2023.sha256sum] = "076f1fea90ac29155ebf77c17682f75f1fdd1be196da302dc8461e350a9ae330"
SRC_URI[msdbuefi2023.sha256sum] = "f6124e34125bee3fe6d79a574eaa7b91c0e7bd9d929c1a321178efd611dad901"
# Upstream pins chosen for byte-parity with the retired NumberOneGit forks
# (audited 2026-08-17 with git merge-base + reconstruction diffs):
#   edk2:      the fork was upstream master @ this exact commit plus ONE
#              commit -- the SD fixup now carried as 0001-EDK2-Sd-Mmc-v4.patch.
#   platforms: the fork's merge-base with upstream master (2024-03-12); the
#              fork's 32-commit RPi5 port on top of it is decomposed into
#              files/edk2-platforms/ (75 added files) + the 0000 patch
#              (43 changed/deleted files). The fork's bulk-sync churn in
#              other vendors' trees (187 files never referenced by
#              RPi5.dsc/.fdf) is deliberately dropped.
#   non-osi:   the fork was upstream @ this commit plus only an RPi5
#              TrustedFirmware bl31.bin -- dead weight here, because this
#              recipe always passes -D TFA_BUILD_ARTIFACTS pointing at our
#              own arm-trusted-firmware deploy.
SRCREV_edk2 = "c4d29cb62187060493a1f595083ddfb6dd346f39"
SRCREV_platforms = "80ee8b861edb6a8b02a100f63bbb435499f8741a"
SRCREV_nonosi = "94d048981116e2e3eda52dad1a89958ee404098d"
# edk2-redfish-client tracks edk2 MASTER, and the pinned NumberOneGit edk2
# is master-based too -- its RedfishPkg carries commits through 2026-02-03,
# despite the fork's "202405" branding. That dates the compatibility
# window precisely (audited 2026-08-17):
#   >= 73a1eaa41 (2026-01-17): RedfishPlatformConfigSetValue grew a
#     by-pointer value argument in edk2 and the client the same day; an
#     older client passes by value and fails to compile against this fork.
#   <  b8ffa6e45 (2026-05-05): the client's RedfishEventLib starts needing
#     gEdkIIRedfisEventRedfishInterfaceDisconnectionGuid, which this fork's
#     RedfishPkg.dec predates (the GUID nuc-bios-build moved to edk2 master
#     for -- see edk2-uefipayload_2605.bb).
# This pin is the last commit before that boundary; every external GUID it
# references is declared by the pinned edk2's .dec files.
SRCREV_redfishclient = "a75f45cd69c74121fbf58900b9d92735d9a3373c"
SRCREV_FORMAT = "edk2_platforms_nonosi_redfishclient"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S = "${UNPACKDIR}/git" never
# expands and do_unpack fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

EDK2_PLATFORMS_PATH = "${UNPACKDIR}/edk2-platforms"
EDK2_NON_OSI_PATH = "${UNPACKDIR}/edk2-non-osi"
EDK2_REDFISH_CLIENT_PATH = "${UNPACKDIR}/edk2-redfish-client"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

S = "${UNPACKDIR}/git"

do_compile[depends] += "arm-trusted-firmware:do_deploy"
do_compile[depends] += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi:do_deploy', '', d)}"

# iPXE embedding, now OFF by default: the onboard RJ45 PXE/HTTP-boots
# natively via Rp1GemDxe (RPI5_RP1_ETH) + NetworkPkg's own UefiPxeBcDxe, so
# iPXE's only remaining coverage is add-on NICs (a PCIe card on the FPC, or
# a USB dongle from iPXE's driver table). Set to "1" to embed it for those.
RPI5_IPXE ??= "0"

# Native RP1 GEM (onboard RJ45) SNP driver, built from the local Rp1GemPkg
# source package. Coexists with iPXE: disjoint hardware, both SNPs feed the
# same NetworkPkg stack.
RPI5_RP1_ETH ??= "1"

# Board-integration driver set (local RpiBmcPkg): power button, active
# cooler, BootloaderConfig provenance variables. (The I2C EEPROM sync
# drivers that used to live behind this knob were replaced by the Redfish
# host interface -- see RPI5_REDFISH.)
RPI5_BMC ??= "1"

# Wire edk2's own USB CDC-ECM/NCM/RNDIS class drivers (present in the
# pinned tree, not in RPi5.dsc) into the build, so a USB Ethernet gadget on
# an RP1 port becomes an SNP interface. The BMC's host-interface link rides
# the ncm.usb0 function of this -- required by RPI5_REDFISH.
RPI5_USBNET ??= "1"

# Boot-time profiling: adds ShellPkg's Dp ("dump performance") dynamic
# command, so `dp` at the UEFI shell prints per-driver load/init times in
# milliseconds. The platform already records the measurements
# (PcdPerformanceLibraryPropertyMask|1 + DxeCorePerformanceLib on DxeCore
# in RPi5.dsc), so this only adds the reader -- and only the FV space it
# occupies, which is why it is off by default.
#
# Set it in local.conf/kas.yml (PROFILING_ENABLED = "1"), or per-invocation
# with `bitbake -R` / an env var exported through BB_ENV_PASSTHROUGH_ADDITIONS.
# Whatever the value, it reaches EDK2 as -D PROFILING_ENABLED=TRUE|FALSE,
# which is the macro the profiling-*-snippet files gate on.
PROFILING_ENABLED ??= "0"

# Redfish Host Interface (DSP0270) over the BMC's USB CDC-NCM gadget: the
# JetKVM method from ../nuc-bios-build, NCM instead of ECM. Replaces the
# old I2C shared-EEPROM sync as the BMC data path (local RpiRedfishPkg;
# wire contract in its README.md). Needs RPI5_USBNET=1 for the NIC driver.
RPI5_REDFISH ??= "1"

# edk2-redfish-client (RedfishClientPkg) on top of the host interface: the
# standard feature layer, with the FV set taken verbatim from the client's
# own RedfishClient.fdf.inc (REDFISH_CLIENT=TRUE is passed to the build
# when this knob is on; RpiRedfishPkg/RpiRedfishClient.dsc.inc builds the
# matching components -- see that file's header). Requires RPI5_REDFISH.
RPI5_REDFISH_CLIENT ??= "1"

# The wire contract with the BMC, rendered into RpiRedfish.dsc.inc's PCDs.
# RPI5_REDFISH_MAC is the gadget's host_addr (the MAC the Pi's NCM NIC
# comes up with) -- the BMC must present exactly this fixed value, or
# RedfishDiscoverDxe rejects the interface. Colon-separated, lowercase ok.
RPI5_REDFISH_MAC ??= "da:c0:ff:ee:10:02"
# HTTP Basic credentials for the BMC's Redfish service (nanokvm-app's
# CheckAuth). Set RPI5_REDFISH_USER to "" for a BMC with authentication
# disabled (the credential library then reports AuthMethodNone).
RPI5_REDFISH_USER ??= "admin"
RPI5_REDFISH_PASSWORD ??= "admin"

# UEFI Secure Boot. RPi5.dsc already carries a complete SECURE_BOOT_ENABLE
# story -- real AuthVariableLib instead of the Null one, SecureBootConfigDxe
# (the Setup pages for enrolling PK/KEK/db and the "Attempt Secure Boot"
# toggle), SecureBootDefaultKeysDxe, EnrollFromDefaultKeysApp, and
# DxeImageVerificationLib wired into SecurityStubDxe -- all of it behind
# !if blocks that default to FALSE upstream. This knob just turns it on.
#
# Two things follow from it:
#   * RpiBmcPkg/SecureBootToggleDxe's checkbox (and therefore the
#     /Bios/Attributes/SecureBoot Redfish attribute) only does anything with
#     this on: with AuthVariableLibNull the SecureBootEnable variable is not
#     interpreted by anyone.
#   * RedfishClientPkg's SecureBootDxe feature driver, which serves
#     Systems/1/SecureBoot, links SecureBootVariableLib either way --
#     RpiRedfishClient.dsc.inc maps it itself when this is FALSE.
#
# Size: enabling this pulls OpensslLib/BaseCryptLib into VariableRuntimeDxe
# and SecurityStubDxe. Check FVMAIN_COMPACT headroom in RPI_EFI.report.txt
# after the first build with it on.
RPI5_SECURE_BOOT ??= "1"

# Embed the default key set (files/secureboot-keys, see its README) as
# PKDefault/KEKDefault/dbDefault. Without this the SECURE_BOOT_ENABLE build
# still boots, but comes up in Setup Mode with empty defaults and there is
# nothing for "Reset Secure Boot Keys" to enroll.
#
# Every file must be a DER X.509 cert with an RSA key: the FDF wires each
# one in as a SECTION RAW and SecureBootVariableProvisionLib runs
# RsaGetPublicKeyFromX509() over it, building the EFI_SIGNATURE_LISTs
# itself. No PEM, no .esl.
#
# DBX is deliberately unset: the UEFI revocation list is image hashes, not
# certificates, so it cannot go through this path at all. Apply revocations
# at runtime with a signed dbx update.
#
# Enrolling stays a deliberate act — see the README. SecureBootDefaultKeysDxe
# only populates the *Default variables; the platform boots in Setup Mode
# until someone runs "Reset Secure Boot Keys" in Setup or
# EnrollFromDefaultKeysApp from the Shell.
RPI5_SECURE_BOOT_DEFAULT_KEYS ??= "1"

# RELEASE, DEBUG or NOOPT, per RPi5.dsc's [Defines] BUILD_TARGETS.
RPI5_BUILD_TARGET ??= "RELEASE"

# Embedded in the built FD via PcdFirmwareVersionString; surfaced by UEFI's
# "Firmware Version" info and by `dmidecode`/`fwupdmgr` on the running OS.
RPI5_FW_VERSION ??= "${PV}"

# Firmware Management Protocol + ESRT + capsule application. Without this the
# firmware can only be updated by rewriting the card by hand; with it, a signed
# capsule applied through UpdateCapsule() replaces the image in place, and the
# running version shows up in ESRT and in the BMC's Redfish SoftwareInventory.
#
# OFF by default. Turning it on needs no other configuration: with neither
# RPI5_FMP_CERT nor RPI5_FMP_KEY set, do_compile generates a self-signed
# capsule signing keypair under RPI5_FMP_KEYDIR and do_deploy emits a signed
# RPi5Firmware.cap alongside the firmware. Read the RPI5_FMP_KEYDIR comment
# before shipping anything built that way -- a generated key is convenient,
# not managed. See files/RpiFmpPkg/README.md.
#
# What is NOT invented on your behalf is a certificate whose private key
# nobody holds: that would look like a working update path and be nothing of
# the sort, since FmpDxe authenticates every payload against the certificates
# in PcdFmpDevicePkcs7CertBufferXdr and applies nothing when the loop over
# candidate keys has no candidates.
RPI5_FMP ??= "0"

# The integer version ESRT publishes and FmpDxe compares for anti-rollback --
# distinct from RPI5_FW_VERSION, which is the human-readable string. Derived
# from PV's leading numeric part (202602+git -> 202602). Carried both ways:
# into the firmware as PcdRpi5FirmwareVersion (what FmpDeviceGetVersion
# reports) and into the capsule as GenerateCapsule's --fw-version, so the
# capsule always declares the version of the image inside it.
#
# It should only ever increase. What actually enforces that is RPI5_FMP_LSV,
# not this: FmpDxe rejects an image whose version is below the lowest
# supported version, and compares nothing against the running one.
RPI5_FMP_VERSION ??= "${@d.getVar('PV').split('+')[0].replace('.', '') or '1'}"

# Anti-rollback floor written into the capsule as GenerateCapsule's --lsv.
#
# STICKY, and in a way that cannot be undone from the board: on a successful
# apply FmpDxe copies the payload's LSV into a variable and thereafter takes
# the max of that, PcdFmpDeviceBuildTimeLowestSupportedVersion and the device
# library's value (FmpDxe.c, SetLowestSupportedVersionInVariable). Shipping a
# capsule with an LSV above a release you may need to go back to makes that
# release unreachable by capsule for the life of the variable store. Leave it
# at 0 unless you mean to burn a downgrade bridge.
RPI5_FMP_LSV ??= "0"

# Extra GenerateCapsule --capflag arguments. Empty on purpose.
#
# The obvious candidate, PersistAcrossReset, does not work on this platform:
# CapsuleRuntimeDxe returns EFI_UNSUPPORTED for it unless
# PcdSupportUpdateCapsuleReset is TRUE, and RPi5 neither sets that PCD nor
# builds the CapsulePei/CapsuleOnDisk machinery that would coalesce a staged
# capsule after the reset. A capsule with no flags is applied immediately, in
# the boot services call itself, which is the path that works here -- and so
# also the one an OS-side fwupd cannot use.
RPI5_FMP_CAPSULE_FLAGS ??= ""

# DER certificate FmpDxe authenticates capsule payloads against, embedded in
# the firmware as PcdFmpDevicePkcs7CertBufferXdr.
#
# Leave both this and RPI5_FMP_KEY unset to have the build generate a keypair
# (see RPI5_FMP_KEYDIR). Set this ALONE when the private key lives somewhere
# this build cannot reach -- an HSM, another machine, a release process --
# and do_deploy will build the firmware but skip the capsule, with a warning
# saying so, leaving you to sign one offline.
RPI5_FMP_CERT ??= ""

# PEM file holding the capsule signing private key AND its certificate,
# concatenated. Both, in one file: GenerateCapsule signs by shelling out to
# `openssl smime -sign -signer <file>` with no -inkey, so a bare private key
# is not enough. do_deploy checks the certificate in here against the one the
# firmware embeds and fails on a mismatch, because a capsule signed by the
# wrong key of a matched pair fails authentication silently on the board.
#
# If this is set and RPI5_FMP_CERT is not, the DER the firmware embeds is
# derived from this file's certificate rather than asked for twice.
RPI5_FMP_KEY ??= ""

# Where a generated keypair lives, when neither RPI5_FMP_CERT nor
# RPI5_FMP_KEY is set. Four files: capsule.key (private key), capsule.crt
# (PEM certificate), capsule.cer (the same certificate as DER, embedded in
# the firmware) and capsule.pem (key + certificate, what signs capsules).
#
# Under TOPDIR rather than WORKDIR because the keypair has to OUTLIVE the
# build: a capsule is authenticated by the firmware ALREADY ON THE BOARD, so
# a key regenerated between releases produces capsules that fielded boards
# reject. Nothing is ever regenerated over an existing file, and a
# half-populated directory is an error rather than a silent re-key -- but
# `rm -rf` the build directory and the next build makes a new identity that
# no deployed board will trust. Point this somewhere backed up, or move to
# RPI5_FMP_CERT + RPI5_FMP_KEY with real key management, before this leaves a
# workbench. The private key is written unencrypted and mode 0600.
RPI5_FMP_KEYDIR ??= "${TOPDIR}/fmp-keys"

# Subject and lifetime of a generated certificate. Long-dated on purpose:
# renewing means reflashing every board that embeds the old certificate.
RPI5_FMP_SUBJECT ??= "/CN=pi-bmc RPi5 UEFI capsule signing/"
RPI5_FMP_CERT_DAYS ??= "7300"

# Escape hatch for one-off `-D FOO=BAR` / `--pcd ...` additions without
# having to override do_compile wholesale.
RPI5_EDK2_EXTRA_FLAGS ??= ""

do_configure[noexec] = "1"

# The GUID below is only used to identify this one prebuilt-driver FFS file
# within RPi5.fdf; it has no meaning outside this build (freshly generated,
# not reused from anywhere else).
IPXE_DRIVER_FILE_GUID = "c3e36d1a-8f42-4b3e-9a5d-2f6c7b8e9a10"

# Generate the capsule signing keypair, once, if it is not already there.
#
# Deliberately all-or-nothing: a directory holding some of the four files is
# an error rather than something to top up. The four are one identity, and
# quietly re-deriving a missing piece is how you end up with a certificate in
# the firmware that no longer matches the key signing capsules for it -- a
# mismatch whose only symptom is that updates stop applying, on the board,
# with no message anyone sees.
#
# Shell locals go unbraced throughout these functions: bitbake expands ${...}
# in a task body against its own datastore before /bin/sh ever sees it, so a
# braced shell variable is one name collision away from being replaced with
# something else entirely.
rpi5_fmp_generate_keys() {
    fmp_keydir="${RPI5_FMP_KEYDIR}"

    # Counted with string accumulation rather than arithmetic: bitbake's shell
    # code parser raises NotImplementedError on $(( )) and the recipe will not
    # parse at all.
    fmp_present=""
    fmp_missing=""
    for f in capsule.key capsule.crt capsule.cer capsule.pem; do
        if [ -e "$fmp_keydir/$f" ]; then
            fmp_present="$fmp_present $f"
        else
            fmp_missing="$fmp_missing $f"
        fi
    done

    if [ -z "$fmp_missing" ]; then
        return 0
    fi

    if [ -n "$fmp_present" ]; then
        bbfatal "$fmp_keydir is a partial capsule signing key directory (has:$fmp_present, missing:$fmp_missing). Refusing to regenerate: the certificate and the key that signs capsules for it must stay one pair, and boards already carrying this certificate would reject capsules signed by a new one. Restore the missing files from wherever this keypair is kept, or move the directory aside to start a new identity -- and reflash every board that has the old one."
    fi

    mkdir -p "$fmp_keydir"

    # umask, not a later chmod: the private key must never exist, even for an
    # instant, at a mode another user on the build host could read.
    (umask 077 && openssl req -x509 -newkey rsa:2048 -nodes -sha256 \
        -days ${RPI5_FMP_CERT_DAYS} -subj "${RPI5_FMP_SUBJECT}" \
        -keyout "$fmp_keydir/capsule.key" -out "$fmp_keydir/capsule.crt") \
        || bbfatal "could not generate a capsule signing keypair in $fmp_keydir"

    # The DER form is what BinToPcd renders into the firmware; the PEM pair is
    # what "openssl smime -sign -signer" wants, key first.
    openssl x509 -in "$fmp_keydir/capsule.crt" -outform DER \
        -out "$fmp_keydir/capsule.cer" \
        || bbfatal "could not convert $fmp_keydir/capsule.crt to DER"
    (umask 077 && cat "$fmp_keydir/capsule.key" "$fmp_keydir/capsule.crt" \
        > "$fmp_keydir/capsule.pem") \
        || bbfatal "could not write $fmp_keydir/capsule.pem"
    chmod 0644 "$fmp_keydir/capsule.crt" "$fmp_keydir/capsule.cer"

    bbwarn "Generated a self-signed capsule signing keypair in $fmp_keydir. This is now the identity every board flashed with this firmware will trust for updates, and it is stored unencrypted in the build directory: back it up, and replace it with managed key material (RPI5_FMP_CERT + RPI5_FMP_KEY) before shipping. Losing it means no board flashed with this firmware can ever be capsule-updated again."
}

# Resolve the capsule certificate and signer into $fmp_cert and $fmp_signer
# for the caller. $fmp_signer comes back empty when the private key is
# deliberately elsewhere; that is the offline-signing case, not an error.
#
# Generation happens only when NEITHER was configured -- setting one of them
# means the operator has an identity of their own, and inventing a second one
# alongside it would be worse than useless.
rpi5_fmp_resolve_keys() {
    fmp_cert="${RPI5_FMP_CERT}"
    fmp_signer="${RPI5_FMP_KEY}"

    if [ -z "$fmp_cert" ] && [ -z "$fmp_signer" ]; then
        rpi5_fmp_generate_keys
        fmp_cert="${RPI5_FMP_KEYDIR}/capsule.cer"
        fmp_signer="${RPI5_FMP_KEYDIR}/capsule.pem"
    fi

    if [ -n "$fmp_signer" ] && [ ! -r "$fmp_signer" ]; then
        bbfatal "RPI5_FMP_KEY '$fmp_signer' is not readable."
    fi

    if [ -z "$fmp_cert" ]; then
        # A signer PEM alone is enough: the certificate the firmware embeds is
        # that PEM's certificate, so derive it instead of asking for the same
        # object twice and then having to check the two against each other.
        mkdir -p "${B}"
        fmp_cert="${B}/fmp-capsule-cert.der"
        openssl x509 -in "$fmp_signer" -outform DER -out "$fmp_cert" \
            || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no certificate. GenerateCapsule signs by running openssl smime -sign -signer <file> and passes no -inkey, so this file must contain the signing certificate as well as the private key -- concatenate them, key first."
    fi

    if [ ! -r "$fmp_cert" ]; then
        bbfatal "RPI5_FMP_CERT '$fmp_cert' is not readable."
    fi
}

do_compile() {
    cd ${S}

    # BaseTools are host-native; keep bitbake's cross toolchain env out of
    # their build, same as every other EDK2 recipe (see meta-nuc-bios's
    # edk2-uefipayload_2605.bb).
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
    oe_runmake -C BaseTools CC=gcc CXX=g++

    # WORKSPACE is the parent of both ${S} (this edk2 checkout, at
    # "${WORKSPACE}/git") and EDK2_PLATFORMS_PATH/EDK2_NON_OSI_PATH (fetched
    # as WORKDIR siblings, at "${WORKSPACE}/edk2-platforms" and
    # "${WORKSPACE}/edk2-non-osi") -- i.e. ${WORKDIR}, NOT ${S}. This matters:
    # RPi5.dsc/.fdf live in edk2-platforms, not in edk2 itself, so "-p
    # edk2-platforms/Platform/..." below only resolves if WORKSPACE is their
    # common parent, exactly mirroring upstream rpi5-uefi/build.sh's own
    # layout (WORKSPACE=repo root, with edk2/edk2-platforms/edk2-non-osi as
    # direct siblings under it).
    # Local out-of-tree EDK2 packages (RpiBmcPkg, Rp1GemPkg), staged into a
    # dedicated PACKAGES_PATH root so EDK2's path resolution sees exactly the
    # two package dirs and nothing else from WORKDIR.
    local_pkgs="${B}/edk2-local-pkgs"
    rm -rf "${local_pkgs}"
    mkdir -p "${local_pkgs}"
    cp -r "${WORKDIR}/RpiBmcPkg" "${WORKDIR}/Rp1GemPkg" "${WORKDIR}/RpiRedfishPkg" \
        "${WORKDIR}/RpiFmpPkg" "${local_pkgs}/"

    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${S}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}:${EDK2_REDFISH_CLIENT_PATH}:${local_pkgs}"
    export EDK_TOOLS_PATH="${S}/BaseTools"
    export CONF_PATH="${WORKDIR}/Conf"
    export PYTHON_COMMAND="python3"
    export PATH="${S}/BaseTools/BinWrappers/PosixLike:${PATH}"

    mkdir -p "${CONF_PATH}"
    for f in target tools_def build_rule; do
        [ -e "${CONF_PATH}/${f}.txt" ] || cp "${S}/BaseTools/Conf/${f}.template" "${CONF_PATH}/${f}.txt"
    done

    # Real AArch64 cross build (unlike BaseTools above): tools_def.txt's GCC
    # toolchain family invokes $(GCC_AARCH64_PREFIX)gcc/-ld/-objcopy directly,
    # not $CC, so leaving CC unset from the BaseTools step above is fine.
    export GCC_AARCH64_PREFIX="${TARGET_PREFIX}"

    # --- wire optional feature modules into RPi5.dsc / RPi5.fdf ---------
    # Sed insertions rather than patch files: RPi5.dsc/.fdf's exact upstream
    # whitespace/formatting isn't reliably knowable ahead of time, so all
    # insertions anchor on edk2's own well-known NetworkPkg include lines and
    # append pre-rendered snippet files with sed's "r" command. The \| |
    # address delimiters matter: the markers contain slashes.
    dsc="${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.dsc"
    fdf="${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.fdf"
    dsc_marker='!include NetworkPkg/Network.dsc.inc'
    fdf_marker='!include NetworkPkg/Network.fdf.inc'
    grep -qF "${dsc_marker}" "${dsc}" || \
        bbfatal "RPi5.dsc: '${dsc_marker}' not found -- edk2-platforms layout changed, update this recipe"
    grep -qF "${fdf_marker}" "${fdf}" || \
        bbfatal "RPi5.fdf: '${fdf_marker}' not found -- edk2-platforms layout changed, update this recipe"

    # BMC-integration driver set (local RpiBmcPkg source package).
    # Every insertion below is grep-guarded so a re-run of do_compile against
    # the persisted WORKDIR checkout stays idempotent -- duplicate includes
    # mean duplicate FFS files and a GenFv failure.
    if [ "${RPI5_BMC}" = "1" ]; then
        printf '%s\n' '!include RpiBmcPkg/RpiBmc.dsc.inc' > "${B}/rpibmc-dsc-line.inc"
        printf '%s\n' '!include RpiBmcPkg/RpiBmc.fdf.inc' > "${B}/rpibmc-fdf-line.inc"
        grep -qF 'RpiBmcPkg/RpiBmc.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rpibmc-dsc-line.inc" "${dsc}"
        grep -qF 'RpiBmcPkg/RpiBmc.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rpibmc-fdf-line.inc" "${fdf}"
    fi

    # Native RP1 GEM onboard-Ethernet SNP driver (local Rp1GemPkg).
    if [ "${RPI5_RP1_ETH}" = "1" ]; then
        printf '%s\n' '!include Rp1GemPkg/Rp1Gem.dsc.inc' > "${B}/rp1gem-dsc-line.inc"
        printf '%s\n' '!include Rp1GemPkg/Rp1Gem.fdf.inc' > "${B}/rp1gem-fdf-line.inc"
        grep -qF 'Rp1GemPkg/Rp1Gem.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rp1gem-dsc-line.inc" "${dsc}"
        grep -qF 'Rp1GemPkg/Rp1Gem.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rp1gem-fdf-line.inc" "${fdf}"
    fi

    # edk2's own USB CDC-ECM/NCM/RNDIS drivers (in-tree, unwired upstream).
    if [ "${RPI5_USBNET}" = "1" ]; then
        grep -qF 'UsbNetwork/NetworkCommon/NetworkCommon.inf' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${WORKDIR}/usbnet-dsc-snippet.inc" "${dsc}"
        grep -qF 'INF MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon/NetworkCommon.inf' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${WORKDIR}/usbnet-fdf-snippet.fdf.inc" "${fdf}"
    fi

    # ShellPkg's Dp profiling command. Unlike every other snippet here these
    # lines go in UNCONDITIONALLY -- they carry their own
    # "!if $(PROFILING_ENABLED) == TRUE" gate, and the macro below is what
    # actually turns Dp on or off. Inserting conditionally would be a one-way
    # door against the persisted WORKDIR checkout: nothing removes the lines
    # again when the knob goes back to 0.
    grep -qF 'DpDynamicCommand/DpDynamicCommand.inf' "${dsc}" || \
        sed -i "\|${dsc_marker}|r ${WORKDIR}/profiling-dsc-snippet.inc" "${dsc}"
    grep -qF 'INF ShellPkg/DynamicCommand/DpDynamicCommand/DpDynamicCommand.inf' "${fdf}" || \
        sed -i "\|${fdf_marker}|r ${WORKDIR}/profiling-fdf-snippet.fdf.inc" "${fdf}"

    if [ "${PROFILING_ENABLED}" = "1" ]; then
        profiling_define="-D PROFILING_ENABLED=TRUE"
        bbnote "profiling enabled: ShellPkg Dp command built into FVMAIN"
    else
        profiling_define="-D PROFILING_ENABLED=FALSE"
    fi

    # Redfish Host Interface stack over the BMC's CDC-NCM gadget (RedfishPkg
    # core drivers + local RpiRedfishPkg; the usbnet snippet above supplies
    # the NIC driver). The wire-contract knobs are rendered into the
    # local_pkgs COPY of RpiRedfish.dsc.inc (the pristine WORKDIR original
    # keeps its placeholders; local_pkgs is rebuilt from it on every
    # do_compile, so the render is idempotent). The dsc include opens its
    # own [LibraryClasses]/[PcdsFixedAtBuild] sections and reopens
    # [Components.common] at its end -- and this insertion runs LAST so that
    # block sits closest to the marker, leaving every earlier-inserted
    # component line after it, back inside [Components.common].
    redfish_client_define=""
    if [ "${RPI5_REDFISH}" = "1" ]; then
        [ "${RPI5_USBNET}" = "1" ] || \
            bbwarn "RPI5_REDFISH=1 without RPI5_USBNET=1: no NIC driver for the BMC link"
        mac_plain=$(printf '%s' "${RPI5_REDFISH_MAC}" | tr -d ':' | tr 'abcdef' 'ABCDEF')
        [ "$(printf '%s' "${mac_plain}" | wc -c)" = "12" ] || \
            bbfatal "RPI5_REDFISH_MAC '${RPI5_REDFISH_MAC}' is not a 6-octet MAC"
        mac_bytes=$(printf '%s' "${mac_plain}" | sed 's/../0x&, /g; s/, $//')
        sed -i \
            -e "s|@RPI5_REDFISH_MAC_PLAIN@|${mac_plain}|g" \
            -e "s|@RPI5_REDFISH_MAC_BYTES@|${mac_bytes}|g" \
            -e "s|@RPI5_REDFISH_USER@|${RPI5_REDFISH_USER}|g" \
            -e "s|@RPI5_REDFISH_PASSWORD@|${RPI5_REDFISH_PASSWORD}|g" \
            "${local_pkgs}/RpiRedfishPkg/RpiRedfish.dsc.inc"
        printf '%s\n' '!include RpiRedfishPkg/RpiRedfish.dsc.inc' > "${B}/rpiredfish-dsc-line.inc"
        printf '%s\n' '!include RpiRedfishPkg/RpiRedfish.fdf.inc' > "${B}/rpiredfish-fdf-line.inc"
        grep -qF 'RpiRedfishPkg/RpiRedfish.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rpiredfish-dsc-line.inc" "${dsc}"
        grep -qF 'RpiRedfishPkg/RpiRedfish.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rpiredfish-fdf-line.inc" "${fdf}"

        # edk2-redfish-client feature layer on top of the host interface.
        # Same include mechanics. The FV list comes from the client's own
        # RedfishClient.fdf.inc (via RpiRedfishClient.fdf.inc), which is
        # gated on REDFISH_CLIENT -- defined on the build command line
        # below. Its nested RedfishJsonStructureDxe.fdf.inc gates every
        # entry on per-schema macros left undefined here; those evaluate
        # false (with "Suspicious expression" parser warnings, which are
        # expected and harmless). The dsc-side component/library lists stay
        # explicit in RpiRedfishClient.dsc.inc -- see its header.
        if [ "${RPI5_REDFISH_CLIENT}" = "1" ]; then
            redfish_client_define="-D REDFISH_CLIENT=TRUE"
            printf '%s\n' '!include RpiRedfishPkg/RpiRedfishClient.dsc.inc' > "${B}/rpiredfishclient-dsc-line.inc"
            printf '%s\n' '!include RpiRedfishPkg/RpiRedfishClient.fdf.inc' > "${B}/rpiredfishclient-fdf-line.inc"
            grep -qF 'RpiRedfishPkg/RpiRedfishClient.dsc.inc' "${dsc}" || \
                sed -i "\|${dsc_marker}|r ${B}/rpiredfishclient-dsc-line.inc" "${dsc}"
            grep -qF 'RpiRedfishPkg/RpiRedfishClient.fdf.inc' "${fdf}" || \
                sed -i "\|${fdf_marker}|r ${B}/rpiredfishclient-fdf-line.inc" "${fdf}"
        fi
    fi

    # --- FMP capsule update ---------------------------------------------
    # FmpDxe + EsrtFmpDxe, plus the certificate their capsule authentication
    # checks against. The certificate is not optional: FmpDxe walks the keys in
    # PcdFmpDevicePkcs7CertBufferXdr and applies nothing if there are none, so
    # firmware built without one advertises an ESRT update path that can never
    # succeed. Hence rpi5_fmp_resolve_keys, which always yields a certificate
    # -- generating a keypair if the operator configured none -- or stops the
    # build. The matching capsule is built in do_deploy.
    if [ "${RPI5_FMP}" = "1" ]; then
        # Sets $fmp_cert (and $fmp_signer, which only do_deploy needs),
        # generating a keypair under RPI5_FMP_KEYDIR if none was configured.
        rpi5_fmp_resolve_keys

        # BinToPcd renders the DER certificate as the XDR-encoded VOID* PCD
        # FmpDevicePkg expects. Passing it through --pcd is not an option: it is
        # a multi-hundred-byte binary blob.
        cert_pcd="${B}/fmp-capsule-cert.pcd"
        python3 "${S}/BaseTools/Scripts/BinToPcd.py" \
            -i "$fmp_cert" -x -o "${cert_pcd}" \
            -p gFmpDevicePkgTokenSpaceGuid.PcdFmpDevicePkcs7CertBufferXdr

        printf '%s\n' '!include RpiFmpPkg/RpiFmp.dsc.inc' > "${B}/rpifmp-dsc-line.inc"
        printf '%s\n' '!include RpiFmpPkg/RpiFmp.fdf.inc' > "${B}/rpifmp-fdf-line.inc"

        grep -qF 'RpiFmpPkg/RpiFmp.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rpifmp-dsc-line.inc" "${dsc}"
        grep -qF 'RpiFmpPkg/RpiFmp.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rpifmp-fdf-line.inc" "${fdf}"

        # The certificate PCD goes at the END of the DSC, not at the marker the
        # other snippets use. BinToPcd emits a bare PCD assignment with no
        # section header, and the marker sits inside [Components.common] -- an
        # opened [PcdsFixedAtBuild] there would swallow every component after
        # it. Appending a fresh section at end of file cannot do that.
        #
        # The re-run guard matches THIS marker line, not the PCD name: patch
        # 0018 names PcdFmpDevicePkcs7CertBufferXdr in a comment it adds to
        # RPi5.dsc, so a grep for the PCD name matches that comment on a clean
        # tree and skips the append entirely -- producing firmware whose
        # certificate buffer is empty, which is to say firmware that accepts
        # no capsule ever, with nothing said about it at build time.
        cert_marker='# Capsule signing certificate, appended by the edk2-rpi5-firmware recipe.'
        grep -qF "${cert_marker}" "${dsc}" || {
            printf '\n#\n%s\n# FmpDxe authenticates every capsule payload against it.\n#\n[PcdsFixedAtBuild.common]\n' "${cert_marker}" >> "${dsc}"
            cat "${cert_pcd}" >> "${dsc}"
        }

        # Belt and braces, because the failure above is invisible from the
        # board: the assignment must be in the DSC and must carry bytes.
        grep -q 'PcdFmpDevicePkcs7CertBufferXdr|{0x' "${dsc}" || \
            bbfatal "the capsule signing certificate did not reach ${dsc}. Without it FmpDxe has no key to authenticate against and no capsule can ever be applied."

        fmp_pcds="--pcd gRpiFmpTokenSpaceGuid.PcdRpi5FirmwareVersion=${RPI5_FMP_VERSION}"
    else
        fmp_pcds=""
    fi

    # --- embed the iPXE UNDI/SNP driver, if built -----------------------
    # ipxe-efi's do_deploy publishes bin-arm64-efi/ipxe.efidrv to
    # DEPLOY_DIR_IMAGE; wire it into the DXE firmware volume as a prebuilt
    # driver FFS file, right after the point where RPi5.fdf pulls in edk2's
    # own NetworkPkg PXE/HTTP stack. Covers add-on PCIe/USB NICs iPXE
    # recognises; the onboard RJ45 is Rp1GemDxe's job (see RPI5_RP1_ETH).
    if [ "${RPI5_IPXE}" = "1" ]; then
        snippet="${B}/ipxe-fdf-snippet.fdf.inc"
        sed \
            -e "s|@IPXE_DRIVER_FILE_GUID@|${IPXE_DRIVER_FILE_GUID}|" \
            -e "s|@IPXE_EFIDRV_PATH@|${DEPLOY_DIR_IMAGE}/ipxe.efidrv|" \
            "${WORKDIR}/ipxe-fdf-snippet.fdf.inc" > "${snippet}"
        grep -qF "${IPXE_DRIVER_FILE_GUID}" "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${snippet}" "${fdf}"
    fi

    # --- Secure Boot -----------------------------------------------------
    # SECURE_BOOT_ENABLE swaps RPi5.dsc onto the real AuthVariableLib and
    # brings in SecureBootConfigDxe, SecureBootDefaultKeysDxe and
    # DxeImageVerificationLib. DEFAULT_KEYS then embeds the key set;
    # ArmPlatformPkg/SecureBootDefaultKeys.fdf.inc (already !included by
    # RPi5.fdf inside its SECURE_BOOT_ENABLE guard) reads these five macros.
    #
    # NOTE: an undefined macro in an FDF/DSC !if evaluates to 0 rather than
    # erroring, so a typo here does not fail the build -- it silently ships
    # firmware with empty key defaults. Verify "PK Default"/"KEK Default"/
    # "DB Default" appear in RPI_EFI.report.txt after building.
    secure_boot_define=""
    if [ "${RPI5_SECURE_BOOT}" = "1" ]; then
        secure_boot_define="-D SECURE_BOOT_ENABLE=TRUE"

        if [ "${RPI5_SECURE_BOOT_DEFAULT_KEYS}" = "1" ]; then
            # Two sources, on purpose. The PK is ours and ships in the layer
            # (only its public DER -- the private key lives outside this
            # tree entirely); Microsoft's CAs are fetched by the bitbake
            # fetcher into secureboot-certs/, checksum-pinned in SRC_URI.
            #
            # ${WORKDIR}, not ${UNPACKDIR}: this recipe already locates its
            # other file:// directories that way (see the RpiBmcPkg copy
            # above), so keep the one convention.
            pkdir="${WORKDIR}/secureboot-keys"
            certdir="${WORKDIR}/secureboot-certs"

            [ -f "${pkdir}/PkDefault.der" ] || \
                bbfatal "RPI5_SECURE_BOOT_DEFAULT_KEYS=1 but ${pkdir}/PkDefault.der is missing"
            for k in MicCorKEKCA2011.crt MicCorKEK2KCA2023.crt \
                     MicWinProPCA2011.crt MicCorUEFCA2011.crt \
                     WindowsUEFICA2023.crt MicrosoftUEFICA2023.crt; do
                [ -f "${certdir}/$k" ] || \
                    bbfatal "RPI5_SECURE_BOOT_DEFAULT_KEYS=1 but ${certdir}/$k was not fetched"
            done

            # Every one of these must be a DER X.509 cert with an RSA key:
            # SecureBootVariableProvisionLib runs RsaGetPublicKeyFromX509()
            # over each SECTION RAW. Catch a bad file here rather than
            # shipping firmware whose key defaults silently fail to load.
            for k in "${pkdir}/PkDefault.der" "${certdir}"/*.crt; do
                openssl x509 -inform DER -in "$k" -noout >/dev/null 2>&1 || \
                    bbfatal "$k is not a DER X.509 certificate"
            done

            secure_boot_define="${secure_boot_define} -D DEFAULT_KEYS=TRUE"
            secure_boot_define="${secure_boot_define} -D PK_DEFAULT_FILE=${pkdir}/PkDefault.der"
            # KEK: both Microsoft generations (2011 expired 2026-06-24).
            secure_boot_define="${secure_boot_define} -D KEK_DEFAULT_FILE1=${certdir}/MicCorKEKCA2011.crt"
            secure_boot_define="${secure_boot_define} -D KEK_DEFAULT_FILE2=${certdir}/MicCorKEK2KCA2023.crt"
            # db: Windows + third-party (shim) CAs, both generations. Drop
            # the two UEFI CAs if this board must never boot shim/Linux.
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE1=${certdir}/MicWinProPCA2011.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE2=${certdir}/MicCorUEFCA2011.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE3=${certdir}/WindowsUEFICA2023.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE4=${certdir}/MicrosoftUEFICA2023.crt"
        fi
    fi

    build \
        -a AARCH64 -t GCC -b ${RPI5_BUILD_TARGET} \
        -p edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.dsc \
        -n ${@oe.utils.cpu_count()} \
        -D TFA_BUILD_ARTIFACTS=${DEPLOY_DIR_IMAGE} \
        ${redfish_client_define} \
        ${profiling_define} \
        ${secure_boot_define} \
        --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString=L"${RPI5_FW_VERSION}" \
        ${fmp_pcds} \
        ${RPI5_EDK2_EXTRA_FLAGS} \
        -y ${B}/RPI_EFI.report.txt

    [ -f "${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" ] || \
        bbfatal "edk2 build produced no RPI_EFI.fd -- see ${B}/RPI_EFI.report.txt"

    # Getting the assignment into the DSC is necessary but not sufficient; what
    # decides whether updates work is the value the build actually resolved.
    # The report is the only place that shows it, and an empty buffer there is
    # firmware advertising an ESRT update path that can never succeed -- which
    # nothing on the board will ever tell anyone. Same reasoning as the
    # "PK Default"/"KEK Default" note in the Secure Boot block above, except
    # this one is cheap enough to check rather than leave to a human.
    if [ "${RPI5_FMP}" = "1" ]; then
        if grep -q 'PcdFmpDevicePkcs7CertBufferXdr.*= {0x0}' "${B}/RPI_EFI.report.txt"; then
            bbfatal "the build resolved PcdFmpDevicePkcs7CertBufferXdr to an empty buffer, so FmpDxe would have no certificate to authenticate capsules against and nothing could ever be applied. See ${B}/RPI_EFI.report.txt."
        fi
    fi
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd ${DEPLOYDIR}/RPI_EFI.fd
    # Same bytes under the BCM2712 default armstub filename: the VPU
    # bootloader auto-loads armstub8-2712.bin from the boot partition at
    # address 0x0 (= PcdFdBaseAddress), so config.txt needs no armstub=
    # line. RPI_EFI.fd is kept for anyone following the upstream
    # rpi5-uefi install flow with an explicit armstub= entry.
    install -m 0644 ${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd ${DEPLOYDIR}/armstub8-2712.bin
    install -m 0644 ${WORKDIR}/config.txt ${DEPLOYDIR}/config.txt
    install -m 0644 ${B}/RPI_EFI.report.txt ${DEPLOYDIR}/RPI_EFI.report.txt

    # --- signed capsule ---------------------------------------------------
    # The same FD, wrapped in the UEFI capsule headers and PKCS#7-signed with
    # the key whose certificate the firmware above embeds. What it updates is
    # the firmware region only: Rpi5FmpDeviceLib stops the write at
    # PcdNvStorageVariableBase, so the variable store, the Secure Boot keys
    # and every Setup choice survive being updated over.
    #
    # NOT byte-reproducible, and cannot be made so from here: GenerateCapsule
    # signs by calling `openssl smime -sign`, which stamps a signingTime
    # attribute into the PKCS#7 and offers no way to suppress it short of
    # -noattr, which the script never passes. Two builds of identical inputs
    # produce capsules that differ in those few bytes. Nothing downstream
    # cares -- the capsule is not packaged, and FmpAuthenticationLibPkcs7
    # ignores signingTime -- but a diff of two DEPLOYDIRs will show it.
    if [ "${RPI5_FMP}" = "1" ]; then
        rpi5_fmp_resolve_keys

        if [ -z "$fmp_signer" ]; then
            bbwarn "RPI5_FMP_CERT is set but RPI5_FMP_KEY is not, so no capsule was built -- the private key is not available to this build. Sign one offline against ${DEPLOYDIR}/RPI_EFI.fd with edk2's BaseTools/Source/Python/Capsule/GenerateCapsule.py; see files/RpiFmpPkg/README.md for the arguments, which must match this build's --fw-version ${RPI5_FMP_VERSION} and --lsv ${RPI5_FMP_LSV}."
        else
            # One source of truth for the image type GUID: read it back out of
            # the DSC snippet that put it in the firmware. Duplicating the
            # literal here is exactly the drift the snippet warns about -- a
            # capsule built under a different GUID names a device that is not
            # this one, and is refused with nothing to say why.
            fmp_guid=$(sed -n 's/.*PcdFmpDeviceImageTypeIdGuid[[:space:]]*|[[:space:]]*{GUID("\([0-9A-Fa-f-]\{36\}\)").*/\1/p' \
                "${WORKDIR}/RpiFmpPkg/RpiFmp.dsc.inc")
            if [ -z "$fmp_guid" ]; then
                bbfatal "could not read PcdFmpDeviceImageTypeIdGuid out of ${WORKDIR}/RpiFmpPkg/RpiFmp.dsc.inc -- if the PCD line was reformatted, fix this sed rather than hardcoding the GUID."
            fi

            # The signer's certificate must be the one the firmware trusts.
            # Two files that were never a pair produce a capsule that builds,
            # deploys and installs perfectly, then fails authentication on the
            # board with an error nobody is watching for.
            fmp_cert_pub=$(openssl x509 -inform DER -in "$fmp_cert" -noout -pubkey) \
                || bbfatal "'$fmp_cert' is not a DER X.509 certificate."
            fmp_signer_pub=$(openssl x509 -in "$fmp_signer" -noout -pubkey) \
                || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no certificate. It must contain the signing certificate as well as the private key -- concatenate them, key first."
            if [ "$fmp_cert_pub" != "$fmp_signer_pub" ]; then
                bbfatal "RPI5_FMP_KEY '$fmp_signer' does not hold the certificate in '$fmp_cert'. Capsules signed with that key would be rejected by this firmware."
            fi
            # -passin pass: turns an encrypted key into a failure here rather
            # than a prompt inside GenerateCapsule, which has no way to answer
            # one and would sit waiting on a terminal bitbake does not give it.
            openssl pkey -in "$fmp_signer" -passin pass: -noout >/dev/null 2>&1 \
                || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no usable private key -- either it has only the certificate, or the key is passphrase-encrypted, which openssl smime -sign cannot be given a passphrase for from here. Decrypt it into a build-local copy, or sign offline and set RPI5_FMP_CERT alone."

            # GenerateCapsule wants PEM for --other-public-cert and
            # --trusted-public-cert, and both are mandatory even for a
            # self-signed certificate that is its own chain and its own
            # anchor. Derive them from the DER the firmware embeds, so what
            # signs is checked against what verifies.
            fmp_cert_pem="${B}/fmp-capsule-cert.pem"
            openssl x509 -inform DER -in "$fmp_cert" -out "$fmp_cert_pem" \
                || bbfatal "could not convert '$fmp_cert' to PEM"

            # Run the script directly rather than through the BinWrappers
            # PosixLike wrapper: the wrapper is what sets PYTHONPATH, and it
            # is not on PATH here the way it is inside do_compile's build env.
            # --signing-tool-path pins openssl to the native sysroot's, the
            # one every check above used, instead of whatever is on PATH.
            PYTHONPATH="${S}/BaseTools/Source/Python" python3 \
                "${S}/BaseTools/Source/Python/Capsule/GenerateCapsule.py" -e \
                --guid "$fmp_guid" \
                --fw-version ${RPI5_FMP_VERSION} \
                --lsv ${RPI5_FMP_LSV} \
                ${RPI5_FMP_CAPSULE_FLAGS} \
                --signer-private-cert "$fmp_signer" \
                --other-public-cert "$fmp_cert_pem" \
                --trusted-public-cert "$fmp_cert_pem" \
                --signing-tool-path "${STAGING_BINDIR_NATIVE}" \
                -o "${DEPLOYDIR}/RPi5Firmware.cap" \
                "${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" \
                || bbfatal "GenerateCapsule failed to build ${DEPLOYDIR}/RPi5Firmware.cap"

            chmod 0644 "${DEPLOYDIR}/RPi5Firmware.cap"
            bbnote "Built ${DEPLOYDIR}/RPi5Firmware.cap: image type $fmp_guid, version ${RPI5_FMP_VERSION}, lsv ${RPI5_FMP_LSV}."
        fi
    fi
}

addtask deploy after do_compile

do_install[noexec] = "1"

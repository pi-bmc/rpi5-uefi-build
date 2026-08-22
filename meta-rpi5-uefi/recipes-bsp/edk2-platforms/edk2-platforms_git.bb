SUMMARY = "tianocore edk2-platforms with this layer's Raspberry Pi 5 port applied"
DESCRIPTION = "The edk2-platforms source tree that rpi5-uefi-firmware builds \
               Platform/RaspberryPi/RPi5/RPi5.dsc out of. Nothing is compiled here: \
               this recipe owns the fetch, the RPi5 port, the port's fixes and this \
               layer's own RP1 GEM Ethernet driver, and \
               stages the resulting tree into ${datadir}/edk2/edk2-platforms for the \
               firmware recipe to consume out of its sysroot. Split out of the \
               firmware build so a 96 MB fetch and a 30-patch series are \
               sstate-cached on their own, instead of being redone every time the \
               EDK2 build itself is cleaned."
HOMEPAGE = "https://github.com/tianocore/edk2-platforms"

# Identical License.txt to edk2's.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

PV = "202608+git${SRCPV}"

# The former NumberOneGit fork's edk2-platforms RPi5 port is decomposed into
# two pieces here: files/edk2-platforms/ (the port's ADDED files, 75 of them,
# copied in after unpack) plus 0000-edk2-platforms-RPi5-port.patch (its
# changes to files that exist upstream, 43 of them) -- reconstructed
# byte-identical to the fork within every package tree the RPi5 build
# compiles. The fork's bulk-sync churn in other vendors' trees (187 files
# never referenced by RPi5.dsc/.fdf) is deliberately dropped.
#
# files/edk2-platforms/ ALSO carries source the fork never had: every driver
# and library this project wrote for the Pi 5. It used to live in out-of-tree
# packages (RpiBmcPkg, RpiRedfishPkg, RpiFmpPkg, Rp1GemPkg) that the firmware
# recipe bolted on through a fifth PACKAGES_PATH root; it is now filed the way
# upstream files its own, so upstreaming any of it is a move rather than a
# rewrite:
#
#   Silicon/RaspberryPi/RpiSiliconPkg   RP1 southbridge silicon -- Rp1BusDxe,
#                                       and Library/Rp1GpioLib (the GPIO/PWM
#                                       block behind the PHY reset and the fan)
#   Silicon/Broadcom/Drivers/Net        Rp1GemDxe + Rp1GemPkg.dec, beside
#                                       upstream's BcmGenetDxe + BcmNet.dec --
#                                       the same driver-family shape, for the
#                                       Pi 5's onboard NIC instead of the Pi 4's
#   Platform/RaspberryPi/Drivers        SecureBootToggleDxe, and
#   Platform/RaspberryPi/Library        PlatformThemeLib -- board-independent,
#                                       so they sit with ConfigDxe and friends
#   Platform/RaspberryPi/RPi5/Drivers   the board's own: PowerButtonDxe,
#                                       ActiveCoolerDxe, FanConfigDxe,
#                                       BootloaderConfigDxe, RpiRedfishSyncDxe
#   Platform/RaspberryPi/RPi5/Library   RpiRedfishCredentialLib,
#                                       RpiRedfishHostInterfaceLib,
#                                       Rpi5FmpDeviceLib
#
# All of it is purely additive -- no fork-derived file is touched, so the
# byte-parity audit above still holds for everything it covered. RPi5.dsc and
# RPi5.fdf (also overlay files) list every one of these directly and
# unconditionally, exactly as RPi4.dsc/.fdf list theirs, and the .dec content
# the retired packages carried was folded into RpiSiliconPkg.dec and RPi5.dec.
# The firmware recipe therefore inserts nothing for any of them.
#
# Rp1GpioLib has both its consumers inside this tree -- Rp1GemDxe's PHY-reset
# line and ActiveCoolerDxe's fan PWM -- so RPi5.dsc maps it once globally
# rather than overriding it per component.
#
# Patch order matters and follows SRC_URI order: 0000 (the fork's changed
# files) first, then this layer's own 0001..0007, 0010, the ACPI pair
# 0011/0012, and 0013..0018 -- which edit files the merged tree adds.
#
#   0011 (an RP1 I2C1 device) and 0012 (an RP1 GEM Ethernet device) both
#   append to Rp1.asi, so they must stay in that order relative to each other.
#
#   0013 teaches VarBlockServiceDxe that this layer deploys the FD as
#   armstub8-2712.bin, not RPI_EFI.fd -- without it the driver finds no file
#   to write back to and NOTHING set in Setup survives a reboot. It is
#   therefore coupled to the mcopy target name in
#   recipes-bsp/images/rpi5-uefi-sdimg.bb: change one and you change the other.
#
#   0014 flips the SystemTableMode default from ACPI to Device Tree, in both
#   the PcdsDynamicHii default and the VFR's F9 "Restore Defaults" value --
#   the onboard NIC has no working driver path under ACPI (see the comment
#   0014 adds to RPi5.dsc), and a node that cannot reach the network is no use
#   to the BMC.
#
#   0015 makes FdtDxe publish EFI_DT_FIXUP_PROTOCOL, so systemd-boot can hand
#   us the device tree out of the Talos UKI -- matched to the kernel in the
#   same image -- and we write the board-specific values into IT, rather than
#   shipping a tree of our own and hoping it fits a kernel we do not ship.
#   Same split u-boot runs (its EFI_DT_APPLY_FIXUPS path calls ft_board_setup).
#   The firmware's own tree stays installed as a fallback for anything that
#   brings none. Paired with 0014: in ACPI mode FdtDxe never runs at all.
#
#   0017 completes that: at ReadyToBoot, FdtDxe looks for a tree under \dtb\
#   on any attached filesystem and hands THAT to the OS. The device tree then
#   lives with the OS install rather than on the firmware's card, so one
#   firmware image boots any kernel and changing OS never means reflashing the
#   board. The SD-card tree (see talos-boot-dtbs) stays as the fallback for an
#   OS that ships none.
#
#   0020 tells it WHICH tree, off the board revision code rather than one
#   fixed name: Pi 5 rev 1.1 is BCM2712 D0, whose pinctrl offsets are not
#   C0's, and mainline calls that tree bcm2712-d-rpi-5-b.dtb. Same table
#   u-boot drives its `fdtfile` from (board/raspberrypi/rpi/rpi.c), added to
#   BoardRevisionHelperLib beside BoardRevisionGetModelName. talos-boot-dtbs
#   already deploys both trees under by-uname/<release>/ -- until this, the D0
#   one was sitting there unasked-for while D0 boards booted the C0 tree.
#   Depends on 0017, which is where the lookup lives.
#
#   0016 clears the RP1 MSIX_CFG routing 0001 arms for the two xHCIs, at
#   ExitBootServices. Linux's drivers/misc/rp1/rp1_pci.c owns those same
#   registers but only ever clears ENABLE per-IRQ as its own IRQ domain tears
#   one down -- it never zeroes the block on probe -- so anything left armed
#   stays armed behind the OS's back. Depends on 0001, which is where the
#   arming lives.
#
#   The DWC2 OTG host on the USB-C data port is 0006 (put the driver on the
#   BCM2712 core: 64-bit base PCD, forced host mode), 0007 (make an absent
#   core cheap to discover) and 0010 (fix the reset ordering and the
#   mode-switch wait); they must stay in that order. Numbering is sparse:
#   0008/0009 were a SET_DOMAIN_STATE mailbox experiment that no longer exists
#   -- Linux powers USB through the OLD SET_POWER_STATE tag with device id 3,
#   which is the call 0006 already makes.
#
#   0018 turns on the FMP/ESRT capsule path Rpi5FmpDeviceLib backs.
#
#   0019 declares the Secure Boot toggle's HII formset GUID in
#   RaspberryPi.dec. It exists only because that .dec is an upstream file --
#   the driver it belongs to, and everything else this layer adds, lives in
#   the overlay instead of in a patch.
#
# ORDERING IS LOAD-BEARING: do_unpack processes SRC_URI entries in listing
# order, and the git fetcher PRUNES its destsuffix dir before checkout -- the
# file://edk2-platforms entry must therefore stay AFTER the git entry of the
# same name, or the checkout wipes the port's added files (the patch series
# then fails loudly at 0000/0001).
SRC_URI = "git://github.com/tianocore/edk2-platforms.git;protocol=https;branch=master;destsuffix=edk2-platforms \
           file://edk2-platforms \
           file://0000-edk2-platforms-RPi5-port.patch \
           file://0001-Rp1BusDxe-register-GEM-and-I2C1-vendor-devices.patch \
           file://0002-PlatformSmbiosDxe-deterministic-UUID-and-Type45.patch \
           file://0003-RPi5-AcpiTables-add-SsdtThermal.patch \
           file://0004-PlatformBm-return-boot-option-number-not-list-index.patch \
           file://0005-PlatformBm-prune-USB-NIC-network-boot-options.patch \
           file://0006-DwUsbHostDxe-support-the-BCM2712-DWC2-OTG-Pi-5-USB-C.patch \
           file://0007-DwUsbHostDxe-fail-fast-when-the-DWC2-core-is-absent.patch \
           file://0010-DwUsbHostDxe-fix-the-BCM2712-core-reset-and-mode-swit.patch \
           file://0011-Silicon-RP1-add-an-ACPI-I2C1-device.patch \
           file://0012-Silicon-RP1-add-an-ACPI-GEM-Ethernet-device.patch \
           file://0013-VarBlockServiceDxe-find-the-variable-store-under-eith.patch \
           file://0014-RPi5-default-SystemTableMode-to-Device-Tree.patch \
           file://0015-FdtDxe-publish-EFI_DT_FIXUP_PROTOCOL.patch \
           file://0016-Rp1BusDxe-disarm-RP1-interrupt-routing-at-handoff.patch \
           file://0017-FdtDxe-load-the-OS-provided-device-tree-from-its-own-.patch \
           file://0018-RPi5-enable-FMP-capsule-processing.patch \
           file://0019-RaspberryPi-declare-the-Secure-Boot-toggle-formset-GU.patch \
           file://0020-FdtDxe-pick-the-OS-devicetree-by-board-revision.patch \
           file://0022-DwUsbHostDxe-reset-the-core-before-programming-GUSBCF.patch \
           file://0023-DwUsbHostDxe-select-UTMI-not-ULPI.patch \
           file://0024-DwUsbHostDxe-match-the-reference-kernel-GUSBCFG.patch \
           file://0026-DwUsbHostDxe-stop-truncating-addresses-in-Wait4Bit.patch \
           file://0028-RpiFirmwareDxe-power-state-reply-is-advisory.patch \
           file://0029-DwUsbHostDxe-report-the-real-port-speed.patch \
           file://0030-DwUsbHostDxe-do-not-reject-re-arming-an-async-interr.patch \
           file://0031-DwUsbHostDxe-bound-the-deferred-transfer-timeout.patch \
           file://0032-DwUsbHostDxe-optimize-the-transfer-hot-path.patch \
           "

# Diagnostics, DEBUG builds only. 0033 counts transfers, halt reasons and
# time spent inside DwUsbHostDxe, and reports a line every five seconds. The
# accounting itself is cheap, but it reads the performance counter twice per
# transfer on a path that runs at TPL_NOTIFY, and a production image has no
# reader for the numbers. It applies on top of 0032 and must stay last.
SRC_URI += "${@bb.utils.contains('RPI5_BUILD_TARGET', 'DEBUG', 'file://0033-DIAG-DwUsbHostDxe-account-for-time-spent-in-the-driv.patch', '', d)}"

# Upstream master, 2026-08-21. Moved here from the retired NumberOneGit fork's
# merge-base (80ee8b861, 2024-03-13) when the edk2 recipe took
# edk2-stable202608: a 2024 platform tree cannot build against a 2026 edk2, and
# upstream had already made the adaptations by hand -- RPi4.dsc carries the
# same ArmLib/CpuExceptionHandlerLib/GptLib mappings and the same
# ACPI_NULL_GAS rename this layer needed for RPi5.
#
# Upstream has never carried an RPi5 platform (not at either pin), so the RPi5
# port stays entirely this layer's, in files/edk2-platforms/ + 0000. What the
# move buys is the SHARED Platform/RaspberryPi family code -- ConfigDxe,
# FdtDxe, MmcDxe, RpiFirmwareDxe, PlatformBootManagerLib, the common
# AcpiTables -- maintained against current edk2 by people who test it, and
# RPi3/RPi4 as a worked reference for the next edk2 bump.
SRCREV = "9ef9bcef5090effeb569f61c8585795fdb41d41d"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

# Both the git checkout and the file://edk2-platforms overlay land here; the
# overlay merges into the checkout (bitbake's file:// fetcher copies a
# directory into an existing one of the same name rather than replacing it).
S = "${UNPACKDIR}/edk2-platforms"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach edk2's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. edk2 reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-platforms
    cp -a ${S}/. ${D}${EDK2_SOURCE_ROOT}/edk2-platforms/

    # Build bookkeeping rather than source: the git fetcher's checkout
    # metadata, and quilt's .pc/ backups plus the "patches" symlink it points
    # at ${WORKDIR}/patches (which would stage as a dangling link).
    rm -rf ${D}${EDK2_SOURCE_ROOT}/edk2-platforms/.git \
           ${D}${EDK2_SOURCE_ROOT}/edk2-platforms/.pc \
           ${D}${EDK2_SOURCE_ROOT}/edk2-platforms/patches
}

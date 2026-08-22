SUMMARY = "tianocore edk2-platforms with this layer's Raspberry Pi 5 port applied"
DESCRIPTION = "The edk2-platforms source tree that edk2-rpi5-firmware builds \
               Platform/RaspberryPi/RPi5/RPi5.dsc out of. Nothing is compiled here: \
               this recipe owns the fetch, the RPi5 port, the port's fixes and this \
               layer's own RP1 GEM Ethernet driver, and \
               stages the resulting tree into ${datadir}/edk2/edk2-platforms for the \
               firmware recipe to consume out of its sysroot. Split out of \
               edk2-rpi5-firmware so a 96 MB fetch and a 17-patch series are \
               sstate-cached on their own, instead of being redone every time the \
               EDK2 build itself is cleaned."
HOMEPAGE = "https://github.com/tianocore/edk2-platforms"

# Identical License.txt to edk2's.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

PV = "202403+git${SRCPV}"

# The former NumberOneGit fork's edk2-platforms RPi5 port is decomposed into
# two pieces here: files/edk2-platforms/ (the port's ADDED files, 75 of them,
# copied in after unpack) plus 0000-edk2-platforms-RPi5-port.patch (its
# changes to files that exist upstream, 43 of them) -- reconstructed
# byte-identical to the fork within every package tree the RPi5 build
# compiles. The fork's bulk-sync churn in other vendors' trees (187 files
# never referenced by RPi5.dsc/.fdf) is deliberately dropped.
#
# files/edk2-platforms/ ALSO carries source the fork never had: this layer's
# own Rp1GemDxe, at Silicon/Broadcom/Drivers/Net/. That addition is purely
# additive -- it touches no fork-derived file, so the byte-parity above still
# holds for everything the audit covered -- and it sits where it does because
# upstream already ships the RPi4 equivalent one directory over: BcmNet.dec
# beside BcmGenetDxe/, wired into RPi4.dsc/.fdf right after the NetworkPkg
# includes. Rp1GemPkg.dec + Rp1GemDxe/ mirror that shape exactly, down to the
# component block's scoped DMA PCDs, so upstreaming the driver is a move
# rather than a rewrite. RPi5.dsc/.fdf (also overlay files) carry the component
# and INF entries inline and unconditionally, in the same place and the same
# form RPi4 carries GENET's -- the firmware recipe inserts nothing for it and
# has no knob for it: the onboard NIC is not optional on this board.
#
# Rp1GpioLib (Phy.c's PHY-reset line) still resolves out of RpiBmcPkg, which
# the firmware recipe supplies as its own PACKAGES_PATH root -- so this driver
# does depend on one package it does not ship with. Moving that library into
# RpiSiliconPkg would point the dependency the right way round; not done yet.
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
#   0017 completes that: at ReadyToBoot, FdtDxe looks for
#   \dtb\bcm2712-rpi-5-b.dtb on any attached filesystem and hands THAT to the
#   OS. The device tree then lives with the OS install rather than on the
#   firmware's card, so one firmware image boots any kernel and changing OS
#   never means reflashing the board. The SD-card tree (see talos-boot-dtbs)
#   stays as the fallback for an OS that ships none.
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
#   0018 turns on the FMP/ESRT capsule path RpiFmpPkg supplies the device
#   library for; it is inert unless edk2-rpi5-firmware's RPI5_FMP is set,
#   which is what !includes RpiFmp.dsc.inc/.fdf.inc.
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
           "

# The fork's merge-base with upstream master (2024-03-13); its 32-commit RPi5
# port on top of it is what files/edk2-platforms/ + 0000 reconstruct. Audited
# 2026-08-17 with git merge-base + reconstruction diffs.
SRCREV = "80ee8b861edb6a8b02a100f63bbb435499f8741a"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

# Both the git checkout and the file://edk2-platforms overlay land here; the
# overlay merges into the checkout (bitbake's file:// fetcher copies a
# directory into an existing one of the same name rather than replacing it).
S = "${UNPACKDIR}/edk2-platforms"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach edk2-rpi5-firmware's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. edk2-rpi5-firmware reads exactly this
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

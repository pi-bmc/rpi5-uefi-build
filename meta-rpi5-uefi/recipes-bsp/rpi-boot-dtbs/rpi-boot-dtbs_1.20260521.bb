SUMMARY = "Raspberry Pi 5 (BCM2712) device trees and overlays from the boot firmware release"
DESCRIPTION = "Deploys the bcm2712*.dtb device trees and the full overlays/ \
               directory (including overlay_map.dtb) from a pinned \
               raspberrypi/firmware release, for inclusion on the UEFI SD \
               image's boot partition. The Pi 5 VPU bootloader requires the \
               board's .dtb next to config.txt (it loads and patches it to \
               device_tree_address before starting the armstub), and \
               dtoverlay= lines in config.txt resolve against overlays/. \
               Only the DT artifacts are taken; the start*.elf-era GPU \
               firmware in the same release does not exist on Pi 5 (its \
               equivalent lives in the board's boot EEPROM)."
HOMEPAGE = "https://github.com/raspberrypi/firmware"

# The DTBs/overlays are built from the Raspberry Pi kernel sources (GPL-2.0);
# LICENCE.broadcom in the same boot/ directory covers the GPU blobs we do NOT
# ship, but it is the only license text the release carries, so it is what
# LIC_FILES_CHKSUM can point at.
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://boot/LICENCE.broadcom;md5=c403841ff2837657b2ed8e5bb474ac8d"

SRC_URI = "https://github.com/raspberrypi/firmware/archive/refs/tags/${PV}.tar.gz;downloadfilename=firmware-${PV}.tar.gz"
SRC_URI[sha256sum] = "b900de58571920a306ab2d1296500499d0744451dbefda0c76b79e652bb71cb7"

S = "${UNPACKDIR}/firmware-${PV}"
UNPACKDIR ?= "${WORKDIR}"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

# Prebuilt artifacts; nothing to configure/compile/package.
do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy() {
    install -d ${DEPLOYDIR}/rpi-boot-dtbs/overlays
    install -m 0644 ${S}/boot/bcm2712*.dtb ${DEPLOYDIR}/rpi-boot-dtbs/
    install -m 0644 ${S}/boot/overlays/* ${DEPLOYDIR}/rpi-boot-dtbs/overlays/
}

addtask deploy after do_unpack

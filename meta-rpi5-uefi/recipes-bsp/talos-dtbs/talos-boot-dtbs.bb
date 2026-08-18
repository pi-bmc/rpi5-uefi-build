SUMMARY = "Broadcom device trees from the Talos kernel OCI image"
DESCRIPTION = "Extracts the bcm2712 device trees from ghcr.io/siderolabs/kernel \
so the DTB the VPU bootloader loads is the one the kernel Talos actually runs \
was built against, and bakes in the nodes that DTB omits. \
\
This exists because the device tree has to match the kernel, and this repo does \
not build that kernel. Sourcing it from the kernel's own image is the only way \
to keep the two in step without pinning a DT into firmware and hoping it fits -- \
see the header of rpi5-uefi-sdimg.bb for how the pieces land on the card. \
\
Adapted from the talos-dtbs recipe in ../nanokvm-build, which does the same job \
for the u-boot build. The uefi-eeprom overlay is deliberately not carried over: \
that one wires up an I2C EEPROM as U-Boot's UEFI variable store, and this \
firmware keeps its variables in the FD file instead (see the edk2 recipe's \
patch 0013)."
HOMEPAGE = "https://github.com/siderolabs/kernel"

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# Talos kernel image. The tags here are kernel-build tags, not Talos release
# tags, so they need looking up rather than deriving:
#
#   crane ls ghcr.io/siderolabs/kernel | grep '^v1.13'
#   crane export --platform linux/arm64 ghcr.io/siderolabs/kernel:<tag> - \
#     | tar -x -C /tmp boot/vmlinuz     # then read its version banner
#
# v1.13.0-60-gf541ca4 carries 6.18.44-talos; the metal-arm64 v1.13.8 ISO ships
# 6.18.42-talos. Same lineage, and the RP1 binding is identical across it -- but
# keep this in step with the Talos release the nodes actually run, because the
# DTB and the kernel that consumes it come out of this one image together.
TALOS_KERNEL_IMAGE ?= "ghcr.io/siderolabs/kernel"
TALOS_KERNEL_TAG ?= "v1.13.0-60-gf541ca4"

# Nodes the stripped mainline DTB omits. Merged at build time rather than
# shipped as runtime .dtbo files -- see each .dts header for why that ordering
# matters (blconfig in particular is unpatchable if it is not already present
# when the VPU firmware's fixup runs).
SRC_URI = " \
    file://fixup-blconfig-overlay.dts \
    file://bcm2712-thermal-overlay.dts \
"

DEPENDS = "dtc-native crane-native"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy nopackages

do_configure[noexec] = "1"
do_install[noexec] = "1"

# crane pulls the image rather than the bitbake fetcher, so this task needs the
# network. Unlike ../nanokvm-build's talos-dtbs, crane comes from crane-native
# rather than the build host: a firmware build that silently depends on what
# somebody happened to "go install" is not reproducible, and fails differently
# on a CI runner than on a laptop.
do_compile[network] = "1"
do_compile() {
    if ! command -v crane >/dev/null 2>&1; then
        bbfatal "crane not on PATH despite crane-native in DEPENDS -- the native sysroot did not get staged."
    fi

    rm -rf ${B}/dtbs
    mkdir -p ${B}/dtbs

    crane export --platform linux/arm64 \
        ${TALOS_KERNEL_IMAGE}:${TALOS_KERNEL_TAG} \
        ${B}/talos-kernel.tar

    tar -xf ${B}/talos-kernel.tar --strip-components=2 -C ${B}/dtbs \
        $(tar -tf ${B}/talos-kernel.tar | grep -E '^dtb/broadcom/bcm2712[^/]+\.dtb$')
    rm -f ${B}/talos-kernel.tar

    if [ ! -f ${B}/dtbs/bcm2712-rpi-5-b.dtb ]; then
        bbfatal "bcm2712-rpi-5-b.dtb not extracted from ${TALOS_KERNEL_IMAGE}:${TALOS_KERNEL_TAG}"
    fi

    OVERLAYS="fixup-blconfig-overlay bcm2712-thermal-overlay"
    dtbos=""
    for ovl in ${OVERLAYS}; do
        dtc -O dtb -@ -H epapr -o ${B}/${ovl}.dtbo ${WORKDIR}/${ovl}.dts
        dtbos="${dtbos} ${B}/${ovl}.dtbo"
    done

    # The image ships exactly three bcm2712 trees: bcm2712-rpi-5-b.dtb,
    # bcm2712-d-rpi-5-b.dtb, and bcm2712-rpi-5-b-ovl-rp1.dtb.
    #
    # The -ovl-rp1 one is deliberately left alone AND never handed to the OS: it
    # is the variant whose pcie@1000120000 is bare, with RP1 supplied by the
    # kernel's own built-in dtso at runtime. That sounds like exactly what we
    # want -- RP1 travelling with the kernel -- but the kernel's dtso declares
    # rp1_eth with status = "disabled" and no phy-mode, phy-handle or
    # reset-gpios, because those are board facts that live in the board file.
    # Booting it means no NIC at all. The full tree carries them.
    for base in bcm2712-rpi-5-b bcm2712-d-rpi-5-b; do
        dtb=${B}/dtbs/${base}.dtb
        [ -f "${dtb}" ] || continue
        fdtoverlay -i "${dtb}" -o "${dtb}.new" ${dtbos}
        mv "${dtb}.new" "${dtb}"
        bbnote "talos-boot-dtbs: baked overlays (${OVERLAYS}) into ${base}.dtb"
    done
}

do_deploy() {
    install -d ${DEPLOYDIR}/talos-boot-dtbs
    install -m 0644 ${B}/dtbs/*.dtb ${DEPLOYDIR}/talos-boot-dtbs/
}
addtask deploy after do_compile before do_build

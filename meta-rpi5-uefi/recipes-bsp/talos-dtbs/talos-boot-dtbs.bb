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
               Trees are deployed twice: once flat, for the VPU bootloader, and once under \
               by-uname/<kernel release>/ for FdtDxe to pick from at boot. The second copy is \
               what makes a Talos A/B upgrade safe -- see TALOS_KERNEL_TAGS below. \
\
               Adapted from the talos-dtbs recipe in ../nanokvm-build, which does the same job \
               for the u-boot build. The uefi-eeprom overlay is deliberately not carried over: \
               that one wires up an I2C EEPROM as U-Boot's UEFI variable store, and this \
               firmware keeps its variables in the FD file instead (see the edk2-platforms \
               recipe's patch 0013)."
HOMEPAGE = "https://github.com/siderolabs/kernel"

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# Talos kernel image. The tags here are kernel-build tags, not Talos release
# tags, so they need looking up rather than deriving:
#
#   crane ls ghcr.io/siderolabs/kernel | grep '^v1.13'
#   crane export --platform linux/arm64 ghcr.io/siderolabs/kernel:<tag> - \
#     | tar -t | grep '^usr/lib/modules/'    # the kernel release
#
# v1.13.0-60-gf541ca4 carries 6.18.44-talos; the metal-arm64 v1.13.8 ISO ships
# 6.18.42-talos. Same lineage, and the RP1 binding is identical across it -- but
# keep this in step with the Talos release the nodes actually run, because the
# DTB and the kernel that consumes it come out of this one image together.
TALOS_KERNEL_IMAGE ?= "ghcr.io/siderolabs/kernel"
TALOS_KERNEL_TAG ?= "v1.13.0-60-gf541ca4"

# Every kernel whose trees should end up on the card, of which TALOS_KERNEL_TAG
# is one. Only that one supplies the flat trees the VPU bootloader loads; the
# rest exist purely so that FdtDxe can hand the right one to whichever kernel
# is actually booting.
#
# One entry is enough right up until an upgrade, which is exactly when it stops
# being enough: Talos installs the new UKI alongside the old and falls back to
# it if the new one does not come up. With trees for one kernel on the card,
# that fallback boots the old kernel against the new kernel's tree -- the
# mismatch this recipe exists to prevent, arriving by the back door. List the
# outgoing tag alongside the incoming one for as long as the rollback is real.
TALOS_KERNEL_TAGS ?= "${TALOS_KERNEL_TAG}"

# Nodes the stripped mainline DTB omits. Merged at build time rather than
# shipped as runtime .dtbo files -- see each .dts header for why that ordering
# matters (blconfig in particular is unpatchable if it is not already present
# when the VPU firmware's fixup runs).
SRC_URI = "\
    file://bcm2712-blconfig-overlay.dts \
    file://bcm2712-scmi-overlay.dts \
    file://bcm2712-boot-spi-overlay.dts \
    file://bcm2712-dwc2-usb-overlay.dts \
    file://bcm2712-serial0-overlay.dts \
"

DEPENDS = "dtc-native crane-native"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy nopackages

do_configure[noexec] = "1"
do_install[noexec] = "1"

# The overlays baked into every tree below.
# The two bcm2712-* additions below are not features: they put back nodes
# mainline omits that the VPU BOOTLOADER's own overlays reference by symbol.
# A symbol it cannot resolve makes it drop the whole overlay, so their
# absence silently cost this board the D0 stepping adaptations
# (bcm2712d0.dtbo, via spi10 and dma40) and USB-C host mode (dwc2.dtbo, via
# usb). Order is not load-bearing between them -- each targets a different
# path, and the one cross-reference (spi10 -> dma40) sits inside a single
# overlay where dtc resolves it through __local_fixups__. See each .dts
# header for the evidence.
TALOS_DTB_OVERLAYS = "bcm2712-blconfig-overlay bcm2712-scmi-overlay \
                      bcm2712-boot-spi-overlay bcm2712-dwc2-usb-overlay \
                      bcm2712-serial0-overlay"

# The trees taken out of each image. The Talos kernel ships three bcm2712 ones;
# bcm2712-rpi-5-b-ovl-rp1.dtb is deliberately not among these two.
#
# That one is the variant whose pcie@1000120000 is bare, with RP1 supplied by
# the kernel's own built-in dtso at runtime. That sounds like exactly what we
# want -- RP1 travelling with the kernel -- but the kernel's dtso declares
# rp1_eth with status = "disabled" and no phy-mode, phy-handle or reset-gpios,
# because those are board facts that live in the board file. Booting it means
# no NIC at all. The full tree carries them.
TALOS_DTB_TREES = "bcm2712-rpi-5-b bcm2712-d-rpi-5-b"

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
    mkdir -p ${B}/dtbs/by-uname

    # Build the overlays once; they are board facts, not kernel facts, and
    # apply to every tree here.
    dtbos=""
    for ovl in ${TALOS_DTB_OVERLAYS}; do
        dtc -O dtb -@ -H epapr -o ${B}/${ovl}.dtbo ${WORKDIR}/${ovl}.dts
        dtbos="${dtbos} ${B}/${ovl}.dtbo"
    done

    for tag in ${TALOS_KERNEL_TAGS}; do
        rm -rf ${B}/stage
        mkdir -p ${B}/stage

        crane export --platform linux/arm64 \
            ${TALOS_KERNEL_IMAGE}:${tag} \
            ${B}/talos-kernel.tar

        # The kernel release, straight off the modules directory. That name is
        # `uname -r` by construction -- it is what the kernel's own modules are
        # keyed by -- which makes it the same string the UKI puts in .uname and
        # therefore the same one FdtDxe builds its lookup path from. Reading it
        # out of the kernel binary would mean parsing a version banner; this is
        # the kernel telling us directly.
        release=$(tar -tf ${B}/talos-kernel.tar \
            | sed -n 's|^\(usr/\)\{0,1\}lib/modules/\([^/]\{1,\}\).*|\2|p' \
            | head -1)
        if [ -z "${release}" ]; then
            bbfatal "no lib/modules/<release> in ${TALOS_KERNEL_IMAGE}:${tag} -- cannot tell what kernel these trees belong to"
        fi

        tar -xf ${B}/talos-kernel.tar --strip-components=2 -C ${B}/stage \
            $(tar -tf ${B}/talos-kernel.tar | grep -E '^dtb/broadcom/bcm2712[^/]+\.dtb$')
        rm -f ${B}/talos-kernel.tar

        if [ ! -f ${B}/stage/bcm2712-rpi-5-b.dtb ]; then
            bbfatal "bcm2712-rpi-5-b.dtb not extracted from ${TALOS_KERNEL_IMAGE}:${tag}"
        fi

        mkdir -p ${B}/dtbs/by-uname/${release}
        for base in ${TALOS_DTB_TREES}; do
            dtb=${B}/stage/${base}.dtb
            [ -f "${dtb}" ] || continue
            fdtoverlay -i "${dtb}" -o "${dtb}.new" ${dtbos}
            mv "${dtb}.new" ${B}/dtbs/by-uname/${release}/${base}.dtb
        done
        bbnote "talos-boot-dtbs: ${tag} is ${release}; baked overlays (${TALOS_DTB_OVERLAYS}) into ${TALOS_DTB_TREES}"

        # The one kernel whose tree the VPU bootloader itself loads, from the
        # root of the boot partition. Everything else is reachable only through
        # FdtDxe's by-uname lookup.
        #
        # The BASE tree only, deliberately -- not the D0 one beside it. The VPU
        # bootloader's own flow is base tree plus a stepping overlay: on D0
        # silicon it loads this tree and then applies bcm2712d0.dtbo from
        # overlays/, which is Raspberry Pi's description of their own part.
        # Leaving a prebuilt D0 tree at the root only invites it to be loaded
        # as the base and then patched a second time.
        #
        # That flow is why bcm2712-boot-spi-overlay.dts exists: the stepping
        # overlay resolves &spi10 and &dma40 through this tree's __symbols__,
        # and one unresolved symbol makes the bootloader drop the whole overlay
        # ("dterror: can't find symbol 'spi10'"), leaving D0 silicon running a
        # C0 description.
        #
        # The result is closer to the part than the mainline D0 tree is: base +
        # bcm2712d0.dtbo differs from bcm2712-d-rpi-5-b.dtb in 7 lines, and in
        # the substantive ones -- main GIO bank widths <0x20 0x04> rather than
        # <0x20 0x16>, HDMI DMA channels present -- the overlay is the side
        # with Raspberry Pi's numbers.
        if [ "${tag}" = "${TALOS_KERNEL_TAG}" ]; then
            cp ${B}/dtbs/by-uname/${release}/bcm2712-rpi-5-b.dtb ${B}/dtbs/
        fi
    done

    rm -rf ${B}/stage

    if [ ! -f ${B}/dtbs/bcm2712-rpi-5-b.dtb ]; then
        bbfatal "TALOS_KERNEL_TAG (${TALOS_KERNEL_TAG}) is not in TALOS_KERNEL_TAGS (${TALOS_KERNEL_TAGS}) -- nothing to give the VPU bootloader"
    fi
}

do_deploy() {
    install -d ${DEPLOYDIR}/talos-boot-dtbs
    cp -a ${B}/dtbs/. ${DEPLOYDIR}/talos-boot-dtbs/
}
addtask deploy after do_compile before do_build

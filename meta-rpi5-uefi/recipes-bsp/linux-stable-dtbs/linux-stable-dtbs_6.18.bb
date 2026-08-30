SUMMARY = "Raspberry Pi 5 device trees from every linux-6.18.y release that changed them"
DESCRIPTION = "Builds bcm2712-rpi-5-b.dtb and bcm2712-d-rpi-5-b.dtb from \
               kernel.org sources at each 6.18.y release whose Pi 5 trees \
               differ from the release before it. Deployed twice: under \
               by-version/<version>/ for FdtDxe's nearest-older-version \
               lookup (edk2-platforms patch 0038), and the newest base tree \
               once flat, for the VPU bootloader to load and patch. \
\
               This is the card's whole device-tree supply. Any stable-series \
               kernel -- Talos included, either half of an A/B pair -- \
               floor-matches onto the newest directory not newer than \
               itself, which is the tree its own sources shipped: stable \
               moves device trees almost never, so four directories cover \
               all forty-seven 6.18.y releases (see LINUX_DTB_VERSIONS). \
               FdtDxe's exact \\dtb\\<uname>\\ tier still exists for a tree \
               an OS install lays down itself; this image just no longer \
               needs to ship one. \
\
               Replaced talos-boot-dtbs (and the crane-native it pulled \
               images with): that recipe lifted these same trees out of the \
               Talos kernel OCI image to keep DTB and kernel in step, and \
               pinned a tag list by hand to keep A/B rollbacks bootable. \
               Building from kernel.org at every change point gives the \
               identical artifact -- Talos ships vanilla bcm2712 dts, and \
               the overlay-merged 6.18.34 trees here came out byte-identical \
               to the 6.18.44-talos extraction -- with no tag list to tend. \
               Six overlays are baked in: five board facts (blconfig, SCMI, \
               boot SPI, dwc2 USB, serial0) and one platform tuning (CMA \
               size, bumped 64 -> 256 MiB so the NVMe HMB stops starving the \
               pool); see each .dts header."
HOMEPAGE = "https://www.kernel.org"

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

# Every linux-6.18.y release whose Pi 5 DTBs differ from the release before
# it, base release included. Found by building both trees at every tag
# v6.18..v6.18.46 with this recipe's exact cpp+dtc invocation and comparing
# hashes -- not by diffing .dts paths, which misses dt-bindings header churn
# and flags no-op reformatting. The changes, for the record:
#
#   6.18.32  bcm2712-d-rpi-5-b: D0 pinctrl compatibles and register widths
#            (brcm,bcm2712d0-pinctrl @ 0x20 wide, not C0's 0x30), uart10's
#            interrupt -- the very mismatch 0020's board-revision lookup
#            exists to route around, fixed at the source
#   6.18.34  bcm2712.dtsi: the pm watchdog@7d200000 node
#            (system-power-controller; reboot/poweroff moved off firmware)
#   6.18.45  bcm2712.dtsi: phantom hypervisor timer PPI 12 removed
#
# To extend for a new release: rerun the sweep over the new tags, and for
# each fresh change point add its version here and a patch-6.18.N.xz entry
# (plus sha256 from cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc)
# below. Nothing else moves.
LINUX_DTB_BASE = "6.18.0"
LINUX_DTB_VERSIONS = "${LINUX_DTB_BASE} 6.18.32 6.18.34 6.18.45"

# The version whose base tree the VPU bootloader itself loads, from the root
# of the boot partition. Newest by default: FdtDxe swaps in the floor-matched
# tree for any UKI kernel anyway, so the flat copy only ever serves the VPU's
# own patching and whatever boots without a versioned lookup, and for those
# the most-fixed tree is the best guess. Pin it to an older list entry if a
# fleet's kernels ever need the flat fallback to match them exactly.
LINUX_DTB_VPU_VERSION ?= "${@d.getVar('LINUX_DTB_VERSIONS').split()[-1]}"

# One base tarball plus kernel.org's cumulative patch-6.18.N (base -> N, a
# few MB each), rather than a full 140 MB tarball per version -- and nothing
# is ever fully unpacked: everything kernel.org is fetched with unpack=0 and
# do_extract_subset takes the ~15 MB device-tree subset out of the tarball
# instead of spreading the 1.5 GB kernel tree across the workdir. The
# tarball itself stays the one big download (once, into the shared DL_DIR)
# because it is the smallest artifact kernel.org publishes signed checksums
# for; fetching the subset file-by-file from cgit would be lighter on the
# wire but self-pinned and brittle against include-closure drift.
#
# The patches are not named *.patch deliberately: the fetcher must not apply
# them (a whole-kernel diff onto ${S} would serialize what do_compile
# applies per-version to its own subset copy); do_compile streams them
# compressed straight into git apply.
SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${PV}.tar.xz;name=base;unpack=0 \
           https://cdn.kernel.org/pub/linux/kernel/v6.x/patch-${PV}.32.xz;name=patch32;unpack=0 \
           https://cdn.kernel.org/pub/linux/kernel/v6.x/patch-${PV}.34.xz;name=patch34;unpack=0 \
           https://cdn.kernel.org/pub/linux/kernel/v6.x/patch-${PV}.45.xz;name=patch45;unpack=0 \
           file://bcm2712-blconfig-overlay.dts \
           file://bcm2712-scmi-overlay.dts \
           file://bcm2712-boot-spi-overlay.dts \
           file://bcm2712-dwc2-usb-overlay.dts \
           file://bcm2712-serial0-overlay.dts \
           file://bcm2712-cma-overlay.dts \
           "
SRC_URI[base.sha256sum] = "9106a4605da9e31ff17659d958782b815f9591ab308d03b0ee21aad6c7dced4b"
SRC_URI[patch32.sha256sum] = "983a951b6572cf547c8fd9148c0cbd4f8dc3d773d76afe5df58de519f04e7243"
SRC_URI[patch34.sha256sum] = "005cd67ffa6f3ef723097b6f6cbef6760047ba4c08b9357fadac926c4912d3a0"
SRC_URI[patch45.sha256sum] = "50a768ef5cb3db296d0c981cb3e5f15562e76e4657df11316ad1b5cc9d8b2af4"

DEPENDS = "dtc-native"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/linux-${PV}"

# B defaults to S; keep the built trees out of the extracted sources.
B = "${WORKDIR}/build"

inherit deploy nopackages

do_configure[noexec] = "1"
do_install[noexec] = "1"

# The full board trees only; bcm2712-rpi-5-b-ovl-rp1 is deliberately absent.
# That variant's pcie@1000120000 is bare, with RP1 supplied at runtime by the
# kernel's own built-in overlay (drivers/misc/rp1/rp1-pci.dtso) -- which
# sounds like exactly what we want, RP1 travelling with the kernel, but that
# dtso declares rp1_eth with status "disabled" and no phy-mode, phy-handle or
# reset-gpios, because those are board facts that live in the board file.
# Booting it means no NIC at all. The full tree carries them.
RPI5_DTB_TREES = "bcm2712-rpi-5-b bcm2712-d-rpi-5-b"

# Nodes the stripped mainline trees omit that this platform needs -- put
# back at build time rather than shipped as runtime .dtbo files (blconfig in
# particular is unpatchable if it is not already present when the VPU
# firmware's fixup runs). Order between them is not load-bearing: each
# targets a different path, and the one cross-reference (spi10 -> dma40)
# sits inside a single overlay where dtc resolves it through
# __local_fixups__. See each .dts header for the evidence.
RPI5_DTB_OVERLAYS = "bcm2712-blconfig-overlay bcm2712-scmi-overlay \
                     bcm2712-boot-spi-overlay bcm2712-dwc2-usb-overlay \
                     bcm2712-serial0-overlay bcm2712-cma-overlay"

# What a Pi 5 DTB is built from: the board .dts files and everything their
# include closure can reach. dt-bindings is taken whole (cheap, and the
# closure through bcm2712.dtsi moves between releases);
# input-event-codes.h is the one target outside it that a dt-bindings
# symlink (input/linux-event-codes.h) points at.
LINUX_DTB_SUBSET = "arch/arm64/boot/dts/broadcom \
                    include/dt-bindings \
                    include/uapi/linux/input-event-codes.h \
                    scripts/dtc/include-prefixes"

# Just the files necessary to compile the DTBs, straight out of the still-
# compressed tarball: the subset above, plus COPYING for LIC_FILES_CHKSUM.
# ${S} afterwards holds ~15 MB. Before do_populate_lic because that task
# reads COPYING out of ${S}.
do_extract_subset() {
    rm -rf ${S}
    tar -xJf ${UNPACKDIR}/linux-${PV}.tar.xz -C ${UNPACKDIR} \
        linux-${PV}/COPYING \
        $(for p in ${LINUX_DTB_SUBSET}; do printf 'linux-${PV}/%s ' "$p"; done)
}
addtask extract_subset after do_unpack before do_patch

# The kernel's own default dtc warning suppressions (scripts/Makefile.lib,
# the non-W=1 set): mainline ships these trees with those warnings extant,
# and re-printing them here six times per tree is noise, not signal.
DTC_QUIET = "-Wno-unit_address_vs_reg -Wno-avoid_unnecessary_addr_size \
             -Wno-alias_paths -Wno-graph_child_address \
             -Wno-simple_bus_reg -Wno-unique_unit_address"

do_compile() {
    # The overlays once; they are board facts and apply to every version.
    dtbos=""
    for ovl in ${RPI5_DTB_OVERLAYS}; do
        dtc -O dtb -@ -H epapr -o ${B}/${ovl}.dtbo ${UNPACKDIR}/${ovl}.dts
        dtbos="${dtbos} ${B}/${ovl}.dtbo"
    done

    rm -rf ${B}/dtbs
    for v in ${LINUX_DTB_VERSIONS}; do
        src=${B}/src-${v}
        rm -rf ${src}
        mkdir -p ${src}
        (cd ${S} && cp -a --parents ${LINUX_DTB_SUBSET} ${src}/)

        # Each version is base-plus-one-patch, applied to this version's own
        # subset copy and filtered to the subset's paths: git apply skips
        # what --include does not match, where plain patch would die on the
        # thousands of files the subset deliberately does not carry.
        #
        # The git init is load-bearing, not hygiene. ${B} sits inside this
        # repo's own work tree, and git apply inside any enclosing repository
        # resolves patch paths against THAT repository's top -- every path
        # then falls outside the subset copy and is ignored, exit 0, no hunk
        # applied. Making the copy a repository of its own pins the paths to
        # it; the .git is dropped as soon as the patch has landed.
        if [ "${v}" != "${LINUX_DTB_BASE}" ]; then
            (cd ${src} && git init -q . && xz -dc ${UNPACKDIR}/patch-${v}.xz \
                | git apply -p1 \
                    --include='arch/arm64/boot/dts/broadcom/*' \
                    --include='include/dt-bindings/*' \
                    --include='include/uapi/linux/input-event-codes.h' \
                && rm -rf .git)
        fi

        mkdir -p ${B}/dtbs/by-version/${v}
        for leaf in ${RPI5_DTB_TREES}; do
            # The kernel's own cmd_dtc, minus kbuild: preprocess with the
            # host cc, compile with dtc -@ (the __symbols__ both fdtoverlay
            # below and the VPU bootloader's overlay fixup resolve through).
            ${BUILD_CC} -E -nostdinc -I ${src}/scripts/dtc/include-prefixes \
                -undef -D__DTS__ -x assembler-with-cpp \
                -o ${src}/${leaf}.pre.dts \
                ${src}/arch/arm64/boot/dts/broadcom/${leaf}.dts
            dtc -I dts -O dtb -@ ${DTC_QUIET} \
                -o ${src}/${leaf}.dtb ${src}/${leaf}.pre.dts
            fdtoverlay -i ${src}/${leaf}.dtb \
                -o ${B}/dtbs/by-version/${v}/${leaf}.dtb ${dtbos}
        done

        rm -rf ${src}
    done

    # The list's own invariant, checked: every listed version changed at
    # least one tree relative to the version before it. A pair of identical
    # directories means either a patch that silently failed to land or a
    # version that no longer belongs in LINUX_DTB_VERSIONS -- both worth
    # stopping the build over, neither visible in the image otherwise.
    prev=""
    for v in ${LINUX_DTB_VERSIONS}; do
        if [ -n "${prev}" ]; then
            same=1
            for leaf in ${RPI5_DTB_TREES}; do
                cmp -s ${B}/dtbs/by-version/${prev}/${leaf}.dtb \
                       ${B}/dtbs/by-version/${v}/${leaf}.dtb || same=0
            done
            if [ "${same}" = "1" ]; then
                bbfatal "${v} built the same trees as ${prev} -- a patch did not land, or LINUX_DTB_VERSIONS is stale"
            fi
        fi
        prev="${v}"
    done

    # The one tree the VPU bootloader itself loads, at the deploy root.
    #
    # The BASE tree only, deliberately -- not the D0 one beside it. The VPU
    # bootloader's own flow is base tree plus a stepping overlay: on D0
    # silicon it loads this tree and then applies bcm2712d0.dtbo from
    # overlays/, which is Raspberry Pi's description of their own part.
    # Leaving a prebuilt D0 tree at the root only invites it to be loaded as
    # the base and then patched a second time. That flow is also why
    # bcm2712-boot-spi-overlay.dts exists: the stepping overlay resolves
    # &spi10 and &dma40 through this tree's __symbols__, and one unresolved
    # symbol makes the bootloader drop the whole overlay ("dterror: can't
    # find symbol 'spi10'"), leaving D0 silicon running a C0 description.
    if [ ! -f ${B}/dtbs/by-version/${LINUX_DTB_VPU_VERSION}/bcm2712-rpi-5-b.dtb ]; then
        bbfatal "LINUX_DTB_VPU_VERSION (${LINUX_DTB_VPU_VERSION}) is not in LINUX_DTB_VERSIONS (${LINUX_DTB_VERSIONS}) -- nothing to give the VPU bootloader"
    fi
    cp ${B}/dtbs/by-version/${LINUX_DTB_VPU_VERSION}/bcm2712-rpi-5-b.dtb ${B}/dtbs/
}

do_deploy() {
    install -d ${DEPLOYDIR}/linux-stable-dtbs
    cp -a ${B}/dtbs/. ${DEPLOYDIR}/linux-stable-dtbs/
}
addtask deploy after do_compile before do_build

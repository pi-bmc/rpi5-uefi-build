SUMMARY = "Bootable SD/NVMe image for the Raspberry Pi 5 UEFI firmware"
DESCRIPTION = "Assembles rpi5-uefi-sd.img with wic: an MBR disk image whose \
               first, bootable FAT32 partition carries everything the Pi 5 \
               VPU bootloader needs to start the alternative TF-A+EDK2 \
               bootloader stack -- armstub8-2712.bin (RPI_EFI.fd under the \
               default armstub filename, so config.txt needs no armstub= \
               line), config.txt, the bcm2712 device trees (from the Talos \
               kernel image, both flat for the VPU bootloader and under \
               dtb/<kernel release>/ for FdtDxe to pick from) and overlays/ \
               (from the Pi firmware release). \
\
               The boot partition contents are assembled into a staging tree \
               and handed to wic's rootfs source; the DTB layout (each tree \
               at the root, under broadcom/ and under dtb/<release>/) does \
               not map onto IMAGE_BOOT_FILES, so bootimg-partition is not \
               used. \
\
               NOTE ON OP-TEE STORAGE: no RPMB partition is created here, \
               because RPMB cannot be. RPMB is a hardware partition inside \
               an eMMC device, provisioned with authenticated eMMC commands \
               and a one-time key -- not a partition-table entry any image \
               can create -- and the Pi 5 boots from SD/NVMe, which have no \
               RPMB at all. UEFI-variables-in-RPMB (StandaloneMM) needs eMMC \
               and stays out of scope; the firmware keeps its FD-backed \
               authenticated variable store. If OP-TEE REE-FS secure storage \
               is wanted later (needs an OS with tee-supplicant), add a data \
               partition to the wks then. See docs/optee-bmc-sensor.md. \
\
               This image is intentionally NOT compatible with the \
               u-boot-based RPi image from ../nanokvm-build: both stacks \
               claim the armstub8-2712.bin name with different payloads, so a \
               card carries one bootloader or the other, never both."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Native tools wic drives: parted (partition table), mtools (vfat), mke2fs
# -d (ext4), plus util-linux/gptfdisk helpers. These land in
# ${RECIPE_SYSROOT_NATIVE}, which is what we hand wic as --native-sysroot.
DEPENDS = "parted-native mtools-native dosfstools-native e2fsprogs-native \
           util-linux-native gptfdisk-native"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

SRC_URI = "file://wic/rpi5-uefi.wks.in"

inherit deploy nopackages

# Everything on the boot partition comes from other recipes' deploy output.
do_fetch[noexec] = "1"
do_patch[noexec] = "1"
do_configure[noexec] = "1"
do_install[noexec] = "1"

do_compile[depends] += "rpi5-uefi-firmware:do_deploy rpi-boot-dtbs:do_deploy talos-boot-dtbs:do_deploy"

# FAT32 boot partition size. Contents are ~9 MiB (3.9M firmware + ~4M
# overlays + DTBs) plus ~160K per extra kernel release; 64 MiB leaves room
# for dtoverlay additions and FD growth.
SDIMG_BOOT_MB ?= "64"

SDIMG_NAME = "rpi5-uefi-sd.img"
WKS_TEMPLATE = "${UNPACKDIR}/wic/rpi5-uefi.wks.in"

# UNPACKDIR only exists from styhead on; scarthgap unpacks into WORKDIR.
UNPACKDIR ?= "${WORKDIR}"

do_compile() {
    boot="${B}/boot"
    rm -rf "${boot}" "${B}/wic-out"
    install -d "${boot}"

    # --- Assemble the FAT32 boot partition tree ------------------------
    # RPI_EFI.fd under the default BCM2712 armstub filename: the VPU
    # bootloader auto-loads armstub8-2712.bin at 0x0 (where RPi5.fdf links
    # the FD, PcdFdBaseAddress=0), no armstub= line needed. This name is
    # ALSO the NV variable store's backing file (VarBlockServiceDxe writes
    # the FD variable region back to the file it loaded, found by name --
    # see 0013-VarBlockServiceDxe-... in the edk2-platforms recipe). Rename
    # it here and Setup changes stop surviving reboot until that patch's
    # candidate list is updated to match.
    install -m 0644 "${DEPLOY_DIR_IMAGE}/RPI_EFI.fd" "${boot}/armstub8-2712.bin"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/config.txt" "${boot}/config.txt"

    # Board device trees from the Talos kernel image (see talos-boot-dtbs),
    # so they match the kernel that consumes them. Twice: at the root and
    # under broadcom/ (the layout mainline/U-Boot DTB_DIR use on arm64);
    # config.txt sets upstream_kernel=1 so the VPU asks for mainline names,
    # and the two locations cover both conventions.
    install -d "${boot}/broadcom"
    for dtb in "${DEPLOY_DIR_IMAGE}/talos-boot-dtbs/"*.dtb; do
        install -m 0644 "${dtb}" "${boot}/"
        install -m 0644 "${dtb}" "${boot}/broadcom/"
    done

    # The same trees keyed by kernel release, where FdtDxe looks:
    # \dtb\<release>\bcm2712-rpi-5-b.dtb, chosen from the UKI's .uname
    # section. Deliberately no unkeyed \dtb\ copy: FdtDxe's fallback is
    # already better served by the VPU-patched tree at the root.
    install -d "${boot}/dtb"
    for dir in "${DEPLOY_DIR_IMAGE}/talos-boot-dtbs/by-uname/"*/; do
        release=$(basename "${dir}")
        install -d "${boot}/dtb/${release}"
        install -m 0644 "${dir}"*.dtb "${boot}/dtb/${release}/"
    done

    # Overlays from the firmware release (resolve against config.txt
    # dtoverlay= lines the VPU processes; the Talos image ships none).
    install -d "${boot}/overlays"
    cp -a "${DEPLOY_DIR_IMAGE}/rpi-boot-dtbs/overlays/." "${boot}/overlays/"

    # --- Render the wks and build the image with wic -------------------
    wks="${B}/rpi5-uefi.wks"
    sed -e 's/@SDIMG_BOOT_MB@/${SDIMG_BOOT_MB}/g' \
        "${WKS_TEMPLATE}" > "${wks}"

    # Direct wic invocation (not do_image_wic: this recipe has no OS rootfs,
    # so it does not inherit image.bbclass). The boot tree is the default
    # ROOTFS_DIR. wic insists on BUILDDIR in its environment; everything
    # else it needs is passed explicitly so it never shells back into
    # bitbake.
    export BUILDDIR="${TOPDIR}"
    wic create "${wks}" \
        --outdir "${B}/wic-out" \
        --rootfs-dir "${boot}" \
        --native-sysroot "${RECIPE_SYSROOT_NATIVE}" \
        --bootimg-dir "${B}" \
        --kernel-dir "${B}"

    # wic names the output <wks>-<timestamp>.direct.
    built=$(ls -1 ${B}/wic-out/*.direct | head -n1)
    [ -n "${built}" ] || bbfatal "wic produced no .direct image -- check the log"
    cp -f "${built}" "${B}/${SDIMG_NAME}"
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/${SDIMG_NAME} ${DEPLOYDIR}/${SDIMG_NAME}
}

addtask deploy after do_compile

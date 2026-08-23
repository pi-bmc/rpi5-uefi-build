SUMMARY = "Bootable SD/NVMe image for the Raspberry Pi 5 UEFI firmware"
DESCRIPTION = "Assembles rpi5-uefi-sd.img with wic (do_image_wic): an MBR \
               disk whose single bootable FAT32 partition carries \
               everything the Pi 5 VPU bootloader needs to start the \
               alternative TF-A+EDK2 bootloader stack -- armstub8-2712.bin \
               (RPI_EFI.fd under the default armstub filename, so config.txt \
               needs no armstub= line), config.txt, the bcm2712 device trees \
               (from the Talos kernel image, both flat for the VPU \
               bootloader and under dtb/<kernel release>/ for FdtDxe to pick \
               from) and overlays/ (from the Pi firmware release). \
\
               This is an image recipe purely to reuse do_image_wic: running \
               wic by hand inside a normal task deadlocks, because wic \
               resolves HOSTTOOLS_DIR/TARGET_SYS/BBLAYERS by shelling out to \
               'bitbake -e', which blocks on the build's cooker lock the \
               running task already holds. do_image_wic feeds those to wic \
               through the imgdata env it generates from WICVARS, so no \
               nested bitbake is spawned. The OS rootfs this recipe builds \
               is empty and unused: the boot partition is populated from the \
               'boottree' staging tree (do_stage_bootfiles), passed to wic \
               as a named --rootfs-dir; the DTB layout (each tree at the \
               root, under broadcom/ and under dtb/<release>/) does not map \
               onto IMAGE_BOOT_FILES, so bootimg-partition is not used. \
\
               NOTE ON OP-TEE STORAGE: no RPMB partition is created, because \
               RPMB cannot be. RPMB is a hardware partition inside an eMMC \
               device -- not a partition-table entry any image can create -- \
               and the Pi 5 boots from SD/NVMe, which have none. \
               UEFI-variables-in-RPMB (StandaloneMM) needs eMMC and stays \
               out of scope; the firmware keeps its FD-backed authenticated \
               variable store. See docs/optee-bmc-sensor.md. \
\
               This image is intentionally NOT compatible with the \
               u-boot-based RPi image from ../nanokvm-build: both stacks \
               claim the armstub8-2712.bin name with different payloads, so a \
               card carries one bootloader or the other, never both."

LICENSE = "MIT"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# xz-compressed wic image; the OS rootfs stays empty (the boot partition
# comes from the boot-staging dir, see below).
IMAGE_FSTYPES = "wic.xz"
IMAGE_INSTALL = ""
IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
PACKAGE_INSTALL = ""
# Nothing here should end up in a package feed or need one.
NO_RECOMMENDATIONS = "1"

# Plain kickstart (boot partition fixed at 64 MiB -- contents are ~13 MiB,
# leaving room for dtoverlay additions and FD growth).
WKS_FILE = "rpi5-uefi.wks"
WKS_SEARCH_PATH = "${THISDIR}/files/wic"

# The boot partition is the boot-staging dir (passed as the default
# ROOTFS_DIR the .wks references), not the empty OS rootfs.
WIC_CREATE_EXTRA_ARGS = "--rootfs-dir ${WORKDIR}/boot-staging"

inherit image

BOOT_STAGING = "${WORKDIR}/boot-staging"

# Assemble the FAT32 boot partition tree from other recipes' deploy output.
do_stage_bootfiles[depends] += "rpi5-uefi-firmware:do_deploy rpi-boot-dtbs:do_deploy talos-boot-dtbs:do_deploy"
do_stage_bootfiles () {
    boot="${BOOT_STAGING}"
    rm -rf "${boot}"
    install -d "${boot}"

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
}
addtask stage_bootfiles after do_rootfs before do_image_wic

# Deploy the stable legacy name (compressed): rpi5-uefi-sd.img.xz.
# Decompress before flashing, e.g. `xzcat rpi5-uefi-sd.img.xz | sudo dd of=...`.
rename_wic_legacy () {
    cp --dereference "${IMGDEPLOYDIR}/${IMAGE_LINK_NAME}.wic.xz" \
        "${IMGDEPLOYDIR}/rpi5-uefi-sd.img.xz"
}
do_image_wic[postfuncs] += "rename_wic_legacy"

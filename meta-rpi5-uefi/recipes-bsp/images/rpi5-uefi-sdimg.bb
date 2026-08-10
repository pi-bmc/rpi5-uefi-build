SUMMARY = "Bootable SD card image for the Raspberry Pi 5 UEFI firmware"
DESCRIPTION = "Assembles rpi5-uefi-sd.img: an MBR disk image with a single \
               bootable FAT32 partition carrying everything the Pi 5 VPU \
               bootloader needs to start the alternative TF-A+EDK2 bootloader \
               stack -- armstub8-2712.bin (RPI_EFI.fd under the default \
               armstub filename, so config.txt needs no armstub= line), \
               config.txt, the bcm2712 device trees, and the overlays/ \
               directory. This image is intentionally NOT compatible with the \
               u-boot-based RPi image from ../nanokvm-build: both stacks claim \
               the armstub8-2712.bin name with different payloads (bare BL31 \
               + kernel=u-boot.bin there, BL31+UEFI here), so a card carries \
               one bootloader or the other, never both. \
\
               Built with plain sfdisk/mkfs.vfat/mcopy rather than wic: the \
               image has no rootfs (UEFI boots the OS from other media), so \
               the image-class machinery would be pure overhead."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "dosfstools-native mtools-native util-linux-native"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

# Everything comes from other recipes' deploy output.
do_fetch[noexec] = "1"
do_unpack[noexec] = "1"
do_patch[noexec] = "1"
do_configure[noexec] = "1"
do_install[noexec] = "1"

do_compile[depends] += "edk2-rpi5-firmware:do_deploy rpi-boot-dtbs:do_deploy"

# FAT32 boot partition size. Contents are ~9 MiB (3.8M firmware + ~4M
# overlays + DTBs); 64 MiB leaves comfortable room for dtoverlay additions
# and future FD growth without resizing the layout.
SDIMG_BOOT_MB ?= "64"
# Partition start offset, the conventional 4 MiB alignment.
SDIMG_OFFSET_MB = "4"

SDIMG_NAME = "rpi5-uefi-sd.img"

do_compile() {
    part="${B}/boot.vfat"
    img="${B}/${SDIMG_NAME}"
    rm -f "${part}" "${img}"

    # --- FAT32 boot partition ------------------------------------------
    truncate -s ${SDIMG_BOOT_MB}M "${part}"
    mkfs.vfat -F 32 -n RPI5-UEFI -S 512 "${part}"

    # RPI_EFI.fd under the default BCM2712 armstub filename: the VPU
    # bootloader auto-loads armstub8-2712.bin at address 0x0 (exactly where
    # RPi5.fdf links the FD, PcdFdBaseAddress=0), no armstub= line needed.
    mcopy -i "${part}" "${DEPLOY_DIR_IMAGE}/RPI_EFI.fd" ::/armstub8-2712.bin
    mcopy -i "${part}" "${DEPLOY_DIR_IMAGE}/config.txt" ::/config.txt

    # Board device trees + overlays (see rpi-boot-dtbs).
    for dtb in "${DEPLOY_DIR_IMAGE}/rpi-boot-dtbs/"*.dtb; do
        mcopy -i "${part}" "${dtb}" ::/
    done
    mmd -i "${part}" ::/overlays
    mcopy -i "${part}" -s "${DEPLOY_DIR_IMAGE}/rpi-boot-dtbs/overlays/"* ::/overlays/

    # --- MBR disk image -------------------------------------------------
    # expr + printf-pipe rather than $(( )) arithmetic and a heredoc:
    # bitbake's shell parser (pysh) rejects both constructs inside task
    # functions.
    total_mb=$(expr ${SDIMG_OFFSET_MB} + ${SDIMG_BOOT_MB} + 1)
    truncate -s ${total_mb}M "${img}"
    printf 'label: dos\n%sMiB,%sMiB,0x0c,*\n' "${SDIMG_OFFSET_MB}" "${SDIMG_BOOT_MB}" | sfdisk "${img}"
    dd if="${part}" of="${img}" bs=1M seek=${SDIMG_OFFSET_MB} conv=notrunc,fsync
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/${SDIMG_NAME} ${DEPLOYDIR}/${SDIMG_NAME}
}

addtask deploy after do_compile

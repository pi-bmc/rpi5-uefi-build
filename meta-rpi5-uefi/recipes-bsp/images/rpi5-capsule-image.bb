SUMMARY = "Firmware update capsule volume for the Raspberry Pi 5 UEFI stack"
DESCRIPTION = "Assembles rpi5-capsule.img with wic (do_image_wic): a GPT \
               disk whose single 64 MiB FAT32 EFI System Partition carries \
               the signed FMP capsule under EFI/UpdateCapsule/ \
               (RPi5Firmware.cap) -- the drop box of UEFI 2.10 8.5.5, \
               'Delivering Capsules Across a System Reset'. \
\
               The layout deliberately matches the volume nanokvm-app's \
               pkg/firmware builds at runtime and presents on the \
               mass-storage gadget's lun.0, so this image serves as a \
               drop-in lun.0 backing file, a dd-able USB stick for manual \
               capsule testing, and the test vehicle for the host-side \
               capsule scanner once that lands (Rpi5FmpDeviceLib's README: \
               the sync driver 'does not yet pull and apply'). \
\
               An image recipe purely to reuse do_image_wic, for the same \
               reason as rpi5-uefi-sdimg: wic run by hand inside a task \
               deadlocks on the cooker lock ('bitbake -e' from within a \
               task). The OS rootfs this recipe builds is empty and unused; \
               the partition is populated from the capsule-staging tree \
               passed to wic as the default --rootfs-dir."

BUGTRACKER = "https://github.com/pi-bmc/rpi5-uefi-build/issues"
SECTION = "firmware"
LICENSE = "MIT"
CVE_PRODUCT = "rpi5-uefi"
COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# xz-compressed: the volume is 64 MiB of mostly-empty FAT, ~2 MiB packed.
# raw image: `gzip -dk rpi5-capsule.img.gz`.
# raw image: `xz -dk rpi5-capsule.img.zstd`.
IMAGE_FSTYPES = "wic.gz"
IMAGE_INSTALL = ""
IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
PACKAGE_INSTALL = ""
# Nothing here should end up in a package feed or need one.
NO_RECOMMENDATIONS = "1"

# A .wks edit rebuilds this image with no help from the recipe:
# image_types_wic.bbclass resolves WKS_FILE through WKS_SEARCH_PATH into
# WKS_FULL_PATH and lists it under do_image_wic[file-checksums], so the
# kickstart's content hash is part of the task signature (verified: editing
# the file reruns do_image_wic). Do not add a duplicate file-checksums line.
WKS_FILE = "rpi5-capsule.wks"
WKS_SEARCH_PATH = "${THISDIR}/files/wic"

# The partition content is the capsule-staging dir (passed as the default
# ROOTFS_DIR the .wks references), not the empty OS rootfs.
WIC_CREATE_EXTRA_ARGS = "--rootfs-dir ${WORKDIR}/capsule-staging"

inherit image

CAPSULE_STAGING = "${WORKDIR}/capsule-staging"

# Stage the signed capsule under the spec's drop-box path.
do_stage_capsule[depends] += "rpi5-uefi-firmware:do_deploy"
do_stage_capsule[doc] = "Stage the signed capsule under EFI/UpdateCapsule in the staging tree"
do_stage_capsule () {
    staging="${CAPSULE_STAGING}"
    rm -rf "${staging}"
    install -d "${staging}/EFI/UpdateCapsule"

    # do_deploy legitimately skips the capsule when RPI5_FMP_CERT is set
    # without RPI5_FMP_KEY (sign-offline mode). A capsule volume with no
    # capsule inside must fail here, loudly, not build as an empty disk.
    if [ ! -f "${DEPLOY_DIR_IMAGE}/RPi5Firmware.cap" ]; then
        bbfatal "No RPi5Firmware.cap in ${DEPLOY_DIR_IMAGE}. The firmware build skipped capsule generation (RPI5_FMP_CERT set without RPI5_FMP_KEY signs offline) -- nothing to stage."
    fi

    install -m 0644 "${DEPLOY_DIR_IMAGE}/RPi5Firmware.cap" \
        "${staging}/EFI/UpdateCapsule/RPi5Firmware.cap"

    # The build's config.txt rides beside the capsule as a sidecar (never
    # a capsule payload: the appliers skip it by name). Once the firmware
    # on the boot medium is proven to match this build -- verified apply
    # or already-running version -- Rpi5CapsuleApp / RpiCapsuleOnDiskLib
    # converge it onto every volume carrying the firmware file. config.txt
    # is firmware-owned and coupled to the FD layout (device_tree_address);
    # user overrides belong in uefi-cfg.txt, which updates never touch.
    if [ ! -f "${DEPLOY_DIR_IMAGE}/config.txt" ]; then
        bbfatal "No config.txt in ${DEPLOY_DIR_IMAGE}: rebuild rpi5-uefi-firmware (its do_deploy ships it)."
    fi

    install -m 0644 "${DEPLOY_DIR_IMAGE}/config.txt" \
        "${staging}/EFI/UpdateCapsule/config.txt"

    # The self-applying updater, on the removable-media default boot path:
    # booting this volume (BMC boot override, boot menu, or fallback) runs
    # it, and it applies the capsule above and deletes it. See
    # Rpi5CapsuleApp in the edk2-platforms recipe.
    if [ ! -f "${DEPLOY_DIR_IMAGE}/Rpi5CapsuleApp.efi" ]; then
        bbfatal "No Rpi5CapsuleApp.efi in ${DEPLOY_DIR_IMAGE}: rebuild rpi5-uefi-firmware (its do_deploy ships the updater)."
    fi

    install -d "${staging}/EFI/BOOT"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/Rpi5CapsuleApp.efi" \
        "${staging}/EFI/BOOT/BOOTAA64.EFI"
}
addtask stage_capsule after do_rootfs before do_image_wic

# Deploy the stable name: rpi5-capsule.img.
rename_wic_capsule[doc] = "Rename the wic gzip image to the stable capsule name"
rename_wic_capsule () {
    cp --dereference "${IMGDEPLOYDIR}/${IMAGE_LINK_NAME}.wic.gz" \
        "${IMGDEPLOYDIR}/rpi5-capsule.img.gz"
}

do_image_wic[postfuncs] += "rename_wic_capsule"

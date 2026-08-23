SUMMARY = "OP-TEE OS (BL32) for Raspberry Pi 5 - BMC sensor push service"
DESCRIPTION = "Builds tee-raw.bin, the Secure-EL1 payload embedded in \
               RPI_EFI.fd (FD region at offset 0x330000, see RPi5.fdf). The \
               patched TF-A rpi5 BL31 copies it to the secure DRAM carve-out \
               at 0x1D000000 and enters it through the opteed dispatcher \
               before EDK2 (BL33) runs. \
\
               Upstream optee_os has carried a (console-only) plat-rpi5 \
               since 4.10; this recipe's patch wires up the GIC, the \
               secure-timer callout service and async notifications, and \
               the files/plat-rpi5/ overlay adds the pi-bmc BMC sensor \
               service: a pseudo-TA that samples the BCM2712 die \
               temperature on a secure timer and pushes records to the \
               BMC-emulated I2C EEPROM behind the RP1 (late-initialized \
               with the RP1 BAR by EDK2's RpiOpteeSensorDxe -- the RP1 is a \
               PCIe endpoint, so its address only exists after PCIe \
               enumeration). \
\
               tee-raw.bin reaches the firmware through edk2-non-osi, which \
               files it next to bl31.bin at \
               Platform/RaspberryPi/RPi5/TrustedFirmware/tee-raw.bin, the \
               path RPi5.fdf names. do_deploy also drops tee-raw.bin and \
               tee.elf in DEPLOY_DIR_IMAGE for inspection and debugging."
HOMEPAGE = "https://github.com/OP-TEE/optee_os"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=c1f21c4f72f372ef38a5a4aee55ec173"

SRC_URI = "git://github.com/OP-TEE/optee_os.git;protocol=https;nobranch=1 \
           file://plat-rpi5;subdir=git/core/arch/arm \
           file://0001-plat-rpi5-wire-up-GIC-secure-timer-callouts-and-the-.patch \
           "
# Tag 4.10.0. The files/plat-rpi5/ overlay directory merges into the
# checkout's core/arch/arm/plat-rpi5/ during do_unpack (new files only;
# the patch carries the edits to upstream files). LOAD-BEARING: the
# file:// entries must stay listed after the git entry -- the git
# fetcher prunes its destdir before checkout.
SRCREV = "753afbbee1682f5d16fd30e87b31058a4fd4f4b8"

PV = "4.10.0+git${SRCPV}"
S = "${WORKDIR}/git"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# One board, one set of carve-out addresses baked in.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# The output is firmware: staged for edk2-non-osi and deployed, never
# packaged into a rootfs.
inherit deploy nopackages python3native

# gen_tee_bin.py needs pyelftools.
DEPENDS = "python3-pyelftools-native"

do_configure[noexec] = "1"

# Set to "1" (e.g. via a local.conf override) for a debug core with the
# full DMSG/FMSG trace stream on the PL011.
OPTEE_DEBUG ??= "0"
OPTEE_LOG_LEVEL = "${@'3' if d.getVar('OPTEE_DEBUG') == '1' else '1'}"
OPTEE_CORE_DEBUG = "${@'y' if d.getVar('OPTEE_DEBUG') == '1' else 'n'}"

# Everything else (TZDRAM/SHM layout, async-notif INTID, user-TA off) is
# set in plat-rpi5/conf.mk by the patch so the configuration lives with
# the platform, not the build system.
do_compile() {
    # OP-TEE's build computes its own cross flags from CROSS_COMPILE64;
    # keep bitbake's target sysroot flags out of the freestanding core
    # build, same as the arm-trusted-firmware recipe.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

    oe_runmake -C ${S} O=${B}/out \
        PLATFORM=rpi5 \
        CROSS_COMPILE64="${TARGET_PREFIX}" \
        CFG_TEE_CORE_LOG_LEVEL=${OPTEE_LOG_LEVEL} \
        CFG_TEE_CORE_DEBUG=${OPTEE_CORE_DEBUG} \
        all

    [ -s "${B}/out/core/tee-raw.bin" ] || \
        bbfatal "OP-TEE produced no out/core/tee-raw.bin -- check the build log"

    # RPi5.fdf gives the image 0x80000 bytes and TF-A copies exactly that
    # much; a bigger core would be silently truncated at boot.
    TEE_SIZE=$(stat -Lc %s ${B}/out/core/tee-raw.bin)
    if [ "$TEE_SIZE" -gt 524288 ]; then
        bbfatal "tee-raw.bin is $TEE_SIZE bytes, over the 0x80000 FD region -- grow the region in RPi5.fdf and RPI5_OPTEE_IMAGE_MAX_SIZE in the TF-A patch together"
    fi
}

# Where edk2-non-osi picks tee-raw.bin up; keep the two in step.
OPTEE_SYSROOT_DIR = "${datadir}/optee-os"

do_install() {
    install -d ${D}${OPTEE_SYSROOT_DIR}
    install -m 0644 ${B}/out/core/tee-raw.bin ${D}${OPTEE_SYSROOT_DIR}/tee-raw.bin
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/out/core/tee-raw.bin ${DEPLOYDIR}/tee-raw.bin
    install -m 0644 ${B}/out/core/tee.elf ${DEPLOYDIR}/tee.elf
}

addtask deploy after do_compile

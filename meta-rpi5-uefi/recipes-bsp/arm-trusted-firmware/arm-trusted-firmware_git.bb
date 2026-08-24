SUMMARY = "ARM Trusted Firmware-A BL31 for Raspberry Pi 5 (BCM2712)"
DESCRIPTION = "Builds bl31.bin, the EL3 runtime firmware EDK2's RPI_EFI.fd is \
               concatenated with (FD.RPI_EFI region 0x00000000, ahead of the UEFI FV) per \
               edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.fdf. RPi5 support has never \
               been upstreamed into ARM-software/arm-trusted-firmware proper -- it only \
               ever lived on a 'rpi5' branch there, which has since been deleted (the \
               matching PLAT=rpi3/rpi4/rpi5 port is Raspberry Pi Foundation/community work, \
               not an Arm-maintained platform). The commit this recipe pins is the exact \
               one the NumberOneGit/rpi5-uefi wrapper repo's arm-trusted-firmware submodule \
               points at (still fetchable from ARM-software's repo by SHA even though no \
               branch/tag names it any more), so this recipe uses nobranch=1 rather than a \
               branch= that no longer resolves. \
\
               bl31.bin reaches the firmware through edk2-non-osi, which depends on \
               this recipe and files the binary at \
               Platform/RaspberryPi/RPi5/TrustedFirmware/bl31.bin -- the path RPi5.dsc \
               already looks for by default, being where upstream keeps checked-in \
               TF-A builds. do_install stages it for that; do_deploy still puts \
               bl31.bin and bl31.elf in DEPLOY_DIR_IMAGE, now for inspection and \
               debugging rather than for anything to consume."
HOMEPAGE = "https://github.com/ARM-software/arm-trusted-firmware"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://docs/license.rst;md5=6ed7bace7b0bc63021c6eba7b524039e"

SRC_URI = "git://github.com/ARM-software/arm-trusted-firmware.git;protocol=https;nobranch=1 \
           file://0001-rpi5-support-OP-TEE-as-BL32-via-SPD-opteed.patch \
           file://0002-rpi5-forward-the-SCMI-doorbell-SiP-SMC-into-OP-TEE.patch \
           "
# Pinned to the exact commit NumberOneGit/rpi5-uefi's .gitmodules records for
# this submodule (branch 'rpi5', now deleted upstream -- see DESCRIPTION).
SRCREV = "000fe221b859ee82a4e2f8bf2c96f0086a772c89"

PV = "2.10+git${SRCPV}"
S = "${WORKDIR}/git"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# bl31.bin is built for one board (PLAT=rpi5) with one set of preloaded-image
# addresses, so it is machine-specific rather than merely AArch64.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# The output is firmware: it is staged for edk2-non-osi and deployed, never
# packaged into a rootfs.
inherit deploy nopackages

# Plain "make PLAT=rpi5 ... all" -- TF-A has no configure step of its own.
do_configure[noexec] = "1"

# Release by default; set to "1" (e.g. via a local.conf override) for a
# DEBUG=1 build with UART crash logging.
TFA_DEBUG ??= "0"
TFA_BUILD_TYPE = "${@'debug' if d.getVar('TFA_DEBUG') == '1' else 'release'}"

# OP-TEE (BL32) support: SPD=opteed plus the preloaded-BL32 patch above
# (BL31 copies tee-raw.bin from FD offset 0x330000 to 0x1D000000 -- see
# the optee-os recipe and RPi5.fdf, and keep RPI5_OPTEE consistent
# across this recipe, edk2-non-osi and rpi5-uefi-firmware). With
# RPI5_OPTEE=0 no SPD is built and the patch's SPD_opteed-guarded code
# compiles out.
RPI5_OPTEE ??= "1"
TFA_SPD_ARG = "${@'SPD=opteed' if d.getVar('RPI5_OPTEE') == '1' else ''}"

do_compile() {
    # TF-A's own Makefile computes CC/AS/LD etc. from CROSS_COMPILE; leave
    # bitbake's exported cross CFLAGS/LDFLAGS out of it the same way the
    # coreboot/edk2 recipes in meta-nuc-bios do for their own toolchains --
    # TF-A's freestanding EL3 build wants none of the target sysroot flags.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

    oe_runmake -C ${S} \
        PLAT=rpi5 \
        PRELOADED_BL33_BASE=0x20000 \
        RPI3_PRELOADED_DTB_BASE=0x3E0000 \
        SUPPORT_VFP=1 \
        SMC_PCI_SUPPORT=1 \
        CRASH_REPORTING=1 \
        ${TFA_SPD_ARG} \
        DEBUG=${TFA_DEBUG} \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        all

    [ -s "${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.bin" ] || \
        bbfatal "TF-A produced no build/rpi5/${TFA_BUILD_TYPE}/bl31.bin -- check the build log"
}

# Where edk2-non-osi picks bl31.bin up; keep the two in step.
TFA_SYSROOT_DIR = "${datadir}/arm-trusted-firmware"

do_install() {
    install -d ${D}${TFA_SYSROOT_DIR}
    install -m 0644 ${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.bin ${D}${TFA_SYSROOT_DIR}/bl31.bin
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.bin ${DEPLOYDIR}/bl31.bin
    # TF-A links the ELF under a per-image subdir (build/<plat>/<type>/bl31/);
    # only the objcopy'd .bin lands at the build-type root.
    install -m 0644 ${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31/bl31.elf ${DEPLOYDIR}/bl31.elf
}

addtask deploy after do_compile

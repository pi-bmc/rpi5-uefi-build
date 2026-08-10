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
               branch= that no longer resolves."
HOMEPAGE = "https://github.com/ARM-software/arm-trusted-firmware"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://docs/license.rst;md5=6ed7bace7b0bc63021c6eba7b524039e"

SRC_URI = "git://github.com/ARM-software/arm-trusted-firmware.git;protocol=https;nobranch=1"
# Pinned to the exact commit NumberOneGit/rpi5-uefi's .gitmodules records for
# this submodule (branch 'rpi5', now deleted upstream -- see DESCRIPTION).
SRCREV = "000fe221b859ee82a4e2f8bf2c96f0086a772c89"

PV = "2.10+git${SRCPV}"
S = "${WORKDIR}/git"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

# Plain "make PLAT=rpi5 ... all" -- TF-A has no configure step of its own.
do_configure[noexec] = "1"

# Release by default; set to "1" (e.g. via a local.conf override) for a
# DEBUG=1 build with UART crash logging.
TFA_DEBUG ??= "0"
TFA_BUILD_TYPE = "${@'debug' if d.getVar('TFA_DEBUG') == '1' else 'release'}"

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
        DEBUG=${TFA_DEBUG} \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        all

    [ -s "${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.bin" ] || \
        bbfatal "TF-A produced no build/rpi5/${TFA_BUILD_TYPE}/bl31.bin -- check the build log"
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.bin ${DEPLOYDIR}/bl31.bin
    install -m 0644 ${S}/build/rpi5/${TFA_BUILD_TYPE}/bl31.elf ${DEPLOYDIR}/bl31.elf
}

addtask deploy after do_compile

do_install[noexec] = "1"

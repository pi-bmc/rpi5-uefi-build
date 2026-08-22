SUMMARY = "tianocore edk2-non-osi source tree, carrying this build's TF-A bl31.bin"
DESCRIPTION = "The edk2-non-osi tree, staged into ${datadir}/edk2/edk2-non-osi as \
               one of the firmware build's PACKAGES_PATH roots. The tree itself is \
               unpatched -- what this recipe adds to it is one file: the bl31.bin \
               arm-trusted-firmware built, installed at \
               Platform/RaspberryPi/RPi5/TrustedFirmware/bl31.bin. \
\
               That is the path RPi5.dsc's TFA_BUILD_BL31 names by default, and \
               edk2-non-osi is where upstream keeps checked-in TF-A builds for the \
               other Raspberry Pi boards -- so the FDF's region-0 payload resolves \
               through PACKAGES_PATH like every other source file, and the firmware \
               recipe passes no -D TFA_BUILD_ARTIFACTS at all. It used to point that \
               macro at DEPLOY_DIR_IMAGE, which meant reaching into another recipe's \
               deploy output and an explicit do_compile[depends] to make the ordering \
               work; a normal DEPENDS on arm-trusted-firmware replaces both. \
\
               Upstream carries no RPi5 directory here (the retired NumberOneGit fork \
               was upstream at this commit plus exactly that one binary), so nothing \
               is being overwritten -- but if upstream ever adds one, this install \
               deliberately wins: the bl31.bin in the FD should be the one this build \
               produced."
HOMEPAGE = "https://github.com/tianocore/edk2-non-osi"

# edk2-non-osi carries no top-level License.txt on purpose -- it is a grab-bag
# of silicon-vendor binaries and sources, each module supplying its own
# License.txt, with the nearest one in the hierarchy taking precedence. Readme.md
# is where the repository states that policy, so that is what LIC_FILES_CHKSUM
# pins; the declared LICENSE is the family every module in it belongs to. The
# one file the RPi5 build actually takes out of this tree is our own TF-A build
# (BSD-3-Clause, declared by the arm-trusted-firmware recipe).
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://Readme.md;md5=7683be75b315e148746417d1238d3157"

PV = "202509+git${SRCPV}"

# bl31.bin, staged under ${STAGING_DATADIR}/arm-trusted-firmware and installed
# into the tree below.
DEPENDS = "arm-trusted-firmware"

SRC_URI = "git://github.com/tianocore/edk2-non-osi.git;protocol=https;branch=master;destsuffix=edk2-non-osi"

# The former NumberOneGit fork was upstream at this commit plus only an RPi5
# TrustedFirmware bl31.bin -- which this recipe now builds rather than vendors.
SRCREV = "94d048981116e2e3eda52dad1a89958ee404098d"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/edk2-non-osi"

# Nothing is compiled and nothing is packaged, but the tree still has to reach
# the firmware recipe's sysroot, so do_populate_sysroot runs.
#
# NOT allarch, unlike the other three source-tree recipes: the bl31.bin this
# stages is an AArch64 binary built for one board, which makes the staged tree
# machine-specific. Nothing here is compiled either way, hence
# INHIBIT_DEFAULT_DEPS -- the toolchain arrives through arm-trusted-firmware,
# which is the recipe that needs one.
inherit nopackages
PACKAGE_ARCH = "${MACHINE_ARCH}"
INHIBIT_DEFAULT_DEPS = "1"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# Where the tree lands in the sysroot. rpi5-uefi-firmware reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

# Must match arm-trusted-firmware's TFA_SYSROOT_DIR.
TFA_SYSROOT_DIR = "${STAGING_DATADIR}/arm-trusted-firmware"

# Where RPi5.dsc's "!ifndef TFA_BUILD_ARTIFACTS" branch looks, relative to a
# PACKAGES_PATH root: Platform/RaspberryPi/$(PLATFORM_NAME)/TrustedFirmware.
TFA_BL31_SUBDIR = "Platform/RaspberryPi/RPi5/TrustedFirmware"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi
    cp -a ${S}/. ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/
    rm -rf ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/.git

    [ -f "${TFA_SYSROOT_DIR}/bl31.bin" ] || \
        bbfatal "arm-trusted-firmware staged no ${TFA_SYSROOT_DIR}/bl31.bin -- without it the FD has no EL3 payload and the board resets at BL31 entry."
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/${TFA_BL31_SUBDIR}
    install -m 0644 ${TFA_SYSROOT_DIR}/bl31.bin \
        ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/${TFA_BL31_SUBDIR}/bl31.bin
}

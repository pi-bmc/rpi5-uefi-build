SUMMARY = "tianocore edk2-non-osi source tree"
DESCRIPTION = "The edk2-non-osi tree, staged into ${datadir}/edk2/edk2-non-osi as \
               one of edk2-rpi5-firmware's PACKAGES_PATH roots. Unpatched: this \
               layer changes nothing in it, and the RPi5 build reaches into it for \
               nothing either -- RPi5.dsc's default TFA_BUILD_BL31 points at \
               Platform/RaspberryPi/RPi5/TrustedFirmware/bl31.bin here, but the \
               firmware recipe always passes -D TFA_BUILD_ARTIFACTS pointing at our \
               own arm-trusted-firmware deploy instead. It stays on PACKAGES_PATH \
               because that is the layout upstream rpi5-uefi's build.sh (and every \
               other edk2-platforms build) assumes."
HOMEPAGE = "https://github.com/tianocore/edk2-non-osi"

# edk2-non-osi carries no top-level License.txt on purpose -- it is a grab-bag
# of silicon-vendor binaries and sources, each module supplying its own
# License.txt, with the nearest one in the hierarchy taking precedence. Readme.md
# is where the repository states that policy, so that is what LIC_FILES_CHKSUM
# pins; the declared LICENSE is the family every module in it belongs to. Nothing
# out of this tree ends up in the firmware anyway (see DESCRIPTION).
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://Readme.md;md5=7683be75b315e148746417d1238d3157"

PV = "202509+git${SRCPV}"

SRC_URI = "git://github.com/tianocore/edk2-non-osi.git;protocol=https;branch=master;destsuffix=edk2-non-osi"

# The former NumberOneGit fork was upstream at this commit plus only an RPi5
# TrustedFirmware bl31.bin -- dead weight here, per the DESCRIPTION.
SRCREV = "94d048981116e2e3eda52dad1a89958ee404098d"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/edk2-non-osi"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach edk2-rpi5-firmware's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. edk2-rpi5-firmware reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi
    cp -a ${S}/. ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/
    rm -rf ${D}${EDK2_SOURCE_ROOT}/edk2-non-osi/.git
}

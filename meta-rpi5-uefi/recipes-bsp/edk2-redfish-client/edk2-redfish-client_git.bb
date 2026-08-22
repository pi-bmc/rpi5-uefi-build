SUMMARY = "tianocore edk2-redfish-client (RedfishClientPkg) source tree"
DESCRIPTION = "RedfishClientPkg -- the standard Redfish feature layer that sits on \
               top of edk2's RedfishPkg host-interface core. Staged into \
               ${datadir}/edk2/edk2-redfish-client as one of edk2-rpi5-firmware's \
               PACKAGES_PATH roots; that recipe's RPI5_REDFISH_CLIENT knob is what \
               actually builds anything out of it (via RpiRedfishPkg's \
               RpiRedfishClient.dsc.inc/.fdf.inc). Unpatched by this layer."
HOMEPAGE = "https://github.com/tianocore/edk2-redfish-client"

# Identical BSD-2-Clause-Patent text to edk2's License.txt, under a different
# filename.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://LICENSE;md5=2b415520383f7964e96700ae12b4570a"

PV = "202605+git${SRCPV}"

SRC_URI = "git://github.com/tianocore/edk2-redfish-client.git;protocol=https;branch=main;destsuffix=edk2-redfish-client"

# edk2-redfish-client tracks edk2 MASTER, and the edk2 pinned by
# edk2-rpi5-firmware is master-based too -- its RedfishPkg carries commits
# through 2026-02-03. That dates the compatibility window precisely (audited
# 2026-08-17):
#   >= 73a1eaa41 (2026-01-17): RedfishPlatformConfigSetValue grew a
#     by-pointer value argument in edk2 and the client the same day; an
#     older client passes by value and fails to compile against that edk2.
#   <  b8ffa6e45 (2026-05-05): the client's RedfishEventLib starts needing
#     gEdkIIRedfisEventRedfishInterfaceDisconnectionGuid, which that edk2's
#     RedfishPkg.dec predates (the GUID nuc-bios-build moved to edk2 master
#     for -- see edk2-uefipayload_2605.bb).
# This pin is the last commit before that boundary; every external GUID it
# references is declared by the pinned edk2's .dec files. Bumping it therefore
# means bumping edk2-rpi5-firmware's SRCREV in step -- the two are one
# compatibility pair, not two independent pins.
SRCREV = "a75f45cd69c74121fbf58900b9d92735d9a3373c"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/edk2-redfish-client"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach edk2-rpi5-firmware's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. edk2-rpi5-firmware reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client
    cp -a ${S}/. ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client/
    rm -rf ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client/.git
}

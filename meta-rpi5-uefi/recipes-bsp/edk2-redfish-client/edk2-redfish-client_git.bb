SUMMARY = "tianocore edk2-redfish-client (RedfishClientPkg) source tree"
DESCRIPTION = "RedfishClientPkg -- the standard Redfish feature layer that sits on \
               top of edk2's RedfishPkg host-interface core. Staged into \
               ${datadir}/edk2/edk2-redfish-client as one of rpi5-uefi-firmware's \
               PACKAGES_PATH roots. RPi5.dsc/.fdf list the feature drivers and schema \
               converters built out of it directly, unconditionally."
HOMEPAGE = "https://github.com/tianocore/edk2-redfish-client"

# Identical BSD-2-Clause-Patent text to edk2's License.txt, under a different
# filename.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://LICENSE;md5=2b415520383f7964e96700ae12b4570a"

PV = "202605+git${SRCPV}"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# 0001 keeps HiiToRedfishBootDxe's configure-language tags on the same
# ComputerSystem version RPi5.fdf builds a feature driver for. A feature
# driver looks its HII questions up by that exact string, so the two have to be
# bumped together or the platform silently loses its boot order.
# 0002 makes BiosDxe consume the BMC's pending settings even without a
# prior-boot ConfigLangMap history record -- upstream's "system reset
# detected" skip silently discards the operator's staged Bios attributes on
# any boot without one, and this board has no reset-to-defaults path that
# would make the staged set stale.
# 0003 fixes the published BiosAttributeRegistry: enumeration values go out
# as the schema's "Value" AttributeValue array (the off-schema "Values"
# string array upstream emits is eaten by the converter round-trip before
# the PUT, verified on hardware), and AttributeName is the bare attribute
# key rather than the full configure-language path.
SRC_URI = "git://github.com/tianocore/edk2-redfish-client.git;protocol=https;branch=main;destsuffix=edk2-redfish-client \
           file://0001-HiiToRedfishBootDxe-track-the-ComputerSystem-version-.patch \
           file://0002-BiosDxe-consume-pending-settings-without-a-history-r.patch \
           file://0003-BiosAttributeRegistry-emit-schema-shaped-Value-and-b.patch \
           "

# edk2-redfish-client tracks edk2 MASTER. The window below was measured
# (2026-08-17) against the master commit the edk2 recipe pinned at the time,
# whose RedfishPkg carried commits through 2026-02-03:
#   >= 73a1eaa41 (2026-01-17): RedfishPlatformConfigSetValue grew a
#     by-pointer value argument in edk2 and the client the same day; an
#     older client passes by value and fails to compile against that edk2.
#   <  b8ffa6e45 (2026-05-05): the client's RedfishEventLib starts needing
#     gEdkIIRedfisEventRedfishInterfaceDisconnectionGuid, which that edk2's
#     RedfishPkg.dec predates (the GUID nuc-bios-build moved to edk2 master
#     for -- see edk2-uefipayload_2605.bb).
# This pin is the last commit before that upper boundary.
#
# STALE NOTICE: the edk2 recipe has since moved to edk2-stable202608, whose
# RedfishPkg is months newer than the tree that window was measured against.
# The upper bound in particular (a GUID that edk2's RedfishPkg.dec did not yet
# declare) is very likely no longer binding, and a newer client may now be both
# possible and preferable. The two remain one compatibility pair -- the client
# compiles against edk2's RedfishPkg -- so re-measure the window before moving
# either, rather than trusting the dates above.
SRCREV = "a75f45cd69c74121fbf58900b9d92735d9a3373c"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S never expands and do_unpack
# fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/edk2-redfish-client"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach edk2's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. edk2 reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client
    cp -a ${S}/. ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client/
    rm -rf ${D}${EDK2_SOURCE_ROOT}/edk2-redfish-client/.git
}

SUMMARY = "tianocore edk2 source tree"
DESCRIPTION = "The edk2 core tree, fetched with its submodules, patched, and staged \
               into ${datadir}/edk2/edk2 as the first of the firmware build's \
               PACKAGES_PATH roots. Nothing is compiled here -- not even BaseTools, \
               which is host-native and therefore built inside rpi5-uefi-firmware's \
               own copy of this tree. \
\
               This recipe owns the edk2 tree and NOTHING else: no board \
               configuration, no key material, no build. Its three sibling source-tree \
               recipes -- edk2-platforms (the RPi5 port), edk2-non-osi (which also \
               carries the TF-A bl31.bin built by arm-trusted-firmware) and \
               edk2-redfish-client -- stage themselves the same way, into the same \
               root, so ${datadir}/edk2 in the sysroot IS the four-tree workspace \
               upstream rpi5-uefi's build.sh assembles by hand. rpi5-uefi-firmware is \
               what turns that workspace into armstub8-2712.bin."
HOMEPAGE = "https://github.com/tianocore/edk2"

LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

PV = "202608"

# Patch order matters and follows SRC_URI order. Every patch here applies to
# the edk2 tree: 0001-EDK2-Sd-Mmc-v4 (the retired NumberOneGit fork's only edk2
# commit), then 0100 (UsbNetwork point-to-point media), 0102 (quiesce the
# Redfish stack after provisioning -- the in-Setup gadget-detach
# use-after-free; NOT at ReadyToBoot, which races the client feature core and
# kills provisioning outright), 0103 (keep USB NICs out of BDS boot-option
# enumeration), 0104 (RELEASE-build JsonLib fix), 0105 (SnpDxe on a USB
# ancestor), 0106 (ConnectAll tracing) and 0107 (one Ethernet frame per NTB
# datagram).
#
# Patches to the OTHER trees live with those trees: the RPi5 port and its
# series in recipes-bsp/edk2-platforms, the HII-to-Redfish boot fix in
# recipes-bsp/edk2-redfish-client. Each recipe's header carries the ordering
# notes for its own series.
SRC_URI = "gitsm://github.com/tianocore/edk2.git;protocol=https;branch=master;destsuffix=edk2 \
           file://0001-EDK2-Sd-Mmc-v4.patch \
           file://0100-UsbNetwork-assume-media-on-a-point-to-point-gadget.patch \
           file://0102-RedfishConfigHandler-quiesce-the-Redfish-stack-after.patch \
           file://0103-UefiBootManagerLib-do-not-enumerate-USB-NICs-as-boot.patch \
           file://0104-JsonLib-fix-RELEASE-build-of-lex_unget_unsave.patch \
           file://0105-SnpDxe-accept-a-USB-ancestor-where-a-PCI-one-is-abse.patch \
           file://0106-UefiBootManagerLib-trace-each-handle-ConnectAll-visi.patch \
           file://0107-UsbCdcNcm-deliver-one-Ethernet-frame-per-NTB-datagra.patch \
           "

# edk2-stable202608, the August 2026 quarterly release tag. A release tag
# rather than a master commit: the tree is the one upstream tested and tagged,
# and the pin reads as a version rather than as a SHA nobody can date.
#
# This replaced a master pin (c4d29cb6, 2026-03-26) that had been chosen for
# byte-parity with the retired NumberOneGit fork -- the fork was that commit
# plus one, the SD fixup carried here as 0001-EDK2-Sd-Mmc-v4.patch. That parity
# argument retired with the fork; there is nothing left to be byte-identical to.
#
# It remains one half of a compatibility pair with edk2-redfish-client's
# SRCREV -- the client is built against THIS tree's RedfishPkg -- so read that
# recipe's header before moving either.
SRCREV = "2970e5699ba6267f3384ffab20f96647578aebc8"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S = "${UNPACKDIR}/edk2" never
# expands and do_unpack fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}/edk2"

# Source only: nothing is compiled and nothing is packaged, but the tree still
# has to reach the firmware recipe's sysroot, so do_populate_sysroot runs.
inherit allarch nopackages

# Where the tree lands in the sysroot. rpi5-uefi-firmware reads exactly this
# path under ${STAGING_DATADIR} -- keep the two in step.
EDK2_SOURCE_ROOT = "${datadir}/edk2"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    edk2_root="${D}${EDK2_SOURCE_ROOT}/edk2"

    install -d "$edk2_root"
    cp -a ${S}/. "$edk2_root/"

    # Drop the submodules OF submodules.
    #
    # gitsm checks submodules out recursively, so edk2's thirteen bring their
    # own along: openssl's interop and fuzzing dependencies (cloudflare-quiche,
    # pyca-cryptography, krb5, wycheproof, tlsfuzzer, fuzz/corpora ...),
    # libspdm's private copies of openssl and mbedtls, mipisyst's and the TPM
    # reference stack's test deps. That is 1.09 GB of a 1.6 GB tree, and not
    # one .inf, .dsc, .dec, .fdf or .inc anywhere in edk2 references a single
    # file inside any of them.
    #
    # libspdm is the case worth stating, because it is the biggest (629 MB) and
    # looks like a counterexample: SpdmDeviceSecretLibNull.inf does compile
    # libspdm/os_stub/spdm_device_secret_lib_null/lib.c. But that file is
    # libspdm's own, sitting beside the nested checkouts rather than inside
    # one -- os_stub/ exists precisely so an integrator can map SpdmCryptLib
    # onto its own crypto, which is what SecurityPkg does with BaseCryptLib.
    # The bundled os_stub/openssllib/openssl and os_stub/mbedtlslib/mbedtls are
    # the backends EDK2 replaces, so nothing builds them.
    #
    # Only the submodule DIRECTORIES go; everything else under a depth-1
    # submodule stays, which is what keeps that lib.c. The list is read out of
    # the .gitmodules files rather than written here, so it stays right when
    # upstream adds or drops one.
    gitmodule_paths='s/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p'
    for sub in $(sed -n "$gitmodule_paths" "$edk2_root/.gitmodules"); do
        [ -f "$edk2_root/$sub/.gitmodules" ] || continue
        for nested in $(sed -n "$gitmodule_paths" "$edk2_root/$sub/.gitmodules"); do
            [ -d "$edk2_root/$sub/$nested" ] || continue
            bbnote "edk2: dropping nested submodule checkout $sub/$nested"
            rm -rf "$edk2_root/$sub/$nested"
        done
    done

    # Build bookkeeping rather than source: quilt's .pc/ backups and the
    # "patches" symlink it points at ${WORKDIR}/patches (which would stage as a
    # dangling link), then every .git in the tree.
    rm -rf "$edk2_root/.pc" "$edk2_root/patches"

    # -prune so find does not try to descend into a directory rm just removed.
    # Recursive rather than just the top level because gitsm leaves one behind
    # in every submodule it checked out. Nothing in the EDK2 build shells out
    # to git.
    find "$edk2_root" -name .git -prune -exec rm -rf {} +
}

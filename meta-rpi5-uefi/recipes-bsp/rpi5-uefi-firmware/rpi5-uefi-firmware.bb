SUMMARY = "Raspberry Pi 5 UEFI firmware (armstub8-2712.bin / RPI_EFI.fd)"
DESCRIPTION = "Builds Platform/RaspberryPi/RPi5/RPi5.dsc out of the four-tree EDK2 \
               workspace its source recipes stage into ${datadir}/edk2 -- edk2 \
               (the core tree and its patches), edk2-platforms (the RPi5 port and its \
               series), edk2-non-osi (which carries the TF-A bl31.bin built by \
               arm-trusted-firmware, embedded as the FD.RPI_EFI region-0 payload) and \
               edk2-redfish-client. \
\
               This recipe fetches no source tree of its own. What it owns is the \
               BUILD: the host-native BaseTools, the feature wiring the RPi5.dsc/.fdf \
               pair needs for the optional module sets, the Redfish wire contract, the \
               Secure Boot default-key macros (whose certificates come from \
               rpi5-secureboot-keys), the FMP capsule signing identity, and the \
               deployed artifacts -- armstub8-2712.bin, the same bytes as RPI_EFI.fd, \
               plus config.txt, the build report and a signed RPi5Firmware.cap."
HOMEPAGE = "https://github.com/tianocore/edk2-platforms"

# Nothing of this recipe's own ends up in the firmware -- every line of source
# comes from one of the four staged trees, each of which declares its own
# LICENSE. What is built here is overwhelmingly edk2 and edk2-platforms, both
# BSD-2-Clause-Patent, so that is what the output is called. The checksum is
# against poky's copy of the licence text rather than a tree this recipe does
# not unpack.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause-Patent;md5=0518d409dae93098cca8dfa932f3ab1b"

# The four source trees, each fetched, patched and staged by its own recipe
# under ${STAGING_DATADIR}/edk2 -- see the EDK2_*_PATH block below and
# recipes-bsp/{edk2,edk2-platforms,edk2-non-osi,edk2-redfish-client}. TF-A is
# NOT listed here: arm-trusted-firmware is a dependency of edk2-non-osi, which
# files its bl31.bin at the path RPi5.dsc looks for by default.
DEPENDS = "edk2 edk2-platforms edk2-non-osi edk2-redfish-client"
DEPENDS += "acpica-native util-linux-native"
# The capsule signing keypair is generated, inspected and used to sign the
# capsule with openssl. Unconditional, because FMP is: see the RPI5_FMP_KEYDIR
# block.
DEPENDS += "openssl-native"
# The Secure Boot default key set -- this project's PK plus Microsoft's KEK/db
# CAs, staged under ${STAGING_DATADIR}/secureboot-keys. Conditional, and that
# is the point: with the knob off, the one part of this build that reaches the
# network for something other than a git tree is not in the dependency graph at
# all.
DEPENDS += "${@bb.utils.contains('RPI5_SECURE_BOOT_DEFAULT_KEYS', '1', 'rpi5-secureboot-keys', '', d)}"

# Not an upstream version: this is the firmware's own, and it is what
# RPI5_FW_VERSION and (via its leading digits) the FMP/ESRT version derive
# from. It tracks the edk2 recipe's release pin by convention, not by
# construction -- bump it when this firmware changes, whatever moved
# underneath it.
PV = "202608"

# Everything under files/ is build configuration for the trees above, not
# source: the DSC/FDF snippets that wire in the optional module sets, and the
# config.txt deployed beside the firmware.
SRC_URI = "file://config.txt \
           file://usbnet-dsc-snippet.inc \
           file://usbnet-fdf-snippet.fdf.inc \
           file://profiling-dsc-snippet.inc \
           file://profiling-fdf-snippet.fdf.inc \
           "

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR.
UNPACKDIR ?= "${WORKDIR}"

# There is no source tree to point S at -- the file:// entries above unpack
# straight into ${WORKDIR}, and the trees that ARE built live in
# EDK2_PATH/EDK2_PLATFORMS_PATH below. ${WORKDIR} is also WORKSPACE (see
# do_compile), so keeping S there means the two agree.
S = "${UNPACKDIR}"

# Where the four sibling recipes stage their trees; must match the
# EDK2_SOURCE_ROOT they install into. In the sysroot this directory IS a
# complete EDK2 workspace, laid out exactly as upstream rpi5-uefi's build.sh
# assembles one by hand.
EDK2_SOURCE_ROOT = "${STAGING_DATADIR}/edk2"

# Read straight out of the sysroot: the build never writes into either.
EDK2_NON_OSI_PATH = "${EDK2_SOURCE_ROOT}/edk2-non-osi"
EDK2_REDFISH_CLIENT_PATH = "${EDK2_SOURCE_ROOT}/edk2-redfish-client"

# The other two cannot be read in place, so they are copied beside each other
# under ${WORKDIR} -- which is also WORKSPACE, and that is what makes
# "-p edk2-platforms/Platform/..." resolve. Reasons differ:
#
#   edk2            do_compile builds BaseTools, which is host-native and
#                   writes its objects and binaries INTO the tree.
#   edk2-platforms  do_compile rewrites RPi5.dsc/.fdf in place to wire in the
#                   optional feature sets.
EDK2_SRC = "${EDK2_SOURCE_ROOT}/edk2"
EDK2_PATH = "${WORKDIR}/edk2"
EDK2_PLATFORMS_SRC = "${EDK2_SOURCE_ROOT}/edk2-platforms"
EDK2_PLATFORMS_PATH = "${WORKDIR}/edk2-platforms"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

# Wire edk2's own USB CDC-ECM/NCM/RNDIS class drivers (present in the
# pinned tree, not in RPi5.dsc) into the build, so a USB Ethernet gadget on
# an RP1 port becomes an SNP interface. The BMC's host-interface link rides
# the ncm.usb0 function of this -- the Redfish host interface has no link
# without it.
RPI5_USBNET ??= "1"

# Boot-time profiling: adds ShellPkg's Dp ("dump performance") dynamic
# command, so `dp` at the UEFI shell prints per-driver load/init times in
# milliseconds. The platform already records the measurements
# (PcdPerformanceLibraryPropertyMask|1 + DxeCorePerformanceLib on DxeCore
# in RPi5.dsc), so this only adds the reader -- and only the FV space it
# occupies, which is why it is off by default.
#
# Set it in local.conf/kas.yml (PROFILING_ENABLED = "1"), or per-invocation
# with `bitbake -R` / an env var exported through BB_ENV_PASSTHROUGH_ADDITIONS.
# Whatever the value, it reaches EDK2 as -D PROFILING_ENABLED=TRUE|FALSE,
# which is the macro the profiling-*-snippet files gate on.
PROFILING_ENABLED ??= "0"

# The wire contract with the BMC. RPi5.dec carries the documented defaults and
# RPi5.dsc the rest of the Redfish PCDs; do_compile appends overrides for the
# three below.
# RPI5_REDFISH_MAC is the gadget's host_addr (the MAC the Pi's NCM NIC
# comes up with) -- the BMC must present exactly this fixed value, or
# RedfishDiscoverDxe rejects the interface. Colon-separated, lowercase ok.
RPI5_REDFISH_MAC ??= "da:c0:ff:ee:10:02"
# HTTP Basic credentials for the BMC's Redfish service (nanokvm-app's
# CheckAuth). Set RPI5_REDFISH_USER to "" for a BMC with authentication
# disabled (the credential library then reports AuthMethodNone).
RPI5_REDFISH_USER ??= "admin"
RPI5_REDFISH_PASSWORD ??= "admin"

# UEFI Secure Boot. RPi5.dsc already carries a complete SECURE_BOOT_ENABLE
# story -- real AuthVariableLib instead of the Null one, SecureBootConfigDxe
# (the Setup pages for enrolling PK/KEK/db and the "Attempt Secure Boot"
# toggle), SecureBootDefaultKeysDxe, EnrollFromDefaultKeysApp, and
# DxeImageVerificationLib wired into SecurityStubDxe -- all of it behind
# !if blocks that default to FALSE upstream. This knob just turns it on.
#
# Two things follow from it:
#   * SecureBootToggleDxe's checkbox (and therefore the
#     /Bios/Attributes/SecureBoot Redfish attribute) only does anything with
#     this on: with AuthVariableLibNull the SecureBootEnable variable is not
#     interpreted by anyone.
#   * RedfishClientPkg's SecureBootDxe feature driver, which serves
#     Systems/1/SecureBoot, links SecureBootVariableLib either way --
#     RPi5.dsc maps it itself when this is FALSE.
#
# Size: enabling this pulls OpensslLib/BaseCryptLib into VariableRuntimeDxe
# and SecurityStubDxe. Check FVMAIN_COMPACT headroom in RPI_EFI.report.txt
# after the first build with it on.
RPI5_SECURE_BOOT ??= "1"

# Embed the default key set as PKDefault/KEKDefault/dbDefault. Without this the
# SECURE_BOOT_ENABLE build still boots, but comes up in Setup Mode with empty
# defaults and there is nothing for "Reset Secure Boot Keys" to enroll.
#
# The key set is a recipe of its own -- rpi5-secureboot-keys, which ships the
# PK, fetches Microsoft's six CA certificates and validates every one of them
# is a DER X.509 cert before staging it. See its README for what each
# certificate is, why both the 2011 and 2023 generations ship, and why there is
# no DBX. This knob only decides whether that recipe is depended on at all.
#
# Enrolling stays a deliberate act -- see the README. SecureBootDefaultKeysDxe
# only populates the *Default variables; the platform boots in Setup Mode
# until someone runs "Reset Secure Boot Keys" in Setup or
# EnrollFromDefaultKeysApp from the Shell.
RPI5_SECURE_BOOT_DEFAULT_KEYS ??= "1"

# Where rpi5-secureboot-keys stages the key set; must match the
# SECUREBOOT_KEYS_DIR it installs into.
SECUREBOOT_KEYS_DIR = "${STAGING_DATADIR}/secureboot-keys"

# Store UEFI variables in OP-TEE-mediated RPMB (StMM secure partition) instead
# of the FD-backed VarBlockServiceDxe: passes -D RPI5_OPTEE_VARS=TRUE, which in
# RPi5.dsc/.fdf swaps VarBlockServiceDxe + VariableRuntimeDxe for
# MmCommunicationOpteeDxe + VariableSmmRuntimeDxe: the store, its auth
# handling and fault-tolerant write run inside StMM, working directly on the
# FD NV window OP-TEE maps into the SP (no storage device, no OP-TEE storage
# traffic); the DXE side persists the window back to armstub8-2712.bin /
# RPI_EFI.fd on the boot FAT (VarStoreSync.c). Requires OP-TEE built with the
# StMM FV embedded -- set RPI5_OPTEE_STMM=1 in the optee-os recipe too.
RPI5_OPTEE_VARS ??= "1"

# Flipping this knob must rebuild the firmware: it changes the -D define that
# swaps the DXE variable stack in RPi5.dsc/.fdf. Make the build dependency
# explicit so a change in kas.yml/local.conf always invalidates do_compile
# rather than risking an sstate hit that ships the old variable stack.
do_compile[vardeps] += "RPI5_OPTEE_VARS"

# RELEASE, DEBUG or NOOPT, per RPi5.dsc's [Defines] BUILD_TARGETS.
RPI5_BUILD_TARGET ??= "RELEASE"

# Embedded in the built FD via PcdFirmwareVersionString; surfaced by UEFI's
# "Firmware Version" info and by `dmidecode`/`fwupdmgr` on the running OS.
RPI5_FW_VERSION ??= "${PV}"

# Firmware Management Protocol + ESRT + capsule application: a signed capsule
# applied through UpdateCapsule() replaces the image in place, and the running
# version shows up in ESRT and in the BMC's Redfish SoftwareInventory. FmpDxe
# and EsrtFmpDxe are listed in RPi5.dsc/.fdf like every other driver, so the
# feature is always built; the knobs below only configure it.
#
# It needs no configuration to work: with neither RPI5_FMP_CERT nor
# RPI5_FMP_KEY set, do_compile generates a self-signed capsule signing keypair
# under RPI5_FMP_KEYDIR and do_deploy emits a signed RPi5Firmware.cap alongside
# the firmware. Read the RPI5_FMP_KEYDIR comment before shipping anything built
# that way -- a generated key is convenient, not managed. See the README beside
# Rpi5FmpDeviceLib in the edk2-platforms recipe.
#
# What is NOT invented on your behalf is a certificate whose private key nobody
# holds: that would look like a working update path and be nothing of the sort,
# since FmpDxe authenticates every payload against the certificates in
# PcdFmpDevicePkcs7CertBufferXdr and applies nothing when the loop over
# candidate keys has no candidates.

# The integer version ESRT publishes and FmpDxe compares for anti-rollback --
# distinct from RPI5_FW_VERSION, which is the human-readable string. Derived
# from PV's leading numeric part (202602+git -> 202602). Carried both ways:
# into the firmware as PcdRpi5FirmwareVersion (what FmpDeviceGetVersion
# reports) and into the capsule as GenerateCapsule's --fw-version, so the
# capsule always declares the version of the image inside it.
#
# It should only ever increase. What actually enforces that is RPI5_FMP_LSV,
# not this: FmpDxe rejects an image whose version is below the lowest
# supported version, and compares nothing against the running one.
RPI5_FMP_VERSION ??= "${@d.getVar('PV').split('+')[0].replace('.', '') or '1'}"

# Anti-rollback floor written into the capsule as GenerateCapsule's --lsv.
#
# STICKY, and in a way that cannot be undone from the board: on a successful
# apply FmpDxe copies the payload's LSV into a variable and thereafter takes
# the max of that, PcdFmpDeviceBuildTimeLowestSupportedVersion and the device
# library's value (FmpDxe.c, SetLowestSupportedVersionInVariable). Shipping a
# capsule with an LSV above a release you may need to go back to makes that
# release unreachable by capsule for the life of the variable store. Leave it
# at 0 unless you mean to burn a downgrade bridge.
RPI5_FMP_LSV ??= "0"

# Extra GenerateCapsule --capflag arguments. Empty on purpose.
#
# The obvious candidate, PersistAcrossReset, does not work on this platform:
# CapsuleRuntimeDxe returns EFI_UNSUPPORTED for it unless
# PcdSupportUpdateCapsuleReset is TRUE, and RPi5 neither sets that PCD nor
# builds the CapsulePei/CapsuleOnDisk machinery that would coalesce a staged
# capsule after the reset. A capsule with no flags is applied immediately, in
# the boot services call itself, which is the path that works here -- and so
# also the one an OS-side fwupd cannot use.
RPI5_FMP_CAPSULE_FLAGS ??= ""

# DER certificate FmpDxe authenticates capsule payloads against, embedded in
# the firmware as PcdFmpDevicePkcs7CertBufferXdr.
#
# Leave both this and RPI5_FMP_KEY unset to have the build generate a keypair
# (see RPI5_FMP_KEYDIR). Set this ALONE when the private key lives somewhere
# this build cannot reach -- an HSM, another machine, a release process --
# and do_deploy will build the firmware but skip the capsule, with a warning
# saying so, leaving you to sign one offline.
RPI5_FMP_CERT ??= ""

# PEM file holding the capsule signing private key AND its certificate,
# concatenated. Both, in one file: GenerateCapsule signs by shelling out to
# `openssl smime -sign -signer <file>` with no -inkey, so a bare private key
# is not enough. do_deploy checks the certificate in here against the one the
# firmware embeds and fails on a mismatch, because a capsule signed by the
# wrong key of a matched pair fails authentication silently on the board.
#
# If this is set and RPI5_FMP_CERT is not, the DER the firmware embeds is
# derived from this file's certificate rather than asked for twice.
RPI5_FMP_KEY ??= ""

# Where a generated keypair lives, when neither RPI5_FMP_CERT nor
# RPI5_FMP_KEY is set. Four files: capsule.key (private key), capsule.crt
# (PEM certificate), capsule.cer (the same certificate as DER, embedded in
# the firmware) and capsule.pem (key + certificate, what signs capsules).
#
# Under TOPDIR rather than WORKDIR because the keypair has to OUTLIVE the
# build: a capsule is authenticated by the firmware ALREADY ON THE BOARD, so
# a key regenerated between releases produces capsules that fielded boards
# reject. Nothing is ever regenerated over an existing file, and a
# half-populated directory is an error rather than a silent re-key -- but
# `rm -rf` the build directory and the next build makes a new identity that
# no deployed board will trust. Point this somewhere backed up, or move to
# RPI5_FMP_CERT + RPI5_FMP_KEY with real key management, before this leaves a
# workbench. The private key is written unencrypted and mode 0600.
RPI5_FMP_KEYDIR ??= "${TOPDIR}/fmp-keys"

# Subject and lifetime of a generated certificate. Long-dated on purpose:
# renewing means reflashing every board that embeds the old certificate.
RPI5_FMP_SUBJECT ??= "/CN=pi-bmc RPi5 UEFI capsule signing/"
RPI5_FMP_CERT_DAYS ??= "7300"

# Escape hatch for one-off `-D FOO=BAR` / `--pcd ...` additions without
# having to override do_compile wholesale.
RPI5_EDK2_EXTRA_FLAGS ??= ""

# Private, writable copy of the edk2 tree.
#
# The staged tree is read-only and shared, and do_compile builds BaseTools --
# host-native C and Python tooling that lands inside it. Copied here rather
# than in do_compile because nothing in this recipe ever rewrites an edk2 file:
# unlike the edk2-platforms copy (see do_compile), there is no knob whose
# turning-off has to be undone, so once per configure is enough.
#
# --reflink=auto because this tree is ~520 MB even after the edk2 recipe drops
# gitsm's recursively-fetched depth-2 submodule checkouts. On a CoW filesystem
# the copy is metadata only; anywhere else it falls back to a real one, which
# is why do_configure is the only place it happens.
do_configure() {
    rm -rf "${EDK2_PATH}"
    mkdir -p "${EDK2_PATH}"
    cp -a --reflink=auto "${EDK2_SRC}/." "${EDK2_PATH}/"
}

# Generate the capsule signing keypair, once, if it is not already there.
#
# Deliberately all-or-nothing: a directory holding some of the four files is
# an error rather than something to top up. The four are one identity, and
# quietly re-deriving a missing piece is how you end up with a certificate in
# the firmware that no longer matches the key signing capsules for it -- a
# mismatch whose only symptom is that updates stop applying, on the board,
# with no message anyone sees.
#
# Shell locals go unbraced throughout these functions: bitbake expands ${...}
# in a task body against its own datastore before /bin/sh ever sees it, so a
# braced shell variable is one name collision away from being replaced with
# something else entirely.
rpi5_fmp_generate_keys() {
    fmp_keydir="${RPI5_FMP_KEYDIR}"

    # Counted with string accumulation rather than arithmetic: bitbake's shell
    # code parser raises NotImplementedError on $(( )) and the recipe will not
    # parse at all.
    fmp_present=""
    fmp_missing=""
    for f in capsule.key capsule.crt capsule.cer capsule.pem; do
        if [ -e "$fmp_keydir/$f" ]; then
            fmp_present="$fmp_present $f"
        else
            fmp_missing="$fmp_missing $f"
        fi
    done

    if [ -z "$fmp_missing" ]; then
        return 0
    fi

    if [ -n "$fmp_present" ]; then
        bbfatal "$fmp_keydir is a partial capsule signing key directory (has:$fmp_present, missing:$fmp_missing). Refusing to regenerate: the certificate and the key that signs capsules for it must stay one pair, and boards already carrying this certificate would reject capsules signed by a new one. Restore the missing files from wherever this keypair is kept, or move the directory aside to start a new identity -- and reflash every board that has the old one."
    fi

    mkdir -p "$fmp_keydir"

    # umask, not a later chmod: the private key must never exist, even for an
    # instant, at a mode another user on the build host could read.
    (umask 077 && openssl req -x509 -newkey rsa:2048 -nodes -sha256 \
        -days ${RPI5_FMP_CERT_DAYS} -subj "${RPI5_FMP_SUBJECT}" \
        -keyout "$fmp_keydir/capsule.key" -out "$fmp_keydir/capsule.crt") \
        || bbfatal "could not generate a capsule signing keypair in $fmp_keydir"

    # The DER form is what BinToPcd renders into the firmware; the PEM pair is
    # what "openssl smime -sign -signer" wants, key first.
    openssl x509 -in "$fmp_keydir/capsule.crt" -outform DER \
        -out "$fmp_keydir/capsule.cer" \
        || bbfatal "could not convert $fmp_keydir/capsule.crt to DER"
    (umask 077 && cat "$fmp_keydir/capsule.key" "$fmp_keydir/capsule.crt" \
        > "$fmp_keydir/capsule.pem") \
        || bbfatal "could not write $fmp_keydir/capsule.pem"
    chmod 0644 "$fmp_keydir/capsule.crt" "$fmp_keydir/capsule.cer"

    bbwarn "Generated a self-signed capsule signing keypair in $fmp_keydir. This is now the identity every board flashed with this firmware will trust for updates, and it is stored unencrypted in the build directory: back it up, and replace it with managed key material (RPI5_FMP_CERT + RPI5_FMP_KEY) before shipping. Losing it means no board flashed with this firmware can ever be capsule-updated again."
}

# Resolve the capsule certificate and signer into $fmp_cert and $fmp_signer
# for the caller. $fmp_signer comes back empty when the private key is
# deliberately elsewhere; that is the offline-signing case, not an error.
#
# Generation happens only when NEITHER was configured -- setting one of them
# means the operator has an identity of their own, and inventing a second one
# alongside it would be worse than useless.
rpi5_fmp_resolve_keys() {
    fmp_cert="${RPI5_FMP_CERT}"
    fmp_signer="${RPI5_FMP_KEY}"

    if [ -z "$fmp_cert" ] && [ -z "$fmp_signer" ]; then
        rpi5_fmp_generate_keys
        fmp_cert="${RPI5_FMP_KEYDIR}/capsule.cer"
        fmp_signer="${RPI5_FMP_KEYDIR}/capsule.pem"
    fi

    if [ -n "$fmp_signer" ] && [ ! -r "$fmp_signer" ]; then
        bbfatal "RPI5_FMP_KEY '$fmp_signer' is not readable."
    fi

    if [ -z "$fmp_cert" ]; then
        # A signer PEM alone is enough: the certificate the firmware embeds is
        # that PEM's certificate, so derive it instead of asking for the same
        # object twice and then having to check the two against each other.
        mkdir -p "${B}"
        fmp_cert="${B}/fmp-capsule-cert.der"
        openssl x509 -in "$fmp_signer" -outform DER -out "$fmp_cert" \
            || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no certificate. GenerateCapsule signs by running openssl smime -sign -signer <file> and passes no -inkey, so this file must contain the signing certificate as well as the private key -- concatenate them, key first."
    fi

    if [ ! -r "$fmp_cert" ]; then
        bbfatal "RPI5_FMP_CERT '$fmp_cert' is not readable."
    fi
}

do_compile() {
    # edk2-platforms arrives from its own recipe, read-only and shared, in the
    # sysroot. The feature wiring further down rewrites RPi5.dsc/.fdf in place,
    # so build from a private copy instead. Rebuilt from scratch every run --
    # unlike the edk2 copy do_configure makes, which nothing rewrites -- and
    # that is also what makes turning a knob back off (RPI5_USBNET=0, say)
    # actually take effect rather than leaving yesterday's !include lines
    # behind in a persisted checkout.
    rm -rf "${EDK2_PLATFORMS_PATH}"
    mkdir -p "${EDK2_PLATFORMS_PATH}"
    cp -a --reflink=auto "${EDK2_PLATFORMS_SRC}/." "${EDK2_PLATFORMS_PATH}/"

    cd "${EDK2_PATH}"

    # BaseTools are host-native; keep bitbake's cross toolchain env out of
    # their build, same as every other EDK2 recipe (see meta-nuc-bios's
    # edk2-uefipayload_2605.bb). They are built HERE rather than in the edk2
    # recipe because they are host binaries: that recipe stages an allarch
    # source tree and compiles nothing.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
    oe_runmake -C BaseTools CC=gcc CXX=g++

    # WORKSPACE is the parent of both EDK2_PATH (the edk2 copy, at
    # "${WORKSPACE}/edk2") and EDK2_PLATFORMS_PATH (the copy made above, at
    # "${WORKSPACE}/edk2-platforms") -- i.e. ${WORKDIR}. This matters:
    # RPi5.dsc/.fdf live in edk2-platforms, not in edk2 itself, so
    # "-p edk2-platforms/Platform/..." below only resolves if WORKSPACE is
    # their common parent, exactly mirroring upstream rpi5-uefi/build.sh's own
    # layout (WORKSPACE=repo root, with edk2/edk2-platforms as direct siblings
    # under it). edk2-non-osi and edk2-redfish-client need none of that: they
    # go on PACKAGES_PATH as absolute sysroot paths, which EDK2 resolves from
    # anywhere -- BaseTools' MultipleWorkspace.join() falls through WORKSPACE
    # to each PACKAGES_PATH entry in turn. That fallthrough is also how
    # RPi5.fdf's region-0 "FILE = Platform/RaspberryPi/RPi5/TrustedFirmware/
    # bl31.bin" finds the TF-A binary edk2-non-osi carries.
    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${EDK2_PATH}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}:${EDK2_REDFISH_CLIENT_PATH}"
    export EDK_TOOLS_PATH="${EDK2_PATH}/BaseTools"
    export CONF_PATH="${WORKDIR}/Conf"
    export PYTHON_COMMAND="python3"
    export PATH="${EDK2_PATH}/BaseTools/BinWrappers/PosixLike:${PATH}"

    mkdir -p "${CONF_PATH}"
    for f in target tools_def build_rule; do
        [ -e "${CONF_PATH}/${f}.txt" ] || cp "${EDK2_PATH}/BaseTools/Conf/${f}.template" "${CONF_PATH}/${f}.txt"
    done

    # Real AArch64 cross build (unlike BaseTools above): tools_def.txt's GCC
    # toolchain family invokes $(GCC_AARCH64_PREFIX)gcc/-ld/-objcopy directly,
    # not $CC, so leaving CC unset from the BaseTools step above is fine.
    export GCC_AARCH64_PREFIX="${TARGET_PREFIX}"

    # --- wire optional feature modules into RPi5.dsc / RPi5.fdf ---------
    # Sed insertions rather than patch files: RPi5.dsc/.fdf's exact upstream
    # whitespace/formatting isn't reliably knowable ahead of time, so all
    # insertions anchor on edk2's own well-known NetworkPkg include lines and
    # append pre-rendered snippet files with sed's "r" command. The \| |
    # address delimiters matter: the markers contain slashes.
    dsc="${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.dsc"
    fdf="${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.fdf"
    dsc_marker='!include NetworkPkg/Network.dsc.inc'
    fdf_marker='!include NetworkPkg/Network.fdf.inc'
    grep -qF "${dsc_marker}" "${dsc}" || \
        bbfatal "RPi5.dsc: '${dsc_marker}' not found -- edk2-platforms layout changed, update this recipe"
    grep -qF "${fdf_marker}" "${fdf}" || \
        bbfatal "RPi5.fdf: '${fdf_marker}' not found -- edk2-platforms layout changed, update this recipe"

    # The board's own drivers -- power button, Active Cooler, fan policy page,
    # bootloader-config provenance, the Secure Boot toggle, the Redfish stack
    # and FMP -- need nothing inserted here: RPi5.dsc/.fdf list them directly,
    # the way upstream lists RPi4's. Only edk2-tree modules (below) still get
    # sed-inserted, because their sources are not in edk2-platforms at all.
    #
    # Insertions that remain are grep-guarded: it costs nothing, and it keeps a
    # hand-edited or half-applied tree from ever producing duplicate includes,
    # which mean duplicate FFS files and a GenFv failure.

    # edk2's own USB CDC-ECM/NCM/RNDIS drivers (in-tree, unwired upstream).
    if [ "${RPI5_USBNET}" = "1" ]; then
        grep -qF 'UsbNetwork/NetworkCommon/NetworkCommon.inf' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${WORKDIR}/usbnet-dsc-snippet.inc" "${dsc}"
        grep -qF 'INF MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon/NetworkCommon.inf' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${WORKDIR}/usbnet-fdf-snippet.fdf.inc" "${fdf}"
    fi

    # ShellPkg's Dp profiling command. Unlike every other snippet here these
    # lines go in UNCONDITIONALLY -- they carry their own
    # "!if $(PROFILING_ENABLED) == TRUE" gate, and the macro below is what
    # actually turns Dp on or off, without the line set having to change.
    grep -qF 'DpDynamicCommand/DpDynamicCommand.inf' "${dsc}" || \
        sed -i "\|${dsc_marker}|r ${WORKDIR}/profiling-dsc-snippet.inc" "${dsc}"
    grep -qF 'INF ShellPkg/DynamicCommand/DpDynamicCommand/DpDynamicCommand.inf' "${fdf}" || \
        sed -i "\|${fdf_marker}|r ${WORKDIR}/profiling-fdf-snippet.fdf.inc" "${fdf}"

    if [ "${PROFILING_ENABLED}" = "1" ]; then
        profiling_define="-D PROFILING_ENABLED=TRUE"
        bbnote "profiling enabled: ShellPkg Dp command built into FVMAIN"
    else
        profiling_define="-D PROFILING_ENABLED=FALSE"
    fi

    # --- Redfish host-interface wire contract ----------------------------
    # RPi5.dsc builds the whole Redfish stack (host interface + the
    # edk2-redfish-client feature layer) unconditionally, and RPi5.dec carries
    # the contract's documented defaults. What the recipe still owns is the
    # three values an operator may need to change per deployment -- the NCM
    # gadget's MAC and the HTTP Basic credentials -- plus the RestEx device
    # path that has to match that MAC.
    #
    # They are appended as a fresh [PcdsFixedAtBuild.common] section at the END
    # of the DSC, the same way the FMP certificate below is appended and for
    # the same reason: a PCD section opened anywhere inside [Components.common]
    # would swallow every component after it.
    [ "${RPI5_USBNET}" = "1" ] || \
        bbwarn "RPI5_USBNET=0: the Redfish stack is built but has no NIC driver for the BMC link"

    mac_plain=$(printf '%s' "${RPI5_REDFISH_MAC}" | tr -d ':' | tr 'abcdef' 'ABCDEF')
    [ "$(printf '%s' "${mac_plain}" | wc -c)" = "12" ] || \
        bbfatal "RPI5_REDFISH_MAC '${RPI5_REDFISH_MAC}' is not a 6-octet MAC"
    mac_bytes=$(printf '%s' "${mac_plain}" | sed 's/../0x&, /g; s/, $//')

    redfish_marker='# Redfish wire contract, appended by the firmware recipe.'
    grep -qF "${redfish_marker}" "${dsc}" || {
        printf '\n#\n%s\n#\n[PcdsFixedAtBuild.common]\n' "${redfish_marker}" >> "${dsc}"
        printf '  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishGadgetMac|{%s}\n' "${mac_bytes}" >> "${dsc}"
        printf '  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishUser|"%s"\n' "${RPI5_REDFISH_USER}" >> "${dsc}"
        printf '  gRpiRedfishTokenSpaceGuid.PcdRpiRedfishPassword|"%s"\n' "${RPI5_REDFISH_PASSWORD}" >> "${dsc}"
        printf '  gEfiRedfishPkgTokenSpaceGuid.PcdRedfishRestExServiceDevicePath.DevicePath|{DEVICE_PATH("MAC(%s,0x1)")}\n' "${mac_plain}" >> "${dsc}"
    }

    # RedfishClientPkg's own includes gate on this macro; the client is always
    # built here, so it is always TRUE.
    redfish_client_define="-D REDFISH_CLIENT=TRUE"

    # --- FMP capsule update ---------------------------------------------
    # FmpDxe + EsrtFmpDxe, plus the certificate their capsule authentication
    # checks against. The certificate is not optional: FmpDxe walks the keys in
    # PcdFmpDevicePkcs7CertBufferXdr and applies nothing if there are none, so
    # firmware built without one advertises an ESRT update path that can never
    # succeed. Hence rpi5_fmp_resolve_keys, which always yields a certificate
    # -- generating a keypair if the operator configured none -- or stops the
    # build. The matching capsule is built in do_deploy.
    #
    # FmpDxe/EsrtFmpDxe themselves are listed in RPi5.dsc/.fdf like every other
    # driver; all that is left here is the certificate they authenticate
    # against, which cannot live in the tree.
    # Sets $fmp_cert (and $fmp_signer, which only do_deploy needs),
    # generating a keypair under RPI5_FMP_KEYDIR if none was configured.
    rpi5_fmp_resolve_keys

    # BinToPcd renders the DER certificate as the XDR-encoded VOID* PCD
    # FmpDevicePkg expects. Passing it through --pcd is not an option: it is
    # a multi-hundred-byte binary blob.
    cert_pcd="${B}/fmp-capsule-cert.pcd"
    python3 "${EDK2_PATH}/BaseTools/Scripts/BinToPcd.py" \
        -i "$fmp_cert" -x -o "${cert_pcd}" \
        -p gFmpDevicePkgTokenSpaceGuid.PcdFmpDevicePkcs7CertBufferXdr

    # The certificate PCD goes at the END of the DSC, not at the marker the
    # other snippets use. BinToPcd emits a bare PCD assignment with no
    # section header, and the marker sits inside [Components.common] -- an
    # opened [PcdsFixedAtBuild] there would swallow every component after
    # it. Appending a fresh section at end of file cannot do that.
    #
    # The re-run guard matches THIS marker line, not the PCD name: patch
    # 0018 names PcdFmpDevicePkcs7CertBufferXdr in a comment it adds to
    # RPi5.dsc, so a grep for the PCD name matches that comment on a clean
    # tree and skips the append entirely -- producing firmware whose
    # certificate buffer is empty, which is to say firmware that accepts
    # no capsule ever, with nothing said about it at build time.
    cert_marker='# Capsule signing certificate, appended by the firmware recipe.'
    grep -qF "${cert_marker}" "${dsc}" || {
        printf '\n#\n%s\n# FmpDxe authenticates every capsule payload against it.\n#\n[PcdsFixedAtBuild.common]\n' "${cert_marker}" >> "${dsc}"
        cat "${cert_pcd}" >> "${dsc}"
    }

    # Belt and braces, because the failure above is invisible from the
    # board: the assignment must be in the DSC and must carry bytes.
    grep -q 'PcdFmpDevicePkcs7CertBufferXdr|{0x' "${dsc}" || \
        bbfatal "the capsule signing certificate did not reach ${dsc}. Without it FmpDxe has no key to authenticate against and no capsule can ever be applied."

    fmp_pcds="--pcd gRpiFmpTokenSpaceGuid.PcdRpi5FirmwareVersion=${RPI5_FMP_VERSION}"

    # --- Secure Boot -----------------------------------------------------
    # SECURE_BOOT_ENABLE swaps RPi5.dsc onto the real AuthVariableLib and
    # brings in SecureBootConfigDxe, SecureBootDefaultKeysDxe and
    # DxeImageVerificationLib. DEFAULT_KEYS then embeds the key set;
    # ArmPlatformPkg/SecureBootDefaultKeys.fdf.inc (already !included by
    # RPi5.fdf inside its SECURE_BOOT_ENABLE guard) reads these five macros.
    #
    # NOTE: an undefined macro in an FDF/DSC !if evaluates to 0 rather than
    # erroring, so a typo here does not fail the build -- it silently ships
    # firmware with empty key defaults. Verify "PK Default"/"KEK Default"/
    # "DB Default" appear in RPI_EFI.report.txt after building.
    secure_boot_define=""
    if [ "${RPI5_SECURE_BOOT}" = "1" ]; then
        secure_boot_define="-D SECURE_BOOT_ENABLE=TRUE"

        if [ "${RPI5_SECURE_BOOT_DEFAULT_KEYS}" = "1" ]; then
            # Both halves come from rpi5-secureboot-keys' sysroot staging, in
            # the two subdirectories that record their provenance: pk/ ships in
            # this layer, ms/ was fetched and checksum-pinned. That recipe has
            # already checked every one of them is a DER X.509 certificate --
            # SecureBootVariableProvisionLib runs RsaGetPublicKeyFromX509()
            # over each SECTION RAW and says nothing when it fails -- so all
            # that is left here is to confirm the sysroot has what the macros
            # are about to name.
            pkdir="${SECUREBOOT_KEYS_DIR}/pk"
            certdir="${SECUREBOOT_KEYS_DIR}/ms"

            for k in "${pkdir}/PkDefault.der" \
                     "${certdir}/MicCorKEKCA2011.crt" "${certdir}/MicCorKEK2KCA2023.crt" \
                     "${certdir}/MicWinProPCA2011.crt" "${certdir}/MicCorUEFCA2011.crt" \
                     "${certdir}/WindowsUEFICA2023.crt" "${certdir}/MicrosoftUEFICA2023.crt"; do
                [ -f "$k" ] || \
                    bbfatal "RPI5_SECURE_BOOT_DEFAULT_KEYS=1 but $k is not in the sysroot -- rpi5-secureboot-keys did not stage it"
            done

            secure_boot_define="${secure_boot_define} -D DEFAULT_KEYS=TRUE"
            secure_boot_define="${secure_boot_define} -D PK_DEFAULT_FILE=${pkdir}/PkDefault.der"
            # KEK: both Microsoft generations (2011 expired 2026-06-24).
            secure_boot_define="${secure_boot_define} -D KEK_DEFAULT_FILE1=${certdir}/MicCorKEKCA2011.crt"
            secure_boot_define="${secure_boot_define} -D KEK_DEFAULT_FILE2=${certdir}/MicCorKEK2KCA2023.crt"
            # db: Windows + third-party (shim) CAs, both generations. Drop
            # the two UEFI CAs if this board must never boot shim/Linux.
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE1=${certdir}/MicWinProPCA2011.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE2=${certdir}/MicCorUEFCA2011.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE3=${certdir}/WindowsUEFICA2023.crt"
            secure_boot_define="${secure_boot_define} -D DB_DEFAULT_FILE4=${certdir}/MicrosoftUEFICA2023.crt"
        fi
    fi

    # UEFI variables in OP-TEE-mediated RPMB (StMM). Off by default; requires
    # the OP-TEE build to embed the StMM FV (RPI5_OPTEE_STMM=1 in optee-os).
    optee_vars_define=""
    if [ "${RPI5_OPTEE_VARS}" = "1" ]; then
        optee_vars_define="-D RPI5_OPTEE_VARS=TRUE"
    fi

    # No -D TFA_BUILD_ARTIFACTS: bl31.bin reaches the FD through edk2-non-osi,
    # at the path RPi5.dsc's !ifndef branch already names. See the
    # PACKAGES_PATH note above.
    build \
        -a AARCH64 -t GCC -b ${RPI5_BUILD_TARGET} \
        -p edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.dsc \
        -n ${@oe.utils.cpu_count()} \
        ${redfish_client_define} \
        ${profiling_define} \
        ${secure_boot_define} \
        ${optee_vars_define} \
        --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString=L"${RPI5_FW_VERSION}" \
        ${fmp_pcds} \
        ${RPI5_EDK2_EXTRA_FLAGS} \
        -y ${B}/RPI_EFI.report.txt

    [ -f "${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" ] || \
        bbfatal "edk2 build produced no RPI_EFI.fd -- see ${B}/RPI_EFI.report.txt"

    # Getting the assignment into the DSC is necessary but not sufficient; what
    # decides whether updates work is the value the build actually resolved.
    # The report is the only place that shows it, and an empty buffer there is
    # firmware advertising an ESRT update path that can never succeed -- which
    # nothing on the board will ever tell anyone. Same reasoning as the
    # "PK Default"/"KEK Default" note in the Secure Boot block above, except
    # this one is cheap enough to check rather than leave to a human.
    if grep -q 'PcdFmpDevicePkcs7CertBufferXdr.*= {0x0}' "${B}/RPI_EFI.report.txt"; then
        bbfatal "the build resolved PcdFmpDevicePkcs7CertBufferXdr to an empty buffer, so FmpDxe would have no certificate to authenticate capsules against and nothing could ever be applied. See ${B}/RPI_EFI.report.txt."
    fi
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd ${DEPLOYDIR}/RPI_EFI.fd
    # Same bytes under the BCM2712 default armstub filename: the VPU
    # bootloader auto-loads armstub8-2712.bin from the boot partition at
    # address 0x0 (= PcdFdBaseAddress), so config.txt needs no armstub=
    # line. RPI_EFI.fd is kept for anyone following the upstream
    # rpi5-uefi install flow with an explicit armstub= entry.
    install -m 0644 ${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd ${DEPLOYDIR}/armstub8-2712.bin
    install -m 0644 ${WORKDIR}/config.txt ${DEPLOYDIR}/config.txt
    install -m 0644 ${B}/RPI_EFI.report.txt ${DEPLOYDIR}/RPI_EFI.report.txt

    # --- signed capsule ---------------------------------------------------
    # The same FD, wrapped in the UEFI capsule headers and PKCS#7-signed with
    # the key whose certificate the firmware above embeds. What it updates is
    # the firmware region only: Rpi5FmpDeviceLib stops the write at
    # PcdNvStorageVariableBase, so the variable store, the Secure Boot keys
    # and every Setup choice survive being updated over.
    #
    # NOT byte-reproducible, and cannot be made so from here: GenerateCapsule
    # signs by calling `openssl smime -sign`, which stamps a signingTime
    # attribute into the PKCS#7 and offers no way to suppress it short of
    # -noattr, which the script never passes. Two builds of identical inputs
    # produce capsules that differ in those few bytes. Nothing downstream
    # cares -- the capsule is not packaged, and FmpAuthenticationLibPkcs7
    # ignores signingTime -- but a diff of two DEPLOYDIRs will show it.
    rpi5_fmp_resolve_keys

    if [ -z "$fmp_signer" ]; then
        bbwarn "RPI5_FMP_CERT is set but RPI5_FMP_KEY is not, so no capsule was built -- the private key is not available to this build. Sign one offline against ${DEPLOYDIR}/RPI_EFI.fd with edk2's BaseTools/Source/Python/Capsule/GenerateCapsule.py; see the README beside Rpi5FmpDeviceLib in the edk2-platforms recipe for the arguments, which must match this build's --fw-version ${RPI5_FMP_VERSION} and --lsv ${RPI5_FMP_LSV}."
    else
        # One source of truth for the image type GUID: read it back out of
        # the FmpDxe component block in RPi5.dsc that put it in the
        # firmware. Duplicating the literal here is exactly the drift that
        # block warns about -- a capsule built under a different GUID names
        # a device that is not this one, and is refused with nothing to say
        # why.
        fmp_guid=$(sed -n 's/.*PcdFmpDeviceImageTypeIdGuid[[:space:]]*|[[:space:]]*{GUID("\([0-9A-Fa-f-]\{36\}\)").*/\1/p' \
            "${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.dsc")
        if [ -z "$fmp_guid" ]; then
            bbfatal "could not read PcdFmpDeviceImageTypeIdGuid out of RPi5.dsc -- if the PCD line was reformatted, fix this sed rather than hardcoding the GUID."
        fi

        # The signer's certificate must be the one the firmware trusts.
        # Two files that were never a pair produce a capsule that builds,
        # deploys and installs perfectly, then fails authentication on the
        # board with an error nobody is watching for.
        fmp_cert_pub=$(openssl x509 -inform DER -in "$fmp_cert" -noout -pubkey) \
            || bbfatal "'$fmp_cert' is not a DER X.509 certificate."
        fmp_signer_pub=$(openssl x509 -in "$fmp_signer" -noout -pubkey) \
            || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no certificate. It must contain the signing certificate as well as the private key -- concatenate them, key first."
        if [ "$fmp_cert_pub" != "$fmp_signer_pub" ]; then
            bbfatal "RPI5_FMP_KEY '$fmp_signer' does not hold the certificate in '$fmp_cert'. Capsules signed with that key would be rejected by this firmware."
        fi
        # -passin pass: turns an encrypted key into a failure here rather
        # than a prompt inside GenerateCapsule, which has no way to answer
        # one and would sit waiting on a terminal bitbake does not give it.
        openssl pkey -in "$fmp_signer" -passin pass: -noout >/dev/null 2>&1 \
            || bbfatal "RPI5_FMP_KEY '$fmp_signer' holds no usable private key -- either it has only the certificate, or the key is passphrase-encrypted, which openssl smime -sign cannot be given a passphrase for from here. Decrypt it into a build-local copy, or sign offline and set RPI5_FMP_CERT alone."

        # GenerateCapsule wants PEM for --other-public-cert and
        # --trusted-public-cert, and both are mandatory even for a
        # self-signed certificate that is its own chain and its own
        # anchor. Derive them from the DER the firmware embeds, so what
        # signs is checked against what verifies.
        fmp_cert_pem="${B}/fmp-capsule-cert.pem"
        openssl x509 -inform DER -in "$fmp_cert" -out "$fmp_cert_pem" \
            || bbfatal "could not convert '$fmp_cert' to PEM"

        # Run the script directly rather than through the BinWrappers
        # PosixLike wrapper: the wrapper is what sets PYTHONPATH, and it
        # is not on PATH here the way it is inside do_compile's build env.
        # --signing-tool-path pins openssl to the native sysroot's, the
        # one every check above used, instead of whatever is on PATH.
        PYTHONPATH="${EDK2_PATH}/BaseTools/Source/Python" python3 \
            "${EDK2_PATH}/BaseTools/Source/Python/Capsule/GenerateCapsule.py" -e \
            --guid "$fmp_guid" \
            --fw-version ${RPI5_FMP_VERSION} \
            --lsv ${RPI5_FMP_LSV} \
            ${RPI5_FMP_CAPSULE_FLAGS} \
            --signer-private-cert "$fmp_signer" \
            --other-public-cert "$fmp_cert_pem" \
            --trusted-public-cert "$fmp_cert_pem" \
            --signing-tool-path "${STAGING_BINDIR_NATIVE}" \
            -o "${DEPLOYDIR}/RPi5Firmware.cap" \
            "${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" \
            || bbfatal "GenerateCapsule failed to build ${DEPLOYDIR}/RPi5Firmware.cap"

        chmod 0644 "${DEPLOYDIR}/RPi5Firmware.cap"
        bbnote "Built ${DEPLOYDIR}/RPi5Firmware.cap: image type $fmp_guid, version ${RPI5_FMP_VERSION}, lsv ${RPI5_FMP_LSV}."
    fi
}

addtask deploy after do_compile

do_install[noexec] = "1"

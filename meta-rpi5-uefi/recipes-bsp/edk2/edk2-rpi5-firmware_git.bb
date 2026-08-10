SUMMARY = "EDK2 UEFI firmware (RPI_EFI.fd) for the Raspberry Pi 5"
DESCRIPTION = "Builds Platform/RaspberryPi/RPi5/RPi5.dsc from edk2-platforms \
               against NumberOneGit's edk2 (upstream tianocore/edk2 fork carrying whatever \
               core changes the RPi5 D0 port currently needs) and edk2-non-osi (silicon- \
               vendor binary blobs edk2-platforms links against), then embeds TF-A's bl31.bin \
               (see the arm-trusted-firmware recipe) as the FD.RPI_EFI region-0 payload, and, \
               if RPI5_IPXE is enabled, an iPXE UNDI/SNP driver (see the ipxe-efi recipe) as \
               a prebuilt DXE driver -- giving RPi5's PXE/HTTP boot stack (edk2's own \
               NetworkPkg, wired in by RPi5.fdf, has zero NIC drivers of its own) something \
               to actually bind a network interface to."
HOMEPAGE = "https://github.com/NumberOneGit/rpi5-uefi"

# edk2 + edk2-platforms are both BSD-2-Clause-Patent (identical License.txt).
# edk2-non-osi carries no single root license file -- it's a grab-bag of
# silicon-vendor binaries/sources, each under its own BSD-2-Clause-Patent-
# family license text embedded in the relevant .inf/source comments, so
# there's nothing meaningful to point LIC_FILES_CHKSUM at there.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

DEPENDS = "acpica-native arm-trusted-firmware util-linux-native"
DEPENDS += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi', '', d)}"

PV = "202405+git${SRCPV}"

SRC_URI = "gitsm://github.com/NumberOneGit/edk2.git;protocol=https;branch=master;name=edk2;destsuffix=git \
           git://github.com/NumberOneGit/edk2-platforms.git;protocol=https;branch=master;name=platforms;destsuffix=edk2-platforms \
           git://github.com/NumberOneGit/edk2-non-osi.git;protocol=https;branch=master;name=nonosi;destsuffix=edk2-non-osi \
           file://config.txt \
           file://ipxe-fdf-snippet.fdf.inc \
           "
# All three pinned to the exact commits NumberOneGit/rpi5-uefi's own
# submodules point at (see that repo's git tree), i.e. what its published
# images are actually built from.
SRCREV_edk2 = "15590903fe016afc6c1a26300caddcdfd0c99090"
SRCREV_platforms = "4e426104a1f6371484f417650e339a43480cf701"
SRCREV_nonosi = "07fe302e6eaff27b4afaef5eb868c6759923ba45"
SRCREV_FORMAT = "edk2_platforms_nonosi"

EDK2_PLATFORMS_PATH = "${UNPACKDIR}/edk2-platforms"
EDK2_NON_OSI_PATH = "${UNPACKDIR}/edk2-non-osi"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

S = "${UNPACKDIR}/git"

do_compile[depends] += "arm-trusted-firmware:do_deploy"
do_compile[depends] += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi:do_deploy', '', d)}"

# Embed the iPXE UNDI/SNP driver by default -- see ipxe-efi_git.bb's
# DESCRIPTION for exactly what this does and does not cover (add-on NIC PXE
# boot: yes; RP1's onboard Ethernet specifically: no). Set to "0" to build a
# plain, unmodified RPi5.fdf.
RPI5_IPXE ??= "1"

# RELEASE, DEBUG or NOOPT, per RPi5.dsc's [Defines] BUILD_TARGETS.
RPI5_BUILD_TARGET ??= "RELEASE"

# Embedded in the built FD via PcdFirmwareVersionString; surfaced by UEFI's
# "Firmware Version" info and by `dmidecode`/`fwupdmgr` on the running OS.
RPI5_FW_VERSION ??= "${PV}"

# Escape hatch for one-off `-D FOO=BAR` / `--pcd ...` additions without
# having to override do_compile wholesale.
RPI5_EDK2_EXTRA_FLAGS ??= ""

do_configure[noexec] = "1"

# The GUID below is only used to identify this one prebuilt-driver FFS file
# within RPi5.fdf; it has no meaning outside this build (freshly generated,
# not reused from anywhere else).
IPXE_DRIVER_FILE_GUID = "c3e36d1a-8f42-4b3e-9a5d-2f6c7b8e9a10"

do_compile() {
    cd ${S}

    # BaseTools are host-native; keep bitbake's cross toolchain env out of
    # their build, same as every other EDK2 recipe (see meta-nuc-bios's
    # edk2-uefipayload_2605.bb).
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
    oe_runmake -C BaseTools CC=gcc CXX=g++

    # WORKSPACE is the parent of both ${S} (this edk2 checkout, at
    # "${WORKSPACE}/git") and EDK2_PLATFORMS_PATH/EDK2_NON_OSI_PATH (fetched
    # as WORKDIR siblings, at "${WORKSPACE}/edk2-platforms" and
    # "${WORKSPACE}/edk2-non-osi") -- i.e. ${WORKDIR}, NOT ${S}. This matters:
    # RPi5.dsc/.fdf live in edk2-platforms, not in edk2 itself, so "-p
    # edk2-platforms/Platform/..." below only resolves if WORKSPACE is their
    # common parent, exactly mirroring upstream rpi5-uefi/build.sh's own
    # layout (WORKSPACE=repo root, with edk2/edk2-platforms/edk2-non-osi as
    # direct siblings under it).
    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${S}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}"
    export EDK_TOOLS_PATH="${S}/BaseTools"
    export CONF_PATH="${WORKDIR}/Conf"
    export PYTHON_COMMAND="python3"
    export PATH="${S}/BaseTools/BinWrappers/PosixLike:${PATH}"

    mkdir -p "${CONF_PATH}"
    for f in target tools_def build_rule; do
        [ -e "${CONF_PATH}/${f}.txt" ] || cp "${S}/BaseTools/Conf/${f}.template" "${CONF_PATH}/${f}.txt"
    done

    # Real AArch64 cross build (unlike BaseTools above): tools_def.txt's GCC
    # toolchain family invokes $(GCC_AARCH64_PREFIX)gcc/-ld/-objcopy directly,
    # not $CC, so leaving CC unset from the BaseTools step above is fine.
    export GCC_AARCH64_PREFIX="${TARGET_PREFIX}"

    # --- embed the iPXE UNDI/SNP driver, if built -----------------------
    # ipxe-efi's do_deploy publishes bin-arm64-efi/ipxe.efidrv to
    # DEPLOY_DIR_IMAGE; wire it into the DXE firmware volume as a prebuilt
    # driver FFS file, right after the point where RPi5.fdf pulls in edk2's
    # own NetworkPkg PXE/HTTP stack -- that stack has no NIC of its own, and
    # neither does Rp1BusDxe, so without an SNP provider PXE has nothing to
    # bind to.
    #
    # This is a sed insertion rather than a proper patch file: RPi5.fdf's
    # exact upstream whitespace/formatting isn't reliably knowable ahead of
    # time (only reviewed via a web-rendered copy), so a hand-written unified
    # diff risks failing to apply on a real checkout. Anchoring on the exact,
    # well-known EDK2 include line below, and appending the pre-rendered
    # snippet file with sed's "r" command, avoids that risk entirely (no
    # multi-line shell/sed escaping to get wrong).
    if [ "${RPI5_IPXE}" = "1" ]; then
        fdf="${EDK2_PLATFORMS_PATH}/Platform/RaspberryPi/RPi5/RPi5.fdf"
        marker='!include NetworkPkg/Network.fdf.inc'
        grep -qF "${marker}" "${fdf}" || \
            bbfatal "RPi5.fdf: '${marker}' not found -- edk2-platforms layout changed, update this recipe"

        snippet="${B}/ipxe-fdf-snippet.fdf.inc"
        sed \
            -e "s|@IPXE_DRIVER_FILE_GUID@|${IPXE_DRIVER_FILE_GUID}|" \
            -e "s|@IPXE_EFIDRV_PATH@|${DEPLOY_DIR_IMAGE}/ipxe.efidrv|" \
            "${WORKDIR}/ipxe-fdf-snippet.fdf.inc" > "${snippet}"
        sed -i "/${marker}/r ${snippet}" "${fdf}"
    fi

    build \
        -a AARCH64 -t GCC -b ${RPI5_BUILD_TARGET} \
        -p edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.dsc \
        -n ${@oe.utils.cpu_count()} \
        -D TFA_BUILD_ARTIFACTS=${DEPLOY_DIR_IMAGE} \
        --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString=L"${RPI5_FW_VERSION}" \
        ${RPI5_EDK2_EXTRA_FLAGS} \
        -y ${B}/RPI_EFI.report.txt

    [ -f "${S}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" ] || \
        bbfatal "edk2 build produced no RPI_EFI.fd -- see ${B}/RPI_EFI.report.txt"
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${S}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd ${DEPLOYDIR}/RPI_EFI.fd
    install -m 0644 ${WORKDIR}/config.txt ${DEPLOYDIR}/config.txt
    install -m 0644 ${B}/RPI_EFI.report.txt ${DEPLOYDIR}/RPI_EFI.report.txt
}

addtask deploy after do_compile

do_install[noexec] = "1"

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
           file://0001-Rp1BusDxe-register-GEM-and-I2C1-vendor-devices.patch;patchdir=../edk2-platforms \
           file://RpiBmcPkg \
           file://Rp1GemPkg \
           file://usbnet-dsc-snippet.inc \
           file://usbnet-fdf-snippet.fdf.inc \
           "
# All three pinned to the exact commits NumberOneGit/rpi5-uefi's own
# submodules point at (see that repo's git tree), i.e. what its published
# images are actually built from.
SRCREV_edk2 = "15590903fe016afc6c1a26300caddcdfd0c99090"
SRCREV_platforms = "4e426104a1f6371484f417650e339a43480cf701"
SRCREV_nonosi = "07fe302e6eaff27b4afaef5eb868c6759923ba45"
SRCREV_FORMAT = "edk2_platforms_nonosi"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S = "${UNPACKDIR}/git" never
# expands and do_unpack fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

EDK2_PLATFORMS_PATH = "${UNPACKDIR}/edk2-platforms"
EDK2_NON_OSI_PATH = "${UNPACKDIR}/edk2-non-osi"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

inherit deploy

S = "${UNPACKDIR}/git"

do_compile[depends] += "arm-trusted-firmware:do_deploy"
do_compile[depends] += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi:do_deploy', '', d)}"

# iPXE embedding, now OFF by default: the onboard RJ45 PXE/HTTP-boots
# natively via Rp1GemDxe (RPI5_RP1_ETH) + NetworkPkg's own UefiPxeBcDxe, so
# iPXE's only remaining coverage is add-on NICs (a PCIe card on the FPC, or
# a USB dongle from iPXE's driver table). Set to "1" to embed it for those.
RPI5_IPXE ??= "0"

# Native RP1 GEM (onboard RJ45) SNP driver, built from the local Rp1GemPkg
# source package. Coexists with iPXE: disjoint hardware, both SNPs feed the
# same NetworkPkg stack.
RPI5_RP1_ETH ??= "1"

# BMC-integration driver set (local RpiBmcPkg): RP1 I2C1 master, EEPROM-backed
# UEFI variable sync (UbEfiVa blob at 0x0000), SMBIOS mirror (0x6000), block
# inventory (0x6800), BootloaderConfig publishing -- the host side of the
# shared-24c256 contract with the BMC (see RpiBmcPkg.dec's region map).
RPI5_BMC ??= "1"

# Wire edk2's own USB CDC-ECM/NCM/RNDIS class drivers (present in the pinned
# tree, not in RPi5.dsc) into the build, so a BMC g_ether/f_ecm gadget on an
# RP1 USB port becomes a bootable SNP interface.
RPI5_USBNET ??= "1"

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
    # Local out-of-tree EDK2 packages (RpiBmcPkg, Rp1GemPkg), staged into a
    # dedicated PACKAGES_PATH root so EDK2's path resolution sees exactly the
    # two package dirs and nothing else from WORKDIR.
    local_pkgs="${B}/edk2-local-pkgs"
    rm -rf "${local_pkgs}"
    mkdir -p "${local_pkgs}"
    cp -r "${WORKDIR}/RpiBmcPkg" "${WORKDIR}/Rp1GemPkg" "${local_pkgs}/"

    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${S}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}:${local_pkgs}"
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

    # BMC-integration driver set (local RpiBmcPkg source package).
    # Every insertion below is grep-guarded so a re-run of do_compile against
    # the persisted WORKDIR checkout stays idempotent -- duplicate includes
    # mean duplicate FFS files and a GenFv failure.
    if [ "${RPI5_BMC}" = "1" ]; then
        printf '%s\n' '!include RpiBmcPkg/RpiBmc.dsc.inc' > "${B}/rpibmc-dsc-line.inc"
        printf '%s\n' '!include RpiBmcPkg/RpiBmc.fdf.inc' > "${B}/rpibmc-fdf-line.inc"
        grep -qF 'RpiBmcPkg/RpiBmc.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rpibmc-dsc-line.inc" "${dsc}"
        grep -qF 'RpiBmcPkg/RpiBmc.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rpibmc-fdf-line.inc" "${fdf}"
    fi

    # Native RP1 GEM onboard-Ethernet SNP driver (local Rp1GemPkg).
    if [ "${RPI5_RP1_ETH}" = "1" ]; then
        printf '%s\n' '!include Rp1GemPkg/Rp1Gem.dsc.inc' > "${B}/rp1gem-dsc-line.inc"
        printf '%s\n' '!include Rp1GemPkg/Rp1Gem.fdf.inc' > "${B}/rp1gem-fdf-line.inc"
        grep -qF 'Rp1GemPkg/Rp1Gem.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rp1gem-dsc-line.inc" "${dsc}"
        grep -qF 'Rp1GemPkg/Rp1Gem.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rp1gem-fdf-line.inc" "${fdf}"
    fi

    # edk2's own USB CDC-ECM/NCM/RNDIS drivers (in-tree, unwired upstream).
    if [ "${RPI5_USBNET}" = "1" ]; then
        grep -qF 'UsbNetwork/NetworkCommon/NetworkCommon.inf' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${WORKDIR}/usbnet-dsc-snippet.inc" "${dsc}"
        grep -qF 'INF MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon/NetworkCommon.inf' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${WORKDIR}/usbnet-fdf-snippet.fdf.inc" "${fdf}"
    fi

    # --- embed the iPXE UNDI/SNP driver, if built -----------------------
    # ipxe-efi's do_deploy publishes bin-arm64-efi/ipxe.efidrv to
    # DEPLOY_DIR_IMAGE; wire it into the DXE firmware volume as a prebuilt
    # driver FFS file, right after the point where RPi5.fdf pulls in edk2's
    # own NetworkPkg PXE/HTTP stack. Covers add-on PCIe/USB NICs iPXE
    # recognises; the onboard RJ45 is Rp1GemDxe's job (see RPI5_RP1_ETH).
    if [ "${RPI5_IPXE}" = "1" ]; then
        snippet="${B}/ipxe-fdf-snippet.fdf.inc"
        sed \
            -e "s|@IPXE_DRIVER_FILE_GUID@|${IPXE_DRIVER_FILE_GUID}|" \
            -e "s|@IPXE_EFIDRV_PATH@|${DEPLOY_DIR_IMAGE}/ipxe.efidrv|" \
            "${WORKDIR}/ipxe-fdf-snippet.fdf.inc" > "${snippet}"
        grep -qF "${IPXE_DRIVER_FILE_GUID}" "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${snippet}" "${fdf}"
    fi

    build \
        -a AARCH64 -t GCC -b ${RPI5_BUILD_TARGET} \
        -p edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.dsc \
        -n ${@oe.utils.cpu_count()} \
        -D TFA_BUILD_ARTIFACTS=${DEPLOY_DIR_IMAGE} \
        --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString=L"${RPI5_FW_VERSION}" \
        ${RPI5_EDK2_EXTRA_FLAGS} \
        -y ${B}/RPI_EFI.report.txt

    [ -f "${WORKDIR}/Build/RPi5/${RPI5_BUILD_TARGET}_GCC/FV/RPI_EFI.fd" ] || \
        bbfatal "edk2 build produced no RPI_EFI.fd -- see ${B}/RPI_EFI.report.txt"
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
}

addtask deploy after do_compile

do_install[noexec] = "1"

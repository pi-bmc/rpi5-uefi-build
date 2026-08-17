SUMMARY = "EDK2 UEFI firmware (RPI_EFI.fd) for the Raspberry Pi 5"
DESCRIPTION = "Builds Platform/RaspberryPi/RPi5/RPi5.dsc against UPSTREAM \
               tianocore trees. The former NumberOneGit forks are fully decomposed into \
               this layer: their edk2 delta is one commit (0001-EDK2-Sd-Mmc-v4.patch on \
               the fork's exact master merge-base), and their edk2-platforms RPi5 port is \
               files/edk2-platforms/ (the port's ADDED files, copied in after unpack) \
               plus 0000-edk2-platforms-RPi5-port.patch (its changes to files that exist \
               upstream) -- reconstructed byte-identical to the fork within every package \
               tree this build compiles. TF-A's bl31.bin (see the arm-trusted-firmware \
               recipe) is embedded as the FD.RPI_EFI region-0 payload, and, if RPI5_IPXE \
               is enabled, an iPXE UNDI/SNP driver (see the ipxe-efi recipe) rides along \
               as a prebuilt DXE driver."
HOMEPAGE = "https://github.com/tianocore/edk2"

# edk2 + edk2-platforms are both BSD-2-Clause-Patent (identical License.txt).
# edk2-non-osi carries no single root license file -- it's a grab-bag of
# silicon-vendor binaries/sources, each under its own BSD-2-Clause-Patent-
# family license text embedded in the relevant .inf/source comments, so
# there's nothing meaningful to point LIC_FILES_CHKSUM at there.
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://License.txt;md5=2b415520383f7964e96700ae12b4570a"

DEPENDS = "acpica-native arm-trusted-firmware util-linux-native"
DEPENDS += "${@bb.utils.contains('RPI5_IPXE', '1', 'ipxe-efi', '', d)}"

PV = "202602+git${SRCPV}"

# Patch order matters and follows SRC_URI order:
#   edk2 tree:            0001-EDK2-Sd-Mmc-v4 (the former fork's only commit),
#                         then 0100 (UsbNetwork point-to-point media).
#   edk2-platforms tree:  file://edk2-platforms (the RPi5 port's ADDED files)
#                         merges INTO the git checkout at
#                         ${UNPACKDIR}/edk2-platforms during do_unpack, then
#                         0000 (the former fork's changed files), then this
#                         layer's own 0001..0005 -- which edit files the
#                         merged tree adds.
#
# ORDERING IS LOAD-BEARING: do_unpack processes SRC_URI entries in listing
# order, and the git fetcher PRUNES its destsuffix dir before checkout --
# the file://edk2-platforms entry must therefore stay AFTER the git entry
# of the same name, or the checkout wipes the port's added files (the
# patch series then fails loudly at 0000/0001).
SRC_URI = "gitsm://github.com/tianocore/edk2.git;protocol=https;branch=master;name=edk2;destsuffix=git \
           git://github.com/tianocore/edk2-platforms.git;protocol=https;branch=master;name=platforms;destsuffix=edk2-platforms \
           git://github.com/tianocore/edk2-non-osi.git;protocol=https;branch=master;name=nonosi;destsuffix=edk2-non-osi \
           git://github.com/tianocore/edk2-redfish-client.git;protocol=https;branch=main;name=redfishclient;destsuffix=edk2-redfish-client \
           file://config.txt \
           file://ipxe-fdf-snippet.fdf.inc \
           file://edk2-platforms \
           file://0001-EDK2-Sd-Mmc-v4.patch \
           file://0000-edk2-platforms-RPi5-port.patch;patchdir=../edk2-platforms \
           file://0001-Rp1BusDxe-register-GEM-and-I2C1-vendor-devices.patch;patchdir=../edk2-platforms \
           file://0002-PlatformSmbiosDxe-deterministic-UUID-and-Type45.patch;patchdir=../edk2-platforms \
           file://0003-RPi5-AcpiTables-add-SsdtThermal.patch;patchdir=../edk2-platforms \
           file://0004-PlatformBm-return-boot-option-number-not-list-index.patch;patchdir=../edk2-platforms \
           file://0005-PlatformBm-prune-USB-NIC-network-boot-options.patch;patchdir=../edk2-platforms \
           file://0100-UsbNetwork-assume-media-on-a-point-to-point-gadget.patch \
           file://0101-RedfishDiscoverDxe-skip-the-IPv6-discovery-leg.patch \
           file://RpiBmcPkg \
           file://Rp1GemPkg \
           file://RpiRedfishPkg \
           file://usbnet-dsc-snippet.inc \
           file://usbnet-fdf-snippet.fdf.inc \
           "
# Upstream pins chosen for byte-parity with the retired NumberOneGit forks
# (audited 2026-08-17 with git merge-base + reconstruction diffs):
#   edk2:      the fork was upstream master @ this exact commit plus ONE
#              commit -- the SD fixup now carried as 0001-EDK2-Sd-Mmc-v4.patch.
#   platforms: the fork's merge-base with upstream master (2024-03-12); the
#              fork's 32-commit RPi5 port on top of it is decomposed into
#              files/edk2-platforms/ (75 added files) + the 0000 patch
#              (43 changed/deleted files). The fork's bulk-sync churn in
#              other vendors' trees (187 files never referenced by
#              RPi5.dsc/.fdf) is deliberately dropped.
#   non-osi:   the fork was upstream @ this commit plus only an RPi5
#              TrustedFirmware bl31.bin -- dead weight here, because this
#              recipe always passes -D TFA_BUILD_ARTIFACTS pointing at our
#              own arm-trusted-firmware deploy.
SRCREV_edk2 = "c4d29cb62187060493a1f595083ddfb6dd346f39"
SRCREV_platforms = "80ee8b861edb6a8b02a100f63bbb435499f8741a"
SRCREV_nonosi = "94d048981116e2e3eda52dad1a89958ee404098d"
# edk2-redfish-client tracks edk2 MASTER, and the pinned NumberOneGit edk2
# is master-based too -- its RedfishPkg carries commits through 2026-02-03,
# despite the fork's "202405" branding. That dates the compatibility
# window precisely (audited 2026-08-17):
#   >= 73a1eaa41 (2026-01-17): RedfishPlatformConfigSetValue grew a
#     by-pointer value argument in edk2 and the client the same day; an
#     older client passes by value and fails to compile against this fork.
#   <  b8ffa6e45 (2026-05-05): the client's RedfishEventLib starts needing
#     gEdkIIRedfisEventRedfishInterfaceDisconnectionGuid, which this fork's
#     RedfishPkg.dec predates (the GUID nuc-bios-build moved to edk2 master
#     for -- see edk2-uefipayload_2605.bb).
# This pin is the last commit before that boundary; every external GUID it
# references is declared by the pinned edk2's .dec files.
SRCREV_redfishclient = "a75f45cd69c74121fbf58900b9d92735d9a3373c"
SRCREV_FORMAT = "edk2_platforms_nonosi_redfishclient"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR. Without this shim, S = "${UNPACKDIR}/git" never
# expands and do_unpack fails its unexpanded-variable QA check.
UNPACKDIR ?= "${WORKDIR}"

EDK2_PLATFORMS_PATH = "${UNPACKDIR}/edk2-platforms"
EDK2_NON_OSI_PATH = "${UNPACKDIR}/edk2-non-osi"
EDK2_REDFISH_CLIENT_PATH = "${UNPACKDIR}/edk2-redfish-client"

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

# Board-integration driver set (local RpiBmcPkg): power button, active
# cooler, BootloaderConfig provenance variables. (The I2C EEPROM sync
# drivers that used to live behind this knob were replaced by the Redfish
# host interface -- see RPI5_REDFISH.)
RPI5_BMC ??= "1"

# Wire edk2's own USB CDC-ECM/NCM/RNDIS class drivers (present in the
# pinned tree, not in RPi5.dsc) into the build, so a USB Ethernet gadget on
# an RP1 port becomes an SNP interface. The BMC's host-interface link rides
# the ncm.usb0 function of this -- required by RPI5_REDFISH.
RPI5_USBNET ??= "1"

# Redfish Host Interface (DSP0270) over the BMC's USB CDC-NCM gadget: the
# JetKVM method from ../nuc-bios-build, NCM instead of ECM. Replaces the
# old I2C shared-EEPROM sync as the BMC data path (local RpiRedfishPkg;
# wire contract in its README.md). Needs RPI5_USBNET=1 for the NIC driver.
RPI5_REDFISH ??= "1"

# edk2-redfish-client (RedfishClientPkg) on top of the host interface: the
# standard feature layer -- ComputerSystemDxe, BiosDxe, BootOptionDxe and
# their JSON converters (RpiRedfishPkg/RpiRedfishClient.dsc.inc; the Memory
# feature, sample Bios form and SecureBoot are deliberately absent -- see
# that file's header). Requires RPI5_REDFISH.
RPI5_REDFISH_CLIENT ??= "1"

# The wire contract with the BMC, rendered into RpiRedfish.dsc.inc's PCDs.
# RPI5_REDFISH_MAC is the gadget's host_addr (the MAC the Pi's NCM NIC
# comes up with) -- the BMC must present exactly this fixed value, or
# RedfishDiscoverDxe rejects the interface. Colon-separated, lowercase ok.
RPI5_REDFISH_MAC ??= "da:c0:ff:ee:10:02"
# HTTP Basic credentials for the BMC's Redfish service (nanokvm-app's
# CheckAuth). Set RPI5_REDFISH_USER to "" for a BMC with authentication
# disabled (the credential library then reports AuthMethodNone).
RPI5_REDFISH_USER ??= "admin"
RPI5_REDFISH_PASSWORD ??= "admin"

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
    cp -r "${WORKDIR}/RpiBmcPkg" "${WORKDIR}/Rp1GemPkg" "${WORKDIR}/RpiRedfishPkg" "${local_pkgs}/"

    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${S}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}:${EDK2_REDFISH_CLIENT_PATH}:${local_pkgs}"
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

    # Redfish Host Interface stack over the BMC's CDC-NCM gadget (RedfishPkg
    # core drivers + local RpiRedfishPkg; the usbnet snippet above supplies
    # the NIC driver). The wire-contract knobs are rendered into the
    # local_pkgs COPY of RpiRedfish.dsc.inc (the pristine WORKDIR original
    # keeps its placeholders; local_pkgs is rebuilt from it on every
    # do_compile, so the render is idempotent). The dsc include opens its
    # own [LibraryClasses]/[PcdsFixedAtBuild] sections and reopens
    # [Components.common] at its end -- and this insertion runs LAST so that
    # block sits closest to the marker, leaving every earlier-inserted
    # component line after it, back inside [Components.common].
    if [ "${RPI5_REDFISH}" = "1" ]; then
        [ "${RPI5_USBNET}" = "1" ] || \
            bbwarn "RPI5_REDFISH=1 without RPI5_USBNET=1: no NIC driver for the BMC link"
        mac_plain=$(printf '%s' "${RPI5_REDFISH_MAC}" | tr -d ':' | tr 'abcdef' 'ABCDEF')
        [ "$(printf '%s' "${mac_plain}" | wc -c)" = "12" ] || \
            bbfatal "RPI5_REDFISH_MAC '${RPI5_REDFISH_MAC}' is not a 6-octet MAC"
        mac_bytes=$(printf '%s' "${mac_plain}" | sed 's/../0x&, /g; s/, $//')
        sed -i \
            -e "s|@RPI5_REDFISH_MAC_PLAIN@|${mac_plain}|g" \
            -e "s|@RPI5_REDFISH_MAC_BYTES@|${mac_bytes}|g" \
            -e "s|@RPI5_REDFISH_USER@|${RPI5_REDFISH_USER}|g" \
            -e "s|@RPI5_REDFISH_PASSWORD@|${RPI5_REDFISH_PASSWORD}|g" \
            "${local_pkgs}/RpiRedfishPkg/RpiRedfish.dsc.inc"
        printf '%s\n' '!include RpiRedfishPkg/RpiRedfish.dsc.inc' > "${B}/rpiredfish-dsc-line.inc"
        printf '%s\n' '!include RpiRedfishPkg/RpiRedfish.fdf.inc' > "${B}/rpiredfish-fdf-line.inc"
        grep -qF 'RpiRedfishPkg/RpiRedfish.dsc.inc' "${dsc}" || \
            sed -i "\|${dsc_marker}|r ${B}/rpiredfish-dsc-line.inc" "${dsc}"
        grep -qF 'RpiRedfishPkg/RpiRedfish.fdf.inc' "${fdf}" || \
            sed -i "\|${fdf_marker}|r ${B}/rpiredfish-fdf-line.inc" "${fdf}"

        # edk2-redfish-client feature layer on top of the host interface.
        # Same include mechanics; the component/library lists live in
        # RpiRedfishClient.dsc.inc (explicit, not the client's own gated
        # .inc files -- see its header).
        if [ "${RPI5_REDFISH_CLIENT}" = "1" ]; then
            printf '%s\n' '!include RpiRedfishPkg/RpiRedfishClient.dsc.inc' > "${B}/rpiredfishclient-dsc-line.inc"
            printf '%s\n' '!include RpiRedfishPkg/RpiRedfishClient.fdf.inc' > "${B}/rpiredfishclient-fdf-line.inc"
            grep -qF 'RpiRedfishPkg/RpiRedfishClient.dsc.inc' "${dsc}" || \
                sed -i "\|${dsc_marker}|r ${B}/rpiredfishclient-dsc-line.inc" "${dsc}"
            grep -qF 'RpiRedfishPkg/RpiRedfishClient.fdf.inc' "${fdf}" || \
                sed -i "\|${fdf_marker}|r ${B}/rpiredfishclient-fdf-line.inc" "${fdf}"
        fi
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

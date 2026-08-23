SUMMARY = "EDK2 StandaloneMM RPMB firmware volume (BL32_AP_MM.fd) for OP-TEE"
DESCRIPTION = "Builds BL32_AP_MM.fd from edk2-platforms' \
               Platform/StandaloneMm/PlatformStandaloneMmPkg/PlatformStandaloneMmRpmb.dsc: \
               a StandaloneMM firmware volume packing StandaloneMmCore, the \
               MM variable service (VariableStandaloneMm), the fault-tolerant \
               write service (FaultTolerantWriteStandaloneMm), the MM CPU \
               driver, and Drivers/OpTee/OpteeRpmbPkg/OpTeeRpmbFv -- the FVB \
               that persists the UEFI variable store to eMMC/SD RPMB through \
               OP-TEE (SP_SVC_RPMB_READ/WRITE to the OP-TEE core). \
\
               OP-TEE, built as the S-EL1 FF-A SPM Core (CFG_CORE_SEL1_SPMC), \
               loads this FV as the StMM secure partition via CFG_STMM_PATH; \
               the optee-os recipe DEPENDS on this recipe and points \
               CFG_STMM_PATH at the staged BL32_AP_MM.fd. EDK2 (BL33) reaches \
               the variable service over FF-A (MmCommunicationDxe + \
               VariableSmmRuntimeDxe). \
\
               Built the same way as rpi5-uefi-firmware: the edk2 and \
               edk2-platforms trees come from the sysroot (staged by their \
               recipes), do_compile builds host-native BaseTools inside the \
               edk2 copy and then the AArch64 StandaloneMM image."
HOMEPAGE = "https://github.com/tianocore/edk2-platforms"
LICENSE = "BSD-2-Clause-Patent"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/BSD-2-Clause-Patent;md5=0518d409dae93098cca8dfa932f3ab1b"

# The edk2 + edk2-platforms source trees, staged under ${STAGING_DATADIR}/edk2
# by their recipes (same as rpi5-uefi-firmware consumes them).
DEPENDS = "edk2 edk2-platforms"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# One AArch64 image; not packaged into a rootfs, staged for optee-os + deployed.
PACKAGE_ARCH = "${MACHINE_ARCH}"
inherit deploy nopackages

# UNPACKDIR shim (scarthgap unpacks into WORKDIR); mirrors the other recipes.
UNPACKDIR ?= "${WORKDIR}"
S = "${UNPACKDIR}"

# Where the source trees live in the sysroot (see edk2 / edk2-platforms recipes).
EDK2_SOURCE_ROOT = "${STAGING_DATADIR}/edk2"
EDK2_SRC = "${EDK2_SOURCE_ROOT}/edk2"
EDK2_PLATFORMS_SRC = "${EDK2_SOURCE_ROOT}/edk2-platforms"
# Working copies under WORKSPACE (=${WORKDIR}); BaseTools is built in the edk2 copy.
EDK2_PATH = "${WORKDIR}/edk2"
EDK2_PLATFORMS_PATH = "${WORKDIR}/edk2-platforms"

# RELEASE (default), DEBUG or NOOPT, per the dsc's BUILD_TARGETS.
STMM_BUILD_TARGET ??= "RELEASE"

# Where the built FV is staged for optee-os (keep in step with optee-os's
# STMM_FV consumption).
STMM_SYSROOT_DIR = "${datadir}/edk2-standalone-mm"
STMM_FV_NAME = "BL32_AP_MM.fd"

do_fetch[noexec] = "1"
do_unpack[noexec] = "1"
do_patch[noexec] = "1"

# The staged edk2 tree is read-only/shared and do_compile builds BaseTools into
# it, so work on private copies -- same reasoning as rpi5-uefi-firmware.
do_configure() {
    rm -rf "${EDK2_PATH}" "${EDK2_PLATFORMS_PATH}"
    mkdir -p "${EDK2_PATH}" "${EDK2_PLATFORMS_PATH}"
    cp -a --reflink=auto "${EDK2_SRC}/." "${EDK2_PATH}/"
    cp -a --reflink=auto "${EDK2_PLATFORMS_SRC}/." "${EDK2_PLATFORMS_PATH}/"

    # This tree (edk2-stable202608 + edk2-platforms master) predates the host
    # aarch64 gcc (13.4), which is stricter than the toolchain EDK2 targets and
    # trips over things the main firmware never hits (it does not build the
    # StandaloneMM libraries or MbedTls):
    #
    #   * MbedTlsLib (3.6.6) constant_time_impl.h emits aarch64 inline asm
    #     (MBEDTLS_CT_AARCH64_ASM) whose pointer-operand constraint miscompiles
    #     under the EDK2 GCC toolchain's -flto ("invalid 'asm': invalid
    #     operand"). Every asm block has a portable C #else fallback, so
    #     neutralise the single MBEDTLS_HAVE_ASM guard that gates all the CT
    #     asm and let mbedtls take its portable constant-time path. This keeps
    #     LTO (and its bundled -llto-aarch64 intrinsics linkage) intact for
    #     every other module -- the main firmware dodges this only because it
    #     uses OpenSSL, not MbedTls.
    CT=${EDK2_PATH}/CryptoPkg/Library/MbedTlsLib/mbedtls/library/constant_time_impl.h
    if [ -f "${CT}" ]; then
        sed -i 's/#if defined(MBEDTLS_HAVE_ASM)/#if 0 \&\& defined(MBEDTLS_HAVE_ASM)/' "${CT}"
    fi

    #   * gcc 12/13 emit false-positive -Wmaybe-uninitialized / stringop /
    #     array-bounds / dangling-pointer diagnostics in upstream EDK2 sources
    #     (e.g. ArmStandaloneMmCoreEntryPoint.c); demote those to warnings.
    # Appended (=) to the dsc's existing AARCH64 BuildOptions, so these flags
    # come after (and override) the toolchain's -Werror.
    printf '%s\n' \
        '' \
        '[BuildOptions.AARCH64]' \
        '  GCC:*_*_*_CC_FLAGS = -Wno-error=maybe-uninitialized -Wno-error=uninitialized -Wno-error=stringop-overflow -Wno-error=stringop-overread -Wno-error=array-bounds -Wno-error=dangling-pointer -Wno-error=nonnull' \
        >> "${EDK2_PLATFORMS_PATH}/Platform/StandaloneMm/PlatformStandaloneMmPkg/PlatformStandaloneMmRpmb.dsc"
}

do_compile() {
    cd "${EDK2_PATH}"

    # BaseTools are host-native; keep bitbake's cross env out of that build.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
    oe_runmake -C BaseTools CC=gcc CXX=g++

    # WORKSPACE is the parent of both EDK2_PATH and EDK2_PLATFORMS_PATH; the
    # -p edk2-platforms/... path resolves through WORKSPACE, and PACKAGES_PATH
    # carries the absolute sysroot roots. Same layout as rpi5-uefi-firmware.
    export WORKSPACE="${WORKDIR}"
    export PACKAGES_PATH="${EDK2_PATH}:${EDK2_PLATFORMS_PATH}"
    export EDK_TOOLS_PATH="${EDK2_PATH}/BaseTools"
    export CONF_PATH="${WORKDIR}/Conf"
    export PYTHON_COMMAND="python3"
    export PATH="${EDK2_PATH}/BaseTools/BinWrappers/PosixLike:${PATH}"

    install -d "${CONF_PATH}"
    for f in target tools_def build_rule; do
        [ -e "${CONF_PATH}/${f}.txt" ] || cp "${EDK2_PATH}/BaseTools/Conf/${f}.template" "${CONF_PATH}/${f}.txt"
    done

    # AArch64 cross build via tools_def's GCC family; it keys the toolchain off
    # GCC_AARCH64_PREFIX, not $CC.
    export GCC_AARCH64_PREFIX="${TARGET_PREFIX}"

    dsc="edk2-platforms/Platform/StandaloneMm/PlatformStandaloneMmPkg/PlatformStandaloneMmRpmb.dsc"
    build -a AARCH64 -t GCC -b ${STMM_BUILD_TARGET} -p "${dsc}"

    fv="${WORKDIR}/Build/MmStandaloneRpmb/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME}"
    [ -f "${fv}" ] || bbfatal "StandaloneMM build produced no ${fv} -- check the build log"
}

do_install() {
    install -d ${D}${STMM_SYSROOT_DIR}
    install -m 0644 ${WORKDIR}/Build/MmStandaloneRpmb/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME} \
        ${D}${STMM_SYSROOT_DIR}/${STMM_FV_NAME}
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${WORKDIR}/Build/MmStandaloneRpmb/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME} \
        ${DEPLOYDIR}/${STMM_FV_NAME}
}

addtask deploy after do_compile

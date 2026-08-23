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
inherit deploy nopackages python3native

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

# Enlarge the RPMB variable-store geometry (see do_configure). Default OFF while
# we isolate a StandaloneMmCore memory-map abort: the 3x64KB store + 64KB FTW
# blocks, against OP-TEE's 16KB-aligned MMRAM and AArch64's 64KB
# RUNTIME_PAGE_ALLOCATION_GRANULARITY, is the leading suspect. "0" = stock
# (8KB var / 48KB EFI_VARS object); "1" = 192KB object.
STMM_BIG_VARS ??= "0"

# StandaloneMmCore/Page.c bounds its temporary memory-map descriptor stack at
# MAX_MAP_DEPTH (upstream 6), guarded only by an ASSERT -- a no-op in RELEASE.
# A re-entrant CoreConvertPages (AllocateMemoryMapEntry's refuel calls
# MmInternalAllocatePagesEx -> CoreConvertPages) can push mMapDepth past the
# bound, writing &mMapStack[mMapDepth] into the adjacent .data globals and
# corrupting the memory-map list. Tested -- raising it did NOT fix the abort
# (root cause was the missing free-range validation ported below), so kept at
# stock "6". Left as a knob for future stress.
STMM_MAP_DEPTH ??= "6"
do_configure[vardeps] += "STMM_BIG_VARS STMM_MAP_DEPTH"

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

    # Enlarge the RPMB-backed UEFI variable store. PlatformStandaloneMmRpmb.dsc
    # ships a tiny geometry -- 16KB variable region, 48KB EFI_VARS object
    # (var + FTW working + FTW spare), 8KB max variable, 10KB max auth variable.
    # That is smaller than the FD-backed store this replaces (RPi5.fdf
    # NV_VARIABLE_STORE is 0xe000 = 56KB) and too small for a Secure Boot dbx.
    # Bump to a 64KB variable store + 64KB/64KB FTW (a 192KB EFI_VARS object --
    # the RPMB partition must be at least that; industrial SD / eMMC RPMB is
    # far larger) and a 32KB max auth variable. The NV regions are a runtime RAM
    # mirror OpTeeRpmbFvb syncs to RPMB, not part of BL32_AP_MM.fd, so the FV
    # binary size is unchanged. Edited in place because a dsc may set each PCD
    # only once (append would duplicate); verified after so a silent no-match
    # (upstream reformatted the lines) fails the build instead of shipping the
    # tiny store.
    stmm_dsc="${EDK2_PLATFORMS_PATH}/Platform/StandaloneMm/PlatformStandaloneMmPkg/PlatformStandaloneMmRpmb.dsc"
    if [ "${STMM_BIG_VARS}" = "1" ]; then
        sed -i \
            -e 's#PcdMaxVariableSize|0x2000#PcdMaxVariableSize|0x8000#' \
            -e 's#PcdMaxAuthVariableSize|0x2800#PcdMaxAuthVariableSize|0x8000#' \
            -e 's#PcdFlashNvStorageVariableSize|0x00004000#PcdFlashNvStorageVariableSize|0x00010000#' \
            -e 's#PcdFlashNvStorageFtwWorkingSize|0x00004000#PcdFlashNvStorageFtwWorkingSize|0x00010000#' \
            -e 's#PcdFlashNvStorageFtwSpareSize|0x00004000#PcdFlashNvStorageFtwSpareSize|0x00010000#' \
            -e 's#PcdVariableStoreSize|0x00004000#PcdVariableStoreSize|0x00010000#' \
            "${stmm_dsc}"
        for chk in "PcdMaxVariableSize|0x8000" "PcdMaxAuthVariableSize|0x8000" \
                   "PcdFlashNvStorageVariableSize|0x00010000" \
                   "PcdFlashNvStorageFtwWorkingSize|0x00010000" \
                   "PcdFlashNvStorageFtwSpareSize|0x00010000" \
                   "PcdVariableStoreSize|0x00010000"; do
            grep -qF "${chk}" "${stmm_dsc}" || \
                bbfatal "RPMB variable-store PCD bump failed for '${chk}' -- upstream PlatformStandaloneMmRpmb.dsc changed; fix the sed in do_configure"
        done
    else
        bbnote "STMM_BIG_VARS=0: stock PlatformStandaloneMmRpmb.dsc geometry (8KB var / 48KB EFI_VARS object) -- isolating the StandaloneMmCore map abort"
    fi

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

    # Raise StandaloneMmCore's memory-map temp-stack depth (see STMM_MAP_DEPTH).
    mm_page="${EDK2_PATH}/StandaloneMmPkg/Core/Page.c"
    sed -i "s/\(#define[[:space:]]\+MAX_MAP_DEPTH[[:space:]]\+\)6\b/\1${STMM_MAP_DEPTH}/" "${mm_page}"
    grep -qE "#define[[:space:]]+MAX_MAP_DEPTH[[:space:]]+${STMM_MAP_DEPTH}\b" "${mm_page}" || \
        bbfatal "MAX_MAP_DEPTH bump to ${STMM_MAP_DEPTH} failed -- StandaloneMmPkg/Core/Page.c changed; fix the sed"

    # ROOT-CAUSE FIX: StandaloneMmCore/Page.c is missing the "check valid memory
    # range" hardening that MdeModulePkg/Core/PiSmmCore/Page.c has (the two are
    # maintained separately and drifted). Without it, a free of a zero/out-of-map
    # range reaches ConvertMmMemoryMapEntry() and splices a bogus node into
    # gMemoryMap -> a NULL LIST_ENTRY deref on a later allocation walk during StMM
    # driver init (the storage/geometry/map-depth-independent abort). Port both
    # guards from PiSmmCore: (1) MmInternalFreePagesEx rejects Memory==0 /
    # NumberOfPages==0; (2) MmFreePages refuses a range not in gMemoryMap via a
    # new InMemMap(). Page.c is CRLF -> edit in bytes.
    nativepython3 - "${mm_page}" <<'PYEOF'
import sys
p = sys.argv[1]
d = open(p, 'rb').read()

# (1) MmInternalFreePagesEx: also reject zero base / zero pages.
old = b'  if ((Memory & EFI_PAGE_MASK) != 0) {'
new = b'  if (((Memory & EFI_PAGE_MASK) != 0) || (Memory == 0) || (NumberOfPages == 0)) {'
assert d.count(old) == 1, 'freepagesex check anchor count != 1'
d = d.replace(old, new, 1)

# (2a) Insert InMemMap() ahead of MmFreePages.
anchor = b'EFI_STATUS\r\nEFIAPI\r\nMmFreePages (\r\n'
assert d.count(anchor) == 1, 'MmFreePages anchor count != 1'
inmemmap = (
b'/**\r\n'
b'  Check whether the input range is fully within gMemoryMap (ported from\r\n'
b'  PiSmmCore InMemMap()).\r\n'
b'**/\r\n'
b'STATIC\r\n'
b'BOOLEAN\r\n'
b'InMemMap (\r\n'
b'  IN EFI_PHYSICAL_ADDRESS  Memory,\r\n'
b'  IN UINTN                 NumberOfPages\r\n'
b'  )\r\n'
b'{\r\n'
b'  LIST_ENTRY            *Link;\r\n'
b'  MEMORY_MAP            *Entry;\r\n'
b'  EFI_PHYSICAL_ADDRESS  Last;\r\n'
b'\r\n'
b'  Last = Memory + EFI_PAGES_TO_SIZE (NumberOfPages) - 1;\r\n'
b'  Link = gMemoryMap.ForwardLink;\r\n'
b'  while (Link != &gMemoryMap) {\r\n'
b'    Entry = CR (Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);\r\n'
b'    Link  = Link->ForwardLink;\r\n'
b'    if ((Entry->Start <= Memory) && (Entry->End >= Last)) {\r\n'
b'      return TRUE;\r\n'
b'    }\r\n'
b'  }\r\n'
b'\r\n'
b'  return FALSE;\r\n'
b'}\r\n'
b'\r\n'
)
d = d.replace(anchor, inmemmap + anchor, 1)

# (2b) MmFreePages: refuse a range not in gMemoryMap.
oldc = b'  Status = MmInternalFreePages (Memory, NumberOfPages);'
newc = (b'  if (!InMemMap (Memory, NumberOfPages)) {\r\n'
        b'    return EFI_NOT_FOUND;\r\n'
        b'  }\r\n'
        b'\r\n'
        b'  Status = MmInternalFreePages (Memory, NumberOfPages);')
assert d.count(oldc) == 1, 'MmInternalFreePages call anchor count != 1'
d = d.replace(oldc, newc, 1)

open(p, 'wb').write(d)
print('StandaloneMmCore Page.c: ported PiSmmCore free-range validation')
PYEOF

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

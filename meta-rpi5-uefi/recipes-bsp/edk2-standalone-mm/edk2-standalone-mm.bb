SUMMARY = "EDK2 StandaloneMM firmware volume (BL32_AP_MM.fd) for OP-TEE"
DESCRIPTION = "Builds BL32_AP_MM.fd from this layer's \
               Platform/RaspberryPi/RPi5/StandaloneMm/PlatformStandaloneMmRpi5.dsc \
               (in the edk2-platforms overlay): a StandaloneMM firmware \
               volume packing StandaloneMmCore, the MM variable service \
               (VariableStandaloneMm), the fault-tolerant write service \
               (FaultTolerantWriteStandaloneMm), the MM CPU driver, and \
               RpiNvMemFvb -- an FVB over the RPi5.fdf NV window of the \
               VPU-loaded FD, which OP-TEE maps into the SP (patch 0003 in \
               the optee-os recipe, CFG_STMM_VARSTORE_*). No storage device \
               and no OP-TEE storage-service traffic: variable + FTW writes \
               land directly in that memory window; EDK2's \
               MmCommunicationOpteeDxe persists the window back into \
               armstub8-2712.bin / RPI_EFI.fd on the boot FAT. \
\
               OP-TEE loads this FV as the StMM secure partition via \
               CFG_STMM_PATH (an ordinary pseudo-TA under opteed, no FF-A \
               SPMC); the optee-os recipe DEPENDS on this recipe and points \
               CFG_STMM_PATH at the staged BL32_AP_MM.fd. EDK2 (BL33) \
               reaches the variable service through MmCommunicationOpteeDxe \
               + VariableSmmRuntimeDxe. \
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

# The platform: this layer's StandaloneMM dsc from the edk2-platforms overlay.
# Its NV geometry is pinned to the RPi5.fdf FD NV window (variable 0xe000,
# FTW working 0x1000, FTW spare 0x10000 at fixed SP VAs) -- growing the store
# means growing the FDF NV regions and the dsc PCDs together, so there is no
# geometry knob here.
STMM_DSC_DIR = "Platform/RaspberryPi/RPi5/StandaloneMm"
STMM_DSC = "${STMM_DSC_DIR}/PlatformStandaloneMmRpi5.dsc"
STMM_BUILD_DIR = "Build/MmStandaloneRpi5"

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

    # The platform dsc/fdf and the RpiNvMemFvb driver come from this layer's
    # edk2-platforms overlay (staged by the edk2-platforms recipe); nothing to
    # edit -- just make sure the overlay actually reached the staged tree, so
    # a stale sysroot fails loudly here instead of deep inside the build.
    [ -f "${EDK2_PLATFORMS_PATH}/${STMM_DSC}" ] || \
        bbfatal "${STMM_DSC} missing from the staged edk2-platforms tree -- rebuild edk2-platforms so the StandaloneMm overlay is staged"

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

    mm_page="${EDK2_PATH}/StandaloneMmPkg/Core/Page.c"

    # HARDENING: StandaloneMmCore/Page.c is missing the "check valid memory
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

    # (The gcc-12/13 false-positive -Wno-error demotions live in the dsc's own
    # [BuildOptions.AARCH64] now -- see PlatformStandaloneMmRpi5.dsc.)
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

    build -a AARCH64 -t GCC -b ${STMM_BUILD_TARGET} -p "edk2-platforms/${STMM_DSC}"

    fv="${WORKDIR}/${STMM_BUILD_DIR}/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME}"
    [ -f "${fv}" ] || bbfatal "StandaloneMM build produced no ${fv} -- check the build log"

    # FP/SIMD gate. This code runs in the StMM secure partition at S-EL0
    # where FP/SIMD access traps (esr EC 0x07) and OP-TEE kills the SP --
    # measured on hardware: gcc's q-register varargs prologue in
    # VariableSmm.c took down the first SetVariable of the boot. The dsc
    # forces -mgeneral-regs-only, but only this scan proves the invariant
    # held for every module (a future lib swap or dsc edit could silently
    # drop the flag). Scans the pre-GenFw ELFs; any FP/SIMD-register
    # instruction anywhere is fatal.
    for dll in $(find "${WORKDIR}/${STMM_BUILD_DIR}/${STMM_BUILD_TARGET}_GCC/AARCH64" \
                   -name '*.dll' -path '*/DEBUG/*'); do
        n=$(${TARGET_PREFIX}objdump -d "${dll}" | \
            grep -cE '\s(ldr|str|ldp|stp|fmov|movi|ld1|st1|dup|fadd|fsub|fmul|fdiv|fcvt|scvtf|ucvtf)\s+[qvds][0-9]+' || true)
        if [ "${n}" != "0" ]; then
            bbfatal "$(basename ${dll}) contains ${n} FP/SIMD instruction(s) -- would trap in the StMM SP (S-EL0). Check -mgeneral-regs-only in ${STMM_DSC}."
        fi
    done
    bbnote "StMM FP/SIMD gate: all modules clean"
}

do_install() {
    install -d ${D}${STMM_SYSROOT_DIR}
    install -m 0644 ${WORKDIR}/${STMM_BUILD_DIR}/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME} \
        ${D}${STMM_SYSROOT_DIR}/${STMM_FV_NAME}
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${WORKDIR}/${STMM_BUILD_DIR}/${STMM_BUILD_TARGET}_GCC/FV/${STMM_FV_NAME} \
        ${DEPLOYDIR}/${STMM_FV_NAME}
}

addtask deploy after do_compile

SUMMARY = "OP-TEE OS (BL32) for Raspberry Pi 5 - BMC sensor push service"
DESCRIPTION = "Builds tee-raw.bin, the Secure-EL1 payload embedded in \
               RPI_EFI.fd (FD region at offset 0x330000, see RPi5.fdf). The \
               patched TF-A rpi5 BL31 copies it to the secure DRAM carve-out \
               at 0x1D000000 and enters it through the opteed dispatcher \
               before EDK2 (BL33) runs. \
\
               Upstream optee_os has carried a (console-only) plat-rpi5 \
               since 4.10; this recipe's patch wires up the GIC, the \
               secure-timer callout service and async notifications, and \
               the files/plat-rpi5/ overlay adds the pi-bmc BMC sensor \
               service: a pseudo-TA that samples the BCM2712 die \
               temperature on a secure timer and pushes records to the \
               BMC-emulated I2C EEPROM behind the RP1 (late-initialized \
               with the RP1 BAR by EDK2's RpiOpteeSensorDxe -- the RP1 is a \
               PCIe endpoint, so its address only exists after PCIe \
               enumeration). \
\
               tee-raw.bin reaches the firmware through edk2-non-osi, which \
               files it next to bl31.bin at \
               Platform/RaspberryPi/RPi5/TrustedFirmware/tee-raw.bin, the \
               path RPi5.fdf names. do_deploy also drops tee-raw.bin and \
               tee.elf in DEPLOY_DIR_IMAGE for inspection and debugging."
HOMEPAGE = "https://github.com/OP-TEE/optee_os"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=c1f21c4f72f372ef38a5a4aee55ec173"

SRC_URI = "git://github.com/OP-TEE/optee_os.git;protocol=https;nobranch=1 \
           file://plat-rpi5;subdir=git/core/arch/arm \
           file://0001-plat-rpi5-wire-up-GIC-secure-timer-callouts-and-the-.patch \
           file://0002-scmi-msg-add-Sensor-Management-protocol-0x15.patch \
           "
# Tag 4.10.0. The files/plat-rpi5/ overlay directory merges into the
# checkout's core/arch/arm/plat-rpi5/ during do_unpack (new files only;
# the patch carries the edits to upstream files). LOAD-BEARING: the
# file:// entries must stay listed after the git entry -- the git
# fetcher prunes its destdir before checkout.
SRCREV = "753afbbee1682f5d16fd30e87b31058a4fd4f4b8"

PV = "4.10.0+git${SRCPV}"
S = "${WORKDIR}/git"

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# One board, one set of carve-out addresses baked in.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# The output is firmware: staged for edk2-non-osi and deployed, never
# packaged into a rootfs.
inherit deploy nopackages python3native

# gen_tee_bin.py needs pyelftools.
DEPENDS = "python3-pyelftools-native"

# Opt-in: embed the EDK2 StandaloneMM RPMB firmware volume (BL32_AP_MM.fd,
# built by the edk2-standalone-mm recipe) as an OP-TEE StMM secure partition
# (CFG_STMM_PATH -> CFG_WITH_STMM_SP), and turn on the RPMB filesystem
# backend that its EFI_VARS object lives in. StMM runs as an ordinary OP-TEE
# pseudo-TA under the existing opteed dispatcher -- it does NOT need the FF-A
# SPMC. The 2.5MB StMM FV is almost all padding, so gen_stmm_hex zlib-embeds
# it in ~130KB: measured tee-raw.bin grows to ~290KB, still inside the
# existing 0x80000 tee FD region in RPi5.fdf -- so NO FD-layout growth is
# needed. Default off.
RPI5_OPTEE_STMM ??= "0"
STMM_FV = "${STAGING_DATADIR}/edk2-standalone-mm/BL32_AP_MM.fd"
DEPENDS += "${@'edk2-standalone-mm' if d.getVar('RPI5_OPTEE_STMM') == '1' else ''}"

# Secure-storage backend for the StMM variable object (EFI_VARS):
#   rpmb  - RPMB filesystem (CFG_RPMB_FS). Real hardware anti-rollback, but
#           needs an RPMB-capable device (eMMC/CM5). CFG_RPMB_TESTKEY uses the
#           well-known key until a HUK is wired to BCM2712 OTP -- INSECURE.
#   reefs - REE filesystem (CFG_REE_FS, OP-TEE default). OP-TEE encrypts +
#           hash-tree-integrity-protects the blob and stores it in the NORMAL
#           world via the FS RPC (serviced by MmCommunicationOpteeDxe's ReeFs
#           backend on the ESP). Works on a plain SD, but gives NO hardware
#           anti-rollback (the on-disk state can be rolled back) -- see the
#           note in Rpmb.c/ReeFs.c. This is OP-TEE's own "no RPMB" answer.
# The two differ only in which storage id stmm_sp.c passes to sec_storage; the
# reefs redirect is applied in do_compile (idempotent sed), keyed on this knob.
RPI5_OPTEE_STMM_BACKEND ??= "rpmb"
STMM_STORAGE_ARGS = "${@'CFG_RPMB_FS=y CFG_RPMB_TESTKEY=y' if d.getVar('RPI5_OPTEE_STMM_BACKEND') == 'rpmb' else 'CFG_REE_FS=y'}"
STMM_MAKE_ARGS = "${@('%s CFG_STMM_PATH=%s' % (d.getVar('STMM_STORAGE_ARGS'), d.getVar('STMM_FV'))) if d.getVar('RPI5_OPTEE_STMM') == '1' else ''}"

# tee-raw.bin must fit the FD region TF-A copies verbatim (RPi5.fdf tee region
# is 0x80000). Embedded StMM measured ~290KB, so it fits -- keep the guard at
# the real FD-region size in both modes to catch any future overflow.
OPTEE_MAX_SIZE ??= "0x80000"
# Decimal form for the shell size check (bitbake's shell parser rejects $(( ))).
OPTEE_MAX_BYTES = "${@int(d.getVar('OPTEE_MAX_SIZE'), 0)}"

# Flipping these knobs (in kas.yml/local.conf) must rebuild the core: they
# decide whether StMM is embedded and which secure-storage backend its
# stmm_sp.c is patched for. bitbake's automatic signature tracking does pick up
# the shell ${VAR} refs and literal d.getVar() uses, but the do_compile sed and
# the inline-python STMM_MAKE_ARGS make that fragile -- so make the dependency
# explicit and self-documenting. Without this a knob change could be masked by
# an sstate hit and silently ship the wrong storage backend.
do_compile[vardeps] += "RPI5_OPTEE_STMM RPI5_OPTEE_STMM_BACKEND"

do_configure[noexec] = "1"

# Set to "1" (e.g. via a local.conf override) for a debug core with the
# full DMSG/FMSG trace stream on the PL011.
OPTEE_DEBUG ??= "0"
OPTEE_LOG_LEVEL = "${@'3' if d.getVar('OPTEE_DEBUG') == '1' else '1'}"
OPTEE_CORE_DEBUG = "${@'y' if d.getVar('OPTEE_DEBUG') == '1' else 'n'}"

# Everything else (TZDRAM/SHM layout, async-notif INTID, user-TA off) is
# set in plat-rpi5/conf.mk by the patch so the configuration lives with
# the platform, not the build system.
do_compile() {
    # OP-TEE's build computes its own cross flags from CROSS_COMPILE64;
    # keep bitbake's target sysroot flags out of the freestanding core
    # build, same as the arm-trusted-firmware recipe.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

    # REE-FS backend: point the StMM storage service at TEE_STORAGE_PRIVATE_REE
    # instead of _RPMB. stmm_sp.c hardcodes RPMB at the two sec_storage calls in
    # stmm_handle_storage_service(); redirect only those (NOT the __FFA_SVC_RPMB
    # case labels). Idempotent -- a second run finds no RPMB in those lines.
    if [ "${RPI5_OPTEE_STMM}" = "1" ] && [ "${RPI5_OPTEE_STMM_BACKEND}" = "reefs" ]; then
        sed -i \
            -e 's/sec_storage_obj_read(TEE_STORAGE_PRIVATE_RPMB,/sec_storage_obj_read(TEE_STORAGE_PRIVATE_REE,/' \
            -e 's/sec_storage_obj_write(TEE_STORAGE_PRIVATE_RPMB,/sec_storage_obj_write(TEE_STORAGE_PRIVATE_REE,/' \
            "${S}/core/arch/arm/kernel/stmm_sp.c"
        grep -q "sec_storage_obj_read(TEE_STORAGE_PRIVATE_REE," "${S}/core/arch/arm/kernel/stmm_sp.c" || \
            bbfatal "REE-FS storage redirect sed matched nothing -- stmm_sp.c changed upstream; fix the sed"
    fi

    oe_runmake -C ${S} O=${B}/out \
        PLATFORM=rpi5 \
        CROSS_COMPILE64="${TARGET_PREFIX}" \
        CFG_TEE_CORE_LOG_LEVEL=${OPTEE_LOG_LEVEL} \
        CFG_TEE_CORE_DEBUG=${OPTEE_CORE_DEBUG} \
        ${STMM_MAKE_ARGS} \
        all

    [ -s "${B}/out/core/tee-raw.bin" ] || \
        bbfatal "OP-TEE produced no out/core/tee-raw.bin -- check the build log"

    # RPi5.fdf gives the image OPTEE_MAX_SIZE bytes and TF-A copies exactly
    # that much; a bigger core would be silently truncated at boot. The limit
    # grows with RPI5_OPTEE_STMM (StMM is embedded in the core), and RPi5.fdf +
    # the TF-A RPI5_OPTEE_IMAGE_* window must be grown to match.
    TEE_SIZE=$(stat -Lc %s ${B}/out/core/tee-raw.bin)
    bbnote "tee-raw.bin is ${TEE_SIZE} bytes (limit ${OPTEE_MAX_BYTES} = ${OPTEE_MAX_SIZE})"
    if [ "$TEE_SIZE" -gt "${OPTEE_MAX_BYTES}" ]; then
        bbfatal "tee-raw.bin is $TEE_SIZE bytes, over the ${OPTEE_MAX_SIZE} FD region -- grow the region in RPi5.fdf and RPI5_OPTEE_IMAGE_MAX_SIZE in the TF-A patch together"
    fi
}

# Where edk2-non-osi picks tee-raw.bin up; keep the two in step.
OPTEE_SYSROOT_DIR = "${datadir}/optee-os"

do_install() {
    install -d ${D}${OPTEE_SYSROOT_DIR}
    install -m 0644 ${B}/out/core/tee-raw.bin ${D}${OPTEE_SYSROOT_DIR}/tee-raw.bin
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/out/core/tee-raw.bin ${DEPLOYDIR}/tee-raw.bin
    install -m 0644 ${B}/out/core/tee.elf ${DEPLOYDIR}/tee.elf
}

addtask deploy after do_compile

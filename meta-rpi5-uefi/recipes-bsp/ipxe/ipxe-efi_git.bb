SUMMARY = "iPXE AArch64 UEFI UNDI/SNP driver, embedded in the RPi5 EDK2 build"
DESCRIPTION = "Builds bin-arm64-efi/ipxe.efidrv, a DXE_DRIVER-style UEFI \
               driver (UefiDriverEntryPoint, not an application) carrying iPXE's full \
               default set of compiled-in PCI/USB network drivers, plus bin-arm64-efi/ipxe.efi \
               (the standalone boot application, deployed as a convenience artifact but not \
               embedded in the firmware image). \
\
               This exists because RP1 -- the RPi5 southbridge that carries the onboard \
               Ethernet MAC -- has no driver of its own in edk2-platforms' RPi5 port: \
               Rp1BusDxe only handles the I/O bridge/GPIO/PWM side, and NetworkPkg's own \
               PXE/HTTP boot stack (wired in via RPi5.dsc/.fdf's \
               '!include NetworkPkg/Network.dsc.inc') has no NIC to sit on without an \
               EFI_SIMPLE_NETWORK_PROTOCOL provider underneath it. The .efidrv, embedded \
               as a prebuilt DXE driver FFS file by edk2-rpi5-firmware.bb's do_configure, \
               supplies that -- for whatever NIC iPXE actually recognises. \
\
               CAVEAT (this is the 'This *should*' in the parent task): iPXE has no \
               RP1-specific Ethernet MAC driver -- RP1's MAC is Raspberry Pi silicon with a \
               Linux driver only a couple of years old, not a card iPXE's PCI driver table \
               knows about. So this does NOT make the onboard RJ45 jack work in UEFI. What \
               it does enable is PXE boot from anything iPXE *does* recognise and RPi5 can \
               physically expose one of: a PCIe NIC on the M.2/PCIe FPC connector (iPXE \
               carries drivers for most Intel/Realtek/Broadcom/virtio PCI NICs), or a USB \
               Ethernet dongle using a chipset iPXE supports (asix, smsc95xx, lan78xx -- \
               note iPXE upstream already carries a Config.mk 'DRIVERS_rpi = smsc95xx \
               lan78xx' group for the USB NICs used on the RPi3B+/4's own onboard-via-USB \
               Ethernet, both of which this default/unrestricted build includes too)."
HOMEPAGE = "https://ipxe.org"

# iPXE is GPL-2.0-or-later with the UBDL (UEFI Binary Distribution License)
# additional-permissions exception. The md5 is the canonical GPLv2 text
# (oe-core's common-licenses GPL-2.0-only checksum), which is what the
# pinned SRCREV ships as COPYING.GPLv2.
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING.GPLv2;md5=b234ee4d69f5fce4486a80fdaf4a4263"

SRC_URI = "git://github.com/ipxe/ipxe.git;protocol=https;branch=master"
# master HEAD as of 2026-08-10.
SRCREV = "8baf1cda7762b6ee880502ac1d6f507a22959a36"

PV = "1.21.1+git${SRCPV}"
S = "${WORKDIR}/git"

inherit deploy

COMPATIBLE_MACHINE = "raspberrypi5-uefi"

# The ROM is firmware; it embeds its own libc/drivers and links nothing from
# the target sysroot -- but unlike meta-nuc-bios's x86_64-native ipxe-efi
# recipe, this one genuinely cross-compiles (build host is normally x86_64,
# target is AArch64), so the default DEPENDS chain (virtual/${TARGET_PREFIX}gcc
# and friends) is left enabled rather than inhibited.
INHIBIT_DEFAULT_DEPS = "0"

do_configure[noexec] = "1"

# iPXE source is fully vendored; no submodule fetch, so no network at compile.
do_compile() {
    # Keep bitbake's exported cross CFLAGS/LDFLAGS (target sysroot, hardening
    # flags) out of iPXE's freestanding build -- same 'unset' dance as
    # meta-nuc-bios's ipxe-efi and arm-trusted-firmware. CROSS_COMPILE below
    # is what actually selects the compiler; iPXE's Makefile defines
    # CC = $(CROSS_COMPILE)gcc unconditionally, ignoring any inherited CC.
    unset CC CXX CPP AS AR LD RANLIB STRIP OBJCOPY NM CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

    # HOST_CC builds iPXE's own util/ tools (elf2efi, efirom) that run on the
    # build machine to convert the linked ELF into a PE32+ image -- these must
    # use the native compiler regardless of CROSS_COMPILE.
    oe_runmake -C ${S}/src \
        bin-arm64-efi/ipxe.efidrv \
        bin-arm64-efi/ipxe.efi \
        CROSS_COMPILE="${TARGET_PREFIX}" \
        HOST_CC="${BUILD_CC}" \
        V=1

    [ -s "${S}/src/bin-arm64-efi/ipxe.efidrv" ] || \
        bbfatal "iPXE produced no bin-arm64-efi/ipxe.efidrv -- check the build log"
    [ -s "${S}/src/bin-arm64-efi/ipxe.efi" ] || \
        bbfatal "iPXE produced no bin-arm64-efi/ipxe.efi -- check the build log"
}

do_deploy() {
    install -d ${DEPLOYDIR}
    # The DXE driver edk2-rpi5-firmware embeds in the FV (see its
    # do_configure).
    install -m 0644 ${S}/src/bin-arm64-efi/ipxe.efidrv ${DEPLOYDIR}/ipxe.efidrv
    # Standalone boot application -- not embedded, just a convenience copy
    # deployed alongside RPI_EFI.fd for anyone who wants to drop it on the ESP
    # directly (e.g. to netboot over a link the .efidrv above doesn't cover,
    # such as a USB NIC hot-plugged after boot).
    install -m 0644 ${S}/src/bin-arm64-efi/ipxe.efi ${DEPLOYDIR}/ipxe.efi
}

addtask deploy after do_compile

do_install[noexec] = "1"

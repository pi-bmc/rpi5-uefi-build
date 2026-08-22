# rpi5-uefi-build

Yocto/kas build of the [worproject](https://github.com/worproject/rpi5-uefi) /
[NumberOneGit](https://github.com/NumberOneGit/rpi5-uefi) EDK2 UEFI firmware
for the Raspberry Pi 5, structured the same way as
[`../nuc-bios-build`](../nuc-bios-build): each upstream component is a bitbake
recipe (fetched by pinned `SRCREV`, no submodules) rather than a git submodule
checked directly into this repo.

Output: `rpi5-uefi-sd.img`, deployed to
`build/tmp/deploy/images/raspberrypi5-uefi/` -- a flashable MBR image with a
single FAT32 boot partition carrying `armstub8-2712.bin` (the UEFI firmware
under the default BCM2712 armstub filename, auto-loaded by the VPU bootloader
at address 0x0 with no `armstub=` line), `config.txt`, the `bcm2712*.dtb`
device trees and `overlays/` (pinned raspberrypi/firmware release, see
`rpi-boot-dtbs`). Write it with `dd`/Raspberry Pi Imager. The raw
`RPI_EFI.fd`/`armstub8-2712.bin` + `config.txt` are deployed alongside for
hand-made boot partitions.

This image is an **alternative bootloader** and deliberately incompatible
with the u-boot-based image from `../nanokvm-build`: both stacks claim the
`armstub8-2712.bin` name with different payloads (bare BL31 +
`kernel=u-boot.bin` there; BL31+UEFI, no kernel, here). One card carries one
bootloader.

## Build

```sh
pip3 install kas
kas build kas.yml
hack/flash-sd.sh        # write rpi5-uefi-sd.img to the first SD card found
```

`hack/flash-sd.sh` picks the first non-system SD/MMC disk (then removable USB
disk), shows it, and asks before erasing; `-y` skips the prompt, `--dry-run`
only shows the choice, `-d /dev/sdX` overrides detection (system disks are
refused even then).

## Layer / recipe layout

| Recipe | Upstream | Produces |
| --- | --- | --- |
| `meta-rpi5-uefi/recipes-bsp/arm-trusted-firmware/arm-trusted-firmware_git.bb` | `ARM-software/arm-trusted-firmware`, pinned commit (see caveat below) | `bl31.bin`, staged into the sysroot (and deployed) |
| `meta-rpi5-uefi/recipes-bsp/edk2/edk2_git.bb` | `tianocore/edk2` @ tag `edk2-stable202608` | patched source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2-platforms/edk2-platforms_git.bb` | `tianocore/edk2-platforms` `master` @ `9ef9bcef` (2026-08-21), plus this layer's RPi5 port | patched source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2-non-osi/edk2-non-osi_git.bb` | `tianocore/edk2-non-osi` `master`, pinned, plus `bl31.bin` from `arm-trusted-firmware` | source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2-redfish-client/edk2-redfish-client_git.bb` | `tianocore/edk2-redfish-client` `main`, pinned | source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/rpi5-secureboot-keys/rpi5-secureboot-keys_1.0.bb` | this layer's PK + Microsoft's six KEK/db CA certificates, fetched and checksum-pinned | DER certificates, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/rpi5-uefi-firmware/rpi5-uefi-firmware.bb` | none -- builds the trees above | `armstub8-2712.bin` / `RPI_EFI.fd`, `config.txt`, `RPi5Firmware.cap` |

One recipe per upstream repository, and none of them builds anything: `edk2`,
`edk2-platforms`, `edk2-non-osi` and `edk2-redfish-client` each fetch and patch
their own tree and stage it under `${STAGING_DATADIR}/edk2`, which in the
sysroot is exactly the four-tree EDK2 workspace `worproject`/`NumberOneGit`'s
`build.sh` assembles by hand. The RPi5 port and its patch series therefore live
in `recipes-bsp/edk2-platforms/`, and the edk2 patches in `recipes-bsp/edk2/`,
not in the firmware recipe.

`rpi5-uefi-firmware` is what turns that workspace into firmware. It fetches no
source of its own: `do_configure` copies the `edk2` tree out of the sysroot
(`do_compile` builds host-native BaseTools inside it) and `do_compile` copies
`edk2-platforms` (wiring the optional feature sets in rewrites
`RPi5.dsc`/`.fdf`); `edk2-non-osi` and `edk2-redfish-client` are read from the
sysroot in place.

`arm-trusted-firmware`'s `bl31.bin` is embedded as the FD's region-0 payload,
and it gets there through `edk2-non-osi`: that recipe `DEPENDS` on TF-A and
files the binary at `Platform/RaspberryPi/RPi5/TrustedFirmware/bl31.bin`, which
is both where upstream keeps checked-in TF-A builds for the other Pi boards and
the path `RPi5.dsc`'s `TFA_BUILD_BL31` default names. So the FDF resolves it
through `PACKAGES_PATH` like any other source file, and the firmware recipe
passes no `-D TFA_BUILD_ARTIFACTS`.

The Secure Boot default key set is its own recipe too, `rpi5-secureboot-keys`:
it ships this project's PK, fetches Microsoft's KEK and db CAs (the build's only
non-git download, with its own `FETCHCMD_wget` User-Agent workaround), checks
every one of the seven files really is a DER X.509 certificate, and stages them
under `${STAGING_DATADIR}/secureboot-keys`. `rpi5-uefi-firmware` depends on it
only when `RPI5_SECURE_BOOT_DEFAULT_KEYS = "1"`, so a build with the key set off
downloads nothing for it.

All `SRCREV`s are pinned. They were originally the commits
`NumberOneGit/rpi5-uefi`'s own git tree pointed at; that fork is retired, and
the pins now track upstream directly -- `edk2` at the `edk2-stable202608`
release tag, `edk2-platforms` at the master commit contemporary with it.

Upstream `edk2-platforms` has never carried an RPi5 platform, so the RPi5 port
is entirely this layer's: the added files live in the recipe's
`files/edk2-platforms/` overlay and the changes to upstream files in
`0000-edk2-platforms-RPi5-port.patch`. Moving both pins forward together is
what keeps that workable -- the shared `Platform/RaspberryPi` family code
(`ConfigDxe`, `FdtDxe`, `MmcDxe`, `RpiFirmwareDxe`, `PlatformBootManagerLib`,
the common `AcpiTables`) comes maintained against current edk2, and `RPi4.dsc`
serves as a worked reference for the next bump. When the two pins moved to
2026-08, the rebase shrank `0000` from 43 changed files to 24: upstream has
independently absorbed most of what the fork once changed.

## Three things worth flagging up front

**"IPMI recipe" → iPXE → native driver (historical).** The task that produced
this repo asked for an "IPMI recipe that builds an efi rom ... to enable
ethernet/PXE". There's no such thing as an EDK2-embeddable IPMI ROM for that
purpose; the description matched `../nuc-bios-build`'s `ipxe-efi_git.bb` (an
EFI ROM built from iPXE, embedded to give the UEFI PXE/HTTP boot stack a NIC
driver), so this layer shipped one for a while. It has since been removed:
iPXE never recognised RP1's Ethernet MAC, so it could not drive the onboard
jack, and `Rp1GemDxe` now does that natively. See "Onboard Ethernet" below.

**`arm-trusted-firmware` branch substitution, sort of.** The task specified
`https://github.com/ARM-software/arm-trusted-firmware.git` branch `rpi5`.
That branch has since been deleted upstream (`git ls-remote` no longer lists
it) -- but the exact commit `NumberOneGit/rpi5-uefi`'s own submodule pins
(`000fe221b859ee82a4e2f8bf2c96f0086a772c89`) is still fetchable directly from
that same `ARM-software` repository by SHA, even with the branch gone. So the
recipe keeps the URL exactly as given, using `nobranch=1` + a pinned `SRCREV`
instead of `branch=rpi5`. No fork substitution was actually needed here --
just pin-by-commit instead of pin-by-branch.

**Onboard Ethernet is a native driver, not iPXE.** RP1 -- the RPi5 southbridge
carrying the onboard RJ45's MAC -- has no PCI/USB-recognisable identity iPXE's
driver table knows about; it is Raspberry Pi silicon with a Linux driver only a
couple of years old. `Rp1GemDxe` (see below) is a from-scratch EDK2 SNP driver
for it, so the onboard jack PXE/HTTP-boots through NetworkPkg's own
`UefiPxeBcDxe` exactly as the Pi 4's does through `BcmGenetDxe`.

iPXE was removed once that landed. The only coverage it still offered was
add-on NICs -- a PCIe card on the M.2/FPC connector, or a USB dongle from its
driver table -- and USB NICs are deliberately kept out of boot-option
enumeration here (patches `0005` and `0103`), because that port is the BMC's
host-interface link. If you need to network-boot from a PCIe add-in card, iPXE
is the thing to bring back; `git log` has the recipe.

## Local drivers (pi-bmc port)

The `../u-boot` RPi 5 driver set is ported to EDK2 as fresh BSD-2-Clause-Patent
code (the GPL u-boot drivers were behavioral/wire-format reference only). It all
lives in the **edk2-platforms tree**, under
`meta-rpi5-uefi/recipes-bsp/edk2-platforms/files/edk2-platforms/`, filed where
upstream files its own drivers -- so upstreaming any of it is a move, not a
rewrite:

| Location | Contents |
| --- | --- |
| `Silicon/RaspberryPi/RpiSiliconPkg/` | RP1 southbridge silicon: `Rp1BusDxe`, `Library/Rp1GpioLib` (GPIO/PWM block behind the PHY reset and the fan) |
| `Silicon/Broadcom/Drivers/Net/` | `Rp1GemDxe` + `Rp1GemPkg.dec` -- a native SNP driver for the onboard RJ45 (Cadence GEM_GXL in RP1, registers from Xilinx UG585 / FreeBSD `if_cgem.c`), beside upstream's `BcmGenetDxe` + `BcmNet.dec` in the identical shape |
| `Platform/RaspberryPi/Drivers/`, `Library/` | board-independent: `SecureBootToggleDxe`, `PlatformThemeLib` |
| `Platform/RaspberryPi/RPi5/Drivers/` | the board's own: `PowerButtonDxe`, `ActiveCoolerDxe`, `FanConfigDxe`, `BootloaderConfigDxe`, `RpiRedfishSyncDxe` |
| `Platform/RaspberryPi/RPi5/Library/` | `RpiRedfishCredentialLib`, `RpiRedfishHostInterfaceLib`, `Rpi5FmpDeviceLib` |

`RPi5.dsc`/`.fdf` list every one of them directly and unconditionally, the way
`RPi4.dsc`/`.fdf` list theirs, and the GUIDs and PCDs they need are declared in
`RPi5.dec`, `RaspberryPi.dec` and `RpiSiliconPkg.dec`. The firmware recipe adds
no `PACKAGES_PATH` root and inserts nothing for them; its only remaining
`sed`-marker insertions are for edk2-tree modules (`RPI5_USBNET`, profiling)
whose sources are not in edk2-platforms at all.

`0001-Rp1BusDxe-...patch` extends `Rp1BusDxe` to register the GEM and I2C1
blocks as vendor NonDiscoverable children (the xHCI pattern), and
`0019-RaspberryPi-declare-...patch` declares the Secure Boot toggle's formset
GUID in the upstream `RaspberryPi.dec` -- the only two upstream-file changes the
driver set needs.

## Variables

Set any of these in `kas.yml`'s `local_conf_header` (or `local.conf`):

- `RPI5_USBNET` (default `"1"`) -- wire edk2's USB CDC-ECM/NCM/RNDIS
  drivers into the build; the Redfish host interface has no link without it.
- `RPI5_REDFISH_MAC` / `RPI5_REDFISH_USER` / `RPI5_REDFISH_PASSWORD` -- the
  wire contract with the BMC, appended to `RPi5.dsc` as PCD overrides.
- `RPI5_BUILD_TARGET` (default `"RELEASE"`) -- `RELEASE`, `DEBUG` or `NOOPT`.
- `RPI5_FW_VERSION` (default `${PV}`) -- `PcdFirmwareVersionString`.
- `RPI5_EDK2_EXTRA_FLAGS` (default empty) -- extra `build` args
  (`-D FOO=BAR`, `--pcd ...`) without overriding `do_compile` wholesale.
- `TFA_DEBUG` (default `"0"`) -- TF-A `DEBUG=1` build with UART crash
  logging.

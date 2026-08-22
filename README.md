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
| `meta-rpi5-uefi/recipes-bsp/arm-trusted-firmware/arm-trusted-firmware_git.bb` | `ARM-software/arm-trusted-firmware`, pinned commit (see caveat below) | `bl31.bin` |
| `meta-rpi5-uefi/recipes-bsp/ipxe/ipxe-efi_git.bb` | `ipxe/ipxe` master | `ipxe.efidrv`, `ipxe.efi` |
| `meta-rpi5-uefi/recipes-bsp/edk2-platforms/edk2-platforms_git.bb` | `tianocore/edk2-platforms` `master`, pinned + this layer's RPi5 port | patched source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2-non-osi/edk2-non-osi_git.bb` | `tianocore/edk2-non-osi` `master`, pinned | source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2-redfish-client/edk2-redfish-client_git.bb` | `tianocore/edk2-redfish-client` `main`, pinned | source tree, staged into the sysroot |
| `meta-rpi5-uefi/recipes-bsp/edk2/edk2-rpi5-firmware_git.bb` | `tianocore/edk2` `master`, pinned, plus the three trees above | `RPI_EFI.fd`, `config.txt` |

One recipe per upstream repository: `edk2-rpi5-firmware` owns the `edk2` tree
and the out-of-tree packages under its own `files/`, and `DEPENDS` on the other
three, which fetch and patch their trees and stage them under
`${STAGING_DATADIR}/edk2`. The RPi5 port and its patch series therefore live in
`recipes-bsp/edk2-platforms/`, not in the firmware recipe. `do_compile` copies
`edk2-platforms` out of the sysroot before building, because wiring the optional
feature sets in rewrites `RPi5.dsc`/`.fdf`; `edk2-non-osi` and
`edk2-redfish-client` are read from the sysroot in place.

The `edk2-rpi5-firmware` recipe builds
`edk2-platforms/Platform/RaspberryPi/RPi5/RPi5.dsc`/`.fdf`, with
`arm-trusted-firmware`'s `bl31.bin` embedded as the FD's region-0 payload
(via `-D TFA_BUILD_ARTIFACTS=...`, the same mechanism
`worproject`/`NumberOneGit`'s own `build.sh` uses) and, if `RPI5_IPXE = "1"`
(the default), the iPXE driver embedded as an extra DXE driver FFS file
inserted into `RPi5.fdf` at build time -- see that recipe's `do_compile()`
for exactly how and why (a `sed`-based insertion, not a patch file, since the
upstream file's exact current whitespace isn't reliably knowable ahead of
time).

All `SRCREV`s are pinned to the exact commits `NumberOneGit/rpi5-uefi`'s own
git tree points at, i.e. what that project's own published images are built
from.

## Three things worth flagging up front

**"IPMI recipe" → iPXE.** The task that produced this repo asked for an
"IPMI recipe that builds an efi rom ... to enable ethernet/PXE" -- there's no
such thing as an EDK2-embeddable IPMI ROM for this purpose, but that
description exactly matches `../nuc-bios-build`'s actual `ipxe-efi_git.bb`
recipe (an EFI ROM built from iPXE, embedded in the firmware to give the
UEFI PXE/HTTP boot stack a NIC driver). Read as "iPXE", which is what
`meta-rpi5-uefi/recipes-bsp/ipxe/ipxe-efi_git.bb` builds.

**`arm-trusted-firmware` branch substitution, sort of.** The task specified
`https://github.com/ARM-software/arm-trusted-firmware.git` branch `rpi5`.
That branch has since been deleted upstream (`git ls-remote` no longer lists
it) -- but the exact commit `NumberOneGit/rpi5-uefi`'s own submodule pins
(`000fe221b859ee82a4e2f8bf2c96f0086a772c89`) is still fetchable directly from
that same `ARM-software` repository by SHA, even with the branch gone. So the
recipe keeps the URL exactly as given, using `nobranch=1` + a pinned `SRCREV`
instead of `branch=rpi5`. No fork substitution was actually needed here --
just pin-by-commit instead of pin-by-branch.

**iPXE does *not* drive RP1's onboard Ethernet.** RP1 (the RPi5 southbridge
with the onboard RJ45's MAC) has no PCI/USB-recognisable identity iPXE's
driver table knows about -- it's Raspberry Pi silicon with a Linux driver
only a couple of years old. Embedding iPXE gives the firmware's PXE/HTTP boot
stack a NIC driver for whatever iPXE *does* recognise: a PCIe NIC on the RPi5's
M.2/PCIe FPC connector, or a supported USB Ethernet dongle (iPXE's own
`DRIVERS_rpi` group -- `smsc95xx`/`lan78xx` -- already exists upstream for the
USB-Ethernet chips RPi3B+/4 use, and is included in this unrestricted build
too). It does **not** make the onboard jack usable in UEFI. This matches the
task's own "*This should enable ethernet/PXE*" hedge -- it does, but only for
add-on NICs.

## Local driver packages (pi-bmc port)

Beyond the upstream build, `meta-rpi5-uefi/recipes-bsp/edk2/files/` carries
the out-of-tree EDK2 source packages, and
`meta-rpi5-uefi/recipes-bsp/edk2-platforms/files/` the one small patch, porting the
`../u-boot` RPi 5 driver set to EDK2 (all fresh BSD-2-Clause-Patent code;
the GPL u-boot drivers were behavioral/wire-format reference only):

- **`Rp1GemPkg`** (`RPI5_RP1_ETH`) -- `Rp1GemDxe`, a native SNP driver for
  the onboard RJ45 (Cadence GEM_GXL in RP1), registers from Xilinx UG585 /
  FreeBSD `if_cgem.c`. What iPXE never covered.
- **`RpiBmcPkg`** (`RPI5_BMC`) -- the host side of the BMC shared-EEPROM
  contract (24c256 @0x50 on RP1 I2C1, real or BMC-emulated): `Rp1DwI2cDxe`
  (DesignWare I2C master), `EepromVarStoreDxe` (UbEfiVa variable blob at
  0x0000: restore at BDS, sync-back at ReadyToBoot/reset),
  `SmbiosEepromMirrorDxe` (SM3 blob at 0x6000), `BlkInfoMirrorDxe`
  (BLK1+JSON at 0x6800), `BootloaderConfigDxe` (blconfig -> UEFI variable,
  timestamp-gated), plus `Rp1GpioLib`/`BmcEepromLib`.
- **`0001-Rp1BusDxe-...patch`** (in `recipes-bsp/edk2-platforms/files/`, with
  the rest of the `edk2-platforms` series) -- extends upstream `Rp1BusDxe` to
  register the GEM and I2C1 blocks as vendor NonDiscoverable children (the xHCI
  pattern); the only upstream file change the set needs.
- **`RPI5_USBNET`** -- wires edk2's own (present-but-unwired) USB
  CDC-ECM/NCM/RNDIS class drivers into the platform, so the BMC's
  `g_ether` gadget is a bootable NIC.

Integration is patch-light by design: the packages ride an extra
`PACKAGES_PATH` entry and are pulled into `RPi5.dsc`/`.fdf` via the same
sed-marker idiom the iPXE embedding uses.

## Variables

Set any of these in `kas.yml`'s `local_conf_header` (or `local.conf`):

- `RPI5_IPXE` (default `"0"` since the native Rp1GemDxe covers onboard
  PXE) -- embed the iPXE driver for add-on PCIe/USB NICs.
- `RPI5_RP1_ETH` (default `"1"`) -- build/embed the native RP1 GEM
  onboard-Ethernet SNP driver (`Rp1GemPkg`).
- `RPI5_BMC` (default `"1"`) -- build/embed the BMC-integration driver set
  (`RpiBmcPkg`).
- `RPI5_USBNET` (default `"1"`) -- wire edk2's USB CDC-ECM/NCM/RNDIS
  drivers into the build.
- `RPI5_BUILD_TARGET` (default `"RELEASE"`) -- `RELEASE`, `DEBUG` or `NOOPT`.
- `RPI5_FW_VERSION` (default `${PV}`) -- `PcdFirmwareVersionString`.
- `RPI5_EDK2_EXTRA_FLAGS` (default empty) -- extra `build` args
  (`-D FOO=BAR`, `--pcd ...`) without overriding `do_compile` wholesale.
- `TFA_DEBUG` (default `"0"`) -- TF-A `DEBUG=1` build with UART crash
  logging.

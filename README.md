# rpi5-uefi-build

Yocto/kas build of the [worproject](https://github.com/worproject/rpi5-uefi) /
[NumberOneGit](https://github.com/NumberOneGit/rpi5-uefi) EDK2 UEFI firmware
for the Raspberry Pi 5, structured the same way as
[`../nuc-bios-build`](../nuc-bios-build): each upstream component is a bitbake
recipe (fetched by pinned `SRCREV`, no submodules) rather than a git submodule
checked directly into this repo.

Output: `RPI_EFI.fd` + `config.txt`, deployed to
`build/tmp/deploy/images/raspberrypi5-uefi/`. Flash `RPI_EFI.fd` as
`pieeprom.upd`-style firmware per the
[upstream README](https://github.com/NumberOneGit/rpi5-uefi#usage), or place
both files on the boot partition per that project's install instructions.

## Build

```sh
pip3 install kas
kas build kas.yml
```

## Layer / recipe layout

| Recipe | Upstream | Produces |
| --- | --- | --- |
| `meta-rpi5-uefi/recipes-bsp/arm-trusted-firmware/arm-trusted-firmware_git.bb` | `ARM-software/arm-trusted-firmware`, pinned commit (see caveat below) | `bl31.bin` |
| `meta-rpi5-uefi/recipes-bsp/ipxe/ipxe-efi_git.bb` | `ipxe/ipxe` master | `ipxe.efidrv`, `ipxe.efi` |
| `meta-rpi5-uefi/recipes-bsp/edk2/edk2-rpi5-firmware_git.bb` | `NumberOneGit/edk2`, `edk2-platforms`, `edk2-non-osi`, all `master` | `RPI_EFI.fd`, `config.txt` |

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

## Variables

Set any of these in `kas.yml`'s `local_conf_header` (or `local.conf`):

- `RPI5_IPXE` (default `"1"`) -- embed the iPXE driver; `"0"` for an
  unmodified `RPi5.fdf`.
- `RPI5_BUILD_TARGET` (default `"RELEASE"`) -- `RELEASE`, `DEBUG` or `NOOPT`.
- `RPI5_FW_VERSION` (default `${PV}`) -- `PcdFirmwareVersionString`.
- `RPI5_EDK2_EXTRA_FLAGS` (default empty) -- extra `build` args
  (`-D FOO=BAR`, `--pcd ...`) without overriding `do_compile` wholesale.
- `TFA_DEBUG` (default `"0"`) -- TF-A `DEBUG=1` build with UART crash
  logging.

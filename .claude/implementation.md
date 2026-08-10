# Porting the U-Boot RPi 5 drivers to EDK2

pi-bmc · firmware research dossier · verified 24/24 claims

An implementation path for every RPi 5 driver in `../u-boot` — as files and directories in the EDK2 tree, and as modifications to the meta-rpi5-uefi Yocto layer.

## Metadata

- **Date**: 2026-08-10
- **U-Boot tree**: v2026.07 + uncommitted RPi 5 port
- **EDK2 tree**: NumberOneGit/edk2-platforms @ 4e42610 (recipe-pinned)
- **Layer**: meta-rpi5-uefi (kas/poky scarthgap)
- **Method**: 13 agents · 3-vote adversarial verify · 0 refuted

## Verdicts

### The short version

- Most of the u-boot port needs no EDK2 counterpart. Of 20 capability areas analyzed, only 3 are P0 work. Much of the u-boot diff (BAR swap, DT association, dma-ranges parsing, composite-USB fixes, PSCI dispatch, NVMe address translation) compensates for U-Boot's own machinery — EDK2's architecture makes those problems structurally absent, verified against Rp1BusDxe.c and Bcm2712PciHostBridgeLib.c.

- The one real driver gap is onboard Ethernet. The RPi5 build wires the full NetworkPkg PXE/HTTP stack but ships zero SNP producers. A new Rp1GemDxe binding the existing gRp1BusProtocolGuid is buildable without patching upstream, and license-clean: the RP1 MAC is a Cadence GEM_GXL r1p09, publicly documented in Xilinx UG585, with BSD-2-Clause FreeBSD if_cgem.c as a reference.

- The BMC contract (EEPROM variables, SMBIOS mirror, blkinfo) is one substrate away. Everything blocks on an RP1 I²C master stack, absent from the platform. One small Rp1BusDxe.c patch + a DesignWare I²C DXE (reusing BSD Ampere/SynQuacer code) unlocks all three as clean out-of-tree drivers.

- Don't port GPL code; you rarely need to. Every new u-boot driver is GPL-2.0(+), Linux-derived — unacceptable in edk2-platforms (BSD-2-Clause-Patent only). Clean-room BSD paths exist for everything: the RP1 datasheet fully documents GPIO; UG585 + FreeBSD cover the GEM; wire formats (UbEfiVa, SM3 blob, BLK1 JSON) are interfaces, reimplementable freely.

- The layer already has both integration idioms you need. New drivers ship as a local package dir added as a 4th PACKAGES_PATH entry (zero upstream patches); edits to existing upstream files (Rp1BusDxe, PlatformSmbiosDxe, FdtDxe) ship as SRC_URI patches; module wiring reuses the proven sed-snippet anchor on !include NetworkPkg/Network.fdf.inc.

## Landscape

### What exists on each side

The u-boot side is a substantial uncommitted port: an RP1 southbridge stack (drivers/mfd/rp1.c PCI-endpoint glue, clk-rp1.c, rp1_gpio.c), BCM2712 SoC drivers (brcmstb_gpio.c, pinctrl-bcm2712.c, bcm2711_thermal.c), MACB changes adding a raspberrypi,rp1-gem Ethernet path, a host-side USB CDC-ECM driver, and a BMC integration layer that persists EFI variables, SMBIOS tables, and a block-device inventory into a shared I²C EEPROM.

The EDK2 side (the exact commit your recipe pins) is further along than its driver count suggests. Rp1BusDxe already binds the RP1 PCI endpoint (1de4:0001), maps BAR1, installs gRp1BusProtocolGuid, and registers both xHCIs — USB, SD/eMMC (with UHS voltage switching), NVMe, RTC-via-mailbox, RNG, display, and PSCI reset all work today. Missing: any NIC driver, RP1 GPIO, RP1 I²C, thermal ACPI, and hardware-backed variable storage (variables currently persist by rewriting RPI_EFI.fd on the FAT partition).

Ecosystem status (Aug 2026): worproject/rpi5-uefi was archived Feb 2025; the NumberOneGit fork adds only D0-stepping pinctrl fixes. Upstream tianocore/edk2-platforms still has no RPi5 platform. Nobody, in any fork or issue tracker, is working on an RP1 Ethernet UEFI driver — the P0 item here would be the first.

## Ground rules

### Licensing and the two integration vehicles

Licensing. edk2-platforms accepts BSD-2-Clause-Patent only ("no components covered by additional licenses"). The u-boot drivers are GPL-2.0(+) Linux derivatives, so there are three lanes: (a) clean-room BSD — preferred, viable for everything below (register offsets and wire formats are uncopyrightable facts; the GPL sources serve as behavioral reference only); (b) reuse of in-tree BSD code — Ampere DwI2cLib, SynQuacerI2cDxe, BcmGenetDxe's PHY layer, NetsecDxe's SNP shape; (c) GPL-as-aggregate — the iPXE precedent the layer already ships: a separately built GPL PE image inserted into the FD. Lane (c) is fine for self-contained binaries but must never be used for code co-owning the RP1 PCI function (see the iPXE hazard in §6).

Vehicle 1 — files/directory. New code lives in a self-contained EDK2 package directory with its own .dec/.dsc. Two proposed packages: Rp1GemPkg (the Ethernet driver, upstreamable to NumberOneGit later as Silicon/RaspberryPi/RpiSiliconPkg/Drivers/Rp1GemDxe) and RpiBmcPkg (the BMC-contract drivers). Because EDK2 resolves packages via PACKAGES_PATH, these need no upstream file changes at all:

meta-rpi5-uefi/recipes-bsp/edk2/files/
├── Rp1GemPkg/
│   ├── Rp1GemPkg.dec
│   ├── Rp1GemPkg.dsc                  # standalone build → Rp1GemDxe.efi
│   └── Drivers/Rp1GemDxe/
│       ├── Rp1GemDxe.inf
│       ├── DriverBinding.c            # binds gRp1BusProtocolGuid BY_DRIVER
│       ├── GemHw.c / GemHw.h          # Cadence GEM_GXL (UG585 / if_cgem.c)
│       ├── Phy.c                      # BCM54213PE, GenericPhy pattern
│       ├── Snp.c                      # EFI_SIMPLE_NETWORK_PROTOCOL
│       └── ComponentName.c
└── RpiBmcPkg/
    ├── RpiBmcPkg.dec                  # GUIDs: BMC contract, I2C device GUID
    ├── RpiBmc.dsc.inc / RpiBmc.fdf.inc  # sed-inserted into RPi5.dsc/.fdf
    ├── Drivers/Rp1DwI2cDxe/           # DW I2C master on RP1 I2C1 (BAR1+0x74000)
    ├── Drivers/EepromVarStoreDxe/     # UbEfiVa blob sync @ EEPROM 0x0000
    ├── Drivers/SmbiosEepromMirrorDxe/ # SM3 blob mirror @ 0x6000
    ├── Drivers/BlkInfoMirrorDxe/      # BLK1+JSON inventory @ 0x6800
    ├── Drivers/BootloaderConfigDxe/   # blconfig → UEFI variable
    └── Library/Rp1GpioLib/            # io_bank/sys_rio/pads MMIO helpers

Vehicle 2 — layer modification. The recipe (edk2-rpi5-firmware_git.bb) grows four kinds of change, all extending idioms it already uses:

# 1. Ship local packages + patches
SRC_URI += "file://Rp1GemPkg file://RpiBmcPkg \
            file://0001-Rp1BusDxe-register-I2C1-child.patch;patchdir=../edk2-platforms \
            file://0002-PlatformSmbiosDxe-uuid-type45.patch;patchdir=../edk2-platforms \
            file://rpibmc-dsc-snippet.inc file://rpibmc-fdf-snippet.fdf.inc \
            file://usbnet-dsc-snippet.inc file://usbnet-fdf-snippet.fdf.inc"

# 2. Widen the workspace (do_compile)
export PACKAGES_PATH="${S}:${EDK2_PLATFORMS_PATH}:${EDK2_NON_OSI_PATH}:${UNPACKDIR}/Rp1GemPkg:${UNPACKDIR}/RpiBmcPkg"

# 3. Wire modules via the existing sed-marker idiom (same anchor as iPXE):
#    RPi5.dsc  ← '!include NetworkPkg/Network.dsc.inc'  + snippet files
#    RPi5.fdf  ← '!include NetworkPkg/Network.fdf.inc'  + snippet files

# 4. New knobs, parallel to RPI5_IPXE
RPI5_USBNET ??= "1"     # CDC-ECM/NCM/RNDIS drivers (dsc/fdf only)
RPI5_RP1_ETH ??= "1"    # Rp1GemDxe
RPI5_BMC ??= "1"        # RpiBmcPkg driver set

Rule of thumb from the analysis: wholly new code → package dir on PACKAGES_PATH + snippet wiring; edits to existing upstream files → SRC_URI patch. Only three upstream files ever need patching: Rp1BusDxe.c (+RpiSiliconPkg.dec), PlatformSmbiosDxe.c, and — contingently — FdtDxe.c and AcpiTables.inf.

## Gap matrix

### All 20 capability areas

| Capability (u-boot source) | EDK2 status | Action | Effort | Prio |
| --- | --- | --- | --- | --- |
| RP1 GEM onboard Ethernet (macb.c) | missing | New Rp1GemDxe SNP driver (§5.1) | L | P0 |
| RP1 bus / MFD core (mfd/rp1.c) | exists | Extend Rp1BusRegisterDevices() (§5.2) | S | P0 |
| EFI vars in I²C EEPROM (efi_var_i2c.c) | partial | I²C stack + EepromVarStoreDxe (§5.3) | L | P0 |
| USB CDC-ECM NIC (usb/eth/ecm.c) | partial | dsc/fdf wiring only — drivers already in pinned edk2 (§5.4) | S | P1 |
| RP1 GPIO / pinmux (rp1_gpio.c) | missing | New Rp1GpioLib BASE library (§5.5) | M | P1 |
| SMBIOS enrichment + EEPROM mirror (sysinfo.c, smbios_i2c.c) | partial | Patch PlatformSmbiosDxe + new mirror DXE (§5.6) | M | P1 |
| RP1 clocks (clk-rp1.c) | missing | Skip the driver — 2 register writes fold into consumers (§5.7) | S | P2 |
| Thermal sensor (bcm2711_thermal.c) | missing | New SsdtThermal.asl ACPI zone, no DXE (§5.8) | S | P2 |
| Block-device inventory → EEPROM (blkinfo_i2c.c) | missing | New BlkInfoMirrorDxe (§5.9) | S | P2 |
| Bootloader-config publishing (rpi.c blconfig) | missing | New BootloaderConfigDxe (§5.10) | S | P2 |
| iPXE embedding layer (RPI5_IPXE) | exists | Keep; coexists with Rp1GemDxe (§6, §7) | S | P2 |
| RP1 xHCI USB (xhci-brcm.c) | exists | None — u-boot delta is one compatible-string line | — | P3 |
| Composite USB gadgets (usb-uclass.c, usb_storage.c) | n/a | None — EDK2 binds per-interface by design | — | P3 |
| BCM2712 SoC GPIO (brcmstb_gpio.c) | exists | None for boot; optional power-button DXE (§7) | — | P3 |
| BCM2712 pinctrl (pinctrl-bcm2712.c) | exists | None; watch DTB-handoff hazard (§6) | — | P3 |
| Reset / poweroff (reset.c) | n/a | None — platform is PSCI-native | — | P3 |
| NVMe DMA fixes (nvme.c) | exists | None — inbound window is identity-mapped | — | P3 |
| SD / eMMC (no u-boot delta) | exists | None — EDK2 path is richer (UHS, voltage switch) | — | P3 |
| RTC (no u-boot driver) | exists | None — EDK2 is ahead (mailbox-backed GetTime) | — | P3 |
| Boot SPI NOR access (bootspi.c) | n/a | None — vestigial in u-boot; BMC owns pieeprom | — | P3 |

## Work plan

### Implementation paths for the real work

### 5.1 Rp1GemDxe — onboard Ethernet SNP driver

**Status**: missing | **Priority**: P0 | **Effort**: L

Replaces: u-boot macb.c rp1-gem path + clk-rp1 TSU writes. The single biggest gap; nobody in the ecosystem has attempted it.

#### Files / directory

UEFI_DRIVER at Rp1GemPkg/Drivers/Rp1GemDxe/ (layout in §3; upstreamable later as RpiSiliconPkg/Drivers/Rp1GemDxe). Binding: DriverBinding opens the existing gRp1BusProtocolGuid BY_DRIVER on the RP1 PCI handle — patchless; GEM base = GetPeripheralBase() + 0x100000, eth_cfg wrapper at +0x104000. Child handle carries EFI_SIMPLE_NETWORK_PROTOCOL + AIP + a MAC() device-path node, so BDS mints network boot options natively (u-boot's CONFIG_EFI_NET_PXE_BOOT hack becomes free). Registers: clean-room Cadence GEM_GXL r1p09 from Xilinx UG585 ch.16, FreeBSD if_cgem.c (BSD-2-Clause) as reference. Three RP1-specific facts to replicate from the u-boot port: 64-bit DMA descriptors (DMACFG ADDR64 + TBQPH/RBQPH, burst 16); AXI-pipeline reg 0x0054 = AR2R 8 / AW2W 8 / AW2B_FILL 1; MDC divisor from the firmware-fixed 200 MHz clk_sys. No clock driver — RP1 firmware pre-programs the tree (verified: u-boot passes traffic with a clock driver that only pokes the PTP TSU divider). DMA: do not port dev_phys_to_bus() — EDK2's inbound window is identity-mapped 0–64 GB; link NonCoherentDmaLib with PcdDmaDeviceLimit 0xbfffffff (same scoping as NonCoherentIoMmuDxe) for uncached sub-3 GB rings. PHY: BCM54213PE at MDIO addr 1, generic C22 autoneg lifted from BcmGenetDxe's GenericPhy; no reset GPIO needed on Pi 5. MAC: RASPBERRY_PI_FIRMWARE_PROTOCOL.GetMacAddress, VPU-DTB fallback. Polled SNP; no MSI-X.

#### Layer modification

Zero upstream patches: ship Rp1GemPkg in SRC_URI, append to PACKAGES_PATH, pre-build standalone (build -p Rp1GemPkg/Rp1GemPkg.dsc — DMA PCDs scoped inside the package dsc), then embed Rp1GemDxe.efi with a second sed-inserted FDF snippet on the !include NetworkPkg/Network.fdf.inc anchor — byte-for-byte the iPXE idiom. Alternative: source-build inside the main invocation via dsc/fdf sed snippets. Knob: RPI5_RP1_ETH ??= "1".

#### Licensing

Clean-room BSD-2-Clause-Patent. Datasheet gives eth/eth_cfg bases; UG585 documents the identical GEM core; FreeBSD if_cgem.c is directly adaptable. Known carried-over limitation: tx_clk stays at firmware's 125 MHz — reliable at gigabit only, unless CLK_ETH divider writes are added later.

### 5.2 Rp1BusDxe extension — the registration point for everything

**Status**: exists | **Priority**: P0 | **Effort**: S

Replaces: u-boot mfd/rp1.c + its pci-uclass/of_addr core hacks — all three of which (BAR swap, DT association, dma-ranges) are structurally unnecessary in EDK2.

#### Files / directory

Rp1BusDxe already is the MFD equivalent. The gap: Rp1BusRegisterDevices() is a static list registering only the two xHCIs. Extend it with one RegisterNonDiscoverableMmioDevice call per new peripheral (I²C1 now; SDIO later if ever needed), each with a vendor GUID in the local .dec, opened gRp1BusProtocolGuid BY_CHILD_CONTROLLER (mirroring the DWC3 pattern), plus MSI-X unmask in Rp1BusEnableInterrupts() where needed (vectors already enumerated in Rp1.h). Optional 5-line parity fix: validate SYSINFO chip-id == 0x20001927 (C0) like u-boot's probe does.

#### Layer modification

This touches an upstream file, so it's the one mandatory SRC_URI patch of the P0 set: 0001-Rp1BusDxe-register-I2C1-child.patch;patchdir=../edk2-platforms (one hunk + a .dec GUID line). Note Rp1GemDxe deliberately avoids needing this patch by binding the bus protocol directly.

#### Licensing

Existing BSD code; extensions are fresh BSD against Rp1.h's own defines. Nothing from GPL rp1.c is needed.

### 5.3 Rp1DwI2cDxe + EepromVarStoreDxe — hardware-backed EFI variables

**Status**: partial | **Priority**: P0 | **Effort**: L

Replaces: u-boot efi_var_i2c.c (UbEfiVa blob, 24c256 @0x50 on RP1 I2C1, offsets 0x0000–0x3fff). Today variables persist by rewriting RPI_EFI.fd on the FAT partition — invisible to the BMC.

#### Files / directory

(A) I²C substrate: RpiBmcPkg/Drivers/Rp1DwI2cDxe — DriverBinding on gEdkiiNonDiscoverableDeviceProtocolGuid matching the new GUID from §5.2 (RP1 I2C1 at BAR1+0x74000), produces gEfiI2cMasterProtocolGuid. Register layer transplanted from BSD Ampere DwI2cLib (DW_IC_CON/TAR, COMP_PARAM_1 FIFO autodetect, 100 kHz); binding shape from SynQuacerI2cDxe. GPIO2/3 pinmux (funcsel 3 + pull-up) via Rp1GpioLib if VPU firmware defaults don't already set it (verify on hardware). (B) Variable sync: EepromVarStoreDxe — leaves VariableRuntimeDxe/FTW/VarBlockServiceDxe untouched and keeps the BMC wire contract: on I²C arrival (protocol notify, during BDS connect — before boot-option evaluation) read and validate the UbEfiVa blob (magic 0x0161566966456255, len ≤ 0x4000, CRC32) and SetVariable() each NV entry — BMC-written BootOrder/BootNext take effect that same boot; at ReadyToBoot + reset-notify (VarBlockServiceDxe's own event set), serialize NV variables back, compare-skip, page-chunked 64 B writes. The full-FVB alternative (FVB-on-EEPROM) was analyzed and rejected: a 0xe000 varstore + FTW spare can't fit 16 KiB, and the BMC would have to parse EDK2's internal varstore format instead of UbEfiVa.

#### Layer modification

RpiBmcPkg dir in SRC_URI → 4th/5th PACKAGES_PATH entry; modules wired by sed-inserting !include RpiBmcPkg/RpiBmc.dsc.inc / .fdf.inc at the Network include anchors — the existing iPXE mechanism extended to the dsc. Plus the one Rp1BusDxe patch from §5.2. Knob: RPI5_BMC ??= "1".

#### Licensing

Clean-room BSD: DW I²C code from BSD Ampere; UbEfiVa is a wire format (interface, not expression). Known limitation, same as today's VarBlockServiceDxe: post-ExitBootServices SetVariable is not persisted — acceptable since the BMC owns out-of-band edits.

### 5.4 USB CDC-ECM/NCM/RNDIS — configuration-only

**Status**: partial | **Priority**: P1 | **Effort**: S

Replaces: u-boot drivers/usb/eth/ecm.c (the BMC gadget network link). The drivers already exist, unbuilt, in the exact edk2 commit the recipe pins.

#### Files / directory

No new code. Add MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon + UsbCdcEcm (optionally UsbCdcNcm, UsbRndis) to RPi5.dsc [Components] and RPi5.fdf. A Linux g_ether gadget on an RP1 USB port then surfaces as SNP → full PXE/HTTP boot over the BMC link. EDK2's UsbBusDxe already handles composite gadgets per-interface, so u-boot's usb-uclass/usb_storage surgery has no counterpart.

#### Layer modification

Two sed snippets (usbnet-dsc-snippet.inc, usbnet-fdf-snippet.fdf.inc) on the existing grep-guarded Network include markers, gated by RPI5_USBNET ??= "1". Mirrors the iPXE idiom exactly.

#### Licensing

None — the AMI-donated UsbNetwork drivers are BSD, in-tree. Caveat: they're the least battle-tested piece; validate against the actual BMC gadget early, fall back to NCM/RNDIS framing from the BMC side if ECM misbehaves.

### 5.5 Rp1GpioLib — RP1 header GPIO / pinmux

**Status**: missing | **Priority**: P1 | **Effort**: M

Replaces: u-boot rp1_gpio.c (54 pins, 3 banks). Firmware needs exactly two uses: I2C1 pinmux (GPIO2/3 → funcsel 3) and optional PHY/LED control — not a general pinctrl.

#### Files / directory

Stateless BASE library mirroring the proven Bcm2712GpioLib shape — not a protocol driver, not a DT parser: RpiBmcPkg/Library/Rp1GpioLib/, every function taking GetPeripheralBase() as first arg. Register recipe (banks 0–27/28–33/34–53 at +0x0000/+0x4000/+0x8000 across the IO/RIO/PADS windows): IO CTRL per-pin at pin*8+4, FUNCSEL bits 4:0 (GPIO=5), OUTOVER/OEOVER to peripheral on mux; RIO OUT/OE/IN bitmasks with RP2040-style XOR/SET/CLR aliases at +0x1000/2000/3000; PADS pull bits 3:2, IN_ENABLE bit 6, OUT_DISABLE bit 7. API: SetFunction / SetPull / SetDirection / Read / Write.

#### Layer modification

Pure-new code inside RpiBmcPkg (consumers reference the LibraryClass); zero upstream edits. Drops into RpiSiliconPkg unchanged if upstreamed later — it depends only on Rp1.h.

#### Licensing

Clean-room BSD from RP1 datasheet §3.1.4, which documents io_bank/sys_rio/pads completely (CC BY-ND restricts the document, not the facts). GPL rp1_gpio.c is cross-check only.

### 5.6 SMBIOS parity + EEPROM mirror

**Status**: partial | **Priority**: P1 | **Effort**: M

Replaces: u-boot sysinfo.c (deterministic UUID, Type 45) + smbios_i2c.c (verbatim SM3 mirror at EEPROM 0x6000, cap 0x800).

#### Files / directory

(1) Patch PlatformSmbiosDxe.c (shared with RPi3/4 — gate on board family ≥ 5): replace the ad-hoc UUID with u-boot's deterministic RFC-4122-shaped derivation so BMC-visible identity is stable across both firmwares; add a Type 45 firmware-inventory record from the VPU DTB /chosen/bootloader node. (2) New SmbiosEepromMirrorDxe in RpiBmcPkg: at ReadyToBoot, serialize the SMBIOS 3.0 entry point + table into the SM3 blob, compare-skip, chunked write to EEPROM 0x6000 via gEfiI2cMasterProtocolGuid. Blob format is firmware-agnostic — the BMC parser needs no change.

#### Layer modification

The PlatformSmbiosDxe edits = SRC_URI patch (upstream-shared file). The mirror DXE rides the RpiBmcPkg snippet includes.

#### Licensing

BSD throughout; UUID derivation and blob layout are interface specs, GPL sources behavioral reference only.

## Small P2 items

All have **Priority**: P2 | **Effort**: S

### 5.7 RP1 clocks — skip the driver

Verified: u-boot's clk-rp1 is a near-stub proving RP1 firmware pre-programs the whole tree; its only hardware writes are the ETH_TSU divider pair (PTP only). Fold those two MmioWrite32s + the 200 MHz clk_sys constant into consumers (or a 30-line Rp1ClkLib). No DXE, no protocol, no dsc change beyond the consumer.

### 5.8 Thermal — ACPI, not a driver

New Platform/RaspberryPi/RPi5/AcpiTables/SsdtThermal.asl: ThermalZone with an OperationRegion on the AVS monitor (0x10_7D54_2200), _TMP = 2732 + (450000 − 550·raw)/100 with validity-bit check, _PSV/_CRT at 85/90 °C. DT-mode boots need nothing (firmware DTB carries avs-monitor). Layer: SRC_URI patch (touches AcpiTables.inf). Coefficients are published facts — clean BSD ASL.

### 5.9 BlkInfoMirrorDxe — drive inventory for Redfish

ReadyToBoot handler in RpiBmcPkg: enumerate BlockIo handles (skip logical partitions), pull model/serial/rev from DiskInfo (NVMe Identify / SCSI Inquiry), emit the identical BLK1+u16le+JSON blob to EEPROM 0x6800, compare-skip. Feature-gate with a PCD (u-boot's equivalent is currently compiled out too). Zero upstream edits.

### 5.10 BootloaderConfigDxe — blconfig → UEFI variable

Small DXE in RpiBmcPkg: parse VPU DTB for /chosen/bootloader update-timestamp + the blconfig nvmem-rmem region; only when the timestamp advances, SetVariable("BootloaderConfig", d1a0f2c4-…) and bump BootloaderUpdateTimestamp — same EEPROM-wear discipline as u-boot. GUID + names are the frozen BMC contract, declared in RpiBmcPkg.dec. Depends on §5.3 to be BMC-visible.

## Hazards

### Watch-items the analysis surfaced

- Never solve onboard Ethernet inside iPXE. An iPXE RP1 driver would open PciIo BY_DRIVER on the single RP1 PCI function and race Rp1BusDxe for exclusive ownership — if iPXE wins, both xHCIs (all USB) die. Structural, not fixable by load ordering. Keep iPXE for add-on NICs; native Rp1GemDxe is the only safe route to the onboard jack. (Coexistence of the two is clean — disjoint hardware, two SNPs into one NetworkPkg stack.)

- DTB-handoff pinctrl hazard transfers from u-boot. u-boot strips all pinctrl-* consumer properties before booting Linux because mainline pinctrl-bcm2712 writes outside its advertised window → async SError panic. EDK2's FdtDxe does no such strip in DT mode. If target kernels crash, port the ~40-line node-walk strip into FdtDxe.c (SRC_URI patch), gated on family ≥ 5.

- Verify GPIO2/3 I²C mux on hardware first. The VPU firmware may already mux I2C1's pins before UEFI runs — if so, §5.3 works before Rp1GpioLib exists, decoupling P0 from P1.

- D0-stepping display quirk: worproject builds break graphical output on newer D0 boards/EEPROMs; the NumberOneGit pin exists precisely for D0 — keep the 2025-06-09+ EEPROM noted in its README.

- FD headroom: each embedded driver (iPXE + Rp1GemDxe + BmcPkg) costs FVMAIN space — watch RPI_EFI.report.txt.

- Optional power-button service: u-boot polls GIO20 edge-latch + PSCI poweroff; Bcm2712GpioLib lacks the edge registers (verified). Recommended: skip — the BMC owns power sequencing.

## Method

### How this was verified

13 agents in three phases: 7 parallel inventory readers (4 over the u-boot diff, 2 over the pinned edk2-platforms checkout, 1 web), 3 domain gap-analysts instructed to re-verify pivotal points against both trees, then 24 load-bearing claims put to a 3-voter adversarial panel (source-code ground truth · EDK2 architecture · external sources), each voter instructed to refute. Zero claims were refuted or contested. Key ground-truth anchors: Rp1BusDxe.c BAR handling and static device registration; Bcm2712PciHostBridgeLib.c:157-159 identity inbound window; VarBlockServiceDxe.c file-backed persistence; the one-line xhci-brcm.c diff; clk-rp1.c's single-entry clock table; UsbNetwork presence at the pinned edk2 SHA via GitHub API.

## Sources

- worproject/rpi5-uefi — archived Feb 4 2025; feature matrix (RP1 Ethernet/GPIO/PWM/EEPROM-NVRAM not working)
- NumberOneGit/rpi5-uefi — live continuation; D0 pinctrl remap; pins edk2-platforms @4e42610 (this build's SRCREV)
- tianocore/edk2-platforms Readme — BSD-2-Clause-Patent only; no RPi5 platform upstream as of Aug 2026
- RP1 Peripherals datasheet (CC BY-ND 4.0) — full GPIO register docs §3.1.4; eth/eth_cfg bases ch.7; clocks deferred to Linux driver
- FreeBSD sys/dev/cadence/if_cgem.c — BSD-2-Clause Cadence GEM driver from public Xilinx UG585 docs
- FreeBSD15-RPi5-modules — out-of-tree RP1 GEM proof that non-Linux PCIe-attached drivers work
- FSF GPL FAQ — mere aggregation · iPXE licensing — the embedded-GPL-driver precedent the layer already uses
- Local ground truth: ~/src/pi-bmc/u-boot (uncommitted RPi 5 port, all new drivers GPL-2.0(+)); NumberOneGit/edk2-platforms @ 4e426104; meta-rpi5-uefi/recipes-bsp/edk2/edk2-rpi5-firmware_git.bb


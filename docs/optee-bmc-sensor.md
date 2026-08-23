# OP-TEE BMC sensor service + SCMI for the Raspberry Pi 5

This adds a Secure-EL1 (OP-TEE / BL32) service layer to the RPi5 UEFI
firmware:

1. **BMC sensor push** — a pseudo-TA samples the BCM2712 die temperature
   on a secure timer and pushes a small record to the BMC over RP1 I2C1
   (the pi-bmc EEPROM wire contract). OP-TEE is the I2C **master**; the
   BMC reads a fixed EEPROM offset. This is the "pragmatic push service."
2. **SCMI server** — the same secure world exposes the die temperature
   (SCMI Sensor Management, protocol `0x15`) and the RP1 Active Cooler
   fan (SCMI Performance Domain, protocol `0x13`) to Linux, over the
   standard `arm,scmi-smc` transport: a fast SiP SMC that TF-A forwards
   into OP-TEE (see "SCMI transport" below).

Everything is gated so a build with OP-TEE off is byte-identical to the
prior firmware.

## Boot flow and the late-init handshake

TF-A boots BL32 (OP-TEE) **before** BL33 (EDK2). But the RP1 southbridge
is a PCIe endpoint: its register window (I2C1, PWM, clocks) has no address
until EDK2 enumerates PCIe and assigns BAR1. So OP-TEE cannot map RP1 at
its own boot. The sequence is:

```
VPU → armstub8-2712.bin (RPI_EFI.fd @0x0)
  BL31 (TF-A, SPD=opteed)
    copies the OP-TEE image from FD offset 0x330000 to 0x1D000000
    enters BL32 (OP-TEE)  ── secure timer + sampler start; SCMI server up
  BL33 (EDK2)
    PciHostBridgeDxe enumerates segment 2, assigns RP1 BAR1 (0x1F_00000000)
    Rp1BusDxe binds RP1, installs gRp1BusProtocolGuid
    RpiOpteeSensorDxe: muxes GPIO2/3 → I2C1, SMC → pTA CMD_INIT(BAR)
      OP-TEE maps the RP1 window; the I2C push and the SCMI fan go live
    FdtDxe: injects /firmware/optee, /firmware/scmi, /reserved-memory/optee;
            disables the RP1 i2c@74000 node in the OS device tree
  Linux
    OP-TEE driver binds (/firmware/optee); arm-scmi binds (/firmware/scmi)
    optional: bmc_sensord re-runs CMD_INIT if the OS moved the BAR
```

The handshake is the standard OP-TEE **message/SMC** path (EDK2 uses
ArmPkg's `OpteeLib`), *not* device-tree — the DT that EDK2 patches is the
non-secure tree for Linux; OP-TEE never reads it.

## Memory carve-out

A single hole is carved out of "System RAM < 1 GB" and must match across
four places (change them together):

| Region | Range | Where |
| --- | --- | --- |
| OP-TEE TZDRAM | `0x1D000000`–`0x1F000000` (32 MiB) | `plat-rpi5/conf.mk` `CFG_TZDRAM_*`; TF-A `BL32_MEM_*`; EDK2 `PcdOpteeTzdram*` |
| OP-TEE static SHM | `0x1F000000`–`0x1F400000` (4 MiB) | `plat-rpi5/conf.mk` `CFG_SHMEM_*`; EDK2 `PcdOpteeShmSize` |

- TF-A (`0001-rpi5-support-OP-TEE-as-BL32...`) copies `tee-raw.bin` from
  the FD to `BL32_MEM_BASE` and enters it via the `opteed` dispatcher.
- EDK2 `RaspberryPiMem.c` splits the below-1 GB RAM descriptor around the
  hole: TZDRAM is device-mapped and given no HOB (invisible to the OS,
  like the GPU carve-out); the SHM is mapped write-back with a reserved
  HOB so `OpteeLib` can use it.
- `FdtDxe` adds `/reserved-memory/optee` (no-map) as belt-and-suspenders
  for a DT-only boot. (The Pi 5 has no TZASC, so protection of TZDRAM is
  by memory-map reservation, not hardware.)

## SCMI transport: `arm,scmi-smc` via a TF-A SiP forward

Linux uses the standard `arm,scmi-smc` transport: it writes an SCMI
message into a static SMT page and rings a fast SiP SMC
(`arm,smc-id = <0x82000010>`). The SCMI server lives in OP-TEE, so TF-A
forwards that SMC into BL32:

- **TF-A** (`0002-rpi5-forward-the-SCMI-doorbell...`) registers a SiP
  runtime service on the otherwise-unused SiP OEN (the platform's PCI
  config SMCs are *Standard Service* calls). Its handler,
  `rpi_sip_forward_to_optee`, mirrors `opteed_smc_handler`'s fresh-request
  path using the dispatcher's exported per-core contexts
  (`opteed_sp_context[]`, `optee_vector_table`): it saves the NS context
  and ERETs into OP-TEE's `fast_smc_entry` with the SiP FID in x0. OP-TEE
  finishes with `TEESMC_OPTEED_RETURN_CALL_DONE`, and opteed restores the
  saved NS context and lands the result straight back in the Linux SCMI
  driver — the SiP handler never sees the return leg. Only fast calls are
  forwarded (a yielding forward would surface OP-TEE RPC codes to a caller
  that doesn't speak them), and only once OP-TEE is resident on the core.
  The forward lives entirely in a new platform file
  (`plat/rpi/common/rpi_scmi_svc.c` + `-Iservices/spd/opteed`); no opteed
  code changes.
- **OP-TEE** builds the SMT fastcall transport (`CFG_SCMI_MSG_SMT` +
  `CFG_SCMI_MSG_SMT_FASTCALL_ENTRY`). `plat-rpi5` overrides the weak
  `tee_entry_fast`: when it sees the SiP FID it calls
  `scmi_smt_fastcall_smc_entry(0)` and returns `OPTEE_SMC_RETURN_OK`. The
  SMT channel is **non-threaded** — the fastcall runs in atomic context —
  so the fan driver uses a spinlock, not a mutex, and any future slow
  sensor must serve from the polling cache, never touch a bus in the
  handler.
- **SMT page**: one 4 KiB page at the **top** of the reserved SHM window
  (`SCMI_SMT_BASE = 0x1F3FF000`). `CFG_SHMEM_SIZE` shrinks by that page so
  OP-TEE's SHM pool never hands it out. The whole
  `0x1F000000`–`0x1F400000` window is already `EfiReservedMemoryType`, so
  Linux won't touch the page; `FdtDxe` also wraps it in an `mmio-sram` +
  `arm,scmi-shmem` node the `scmi` node references by a generated phandle.

The alternative OP-TEE-native transport (`linaro,optee-scmi`, via OP-TEE's
SCMI PTA and dynamic SHM) needs zero TF-A changes but only works once the
Linux OP-TEE driver is up; the `arm,scmi-smc` path here works for any
SMCCC caller independent of `optee.ko`, at the cost of the SiP forward and
the atomic-context restriction. Switching back is a `conf.mk`
transport-flag change plus the `FdtDxe` node shape.

## What is implemented vs. what is an extension point

**Implemented and build-verified:**
- SoC die temperature (AVS monitor MMIO) — pushed over I2C and exposed as
  SCMI sensor 0.
- RP1 Active Cooler fan — SCMI performance domain 0, 5 levels (the
  ActiveCoolerDxe duty table), driving PWM1 ch3.

**Extension points (hooks are in place; hardware not verified on this
board, so left unimplemented):**
- PMIC (DA9091) voltage/current over the BCM2712 internal BSC I2C, and
  external I2C sensors (INA219 power, TMP102 ambient, …) over RP1 I2C.
  Add a `sensor_id` case in `scmi_server.c`'s `plat_scmi_sensor_*` and a
  reader; for **slow** buses use the autonomous-polling-cache pattern the
  sensor pTA already demonstrates (a secure-timer callout samples into a
  spinlock-protected cache; clients read the cache, never the bus) so a
  synchronous SCMI SMC never blocks a Linux CPU on an I2C conversion. The
  SoC temp reads AVS directly because it is a single non-blocking MMIO
  status register.

## Sensor record on the BMC EEPROM

Written to EEPROM offset `0x7800` (spare region of the pi-bmc 24c256 map),
little-endian, 32 bytes, one 64-byte page:

```c
struct bmc_sensor_record {
	uint32_t magic;        // "SNSR" 0x52534E53
	uint16_t version;      // 1
	uint16_t length;       // 32
	uint32_t seq;          // increments each sample
	int32_t  soc_temp_mc;  // milli-Celsius
	uint32_t uptime_s;
	uint32_t status;       // PTA_BMC_SENSOR_STATUS_*
	uint32_t reserved;
	uint32_t crc32;        // IEEE CRC32 of bytes 0..27
};
```

The BMC just polls this offset; no I2C-target/slave stack is needed on the
Pi side. (An SCMI-over-I2C-target design — the Pi as I2C slave answering
SCMI frames — is possible but heavier; see "Alternatives" below.)

## Normal-world daemon

`docs/optee-sensor/bmc_sensord.c` (needs optee_client `libteec`):

- `bmc_sensord --once` — print the latest cached sample.
- `bmc_sensord` — block on `CMD_WAIT`, printing each new sample (woken by
  the OP-TEE async notification bottom half).
- `bmc_sensord --init [--bar ADDR]` — re-run the RP1-BAR handshake if the
  OS relocated the BAR (normally unnecessary — EDK2 already did it).

## Files

OP-TEE (recipe `meta-rpi5-uefi/recipes-bsp/optee-os/`):
- `files/plat-rpi5/` overlay: `rp1_periph.[ch]` (shared BAR window map),
  `rp1_i2c.[ch]` (DW I2C master), `soc_temp.[ch]` (AVS temp),
  `bmc_sensor_pta.c` + `pta_bmc_sensor.h` (the push pTA), `scmi_server.c`
  (SCMI plat hooks), `rp1_pwm.[ch]` (fan).
- `files/0001-plat-rpi5-...patch` — GIC/timer/notif/SCMI wiring in the
  four upstream plat files.
- `files/0002-scmi-msg-add-Sensor-Management-protocol-0x15.patch` — the
  sensor protocol added to the lightweight scmi-msg framework (+ a
  perf_domain.c type-mismatch fix GCC 13 needs).

TF-A (`recipes-bsp/arm-trusted-firmware/files/`):
- `0001-rpi5-support-OP-TEE...patch` — `RPI5_OPTEE` knob → `SPD=opteed`,
  BL32 preloaded-image copy + entry.
- `0002-rpi5-forward-the-SCMI-doorbell...patch` — the SiP runtime service
  (`plat/rpi/common/rpi_scmi_svc.c`) forwarding the SCMI doorbell into
  OP-TEE. Compiled only with `SPD=opteed`.

EDK2 (`recipes-bsp/edk2-platforms/files/edk2-platforms/Platform/RaspberryPi/RPi5/`):
- `Drivers/RpiOpteeSensorDxe/` — the EDK2→OP-TEE handshake driver.
- `RPi5.dsc`/`.fdf`/`.dec`, `Library/PlatformLib/RaspberryPiMem.c` — the
  `RPI5_OPTEE` build option, the FD region for `tee-raw.bin`, the PCDs and
  the memory carve-out.
- `files/0037-FdtDxe-inject-OP-TEE-...patch` — the OS device-tree nodes.

`recipes-bsp/edk2-non-osi` files `tee-raw.bin` next to `bl31.bin` for the
FDF, mirroring the TF-A arrangement.

## Build knobs

- `RPI5_OPTEE` (default `1`) — TF-A `SPD=opteed` + the EDK2 `RPI5_OPTEE`
  define. `0` drops OP-TEE entirely (byte-identical to the prior firmware).
- `OPTEE_DEBUG` (default `0`) — OP-TEE debug core with the full DMSG/FMSG
  trace on the PL011.

## Alternatives considered (not built)

- **SCMI over I2C (Pi as target/slave).** SCMI 3.0 defines an I2C/SMBus
  transport; an external MCU could master SCMI frames to the Pi. OP-TEE's
  transport layer only does SMT/MSG, so this needs a from-scratch DW-I2C
  *target*-mode driver plus a secure FIQ handler feeding
  `scmi_process_message()`. Heavier than the push service and only worth
  it if the BMC already speaks SCMI. The lightweight push (this design) is
  the recommended path.
- **RPMB-backed UEFI variables (StandaloneMM).** Requested but not built:
  the EDK2 `OpteeClientDxe`/`OpteeRpmbDxe` drivers named in the classic
  recipe do **not** exist in edk2-stable202608, and the modern path is a
  StandaloneMM variable image running in S-EL0 under OP-TEE — which needs
  RPMB-capable eMMC. The Pi 5 boots SD/NVMe, neither of which exposes an
  RPMB partition, so there is no secure backing store to route to. The
  firmware keeps its working FD-backed authenticated variable store.

## RPMB variable storage — assessment (not implemented)

The request was to move UEFI variables to OP-TEE-mediated RPMB with the
StandaloneMM variable stack. Two blockers, verified against this tree:

1. **The named EDK2 drivers do not exist here.** `edk2-stable202608` has
   no `ArmPkg/Drivers/OpteeClientDxe` and no `ArmPkg/Drivers/OpteeRpmb/…`.
   `grep` of the pinned edk2 finds only `MmCommunicationDxe` and
   `VariableSmmRuntimeDxe`. The modern secure-variable path is **not** a
   pair of DXE drivers; it is a StandaloneMM firmware volume (the variable
   driver + `VariableRuntimeDxe`'s SMM half) that runs in **S-EL0 as an
   OP-TEE Secure Partition**, reached from EDK2 via `MmCommunication` →
   `MM_COMMUNICATE` SMC → OP-TEE. Building that means a whole second EDK2
   `StandaloneMmPkg` image loaded as an early SP, plus OP-TEE built with
   `CFG_STMM_PATH`/the StMM SP — a large subsystem, not a `.dsc` edit.

2. **No RPMB hardware on the boot media.** RPMB is an eMMC feature. The
   Pi 5 boots from SD card or NVMe; neither exposes an RPMB partition, so
   even a correct StMM stack would have nothing to bind its authenticated
   store to. (OP-TEE could fall back to a REE-file store on the ESP, but
   that is not RPMB and buys little over the current FD store.)

Recommendation: keep the FD-backed authenticated variable store (Secure
Boot works against it today). Revisit only if the board gains eMMC, at
which point the work is "add a StandaloneMM SP," documented upstream in
OP-TEE `documentation/architecture/stmm.rst` and EDK2
`StandaloneMmPkg`.
```

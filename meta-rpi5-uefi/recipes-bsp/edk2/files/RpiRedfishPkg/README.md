# RpiRedfishPkg — Pi 5 ⇄ BMC Redfish-over-USB glue

EDK2 Redfish Host Interface (DSP0270) support for the Raspberry Pi 5, talking
to the pi-bmc nanokvm BMC's Redfish service over the BMC's USB **CDC-NCM**
gadget on an RP1 USB port. This is the JetKVM method from nuc-bios-build's
`NucRedfishPkg`, with two deliberate differences:

* **CDC-NCM instead of CDC-ECM** — the nanokvm gadget's `ncm.usb0` function.
  All three of EDK2's in-tree USB network class drivers (ECM/NCM/RNDIS, plus
  their shared `NetworkCommon` UNDI, wired by the recipe's usbnet snippet,
  `RPI5_USBNET`) are built so the gadget mode is the BMC's choice; NCM is
  what the nanokvm presents, and `UsbCdcNcm` turns it into the SNP
  interface the stack rides. BDS never offers the link as a boot target:
  patch 0005 prunes auto-created network boot options for USB NICs.
* **HTTP Basic instead of AuthMethodNone** — nanokvm-app puts `/redfish/v1`
  behind `CheckAuth()` (session token or Basic) unless authentication is
  disabled; `RpiRedfishCredentialLib` sends the PCD credentials, or degrades
  to `AuthMethodNone` when the user PCD is empty.

It replaces the I2C shared-EEPROM sync stack that `RpiBmcPkg` used to carry
(`Rp1DwI2cDxe` + `BmcEepromLib` + `EepromVarStoreDxe` +
`SmbiosEepromMirrorDxe` + `BlkInfoMirrorDxe`).

## What runs at boot

    RedfishHostInterfaceDxe    publishes SMBIOS type 42 from RpiRedfishHostInterfaceLib
    RedfishDiscoverDxe         matches the type 42 MAC to the NCM NIC, configures REST EX
    RedfishConfigHandlerDriver signals "service discovered"
    RpiRedfishSyncDxe          <- the part that actually talks to the BMC

`RpiRedfishSyncDxe` produces `EDKII_REDFISH_CONFIG_HANDLER_PROTOCOL` and
performs the exchange (all fail-open — a dead BMC never blocks booting):

| Step | Request | Replaces |
|---|---|---|
| 1 | `GET /redfish/v1/` | — (link liveness proof) |
| 2 | `PATCH /redfish/v1/Systems/1` (identity + `BootProgress`) | SmbiosEepromMirrorDxe |
| 2b | `POST /redfish/v1/Systems/1/Memory` per module | — (new) |
| 2c | `POST /redfish/v1/Systems/1/Storage/1/Drives` per drive | BlkInfoMirrorDxe |
| 2d | `PATCH`+`GET /redfish/v1/Chassis/1/Thermal` (thermal telemetry + fan steering, then every 10 s) | — (new) |
| 3 | `GET /redfish/v1/Systems/1` → apply one-time boot override | EEPROM BootNext/BootOrder writes |

The override is acknowledged (`BootSourceOverrideEnabled: "Disabled"`)
**before** booting the target, so it stays genuinely one-shot.

### Thermal / fan steering (step 2d)

The firmware PATCHes a Thermal-schema body — SoC temperature (from the
BCM2712 AVS monitor) plus the active cooler's commanded duty and level:

```json
{"Temperatures": [{"MemberId": "SoC", "ReadingCelsius": 47.3}],
 "Fans": [{"MemberId": "ActiveCooler", "Reading": 49, "ReadingUnits": "Percent",
           "Oem": {"PiBmc": {"Level": 2, "MaxLevel": 4, "OverrideActive": false}}}]}
```

and reads the resource back for steering: an integer
`Oem.PiBmc.FanOverrideLevel` (0..4) in the BMC's representation pins the
fan to that level through `RPI_FAN_PROTOCOL` (ActiveCoolerDxe); removing
the property (or any non-integer value) releases it back to the FanPolicy
variable / automatic loop. Both legs repeat every 10 s for as long as the
firmware phase lasts (BDS wait, Setup, the shell) and stop themselves the
first time both fail. Fail-open: a BMC without the Chassis handler just
404s and the host keeps regulating its own fan.

The *persistent* fan policy is additionally exposed as BIOS attributes:
FanConfigDxe's Setup questions carry `x-UEFI-redfish-Bios.v1_0_9` labels
(`FanConfigDxeMap.uni`), so the edk2-redfish-client Bios feature driver
publishes `FanMode` (`Automatic`/`FixedSpeed`/`CustomTripPoints`),
`FanFixedLevel` (`Level0`..`Level4`) and `FanTrip1C`..`FanTrip4C` under
`/redfish/v1/Systems/1/Bios` with their allowable values in the
BiosAttributeRegistry. A BMC write through that path lands in the same
FanPolicy variable the Setup page edits and ActiveCoolerDxe re-reads
every second — use it for durable policy, and the Thermal override for
live steering.

## Lifecycle: everything stops at ReadyToBoot

The whole exchange is a BDS-time one-shot. Two crashes taught us the stack
must be treated as *finished* once boot begins:

* Same-boot `EfiBootManagerBoot()` from the config-handler callback
  use-after-freed in-flight discovery state (fixed: override always stages
  `BootNext` + cold reset; recipe patch 0101 also drops the pointless IPv6
  discovery leg).
* The BMC gadget re-enumerating while the user sat in Setup tore the USB
  network stack down; the disconnect cascade swept every config handler's
  `Stop()`, and the RedfishClientPkg feature drivers then ran
  `RedfishCleanupService` over the already-freed HTTP instance
  (`Http->Configure` through `0xAFAFAFAFAFAFAFAF`). Fixed by recipe patch
  0102: `RedfishConfigHandlerDriver` stops all handlers at ReadyToBoot while
  the transport is alive and latches so discovery/config never restarts
  (Setup runs after ReadyToBoot, so the hot-unplug window is covered).
  Patch 0103 additionally keeps USB NICs out of BDS boot-option enumeration
  entirely, so Boot Manager refreshes in Setup stop poking the gadget link.

Consequence: if the gadget enumerates so late that discovery would finish
after ReadyToBoot, that boot simply skips the sync (fail-open) and the next
reboot retries.

## Deployment contract (both sides MUST agree)

The firmware side is compiled from the recipe knobs below; the BMC must be
configured to match or discovery never happens.

| Thing | Default | Firmware side | BMC side (nanokvm) |
|---|---|---|---|
| Link subnet | `169.254.0.0/16` | type 42 record (PCDs) | ncm `usb0` = `169.254.10.1/16` (pkg/config default) |
| Host (Pi) IP | `169.254.10.2` | static, from type 42 | its RHI DHCP lease for the same address |
| Service (BMC) IP | `169.254.10.1` | type 42 record | `usb0` address |
| Service port | `80` (HTTP, no TLS) | `PcdRedfishServicePort` | Redfish HTTP listener |
| **Gadget MAC** | `da:c0:ff:ee:10:02` | `RPI5_REDFISH_MAC` → type 42 **and** REST EX device path | ncm function `host_addr` must be **fixed** to this |
| Credentials | `admin`/`admin` | `RPI5_REDFISH_USER`/`_PASSWORD` | its HTTP Basic account |
| Service UUID | none | type 42 field zeroed | nanokvm publishes none — nothing to match |

## BMC-side work this depends on (nanokvm-app)

1. **Fixed `host_addr`** on the ncm function. `ensureEthernetFunc()` currently
   lets the kernel randomize both MACs; `RedfishDiscoverDxe` byte-matches the
   type 42 MAC against the NIC, so a random MAC means discovery never starts.
   Write `functions/ncm.usb0/host_addr` (and ideally `dev_addr`) before
   binding, JetKVM-style (`deriveGadgetMAC`) or from config.
2. **Ethernet mode `ncm`** enabled in the gadget config.
3. `PATCH /Systems/1` currently only consumes the `Boot` object; the identity
   fields (`BiosVersion`, `SerialNumber`, `UUID`, `BootProgress`) are accepted
   and dropped. Storing them replaces what the SMBIOS EEPROM mirror provided.
4. `POST` handlers for `/Systems/1/Memory` and `/Systems/1/Storage/1/Drives`
   (today they 404; the firmware logs and moves on). Storing the drive POSTs
   replaces the blkinfo EEPROM region as `storage.go`'s source.
5. `PATCH`/`GET` handlers for `/Chassis/1/Thermal` (today they 404). Store
   the firmware's Temperatures/Fans arrays for the UI, and serve
   `Oem.PiBmc.FanOverrideLevel` (integer 0–4, or absent for "no opinion")
   to steer the host fan during the firmware phase.

u-boot (nanokvm-build's image) still uses the I2C EEPROM contract — the BMC
keeps the emulated 24c256 for it; only this UEFI firmware moved off it.

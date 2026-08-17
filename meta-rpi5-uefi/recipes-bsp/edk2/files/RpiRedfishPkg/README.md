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
| 3 | `GET /redfish/v1/Systems/1` → apply one-time boot override | EEPROM BootNext/BootOrder writes |

The override is acknowledged (`BootSourceOverrideEnabled: "Disabled"`)
**before** booting the target, so it stays genuinely one-shot.

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

u-boot (nanokvm-build's image) still uses the I2C EEPROM contract — the BMC
keeps the emulated 24c256 for it; only this UEFI firmware moved off it.

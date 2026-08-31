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
| 2d | `PATCH`+`GET /redfish/v1/Chassis/1/Thermal` (thermal telemetry + fan steering, once per boot) | — (new) |
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
variable / automatic loop.

Both legs run **once**, as part of the host-interface exchange. There is no
periodic refresh: the exchange is synchronous HTTP, and a timer driving it
would have to run at TPL_CALLBACK, where a blocking request starves BDS at
TPL_APPLICATION -- a 10 s cadence measurably stalled the boot. The BMC gets
one reading and one chance to steer per boot; anything finer belongs to the
OS, which owns the thermal loop from ExitBootServices onwards.

Fail-open: a BMC without the Chassis handler just 404s and the host keeps
regulating its own fan.

## BIOS attributes

Every configurable Setup question on the platform is exposed at
`/redfish/v1/Systems/1/Bios`, with allowable values, defaults and menu
paths in the BiosAttributeRegistry. The mechanism is one `*Map.uni` file
per formset: a `#string` in the `x-UEFI-redfish-Bios.v1_1_0` language whose
value is the attribute's schema path. `RedfishPlatformConfigDxe` harvests
those from the HII database and the Bios feature driver publishes them.
A question with no such string is invisible to Redfish — there is no
fallback to the prompt text or the question ID.

| Formset | Map file | Attributes |
|---|---|---|
| FanConfigDxe | `FanConfigDxeMap.uni` | `FanMode`, `FanFixedLevel`, `FanTrip1C`..`FanTrip4C` |
| RpiPlatformDxe | `RpiPlatformDxeHiiMap.uni` | `SystemTableMode`, `AcpiSdCompatMode`, `AcpiSdLimitUhs`, `AcpiPcieEcamCompatMode`, `AcpiPcie32BitBarSpaceSizeMB`, `Pcie1Enabled`, `Pcie1MaxLinkSpeed` |
| BootloaderConfigDxe | `BootloaderConfigDxeMap.uni` | `BlBootOrder`, `BlBootUart`, `BlPowerOffOnHalt`, `BlWakeOnGpio`, `BlPsuMaxCurrent` |
| MemoryAttributeManagerDxe | `MemoryAttributeManagerDxeHiiMap.uni` | `MemoryAttributeProtocol` |
| SecureBootToggleDxe | `SecureBootToggleDxeMap.uni` | `SecureBoot` (only with `RPI5_SECURE_BOOT=1`) |

EthConfigDxe's questions are deliberately **not** Bios attributes: they
carry `x-UEFI-redfish-EthernetInterface.v1_8_0` configure language and are
served as the standard `/Systems/1/EthernetInterfaces/{id}` resource by
`RedfishEthernetInterfaceDxe` + `RedfishEthernetInterfaceCollectionDxe`
(this platform's own feature/collection drivers - upstream ships only the
schema converters), so the BMC manages NIC IPv4 with plain Redfish and no
vendor-specific bridge.

Attribute names share one flat namespace across the whole platform, hence
the per-formset prefixes. Storage is each question's own efivarstore:
HiiDatabase's ConfigRouting reads and writes `EFI_HII_VARSTORE_EFI_VARIABLE`
questions with `gRT->GetVariable`/`SetVariable` directly, so a BMC write
lands in exactly the variable the Setup page edits — no ConfigAccess
needed. Most carry `RESET_REQUIRED` and take effect on the next boot; the
fan policy applies live (ActiveCoolerDxe re-reads it every second).

Three caveats worth knowing:

* EthernetInterface writes (DHCPv4/IPv4StaticAddresses/StaticNameServers
  PATCHed on `/Systems/1/EthernetInterfaces/eth0`) reach the `EthCfg`
  variable; EthConfigDxe applies it to the onboard NIC's `Ip4Config2` on
  the **next** boot, when Ip4Dxe binds the GEM (RESET_REQUIRED - the
  feature core reboots on its own after a consumed change). DHCP off with
  no static address leaves the NIC's own configuration alone, so the
  native IPv4 Setup page still works until the BMC opts in. The BMC's own
  USB NCM link is explicitly excluded from the apply - a PATCH here can
  never cut off the RHI.
* `Bl*` writes reach the `BlCfg` variable, **not** the EEPROM.
  BootloaderConfigDxe re-seeds that variable from the live blconfig every
  boot, and only the interactive "stage update" action in Setup writes the
  SPI flash. Treat them as read-mostly.
* Both platform formsets hide most questions behind `suppressif`, so
  `PcdRedfishPlatformConfigFeatureProperty` is set to `0x03` in
  `RPi5.dsc` (bit 1 harvests suppressed questions, bit 0
  records menu paths). At the default of `0` the BMC would see one
  attribute per formset, appearing and disappearing with unrelated
  settings.

## Lifecycle: everything stops once provisioning is done

The whole exchange is a BDS-time one-shot. Three bugs taught us how narrow
the window is:

* Same-boot `EfiBootManagerBoot()` from the config-handler callback
  use-after-freed in-flight discovery state (fixed: override always stages
  `BootNext` + cold reset; recipe patch 0101 also drops the pointless IPv6
  discovery leg).
* The BMC gadget re-enumerating while the user sat in Setup tore the USB
  network stack down; the disconnect cascade swept every config handler's
  `Stop()`, and the RedfishClientPkg feature drivers then ran
  `RedfishCleanupService` over the already-freed HTTP instance
  (`Http->Configure` through `0xAFAFAFAFAFAFAFAF`). Fixed by recipe patch
  0102, which stops all handlers while the transport is alive and latches
  so discovery/config never restarts. Patch 0103 additionally keeps USB
  NICs out of BDS boot-option enumeration entirely, so Boot Manager
  refreshes in Setup stop poking the gadget link.
* **The first version of that 0102 fix quiesced at ReadyToBoot, and that
  silently disabled the entire client feature layer.** `RedfishFeatureCoreDxe`
  provisions from a ReadyToBoot callback of its own
  (`PcdEdkIIRedfishFeatureDriverStartupEventGuid` defaults to that group),
  and both callbacks sat in the same group at `TPL_CALLBACK` with nothing
  ordering them. The quiesce won every time — the DXE core queues signal
  entries with `InsertHeadList` so the later-created event notifies first,
  and `RedfishConfigHandlerDriver` is dispatched after the feature core
  because of its credential-protocol depex; and even reversed, the feature
  core lowers to `TPL_APPLICATION` before walking the tree, which drains the
  quiesce first anyway. Every feature driver then found
  `RedfishService == NULL` and returned `EFI_NOT_READY` **without logging
  anything**, so the platform published no BIOS attributes, no attribute
  registry and no boot options, while `RpiRedfishSyncDxe` — which runs at
  discovery time, long before any of this — kept working and made the stack
  look healthy. 0102 now quiesces on the feature core's own
  after-provisioning event (`gEfiRedfishClientFeatureAfterProvisioningGuid`,
  detected via its feature protocol), falls back to ReadyToBoot only when
  the client layer is absent, and backstops at ExitBootServices.

Consequence: if the gadget enumerates so late that discovery would finish
after ReadyToBoot, that boot simply skips the sync (fail-open) and the next
reboot retries.

**Rule for anything added here later:** never hang cleanup on ReadyToBoot
while RedfishClientPkg is built in. That is the same event the feature core
provisions on, and losing that race costs you the entire feature layer with
no diagnostic whatsoever.

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

# CDC-EEM migration: design

Status: approved for planning · 2026-09-01

Move the Redfish host interface (RHI) from CDC-NCM to CDC-EEM on both managed
hosts, which requires writing the CDC-EEM class driver that edk2 does not have.

## Motivation

The RHI link is a point-to-point USB gadget between the BMC and the managed
host. NCM was chosen for it, and NCM is the wrong shape for the job:

* It is an aggregation protocol. Its NTB/NDP16 framing exists so many frames
  can share one bulk transfer -- value the RHI never collects, since it carries
  one small HTTP exchange at a time. It cost a real bug (patch 0107, where
  datagrams were spliced into one oversized frame and every frame after the
  first was lost).
* It has a notification interrupt endpoint, which on the RPi5's DWC2 USB-C port
  costs up to 5 ms of TPL_NOTIFY busy-wait per tick (see the DWC2 BDS stall
  analysis).

EEM is the opposite: a single interface, two bulk endpoints, no notification
endpoint, no functional descriptors, and a 2-byte header per frame. It removes
two of the three stacked causes in that stall analysis outright.

## Decisions

| Question | Decision |
|---|---|
| Scope | rpi5-uefi-build, nuc-bios-build, nanokvm-app. **Not** nanokvm-build. |
| Gadget mechanism | configfs `f_eem` / `eem.usb0`, composed like `ncm.usb0` today |
| Replace or add | Add EEM, default to it, keep ECM/NCM/RNDIS built |
| Driver home | New `MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/`, delivered as an edk2 patch per repo |
| Station address | Build-time PCD, defaulted to the existing `da:c0:ff:ee:10:02` |

`nanokvm-build` needs no change: the built BMC kernel already carries
`CONFIG_USB_F_EEM=y` and `CONFIG_USB_CONFIGFS_EEM=y` from the riscv defconfig.
(`nanokvm.cfg`'s `CONFIG_USB_CONFIGFS_ECM=y` is redundant with that default and
is not evidence of what is enabled.)

Legacy `g_ether use_eem=1` was considered and rejected: it binds a UDC by
itself and cannot compose with the existing `mass_storage.disk0` + `hid.GS0` +
`hid.GS1` gadget, so adopting it would cost virtual media and HID.

## Architecture

`NetworkCommonSupported` opens `gEdkIIUsbEthProtocolGuid` and tests nothing
else. A new class driver that produces that protocol is therefore bound by the
existing UNDI with **no change to NetworkCommon**, and inherits both
NetworkCommon patches for free:

* 0100 (sticky media on a point-to-point gadget) -- patches NetworkCommon, so
  it covers EEM without modification.
* 0108 (hold off binding until the platform opens the gate) -- likewise. The
  gate is what keeps the RHI NIC out of `EfiBootManagerConnectAll`.

The driver body is a pure *add-files* hunk, so one identical body applies to
both repos despite their different edk2 SRCREVs (rpi5 pins
`2970e5699ba6267f3384ffab20f96647578aebc8`, nuc pins
`fa41c179db1f9fc21eb425f44b85a16262c806ca`). Only the DSC/FDF wiring differs,
and that differs per repo anyway.

## The wire contract

Verified against the exact `drivers/usb/gadget/function/f_eem.c` the BMC runs,
not from spec prose. The two functions that matter are `eem_wrap` (what the
gadget sends us) and `eem_unwrap` (what the gadget will accept from us).

### Descriptors

`bInterfaceClass = USB_CLASS_COMM` (0x02), `bInterfaceSubClass =
USB_CDC_SUBCLASS_EEM` (0x0C), `bInterfaceProtocol = USB_CDC_PROTO_EEM` (0x07),
`bNumEndpoints = 2`. The descriptor array is *only* the interface and its two
bulk endpoints -- no CDC Header, no Union, no Ethernet Functional Descriptor,
no `iMACAddress`, no interrupt endpoint, no alternate settings.

Consequences: `IsSupportedDevice` is a three-field test; there is no sibling
CDC-Data interface, so no `IsSameDevice` device-path walk and no
`UsbCdcDataHandle`; and the station address must come from somewhere else
(below).

### Header

Every packet on either bulk pipe begins with a 2-byte little-endian header.

* bit 15 `bmType`: 0 = data, 1 = command
* Data: bit 14 `bmCRC` (0 = sentinel, 1 = real CRC32), bits 13:0 = length
* Command: bit 14 reserved (0), bits 13:11 `bmEEMCmd`, bits 10:0 parameter
* A header of `0x0000` is a zero-length packet used as padding

### Transmit (host -> gadget)

Judged by `eem_unwrap`:

* Header: bit15 = 0, bit14 = 0, bits 13:0 = `EthLen + 4`.
* Trailing 4 sentinel bytes written **big-endian**: `DE AD BE EF`
  (`get_unaligned_be32(...) != 0xdeadbeef` -> `goto next`). Getting the byte
  order wrong drops that frame **silently** -- no error is reported anywhere.
* `EthLen + 4` must be `>= ETH_HLEN + ETH_FCS_LEN` = 18. A shorter packet makes
  the gadget `goto error`, which frees the **entire URB** -- one runt frame
  destroys every other frame bundled with it. The driver enforces this.

### Receive (gadget -> host)

Produced by `eem_wrap`:

* The header length **includes** the 4-byte CRC field: `eem_wrap` re-reads
  `skb->len` *after* `skb_put(skb, 4)` appends the sentinel. The Ethernet frame
  is therefore `len - 4` bytes.
* The gadget appends a bare `0x0000` header whenever
  `(len + EEM_HLEN + ETH_FCS_LEN) % maxpacket == 0`, purely to force a short
  packet. It is padding and must be skipped, not treated as an error or as a
  hard end-of-buffer.
* Command packets may appear in the stream and must be skipped.

## Receive buffering

`NetworkCommon` takes `*PacketLength` at face value and hands the result
straight to SNP: the contract is **one Ethernet frame per call**. This is the
same contract patch 0107 had to teach NCM after it spliced datagrams together.

`UsbEthEemReceive` buffers one bulk transfer and walks it with a byte cursor:

```
RxBuffer / RxLength / RxOffset
if RxOffset >= RxLength: one bulk IN into RxBuffer, reset cursor
loop at cursor:
  header == 0x0000   -> advance 2, continue                (pad)
  header & BIT(15)   -> advance 2 + (header & 0x7FF), continue   (command)
  otherwise          -> copy (len - 4) out, advance 2 + len, return  (data)
exhausted with no frame -> EFI_NOT_READY
```

A byte cursor is chosen deliberately over NCM's `TotalDatagram`/`NowDatagram`
index pair. That pair is what produced the underflow-to-255 bug patch 0107 had
to guard against; a cursor cannot represent that state at all.

`RxBuffer` is `USB_EEM_MAX_BULK_SIZE` = 0x1000 (4096). One maximum VLAN-tagged
Ethernet frame is 1522 (NCM's own `USB_ETHERNET_FRAME_SIZE`, 0x5F2), plus 2
header and 4 CRC bytes, plus a possible 2-byte pad = 1530. 4096 leaves room for
a bundle without being large enough to matter, and the cursor handles bundling
correctly regardless -- the gadget's short-packet padding means transfers
normally carry a single frame.

## Station address

EEM has no `iMACAddress`, and Linux's own EEM host driver lets usbnet invent a
random MAC. The RHI is discovered *by* MAC -- `RedfishDiscoverDxe` rejects an
interface whose station address is not the expected one -- so a random address
is not viable.

The driver reports a build-time PCD. Because the driver lives in
`MdeModulePkg`, the PCD is declared in `MdeModulePkg.dec` as
`gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress`, a 6-byte `VOID*`
defaulting to all zeroes, and each platform DSC sets it:

* **rpi5** from `RPI5_REDFISH_MAC`, the recipe variable that already feeds the
  discovery MAC and the RestEx device path. `rpi5-uefi-firmware.bb` already
  `bbfatal`s on a value that is not 6 octets, so the build-time guard exists.
* **nuc** from the value already carried as
  `gNucRedfishPkgTokenSpaceGuid.PcdNucRedfishEcmMac`
  (`{0xDA, 0xC0, 0xFF, 0xEE, 0x10, 0x02}` in `NucRedfishPkg.dec`). Nothing new
  is invented; the literal already exists beside the RestEx device path it has
  to match. An equivalent build-time guard is added there.

**An all-zero PCD makes `Supported` decline to bind.** A NIC with an invalid
station address would fail discovery anyway, and declining is a diagnosable
failure rather than a mystery MAC. This is a deliberate call: it means an
upstream integrator who builds the driver without setting the PCD gets no
interface, which the platform recipes' existing MAC validation prevents here.

There is no information loss in any of this: the firmware already hardcodes and
requires that exact value, so the descriptor channel EEM lacks was never
carrying decision-making information.

The gadget does not care what MAC the host presents. `host_addr` in u_ether
only ever fed ECM's `iMACAddress` string; frame delivery is by ordinary ARP
resolution against whatever address the host NIC comes up with.

Rejected alternatives: publishing the MAC in the gadget's `iSerialNumber`
(preserves "the BMC decides" but is a bespoke non-standard contract on both
sides); deriving a locally-administered address from VID:PID + serial (most
standards-faithful, but it would not equal the value discovery expects, forcing
both the PCD and the BMC constant to change).

## Error handling

* Malformed framing truncates the buffer and returns `EFI_NOT_READY`, not a
  device error: a bad bundle costs one poll, never the link.
* `len > remaining` is malformed.
* `EFI_TIMEOUT` from an empty bulk IN is normal polling, reported as
  `EFI_NOT_READY`.
* Bulk timeout stays at 1 ms, matching ECM and NCM. EEM's win over NCM here is
  structural (no interrupt endpoint, no NTB parsing), not a tuning change.

## Per-repo changes

### rpi5-uefi-build

* New `0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch`
  (`UsbCdcEem.c/.h`, `UsbEemFunction.c`, `ComponentName.c`, `UsbCdcEem.inf`,
  plus the MAC PCD declaration).
* One line each in `usbnet-dsc-snippet.inc` and `usbnet-fdf-snippet.fdf.inc`.
* `edk2_git.bb`: SRC_URI entry and the ordering note in the header comment.
* `rpi5-uefi-firmware.bb`: feed `PcdUsbCdcEemMacAddress` from
  `RPI5_REDFISH_MAC`; refresh the comments that describe the link as NCM.

### nuc-bios-build

* The same driver patch, renumbered for that recipe's series.
* Extend patch 0030's `UefiPayloadPkg.dsc` / `.fdf` hunks.
* Add the component to `NucRedfishPkg/NucRedfish.dsc`, and set
  `PcdUsbCdcEemMacAddress` there from the same literal as `PcdNucRedfishEcmMac`.
* Add a `bcfg driver add` line to `deploy/install-drivers.nsh`. Entries are
  sequentially numbered, so everything after the insertion point renumbers.

### nanokvm-app

* `eemFuncName = "eem.usb0"`, `EthernetEEM = "eem"`.
* Accept `eem` in `SetEthernet`; add the `ethernetFuncName` case.
* Config default flips to `"eem"`, with a migration for configs holding `"ncm"`.
* The endpoint-budget table in `serialconsole.go` already carries `eem: 1`.
* A test for the new mode alongside the existing NCM `ethernet_attr_test.go`.
  `f_eem` is u_ether-based, so `eem.usb0` exposes the same `dev_addr` /
  `host_addr` / `qmult` / `ifname` attributes the NCM tests exercise.

## Unchanged

The MAC-matched discovery contract, `RpiRedfishSyncDxe`,
`RpiRedfishHostInterfaceLib`, SMBIOS type 42, the credential library, patches
0100/0102/0103/0105/0108/0109, and the BMC's on/off UI toggle
(`Network: st.Ethernet != EthernetOff` stays a boolean).

## Verification

Neither firmware repo has a host-side unit harness for EDK2 code, so:

1. Both firmwares build green.
2. `nanokvm-app`: `go test -race -count=1 ./...` including the new EEM test.
3. On hardware: the NIC enumerates with the expected station address,
   `RedfishDiscoverDxe` accepts the interface, and a Redfish sync completes.

Two build-level traps to respect: edk2 patch bodies carry **CRLF** line endings
and must be written in bytes mode, never with a text editor tool; and RELEASE
builds strip `DEBUG()`, so driver logging is not available as a validation
channel in a default build.

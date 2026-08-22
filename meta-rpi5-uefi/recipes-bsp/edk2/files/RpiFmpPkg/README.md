# RpiFmpPkg — capsule updates for the Pi 5's own firmware

Firmware Management Protocol, ESRT, and capsule application for this platform's
UEFI firmware, so the board can be updated without anyone touching the SD card,
and so the version it runs is visible to `fwupdmgr`, to Windows Update, and to
the BMC's Redfish `SoftwareInventory`.

Off by default. See [Turning it on](#turning-it-on) — it needs key material this
layer will not invent for you.

## The shape of the problem

The Pi 5 has no firmware flash of its own. The VPU bootloader reads the UEFI
firmware out of a file on the FAT boot partition, so "update the firmware" here
means "rewrite that file". Which is the same file `VarBlockServiceDxe` writes
EFI variables into, because they are the same object:

```
0x000000 ┌──────────────────────────────┐
         │ TF-A BL31 + UEFI             │  ← what a capsule replaces
0x3b0000 ├──────────────────────────────┤  PcdNvStorageVariableBase
         │ NV variable store            │
0x3be000 │ event log                    │  ← what a capsule must NOT touch
0x3bf000 │ FTW working                  │
0x3c0000 │ FTW spare                    │
0x3d0000 └──────────────────────────────┘
```

That layout is the single thing `Rpi5FmpDeviceLib` exists to respect. A capsule
that wrote the whole file would take the boot entries, the enrolled Secure Boot
keys and every BIOS setting with it — silently, and visibly only on the next
boot. So `FmpDeviceGetSize()` reports `0x3b0000` rather than the file size, and
`FmpDeviceSetImage()` writes exactly that much and stops.

A payload longer than `0x3b0000` is accepted, because a whole FD is what the
build produces and what an operator will have to hand. It is only accepted after
confirming the bytes at `0x3b0000` really are an NV firmware volume header
(`gEfiSystemNvDataFvGuid`) — the same identity check `VarBlockServiceDxe` makes
before it writes variables there, and the same one used to pick the file out of
whatever volumes are attached. A payload built for a different flash layout
cannot be written at an offset that means something else here.

## What runs

| Component | Role |
|---|---|
| `Rpi5FmpDeviceLib` | this package — finds the firmware file, validates a payload, writes it |
| `FmpDxe` | produces `EFI_FIRMWARE_MANAGEMENT_PROTOCOL`, authenticates payloads |
| `EsrtFmpDxe` | turns FMP instances into the ESRT the OS reads |
| `CapsuleRuntimeDxe` | `UpdateCapsule()` — already built, stages the capsule |
| `DxeCapsuleLibFmp` | applies it; `ProcessCapsules()` is already called from `PlatformBm` |

Only the first two are new. `CapsuleRuntimeDxe` and both `ProcessCapsules()`
calls have been in this platform all along — with `CapsuleLib` mapped to the
Null instance, which is why `UpdateCapsule()` has always returned success and
then quietly dropped the image. Patch 0018 is what changes that.

## Capsules must be signed

There is no way around this and it is worth being explicit, because the failure
is silent. `FmpDxe`'s only `FmpAuthenticationLib` instances are `Pkcs7` and
`Rsa2048Sha256` — there is no Null — and it authenticates every payload against
the certificates in `PcdFmpDevicePkcs7CertBufferXdr` before the device library
is ever called. An empty certificate buffer is therefore not "no verification",
it is "nothing can ever be applied": the loop over candidate keys simply has no
candidates.

That is a safe failure but an opaque one, so the recipe refuses to build with
`RPI5_FMP=1` and no `RPI5_FMP_CERT` rather than shipping firmware whose ESRT
advertises an update path that cannot work.

Worth being clear-eyed about what the signature buys on this board: anyone who
can write the SD card can replace the firmware without a capsule at all. Capsule
signing protects the *remote* path — an update arriving over Redfish, or through
`fwupd` on a running OS — not the card in someone's hand.

## Turning it on

```sh
openssl req -x509 -newkey rsa:2048 -keyout capsule.key -outform DER \
        -out capsule.cer -days 3650 -nodes -subj "/CN=<your org> capsule/"
```

Keep `capsule.key` somewhere real; `capsule.cer` is what the firmware embeds.
Then build with:

```sh
RPI5_FMP=1 RPI5_FMP_CERT=/path/to/capsule.cer kas build kas.yml
```

Build a signed capsule from a firmware image with EDK2's own tool:

```sh
python3 BaseTools/Scripts/GenerateCapsule.py -e \
  --guid a3f8e2d1-5c47-4b96-8f0a-6d21b7e4c358 \
  --fw-version 202603 --lsv 0 \
  --signer-private-cert capsule.pem --other-public-cert ... \
  -o RPi5Firmware.cap RPI_EFI.fd
```

The GUID is `PcdFmpDeviceImageTypeIdGuid` from `RpiFmp.dsc.inc`. It names *this*
firmware on *this* board with *this* flash layout; change it and every capsule
built before stops applying.

## Versions

Two different things, deliberately kept in step by the recipe:

- `RPI5_FW_VERSION` → `PcdFirmwareVersionString` → SMBIOS type 0, the UEFI
  "Firmware Version" display, Redfish `BiosVersion`, and FMP's `VersionString`.
  Human-readable, e.g. `202602+git`.
- `RPI5_FMP_VERSION` → `PcdRpi5FirmwareVersion` → the integer ESRT publishes and
  `FmpDxe` compares for anti-rollback. Derived from `PV`'s leading numeric part.

The integer **must only ever increase**. `FmpDxe` refuses an image whose version
is below the running one, so a number that goes backwards makes the board
permanently unupdatable by capsule — recoverable only by rewriting the card.

`PcdFmpDeviceBuildTimeLowestSupportedVersion` is 0, which accepts any version on
a first deployment. Raise it once a release exists that must never be downgraded
past; note it is a build-time floor, because `Rpi5FmpDeviceLib` reports no
device-side value of its own (there is nowhere on this platform to keep one that
a downgrade could not also clear).

## What this does not do

- **No A/B slots, no rollback on failure.** The write is in place. A power cut
  partway through leaves a card that will not boot, recoverable by rewriting it
  from another machine. The NV region surviving means settings and keys come
  back with it; the firmware does not.
- **No runtime (OS-visible) capsule application.** `UpdateCapsule()` stages;
  application happens at the next boot, in BDS, which is where a filesystem
  exists to write to. This is why `IMAGE_ATTRIBUTE_RESET_REQUIRED` is set.
- **No dependency expressions.** `FmpDependencyCheckLibNull` — there is one
  updatable device here and nothing to order against.

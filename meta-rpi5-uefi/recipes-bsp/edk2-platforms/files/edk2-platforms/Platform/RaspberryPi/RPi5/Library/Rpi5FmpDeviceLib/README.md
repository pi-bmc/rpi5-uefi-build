# RpiFmpPkg — capsule updates for the Pi 5's own firmware

Firmware Management Protocol, ESRT, and capsule application for this platform's
UEFI firmware, so the board can be updated without anyone touching the SD card,
and so the version it runs is visible to `fwupdmgr`, to Windows Update, and to
the BMC's Redfish `SoftwareInventory`.

Off by default; `RPI5_FMP=1` is the whole of turning it on, and the build then
generates its own signing keypair unless given one. Read
[Turning it on](#turning-it-on) before shipping anything built that way — a
generated key is convenient, not managed.

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

That is a safe failure but an opaque one, so the recipe never produces firmware
without a certificate in that buffer: it uses the one you configured, or
generates a keypair and uses that, or stops the build. What it will not do is
vendor a certificate whose private key nobody holds, which would look like a
working update path and be nothing of the sort.

Worth being clear-eyed about what the signature buys on this board: anyone who
can write the SD card can replace the firmware without a capsule at all. Capsule
signing protects the *remote* path — an update arriving over Redfish, or through
`fwupd` on a running OS — not the card in someone's hand.

## Turning it on

```sh
RPI5_FMP=1 kas build kas.yml
```

That is the whole configuration. With no key material configured, `do_compile`
generates a self-signed capsule signing keypair under `RPI5_FMP_KEYDIR`
(default `${TOPDIR}/fmp-keys`), embeds its certificate in the firmware, and
`do_deploy` emits a signed `RPi5Firmware.cap` next to `RPI_EFI.fd`:

```
fmp-keys/capsule.key   private key, mode 0600
fmp-keys/capsule.crt   certificate, PEM
fmp-keys/capsule.cer   the same certificate as DER, what the firmware embeds
fmp-keys/capsule.pem   key + certificate, what signs capsules
```

**That keypair is an identity with a long tail.** Every board flashed with the
resulting firmware will accept updates signed by it and nothing else, forever,
or until it is reflashed by hand. It is generated once and never regenerated
over an existing file — but it lives unencrypted in the build directory, so a
`rm -rf build/` makes a new identity that no fielded board trusts. Back it up,
or better, replace it with key material you actually manage:

| | |
|---|---|
| `RPI5_FMP_CERT` | DER certificate the firmware embeds |
| `RPI5_FMP_KEY` | PEM holding the signing key **and** its certificate |

Set both and the build signs with them, after checking they are one pair — a
mismatch is fatal, because a capsule signed by the wrong half of a matched set
builds and deploys perfectly and then fails authentication on the board with
nobody watching. Set `RPI5_FMP_CERT` **alone** when the private key lives in an
HSM or another machine: the firmware is built, the capsule is skipped with a
warning, and you sign one offline.

### Signing offline

```sh
PYTHONPATH=<edk2>/BaseTools/Source/Python \
python3 <edk2>/BaseTools/Source/Python/Capsule/GenerateCapsule.py -e \
  --guid a3f8e2d1-5c47-4b96-8f0a-6d21b7e4c358 \
  --fw-version 202602 --lsv 0 \
  --signer-private-cert capsule.pem \
  --other-public-cert   capsule.crt \
  --trusted-public-cert capsule.crt \
  -o RPi5Firmware.cap RPI_EFI.fd
```

Four things that are easy to get wrong, all of which the recipe handles:

- The script is under `BaseTools/Source/Python/Capsule/`, not
  `BaseTools/Scripts/`, and it imports from `BaseTools/Source/Python`, so it
  needs `PYTHONPATH` set (or run it through
  `BaseTools/BinWrappers/PosixLike/GenerateCapsule`).
- All three OpenSSL arguments are mandatory, even for a self-signed
  certificate that is its own chain and its own trust anchor.
- `--signer-private-cert` must hold the key **and** the certificate in one
  file. Signing shells out to `openssl smime -sign -signer <file>` with no
  `-inkey`, so a bare private key is not enough — and a passphrase-encrypted
  key cannot be used at all, because there is nothing to answer the prompt.
- The GUID is `PcdFmpDeviceImageTypeIdGuid` from `RpiFmp.dsc.inc`. It names
  *this* firmware on *this* board with *this* flash layout; change it and every
  capsule built before stops applying. The recipe reads it back out of that
  file rather than repeating the literal, so the two cannot drift.

## Applying one

`RPI5_FMP_CAPSULE_FLAGS` is empty, so the capsule carries no
`PersistAcrossReset` flag, and that is not an oversight:

> `CapsuleRuntimeDxe` returns `EFI_UNSUPPORTED` for a persist-across-reset
> capsule unless `PcdSupportUpdateCapsuleReset` is `TRUE`. RPi5 neither sets
> that PCD nor builds the `CapsulePei`/Capsule-on-Disk machinery that would
> coalesce a staged capsule after the reset.

A capsule with no flags is applied *immediately*, inside the `UpdateCapsule()`
call itself, by a caller running under boot services. So the paths that work
here are a UEFI-Shell `CapsuleApp.efi` (not currently built into the FD) or a
DXE driver calling `UpdateCapsule()` — and the path that does **not** work is
`fwupd` from a running Linux, which delivers capsules the persist-across-reset
way. `RpiRedfishSyncDxe` reports the running version to the BMC today; it does
not yet pull and apply.

## Versions

Two different things, deliberately kept in step by the recipe:

- `RPI5_FW_VERSION` → `PcdFirmwareVersionString` → SMBIOS type 0, the UEFI
  "Firmware Version" display, Redfish `BiosVersion`, and FMP's `VersionString`.
  Human-readable, e.g. `202602+git`.
- `RPI5_FMP_VERSION` → `PcdRpi5FirmwareVersion` (what `FmpDeviceGetVersion`
  reports, and what ESRT publishes) **and** the capsule's `--fw-version`, so a
  capsule always declares the version of the image inside it. Derived from
  `PV`'s leading numeric part.

The integer should only ever increase, but nothing about the *running* version
enforces that: `FmpDxe` compares an incoming image against the lowest supported
version and never against what is currently installed. Anti-rollback here is
`RPI5_FMP_LSV`, and it is a one-way door:

> On a successful apply, `FmpDxe` copies the payload's LSV into a variable, and
> from then on takes the maximum of that, the device library's value, and
> `PcdFmpDeviceBuildTimeLowestSupportedVersion`. Ship one capsule with an LSV
> above a release you may need to return to and that release is unreachable by
> capsule for the life of the variable store.

So `RPI5_FMP_LSV` stays 0 until you mean to burn a downgrade bridge.
`PcdFmpDeviceBuildTimeLowestSupportedVersion` is 0 for the same reason, and is
a build-time floor because `Rpi5FmpDeviceLib` reports no device-side value of
its own — there is nowhere on this platform to keep one that a downgrade could
not also clear.

## What this does not do

- **No A/B slots, no rollback on failure.** The write is in place. A power cut
  partway through leaves a card that will not boot, recoverable by rewriting it
  from another machine. The NV region surviving means settings and keys come
  back with it; the firmware does not.
- **No runtime (OS-visible) capsule application, and no staging.** The
  capsule is applied in the boot-services call that delivers it, by a caller
  that already has a filesystem to write to. Nothing survives a reset to be
  picked up later — see [Applying one](#applying-one).
- **No dependency expressions.** `FmpDependencyCheckLibNull` — there is one
  updatable device here and nothing to order against.

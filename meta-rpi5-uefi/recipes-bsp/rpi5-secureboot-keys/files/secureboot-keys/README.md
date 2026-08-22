# Secure Boot default keys

Embedded into the firmware volume as `PKDefault` / `KEKDefault` / `dbDefault`
when `RPI5_SECURE_BOOT=1`, via `ArmPlatformPkg/SecureBootDefaultKeys.fdf.inc`
and `SecureBootDefaultKeysDxe`.

These are **defaults**, not the live keys. Nothing is enforced until they are
enrolled — see "Enrolling" below.

## Format is not negotiable

Each file is wired into the FDF as a `SECTION RAW`, and
`SecureBootVariableProvisionLib`'s `SecureBootFetchData()` runs
`RsaGetPublicKeyFromX509()` over every section it finds. So every file here
must be a **DER-encoded X.509 certificate with an RSA key**. PEM will not
work, and neither will an `EFI_SIGNATURE_LIST` (`.esl`) — the library builds
the signature lists itself, wrapping each cert with `gEfiCertX509Guid`.

`do_compile` runs `openssl x509 -inform DER` over every one of them before
passing them to the build, because the failure mode otherwise is silent:
a bad file produces firmware whose key defaults simply never load.

That also rules out a conventional `dbx`: the UEFI revocation list is a set
of SHA-256 image hashes, not certificates, so it cannot be provisioned
through this path. `DBX_DEFAULT_FILE*` is deliberately left unset. Apply
revocations at runtime with a signed `dbx` update instead.

## What ships here, and what is fetched

This directory holds **only the Platform Key certificate**. Microsoft's CAs
are not vendored — the recipe fetches them through the bitbake fetcher, so
they are visible in `SRC_URI`, cached in `DL_DIR`, mirrorable, and pinned by
checksum like every other upstream artifact.

| File | Role | Subject | SHA-256 |
| --- | --- | --- | --- |
| `PkDefault.der` | PK | `CN=pi-bmc RPi5 Platform Key, O=pi-bmc, C=US` | `5d5a64db59df073a37848adaa1433e8acbdbbd80c3deef5a0012213d4207970c` |

Fetched into `${WORKDIR}/secureboot-certs/` at build time:

| Downloaded as | Role | Subject | SHA-256 (= fingerprint) |
| --- | --- | --- | --- |
| `MicCorKEKCA2011.crt` | KEK | `Microsoft Corporation KEK CA 2011` | `a1117f516a32cefcba3f2d1ace10a87972fd6bbe8fe0d0b996e09e65d802a503` |
| `MicCorKEK2KCA2023.crt` | KEK | `Microsoft Corporation KEK 2K CA 2023` | `3cd3f0309edae228767a976dd40d9f4affc4fbd5218f2e8cc3c9dd97e8ac6f9d` |
| `MicWinProPCA2011.crt` | db | `Microsoft Windows Production PCA 2011` | `e8e95f0733a55e8bad7be0a1413ee23c51fcea64b3c8fa6a786935fddcc71961` |
| `MicCorUEFCA2011.crt` | db | `Microsoft Corporation UEFI CA 2011` | `48e99b991f57fc52f76149599bff0a58c47154229b9f8d603ac40d3500248507` |
| `WindowsUEFICA2023.crt` | db | `Windows UEFI CA 2023` | `076f1fea90ac29155ebf77c17682f75f1fdd1be196da302dc8461e350a9ae330` |
| `MicrosoftUEFICA2023.crt` | db | `Microsoft UEFI CA 2023` | `f6124e34125bee3fe6d79a574eaa7b91c0e7bd9d929c1a321178efd611dad901` |

Because a DER file is exactly the bytes a certificate fingerprint is taken
over, **each `SRC_URI[…sha256sum]` is that certificate's SHA-256
fingerprint**. Pinning the download therefore pins the certificate identity:
if Microsoft ever serves different bytes at those URLs the build stops. Do
not "fix" such a failure by pasting in a new checksum — check the new file's
subject and fingerprint first.

Two notes on the URLs, both learned the hard way:

* The recipe uses the direct `www.microsoft.com/pkiops/certs/` paths rather
  than the `go.microsoft.com/fwlink` redirects those are usually documented
  as. bitbake's `decodeurl()`/`encodeurl()` round trip percent-escapes a
  query string, so `?linkid=2239775` becomes `%3Flinkid%3D2239775` and the
  fetch fails. The `%20` in the 2023 filenames survives that round trip
  fine.
* The fwlink IDs, for provenance: 321185, 321192, 321194 (2011 generation);
  2239775, 2239776, 2239872 (2023).

The two `*UEFI CA*` certificates are the **third-party** CAs — they are what
signs shim, and therefore what lets Linux distributions boot. Drop them from
`db` if you only ever intend to boot Windows.

Building with `RPI5_SECURE_BOOT_DEFAULT_KEYS=0` removes these from `SRC_URI`
entirely, so an offline build that does not want Secure Boot defaults needs
no network access for them.

## Why both 2011 and 2023

Microsoft rotated its Secure Boot CAs in 2023, and the 2011 generation is
expiring right now:

* `Microsoft Corporation KEK CA 2011` — expired 2026-06-24
* `Microsoft Corporation UEFI CA 2011` — expired 2026-06-27
* `Microsoft Windows Production PCA 2011` — expires 2026-10-19

Both generations are shipped on purpose. UEFI image verification does not
check certificate validity dates — there is no trustworthy clock that early
in boot, and `db` entries are trust anchors rather than a chain to validate —
so the expired 2011 certificates keep working for the large body of binaries
signed under them. New binaries are signed under the 2023 CAs. Shipping only
one generation breaks half the world either way.

## Enrolling

`SecureBootDefaultKeysDxe` populates the `*Default` variables on every boot.
It does **not** enroll them, so a freshly flashed board comes up in Setup
Mode with Secure Boot inactive. To go live:

**From Setup:** Device Manager → Secure Boot Configuration → *Reset Secure
Boot Keys*. That is `KEY_SECURE_BOOT_RESET_TO_DEFAULT`, which runs
`KeyEnrollReset()` — it clears any existing keys, enrolls db/KEK/PK from the
defaults, and returns the platform to Standard mode. The platform moves to
User Mode and `SecureBootToggleDxe`'s checkbox (and the `SecureBoot` Redfish
BIOS attribute) starts having an effect.

This board has BMC KVM, so "use Setup" is not a real constraint here.

`EnrollFromDefaultKeysApp` does the same thing non-interactively and is now
in the firmware volume (upstream builds it but places it in no FV, so the
binary went nowhere). Note that being in an FV does not make it launchable
on its own — the Shell does not map firmware volumes as a filesystem, so it
needs a boot option carrying a `MEDIA_FW_VOL_FILEPATH` device path for
`FILE_GUID 6F18CB2F-1293-4BC1-ABB8-35F84C71812E`, or a
`PlatformBootManagerLib` hook. It is included so that becomes possible.

To go back, use *Reset to Setup Mode* on the same page.

## The PK private key is not in this repository

`PkDefault.der` is only the public certificate. The matching private key was
generated at `../../../../../secureboot-pk/pk.key` — outside the git work
tree, deliberately, and it is **not backed up anywhere**.

Whoever holds that key controls Secure Boot policy on this board: it is what
signs updates to KEK, db and dbx once the platform is in User Mode, and what
signs a replacement PK. Move it to real secret storage and keep it. Losing it
does not brick anything, but it does mean the only way to change keys again is
physically resetting the board to Setup Mode from the firmware UI.

Regenerating it is a one-liner if you would rather use your own:

```sh
openssl req -newkey rsa:2048 -nodes -keyout pk.key -new -x509 -sha256 \
  -days 7300 -subj "/CN=your platform key/O=your org/C=US" -out pk.pem
openssl x509 -in pk.pem -outform DER -out PkDefault.der
```

Replace `PkDefault.der` here, rebuild, and re-enroll from Setup.

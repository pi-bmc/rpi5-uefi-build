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

That also rules out a conventional `dbx`: the UEFI revocation list is a set
of SHA-256 image hashes, not certificates, so it cannot be provisioned
through this path. `DBX_DEFAULT_FILE*` is deliberately left unset. Apply
revocations at runtime with a signed `dbx` update instead.

## Contents

| File | Role | Subject | SHA-256 fingerprint |
| --- | --- | --- | --- |
| `PkDefault.der` | PK | `CN=pi-bmc RPi5 Platform Key, O=pi-bmc, C=US` | `5D:5A:64:DB:59:DF:07:3A:37:84:8A:DA:A1:43:3E:8A:CB:DB:BD:80:C3:DE:EF:5A:00:12:21:3D:42:07:97:0C` |
| `MicCorKEKCA2011.crt` | KEK | `Microsoft Corporation KEK CA 2011` | `A1:11:7F:51:6A:32:CE:FC:BA:3F:2D:1A:CE:10:A8:79:72:FD:6B:BE:8F:E0:D0:B9:96:E0:9E:65:D8:02:A5:03` |
| `MicCorKEK2KCA2023.crt` | KEK | `Microsoft Corporation KEK 2K CA 2023` | `3C:D3:F0:30:9E:DA:E2:28:76:7A:97:6D:D4:0D:9F:4A:FF:C4:FB:D5:21:8F:2E:8C:C3:C9:DD:97:E8:AC:6F:9D` |
| `MicWinProPCA2011.crt` | db | `Microsoft Windows Production PCA 2011` | `E8:E9:5F:07:33:A5:5E:8B:AD:7B:E0:A1:41:3E:E2:3C:51:FC:EA:64:B3:C8:FA:6A:78:69:35:FD:DC:C7:19:61` |
| `MicCorUEFCA2011.crt` | db | `Microsoft Corporation UEFI CA 2011` | `48:E9:9B:99:1F:57:FC:52:F7:61:49:59:9B:FF:0A:58:C4:71:54:22:9B:9F:8D:60:3A:C4:0D:35:00:24:85:07` |
| `WindowsUEFICA2023.crt` | db | `Windows UEFI CA 2023` | `07:6F:1F:EA:90:AC:29:15:5E:BF:77:C1:76:82:F7:5F:1F:DD:1B:E1:96:DA:30:2D:C8:46:1E:35:0A:9A:E3:30` |
| `MicrosoftUEFICA2023.crt` | db | `Microsoft UEFI CA 2023` | `F6:12:4E:34:12:5B:EE:3F:E6:D7:9A:57:4E:AA:7B:91:C0:E7:BD:9D:92:9C:1A:32:11:78:EF:D6:11:DA:D9:01` |

The Microsoft certificates were fetched from Microsoft's official
`go.microsoft.com/fwlink` redirects to `www.microsoft.com/pkiops/certs/`.
Verify any replacement against the fingerprints above before trusting it.

The two `*UEFI CA*` certificates are the **third-party** CAs — they are what
signs shim, and therefore what lets Linux distributions boot. Drop them from
`db` if you only ever intend to boot Windows.

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

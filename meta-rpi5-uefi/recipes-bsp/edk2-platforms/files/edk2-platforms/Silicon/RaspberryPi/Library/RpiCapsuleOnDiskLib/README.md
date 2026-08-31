# RpiCapsuleOnDiskLib — capsule-on-disk for a platform with no PEI

The boot-time half of firmware updates: a NULL library linked into `BdsDxe`
that, at ReadyToBoot, scans every attached volume's `\EFI\UpdateCapsule` drop
box (UEFI 2.10 §8.5.5) and applies the staged FMP capsules synchronously
through `gRT->UpdateCapsule()`. It is what makes a capsule the BMC staged on
its USB mass-storage LUN — or one an OS dropped on the boot ESP — apply during
the *next ordinary boot*, with no boot override and no one booting the capsule
volume by hand.

## Why upstream Capsule-on-Disk is not used

NVIDIA's Jetson platforms (see their `CapsuleUpdateJetson.md`) get this
delivery path from stock EDK2: `PcdCapsuleOnDiskSupport`, `CoDRelocateCapsule`,
and a reset, after which either `CapsulePei` coalesces an in-RAM capsule or
`CapsuleOnDiskLoadPei` reloads a relocated one. Every branch of that machinery
runs in PEI and depends on state surviving a reset. This platform boots through
PrePi — there is no PEI phase — and deliberately supports no persist-across-reset
capsules (`Rpi5FmpDeviceLib`'s README explains why the capsule is flagless).

A flagless capsule needs none of it: it is applied *inside* the
`UpdateCapsule()` call, by a caller running under boot services with
filesystems at hand. So the scan is the processing, and
`PcdCapsuleOnDiskSupport` must stay `FALSE` — setting it would arm `BdsEntry`'s
relocate-and-reset path, which on this platform reboots into nothing that will
ever pick the capsule up.

## What one boot does

1. **Advertise** `EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED` in
   `OsIndicationsSupported` (BdsDxe recomputed the variable earlier from that
   same PCD, so the bit is OR'd back in here). `fwupd`'s capsule-on-disk mode
   keys off it; our capsules are flagless, which is exactly what this scanner
   applies.
2. **Connect USB mass storage, and only that**: bind `UsbBusDxe` to every
   running host controller (non-recursive), then recursively connect only
   interfaces of class 0x08. The BMC gadget's CDC-NCM interface stays exactly
   as the Redfish stack left it — no connect-all, which on this platform can
   stall on NCM.
3. **Scan** every `SimpleFileSystem` volume for a non-empty
   `\EFI\UpdateCapsule`. The common boot ends here, silently, having spent a
   few directory opens.
4. **Gate on the write target**: if no connected volume carries
   `armstub8-2712.bin` / `RPI_EFI.FD`, leave everything staged and say so — a
   split like "booted the capsule volume minimally, firmware file on an
   unconnected SD card" defers to `Rpi5CapsuleApp`, whose one-shot context can
   afford connect-all.
5. **Apply** each capsule with the same checks and contract as
   `Rpi5CapsuleApp`: size and header sanity, explicit rejection of
   `PersistAcrossReset`, then a flagless `UpdateCapsule()` under FmpDxe's
   PKCS#7 and LSV gates. Applied capsules are deleted (the BMC's
   applied-vs-pending signal); failures stay put with `LastAttemptStatus`
   telling the next inventory report why.
6. **Clear** the file-delivery bit from `OsIndications`, and cold-reset if
   anything was applied.

## Why ReadyToBoot at TPL_CALLBACK

`RpiRedfishSyncDxe` executes BMC boot overrides — stage `BootNext`, cold
reset — from network callbacks at `TPL_CALLBACK`, latched off at ReadyToBoot.
Doing the whole scan-and-write inside a `TPL_CALLBACK` notification means those
callbacks cannot interleave with the in-place FD rewrite, whichever order the
ReadyToBoot notifications run in. That is the same mid-write protection
`Rpi5CapsuleApp` gets from running after the latch, without depending on
registration order. `ConnectController` from a `TPL_CALLBACK` notification
follows `UsbBusDxe`'s own hot-plug enumeration precedent.

## Relationship to Rpi5CapsuleApp

Same contract, different trigger. The app ships as `BOOTAA64.EFI` on the
capsule volume and runs when that volume is *booted* (BMC "Usb" override, boot
menu, removable-media fallback); this library runs on every boot that never
chose it. When the override boot happens, this scanner usually applies the
capsule before the app would even load — and if it cannot reach the firmware
volume (step 4), it stands aside and the app's connect-all finishes the job.

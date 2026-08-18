# Kernel patches to carry in the Talos build

Patches that belong to the OS kernel, not to this firmware. This repo builds
no kernel, so nothing here is applied by any recipe — they live here because
they are the other half of a firmware change, and losing track of that pairing
is how a working board becomes an unexplainable one.

Apply them in your Talos kernel image build (`siderolabs/pkgs`, the `linux`
package), or via the Image Factory's customization if it grows a hook for it.

## 0001-net-macb-bind-and-run-from-an-ACPI-description.patch

Written against **v6.18** — the lineage Talos v1.13.8 ships (`6.18.42-talos`).
Verified to apply cleanly to that source. **Not compile-tested**: no kernel was
built here. Build it before trusting it.

### What it unlocks

A firmware that describes the platform in ACPI does not have to ship a device
tree, and therefore does not have to be reflashed when the OS's kernel changes.
That is the whole reason UEFI+ACPI exists, and this board is one driver away
from it: everything else already works under `SystemTableMode=Acpi` — USB,
storage, PCIe, the RP1 blocks described by patches 0011/0012 in the edk2 recipe
— and the on-board NIC is the only thing that does not come up.

It does not come up because `macb` has no ACPI story at all, in any shipping
kernel. Not a missing quirk: `of_match_node()`, `devm_clk_get()` and
`of_get_phy_mode()` all silently do nothing useful without an `of_node`, and
probe ends at `failed to get pclk`.

### If this lands

`SystemTableMode` can go back to `Acpi` (revert patch 0014 in the edk2 recipe),
and then:

- No device tree is needed anywhere — not embedded in firmware, not on the SD
  card (`talos-boot-dtbs`), not on the ESP (patch 0017).
- One firmware image boots any kernel, indefinitely, with no re-flash.
- The RP1 device-tree churn upstream — `rp1_nexus` in 6.17/6.18, `pci@0,0` /
  `dev@0,0` in 6.19, the built-in overlay gone again in master — stops being
  this project's problem.

Until then, patch 0017 (device tree loaded from the OS's own ESP) is the
decoupling that works without touching the kernel.

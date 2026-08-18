  #
  # Upstream Raspberry Pi 5 device tree, built from the devicetree-rebasing
  # tree (the kernel's own DTs, split out of Linux) by this layer's edk2
  # recipe.
  #
  # FdtDxe hands this to the OS in Device Tree mode instead of the one the
  # VPU bootloader loaded, because the firmware tree describes RP1 the
  # downstream way -- a bare "simple-bus" under the PCIe root port -- which a
  # kernel built against the upstream bindings will not bind: no clocks, no
  # GPIO chip, no on-board NIC. The firmware tree is still loaded and still
  # read, for the runtime values only it carries (MAC, dma-ranges, model,
  # bootloader config placement); FdtDxe's UpdateFdtFromFw() copies those in.
  #
  # RAW rather than a typed section: this is an opaque blob that FdtDxe pulls
  # back out with GetSectionFromAnyFv (EFI_SECTION_RAW).
  #
  FILE FREEFORM = @UPSTREAM_DTB_FILE_GUID@ {
    SECTION RAW = @UPSTREAM_DTB_PATH@
  }

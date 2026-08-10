
  #
  # iPXE UNDI/SNP driver (prebuilt by the ipxe-efi Yocto recipe). RP1's
  # onboard Ethernet has no edk2 driver of its own, so without this PXE has
  # no NIC to bind to. Driven by whatever NIC iPXE recognises: a PCIe card
  # on the M.2/PCIe FPC connector, or a USB Ethernet dongle.
  #
  FILE DRIVER = @IPXE_DRIVER_FILE_GUID@ {
    SECTION PE32 = @IPXE_EFIDRV_PATH@
  }

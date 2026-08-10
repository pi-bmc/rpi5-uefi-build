# Rp1GemDxe component, sed-included into RPi5.dsc's [Components.common] by
# the edk2-rpi5-firmware recipe. The DMA PCDs are scoped here (same values
# NonCoherentIoMmuDxe gets in RPi5.dsc): identity bus mapping, buffers kept
# below 3 GiB where the 32-bit BAR window overlaps DRAM. DmaLib is already
# NonCoherentDmaLib globally in RPi5.dsc.

  Rp1GemPkg/Drivers/Rp1GemDxe/Rp1GemDxe.inf {
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0x0
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xBFFFFFFF
  }

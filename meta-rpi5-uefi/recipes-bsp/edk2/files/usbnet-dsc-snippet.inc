# USB CDC network-class drivers (AMI-donated, already in the pinned edk2
# tree but not wired into the RPi5 platform). Sed-included into RPi5.dsc's
# [Components.common]. Gives the NetworkPkg stack an SNP over a USB
# CDC-ECM/NCM/RNDIS gadget on an RP1 port - the BMC's host-interface link
# rides the ncm.usb0 function (see RpiRedfishPkg); all three class drivers
# are built so the gadget mode stays the BMC's choice.

  MdeModulePkg/Bus/Usb/UsbNetwork/NetworkCommon/NetworkCommon.inf
  MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEcm/UsbCdcEcm.inf
  MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcNcm/UsbCdcNcm.inf
  MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis/UsbRndis.inf

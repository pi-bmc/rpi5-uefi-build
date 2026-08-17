/** @file

  Generic IEEE 802.3 Clause 22 PHY support for the RP1 GEM SNP driver.

  The Raspberry Pi 5 onboard PHY is a Broadcom BCM54213PE at the MDIO
  address given by PcdRp1GemPhyAddress (firmware DTB: ethernet-phy@1),
  driven entirely through the generic IEEE register set. There is no PHY
  reset GPIO on the Pi 5.

  Modeled on BcmGenetDxe's GenericPhy.c:
  Copyright (c) 2020 Jared McNeill. All rights reserved.
  Copyright (c) 2020 Andrey Warkentin <andrey.warkentin@gmail.com>
  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1GemDxe.h"

#include <Library/Rp1GpioLib.h>
#include <Protocol/Rp1Bus.h>

//
// The BCM54213PE's RESET_N pin is wired to RP1 GPIO 32, active-low, with a
// 5 ms assert time (board DT: mdio { reset-gpios = <&rp1_gpio 32
// GPIO_ACTIVE_LOW>; reset-delay-us = <5000>; }). The VPU firmware leaves it
// asserted unless it network-boots, so the PHY tristates MDIO (every read
// returns 0xFFFF) until this pulse - u-boot's macb does the same before
// every bus scan, with a 15 ms post-release settle.
//
#define PHY_RESET_GPIO           32
#define PHY_RESET_ASSERT_US      5000
#define PHY_RESET_SETTLE_US      15000

//
// Basic Mode Control Register
//
#define PHY_BMCR                 0x00
#define  PHY_BMCR_RESET          BIT15
#define  PHY_BMCR_ANE            BIT12
#define  PHY_BMCR_RESTART_AN     BIT9

//
// Basic Mode Status Register
//
#define PHY_BMSR                 0x01
#define  PHY_BMSR_ANEG_COMPLETE  BIT5
#define  PHY_BMSR_LINK_STATUS    BIT2

//
// PHY Identifier registers
//
#define PHY_IDR1                 0x02
#define PHY_IDR2                 0x03

//
// Auto-Negotiation Advertisement Register
//
#define PHY_ANAR                 0x04
#define  PHY_ANAR_100BASETX_FDX  BIT8
#define  PHY_ANAR_100BASETX      BIT7
#define  PHY_ANAR_10BASET_FDX    BIT6
#define  PHY_ANAR_10BASET        BIT5

//
// Auto-Negotiation Link Partner Ability Register
//
#define PHY_ANLPAR               0x05

//
// 1000BASE-T Control Register
//
#define PHY_GBCR                 0x09
#define  PHY_GBCR_1000BASET_FDX  BIT9
#define  PHY_GBCR_1000BASET      BIT8

//
// 1000BASE-T Status Register (link partner abilities at bits 11:10)
//
#define PHY_GBSR                 0x0A

//
// Broadcom BCM54xx vendor registers: the auxiliary control register at
// 0x18 (shadow selected by the low bits, read-select in bits 14:12) and
// the shadow register file behind 0x1c. The Pi 5 wires the BCM54213PE in
// "rgmii-id" mode -- both RGMII clock delays live inside the PHY -- and a
// hardware reset clears them, so they must be re-applied after every
// reset pulse or nothing is received (TX may still work off strap
// defaults, giving one-way traffic).
//
#define PHY_BCM_AUXCTL                  0x18
#define  PHY_BCM_AUXCTL_SHD_MISC        0x0007
#define  PHY_BCM_AUXCTL_MISC_WREN       0x8000
#define  PHY_BCM_AUXCTL_MISC_RGMII_SKEW 0x0100
#define PHY_BCM_SHD                     0x1C
#define  PHY_BCM_SHD_WRITE              0x8000
#define  PHY_BCM_SHD_CLK_CTL            0x03
#define  PHY_BCM_SHD_CLK_CTL_GTXCLK_EN  BIT9
#define PHY_ID1_BCM54XX                 0x600D

#define PHY_RESET_TIMEOUT        500   // x 1 ms

/**
  Detect the PHY: try the configured address first, then scan the whole
  MDIO range as a fallback.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS    PHY found; Gem->PhyAddr is valid.
  @retval EFI_NOT_FOUND  No PHY responded.

**/
/**
  Pulse the PHY's hardware reset line (RP1 GPIO 32, active-low) so it comes
  out of reset and answers MDIO. Best-effort: if the RP1 bus protocol is not
  around the scan still runs (and diagnoses a dead bus).

  @param  Gem[in]  Driver private data.

**/
STATIC
VOID
Rp1GemPhyHwReset (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS            Status;
  RP1_BUS_PROTOCOL      *Rp1Bus;
  EFI_PHYSICAL_ADDRESS  PeripheralBase;

  Status = gBS->LocateProtocol (&gRp1BusProtocolGuid, NULL, (VOID **)&Rp1Bus);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "Rp1GemDxe: no RP1 bus protocol (%r) - skipping PHY reset pulse\n",
      Status
      ));
    return;
  }

  PeripheralBase = Rp1Bus->GetPeripheralBase (Rp1Bus);

  //
  // u-boot ordering: preload the output value, then enable the driver
  // (OE + funcsel RIO), then release after the assert time.
  //
  Rp1GpioWrite (PeripheralBase, PHY_RESET_GPIO, FALSE);
  Rp1GpioSetDirection (PeripheralBase, PHY_RESET_GPIO, TRUE);
  gBS->Stall (PHY_RESET_ASSERT_US);
  Rp1GpioWrite (PeripheralBase, PHY_RESET_GPIO, TRUE);
  gBS->Stall (PHY_RESET_SETTLE_US);
}

STATIC
EFI_STATUS
Rp1GemPhyDetect (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;
  UINT8       PhyAddr;
  UINT8       Try;
  UINT16      Id1;
  UINT16      Id2;

  Rp1GemPhyHwReset (Gem);

  PhyAddr = PcdGet8 (PcdRp1GemPhyAddress) & 0x1F;

  for (Try = 0; Try < 32; Try++) {
    Status = GemMdioRead (Gem, PhyAddr, PHY_IDR1, &Id1);
    if (!EFI_ERROR (Status)) {
      Status = GemMdioRead (Gem, PhyAddr, PHY_IDR2, &Id2);
      //
      // Log raw reads for the configured address: on a dead bus this
      // distinguishes all-ones (nothing driving MDIO) from all-zeroes
      // (transactions complete, data line stuck) at a glance.
      //
      if (Try == 0) {
        DEBUG ((
          DEBUG_INFO,
          "Rp1GemDxe: MDIO probe addr 0x%02x: %r ID 0x%04x:0x%04x\n",
          PhyAddr,
          Status,
          Id1,
          Id2
          ));
      }

      if (!EFI_ERROR (Status) && (Id1 != 0xFFFF) && (Id2 != 0xFFFF) &&
          ((Id1 != 0) || (Id2 != 0)))
      {
        Gem->PhyAddr = PhyAddr;
        DEBUG ((
          DEBUG_INFO,
          "Rp1GemDxe: PHY at address 0x%02x (ID 0x%04x:0x%04x)\n",
          PhyAddr,
          Id1,
          Id2
          ));
        return EFI_SUCCESS;
      }
    }

    PhyAddr = (PhyAddr + 1) & 0x1F;
  }

  //
  // Nothing answered: dump a full diagnostic block in one place so a lossy
  // serial capture still tells the whole story. All-ones IDs = nothing
  // drives MDIO (pad/power/routing); all-zeroes = transactions complete
  // but the data line is stuck low. The USER_IO readback shows whether
  // RP1's GEM even implements the SAMA-style interface mux (reads back
  // the written RGMII value) or ignores it (interface select lives in the
  // eth_cfg wrapper instead).
  //
  DEBUG ((DEBUG_ERROR, "Rp1GemDxe: no PHY detected on MDIO bus\n"));
  DEBUG ((
    DEBUG_ERROR,
    "Rp1GemDxe: NET_CTRL %08x NET_CFG %08x NET_STAT %08x USER_IO %08x\n",
    MmioRead32 ((UINTN)Gem->GemBase + GEM_NET_CTRL),
    MmioRead32 ((UINTN)Gem->GemBase + GEM_NET_CFG),
    MmioRead32 ((UINTN)Gem->GemBase + GEM_NET_STAT),
    MmioRead32 ((UINTN)Gem->GemBase + GEM_USER_IO)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "Rp1GemDxe: eth_cfg +0x00 %08x +0x04 %08x +0x08 %08x +0x0c %08x\n",
    MmioRead32 ((UINTN)Gem->EthCfgBase + 0x0),
    MmioRead32 ((UINTN)Gem->EthCfgBase + 0x4),
    MmioRead32 ((UINTN)Gem->EthCfgBase + 0x8),
    MmioRead32 ((UINTN)Gem->EthCfgBase + 0xc)
    ));

  for (PhyAddr = 0; PhyAddr < 32; PhyAddr += 8) {
    UINT16  Ids[8];
    UINT8   Sub;

    for (Sub = 0; Sub < 8; Sub++) {
      Ids[Sub] = 0xDEAD;
      GemMdioRead (Gem, PhyAddr + Sub, PHY_IDR1, &Ids[Sub]);
    }

    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: ID1[%02x..%02x] %04x %04x %04x %04x %04x %04x %04x %04x\n",
      PhyAddr,
      PhyAddr + 7,
      Ids[0],
      Ids[1],
      Ids[2],
      Ids[3],
      Ids[4],
      Ids[5],
      Ids[6],
      Ids[7]
      ));
  }

  return EFI_NOT_FOUND;
}

/**
  Reset the PHY and wait for the reset bit to self-clear.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS   Reset complete.
  @retval EFI_TIMEOUT   Reset did not complete in time.
  @retval other         MDIO access failure.

**/
EFI_STATUS
Rp1GemPhyReset (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;
  UINTN       Retry;
  UINT16      Data;

  Status = GemMdioWrite (Gem, Gem->PhyAddr, PHY_BMCR, PHY_BMCR_RESET);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = PHY_RESET_TIMEOUT; Retry > 0; Retry--) {
    Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_BMCR, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Data & PHY_BMCR_RESET) == 0) {
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);
  }

  return EFI_TIMEOUT;
}

/**
  Program the "rgmii-id" clock delays into a BCM54xx PHY: RXC-RXD skew via
  the auxiliary-control misc shadow, GTXCLK delay via shadow register 0x03.
  Reading an auxctl shadow needs the register number in both the select
  (bits 2:0) and read-select (bits 14:12) fields; a write carries it in the
  select field alongside the data. No-op for non-Broadcom PHYs.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS  Delays programmed, or PHY is not a BCM54xx.
  @retval other        MDIO access failure.

**/
STATIC
EFI_STATUS
Rp1GemPhyConfigRgmiiDelays (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;
  UINT16      Id1;
  UINT16      Value;

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_IDR1, &Id1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Id1 != PHY_ID1_BCM54XX) {
    return EFI_SUCCESS;
  }

  //
  // RX clock skew (auxctl shadow 7): read-modify-write with the write
  // enable bit set.
  //
  Status = GemMdioWrite (
             Gem,
             Gem->PhyAddr,
             PHY_BCM_AUXCTL,
             (PHY_BCM_AUXCTL_SHD_MISC << 12) | PHY_BCM_AUXCTL_SHD_MISC
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_BCM_AUXCTL, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value |= PHY_BCM_AUXCTL_MISC_WREN | PHY_BCM_AUXCTL_MISC_RGMII_SKEW;
  Status  = GemMdioWrite (
              Gem,
              Gem->PhyAddr,
              PHY_BCM_AUXCTL,
              PHY_BCM_AUXCTL_SHD_MISC | Value
              );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // TX clock delay (shadow 0x03, clock alignment control): the shadow
  // data field is 10 bits, so mask the readback before setting BIT9.
  //
  Status = GemMdioWrite (
             Gem,
             Gem->PhyAddr,
             PHY_BCM_SHD,
             PHY_BCM_SHD_CLK_CTL << 10
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_BCM_SHD, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value = (Value & 0x3FF) | PHY_BCM_SHD_CLK_CTL_GTXCLK_EN;
  Status = GemMdioWrite (
             Gem,
             Gem->PhyAddr,
             PHY_BCM_SHD,
             PHY_BCM_SHD_WRITE | (PHY_BCM_SHD_CLK_CTL << 10) | Value
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read both registers back and log the raw values: MDIO reads are proven
  // (ID scan works) but nothing else in this driver verifies that MDIO
  // WRITES reach the PHY. The skew bit (0x0100) must show in the auxctl
  // readback and BIT9 in the shadow readback, or the writes are being
  // dropped on the wire.
  //
  {
    UINT16  AuxRb;
    UINT16  ShdRb;

    GemMdioWrite (
      Gem,
      Gem->PhyAddr,
      PHY_BCM_AUXCTL,
      (PHY_BCM_AUXCTL_SHD_MISC << 12) | PHY_BCM_AUXCTL_SHD_MISC
      );
    GemMdioRead (Gem, Gem->PhyAddr, PHY_BCM_AUXCTL, &AuxRb);
    GemMdioWrite (Gem, Gem->PhyAddr, PHY_BCM_SHD, PHY_BCM_SHD_CLK_CTL << 10);
    GemMdioRead (Gem, Gem->PhyAddr, PHY_BCM_SHD, &ShdRb);
    DEBUG ((
      DEBUG_ERROR,
      "Rp1GemDxe: RGMII delays readback auxctl 0x%04x (want bit 0x0100) shd3 0x%04x (want bit 0x0200)\n",
      AuxRb,
      ShdRb
      ));
  }

  return EFI_SUCCESS;
}

/**
  Advertise 10/100 (half and full duplex) plus 1000BASE-T full duplex and
  (re)start autonegotiation. 1000BASE-T half duplex is not advertised: the
  GEM MAC does not support half duplex at gigabit speed.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS   Autonegotiation restarted.
  @retval other         MDIO access failure.

**/
STATIC
EFI_STATUS
Rp1GemPhyAutoNegotiate (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;
  UINT16      Anar;
  UINT16      Gbcr;
  UINT16      Bmcr;

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_ANAR, &Anar);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Anar |= PHY_ANAR_100BASETX_FDX |
          PHY_ANAR_100BASETX |
          PHY_ANAR_10BASET_FDX |
          PHY_ANAR_10BASET;
  Status = GemMdioWrite (Gem, Gem->PhyAddr, PHY_ANAR, Anar);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_GBCR, &Gbcr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gbcr |= PHY_GBCR_1000BASET_FDX;
  Gbcr &= (UINT16)~PHY_GBCR_1000BASET;
  Status = GemMdioWrite (Gem, Gem->PhyAddr, PHY_GBCR, Gbcr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_BMCR, &Bmcr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bmcr |= PHY_BMCR_ANE | PHY_BMCR_RESTART_AN;
  return GemMdioWrite (Gem, Gem->PhyAddr, PHY_BMCR, Bmcr);
}

/**
  Detect and reset the PHY, then start autonegotiation. Does not wait for
  the link to come up; link state is tracked by Rp1GemPhyUpdateConfig.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS    PHY initialized, autonegotiation running.
  @retval EFI_NOT_FOUND  No PHY detected.
  @retval EFI_TIMEOUT    PHY reset timed out.
  @retval other          MDIO access failure.

**/
EFI_STATUS
Rp1GemPhyInit (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;

  Gem->LinkUp = FALSE;

  Status = Rp1GemPhyDetect (Gem);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Rp1GemPhyReset (Gem);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The BMCR reset (and the hardware reset pulse before it) clears the
  // vendor RGMII delay configuration; without it RX is dead even though
  // TX works, so re-apply before autonegotiation.
  //
  Status = Rp1GemPhyConfigRgmiiDelays (Gem);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Rp1GemPhyAutoNegotiate (Gem);
}

/**
  Read the negotiated link parameters, preferring the highest common
  denominator: 1000, then 100, then 10 Mbps.

  @param  Gem[in]          Driver private data.
  @param  SpeedMbps[out]   Negotiated speed.
  @param  FullDuplex[out]  Negotiated duplex.

  @retval EFI_SUCCESS   Configuration resolved.
  @retval other         MDIO access failure.

**/
STATIC
EFI_STATUS
Rp1GemPhyGetConfig (
  IN  RP1_GEM_PRIVATE_DATA  *Gem,
  OUT UINTN                 *SpeedMbps,
  OUT BOOLEAN               *FullDuplex
  )
{
  EFI_STATUS  Status;
  UINT16      Gbcr;
  UINT16      Gbsr;
  UINT16      Anar;
  UINT16      Anlpar;
  UINT16      Gb;
  UINT16      An;

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_GBCR, &Gbcr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_GBSR, &Gbsr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_ANAR, &Anar);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_ANLPAR, &Anlpar);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 1000BASE-T partner abilities sit two bits above the corresponding
  // advertisement bits.
  //
  Gb = (UINT16)((Gbsr >> 2) & Gbcr);
  An = (UINT16)(Anlpar & Anar);

  if ((Gb & (PHY_GBCR_1000BASET_FDX | PHY_GBCR_1000BASET)) != 0) {
    *SpeedMbps  = 1000;
    *FullDuplex = (BOOLEAN)((Gb & PHY_GBCR_1000BASET_FDX) != 0);
  } else if ((An & (PHY_ANAR_100BASETX_FDX | PHY_ANAR_100BASETX)) != 0) {
    *SpeedMbps  = 100;
    *FullDuplex = (BOOLEAN)((An & PHY_ANAR_100BASETX_FDX) != 0);
  } else {
    *SpeedMbps  = 10;
    *FullDuplex = (BOOLEAN)((An & PHY_ANAR_10BASET_FDX) != 0);
  }

  return EFI_SUCCESS;
}

/**
  Poll the PHY link state and, on a link-up transition, propagate the
  negotiated speed/duplex into the MAC configuration.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS    Link is up.
  @retval EFI_NOT_READY  Link is down (or autonegotiation incomplete).
  @retval other          MDIO access failure.

**/
EFI_STATUS
Rp1GemPhyUpdateConfig (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;
  UINT16      Bmsr;
  BOOLEAN     LinkUp;
  UINTN       SpeedMbps;
  BOOLEAN     FullDuplex;

  Status = GemMdioRead (Gem, Gem->PhyAddr, PHY_BMSR, &Bmsr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LinkUp = (BOOLEAN)(((Bmsr & PHY_BMSR_LINK_STATUS) != 0) &&
                     ((Bmsr & PHY_BMSR_ANEG_COMPLETE) != 0));

  if (Gem->LinkUp != LinkUp) {
    if (LinkUp) {
      Status = Rp1GemPhyGetConfig (Gem, &SpeedMbps, &FullDuplex);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      GemUpdateLinkSpeed (Gem, SpeedMbps, FullDuplex);
    } else {
      DEBUG ((DEBUG_INFO, "Rp1GemDxe: link is down\n"));
    }

    Gem->LinkUp = LinkUp;
  }

  return LinkUp ? EFI_SUCCESS : EFI_NOT_READY;
}

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

#define PHY_RESET_TIMEOUT        500   // x 1 ms

/**
  Detect the PHY: try the configured address first, then scan the whole
  MDIO range as a fallback.

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS    PHY found; Gem->PhyAddr is valid.
  @retval EFI_NOT_FOUND  No PHY responded.

**/
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

  DEBUG ((DEBUG_ERROR, "Rp1GemDxe: no PHY detected on MDIO bus\n"));
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

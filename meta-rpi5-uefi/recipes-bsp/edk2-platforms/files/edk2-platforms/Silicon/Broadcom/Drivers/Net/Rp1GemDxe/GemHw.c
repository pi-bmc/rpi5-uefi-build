/** @file

  Cadence GEM_GXL hardware access layer for the RP1 GEM SNP driver.

  Initialization order and descriptor handling adapted from FreeBSD
  sys/dev/cadence/if_cgem.c (BSD-2-Clause), with RP1-specific behavior
  (AXI pipeline register, 64-bit descriptors, fixed 200 MHz pclk) taken
  from the verified RP1 u-boot port configuration.

  Copyright (c) 2012-2013 Thomas Skibo. All rights reserved.
  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1GemDxe.h"

#define GEM_MDIO_TIMEOUT_TRIES  400   // x 5 us = 2 ms per MDIO operation

STATIC
UINT32
GemRead32 (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINTN                 Offset
  )
{
  return MmioRead32 ((UINTN)Gem->GemBase + Offset);
}

STATIC
VOID
GemWrite32 (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINTN                 Offset,
  IN UINT32                Value
  )
{
  MmioWrite32 ((UINTN)Gem->GemBase + Offset, Value);
}

/**
  Allocate the descriptor page and the RX/TX packet buffers from uncached
  DMA-able memory (NonCoherentDmaLib; identity bus mapping, kept below
  3 GiB by the module-scoped PcdDmaDeviceLimit).

  @param  Gem[in]  Driver private data.

  @retval EFI_SUCCESS           Buffers allocated.
  @retval EFI_OUT_OF_RESOURCES  Allocation failure.

**/
EFI_STATUS
GemDmaAlloc (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  EFI_STATUS  Status;

  ASSERT (
    (GEM_RX_DESC_COUNT + GEM_TX_DESC_COUNT + 2) * sizeof (GEM_DMA_DESC) <=
    EFI_PAGE_SIZE
    );

  Status = DmaAllocateBuffer (EfiBootServicesData, 1, &Gem->DescBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DmaAllocateBuffer (
             EfiBootServicesData,
             EFI_SIZE_TO_PAGES (GEM_RX_DESC_COUNT * GEM_RX_BUFFER_SIZE),
             (VOID **)&Gem->RxBuffers
             );
  if (EFI_ERROR (Status)) {
    goto FreeDesc;
  }

  Status = DmaAllocateBuffer (
             EfiBootServicesData,
             EFI_SIZE_TO_PAGES (GEM_TX_DESC_COUNT * GEM_TX_BUFFER_SIZE),
             (VOID **)&Gem->TxBuffers
             );
  if (EFI_ERROR (Status)) {
    goto FreeRxBuffers;
  }

  //
  // The shared TBQPH/RBQPH registers constrain descriptors and buffers to a
  // single 4 GiB segment; the platform DMA allocation policy keeps them
  // below 3 GiB. Fail loudly if that ever stops holding.
  //
  if (((UINTN)Gem->DescBuffer >= SIZE_4GB) ||
      ((UINTN)Gem->RxBuffers + GEM_RX_DESC_COUNT * GEM_RX_BUFFER_SIZE > SIZE_4GB) ||
      ((UINTN)Gem->TxBuffers + GEM_TX_DESC_COUNT * GEM_TX_BUFFER_SIZE > SIZE_4GB))
  {
    ASSERT (FALSE);
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeTxBuffers;
  }

  Gem->RxRing     = (GEM_DMA_DESC *)Gem->DescBuffer;
  Gem->TxRing     = Gem->RxRing + GEM_RX_DESC_COUNT;
  Gem->NullRxDesc = Gem->TxRing + GEM_TX_DESC_COUNT;
  Gem->NullTxDesc = Gem->NullRxDesc + 1;

  return EFI_SUCCESS;

FreeTxBuffers:
  DmaFreeBuffer (
    EFI_SIZE_TO_PAGES (GEM_TX_DESC_COUNT * GEM_TX_BUFFER_SIZE),
    Gem->TxBuffers
    );
  Gem->TxBuffers = NULL;

FreeRxBuffers:
  DmaFreeBuffer (
    EFI_SIZE_TO_PAGES (GEM_RX_DESC_COUNT * GEM_RX_BUFFER_SIZE),
    Gem->RxBuffers
    );
  Gem->RxBuffers = NULL;

FreeDesc:
  DmaFreeBuffer (1, Gem->DescBuffer);
  Gem->DescBuffer = NULL;

  return Status;
}

/**
  Release the DMA descriptor page and packet buffers.

  @param  Gem[in]  Driver private data.

**/
VOID
GemDmaFree (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  if (Gem->TxBuffers != NULL) {
    DmaFreeBuffer (
      EFI_SIZE_TO_PAGES (GEM_TX_DESC_COUNT * GEM_TX_BUFFER_SIZE),
      Gem->TxBuffers
      );
    Gem->TxBuffers = NULL;
  }

  if (Gem->RxBuffers != NULL) {
    DmaFreeBuffer (
      EFI_SIZE_TO_PAGES (GEM_RX_DESC_COUNT * GEM_RX_BUFFER_SIZE),
      Gem->RxBuffers
      );
    Gem->RxBuffers = NULL;
  }

  if (Gem->DescBuffer != NULL) {
    DmaFreeBuffer (1, Gem->DescBuffer);
    Gem->DescBuffer = NULL;
  }

  Gem->RxRing     = NULL;
  Gem->TxRing     = NULL;
  Gem->NullRxDesc = NULL;
  Gem->NullTxDesc = NULL;
}

/**
  Compose the static part of NET_CFG: data bus width (probed from the design
  configuration register, as if_cgem does) and the MDC divisor.

  pclk (clk_sys) is fixed at 200 MHz by the RP1 firmware clock setup, so the
  MDC divisor is hardcoded: 200 MHz / 96 = ~2.08 MHz, within the 2.5 MHz
  IEEE 802.3 limit.

  @param  Gem[in]  Driver private data.

  @return NET_CFG base value (bus width + MDC divisor bits only).

**/
STATIC
UINT32
GemNetCfgBase (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  UINT32  NetCfg;

  switch (GemRead32 (Gem, GEM_DESIGN_CFG1) & GEM_DESIGN_CFG1_DBW_MASK) {
    case GEM_DESIGN_CFG1_DBW_64:
      NetCfg = GEM_NET_CFG_DBUS_WIDTH_64;
      break;
    case GEM_DESIGN_CFG1_DBW_128:
      NetCfg = GEM_NET_CFG_DBUS_WIDTH_128;
      break;
    default:
      NetCfg = GEM_NET_CFG_DBUS_WIDTH_32;
      break;
  }

  return NetCfg | GEM_NET_CFG_MDC_CLK_DIV_96;
}

/**
  Quiesce and reset the MAC register state: disable RX/TX and interrupts,
  clear statistics and status, zero the filters and ring pointers, then
  re-enable only the MDIO management port.

  @param  Gem[in]  Driver private data.

**/
VOID
GemMacReset (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  GemWrite32 (Gem, GEM_NET_CTRL, 0);
  GemWrite32 (Gem, GEM_NET_CFG, GemNetCfgBase (Gem));
  //
  // Select the RGMII PHY interface. RP1 integrates the GEM with the
  // SAMA-style USER_IO interface mux (the Zynq reference this driver's
  // register layer derives from muxes via SLCR instead and never touches
  // this register); u-boot's working rp1-gem port programs it before any
  // PHY access.
  //
  GemWrite32 (Gem, GEM_USER_IO, GEM_USER_IO_RGMII);
  GemWrite32 (Gem, GEM_NET_CTRL, GEM_NET_CTRL_CLR_STAT_REGS);
  GemWrite32 (Gem, GEM_TX_STAT, GEM_TX_STAT_ALL);
  GemWrite32 (Gem, GEM_RX_STAT, GEM_RX_STAT_ALL);
  GemWrite32 (Gem, GEM_INTR_DIS, GEM_INTR_ALL);
  GemWrite32 (Gem, GEM_HASH_BOT, 0);
  GemWrite32 (Gem, GEM_HASH_TOP, 0);
  GemWrite32 (Gem, GEM_TX_QBAR, 0);
  GemWrite32 (Gem, GEM_RX_QBAR, 0);
  GemWrite32 (Gem, GEM_TX_QBAR_HI, 0);
  GemWrite32 (Gem, GEM_RX_QBAR_HI, 0);

  //
  // Keep the management (MDIO) port running even while the interface is
  // down so the PHY stays reachable.
  //
  GemWrite32 (Gem, GEM_NET_CTRL, GEM_NET_CTRL_MGMT_PORT_EN);
}

/**
  Point every RX descriptor at its buffer, hand ownership back to the
  hardware and reset the software head.

  Also used to recover from a receive stall: re-enabling RX restarts the
  engine from RX_QBAR, so the ring has to be re-armed from descriptor 0 and
  RxHead resynchronised to match, or software and hardware immediately
  disagree about where the head is.

  @param  Gem[in]  Driver private data.

**/
STATIC
VOID
GemArmRxRing (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  BufAddr;

  for (Index = 0; Index < GEM_RX_DESC_COUNT; Index++) {
    BufAddr = (EFI_PHYSICAL_ADDRESS)(UINTN)
              (Gem->RxBuffers + Index * GEM_RX_BUFFER_SIZE);
    //
    // Bits [1:0] of the RX address word carry WRAP/OWN, so buffers must be
    // 4-byte aligned (page-aligned in practice).
    //
    ASSERT ((BufAddr & (GEM_RXDESC_ADDR_OWN | GEM_RXDESC_ADDR_WRAP)) == 0);

    Gem->RxRing[Index].Addr = (UINT32)BufAddr |
                              ((Index == GEM_RX_DESC_COUNT - 1) ? GEM_RXDESC_ADDR_WRAP : 0);
    Gem->RxRing[Index].Ctrl   = 0;
    Gem->RxRing[Index].AddrHi = (UINT32)(BufAddr >> 32);
    Gem->RxRing[Index].Unused = 0;
  }

  Gem->RxHead = 0;
}

/**
  Initialize the RX/TX descriptor rings and the null descriptors used to
  park the hardware priority queues that cannot be disabled.

  @param  Gem[in]  Driver private data.

**/
STATIC
VOID
GemInitRings (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  BufAddr;
  UINT32                QueueMask;
  UINTN                 Queue;

  GemArmRxRing (Gem);

  for (Index = 0; Index < GEM_TX_DESC_COUNT; Index++) {
    BufAddr = (EFI_PHYSICAL_ADDRESS)(UINTN)
              (Gem->TxBuffers + Index * GEM_TX_BUFFER_SIZE);

    Gem->TxRing[Index].Addr = (UINT32)BufAddr;
    Gem->TxRing[Index].Ctrl = GEM_TXDESC_CTRL_USED |
                              ((Index == GEM_TX_DESC_COUNT - 1) ? GEM_TXDESC_CTRL_WRAP : 0);
    Gem->TxRing[Index].AddrHi = (UINT32)(BufAddr >> 32);
    Gem->TxRing[Index].Unused = 0;

    Gem->TxUserBuffer[Index] = NULL;
  }

  //
  // Empty stub descriptors for the priority queues (cannot be disabled;
  // see if_cgem cgem_null_qs / u-boot gmac_init_multi_queues).
  //
  Gem->NullRxDesc->Addr   = GEM_RXDESC_ADDR_OWN | GEM_RXDESC_ADDR_WRAP;
  Gem->NullRxDesc->Ctrl   = 0;
  Gem->NullRxDesc->AddrHi = 0;
  Gem->NullRxDesc->Unused = 0;

  Gem->NullTxDesc->Addr   = 0;
  Gem->NullTxDesc->Ctrl   = GEM_TXDESC_CTRL_USED | GEM_TXDESC_CTRL_WRAP;
  Gem->NullTxDesc->AddrHi = 0;
  Gem->NullTxDesc->Unused = 0;

  MemoryFence ();

  GemWrite32 (Gem, GEM_RX_QBAR, (UINT32)(UINTN)Gem->RxRing);
  GemWrite32 (Gem, GEM_RX_QBAR_HI, (UINT32)((UINT64)(UINTN)Gem->RxRing >> 32));
  GemWrite32 (Gem, GEM_TX_QBAR, (UINT32)(UINTN)Gem->TxRing);
  GemWrite32 (Gem, GEM_TX_QBAR_HI, (UINT32)((UINT64)(UINTN)Gem->TxRing >> 32));

  QueueMask = GemRead32 (Gem, GEM_DESIGN_CFG6) & GEM_DESIGN_CFG6_PRIO_Q_MASK;
  for (Queue = 1; Queue < 16; Queue++) {
    if ((QueueMask & (1U << Queue)) == 0) {
      continue;
    }

    GemWrite32 (Gem, GEM_RX_QN_BAR (Queue), (UINT32)(UINTN)Gem->NullRxDesc);
    GemWrite32 (Gem, GEM_TX_QN_BAR (Queue), (UINT32)(UINTN)Gem->NullTxDesc);
  }

  Gem->TxProd = 0;
  Gem->TxCons = 0;
}

/**
  Full MAC bring-up (after GemMacReset): AXI pipeline depth, NET_CFG,
  DMA_CFG, descriptor rings, station address and receive filters.

  RX/TX remain disabled; call GemMacEnableTxRx afterwards.

  The eth_cfg wrapper block (second MMIO resource) is intentionally left at
  its firmware defaults: the RP1 boot firmware programs the RGMII interface
  mode and TX clocking, and the verified RP1 u-boot port never touches the
  wrapper either.

  @param  Gem[in]  Driver private data.

**/
VOID
GemMacConfigure (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  UINT32  Amp;
  UINT32  NetCfg;
  UINT32  DmaCfg;

  //
  // AXI Max Pipeline: AR2R = 8, AW2W = 8, AW2B_FILL = 1. Required for
  // throughput on RP1's PCIe-attached AXI fabric (u-boot gem_init_axi).
  //
  Amp  = GemRead32 (Gem, GEM_AMP);
  Amp &= ~(GEM_AMP_AR2R_MAX_PIPE_MASK | GEM_AMP_AW2W_MAX_PIPE_MASK);
  Amp |= (8U << GEM_AMP_AR2R_MAX_PIPE_SHIFT) |
         (8U << GEM_AMP_AW2W_MAX_PIPE_SHIFT) |
         GEM_AMP_AW2B_FILL;
  GemWrite32 (Gem, GEM_AMP, Amp);

  //
  // NET_CFG: strip FCS on RX, allow 1536-byte frames, pause enabled, RX
  // checksum offload off for simplicity. Start out at gigabit full-duplex
  // (the validated path); corrected when autonegotiation resolves.
  //
  NetCfg = GemNetCfgBase (Gem) |
           GEM_NET_CFG_FCS_REMOVE |
           GEM_NET_CFG_PAUSE_EN |
           GEM_NET_CFG_1536RXEN |
           GEM_NET_CFG_FULL_DUPLEX |
           GEM_NET_CFG_GIGE_EN;
  GemWrite32 (Gem, GEM_NET_CFG, NetCfg);

  //
  // DMA_CFG: 64-bit descriptors (TBQPH/RBQPH in use), AXI burst 16,
  // RX buffer size in 64-byte units (2048 -> 0x20), full packet buffer
  // memory, discard RX packets when no descriptor is available.
  //
  DmaCfg = GEM_DMA_CFG_ADDR_BUS_64 |
           GEM_DMA_CFG_DISC_WHEN_NO_AHB |
           GEM_DMA_CFG_RX_BUF_SIZE (GEM_RX_BUFFER_SIZE) |
           GEM_DMA_CFG_RX_PKTBUF_FULL |
           GEM_DMA_CFG_TX_PKTBUF_FULL |
           GEM_DMA_CFG_AHB_BURST_LEN_16;
  GemWrite32 (Gem, GEM_DMA_CFG, DmaCfg);

  GemInitRings (Gem);

  GemSetMacAddress (Gem, &Gem->SnpMode.CurrentAddress);
  GemSetReceiveFilters (Gem, Gem->SnpMode.ReceiveFilterSetting);
}

/**
  Enable the receiver and transmitter (management port stays enabled).

  @param  Gem[in]  Driver private data.

**/
VOID
GemMacEnableTxRx (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  MmioOr32 (
    (UINTN)Gem->GemBase + GEM_NET_CTRL,
    GEM_NET_CTRL_RX_EN | GEM_NET_CTRL_TX_EN
    );
}

/**
  Disable the receiver and transmitter (management port stays enabled).

  @param  Gem[in]  Driver private data.

**/
VOID
GemMacDisableTxRx (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  MmioAnd32 (
    (UINTN)Gem->GemBase + GEM_NET_CTRL,
    ~(UINT32)(GEM_NET_CTRL_RX_EN | GEM_NET_CTRL_TX_EN)
    );
}

/**
  Program specific address register 1 with the station MAC address.
  Writing the bottom half disables the match until the top half is written.

  @param  Gem[in]         Driver private data.
  @param  MacAddress[in]  Station address to program.

**/
VOID
GemSetMacAddress (
  IN RP1_GEM_PRIVATE_DATA   *Gem,
  IN CONST EFI_MAC_ADDRESS  *MacAddress
  )
{
  GemWrite32 (
    Gem,
    GEM_SPEC_ADDR_LOW (0),
    ((UINT32)MacAddress->Addr[3] << 24) |
    ((UINT32)MacAddress->Addr[2] << 16) |
    ((UINT32)MacAddress->Addr[1] << 8) |
    (UINT32)MacAddress->Addr[0]
    );
  GemWrite32 (
    Gem,
    GEM_SPEC_ADDR_HI (0),
    ((UINT32)MacAddress->Addr[5] << 8) |
    (UINT32)MacAddress->Addr[4]
    );
}

/**
  Apply SNP receive filter settings to NET_CFG, the hash registers and
  specific address register 1.

  - PROMISCUOUS               -> COPY_ALL
  - BROADCAST disabled        -> NO_BCAST
  - MULTICAST / PROMISCUOUS_MULTICAST -> multicast hash enabled with an
    all-ones hash (accept every multicast frame)
  - UNICAST                   -> station address match via SA1 (disabled by
    clearing SA1 when unicast reception is off)

  @param  Gem[in]      Driver private data.
  @param  Filters[in]  EFI_SIMPLE_NETWORK_RECEIVE_* bitmask.

**/
VOID
GemSetReceiveFilters (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINT32                Filters
  )
{
  UINT32  NetCfg;
  UINT32  Hash;

  NetCfg  = GemRead32 (Gem, GEM_NET_CFG);
  NetCfg &= ~(GEM_NET_CFG_COPY_ALL |
              GEM_NET_CFG_NO_BCAST |
              GEM_NET_CFG_MULTI_HASH_EN |
              GEM_NET_CFG_UNI_HASH_EN);
  Hash = 0;

  if ((Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) != 0) {
    NetCfg |= GEM_NET_CFG_COPY_ALL;
  } else {
    if ((Filters & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) == 0) {
      NetCfg |= GEM_NET_CFG_NO_BCAST;
    }

    if ((Filters & (EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST |
                    EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)) != 0)
    {
      NetCfg |= GEM_NET_CFG_MULTI_HASH_EN;
      Hash    = 0xFFFFFFFF;
    }
  }

  GemWrite32 (Gem, GEM_HASH_BOT, Hash);
  GemWrite32 (Gem, GEM_HASH_TOP, Hash);
  GemWrite32 (Gem, GEM_NET_CFG, NetCfg);

  if ((Filters & (EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                  EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS)) != 0)
  {
    GemSetMacAddress (Gem, &Gem->SnpMode.CurrentAddress);
  } else {
    //
    // Disable the specific address match: writing the bottom register
    // deactivates the entry until the top register is written again.
    //
    GemWrite32 (Gem, GEM_SPEC_ADDR_LOW (0), 0);
    GemWrite32 (Gem, GEM_SPEC_ADDR_HI (0), 0);
  }
}

/**
  Propagate a resolved PHY link into the MAC speed/duplex configuration.

  @param  Gem[in]         Driver private data.
  @param  SpeedMbps[in]   10, 100 or 1000.
  @param  FullDuplex[in]  TRUE for full duplex.

**/
VOID
GemUpdateLinkSpeed (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINTN                 SpeedMbps,
  IN BOOLEAN               FullDuplex
  )
{
  UINT32  NetCfg;

  NetCfg  = GemRead32 (Gem, GEM_NET_CFG);
  NetCfg &= ~(GEM_NET_CFG_SPEED100 |
              GEM_NET_CFG_GIGE_EN |
              GEM_NET_CFG_FULL_DUPLEX);

  switch (SpeedMbps) {
    case 1000:
      NetCfg |= GEM_NET_CFG_GIGE_EN;
      break;
    case 100:
      NetCfg |= GEM_NET_CFG_SPEED100;
      break;
    default:
      break;
  }

  if (FullDuplex) {
    NetCfg |= GEM_NET_CFG_FULL_DUPLEX;
  }

  GemWrite32 (Gem, GEM_NET_CFG, NetCfg);

  DEBUG ((
    DEBUG_INFO,
    "Rp1GemDxe: link %u Mbps, %a-duplex\n",
    (UINT32)SpeedMbps,
    FullDuplex ? "full" : "half"
    ));
}

/**
  Clause 22 MDIO read through the GEM PHY maintenance register.

  @param  Gem[in]       Driver private data.
  @param  PhyAddr[in]   PHY address (0-31).
  @param  Reg[in]       PHY register (0-31).
  @param  Data[out]     Register value read.

  @retval EFI_SUCCESS   Read completed.
  @retval EFI_TIMEOUT   The management interface did not go idle.

**/
EFI_STATUS
GemMdioRead (
  IN  RP1_GEM_PRIVATE_DATA  *Gem,
  IN  UINT8                 PhyAddr,
  IN  UINT8                 Reg,
  OUT UINT16                *Data
  )
{
  UINTN  Tries;

  MmioOr32 ((UINTN)Gem->GemBase + GEM_NET_CTRL, GEM_NET_CTRL_MGMT_PORT_EN);

  GemWrite32 (
    Gem,
    GEM_PHY_MAINT,
    GEM_PHY_MAINT_CLAUSE_22 |
    GEM_PHY_MAINT_MUST_10 |
    GEM_PHY_MAINT_OP_READ |
    ((UINT32)(PhyAddr & 0x1F) << GEM_PHY_MAINT_PHY_ADDR_SHIFT) |
    ((UINT32)(Reg & 0x1F) << GEM_PHY_MAINT_REG_ADDR_SHIFT)
    );

  for (Tries = 0; Tries < GEM_MDIO_TIMEOUT_TRIES; Tries++) {
    if ((GemRead32 (Gem, GEM_NET_STAT) & GEM_NET_STAT_PHY_MGMT_IDLE) != 0) {
      *Data = (UINT16)(GemRead32 (Gem, GEM_PHY_MAINT) &
                       GEM_PHY_MAINT_DATA_MASK);
      return EFI_SUCCESS;
    }

    gBS->Stall (5);
  }

  DEBUG ((DEBUG_ERROR, "Rp1GemDxe: MDIO read timeout (reg %u)\n", Reg));
  return EFI_TIMEOUT;
}

/**
  Clause 22 MDIO write through the GEM PHY maintenance register.

  @param  Gem[in]       Driver private data.
  @param  PhyAddr[in]   PHY address (0-31).
  @param  Reg[in]       PHY register (0-31).
  @param  Data[in]      Register value to write.

  @retval EFI_SUCCESS   Write completed.
  @retval EFI_TIMEOUT   The management interface did not go idle.

**/
EFI_STATUS
GemMdioWrite (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINT8                 PhyAddr,
  IN UINT8                 Reg,
  IN UINT16                Data
  )
{
  UINTN  Tries;

  MmioOr32 ((UINTN)Gem->GemBase + GEM_NET_CTRL, GEM_NET_CTRL_MGMT_PORT_EN);

  GemWrite32 (
    Gem,
    GEM_PHY_MAINT,
    GEM_PHY_MAINT_CLAUSE_22 |
    GEM_PHY_MAINT_MUST_10 |
    GEM_PHY_MAINT_OP_WRITE |
    ((UINT32)(PhyAddr & 0x1F) << GEM_PHY_MAINT_PHY_ADDR_SHIFT) |
    ((UINT32)(Reg & 0x1F) << GEM_PHY_MAINT_REG_ADDR_SHIFT) |
    Data
    );

  for (Tries = 0; Tries < GEM_MDIO_TIMEOUT_TRIES; Tries++) {
    if ((GemRead32 (Gem, GEM_NET_STAT) & GEM_NET_STAT_PHY_MGMT_IDLE) != 0) {
      return EFI_SUCCESS;
    }

    gBS->Stall (5);
  }

  DEBUG ((DEBUG_ERROR, "Rp1GemDxe: MDIO write timeout (reg %u)\n", Reg));
  return EFI_TIMEOUT;
}

/**
  Check whether a received frame is waiting at the RX ring head.

  @param  Gem[in]  Driver private data.

  @return TRUE if a frame is pending.

**/
BOOLEAN
GemRxPending (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  if (Gem->RxRing == NULL) {
    return FALSE;
  }

  return (Gem->RxRing[Gem->RxHead].Addr & GEM_RXDESC_ADDR_OWN) != 0;
}

/**
  Detect and recover a stalled receive engine.

  The Cadence GEM stops its receive DMA when it runs out of descriptors
  under sustained load, latching RX_STAT.BUF_NOT_AVAIL and then delivering
  nothing further. The documented recovery is to toggle receive enable; see
  Linux macb_main.c, the at91rm9200 manual section 41.3.1 and the Zynq
  manual section 16.7.4.

  Nothing else in the driver inspects RX_STAT while no frame is pending --
  GemReceiveFrame and GemRxPending both test only the descriptor OWN bit --
  so an unrecovered stall is invisible and survives until the part is
  reset. Call this from the polling path, where it runs precisely when
  frames have stopped arriving.

  A momentarily full ring is not a stall: while frames are still queued the
  engine is alive and draining them clears the condition, so acknowledge it
  and leave the hardware alone. Toggling receive enable in that case would
  discard perfectly good queued frames and make burst pressure worse.

  @param  Gem[in]  Driver private data.

**/
VOID
GemRecoverRxIfStalled (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  UINT32  RxStat;
  UINT32  NetCtrl;

  if (Gem->RxRing == NULL) {
    return;
  }

  RxStat = GemRead32 (Gem, GEM_RX_STAT);
  if ((RxStat & (GEM_RX_STAT_BUF_NOT_AVAIL | GEM_RX_STAT_OVERRUN)) == 0) {
    return;
  }

  //
  // Acknowledge (write-1-to-clear) before deciding what to do, so a frame
  // landing during recovery cannot leave the condition latched unnoticed.
  //
  GemWrite32 (
    Gem,
    GEM_RX_STAT,
    GEM_RX_STAT_BUF_NOT_AVAIL | GEM_RX_STAT_OVERRUN
    );

  if (GemRxPending (Gem)) {
    return;
  }

  NetCtrl = GemRead32 (Gem, GEM_NET_CTRL);
  GemWrite32 (Gem, GEM_NET_CTRL, NetCtrl & ~(UINT32)GEM_NET_CTRL_RX_EN);
  MemoryFence ();

  //
  // Re-arm the ring and put the hardware queue pointer back at its base
  // explicitly, rather than relying on the engine to rewind it on re-enable
  // -- Linux macb toggles receive enable without touching either, so the
  // rewind behaviour is not something to bet correctness on. Writing
  // RX_QBAR while receive is disabled is exactly what bring-up does, and it
  // leaves hardware and RxHead provably agreed on descriptor 0. Any frame
  // still sitting in the ring is dropped; the stall has already cost far
  // more than that.
  //
  GemArmRxRing (Gem);
  MemoryFence ();

  GemWrite32 (Gem, GEM_RX_QBAR, (UINT32)(UINTN)Gem->RxRing);
  GemWrite32 (Gem, GEM_RX_QBAR_HI, (UINT32)((UINT64)(UINTN)Gem->RxRing >> 32));
  MemoryFence ();

  GemWrite32 (Gem, GEM_NET_CTRL, NetCtrl | GEM_NET_CTRL_RX_EN);

  Gem->RxStallRecoveries++;

  DEBUG ((
    DEBUG_ERROR,
    "Rp1GemDxe: recovered stalled RX (rxstat 0x%x, recoveries %u)\n",
    RxStat,
    Gem->RxStallRecoveries
    ));
}

/**
  Hand the RX descriptor at Index back to the hardware and advance the
  ring head.

  @param  Gem[in]    Driver private data.
  @param  Index[in]  Descriptor index to recycle.

**/
STATIC
VOID
GemRecycleRxDescriptor (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINTN                 Index
  )
{
  Gem->RxRing[Index].Ctrl = 0;
  MemoryFence ();
  Gem->RxRing[Index].Addr &= ~(UINT32)GEM_RXDESC_ADDR_OWN;
  MemoryFence ();

  Gem->RxHead = (UINT16)((Index + 1) % GEM_RX_DESC_COUNT);
}

/**
  Copy the frame at the RX ring head into Buffer.

  @param  Gem[in]             Driver private data.
  @param  Buffer[out]         Destination for the frame.
  @param  BufferSize[in,out]  On input, the size of Buffer; on output the
                              frame length.

  @retval EFI_SUCCESS           Frame copied out and descriptor recycled.
  @retval EFI_NOT_READY         No frame pending.
  @retval EFI_BUFFER_TOO_SMALL  Frame does not fit; *BufferSize holds the
                                required size and the frame is kept.

**/
EFI_STATUS
GemReceiveFrame (
  IN     RP1_GEM_PRIVATE_DATA  *Gem,
  OUT    VOID                  *Buffer,
  IN OUT UINTN                 *BufferSize
  )
{
  GEM_DMA_DESC  *Desc;
  UINTN         Index;
  UINT32        Ctrl;
  UINTN         Length;

  while (TRUE) {
    Index = Gem->RxHead;
    Desc  = &Gem->RxRing[Index];

    if ((Desc->Addr & GEM_RXDESC_ADDR_OWN) == 0) {
      return EFI_NOT_READY;
    }

    MemoryFence ();
    Ctrl   = Desc->Ctrl;
    Length = Ctrl & GEM_RXDESC_CTRL_LENGTH_MASK;

    //
    // With 2048-byte buffers every valid (<= 1536 byte) frame fits a single
    // descriptor, so both SOF and EOF must be set. Drop anything else.
    //
    if (((Ctrl & (GEM_RXDESC_CTRL_SOF | GEM_RXDESC_CTRL_EOF)) !=
         (GEM_RXDESC_CTRL_SOF | GEM_RXDESC_CTRL_EOF)) ||
        (Length < GEM_ETHER_HEADER_SIZE) ||
        (Length > GEM_RX_BUFFER_SIZE))
    {
      DEBUG ((
        DEBUG_WARN,
        "Rp1GemDxe: dropping bad RX descriptor (ctrl 0x%x)\n",
        Ctrl
        ));
      GemRecycleRxDescriptor (Gem, Index);
      continue;
    }

    if (*BufferSize < Length) {
      *BufferSize = Length;
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (Buffer, Gem->RxBuffers + Index * GEM_RX_BUFFER_SIZE, Length);
    *BufferSize = Length;

    GemRecycleRxDescriptor (Gem, Index);

    //
    // Acknowledge receive status (write-1-to-clear).
    //
    GemWrite32 (
      Gem,
      GEM_RX_STAT,
      GEM_RX_STAT_FRAME_RECD | GEM_RX_STAT_BUF_NOT_AVAIL | GEM_RX_STAT_OVERRUN
      );

    return EFI_SUCCESS;
  }
}

/**
  Queue a frame for transmission: copy it into the per-descriptor bounce
  buffer, publish the descriptor and kick the transmitter.

  @param  Gem[in]         Driver private data.
  @param  UserBuffer[in]  Caller's buffer (returned by GetStatus for
                          recycling once the frame is on the wire).
  @param  Length[in]      Frame length in bytes.

  @retval EFI_SUCCESS    Frame queued.
  @retval EFI_NOT_READY  All TX descriptors are in flight.

**/
EFI_STATUS
GemTransmitFrame (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN VOID                  *UserBuffer,
  IN UINTN                 Length
  )
{
  GEM_DMA_DESC  *Desc;
  UINTN         Index;
  UINT32        Ctrl;

  if ((UINT16)(Gem->TxProd - Gem->TxCons) >= GEM_TX_DESC_COUNT) {
    return EFI_NOT_READY;
  }

  ASSERT (Length <= GEM_TX_BUFFER_SIZE);

  Index = Gem->TxProd % GEM_TX_DESC_COUNT;
  Desc  = &Gem->TxRing[Index];

  CopyMem (Gem->TxBuffers + Index * GEM_TX_BUFFER_SIZE, UserBuffer, Length);

  Ctrl = (UINT32)(Length & GEM_TXDESC_CTRL_LENGTH_MASK) |
         GEM_TXDESC_CTRL_LAST_BUF;
  if (Index == GEM_TX_DESC_COUNT - 1) {
    Ctrl |= GEM_TXDESC_CTRL_WRAP;
  }

  //
  // Publishing the control word clears the USED bit, handing the
  // descriptor to the hardware.
  //
  MemoryFence ();
  Desc->Ctrl = Ctrl;
  MemoryFence ();

  Gem->TxUserBuffer[Index] = UserBuffer;
  Gem->TxProd++;

  MmioOr32 ((UINTN)Gem->GemBase + GEM_NET_CTRL, GEM_NET_CTRL_START_TX);

  return EFI_SUCCESS;
}

/**
  Check whether at least one queued TX frame has completed.

  @param  Gem[in]  Driver private data.

  @return TRUE if a completed TX descriptor is waiting to be reclaimed.

**/
BOOLEAN
GemTxPendingCompletion (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  )
{
  if ((Gem->TxRing == NULL) || (Gem->TxCons == Gem->TxProd)) {
    return FALSE;
  }

  return (Gem->TxRing[Gem->TxCons % GEM_TX_DESC_COUNT].Ctrl &
          GEM_TXDESC_CTRL_USED) != 0;
}

/**
  Reclaim at most one completed TX descriptor, returning the caller buffer
  for SNP GetStatus() recycling.

  @param  Gem[in]     Driver private data.
  @param  TxBuf[out]  Recycled caller buffer, or NULL if none completed.

**/
VOID
GemGetRecycledTxBuffer (
  IN  RP1_GEM_PRIVATE_DATA  *Gem,
  OUT VOID                  **TxBuf
  )
{
  GEM_DMA_DESC  *Desc;
  UINTN         Index;

  *TxBuf = NULL;

  if (Gem->TxCons == Gem->TxProd) {
    return;
  }

  Index = Gem->TxCons % GEM_TX_DESC_COUNT;
  Desc  = &Gem->TxRing[Index];

  if ((Desc->Ctrl & GEM_TXDESC_CTRL_USED) == 0) {
    return;
  }

  if ((Desc->Ctrl & (GEM_TXDESC_CTRL_RETRY_ERR |
                     GEM_TXDESC_CTRL_AHB_ERR |
                     GEM_TXDESC_CTRL_LATE_COLL)) != 0)
  {
    DEBUG ((
      DEBUG_WARN,
      "Rp1GemDxe: TX error on descriptor %u (ctrl 0x%x)\n",
      (UINT32)Index,
      Desc->Ctrl
      ));
  }

  *TxBuf                   = Gem->TxUserBuffer[Index];
  Gem->TxUserBuffer[Index] = NULL;
  Gem->TxCons++;

  //
  // Acknowledge transmit status (write-1-to-clear).
  //
  GemWrite32 (
    Gem,
    GEM_TX_STAT,
    GEM_TX_STAT_COMPLETE | GEM_TX_STAT_UNDERRUN | GEM_TX_STAT_USED_BIT_READ
    );
}

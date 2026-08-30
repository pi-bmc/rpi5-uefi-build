/** @file

  EFI_SIMPLE_NETWORK_PROTOCOL implementation for the RP1 GEM (polled, no
  interrupts). Structure modeled on BcmGenetDxe's SimpleNetwork.c.

  Copyright (c) 2020 Jared McNeill. All rights reserved.
  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1GemDxe.h"

//
// How long Transmit() waits for a link before giving up: the boot flow
// often calls Transmit right after Initialize, while autonegotiation
// is still running (up to ~3 s on 1000BASE-T).
//
#define GEM_LINK_TIMEOUT_MS  5000

/**
  Changes the state of a network interface from "stopped" to "started".

  @param  This[in]  Protocol instance pointer.

  @retval EFI_SUCCESS           The network interface was started.
  @retval EFI_ALREADY_STARTED   The network interface was already started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpStart (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStarted) {
    return EFI_ALREADY_STARTED;
  } else if (Gem->SnpMode.State != EfiSimpleNetworkStopped) {
    return EFI_DEVICE_ERROR;
  }

  Gem->SnpMode.State = EfiSimpleNetworkStarted;

  return EFI_SUCCESS;
}

/**
  Changes the state of a network interface from "started" to "stopped".

  @param  This[in]  Protocol instance pointer.

  @retval EFI_SUCCESS           The network interface was stopped.
  @retval EFI_NOT_STARTED       The network interface was not started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpStop (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  } else if (Gem->SnpMode.State != EfiSimpleNetworkStarted) {
    return EFI_DEVICE_ERROR;
  }

  GemMacDisableTxRx (Gem);

  Gem->SnpMode.State = EfiSimpleNetworkStopped;

  return EFI_SUCCESS;
}

/**
  Resets the network adapter and allocates the transmit and receive
  buffers required by the network interface.

  @param  This[in]               Protocol instance pointer.
  @param  ExtraRxBufferSize[in]  Extra receive buffer request (ignored).
  @param  ExtraTxBufferSize[in]  Extra transmit buffer request (ignored).

  @retval EFI_SUCCESS           The network interface was initialized.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.
  @retval EFI_NOT_FOUND         No PHY was detected.
  @retval EFI_TIMEOUT           PHY reset or MDIO access timed out.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpInitialize (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINTN                        ExtraRxBufferSize  OPTIONAL,
  IN UINTN                        ExtraTxBufferSize  OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  EFI_STATUS            Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  } else if (Gem->SnpMode.State != EfiSimpleNetworkStarted) {
    return EFI_DEVICE_ERROR;
  }

  //
  // GemMacReset leaves only the MDIO management port enabled, so the PHY
  // can be brought up before the receiver/transmitter.
  //
  GemMacReset (Gem);
  GemMacConfigure (Gem);

  Status = Rp1GemPhyInit (Gem);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  GemMacEnableTxRx (Gem);

  Gem->SnpMode.MediaPresent = FALSE;
  Gem->SnpMode.State        = EfiSimpleNetworkInitialized;

  return EFI_SUCCESS;
}

/**
  Resets the network adapter and reinitializes it with the parameters
  provided in the previous call to Initialize().

  @param  This[in]                  Protocol instance pointer.
  @param  ExtendedVerification[in]  Ignored.

  @retval EFI_SUCCESS           The network interface was reset.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpReset (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN BOOLEAN                      ExtendedVerification
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  EFI_STATUS            Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  Status = Rp1GemPhyInit (Gem);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gem->SnpMode.MediaPresent = FALSE;

  return EFI_SUCCESS;
}

/**
  Resets the network adapter and leaves it in a state safe for another
  driver to initialize.

  @param  This[in]  Protocol instance pointer.

  @retval EFI_SUCCESS           The network interface was shut down.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpShutdown (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  GemMacDisableTxRx (Gem);
  GemMacReset (Gem);

  Gem->SnpMode.MediaPresent = FALSE;
  Gem->SnpMode.State        = EfiSimpleNetworkStarted;

  return EFI_SUCCESS;
}

/**
  Manages the receive filters of a network interface.

  @param  This[in]              Protocol instance pointer.
  @param  Enable[in]            Bit mask of receive filters to enable.
  @param  Disable[in]           Bit mask of receive filters to disable.
  @param  ResetMCastFilter[in]  TRUE to reset the multicast filter list.
  @param  MCastFilterCnt[in]    Number of multicast addresses in MCastFilter.
  @param  MCastFilter[in]       New multicast address list.

  @retval EFI_SUCCESS           The receive filters were updated.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER Unsupported filter bits or bad list.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpReceiveFilters (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINT32                       Enable,
  IN UINT32                       Disable,
  IN BOOLEAN                      ResetMCastFilter,
  IN UINTN                        MCastFilterCnt   OPTIONAL,
  IN EFI_MAC_ADDRESS              *MCastFilter     OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  UINT32                Setting;
  UINTN                 Index;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (((Enable | Disable) & ~Gem->SnpMode.ReceiveFilterMask) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (!ResetMCastFilter &&
      ((Enable & ~Disable & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) != 0) &&
      ((MCastFilterCnt > Gem->SnpMode.MaxMCastFilterCount) ||
       ((MCastFilterCnt != 0) && (MCastFilter == NULL))))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  Setting = (Gem->SnpMode.ReceiveFilterSetting | Enable) & ~Disable;

  //
  // The multicast filter list is tracked for Mode reporting only: the
  // hardware accepts all multicast frames (all-ones hash) whenever
  // multicast reception is enabled.
  //
  if (ResetMCastFilter) {
    Gem->SnpMode.MCastFilterCount = 0;
    ZeroMem (Gem->SnpMode.MCastFilter, sizeof (Gem->SnpMode.MCastFilter));
  } else if ((Setting & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) != 0) {
    Gem->SnpMode.MCastFilterCount = (UINT32)MCastFilterCnt;
    for (Index = 0; Index < MCastFilterCnt; Index++) {
      CopyMem (
        &Gem->SnpMode.MCastFilter[Index],
        &MCastFilter[Index],
        sizeof (EFI_MAC_ADDRESS)
        );
    }
  }

  GemSetReceiveFilters (Gem, Setting);
  Gem->SnpMode.ReceiveFilterSetting = Setting;

  return EFI_SUCCESS;
}

/**
  Modifies or resets the current station address.

  @param  This[in]   Protocol instance pointer.
  @param  Reset[in]  TRUE to revert to the permanent address.
  @param  New[in]    New station address (ignored when Reset is TRUE).

  @retval EFI_SUCCESS           The station address was updated.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER Invalid parameter combination.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpStationAddress (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN BOOLEAN                      Reset,
  IN EFI_MAC_ADDRESS              *New  OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  if ((This == NULL) || (This->Mode == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Reset && (New == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  if (Reset) {
    CopyMem (
      &Gem->SnpMode.CurrentAddress,
      &Gem->SnpMode.PermanentAddress,
      sizeof (Gem->SnpMode.CurrentAddress)
      );
  } else {
    CopyMem (
      &Gem->SnpMode.CurrentAddress,
      New,
      sizeof (Gem->SnpMode.CurrentAddress)
      );
  }

  GemSetMacAddress (Gem, &Gem->SnpMode.CurrentAddress);

  return EFI_SUCCESS;
}

/**
  Resets or collects statistics on a network interface. Not supported.

  @param  This[in]                Protocol instance pointer.
  @param  Reset[in]               Ignored.
  @param  StatisticsSize[in,out]  Ignored.
  @param  StatisticsTable[out]    Ignored.

  @retval EFI_UNSUPPORTED  Statistics are not supported.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpStatistics (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN     BOOLEAN                      Reset,
  IN OUT UINTN                        *StatisticsSize   OPTIONAL,
  OUT    EFI_NETWORK_STATISTICS       *StatisticsTable  OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Converts a multicast IP address to a multicast hardware MAC address.

  @param  This[in]  Protocol instance pointer.
  @param  IPv6[in]  TRUE for an IPv6 address, FALSE for IPv4.
  @param  IP[in]    Multicast IP address to convert.
  @param  MAC[out]  Resulting multicast hardware MAC address.

  @retval EFI_SUCCESS           The address was converted.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER Invalid pointer or not a multicast address.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpMCastIpToMac (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN  BOOLEAN                      IPv6,
  IN  EFI_IP_ADDRESS               *IP,
  OUT EFI_MAC_ADDRESS              *MAC
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  if ((This == NULL) || (IP == NULL) || (MAC == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (MAC, sizeof (EFI_MAC_ADDRESS));

  if (IPv6) {
    //
    // RFC 2464: 33:33 followed by the last four octets of the address.
    //
    if (IP->v6.Addr[0] != 0xFF) {
      return EFI_INVALID_PARAMETER;
    }

    MAC->Addr[0] = 0x33;
    MAC->Addr[1] = 0x33;
    MAC->Addr[2] = IP->v6.Addr[12];
    MAC->Addr[3] = IP->v6.Addr[13];
    MAC->Addr[4] = IP->v6.Addr[14];
    MAC->Addr[5] = IP->v6.Addr[15];
  } else {
    //
    // RFC 1112: 01:00:5E followed by the low 23 bits of the address.
    //
    if ((IP->v4.Addr[0] & 0xF0) != 0xE0) {
      return EFI_INVALID_PARAMETER;
    }

    MAC->Addr[0] = 0x01;
    MAC->Addr[1] = 0x00;
    MAC->Addr[2] = 0x5E;
    MAC->Addr[3] = IP->v4.Addr[1] & 0x7F;
    MAC->Addr[4] = IP->v4.Addr[2];
    MAC->Addr[5] = IP->v4.Addr[3];
  }

  return EFI_SUCCESS;
}

/**
  Performs read/write operations on the NVRAM device. Not supported.

  @param  This[in]        Protocol instance pointer.
  @param  ReadWrite[in]   Ignored.
  @param  Offset[in]      Ignored.
  @param  BufferSize[in]  Ignored.
  @param  Buffer[in,out]  Ignored.

  @retval EFI_UNSUPPORTED  NVRAM access is not supported.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpNvData (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN     BOOLEAN                      ReadWrite,
  IN     UINTN                        Offset,
  IN     UINTN                        BufferSize,
  IN OUT VOID                         *Buffer
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Reads the current interrupt status and recycled transmit buffer status.
  Also refreshes the link (MediaPresent) state from the PHY.

  @param  This[in]              Protocol instance pointer.
  @param  InterruptStatus[out]  Bit mask of currently active interrupts.
  @param  TxBuf[out]            Recycled transmit buffer address, or NULL.

  @retval EFI_SUCCESS           The status was retrieved.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_INVALID_PARAMETER This is NULL.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.
  @retval EFI_ACCESS_DENIED     The driver instance is busy.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpGetStatus (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  OUT UINT32                       *InterruptStatus  OPTIONAL,
  OUT VOID                         **TxBuf           OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  EFI_STATUS            Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  Status = EfiAcquireLockOrFail (&Gem->Lock);
  if (EFI_ERROR (Status)) {
    return EFI_ACCESS_DENIED;
  }

  Gem->SnpMode.MediaPresent =
    (BOOLEAN) !EFI_ERROR (Rp1GemPhyUpdateConfig (Gem));

  //
  // The receive engine can stall under sustained load and then deliver
  // nothing until it is kicked. This is the only path that still runs once
  // that has happened, so the check belongs here rather than in the receive
  // path.
  //
  GemRecoverRxIfStalled (Gem);

  if (TxBuf != NULL) {
    GemGetRecycledTxBuffer (Gem, TxBuf);
  }

  if (InterruptStatus != NULL) {
    *InterruptStatus = 0;
    if (GemRxPending (Gem)) {
      *InterruptStatus |= EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
    }

    if (GemTxPendingCompletion (Gem)) {
      *InterruptStatus |= EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT;
    }
  }

  //
  // Bring-up diagnostic: dump the cumulative MAC statistics roughly every
  // ten seconds of MNP polling. One compact line so a lossy serial capture
  // still tells RX-dead (all zero) from bad-clocking (fcs/sym climbing)
  // from DMA trouble (rx counts but resource/overrun errors).
  //
  {
    STATIC UINTN  PollCount = 0;

    if ((++PollCount & 0x3FF) == 0) {
      DEBUG ((
        DEBUG_ERROR,
        "Rp1GemDxe: stat tx %u rx %u bc %u mc %u fcs %u sym %u aln %u res %u ovr %u"
        " rec %u\n",
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_FRAMES_TX),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_FRAMES_RX),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_BCAST_FRAMES_RX),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_MULTI_FRAMES_RX),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_FCS_ERRS),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_RX_SYMBOL_ERRS),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_ALIGNMENT_ERRS),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_RX_RESOURCE_ERRS),
        MmioRead32 ((UINTN)Gem->GemBase + GEM_STAT_RX_OVERRUN_ERRS),
        Gem->RxStallRecoveries
        ));
    }
  }

  EfiReleaseLock (&Gem->Lock);

  return EFI_SUCCESS;
}

/**
  Places a packet in the transmit queue of the network interface.

  @param  This[in]        Protocol instance pointer.
  @param  HeaderSize[in]  Size of the media header to fill in; 0 or equal
                          to Mode->MediaHeaderSize.
  @param  BufferSize[in]  Size of the entire packet, in bytes.
  @param  Buffer[in]      Packet to transmit.
  @param  SrcAddr[in]     Source address (optional when HeaderSize != 0).
  @param  DestAddr[in]    Destination address (when HeaderSize != 0).
  @param  Protocol[in]    Ethernet type (when HeaderSize != 0).

  @retval EFI_SUCCESS           The packet was placed on the transmit queue.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_NOT_READY         The transmit ring is full or no link.
  @retval EFI_BUFFER_TOO_SMALL  BufferSize is too small.
  @retval EFI_INVALID_PARAMETER Invalid parameter combination.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.
  @retval EFI_ACCESS_DENIED     The driver instance is busy.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpTransmit (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINTN                        HeaderSize,
  IN UINTN                        BufferSize,
  IN VOID                         *Buffer,
  IN EFI_MAC_ADDRESS              *SrcAddr   OPTIONAL,
  IN EFI_MAC_ADDRESS              *DestAddr  OPTIONAL,
  IN UINT16                       *Protocol  OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  EFI_STATUS            Status;
  UINT8                 *Frame;
  EFI_MAC_ADDRESS       *Src;
  UINTN                 Retries;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  if (HeaderSize != 0) {
    if (HeaderSize != Gem->SnpMode.MediaHeaderSize) {
      return EFI_INVALID_PARAMETER;
    }

    if ((DestAddr == NULL) || (Protocol == NULL)) {
      return EFI_INVALID_PARAMETER;
    }
  }

  if (BufferSize < Gem->SnpMode.MediaHeaderSize) {
    return EFI_BUFFER_TOO_SMALL;
  }

  if (BufferSize > GEM_MAX_FRAME_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Gem->SnpMode.MediaPresent) {
    //
    // Give autonegotiation a chance to finish rather than failing the
    // send: callers frequently transmit right after Initialize().
    //
    Status = EFI_NOT_READY;
    for (Retries = GEM_LINK_TIMEOUT_MS / 10; Retries > 0; Retries--) {
      Status = Rp1GemPhyUpdateConfig (Gem);
      if (!EFI_ERROR (Status)) {
        break;
      }

      gBS->Stall (10000);
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Rp1GemDxe: no link, dropping transmit\n"));
      return EFI_NOT_READY;
    }

    Gem->SnpMode.MediaPresent = TRUE;
  }

  Frame = Buffer;

  if (HeaderSize != 0) {
    Src = (SrcAddr != NULL) ? SrcAddr : &Gem->SnpMode.CurrentAddress;

    CopyMem (&Frame[0], &DestAddr->Addr[0], GEM_ETHER_ADDR_LEN);
    CopyMem (&Frame[GEM_ETHER_ADDR_LEN], &Src->Addr[0], GEM_ETHER_ADDR_LEN);
    Frame[12] = (UINT8)((*Protocol >> 8) & 0xFF);
    Frame[13] = (UINT8)(*Protocol & 0xFF);
  }

  Status = EfiAcquireLockOrFail (&Gem->Lock);
  if (EFI_ERROR (Status)) {
    return EFI_ACCESS_DENIED;
  }

  Status = GemTransmitFrame (Gem, Buffer, BufferSize);

  EfiReleaseLock (&Gem->Lock);

  return Status;
}

/**
  Receives a packet from the network interface.

  @param  This[in]            Protocol instance pointer.
  @param  HeaderSize[out]     Media header size, if not NULL.
  @param  BufferSize[in,out]  On input, size of Buffer; on output, the size
                              of the received packet.
  @param  Buffer[out]         Destination for the received packet.
  @param  SrcAddr[out]        Source address, if not NULL.
  @param  DestAddr[out]       Destination address, if not NULL.
  @param  Protocol[out]       Ethernet type, if not NULL.

  @retval EFI_SUCCESS           The packet was received.
  @retval EFI_NOT_STARTED       The network interface has not been started.
  @retval EFI_NOT_READY         No packet is pending.
  @retval EFI_BUFFER_TOO_SMALL  Buffer is too small; *BufferSize holds the
                                required size.
  @retval EFI_INVALID_PARAMETER Invalid pointer.
  @retval EFI_DEVICE_ERROR      The network interface is in an invalid state.
  @retval EFI_ACCESS_DENIED     The driver instance is busy.

**/
STATIC
EFI_STATUS
EFIAPI
Rp1GemSnpReceive (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  OUT    UINTN                        *HeaderSize  OPTIONAL,
  IN OUT UINTN                        *BufferSize,
  OUT    VOID                         *Buffer,
  OUT    EFI_MAC_ADDRESS              *SrcAddr     OPTIONAL,
  OUT    EFI_MAC_ADDRESS              *DestAddr    OPTIONAL,
  OUT    UINT16                       *Protocol    OPTIONAL
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;
  EFI_STATUS            Status;
  UINT8                 *Frame;

  if ((This == NULL) || (BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Gem = RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS (This);
  if (Gem->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Gem->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  Status = EfiAcquireLockOrFail (&Gem->Lock);
  if (EFI_ERROR (Status)) {
    return EFI_ACCESS_DENIED;
  }

  Status = GemReceiveFrame (Gem, Buffer, BufferSize);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&Gem->Lock);
    return Status;
  }

  Frame = Buffer;

  if (DestAddr != NULL) {
    ZeroMem (DestAddr, sizeof (EFI_MAC_ADDRESS));
    CopyMem (&DestAddr->Addr[0], &Frame[0], GEM_ETHER_ADDR_LEN);
  }

  if (SrcAddr != NULL) {
    ZeroMem (SrcAddr, sizeof (EFI_MAC_ADDRESS));
    CopyMem (&SrcAddr->Addr[0], &Frame[GEM_ETHER_ADDR_LEN], GEM_ETHER_ADDR_LEN);
  }

  if (Protocol != NULL) {
    *Protocol = (UINT16)((Frame[12] << 8) | Frame[13]);
  }

  if (HeaderSize != NULL) {
    *HeaderSize = Gem->SnpMode.MediaHeaderSize;
  }

  EfiReleaseLock (&Gem->Lock);

  return EFI_SUCCESS;
}

/**
  WaitForPacket event notification: signals the event whenever a received
  frame is pending in the RX ring. Registered as an EVT_NOTIFY_WAIT event,
  so this runs each time a caller checks or waits on Snp->WaitForPacket.

  @param  Event[in]    The WaitForPacket event.
  @param  Context[in]  Driver private data.

**/
VOID
EFIAPI
Rp1GemWaitForPacketNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  RP1_GEM_PRIVATE_DATA  *Gem;

  Gem = (RP1_GEM_PRIVATE_DATA *)Context;

  if ((Gem == NULL) || (Gem->SnpMode.State != EfiSimpleNetworkInitialized)) {
    return;
  }

  if (GemRxPending (Gem)) {
    gBS->SignalEvent (Event);
  }
}

///
/// Simple Network Protocol template
///
CONST EFI_SIMPLE_NETWORK_PROTOCOL  gRp1GemSimpleNetworkTemplate = {
  EFI_SIMPLE_NETWORK_PROTOCOL_REVISION,   // Revision
  Rp1GemSnpStart,                         // Start
  Rp1GemSnpStop,                          // Stop
  Rp1GemSnpInitialize,                    // Initialize
  Rp1GemSnpReset,                         // Reset
  Rp1GemSnpShutdown,                      // Shutdown
  Rp1GemSnpReceiveFilters,                // ReceiveFilters
  Rp1GemSnpStationAddress,                // StationAddress
  Rp1GemSnpStatistics,                    // Statistics
  Rp1GemSnpMCastIpToMac,                  // MCastIpToMac
  Rp1GemSnpNvData,                        // NvData
  Rp1GemSnpGetStatus,                     // GetStatus
  Rp1GemSnpTransmit,                      // Transmit
  Rp1GemSnpReceive,                       // Receive
  NULL,                                   // WaitForPacket (set at Start)
  NULL                                    // Mode (set at Start)
};

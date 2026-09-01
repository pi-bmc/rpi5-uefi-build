/** @file
  CDC-EEM wire framing, isolated from the UEFI driver so it can be unit
  tested on a build host.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>

//
// CDC EEM 1.0, section 3. Every packet on either bulk pipe carries a 2-byte
// little-endian header:
//
//   bit 15    bmType   0 = data, 1 = command
//   Data:     bit 14   bmCRC (0 = sentinel, 1 = real CRC32)
//             bits 13:0 length of the Ethernet frame INCLUDING the CRC field
//   Command:  bit 14   reserved, bits 13:11 bmEEMCmd, bits 10:0 parameter
//
// A header of 0x0000 is a zero-length packet the device appends purely to
// force a short packet; it is padding, not an error.
//
#define USB_EEM_HEADER_LEN  2
#define USB_EEM_CRC_LEN     4

//
// The peer (Linux f_eem, eem_unwrap) rejects a data packet whose length is
// below ETH_HLEN + ETH_FCS_LEN and frees the ENTIRE transfer when it does,
// so one runt frame would take every frame bundled with it. Never emit one.
//
#define USB_EEM_MIN_DATA_LEN  18

//
// Largest Ethernet frame carried, matching UsbCdcNcm's USB_ETHERNET_FRAME_SIZE.
//
#define USB_EEM_MAX_FRAME  0x5F2

//
// Receive staging buffer. One max frame plus header, CRC and a possible
// 2-byte pad is 1530; 4096 leaves room for a bundle at negligible cost.
//
#define USB_EEM_MAX_BULK_SIZE  0x1000

/**
  Wrap one Ethernet frame in an EEM data packet.

  @param[out] Out       Destination buffer.
  @param[in]  OutSize   Bytes available at Out.
  @param[in]  Eth       The Ethernet frame, without any FCS.
  @param[in]  EthLen    Length of Eth.
  @param[out] OutLen    Bytes written to Out.

  @retval EFI_SUCCESS            The packet was written.
  @retval EFI_INVALID_PARAMETER  A pointer was NULL, or EthLen is outside
                                 what EEM can express or what the peer accepts.
  @retval EFI_BUFFER_TOO_SMALL   Out is too small for the wrapped frame.
**/
EFI_STATUS
UsbEemFrameTx (
  OUT UINT8        *Out,
  IN  UINTN        OutSize,
  IN  CONST UINT8  *Eth,
  IN  UINTN        EthLen,
  OUT UINTN        *OutLen
  );

/**
  Walk the next Ethernet frame out of a received bulk transfer, stepping over
  command packets and zero-length padding.

  Offset is the caller's cursor into Buf and is advanced past whatever this
  call consumed. On a malformed buffer the cursor is moved to BufLen so the
  caller abandons it rather than re-walking it.

  @param[in]      Buf       The received transfer.
  @param[in]      BufLen    Valid bytes in Buf.
  @param[in, out] Offset    Cursor into Buf.
  @param[out]     Frame     Set to the frame inside Buf (no copy is made).
  @param[out]     FrameLen  Length of Frame, with the CRC field removed.

  @retval EFI_SUCCESS            A frame was found.
  @retval EFI_NOT_FOUND          The buffer holds no further frame, or is
                                 malformed and has been abandoned.
  @retval EFI_INVALID_PARAMETER  A pointer was NULL.
**/
EFI_STATUS
UsbEemNextRxFrame (
  IN     CONST UINT8  *Buf,
  IN     UINTN        BufLen,
  IN OUT UINTN        *Offset,
  OUT    CONST UINT8  **Frame,
  OUT    UINTN        *FrameLen
  );

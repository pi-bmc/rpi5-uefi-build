/** @file
  CDC-EEM wire framing.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbEemFraming.h"

EFI_STATUS
UsbEemFrameTx (
  OUT UINT8        *Out,
  IN  UINTN        OutSize,
  IN  CONST UINT8  *Eth,
  IN  UINTN        EthLen,
  OUT UINTN        *OutLen
  )
{
  UINTN  Len;

  if ((Out == NULL) || (Eth == NULL) || (OutLen == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The length field counts the CRC field as well: the peer trims to
  // len - ETH_FCS_LEN after validating it.
  //
  Len = EthLen + USB_EEM_CRC_LEN;

  if ((Len < USB_EEM_MIN_DATA_LEN) || (Len > 0x3FFF)) {
    return EFI_INVALID_PARAMETER;
  }

  if (OutSize < (USB_EEM_HEADER_LEN + Len)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  //
  // bmType = 0 (data), bmCRC = 0 (sentinel). Little-endian.
  //
  Out[0] = (UINT8)(Len & 0xFF);
  Out[1] = (UINT8)((Len >> 8) & 0x3F);

  CopyMem (Out + USB_EEM_HEADER_LEN, Eth, EthLen);

  //
  // Written big-endian: the peer reads it with get_unaligned_be32 and
  // compares against 0xdeadbeef. Little-endian here silently drops the
  // frame -- no error is reported on either side.
  //
  Out[USB_EEM_HEADER_LEN + EthLen + 0] = 0xDE;
  Out[USB_EEM_HEADER_LEN + EthLen + 1] = 0xAD;
  Out[USB_EEM_HEADER_LEN + EthLen + 2] = 0xBE;
  Out[USB_EEM_HEADER_LEN + EthLen + 3] = 0xEF;

  *OutLen = USB_EEM_HEADER_LEN + Len;
  return EFI_SUCCESS;
}

EFI_STATUS
UsbEemNextRxFrame (
  IN     CONST UINT8  *Buf,
  IN     UINTN        BufLen,
  IN OUT UINTN        *Offset,
  OUT    CONST UINT8  **Frame,
  OUT    UINTN        *FrameLen
  )
{
  UINT16  Header;
  UINTN   Len;

  if ((Buf == NULL) || (Offset == NULL) || (Frame == NULL) || (FrameLen == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  while ((*Offset + USB_EEM_HEADER_LEN) <= BufLen) {
    Header   = (UINT16)(Buf[*Offset] | (Buf[*Offset + 1] << 8));
    *Offset += USB_EEM_HEADER_LEN;

    //
    // Padding the device appends to force a short packet. Not an error, and
    // not necessarily the end of the buffer.
    //
    if (Header == 0) {
      continue;
    }

    if ((Header & BIT15) != 0) {
      //
      // A command packet. Nothing here needs answering: the peer only ever
      // responds to Echo, and the hints carry no state this driver keeps.
      // Step over its parameter bytes and keep walking.
      //
      Len = Header & 0x7FF;
      if ((*Offset + Len) > BufLen) {
        *Offset = BufLen;
        return EFI_NOT_FOUND;
      }

      *Offset += Len;
      continue;
    }

    Len = Header & 0x3FFF;

    //
    // A length that reaches past the buffer, or that leaves no Ethernet
    // bytes once the CRC field is removed, means framing sync is lost.
    // Abandon the whole buffer rather than walk it further.
    //
    if ((Len <= USB_EEM_CRC_LEN) || ((*Offset + Len) > BufLen)) {
      *Offset = BufLen;
      return EFI_NOT_FOUND;
    }

    *Frame    = Buf + *Offset;
    *FrameLen = Len - USB_EEM_CRC_LEN;
    *Offset  += Len;
    return EFI_SUCCESS;
  }

  *Offset = BufLen;
  return EFI_NOT_FOUND;
}

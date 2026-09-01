#include <stdio.h>
#include <string.h>
#include "UsbEemFraming.h"

static int Failures = 0;

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); Failures++; } \
  } while (0)

static void
TestTxFramesA60ByteFrame (void)
{
  UINT8   Eth[60];
  UINT8   Out[128];
  UINTN   OutLen = 0;
  UINTN   Index;

  for (Index = 0; Index < sizeof (Eth); Index++) {
    Eth[Index] = (UINT8)Index;
  }

  CHECK (UsbEemFrameTx (Out, sizeof (Out), Eth, sizeof (Eth), &OutLen) == EFI_SUCCESS,
         "60-byte frame should encode");
  CHECK (OutLen == 2 + 60 + 4, "encoded length is header + frame + CRC");

  // Header counts the CRC field: 60 + 4 = 64 = 0x0040, little-endian.
  CHECK (Out[0] == 0x40 && Out[1] == 0x00, "header is LE len-with-CRC, bmType/bmCRC clear");

  CHECK (memcmp (Out + 2, Eth, sizeof (Eth)) == 0, "frame copied verbatim");

  // The gadget compares with get_unaligned_be32, so this is big-endian.
  CHECK (Out[62] == 0xDE && Out[63] == 0xAD && Out[64] == 0xBE && Out[65] == 0xEF,
         "sentinel CRC is big-endian DE AD BE EF");
}

static void
TestTxRefusesRuntFrame (void)
{
  UINT8  Eth[13];
  UINT8  Out[64];
  UINTN  OutLen = 0;

  memset (Eth, 0xAA, sizeof (Eth));

  // 13 + 4 = 17 < ETH_HLEN + ETH_FCS_LEN. eem_unwrap would `goto error` and
  // free the WHOLE transfer, taking any frame bundled with it.
  CHECK (UsbEemFrameTx (Out, sizeof (Out), Eth, sizeof (Eth), &OutLen) == EFI_INVALID_PARAMETER,
         "a frame below the 18-byte floor must be refused, not emitted");
}

static void
TestTxAcceptsExactMinimum (void)
{
  UINT8  Eth[14];
  UINT8  Out[64];
  UINTN  OutLen = 0;

  memset (Eth, 0xBB, sizeof (Eth));

  CHECK (UsbEemFrameTx (Out, sizeof (Out), Eth, sizeof (Eth), &OutLen) == EFI_SUCCESS,
         "14 + 4 == 18 is exactly the floor and must be accepted");
  CHECK (Out[0] == 0x12 && Out[1] == 0x00, "header is 18");
}

static void
TestTxRejectsTooSmallBuffer (void)
{
  UINT8  Eth[60];
  UINT8  Out[32];
  UINTN  OutLen = 0;

  memset (Eth, 0xCC, sizeof (Eth));

  CHECK (UsbEemFrameTx (Out, sizeof (Out), Eth, sizeof (Eth), &OutLen) == EFI_BUFFER_TOO_SMALL,
         "undersized output buffer is refused");
}

static void
TestRxReadsOneDataFrame (void)
{
  UINT8         Buf[2 + 64];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x5A, sizeof (Buf));
  Buf[0] = 0x40;  // len 64, includes the 4-byte CRC
  Buf[1] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "a single data packet is read");
  CHECK (FrameLen == 60, "frame length is header len minus the CRC field");
  CHECK (Frame == Buf + 2, "frame points just past the header");
  CHECK (Offset == sizeof (Buf), "cursor consumed the whole packet");
}

static void
TestRxSkipsZeroLengthPadding (void)
{
  UINT8         Buf[2 + 2 + 64];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x11, sizeof (Buf));
  Buf[0] = 0x00;  // eem_wrap's short-packet pad
  Buf[1] = 0x00;
  Buf[2] = 0x40;
  Buf[3] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "a zero header is padding and must be skipped, not treated as an error");
  CHECK (FrameLen == 60, "the frame after the pad is returned");
}

static void
TestRxSkipsCommandPacket (void)
{
  UINT8         Buf[2 + 4 + 2 + 64];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x22, sizeof (Buf));

  // bmType=1 (command), bmEEMCmd=0 (echo), param = 4 payload bytes.
  Buf[0] = 0x04;
  Buf[1] = 0x80;
  Buf[6] = 0x40;  // then a normal 64-byte data packet
  Buf[7] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "a command packet is stepped over and the following frame returned");
  CHECK (FrameLen == 60, "the data frame after the command is returned");
  CHECK (Frame == Buf + 8, "frame points past command packet and data header");
}

static void
TestRxWalksTwoBundledFrames (void)
{
  UINT8         Buf[2 + 64 + 2 + 24];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x33, sizeof (Buf));
  Buf[0]  = 0x40;  // 64 -> 60 bytes of Ethernet
  Buf[1]  = 0x00;
  Buf[66] = 0x18;  // 24 -> 20 bytes of Ethernet
  Buf[67] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "first bundled frame");
  CHECK (FrameLen == 60, "first frame length");

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "second bundled frame -- NOT spliced onto the first");
  CHECK (FrameLen == 20, "second frame length");

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_NOT_FOUND,
         "buffer is then exhausted");
}

static void
TestRxRejectsOverlongLength (void)
{
  UINT8         Buf[2 + 8];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x44, sizeof (Buf));
  Buf[0] = 0xFF;  // claims 255 bytes in a 10-byte buffer
  Buf[1] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_NOT_FOUND,
         "a length past the end of the buffer is malformed");
  CHECK (Offset == sizeof (Buf), "a malformed buffer is abandoned, not re-walked");
}

static void
TestRxRejectsLengthBelowCrcField (void)
{
  UINT8         Buf[2 + 8];
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;

  memset (Buf, 0x55, sizeof (Buf));
  Buf[0] = 0x04;  // len == CRC field alone -> zero-length Ethernet frame
  Buf[1] = 0x00;

  CHECK (UsbEemNextRxFrame (Buf, sizeof (Buf), &Offset, &Frame, &FrameLen) == EFI_NOT_FOUND,
         "a length that leaves no Ethernet bytes is malformed");
}

static void
TestTxRxRoundTrip (void)
{
  UINT8         Eth[100];
  UINT8         Out[256];
  UINTN         OutLen = 0;
  UINTN         Offset = 0;
  CONST UINT8  *Frame  = NULL;
  UINTN         FrameLen = 0;
  UINTN         Index;

  for (Index = 0; Index < sizeof (Eth); Index++) {
    Eth[Index] = (UINT8)(Index * 7);
  }

  CHECK (UsbEemFrameTx (Out, sizeof (Out), Eth, sizeof (Eth), &OutLen) == EFI_SUCCESS,
         "round trip encodes");
  CHECK (UsbEemNextRxFrame (Out, OutLen, &Offset, &Frame, &FrameLen) == EFI_SUCCESS,
         "round trip decodes");
  CHECK (FrameLen == sizeof (Eth), "round trip preserves length");
  CHECK (memcmp (Frame, Eth, sizeof (Eth)) == 0, "round trip preserves payload");
}

int
main (void)
{
  TestTxFramesA60ByteFrame ();
  TestTxRefusesRuntFrame ();
  TestTxAcceptsExactMinimum ();
  TestTxRejectsTooSmallBuffer ();
  TestRxReadsOneDataFrame ();
  TestRxSkipsZeroLengthPadding ();
  TestRxSkipsCommandPacket ();
  TestRxWalksTwoBundledFrames ();
  TestRxRejectsOverlongLength ();
  TestRxRejectsLengthBelowCrcField ();
  TestTxRxRoundTrip ();

  if (Failures != 0) {
    printf ("%d check(s) failed\n", Failures);
    return 1;
  }

  printf ("all EEM framing checks passed\n");
  return 0;
}

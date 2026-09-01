# CDC-EEM SNP Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write a CDC-EEM USB Ethernet class driver for EDK2 and wire it into the RPi5 and NUC firmwares, so each managed host can bind the BMC's `eem.usb0` Redfish host-interface NIC.

**Architecture:** A new `MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/` module that produces `gEdkIIUsbEthProtocolGuid`, exactly as its three AMI-donated siblings do, so the existing `NetworkCommon` UNDI binds it with no change. The risky part -- EEM wire framing -- is isolated in a dependency-free `UsbEemFraming.c` that is unit-tested natively on the host before any firmware is built. Both repos receive the same driver body as an add-files quilt patch generated from one canonical source tree.

**Tech Stack:** EDK2 (UEFI_DRIVER, C), Yocto/bitbake, quilt patches, uncrustify (TianoCore fork 73.0.11), plain C99 + gcc for the framing unit tests.

**Spec:** `docs/superpowers/specs/2026-09-01-cdc-eem-migration-design.md`

## Global Constraints

- **Repos in scope:** `rpi5-uefi-build` and `nuc-bios-build` only. `nanokvm-app` is already on EEM (commit `1b003d7`) and is owned by another agent; `nanokvm-build` needs nothing.
- **Station address:** host `da:c0:ff:ee:10:02`, BMC `da:c0:ff:ee:10:01`. Host IP `169.254.10.2` by DHCP, BMC `169.254.10.1/16`.
- **EEM interface triple:** class `0x02`, subclass `0x0C`, protocol `0x07`. Single interface, 2 bulk endpoints, no interrupt endpoint.
- **Sentinel CRC is big-endian on the wire:** `DE AD BE EF`.
- **RX header length includes the 4-byte CRC field.** The Ethernet frame is `len - 4`.
- **TX minimum:** `EthLen + 4 >= 18`, or the gadget discards the entire transfer.
- **No link-state signalling and no speed notification.** Assume link-up on enumeration; report a fixed speed.
- **edk2 patch bodies must use CRLF line endings.** Never write a `.patch` touching edk2 sources with a text editor tool; generate it in bytes mode.
- **RELEASE builds strip `DEBUG()`** (`MDEPKG_NDEBUG` + `BaseDebugLibNull`). Never treat a `strings`-grep of a RELEASE binary as a build check.
- **EDK2 C style is enforced** by `hack/format-edk2.sh` (uncrustify, TianoCore fork 73.0.11). Sources stored LF in git; the patch body is rendered CRLF.
- **New PCD token:** `0x30001064` in `MdeModulePkg.dec` (verified unused).
- **New FILE_GUID:** `2ce7bcaf-b1e5-458a-b6ea-048292c45e1f`.

---

## File Structure

Canonical driver sources live in the rpi5 repo, where `hack/format-edk2.sh` already covers the tree by default:

| Path | Responsibility |
| --- | --- |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.h` | Pure framing API + wire constants. No EDK2 logic. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.c` | EEM header encode/decode. The only file with tests. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.h` | Driver instance struct, prototypes. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.c` | Driver binding: Supported/Start/Stop, entry point. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFunction.c` | `EDKII_USB_ETHERNET_PROTOCOL` implementation. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/ComponentName.c` | `EFI_COMPONENT_NAME2_PROTOCOL`. |
| `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.inf` | Module definition. |
| `hack/usbcdceem/test/` | Native gcc test harness + EDK2 stub headers. |
| `hack/usbcdceem/gen-patch.py` | Renders the add-files patch (CRLF) for either repo. |

The framing split is the load-bearing decision: it is the only part with a
real red/green test cycle, and it is where every wire-format mistake would
otherwise hide until hardware bring-up.

---

### Task 1: EEM framing unit, natively tested

**Files:**
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.h`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.c`
- Create: `hack/usbcdceem/test/stubs/Uefi.h`
- Create: `hack/usbcdceem/test/stubs/Library/BaseMemoryLib.h`
- Test: `hack/usbcdceem/test/test-framing.c`
- Create: `hack/usbcdceem/test/run.sh`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `EFI_STATUS UsbEemFrameTx (UINT8 *Out, UINTN OutSize, CONST UINT8 *Eth, UINTN EthLen, UINTN *OutLen)`
  - `EFI_STATUS UsbEemNextRxFrame (CONST UINT8 *Buf, UINTN BufLen, UINTN *Offset, CONST UINT8 **Frame, UINTN *FrameLen)`
  - Constants `USB_EEM_HEADER_LEN` (2), `USB_EEM_CRC_LEN` (4), `USB_EEM_MIN_DATA_LEN` (18), `USB_EEM_MAX_FRAME` (0x5F2), `USB_EEM_MAX_BULK_SIZE` (0x1000)

- [ ] **Step 1: Create the stub headers so the framing unit compiles natively**

`hack/usbcdceem/test/stubs/Uefi.h`:

```c
// Minimal EDK2 surface for compiling UsbEemFraming.c on the build host.
// Deliberately NOT an ifdef inside the driver: shipped sources include the
// real <Uefi.h>, and the test just puts these earlier on the include path.
#ifndef USB_EEM_TEST_UEFI_H_
#define USB_EEM_TEST_UEFI_H_

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef size_t   UINTN;
typedef int      BOOLEAN;

#define CONST const
#define IN
#define OUT
#define VOID void

typedef UINTN EFI_STATUS;

#define EFI_SUCCESS            0
#define EFI_INVALID_PARAMETER  2
#define EFI_BUFFER_TOO_SMALL   5
#define EFI_NOT_FOUND          14

#define BIT14 0x00004000
#define BIT15 0x00008000

#endif
```

`hack/usbcdceem/test/stubs/Library/BaseMemoryLib.h`:

```c
#ifndef USB_EEM_TEST_BASEMEMORYLIB_H_
#define USB_EEM_TEST_BASEMEMORYLIB_H_

#include <string.h>
#include <Uefi.h>

static inline VOID *
CopyMem (VOID *Dest, CONST VOID *Src, UINTN Len)
{
  return memcpy (Dest, Src, Len);
}

#endif
```

- [ ] **Step 2: Write the failing test**

`hack/usbcdceem/test/test-framing.c`. Every vector is derived from the
gadget's own `eem_wrap`/`eem_unwrap` in
`drivers/usb/gadget/function/f_eem.c`, not from spec prose.

```c
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
```

`hack/usbcdceem/test/run.sh`:

```bash
#!/usr/bin/env bash
# Compile and run the CDC-EEM framing unit tests on the build host.
# The driver sources are unmodified: stubs/ just shadows the EDK2 headers.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/../../../meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem"
OUT="$(mktemp -d)"
trap 'rm -rf "${OUT}"' EXIT

gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"${HERE}/stubs" -I"${SRC}" \
    -o "${OUT}/test-framing" \
    "${HERE}/test-framing.c" "${SRC}/UsbEemFraming.c"

"${OUT}/test-framing"
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
chmod +x hack/usbcdceem/test/run.sh
./hack/usbcdceem/test/run.sh
```

Expected: FAIL -- `UsbEemFraming.h: No such file or directory`.

- [ ] **Step 4: Write the framing header**

`meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.h`:

```c
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
```

- [ ] **Step 5: Write the framing implementation**

`meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFraming.c`:

```c
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
```

- [ ] **Step 6: Run the tests and confirm they pass**

```bash
./hack/usbcdceem/test/run.sh
```

Expected: `all EEM framing checks passed`, exit 0. ASan/UBSan clean.

- [ ] **Step 7: Format check**

```bash
./hack/format-edk2.sh meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem
./hack/format-edk2.sh --check meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem
```

Expected: the `--check` run exits 0. Re-run `./hack/usbcdceem/test/run.sh` afterwards to confirm formatting changed no behaviour.

- [ ] **Step 8: Commit**

```bash
git add meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem hack/usbcdceem
git commit -m "feat(usbcdceem): CDC-EEM wire framing with native unit tests"
```

---

### Task 2: The EDK2 class driver module

**Files:**
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.h`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.c`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbEemFunction.c`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/ComponentName.c`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem/UsbCdcEem.inf`

**Interfaces:**
- Consumes: `UsbEemFrameTx`, `UsbEemNextRxFrame`, `USB_EEM_MAX_BULK_SIZE`, `USB_EEM_MAX_FRAME` from Task 1.
- Produces: a module installing `gEdkIIUsbEthProtocolGuid`; the PCD `gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress` consumed by Task 3's `.dec` hunk.

**Reference for style and structure:** `UsbCdcEcm` in the pinned edk2 tree, at
`/home/appkins/src/pi-bmc/nuc-bios-build/build/tmp/work/corei7-64-poky-linux/edk2-uefipayload/2605+git/git/MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEcm/`.
Read it before writing. Copy `ComponentName.c` from there and change the
strings and symbol names; it is boilerplate and should not be reinvented.

**These protocol members MUST be non-NULL** -- `NetworkCommon` calls them
without a NULL check, and two of them under `ASSERT_EFI_ERROR`:

| Member | Must return |
| --- | --- |
| `UsbEthMacAddress` | `EFI_SUCCESS` (asserted) |
| `UsbEthMaxBulkSize` | `EFI_SUCCESS` |
| `UsbEthInterrupt` | `EFI_SUCCESS` (asserted) -- **not** `EFI_UNSUPPORTED` |
| `UsbEthFunDescriptor` | a synthesized descriptor |
| `SetUsbEthPacketFilter` | any status; result unused |
| `UsbEthTransmit`, `UsbEthReceive` | the real implementations |

Everything under `UsbEthUndi.*`, plus `UsbEthInitialize` and
`UsbEthStatistics`, is NULL-checked and stays NULL (the struct comes from
`AllocateZeroPool`), exactly as `UsbCdcEcm` leaves them.

- [ ] **Step 1: Write the driver header**

`UsbCdcEem.h`:

```c
/** @file
  Header for the USB CDC Ethernet Emulation Model (EEM) driver.

  Copyright (c) 2026, appkins. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiUsbLib.h>
#include <Protocol/UsbIo.h>
#include <Protocol/UsbEthernetProtocol.h>

#include "UsbEemFraming.h"

//
// CDC EEM is a single interface carrying two bulk endpoints: no control
// interface, no CDC functional descriptors, no notification endpoint and no
// alternate settings. That is the whole reason it is used here -- it costs
// one device IN endpoint where ECM/NCM/RNDIS cost two, and the BMC's dwc2
// core has exactly six to spend.
//
#define USB_CDC_EEM_SUBCLASS  0x0C
#define USB_CDC_EEM_PROTOCOL  0x07

#define USB_EEM_DRIVER_VERSION         1
#define USB_ETHERNET_BULK_TIMEOUT      1
#define USB_ETHERNET_TRANSFER_TIMEOUT  200

//
// Six, spelled locally rather than as NetLib's NET_ETHER_ADDR_LEN: NetLib is
// a NetworkPkg library class and none of the other class drivers in this
// directory depend on it.
//
#define USB_EEM_MAC_LEN  6

typedef struct {
  UINTN                          Signature;
  EDKII_USB_ETHERNET_PROTOCOL    UsbEth;
  EFI_USB_IO_PROTOCOL            *UsbIo;
  EFI_USB_CONFIG_DESCRIPTOR      *Config;
  UINT8                          NumOfInterface;
  UINT8                          BulkInEndpoint;
  UINT8                          BulkOutEndpoint;
  EFI_MAC_ADDRESS                MacAddress;

  //
  // Receive staging. One bulk transfer may carry several EEM packets, and
  // NetworkCommon wants exactly one Ethernet frame per call, so the buffer
  // is walked with a byte cursor across calls.
  //
  // A cursor rather than a datagram index pair on purpose: the index
  // arithmetic UsbCdcNcm uses is what produced its underflow-to-255 bug.
  //
  UINT8                          *RxBuffer;
  UINTN                          RxLength;
  UINTN                          RxOffset;

  //
  // Transmit staging, so a frame can be wrapped without allocating per send.
  //
  UINT8                          *TxBuffer;
} USB_ETHERNET_DRIVER;

#define USB_EEM_SIGNATURE  SIGNATURE_32('u','e','e','m')
#define USB_EEM_DEV_FROM_THIS(a)  CR (a, USB_ETHERNET_DRIVER, UsbEth, USB_EEM_SIGNATURE)

extern EFI_COMPONENT_NAME2_PROTOCOL  gUsbEemComponentName2;

EFI_STATUS
EFIAPI
UsbEemDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UsbEemDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UsbEemDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  );

VOID
GetEndpoint (
  IN     EFI_USB_IO_PROTOCOL  *UsbIo,
  IN OUT USB_ETHERNET_DRIVER  *UsbEthDriver
  );

EFI_STATUS
LoadAllDescriptor (
  IN     EFI_USB_IO_PROTOCOL        *UsbIo,
  IN OUT EFI_USB_CONFIG_DESCRIPTOR  **ConfigDesc
  );

EFI_STATUS
EFIAPI
UsbEthEemReceive (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN OUT VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  );

EFI_STATUS
EFIAPI
UsbEthEemTransmit (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN     VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  );

EFI_STATUS
EFIAPI
UsbEthEemInterrupt (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN BOOLEAN                      IsNewTransfer,
  IN UINTN                        PollingInterval,
  IN EFI_USB_DEVICE_REQUEST       *Request
  );

EFI_STATUS
EFIAPI
GetUsbEthMacAddress (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS              *MacAddress
  );

EFI_STATUS
EFIAPI
UsbEthEemBulkSize (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT UINTN                        *BulkSize
  );

EFI_STATUS
EFIAPI
GetUsbEthFunDescriptor (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT USB_ETHERNET_FUN_DESCRIPTOR  *UsbEthFunDescriptor
  );

EFI_STATUS
EFIAPI
SetUsbEthPacketFilter (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN UINT16                       Value
  );
```

- [ ] **Step 2: Write `UsbEemFunction.c` -- the protocol implementation**

Key bodies. `GetEndpoint` and `LoadAllDescriptor` are lifted from
`UsbCdcEcm/UsbEcmFunction.c` with the interrupt-endpoint case deleted (EEM
has none) and the alt-setting probe deleted (EEM has no alt settings):

```c
VOID
GetEndpoint (
  IN     EFI_USB_IO_PROTOCOL  *UsbIo,
  IN OUT USB_ETHERNET_DRIVER  *UsbEthDriver
  )
{
  EFI_STATUS                    Status;
  UINT8                         Index;
  EFI_USB_INTERFACE_DESCRIPTOR  Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR   Endpoint;

  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &Interface);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Interface.NumEndpoints; Index++) {
    Status = UsbIo->UsbGetEndpointDescriptor (UsbIo, Index, &Endpoint);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((Endpoint.Attributes & (BIT0 | BIT1)) != USB_ENDPOINT_BULK) {
      continue;
    }

    if ((Endpoint.EndpointAddress & BIT7) != 0) {
      UsbEthDriver->BulkInEndpoint = Endpoint.EndpointAddress;
    } else {
      UsbEthDriver->BulkOutEndpoint = Endpoint.EndpointAddress;
    }
  }
}
```

Receive -- one frame per call, cursor across calls:

```c
EFI_STATUS
EFIAPI
UsbEthEemReceive (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN OUT VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  )
{
  EFI_STATUS           Status;
  USB_ETHERNET_DRIVER  *UsbEthDriver;
  UINT32               TransStatus;
  UINTN                BulkDataLength;
  CONST UINT8          *Frame;
  UINTN                FrameLen;

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);

  if (UsbEthDriver->BulkInEndpoint == 0) {
    GetEndpoint (UsbEthDriver->UsbIo, UsbEthDriver);
  }

  //
  // Refill only when the previous transfer is walked out.
  //
  if (UsbEthDriver->RxOffset >= UsbEthDriver->RxLength) {
    BulkDataLength           = USB_EEM_MAX_BULK_SIZE;
    UsbEthDriver->RxOffset   = 0;
    UsbEthDriver->RxLength   = 0;

    Status = UsbEthDriver->UsbIo->UsbBulkTransfer (
                                    UsbEthDriver->UsbIo,
                                    UsbEthDriver->BulkInEndpoint,
                                    UsbEthDriver->RxBuffer,
                                    &BulkDataLength,
                                    USB_ETHERNET_BULK_TIMEOUT,
                                    &TransStatus
                                    );
    if (EFI_ERROR (Status)) {
      //
      // An empty poll is the normal case on an idle link, not a fault.
      //
      return (Status == EFI_TIMEOUT) ? EFI_NOT_READY : Status;
    }

    UsbEthDriver->RxLength = BulkDataLength;
  }

  Status = UsbEemNextRxFrame (
             UsbEthDriver->RxBuffer,
             UsbEthDriver->RxLength,
             &UsbEthDriver->RxOffset,
             &Frame,
             &FrameLen
             );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_READY;
  }

  if (FrameLen > *PacketLength) {
    return EFI_NOT_READY;
  }

  CopyMem (Packet, Frame, FrameLen);
  *PacketLength = FrameLen;

  return EFI_SUCCESS;
}
```

Transmit:

```c
EFI_STATUS
EFIAPI
UsbEthEemTransmit (
  IN     PXE_CDB                      *Cdb,
  IN     EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN     VOID                         *Packet,
  IN OUT UINTN                        *PacketLength
  )
{
  EFI_STATUS           Status;
  USB_ETHERNET_DRIVER  *UsbEthDriver;
  UINT32               TransStatus;
  UINTN                WrappedLength;

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);

  if (UsbEthDriver->BulkOutEndpoint == 0) {
    GetEndpoint (UsbEthDriver->UsbIo, UsbEthDriver);
  }

  Status = UsbEemFrameTx (
             UsbEthDriver->TxBuffer,
             USB_EEM_MAX_BULK_SIZE,
             (CONST UINT8 *)Packet,
             *PacketLength,
             &WrappedLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UsbEthDriver->UsbIo->UsbBulkTransfer (
                                  UsbEthDriver->UsbIo,
                                  UsbEthDriver->BulkOutEndpoint,
                                  UsbEthDriver->TxBuffer,
                                  &WrappedLength,
                                  USB_ETHERNET_TRANSFER_TIMEOUT,
                                  &TransStatus
                                  );
  return Status;
}
```

The stubs the missing notification endpoint forces:

```c
/**
  EEM has no notification interface, so there is no interrupt endpoint to
  arm and no link-state or speed change will ever be reported. Succeed
  silently: NetworkCommon calls this under ASSERT_EFI_ERROR, so returning
  EFI_UNSUPPORTED would assert every DEBUG boot.

  This is also how the contract's two consequences are satisfied, and both
  are satisfied by inaction rather than by code:

    * Link-up is assumed on enumeration -- NetworkCommon patch 0100 already
      defaults CableDetect to 1 and makes it sticky, so nothing here waits
      for a NETWORK_CONNECTION notification that can never arrive.
    * The link speed is fixed -- ECM and NCM only ever change the reported
      speed on a CONNECTION_SPEED_CHANGE notification, so a driver that
      never delivers one reports a constant speed for the life of the link.
**/
EFI_STATUS
EFIAPI
UsbEthEemInterrupt (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN BOOLEAN                      IsNewTransfer,
  IN UINTN                        PollingInterval,
  IN EFI_USB_DEVICE_REQUEST       *Request
  )
{
  return EFI_SUCCESS;
}

/**
  The station address comes from a build-time PCD: EEM carries no Ethernet
  functional descriptor, so unlike ECM/NCM there is no iMACAddress to read.
  The BMC identifies the Redfish host interface by this address, so it is a
  contract value, not a discovered one.
**/
EFI_STATUS
EFIAPI
GetUsbEthMacAddress (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS              *MacAddress
  )
{
  USB_ETHERNET_DRIVER  *UsbEthDriver;

  if ((This == NULL) || (MacAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  UsbEthDriver = USB_EEM_DEV_FROM_THIS (This);
  CopyMem (MacAddress, &UsbEthDriver->MacAddress, sizeof (EFI_MAC_ADDRESS));

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UsbEthEemBulkSize (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT UINTN                        *BulkSize
  )
{
  if (BulkSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *BulkSize = USB_EEM_MAX_FRAME;
  return EFI_SUCCESS;
}

/**
  Synthesized: EEM sends no CDC Ethernet functional descriptor at all.

  NumberMcFilters is reported as 0 deliberately. PxeFunction.c branches on
  it, and zero selects the RECEIVE_FILTER_ALL_MULTICAST path, which never
  calls SetUsbEthMcastFilter. A point-to-point link has no use for hardware
  multicast filtering.
**/
EFI_STATUS
EFIAPI
GetUsbEthFunDescriptor (
  IN  EDKII_USB_ETHERNET_PROTOCOL  *This,
  OUT USB_ETHERNET_FUN_DESCRIPTOR  *UsbEthFunDescriptor
  )
{
  if (UsbEthFunDescriptor == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (UsbEthFunDescriptor, sizeof (USB_ETHERNET_FUN_DESCRIPTOR));

  UsbEthFunDescriptor->FunctionLength = sizeof (USB_ETHERNET_FUN_DESCRIPTOR);
  //
  // 0x24 is CS_INTERFACE. The pinned tree defines ETHERNET_FUN_DESCRIPTOR
  // (0x0F) but has no CS_INTERFACE macro, and nothing re-parses this as a
  // real USB descriptor -- NetworkCommon reads only NumberMcFilters -- so the
  // literal is honest rather than inventing a macro upstream does not have.
  //
  UsbEthFunDescriptor->DescriptorType    = 0x24;
  UsbEthFunDescriptor->DescriptorSubtype = ETHERNET_FUN_DESCRIPTOR;
  UsbEthFunDescriptor->MaxSegmentSize     = USB_EEM_MAX_FRAME;
  UsbEthFunDescriptor->NumberMcFilters    = 0;
  UsbEthFunDescriptor->NumberPowerFilters = 0;

  return EFI_SUCCESS;
}

/**
  There is no control interface to send SetEthernetPacketFilter to, and the
  peer is a point-to-point gadget that filters nothing. Accept and ignore.
**/
EFI_STATUS
EFIAPI
SetUsbEthPacketFilter (
  IN EDKII_USB_ETHERNET_PROTOCOL  *This,
  IN UINT16                       Value
  )
{
  return EFI_SUCCESS;
}
```

`ETHERNET_FUN_DESCRIPTOR` (0x0F) is defined in
`Protocol/UsbEthernetProtocol.h`; `CS_INTERFACE` is not defined anywhere in
MdePkg or MdeModulePkg, which is why the literal above is used instead.

- [ ] **Step 3: Write `UsbCdcEem.c` -- driver binding**

Model it on `UsbCdcEcm/UsbCdcEcm.c`, with these differences, all of which
follow from EEM being a single interface. The entry point is `UsbEemEntry`
(the name `UsbCdcEem.inf` declares), and it installs `gUsbEemDriverBinding`
via `EfiLibInstallDriverBindingComponentName2` exactly as `UsbEcmEntry` does:

- `IsSupportedDevice` tests `InterfaceClass == USB_CDC_CLASS &&
  InterfaceSubClass == USB_CDC_EEM_SUBCLASS &&
  InterfaceProtocol == USB_CDC_EEM_PROTOCOL`.
- **Delete** `IsSameDevice`, `IsUsbCdcData`, the `UsbCdcDataHandle` field and
  the `RegisterProtocolNotify` retry path. There is no second interface to
  pair with, so `Start` never has to defer.
- `Start` allocates `RxBuffer` and `TxBuffer` at `USB_EEM_MAX_BULK_SIZE`
  each, reads the MAC from the PCD into `UsbEthDriver->MacAddress`, and
  fails cleanly if the PCD is unset:

```c
  MacPcd = (UINT8 *)PcdGetPtr (PcdUsbCdcEemMacAddress);
  MacLen = PcdGetSize (PcdUsbCdcEemMacAddress);

  //
  // An all-zero or wrong-sized PCD means the platform never set the station
  // address. A NIC with an invalid MAC would fail Redfish discovery anyway,
  // so decline to bind: a missing interface is diagnosable, a mystery MAC
  // is not.
  //
  if ((MacPcd == NULL) || (MacLen < USB_EEM_MAC_LEN)) {
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }

  ZeroMem (&UsbEthDriver->MacAddress, sizeof (EFI_MAC_ADDRESS));
  CopyMem (&UsbEthDriver->MacAddress, MacPcd, USB_EEM_MAC_LEN);

  if (IsZeroBuffer (&UsbEthDriver->MacAddress, USB_EEM_MAC_LEN)) {
    Status = EFI_UNSUPPORTED;
    goto ErrorExit;
  }
```

- The protocol vtable assignment, with every member `NetworkCommon` calls
  unconditionally filled and the rest left NULL:

```c
  UsbEthDriver->Signature                = USB_EEM_SIGNATURE;
  UsbEthDriver->NumOfInterface           = Interface.InterfaceNumber;
  UsbEthDriver->UsbIo                    = UsbIo;
  UsbEthDriver->UsbEth.UsbEthReceive     = UsbEthEemReceive;
  UsbEthDriver->UsbEth.UsbEthTransmit    = UsbEthEemTransmit;
  UsbEthDriver->UsbEth.UsbEthInterrupt   = UsbEthEemInterrupt;
  UsbEthDriver->UsbEth.UsbEthMacAddress  = GetUsbEthMacAddress;
  UsbEthDriver->UsbEth.UsbEthMaxBulkSize = UsbEthEemBulkSize;
  UsbEthDriver->UsbEth.UsbEthFunDescriptor = GetUsbEthFunDescriptor;
  UsbEthDriver->UsbEth.SetUsbEthPacketFilter = SetUsbEthPacketFilter;
```

- `Stop` frees `RxBuffer`, `TxBuffer`, `Config` and the driver struct, and
  uninstalls `gEdkIIUsbEthProtocolGuid`.

- [ ] **Step 4: Write `ComponentName.c` and `UsbCdcEem.inf`**

`ComponentName.c`: copy `UsbCdcEcm/ComponentName.c`, rename
`gUsbEcmComponentName*` to `gUsbEemComponentName*`, and set the driver name
string to `L"USB CDC EEM Driver"`.

`UsbCdcEem.inf`:

```
## @file
#   USB CDC Ethernet Emulation Model (EEM) driver for the DXE phase.
#
# Copyright (c) 2026, appkins. All rights reserved.<BR>
# SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  INF_VERSION                    = 0x00010005
  BASE_NAME                      = UsbCdcEem
  FILE_GUID                      = 2ce7bcaf-b1e5-458a-b6ea-048292c45e1f
  MODULE_TYPE                    = UEFI_DRIVER
  VERSION_STRING                 = 1.0
  ENTRY_POINT                    = UsbEemEntry

[Sources]
  UsbCdcEem.c
  UsbCdcEem.h
  UsbEemFraming.c
  UsbEemFraming.h
  UsbEemFunction.c
  ComponentName.c

[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec

[LibraryClasses]
  UefiDriverEntryPoint
  UefiBootServicesTableLib
  UefiLib
  DebugLib
  UefiUsbLib
  MemoryAllocationLib
  BaseMemoryLib
  PcdLib

[Protocols]
  gEfiUsbIoProtocolGuid
  gEfiDevicePathProtocolGuid
  gEfiDriverBindingProtocolGuid
  gEdkIIUsbEthProtocolGuid

[Pcd]
  gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress
```

- [ ] **Step 5: Re-run the framing tests and the formatter**

```bash
./hack/usbcdceem/test/run.sh
./hack/format-edk2.sh meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem
./hack/format-edk2.sh --check meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem
```

Expected: tests pass, `--check` exits 0. The driver does not compile on its
own yet -- it is compiled for the first time by Task 3's firmware build,
which is that task's gate.

- [ ] **Step 6: Commit**

```bash
git add meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem
git commit -m "feat(usbcdceem): EDK2 CDC-EEM class driver module"
```

---

### Task 3: rpi5 integration and first real build

**Files:**
- Create: `hack/usbcdceem/gen-patch.py`
- Create: `meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch`
- Modify: `meta-rpi5-uefi/recipes-bsp/edk2/edk2_git.bb` (SRC_URI + header comment)
- Modify: `meta-rpi5-uefi/recipes-bsp/rpi5-uefi-firmware/files/usbnet-dsc-snippet.inc`
- Modify: `meta-rpi5-uefi/recipes-bsp/rpi5-uefi-firmware/files/usbnet-fdf-snippet.fdf.inc`
- Modify: `meta-rpi5-uefi/recipes-bsp/rpi5-uefi-firmware/rpi5-uefi-firmware.bb`

**Interfaces:**
- Consumes: the driver sources from Tasks 1-2.
- Produces: `0110-...patch`, reused verbatim as the body of the nuc patch in Task 4.

- [ ] **Step 1: Write the patch generator**

`hack/usbcdceem/gen-patch.py`. It must emit **CRLF** bodies: edk2 sources are
CRLF, and a patch with LF bodies will apply but leave the tree inconsistent
with every neighbouring file.

```python
#!/usr/bin/env python3
"""Render the UsbCdcEem add-files quilt patch.

The driver sources are stored LF in git (repo convention, .editorconfig) but
edk2's tree is CRLF, so the patch body is emitted CRLF. Written in bytes mode
throughout: a text-mode editor would silently normalise the line endings and
the patch would then fight every other file in MdeModulePkg.

Usage: gen-patch.py <output.patch>
"""
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "meta-rpi5-uefi/recipes-bsp/edk2/files/UsbCdcEem"
DEST = "MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem"

FILES = [
    "UsbCdcEem.c",
    "UsbCdcEem.h",
    "UsbCdcEem.inf",
    "UsbEemFraming.c",
    "UsbEemFraming.h",
    "UsbEemFunction.c",
    "ComponentName.c",
]

HEADER = b"""From: appkins <nbatkins@gmail.com>
Date: Tue, 1 Sep 2026 12:00:00 -0500
Subject: [PATCH] MdeModulePkg: add a USB CDC-EEM class driver

edk2 ships UsbCdcEcm, UsbCdcNcm and UsbRndis, and none of them binds
CDC-EEM (class 02, subclass 0C, protocol 07). The BMC's Redfish host
interface NIC is an EEM gadget, because EEM has no notification interface
and so costs one device IN endpoint where the other three cost two -- the
difference that lets a NIC and a CDC-ACM console share a six-endpoint
core.

The driver produces gEdkIIUsbEthProtocolGuid like its siblings, so the
existing NetworkCommon UNDI binds it unchanged. EEM carries no Ethernet
functional descriptor, so the station address comes from
PcdUsbCdcEemMacAddress rather than iMACAddress, and the driver declines to
bind when that PCD is unset.

Upstream-Status: Pending

---
"""


def diff_new_file(rel: str, data: bytes) -> bytes:
    body = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    lines = body.split(b"\r\n")
    if lines and lines[-1] == b"":
        lines.pop()
    out = [
        b"diff --git a/%s b/%s" % (rel.encode(), rel.encode()),
        b"new file mode 100644",
        b"--- /dev/null",
        b"+++ b/%s" % rel.encode(),
        b"@@ -0,0 +1,%d @@" % len(lines),
    ]
    out += [b"+" + line for line in lines]
    return b"\r\n".join(out) + b"\r\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    chunks = [HEADER.replace(b"\n", b"\r\n")]
    for name in FILES:
        path = SRC / name
        chunks.append(diff_new_file(f"{DEST}/{name}", path.read_bytes()))

    Path(sys.argv[1]).write_bytes(b"".join(chunks))
    print(f"wrote {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

The `MdeModulePkg.dec` PCD declaration is a *modification*, not a new file,
so the generator emits it as a context hunk rather than an add-files hunk.
Append this constant and call to `gen-patch.py`, so the whole patch stays
generated and neither repo's copy is ever hand-edited:

```python
# Declares the station address PCD. A context hunk, not an add-files hunk:
# it modifies a file that already exists. The three context lines below are
# the tail of PcdUsbNetworkPeriodicTimerInterval's block in [PcdsFixedAtBuild].
# If a future SRCREV moves them, `git apply --check` fails loudly and the
# context is refreshed from that repo's own tree -- it is never silently
# fuzzed.
DEC_HUNK = b"""diff --git a/MdeModulePkg/MdeModulePkg.dec b/MdeModulePkg/MdeModulePkg.dec
--- a/MdeModulePkg/MdeModulePkg.dec
+++ b/MdeModulePkg/MdeModulePkg.dec
@@ -1250,6 +1250,14 @@
   # @Prompt USB Network periodic timer interval for asynchronous transfers in ms.
   # @ValidRange 0x80000001 | 1 - 255
   gEfiMdeModulePkgTokenSpaceGuid.PcdUsbNetworkPeriodicTimerInterval|16|UINT8|0x30001063
+
+  ## Station address for the USB CDC-EEM NIC.
+  #  CDC-EEM carries no Ethernet functional descriptor, so unlike ECM and NCM
+  #  there is no iMACAddress to read the address from. All zeroes means the
+  #  platform has not set it, and UsbCdcEem then declines to bind rather than
+  #  present a NIC with an invalid station address.
+  # @Prompt USB CDC-EEM station address.
+  gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress|{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}|VOID*|0x30001064
 
 [PcdsFixedAtBuild, PcdsPatchableInModule]
   ## Dynamic type PCD can be registered callback function for Pcd setting action.
"""
```

and in `main()`, after the per-file loop:

```python
    chunks.append(DEC_HUNK.replace(b"\n", b"\r\n"))
```

Verify the context matches each repo's tree before relying on it:

```bash
git -C <that repo's fetched edk2 tree> apply --check <the generated patch>
```

- [ ] **Step 2: Generate the patch and verify it applies**

```bash
chmod +x hack/usbcdceem/gen-patch.py
./hack/usbcdceem/gen-patch.py \
  meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch

# CRLF check: every body line must end CR LF.
file meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch
```

Expected: `file` reports `CRLF line terminators`.

- [ ] **Step 3: Wire the recipe and snippets**

`usbnet-dsc-snippet.inc` -- add after the `UsbCdcNcm` line:

```
  MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/UsbCdcEem.inf
```

`usbnet-fdf-snippet.fdf.inc` -- add after the `UsbCdcNcm` line:

```
  INF MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/UsbCdcEem.inf
```

Update both files' header comments: they currently say the BMC link "rides
the ncm.usb0 function". It rides `eem.usb0` now.

`edk2_git.bb` -- add to SRC_URI after the 0109 line:

```
           file://0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch \
```

and extend the ordering note in the header comment to describe 0110.

`rpi5-uefi-firmware.bb` -- inside the existing `redfish_marker` block, which
already computes and validates `mac_bytes`, add one line after the
`PcdRpiRedfishGadgetMac` line:

```sh
        printf '  gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress|{%s}\n' "${mac_bytes}" >> "${dsc}"
```

Also refresh the NCM wording in that file's comments (lines ~100-105, ~519,
~547 and the `RPI5_REDFISH_MAC` comment).

- [ ] **Step 4: Build the rpi5 firmware**

```bash
# From the repo root, using this repo's normal kas/bitbake entry point.
bitbake rpi5-uefi-firmware
```

Expected: builds green. The EEM driver compiles for the first time here --
expect to iterate on Task 2's sources until it does, re-running
`./hack/usbcdceem/test/run.sh` and regenerating the patch after each change:

```bash
./hack/usbcdceem/gen-patch.py \
  meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch
```

Do **not** verify by grepping `strings` on the RELEASE output: `DEBUG()` is
compiled out and driver names may not appear. The bitbake exit status and the
presence of `UsbCdcEem` in the build's FV report are the checks.

- [ ] **Step 5: Confirm the driver is in the firmware volume**

```bash
find build -name 'FV.inf' -o -name '*.Fv.txt' | head
grep -rl "UsbCdcEem" build/tmp/work/*/rpi5-uefi-firmware/*/build/ 2>/dev/null | head
```

Expected: `UsbCdcEem` appears in the generated FV map, confirming it was
built into FVMAIN and not merely compiled.

- [ ] **Step 6: Commit**

```bash
git add hack/usbcdceem/gen-patch.py \
        meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch \
        meta-rpi5-uefi/recipes-bsp/edk2/edk2_git.bb \
        meta-rpi5-uefi/recipes-bsp/rpi5-uefi-firmware/
git commit -m "feat(rpi5): build the CDC-EEM class driver into the firmware"
```

---

### Task 4: nuc integration

**Repo:** `/home/appkins/src/pi-bmc/nuc-bios-build`

**Files:**
- Create: `meta-nuc-bios/recipes-bsp/edk2/files/0032-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch`
- Modify: `meta-nuc-bios/recipes-bsp/edk2/files/0030-UefiPayloadPkg-build-all-three-USB-CDC-network-class.patch`
- Modify: `meta-nuc-bios/recipes-bsp/edk2/edk2-uefipayload_2605.bb`
- Modify: `meta-nuc-bios/recipes-bsp/edk2/files/NucRedfishPkg/NucRedfish.dsc`
- Modify: `meta-nuc-bios/recipes-bsp/edk2/files/NucRedfishPkg/deploy/install-drivers.nsh`

**Interfaces:**
- Consumes: the generated patch body from Task 3 (identical driver sources).

- [ ] **Step 1: Copy the generated patch into the nuc repo**

The driver body is byte-identical; only the filename differs.

```bash
cp /home/appkins/src/pi-bmc/rpi5-uefi-build/meta-rpi5-uefi/recipes-bsp/edk2/files/0110-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch \
   meta-nuc-bios/recipes-bsp/edk2/files/0032-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch
file meta-nuc-bios/recipes-bsp/edk2/files/0032-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch
```

Expected: `CRLF line terminators`.

- [ ] **Step 2: Add the patch to SRC_URI**

In `edk2-uefipayload_2605.bb`, after the `0031-UsbCdcNcm-...` line:

```
    '0032-MdeModulePkg-add-a-USB-CDC-EEM-class-driver.patch', \
```

- [ ] **Step 3: Extend patch 0030's DSC/FDF hunks**

Patch 0030 currently adds NCM and RNDIS. Add EEM to both hunks. Because 0030
is itself a patch file with CRLF bodies, edit it in bytes mode:

```bash
python3 - <<'PY'
from pathlib import Path
p = Path("meta-nuc-bios/recipes-bsp/edk2/files/0030-UefiPayloadPkg-build-all-three-USB-CDC-network-class.patch")
b = p.read_bytes()

dsc_old = b"+  MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis/UsbRndis.inf\r\n"
dsc_new = dsc_old + b"+  MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/UsbCdcEem.inf\r\n"

fdf_old = b"+  INF MdeModulePkg/Bus/Usb/UsbNetwork/UsbRndis/UsbRndis.inf\r\n"
fdf_new = fdf_old + b"+  INF MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/UsbCdcEem.inf\r\n"

assert b.count(dsc_old) == 1, "expected exactly one DSC UsbRndis line"
assert b.count(fdf_old) == 1, "expected exactly one FDF UsbRndis line"

b = b.replace(dsc_old, dsc_new).replace(fdf_old, fdf_new)
p.write_bytes(b)
print("patched 0030")
PY
```

The `@@` hunk line counts must be corrected after this insertion -- each hunk
gains one added line, so bump the second number of each `@@ -x,y +a,b @@` by
one. Verify by dry-run applying against the fetched tree:

```bash
git -C build/tmp/work/corei7-64-poky-linux/edk2-uefipayload/2605+git/git \
    apply --check /full/path/to/0030-...patch
```

Expected: no output (success). If it reports "corrupt patch", the hunk counts
are wrong.

- [ ] **Step 4: Add the component and PCD to `NucRedfish.dsc`**

In `[Components]`, after the `UsbCdcNcm.inf` line:

```
  MdeModulePkg/Bus/Usb/UsbNetwork/UsbCdcEem/UsbCdcEem.inf
```

In the `[PcdsFixedAtBuild]` section that already carries
`PcdRedfishRestExServiceDevicePath`, add -- keeping it adjacent to the MAC it
must match:

```
  gEfiMdeModulePkgTokenSpaceGuid.PcdUsbCdcEemMacAddress|{0xDA, 0xC0, 0xFF, 0xEE, 0x10, 0x02}
```

Update the `[Components]` section comment: it says "CDC-ECM/RNDIS/NCM class
bindings", which is now also EEM.

- [ ] **Step 5: Register the driver in `install-drivers.nsh`**

The entries are sequentially numbered, so inserting EEM into the USB group
renumbers everything after it. Replace the USB group and shift the rest by
one:

```
#  --- USB-Ethernet transport + SNP (must come first) ---
bcfg driver add 00 NetworkCommon.efi                    "NucRfsh:NetworkCommon"
bcfg driver add 01 UsbCdcEem.efi                        "NucRfsh:UsbCdcEem"
bcfg driver add 02 UsbCdcEcm.efi                        "NucRfsh:UsbCdcEcm"
bcfg driver add 03 UsbRndis.efi                         "NucRfsh:UsbRndis"
bcfg driver add 04 UsbCdcNcm.efi                        "NucRfsh:UsbCdcNcm"
```

Then renumber `DpcDxe` from 04 to 05 and every subsequent entry by +1, ending
with `RedfishBootOption_V1_0_4_Dxe.efi` at 34. EEM goes first in the group
because it is the transport this BMC actually presents.

Verify no number is duplicated or skipped:

```bash
grep -oE '^bcfg driver add [0-9]+' meta-nuc-bios/recipes-bsp/edk2/files/NucRedfishPkg/deploy/install-drivers.nsh \
  | awk '{print $4}' | sort -n | uniq -d
grep -c '^bcfg driver add' meta-nuc-bios/recipes-bsp/edk2/files/NucRedfishPkg/deploy/install-drivers.nsh
```

Expected: the first command prints nothing (no duplicates); the second prints
`35` (00 through 34).

- [ ] **Step 6: Build the nuc payload**

```bash
bitbake edk2-uefipayload
```

Expected: builds green, with `UsbCdcEem.efi` among the produced drivers.

- [ ] **Step 7: Commit**

```bash
git add meta-nuc-bios/recipes-bsp/edk2/
git commit -m "feat(nuc): build the CDC-EEM class driver into the payload"
```

---

### Task 5: Hardware validation

**Files:** none -- this task changes nothing; it is the acceptance gate.

**Interfaces:**
- Consumes: firmware images from Tasks 3 and 4.

Neither repo has a host-side harness that can exercise the USB stack, so the
driver's real behaviour is only observable on hardware. Validate on the RPi5
first: it is the board with a working Redfish sync today, so a regression
there is unambiguous.

- [ ] **Step 1: Flash the rpi5 firmware and confirm the NIC enumerates**

Boot with the BMC's gadget in EEM mode (its default since `nanokvm-app`
`1b003d7`). At the UEFI shell:

```
devices
ifconfig -l
```

Expected: an SNP interface exists with station address `DA:C0:FF:EE:10:02`.
If no interface appears, the most likely causes in order are: the MAC PCD
never reached the DSC (check the generated `[PcdsFixedAtBuild.common]` block
at the end of the built DSC), the gadget is not in EEM mode, or
`Supported` is rejecting the interface triple.

- [ ] **Step 2: Confirm the link carries IP**

```
ifconfig -s eth0 dhcp
ping 169.254.10.1
```

Expected: the host leases `169.254.10.2` and pings the BMC. A lease that
never completes, or pings that time out in one direction only, points at the
framing -- most likely the sentinel byte order, which drops frames silently
with no error on either side.

- [ ] **Step 3: Confirm Redfish discovery and a full sync**

Boot normally and check the BMC's view:

```bash
curl -s http://169.254.10.2/redfish/v1/Systems | head
```

from the BMC side, and confirm the host completed a `SoftwareInventory`
PATCH by reading `/redfish/v1/UpdateService/FirmwareInventory/` on the BMC.

Expected: `RedfishDiscoverDxe` selected the interface and a sync completed.

- [ ] **Step 4: Repeat on the NUC**

Register the drivers from the USB volume per `install-drivers.nsh`, reboot,
and repeat steps 1-3.

- [ ] **Step 5: Record the result**

If hardware behaviour differs from the framing the unit tests assert, the
tests are wrong before the hardware is -- add the failing vector to
`hack/usbcdceem/test/test-framing.c` first, watch it fail, then fix
`UsbEemFraming.c`. Regenerate both patches and rebuild after any change.

---

## Notes for the executor

- **Never hand-edit a `.patch` under `recipes-bsp/edk2*/files/` with an
  editor tool.** They carry CRLF bodies; a text-mode write silently
  normalises them. Use bytes-mode Python, as Task 4 Step 3 shows.
- **`0110` and `0032` must stay byte-identical below the header.** They are
  generated from one source tree by `gen-patch.py`; if you change the driver,
  regenerate both, never edit one.
- **A green build is not a working driver.** Only Task 5 can tell you the
  wire format is right.

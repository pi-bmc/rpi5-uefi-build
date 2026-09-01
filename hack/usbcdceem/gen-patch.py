#!/usr/bin/env python3
"""Render the UsbCdcEem add-files quilt patch.

The driver sources are stored LF in git (repo convention, .editorconfig) but
edk2's tree is CRLF, so the patch BODY is emitted CRLF. Diff *metadata*
lines -- `diff --git`, `---`, `+++`, `@@ ... @@` -- and the commit-message
prose stay LF: that is what every other patch in this series does (checked
against 0107-UsbCdcNcm-..., which applies cleanly today and is CRLF only on
context/`+`/`-` body lines, LF everywhere else), and it is what `quilt push`
expects -- a CRLF `@@` header made Hunk 1 of the .dec context hunk fail with
"different line endings" even though the context text itself matched
byte-for-byte.

Written in bytes mode throughout: a text-mode editor would silently
normalise the line endings and the patch would then fight every other file
in MdeModulePkg.

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


def render_hunk(lf_text: bytes) -> bytes:
    """Convert an LF-authored unified-diff hunk to edk2's mixed convention:
    everything up to and including the `@@ ... @@` line stays LF (diff
    metadata); every hunk body line after it (context/`+`/`-`) becomes CRLF,
    matching the CRLF target file the hunk patches.
    """
    lines = lf_text.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    out = []
    in_body = False
    for line in lines:
        if not in_body:
            out.append(line + b"\n")
            if line.startswith(b"@@"):
                in_body = True
        else:
            out.append(line + b"\r\n")
    return b"".join(out)


def diff_new_file(rel: str, data: bytes) -> bytes:
    text_lines = data.replace(b"\r\n", b"\n").split(b"\n")
    if text_lines and text_lines[-1] == b"":
        text_lines.pop()
    lf_text = b"\n".join(
        [
            b"diff --git a/%s b/%s" % (rel.encode(), rel.encode()),
            b"new file mode 100644",
            b"--- /dev/null",
            b"+++ b/%s" % rel.encode(),
            b"@@ -0,0 +1,%d @@" % len(text_lines),
        ]
        + [b"+" + line for line in text_lines]
    ) + b"\n"
    return render_hunk(lf_text)


# Declares the station address PCD. A context hunk, not an add-files hunk:
# it modifies a file that already exists. The three context lines below are
# the tail of PcdUsbNetworkPeriodicTimerInterval's block in [PcdsFixedAtBuild].
# If a future SRCREV moves them, `git apply --check` fails loudly and the
# context is refreshed from that repo's own tree -- it is never silently
# fuzzed. Authored here as plain LF; render_hunk() applies the mixed
# LF-metadata/CRLF-body convention this file's docstring explains.
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
\x20
 [PcdsFixedAtBuild, PcdsPatchableInModule]
   ## Dynamic type PCD can be registered callback function for Pcd setting action.
"""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    chunks = [HEADER]
    for name in FILES:
        path = SRC / name
        chunks.append(diff_new_file(f"{DEST}/{name}", path.read_bytes()))

    chunks.append(render_hunk(DEC_HUNK))

    Path(sys.argv[1]).write_bytes(b"".join(chunks))
    print(f"wrote {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

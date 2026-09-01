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

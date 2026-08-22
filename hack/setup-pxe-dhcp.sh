#!/usr/bin/env bash
# Make the nanokvm-share dnsmasq instance PXE-capable so the RPi 5 UEFI
# firmware's PxeBc accepts its DHCP offers.
#
#   sudo hack/setup-pxe-dhcp.sh
#
# Why: EDK2's UefiPxeBcDxe discards plain DHCP offers that carry no boot
# file name (NetworkPkg/UefiPxeBcDxe/PxeBcDhcp4.c, offer-selection case 7
# "DhcpOnly offer with bootfilename"), so the board never sends a
# DHCPREQUEST and dnsmasq never ACKs. Advertising a boot file fixes the
# handshake; serving it over TFTP makes PXE boot all the way into the
# UEFI Shell as an end-to-end network-boot test.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHELL_EFI="${REPO_ROOT}/build/tmp/work/cortexa76-poky-linux-musl/edk2/202608/Build/RPi5/DEBUG_GCC/AARCH64/Shell.efi"
CONNECTION="nanokvm-share"
TFTP_ROOT="/srv/tftp"
CONF="/etc/NetworkManager/dnsmasq-shared.d/pxe.conf"

[ "$(id -u)" = "0" ] || { echo "run as root: sudo $0" >&2; exit 1; }
[ -r "${SHELL_EFI}" ] || { echo "Shell.efi not found at ${SHELL_EFI} - run the build first" >&2; exit 1; }

mkdir -p "${TFTP_ROOT}"
install -m 0644 "${SHELL_EFI}" "${TFTP_ROOT}/bootaa64.efi"

cat > "${CONF}" <<'EOF'
# PXE for the RPi 5 UEFI bring-up (hack/setup-pxe-dhcp.sh)
dhcp-boot=bootaa64.efi
enable-tftp
tftp-root=/srv/tftp
log-dhcp
EOF

nmcli connection down "${CONNECTION}"
nmcli connection up "${CONNECTION}"

sleep 2
if pgrep -af "dnsmasq.*${CONNECTION//-/.}|dnsmasq.*NetworkManager" >/dev/null; then
    echo "dnsmasq restarted with PXE config:"
    sed 's/^/    /' "${CONF}"
    echo "TFTP payload: ${TFTP_ROOT}/bootaa64.efi (UEFI Shell)"
    echo "now boot the Pi and watch: journalctl -fu NetworkManager | grep -i dnsmasq"
else
    echo "warning: no dnsmasq process found - check 'journalctl -u NetworkManager'" >&2
fi

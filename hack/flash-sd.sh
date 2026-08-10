#!/usr/bin/env bash
# Flash the built Raspberry Pi 5 UEFI SD image onto the first SD card found
# on this system.
#
#   hack/flash-sd.sh [-y] [--dry-run] [-d /dev/sdX] [-i path/to.img]
#
# "SD card" means, in preference order:
#   1. a real SD/MMC controller disk (/dev/mmcblk*, skipping the eMMC
#      boot0/boot1/rpmb side-devices), then
#   2. a removable USB disk (card reader or USB stick).
# Any disk backing a system mount (/, /boot, /home, ...) or swap is never
# considered, whatever it is. Auto-mounted partitions on the chosen card are
# unmounted before flashing. Flashing still destroys whatever is on the card
# -- hence the confirmation prompt (skip with -y).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${REPO_ROOT}/build/tmp/deploy/images/raspberrypi5-uefi/rpi5-uefi-sd.img"
DEVICE=""
ASSUME_YES=0
DRY_RUN=0

die() { echo "flash-sd: error: $*" >&2; exit 1; }

usage() {
    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)     ASSUME_YES=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        -d|--device)  DEVICE="${2:?-d needs a device}"; shift ;;
        -i|--image)   IMG="${2:?-i needs a path}"; shift ;;
        -h|--help)    usage ;;
        *)            usage 1 ;;
    esac
    shift
done

[ -r "${IMG}" ] || die "image not found: ${IMG}
run 'kas build kas.yml' first, or point -i at an image"

# Disks that hold the running system: never candidates, not even with -d.
system_disks() {
    local mp src
    for mp in / /boot /boot/efi /boot/firmware /home /usr /var; do
        src=$(findmnt -no SOURCE "${mp}" 2>/dev/null) || continue
        # Resolve partition -> whole disk (PKNAME empty when src IS the disk).
        lsblk -no PKNAME "${src}" 2>/dev/null | sed 's|^|/dev/|'
        echo "${src}"
    done
    # Swap devices too.
    lsblk -rnpo NAME,PKNAME,FSTYPE | awk '$3 == "swap" { print $1; if ($2 != "") print "/dev/" $2 }'
}

is_system_disk() {
    local dev="$1"
    system_disks | grep -qxF "${dev}"
}

find_sd() {
    local name type tran rm size
    # Pass 1: SD/MMC controller disks (skip eMMC boot0/boot1/rpmb siblings).
    # Pass 2: removable USB disks. Devices reporting 0 bytes are empty
    # card-reader slots, not cards -- skip them.
    for pass in mmc usb; do
        while read -r name type tran rm size; do
            [ "${type}" = "disk" ] || continue
            [ "${size:-0}" -gt 0 ] || continue
            case "${name}" in *boot[01]|*rpmb) continue ;; esac
            case "${pass}" in
                mmc) case "${name}" in /dev/mmcblk*) ;; *) continue ;; esac ;;
                usb) { [ "${tran}" = "usb" ] && [ "${rm}" = "1" ]; } || continue ;;
            esac
            is_system_disk "${name}" && continue
            echo "${name}"
            return 0
        done < <(lsblk -dbnpo NAME,TYPE,TRAN,RM,SIZE)
    done
    return 1
}

if [ -n "${DEVICE}" ]; then
    [ -b "${DEVICE}" ] || die "not a block device: ${DEVICE}"
    is_system_disk "${DEVICE}" && die "refusing ${DEVICE}: it backs a system mount"
    [ "$(lsblk -dbnpo SIZE "${DEVICE}")" -gt 0 ] || \
        die "refusing ${DEVICE}: it reports 0 bytes (empty card-reader slot?)"
else
    DEVICE=$(find_sd) || die "no SD card found (no non-system, non-empty /dev/mmcblk* disk or removable USB disk)
insert a card, or pass one explicitly with -d"
fi

echo "Image : ${IMG} ($(du -h "${IMG}" | cut -f1))"
echo "Target: ${DEVICE}"
lsblk -o NAME,SIZE,MODEL,TRAN,RM,MOUNTPOINTS "${DEVICE}"

if [ "${DRY_RUN}" = "1" ]; then
    echo "dry run: would flash the image above; stopping here"
    exit 0
fi

if [ "${ASSUME_YES}" != "1" ]; then
    printf "This ERASES %s. Type 'yes' to continue: " "${DEVICE}"
    read -r reply
    [ "${reply}" = "yes" ] || die "aborted"
fi

SUDO=""
[ "$(id -u)" = "0" ] || SUDO="sudo"

unmount_all() {
    local mp
    while read -r mp; do
        [ -n "${mp}" ] || continue
        echo "unmounting ${mp}"
        ${SUDO} umount "${mp}"
    done < <(lsblk -rnpo MOUNTPOINTS "${DEVICE}")
}

# Unmount anything auto-mounted from the card.
unmount_all

${SUDO} dd if="${IMG}" of="${DEVICE}" bs=4M conv=fsync oflag=direct status=progress
sync
${SUDO} blockdev --rereadpt "${DEVICE}" 2>/dev/null || true

# The partition re-read makes desktop automounters grab the fresh FAT
# partition; wait for udev to finish and unmount again so the card can be
# pulled straight away.
udevadm settle 2>/dev/null || true
sleep 2
unmount_all

echo "done: ${DEVICE} now carries the RPi 5 UEFI bootloader image"
echo "(partition 1 is FAT32 'RPI5-UEFI': armstub8-2712.bin + config.txt + DTBs + overlays)"
echo "all partitions unmounted -- safe to remove the card"

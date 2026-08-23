#!/usr/bin/env bash
# Flash the built Raspberry Pi 5 UEFI SD image onto the first SD card found
# on this system.
#
#   hack/flash-sd.sh [-y] [--dry-run] [--eject] [-d /dev/sdX] [-i path/to.img]
#
# "SD card" means, in preference order:
#   1. a real SD/MMC controller disk (/dev/mmcblk*, skipping the eMMC
#      boot0/boot1/rpmb side-devices), then
#   2. a removable USB disk (card reader or USB stick).
# Any disk backing a system mount (/, /boot, /home, ...) or swap -- through
# LUKS/LVM/btrfs stacks too -- is never considered, whatever it is, and a
# partition is never accepted as a target (pass the whole disk). The image
# is checked to fit the card before a byte is written, everything mounted
# from the card is unmounted first (the flash refuses to run if that fails),
# and afterwards the script keeps unmounting until the desktop automounter
# has let go for good before declaring the card safe to pull. --eject also
# powers off a USB reader (udisksctl) so it detaches completely. Flashing
# still destroys whatever is on the card -- hence the confirmation prompt
# (skip with -y).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG_DIR="${REPO_ROOT}/build/tmp/deploy/images/raspberrypi5-uefi"
# The build deploys an xz-compressed image (IMAGE_FSTYPES=wic.xz); flashed
# by streaming through xzcat below. An uncompressed .img (if one is present
# or passed with -i) still works.
DEFAULT_IMG="${IMG_DIR}/rpi5-uefi-sd.img.xz"
IMG="${DEFAULT_IMG}"
DEVICE=""
ASSUME_YES=0
DRY_RUN=0
EJECT=0

die()  { echo "flash-sd: error: $*" >&2; exit 1; }
warn() { echo "flash-sd: warning: $*" >&2; }

usage() {
    awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' \
        "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)     ASSUME_YES=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        --eject)      EJECT=1 ;;
        -d|--device)  DEVICE="${2:?-d needs a device}"; shift ;;
        -i|--image)   IMG="${2:?-i needs a path}"; shift ;;
        -h|--help)    usage ;;
        *)            usage 1 ;;
    esac
    shift
done

# If the default compressed image isn't there but an uncompressed one is,
# use it. (A path given with -i is honoured as-is.)
if [ "${IMG}" = "${DEFAULT_IMG}" ] && [ ! -r "${IMG}" ] && \
   [ -r "${IMG_DIR}/rpi5-uefi-sd.img" ]; then
    IMG="${IMG_DIR}/rpi5-uefi-sd.img"
fi

[ -r "${IMG}" ] || die "image not found: ${IMG}
run 'kas build kas.yml' first, or point -i at an image"

case "${IMG}" in
    *.xz) command -v xzcat >/dev/null 2>&1 || die "xzcat not found (install xz)" ;;
esac

# One lsblk column per call: multi-column -o output drops empty fields
# (TRAN is empty for mmc disks), silently shifting every later column.
blk() { lsblk -dbnro "$1" "$2" 2>/dev/null || true; }

# Every block device backing a system mount or swap: the mount sources plus
# ALL their ancestors (partition -> LUKS/LVM/RAID -> whole disk), so stacked
# setups can't hide the disk they live on. findmnt -v strips the btrfs
# "[/subvolume]" suffix that would otherwise break the lsblk lookup.
system_disks() {
    local mp src
    {
        for mp in / /boot /boot/efi /boot/firmware /home /usr /var; do
            findmnt -nvo SOURCE "${mp}" 2>/dev/null || true
        done
        awk 'NR > 1 { print $1 }' /proc/swaps 2>/dev/null || true
    } | sort -u | while read -r src; do
        [ -b "${src}" ] || continue
        lsblk -slnpo NAME "${src}" 2>/dev/null || true
    done | sort -u
}

is_system_disk() {
    # Capture first: grep -q quitting early would SIGPIPE the producer and,
    # with pipefail, could turn a match into a miss.
    local disks
    disks=$(system_disks)
    printf '%s\n' "${disks}" | grep -qxF "$1"
}

find_sd() {
    local pass dev
    # Pass 1: SD/MMC controller disks (skip eMMC boot0/boot1/rpmb siblings).
    # Pass 2: removable USB disks. Devices reporting 0 bytes are empty
    # card-reader slots, not cards -- skip them.
    for pass in mmc usb; do
        while read -r dev; do
            case "${dev}" in *boot[01]|*rpmb) continue ;; esac
            [ "$(blk TYPE "${dev}")" = "disk" ] || continue
            [ "$(blk SIZE "${dev}")" -gt 0 ] 2>/dev/null || continue
            case "${pass}" in
                mmc) case "${dev}" in /dev/mmcblk*) ;; *) continue ;; esac ;;
                usb) { [ "$(blk TRAN "${dev}")" = "usb" ] && \
                       [ "$(blk RM "${dev}")" = "1" ]; } || continue ;;
            esac
            is_system_disk "${dev}" && continue
            echo "${dev}"
            return 0
        done < <(lsblk -dnpo NAME)
    done
    return 1
}

if [ -n "${DEVICE}" ]; then
    [ -b "${DEVICE}" ] || die "not a block device: ${DEVICE}"
    case "$(blk TYPE "${DEVICE}")" in
        disk|loop) ;;
        part)
            parent=$(blk PKNAME "${DEVICE}")
            die "refusing ${DEVICE}: it is a partition -- pass the whole disk${parent:+ (/dev/${parent})}" ;;
        *)  die "refusing ${DEVICE}: not a whole disk" ;;
    esac
    is_system_disk "${DEVICE}" && die "refusing ${DEVICE}: it backs a system mount or swap"
else
    DEVICE=$(find_sd) || die "no SD card found (no non-system, non-empty /dev/mmcblk* disk or removable USB disk)
insert a card, or pass one explicitly with -d"
fi

DEV_SIZE=$(blk SIZE "${DEVICE}")
[ "${DEV_SIZE:-0}" -gt 0 ] 2>/dev/null || \
    die "refusing ${DEVICE}: it reports 0 bytes (empty card-reader slot?)"

# Uncompressed size, to prove the image fits BEFORE writing (a mid-flash
# out-of-space dd leaves an unbootable card).
case "${IMG}" in
    *.xz) IMG_SIZE=$(xz --robot --list "${IMG}" 2>/dev/null | \
                     awk '$1 == "totals" { print $5 }' || true) ;;
    *)    IMG_SIZE=$(stat -Lc %s "${IMG}" 2>/dev/null || true) ;;
esac
if [ -n "${IMG_SIZE}" ] && [ "${IMG_SIZE}" -gt "${DEV_SIZE}" ]; then
    die "image (${IMG_SIZE} bytes uncompressed) does not fit ${DEVICE} (${DEV_SIZE} bytes)"
fi

# Don't saw off the branch we sit on: refuse if the image file itself is
# stored on the target device.
IMG_SRC=$(findmnt -nvo SOURCE -T "${IMG}" 2>/dev/null || true)
if [ -n "${IMG_SRC}" ] && [ -b "${IMG_SRC}" ]; then
    IMG_ANCESTORS=$(lsblk -slnpo NAME "${IMG_SRC}" 2>/dev/null || true)
    printf '%s\n' "${IMG_ANCESTORS}" | grep -qxF "${DEVICE}" && \
        die "refusing ${DEVICE}: the image itself is stored on it"
fi

hsize() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1 bytes"; }

echo "Image : ${IMG} ($(du -h "${IMG}" | cut -f1))"
if [ -n "${IMG_SIZE}" ]; then
    echo "Writes: $(hsize "${IMG_SIZE}") onto a $(hsize "${DEV_SIZE}") card"
fi
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
# Take the sudo prompt now, not halfway through dd's progress output.
if [ -n "${SUDO}" ]; then ${SUDO} -v; fi

# Partitions (or the whole disk) currently mounted from the card. Raw mode
# escapes spaces in mountpoints, so "NF > 1" is reliable; unmounting by
# device node sidesteps mountpoint quoting entirely.
mounted_parts() {
    lsblk -rnpo NAME,MOUNTPOINT "${DEVICE}" 2>/dev/null | \
        awk 'NF > 1 { print $1 }' | sort -u || true
}

unmount_parts() {
    local part
    for part in "$@"; do
        echo "unmounting ${part}"
        ${SUDO} umount "${part}" || true
    done
}

# Unmount anything auto-mounted from the card -- and refuse to write a
# single byte while any of it is still mounted (busy filesystems).
parts=$(mounted_parts)
if [ -n "${parts}" ]; then
    # shellcheck disable=SC2086
    unmount_parts ${parts}
fi
[ -z "$(mounted_parts)" ] || \
    die "partitions on ${DEVICE} are mounted and busy; close whatever is using them and retry"

# iflag=fullblock stops dd turning short pipe reads into short writes.
# O_DIRECT keeps gigabytes of card traffic out of the page cache and makes
# the progress numbers honest, but needs sector-aligned writes -- fall back
# to cached writes for an odd-sized image. pipefail (set above) makes an
# xzcat failure abort the flash.
DIRECT="oflag=direct"
if [ -n "${IMG_SIZE}" ] && [ $((IMG_SIZE % 512)) -ne 0 ]; then
    DIRECT=""
fi
case "${IMG}" in
    *.xz) xzcat -- "${IMG}" | ${SUDO} dd of="${DEVICE}" bs=4M iflag=fullblock conv=fsync ${DIRECT} status=progress ;;
    *)    ${SUDO} dd if="${IMG}" of="${DEVICE}" bs=4M iflag=fullblock conv=fsync ${DIRECT} status=progress ;;
esac
sync

# Make the kernel pick up the new partition table. If the automounter
# grabbed a partition in the meantime the re-read fails EBUSY; drop the
# mount and try again.
for _ in 1 2 3; do
    if ${SUDO} blockdev --rereadpt "${DEVICE}" 2>/dev/null; then
        break
    fi
    parts=$(mounted_parts)
    if [ -n "${parts}" ]; then
        # shellcheck disable=SC2086
        unmount_parts ${parts}
    fi
    sleep 1
done
udevadm settle 2>/dev/null || true

# Sanity check: a successful flash leaves a visible partition table.
NPARTS=$(lsblk -rnpo TYPE "${DEVICE}" 2>/dev/null | grep -cx part || true)
if [ "${NPARTS:-0}" -eq 0 ]; then
    warn "kernel sees no partitions on ${DEVICE} after flashing"
    warn "the write may not have taken -- re-insert the card and check it"
else
    lsblk -o NAME,SIZE,FSTYPE,LABEL "${DEVICE}" 2>/dev/null || true
fi

# udisks reacts to the new partitions asynchronously over D-Bus, AFTER
# 'udevadm settle' has already returned -- a one-shot unmount or a fixed
# sleep loses that race and the card ends up mounted again right as the
# script claims it is safe to pull. Keep unmounting until the card has
# stayed unmounted for 3 consecutive seconds; give up after ~30.
wait_unmounted() {
    local quiet=0 waited=0 parts
    while [ "${waited}" -lt 30 ]; do
        parts=$(mounted_parts)
        if [ -n "${parts}" ]; then
            # shellcheck disable=SC2086
            unmount_parts ${parts}
            quiet=0
        else
            quiet=$((quiet + 1))
            if [ "${quiet}" -ge 3 ]; then return 0; fi
        fi
        sleep 1
        waited=$((waited + 1))
    done
    [ -z "$(mounted_parts)" ]
}

if ! wait_unmounted; then
    warn "${DEVICE} is still mounted -- something keeps re-mounting or holding it"
    warn "unmount it yourself before pulling the card, e.g.: sudo umount ${DEVICE}* "
    exit 1
fi

if [ "${EJECT}" = "1" ]; then
    if [ "$(blk TRAN "${DEVICE}")" = "usb" ] && command -v udisksctl >/dev/null 2>&1; then
        if udisksctl power-off -b "${DEVICE}" >/dev/null 2>&1 || \
           ${SUDO} udisksctl power-off -b "${DEVICE}" >/dev/null 2>&1; then
            echo "powered off ${DEVICE} -- replug the reader to use it again"
        else
            warn "could not power off ${DEVICE}; it is unmounted, just pull it"
        fi
    else
        warn "--eject only powers off USB readers (via udisksctl); the card is unmounted, just pull it"
    fi
fi

echo "done: ${DEVICE} now carries the RPi 5 UEFI bootloader image"
echo "(partition 1 is FAT32 'RPI5-UEFI': armstub8-2712.bin + config.txt + DTBs + overlays)"
echo "all partitions unmounted -- safe to remove the card"

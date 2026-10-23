#!/bin/bash
# Flash a Korvo-1 SD image to an explicitly selected external disk (Linux).
# Linux counterpart of tools/flash-sd.sh (which uses diskutil).
# Usage: tools/flash-sd-linux.sh /dev/sdX path/to/sdcard.img
set -e
set -o pipefail

if [ "$#" -ne 2 ]; then
    printf 'usage: %s /dev/sdX sdcard.img\n' "$0" >&2
    exit 2
fi

DISK=$1
IMAGE=$2
case "$DISK" in
    /dev/sd[a-z]|/dev/sd[a-z][a-z]|/dev/nvme[0-9]n[0-9]|/dev/mmcblk[0-9])
        ;;
    *) printf 'refusing non-whole-disk target: %s\n' "$DISK" >&2; exit 2 ;;
esac
[ -b "$DISK" ] || { printf 'not a block device: %s\n' "$DISK" >&2; exit 2; }
[ -f "$IMAGE" ] || { printf 'missing image: %s\n' "$IMAGE" >&2; exit 1; }

# Refuse the disk the root filesystem lives on, and require a removable
# transport (USB) or the removable flag — the same "external disk" guarantee
# as the macOS original.
ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null)
case "$ROOTSRC" in
    "$DISK"*) printf 'refusing the root disk: %s\n' "$DISK" >&2; exit 2 ;;
esac
TRAN=$(lsblk -dn -o TRAN "$DISK" 2>/dev/null)
RMFLAG=$(lsblk -dn -o RM "$DISK" 2>/dev/null)
if [ "$TRAN" != "usb" ] && [ "$RMFLAG" != "1" ]; then
    printf 'refusing non-removable disk (%s, RM=%s): %s\n' "${TRAN:-unknown}" "$RMFLAG" "$DISK" >&2
    exit 2
fi

SIZE=$(stat -c %s "$IMAGE")
BLOCKS=$((SIZE / 4194304))
[ $((BLOCKS * 4194304)) -eq "$SIZE" ] || {
    printf 'image size is not a 4MiB multiple: %s\n' "$SIZE" >&2
    exit 1
}
EXPECTED=$(sha256sum "$IMAGE" | awk '{print $1}')

# Unmount anything mounted from this disk.
MNTS=$(lsblk -nro MOUNTPOINT "$DISK" | grep -v '^$' || true)
for m in $MNTS; do
    printf 'unmounting %s\n' "$m"
    sudo umount "$m"
done

sudo dd if="$IMAGE" of="$DISK" bs=4M status=progress conv=fsync
sync
ACTUAL=$(sudo dd if="$DISK" bs=4M count="$BLOCKS" 2>/dev/null | sha256sum | awk '{print $1}')
printf 'expected %s\nactual   %s\n' "$EXPECTED" "$ACTUAL"
[ "$ACTUAL" = "$EXPECTED" ]
printf 'SD_FLASH_VERIFY_OK\n'

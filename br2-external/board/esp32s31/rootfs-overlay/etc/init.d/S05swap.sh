#!/bin/sh
# SPDX-License-Identifier: MIT
# Swap to LZ4-compressed RAM first; p3 of the disk stays off by default.
# The module alone has no disk and swaps to zram only.

ZRAM=/sys/block/zram0
# 12M (2026-10-23): the X desktop working set is ~14 MB against 10.6 MiB
# of physical RAM.  With the 6M cap the anonymous pages filled zram and
# kswapd then reclaimed page cache, so code pages kept streaming back from
# the SD card (wa up to 40%, idle 0, the console unusable).  The pool only
# grows as used and lz4 compresses ~3x, so a bigger cap costs nothing
# until the desktop actually needs it.
ZRAM_SIZE=12M	# uncompressed
. /etc/default/disk

start_zram()
{
	echo lz4 > $ZRAM/comp_algorithm &&
		echo $ZRAM_SIZE > $ZRAM/disksize &&
		mkswap /dev/zram0 >/dev/null &&
		swapon -p 100 /dev/zram0 &&
		echo "swap: /dev/zram0 ($ZRAM_SIZE, lz4) active"
}

# Only type 0x82 (byte 482 of the MBR) marks p3 as swap: never format other
# data.  mkswap at each boot fits the header to the partition size.
start_disk()
{
	[ -b "${DISK_SWAP:-}" ] || return
	set -- $(od -An -tx1 -j482 -N1 $DISK 2>/dev/null)
	[ "$1" = 82 ] || return
	mkswap $DISK_SWAP >/dev/null &&
		swapon -p 10 $DISK_SWAP &&
		echo "swap: $DISK_SWAP active"
}

case "$1" in
start)
	echo 60 > /proc/sys/vm/swappiness
	echo 0 > /proc/sys/vm/page-cluster
	start_zram
	# p3 card swap stays off: with the desktop working set at ~14 MB and
	# 11 MiB of RAM, anything that lands on the SD card faults back at
	# 10-50 ms per page, which livelocked every desktop boot (wa 35-50%,
	# shell dead for minutes, wpa_supplicant misses auth deadlines).
	# zram-only keeps all swap traffic at sub-ms cost; the zram pool +
	# RAM cover ~23 MB effective, comfortably over the working set.
	# Re-enable manually if a workload needs it:
	#   sh -c '. /etc/init.d/S05swap.sh; start_disk'
	# start_disk
	;;
esac

exit 0

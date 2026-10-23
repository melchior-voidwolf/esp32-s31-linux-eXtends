#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

cat > "${BINARIES_DIR}/sd-readme.txt" <<'TXT'
ESP32-S31 Linux SD card.

The board mounts this FAT32 partition on /mnt/sd.  Use it for your files.
The second partition is the ext4 root, the third is swap.  macOS cannot
read them.
TXT

# The loader checks the Image with this manifest before it copies the Image
# to PSRAM.  make flash writes it at LINUX_SIZE_OFFSET.
python3 - "${BINARIES_DIR}" <<'PY'
import struct, sys, zlib, pathlib

images = pathlib.Path(sys.argv[1])
data = (images / "Image").read_bytes()
manifest = struct.pack("<III", 0x455A4953, len(data), zlib.crc32(data))
(images / "linux.size").write_bytes(manifest)
print("linux.size: %d bytes, crc32 %#010x" % (len(data), zlib.crc32(data)))
PY

# Only a board with a card has an ext4 root and gets a card image.
if [ -f "${BINARIES_DIR}/rootfs.ext2" ]; then
	# The loader prefers /IMAGE.BIN on p1 over the flash kernel, so the
	# card image carries the kernel: flashing the card alone gives a
	# fully bootable current system, no esptool step needed.
	cp "${BINARIES_DIR}/Image" "${BINARIES_DIR}/IMAGE.BIN"

	# Wi-Fi: ship NO wpa_supplicant.conf anywhere (rootfs copy removed
	# 2026-10-23, see post-build.sh).  The ESP-Hosted auth-timeout retry
	# storm against the aiot AP freezes the whole 10 MB userspace (~8 min
	# after boot, X included), so wpa must not start uninvited.  To use
	# Wi-Fi, drop a wpa_supplicant.conf on p1 yourself:
	#   wpa_passphrase <ssid> <psk> > /media/KORVO_SD/wpa_supplicant.conf
	# S40wifi prefers the p1 file, so no rebuild is needed.
	cat >> "${BINARIES_DIR}/sd-readme.txt" <<'TXT'

Wi-Fi is off by default (the driver can freeze the box retrying auth).
To enable it, put wpa_supplicant.conf on this FAT partition:

  wpa_passphrase "ssid" "passphrase" > <mountpoint>/wpa_supplicant.conf

then reboot.  Remove the file to turn Wi-Fi back off.
TXT

	# Swap policy marker: while NOSWAP sits on p1, S12swap starts no swap
	# at all.  The trimmed desktop fits in RAM (proven 2026-10-22 by
	# "swapoff -a" succeeding under X+icewm+wallpaper+xclock+xterm), and
	# swap churn on the shared MSPI bus crashes hart0 (rev0), so the
	# default is no swap; delete NOSWAP and reboot to get the zram+p3
	# tiers back for heavier workloads.
	touch "${BINARIES_DIR}/NOSWAP"

	# Debug escape hatch remains available the other way: create NOX on p1
	# to skip X autostart and land on the console.
	rm -f "${BINARIES_DIR}/NOX"

	support/scripts/genimage.sh -c "${BOARD_DIR}/genimage.cfg"
fi

#!/usr/bin/env python3
"""Validate Buildroot flash artifacts without OpenWrt-specific markers."""

import argparse
import hashlib
from pathlib import Path
import struct
import zlib

SQUASHFS_ZSTD = 6
EXPECTED_BLOCK_SIZE = 32768


def require(condition, message):
    if not condition:
        raise SystemExit(f"flash image validation failed: {message}")


def read_nonempty(path):
    require(path.is_file(), f"missing file: {path}")
    data = path.read_bytes()
    require(bool(data), f"empty file: {path}")
    return data


def describe(path, data):
    print(f"{path}: {len(data)} bytes sha256={hashlib.sha256(data).hexdigest()}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--rootfs", type=Path)
    parser.add_argument("--linux-partition-size", required=True)
    parser.add_argument("--rootfs-size")
    args = parser.parse_args()
    linux_size = int(args.linux_partition_size, 0)
    require(linux_size > 4096, "invalid Linux partition size")
    require((args.rootfs is None) == (args.rootfs_size is None),
            "--rootfs and --rootfs-size must be provided together")

    image = read_nonempty(args.image)
    manifest = read_nonempty(args.manifest)
    safe_image_size = linux_size - 4096
    require(len(image) <= safe_image_size,
            f"Image is {len(image)} bytes; manifest-safe limit is {safe_image_size}")
    require(len(manifest) == 12, "linux.size must be exactly 12 bytes")
    magic, image_size, expected_crc = struct.unpack("<III", manifest)
    require(magic == 0x455A4953, f"bad manifest magic {magic:#x}")
    require(image_size == len(image), "manifest Image length mismatch")
    actual_crc = zlib.crc32(image)
    require(expected_crc == actual_crc,
            f"manifest CRC {expected_crc:#010x} != Image CRC {actual_crc:#010x}")
    describe(args.image, image)
    describe(args.manifest, manifest)

    rootfs_limit = "disk-root"
    if args.rootfs is not None:
        rootfs_size = int(args.rootfs_size, 0)
        require(rootfs_size > 0, "invalid rootfs partition size")
        rootfs = read_nonempty(args.rootfs)
        require(len(rootfs) <= rootfs_size,
                f"rootfs is {len(rootfs)} bytes; partition capacity is {rootfs_size}")
        require(len(rootfs) >= 96, "squashfs superblock is truncated")
        require(rootfs[:4] == b"hsqs", "rootfs lacks little-endian squashfs hsqs magic")
        block_size = struct.unpack_from("<I", rootfs, 12)[0]
        compressor = struct.unpack_from("<H", rootfs, 20)[0]
        major, minor = struct.unpack_from("<HH", rootfs, 28)
        require((major, minor) == (4, 0), f"unsupported squashfs version {major}.{minor}")
        require(compressor == SQUASHFS_ZSTD,
                f"squashfs compressor id {compressor} != zstd ({SQUASHFS_ZSTD})")
        require(block_size == EXPECTED_BLOCK_SIZE,
                f"squashfs block size {block_size} != {EXPECTED_BLOCK_SIZE}")
        describe(args.rootfs, rootfs)
        rootfs_limit = rootfs_size

    print(f"FLASH_IMAGES_VALID image_limit={safe_image_size} rootfs_limit={rootfs_limit}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Unit tests for validate-flash-images.py using synthetic files."""

from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

VALIDATOR = Path(__file__).resolve().parent.parent / "scripts" / "validate-flash-images.py"


def main():
    with tempfile.TemporaryDirectory(prefix="s31-flash-validator-") as tmp:
        root = Path(tmp)
        image = root / "Image"
        manifest = root / "linux.size"
        rootfs = root / "rootfs.squashfs"
        payload = b"kernel-test"
        image.write_bytes(payload)
        manifest.write_bytes(struct.pack("<III", 0x455A4953, len(payload), zlib.crc32(payload)))
        superblock = bytearray(96)
        superblock[:4] = b"hsqs"
        struct.pack_into("<I", superblock, 12, 32768)
        struct.pack_into("<H", superblock, 20, 6)
        struct.pack_into("<HH", superblock, 28, 4, 0)
        rootfs.write_bytes(superblock)
        base = ["python3", "-I", str(VALIDATOR),
                "--image", str(image), "--manifest", str(manifest),
                "--rootfs", str(rootfs), "--linux-partition-size", "0x780000",
                "--rootfs-size", "0x400000"]
        result = subprocess.run(base, capture_output=True, text=True, check=False)
        assert result.returncode == 0, (result.stdout, result.stderr)
        struct.pack_into("<H", superblock, 20, 4)
        rootfs.write_bytes(superblock)
        result = subprocess.run(base, capture_output=True, text=True, check=False)
        assert result.returncode != 0 and "compressor" in result.stderr
        result = subprocess.run(base[:7] + base[9:11], capture_output=True, text=True, check=False)
        assert result.returncode == 0, (result.stdout, result.stderr)
    print("FLASH_VALIDATOR_TEST_PASS")


if __name__ == "__main__":
    main()

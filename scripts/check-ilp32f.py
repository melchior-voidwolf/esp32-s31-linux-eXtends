#!/usr/bin/env python3
"""Host acceptance checks for the Korvo Buildroot ilp32f flash rootfs."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def read_config(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("# ") and line.endswith(" is not set"):
            values[line[2:].split("=", 1)[0].split(" ", 1)[0]] = "n"
        elif "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            values[key] = value.strip('"')
    return values


def require(condition, message):
    if not condition:
        raise SystemExit(f"ILP32F_CHECK_FAIL: {message}")


def expect(config, key, value="y"):
    require(config.get(key) == value, f"{key}: expected {value}, got {config.get(key)}")


def elf_flags(data):
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    if data[4] != 1 or data[5] != 1:
        return None
    if struct.unpack_from("<H", data, 18)[0] != 243:
        return None
    return struct.unpack_from("<I", data, 36)[0]


def interpreter(data, path):
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + 32 > len(data):
            raise SystemExit(f"ILP32F_CHECK_FAIL: truncated program headers in {path}")
        ptype, poffset = struct.unpack_from("<II", data, offset)
        filesz = struct.unpack_from("<I", data, offset + 16)[0]
        if ptype == 3:
            require(poffset + filesz <= len(data), f"truncated PT_INTERP in {path}")
            return data[poffset:poffset + filesz].rstrip(b"\0").decode()
    return None


def scan_target(target):
    expected_interpreter = "/lib/ld-musl-riscv32-sp.so.1"
    count = dynamic = 0
    for path in target.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        flags = elf_flags(data)
        if flags is None:
            continue
        count += 1
        require(flags & 0x6 == 0x2, f"not single-float ABI: {path} flags={flags:#x}")
        require(flags & 0x1 == 0x1, f"RVC missing: {path} flags={flags:#x}")
        interp = interpreter(data, path)
        if interp is not None:
            dynamic += 1
            require(interp == expected_interpreter,
                    f"wrong PT_INTERP in {path}: {interp}")
    require(count > 20, f"only {count} RISC-V ELF files found")
    require(dynamic > 5, f"only {dynamic} dynamic RISC-V ELF files found")
    require((target / "lib/ld-musl-riscv32-sp.so.1").exists(),
            "single-float dynamic loader is missing")
    require(not (target / "lib/ld-musl-riscv32-sf.so.1").exists(),
            "soft-float loader leaked into target")
    print(f"ELF_ABI_OK count={count} dynamic={dynamic}")


def check_configs(output, kernel_config):
    br = read_config(output / ".config")
    expect(br, "BR2_RISCV_ABI_ILP32F")
    expect(br, "BR2_RISCV_ABI_ILP32", "n")
    expect(br, "BR2_RISCV_ISA_RVF")
    expect(br, "BR2_RISCV_ISA_RVD", "n")
    expect(br, "BR2_GCC_TARGET_ABI", "ilp32f")
    expect(br, "BR2_RISCV_ISA_EXTRA", "zba_zbb_zbs")
    expect(br, "BR2_TARGET_ROOTFS_SQUASHFS")
    expect(br, "BR2_PACKAGE_BLUEZ5_UTILS")
    expect(br, "BR2_PACKAGE_ALSA_LIB")
    require(br.get("BR2_TARGET_ROOTFS_EXT2") != "y", "disk EXT2 root is enabled")
    for key in ("BR2_PACKAGE_XORG7", "BR2_PACKAGE_XSERVER_XORG_SERVER",
                "BR2_PACKAGE_ICEWM", "BR2_PACKAGE_MATCHBOX"):
        require(br.get(key) != "y", f"desktop package leaked in: {key}")
    require("rootfs-overlay-korvo1flash" in br.get("BR2_ROOTFS_OVERLAY", ""),
            "Korvo flash overlay missing")
    require("local-overlay" not in br.get("BR2_ROOTFS_OVERLAY", ""),
            "desktop local-overlay leaked into target")
    require("linux-korvo1-flash.config" in br.get("BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE", ""),
            "wrong kernel config")

    kernel = read_config(kernel_config)
    for key in ("CONFIG_FPU", "CONFIG_BT", "CONFIG_BT_BREDR", "CONFIG_BT_LE",
                "CONFIG_CFG80211", "CONFIG_ESP32S31_HOSTED", "CONFIG_MTD",
                "CONFIG_MTD_BLOCK", "CONFIG_MTD_UBI", "CONFIG_MISC_FILESYSTEMS",
                "CONFIG_SQUASHFS", "CONFIG_SQUASHFS_ZSTD", "CONFIG_UBIFS_FS",
                "CONFIG_OVERLAY_FS", "CONFIG_I2C", "CONFIG_I2C_ESP32S31",
                "CONFIG_SND_SOC_ES8389", "CONFIG_SND_SIMPLE_CARD"):
        expect(kernel, key)
    expect(kernel, "CONFIG_BLK_DEV_INITRD", "n")
    require(not kernel.get("CONFIG_INITRAMFS_SOURCE"), "external initramfs remains")
    expect(kernel, "CONFIG_BUILTIN_DTB_NAME", "espressif/esp32s31_korvo1")
    cmdline = kernel.get("CONFIG_CMDLINE", "")
    require("root=/dev/mtdblock0" in cmdline and "init=/sbin/overlay-init" in cmdline,
            "wrong direct-root command line")
    for key, value in kernel.items():
        require("openwrt" not in value.lower(), f"{key} references the OpenWrt worktree")
    for key in ("CONFIG_INPUT", "CONFIG_HID", "CONFIG_HID_SUPPORT",
                "CONFIG_USB_HID", "CONFIG_I2C_HID", "CONFIG_VT",
                "CONFIG_NEW_LEDS", "CONFIG_MMC", "CONFIG_FB"):
        require(kernel.get(key) != "y", f"unwanted headless driver enabled: {key}")
    print("EFFECTIVE_CONFIG_OK")


def check_target_layout(target):
    for relative in ("usr/bin/Xorg", "etc/init.d/S40wifi", "etc/init.d/S05swap.sh",
                     "etc/init.d/S70diag", "etc/init.d/S10sdcard.sh",
                     "etc/init.d/S02ledstatus", "etc/init.d/S98ledready",
                     "etc/init.d/wifi", "usr/sbin/wpa_supplicant",
                     "mnt/sd", "usr/bin/aplay"):
        require(not (target / relative).exists(), f"unexpected target path: {relative}")
    for relative in ("etc/init.d/bluetoothd", "etc/init.d/dbus-daemon",
                     "etc/init.d/dropbear", "etc/init.d/swap.sh",
                     "etc/init.d/S41usbnet", "sbin/overlay-init"):
        require((target / relative).exists(), f"missing target path: {relative}")
    require(b"headless-flash::respawn" not in (target / "etc/inittab").read_bytes(),
            "headless-flash was incorrectly added as a VT")
    require(b"flash_erase" not in (target / "sbin/overlay-init").read_bytes().split(b"if ! ubiattach", 1)[-1].split(b"if [ ! -e", 1)[0],
            "overlay-init still auto-erases after an attach failure")
    print("TARGET_LAYOUT_OK")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--board", default="korvo1flashf")
    parser.add_argument("--skip-qemu", action="store_true")
    args = parser.parse_args()
    output = args.repo / "build" / args.board
    target = output / "target"
    require(target.is_dir(), f"missing target: {target}")
    kernels = sorted((output / "build").glob("linux-*/.config"))
    require(len(kernels) == 1, f"expected one effective kernel config, got {kernels}")

    check_configs(output, kernels[0])
    check_target_layout(target)
    scan_target(target)

    compilers = sorted((output / "host/bin").glob("*-linux-musl-gcc"))
    require(compilers, "cross gcc not found")
    cc = compilers[0]
    with tempfile.TemporaryDirectory(prefix="s31-ilp32f-check-") as tmp:
        binary = Path(tmp) / "abi-runtime-smoke"
        source = args.repo / "tests/abi/abi-runtime-smoke.c"
        subprocess.run([
            str(cc), "-O2", "-mabi=ilp32f", "-march=rv32imafc_zba_zbb_zbs",
            "-o", str(binary), str(source), "-lm",
        ], check=True)
        if not args.skip_qemu:
            require(shutil.which("qemu-riscv32"), "qemu-riscv32 not installed")
            subprocess.run([
                "qemu-riscv32", "-cpu",
                "rv32,f=true,d=false,c=true,zba=true,zbb=true,zbs=true",
                "-L", str(target), str(binary),
            ], check=True)
    print("ILP32F_CHECK_PASS")


if __name__ == "__main__":
    main()

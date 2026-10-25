# Korvo-1 Buildroot headless SPI-flash profile

## Scope and current status

`korvo1flashf` is the optimized **ilp32f** profile; `korvo1flash` is the soft-float fallback and A/B variant. Both are independent of the SD/X and OpenWrt outputs. The complete native build, host gates, **direct-root boot, non-audio board acceptance and the 30-minute mixed stability run now pass**. The default is upstream v0.7.2 (`1b29367`) with `-O2`, `LD_DEAD_CODE_DATA_ELIMINATION` and the ES8389 source patch; `-Os` remains available as a measured fallback.

- No X/IceWM/matchbox, no SD root, no desktop `local-overlay` or prebuilt desktop binaries.
- Buildroot/musl userspace, existing `zba_zbb_zbs`, GCC 15.3 and musl 1.2.6.
- Direct squashfs root, no embedded OpenWrt initramfs and no dependency on the OpenWrt worktree.
- Read-only ZSTD squashfs on `mtd0`, persistent UBIFS overlay on `mtd1`.
- UART and wired USB Ethernet; zram, diagnostics, SSH, D-Bus and Bluetooth are not default background workloads. The first 4 MiB profile has no WPA/OpenSSL Wi-Fi userland; Wi-Fi belongs to a later larger variant.
- X/SD work is deferred until an SD card is available.

## Build and host validation

```sh
cd linux/repo
make defconfigs
make check BOARD=korvo1flashf
make BOARD=korvo1flashf build
make BOARD=korvo1flashf validate-images
python3 -I scripts/check-ilp32f.py --board korvo1flashf
bash scripts/build-abi-nolib.sh korvo1flashf
```

Long builds are run in the background with a log. `validate-images` checks the manifest, exact Image CRC/length, the 4 KiB manifest-safe boundary, squashfs `hsqs`, ZSTD and 32 KiB blocks, and partition capacities. `check-ilp32f.py` additionally checks the effective Buildroot/kernel configs, ELF single-float ABI and PT_INTERP, target startup-file layout, and dynamic QEMU runtime smoke.

Current host-verified artifacts:

- Development bundle: `build/release/esp32s31-linux-v0.7.2-x.1-korvo1flashf-flash.zip`, SHA-256 `9231a91ac61bdf48d1a1c7cee93e46f759193cd67bbacfce69cc680533792072`. It contains the accepted `-O2 + LD_DEAD_CODE_DATA_ELIMINATION` Image, manifest, rootfs, loader/OpenSBI and `flash_args`; the merged `flash.bin` stops before the data partition.

- `korvo1flashf/Image`: **5,483,192 bytes** with the measured `-O2 + LD_DEAD_CODE_DATA_ELIMINATION` default; `rootfs.squashfs`: **4,124,672 bytes (3.93 MiB)** within the 4 MiB partition. The soft `korvo1flash` rootfs is **4,079,616 bytes (3.89 MiB)** and its diagnostics also pass. The accepted `-Os` fallback Image and full backup are preserved.
- loader: **1,511,392 bytes**, `LCD_KEEP_ALIVE=n`, `SD_KERNEL=n`, RCP off, dual-mode BT enabled.
- `FLASH_IMAGES_VALID`, `EFFECTIVE_CONFIG_OK`, `TARGET_LAYOUT_OK`, `ELF_ABI_OK count=57 dynamic=33`, `ILP32F_CHECK_PASS`, `ABI_RUNTIME_PASS` and `NOLIB_BENCH_BUILD_OK` have passed.
- The system C benchmark passes under QEMU. No audio or Thread operation has been performed. The separate board backup/deployment log records actual flash activity; host gates do not imply direct-root boot works.

The important effective kernel dependencies include `FPU`, `BT`, `CFG80211`, `ESP32S31_HOSTED`, `MTD_BLOCK`, `MTD_UBI`, `SQUASHFS_ZSTD`, `UBIFS_FS`, `OVERLAY_FS`, I2C and ES8389/ASoC. Dropping BT also removes Hosted and its flash MTD, so it is not a size-saving option.

## Flash layout and data safety

The shared Korvo CSV/DTS layout is unchanged:

| Region | Offset | Size |
| --- | ---: | ---: |
| Linux | `0x2A0000` | `0x780000` |
| manifest | `0xA1FFF4` | 12 bytes |
| rootfs | `0xA20000` | `0x400000` |
| data/UBIFS | `0xE20000` | `0x1E0000` |

The Image limit is `0x77F000`, not partition-size-minus-12: writing the manifest can erase the final 4 KiB sector. `FLASH_ROOT_BOARDS` is explicit, so an SD Korvo build does not accidentally include a nonexistent squashfs merely because the shared CSV contains a rootfs row.

`overlay-init` no longer erases `mtd1` after an arbitrary UBI attach failure. It falls back to the read-only root and leaves data untouched. Blank NOR is handled by UBI's empty-MTD path. Erasing the data region is a separate, explicit factory-reset operation, never a normal update or troubleshooting default.

The deployment entry point is:

```sh
S31_SESSION=linux-flash tools/flash-linux-headless.sh
```

It uses `s31-exclusive.sh`, validates artifacts, makes two full 16 MiB backup reads and compares them, flashes without touching data, reads back kernel/manifest/rootfs, then hard-resets once before reopening the shared UART and waiting for a real Buildroot shell. Backup/write/readback operations leave the chip in the ROM bootloader (`no-reset`) so an OS cannot boot and mutate persistent data between operations. Checksums use basenames inside the backup directory, so moved backups are verified against the actual selected files. `--erase-data` is required for a deliberate data reset. Subsequent `--no-backup` updates must name an existing verified backup directory. Do not use `restore-soft-linux.sh` for this profile.

A full backup is restored only with an explicit second confirmation; the restore path also performs a complete readback comparison:

```sh
S31_CONFIRM_RESTORE=RESTORE_16M \
  tools/restore-buildroot-flash-backup.sh build/s31-flash-backups/<backup-directory>
```

Backups can contain credentials and persistent data. They are local-only sensitive artifacts, not release files.

## First board deployment and acceptance

The first approved deployment used two matching full-flash reads at `build/s31-flash-backups/20261024T201020Z-korvo1flashf`. Kernel, manifest and rootfs then passed byte-for-byte readback, data was explicitly initialized, and the last reset produced `BUILDROOT_FLASH_BOOT_OK`.

`BOARD_ACCEPTANCE_PASS` (log directory `build/korvo1flashf/bench/board/20261024T205226Z`) verified:

- actual Buildroot PID 1, direct-root command line and an rw overlay; no X and no active swap;
- `MemTotal=10608 kB`, boot-time `MemAvailable=5408 kB`;
- `FPUTEST_PASS`, `ABI_SMOKE_PASS`, `ABI_RUNTIME_PASS` and the system C smoke;
- wired `eth0=10.0.0.84` with successful ping to `10.0.0.40`;
- Bluetooth controller enumeration and power-on, followed by bluetoothd/D-Bus cleanup.

No audio probe/playback/record, RCP or Matter operation was performed. The separate performance and stability runs determine the remaining tuning choices.

## Bluetooth

The existing path is Linux BT core/BlueZ → ESP-Hosted HCI → hart0 dual-mode controller. Keep `CONFIG_BT`, `BT_BREDR`, `BT_LE` and `esp32_shm.radio_service=bt`; no USB/UART HCI dongle driver is involved. BlueZ, D-Bus and their manual startup scripts remain, but their roughly 1 MiB-per-daemon working sets are not paid at idle by default.

Board acceptance is limited to local HCI/BR/EDR/BLE functionality. A2DP, HFP, LE Audio and any audio probe/playback are separate work and are not implied by HCI support.

## Audio: source preparation only, hardware testing blocked

Korvo-1 uses ES8389 at I2C address `0x10`, two NS4150B PAs on GPIO7 and two analog microphones. Linux 7.3 has an ES8389 codec driver and `simple-audio-card`; these are enabled for host build coverage together with I2C/regulator/ALSA library support. There are deliberately no codec, I2S or sound-card nodes in the active DTS.

The missing work is substantial: S31 I2S CPU DAI, GDMA/PCM, clocks, pinctrl, TDM slot/BCLK semantics, PA power policy and the final machine link. Therefore the state is **audio-ready / host-compilable / unverified**, not working audio. `linux/patches/0018-es8389-tdm-clock-and-binding.patch` has passed clean-tree application and codec-object compilation for TDM-aware BCLK calculation and the MCLK binding; it does not implement those platform pieces. No codec probe, `aplay`/`arecord` enumeration, playback, recording, mixer/volume or PA operation is allowed until the user explicitly authorizes audio testing.

License-clean local fixtures are already in `assets/audio/` with 48 kHz stereo, 16 kHz stereo and 16 kHz mono PCM WAV variants plus a hash manifest. They stay on the host and are not installed in the small rootfs.

## Matter / Thread: deferred for resource reasons

Matter, Thread and OTBR are **not implemented in this profile**. RCP source and the future `/dev/esp_rcp0` + Spinel/OpenThread route are documented, but RCP stays disabled and no large OTBR/connectedhomeip stack is installed or used to justify repartitioning.

Existing project records show RCP pressure on hart0 Wi-Fi SRAM and reduced Wi-Fi throughput; hart0 heap/loader margins and the approximately 10 MiB Linux RAM / 4 MiB rootfs budgets are limited. RCP lifecycle/recovery and Wi-Fi/BT/MTD coexistence also remain unsolved. Hardware or SDK capability is not a working integrated Matter feature.

## Performance validation plan

All comparisons use the same no-X/no-diag/no-default-swap policy and save raw data:

1. Direct-root/UBIFS boot correctness, actual boot generation and FPU/ABI smoke.
2. Boot time, MemAvailable/Slab, IRQ/system CPU, wired iperf3 and C benchmarks.
3. Same-GCC no-lib `ilp32/ilp32f × base/full-Zb` AB/BA tests; microbenchmarks are not whole-system claims.
4. Independent `vm.min_free_kbytes=512/768/1024` A/B with MTD read deltas, page cache, memory and real workload latency. OpenWrt results motivate the test but do not choose the Buildroot default.
5. Kernel `CC_OPTIMIZE_FOR_PERFORMANCE` versus SIZE only as a separate measured experiment.
6. At least 30 minutes of network, squashfs execution, UBIFS and runtime stress without OOM, reset or MTD/UBI errors.

Measured so far:

- Seven-round no-lib AB/BA (`build/korvo1flashf/bench/nolib/20261024T205505Z`): local float ≈1.00×, cross-function float ABI ≈1.152×, Zba ≈1.60×, Zbb fallback ≈1.9×, integer control ≈1.00×. These are instruction-level results, not whole-system speedups.
- Three rounds of runtime `min_free_kbytes` A/B (`build/korvo1flashf/bench/vm/20261024T205601Z`): the 1024 setting was slower in this small workload (squashfs-read median 0.488s vs 0.459s at 512 and 0.455s at 768), but MTD read counters did not increase. The original value **512** was restored; no persistent VM default was selected from this weak evidence.
- An RV32 `vgettimeofday` host prototype was rejected by the real type/implementation boundary (`old_timespec32` and the `__cvdso_*_time32` path), not merely by a config switch. The prototype was reverted and the baseline Image hash is unchanged. RV32 time vDSO is a separate future port, not a claimed optimization.
- The measured `-O2` default also passed direct-root boot. Its seven-round no-lib run (`build/korvo1flashf/bench/nolib/20261024T212620Z`) was about 3–5% faster than `-Os`; the real workload run (`build/korvo1flashf/bench/vm/20261024T212712Z`) was about 18% faster for fork/exec, 14% for getpid, 8% for clock_gettime and 3% for squashfs reads. The final patched image also passed `BOARD_ACCEPTANCE_PASS` at `build/korvo1flashf/bench/board/20261024T222515Z`.
- The 30-minute mixed soak at `build/korvo1flashf/bench/soak/20261025T032319Z` completed `SOAK_PASS`: six iperf/FPU/UBIFS rounds, no OOM/reset or error trend, final `MemAvailable=4192 kB`. The runtime `min_free_kbytes` remains **512**; no new VM default was needed.
- The v0.7.2 GC-sections A/B also held: no-GC gave `MemTotal=9548 kB` (code 4761 KiB, rodata 621 KiB, reserved 7036 KiB), while GC gave `MemTotal=9960 kB` (code 4417 KiB, rodata 556 KiB, reserved 6620 KiB), a measured 412 KiB improvement. The accepted `-Os` fallback and full backup remain available.

No audio test is part of any of these steps.

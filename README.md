# ESP32-S31 Linux eXtends

**X = X11 + eXtends.** A fork of
[annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux)
that puts a **hardware-accelerated X11 desktop** on the ESP32-S31-Korvo-1 —
the thing the upstream project concluded would not fit. 中文说明:
[README-cn.md](README-cn.md)。

> "An X server and a window manager do not fit in the approximately 3 MB
> that are free with Wi-Fi up, so the desktop is the console."
> — upstream, `e33dacb "Make the Korvo-1 a small desktop for a USB keyboard"`

We disagree. This is the board running IceWM with `xclock` — a native
framebuffer dump, not a photo:

![IceWM desktop, native framebuffer dump](docs/desktop-x11.png)

*(800x480 LCD, `/dev/fb0` exported and upscaled 3x nearest-neighbor.)*

And here is the actual board on the desk, taken with the webcam that
watches it:

![The real board, webcam photo](docs/desktop-photo.jpg)

*(Moiré included at no extra charge.)*

`fastfetch` on the merged v0.7.1 base, numbers straight from the board
(note the swap line: 9 MiB of the desktop lives compressed in zram):

![fastfetch on the ESP32-S31-Korvo-1](docs/fastfetch.svg)

## What this fork adds

Everything below is on top of the upstream `v0.5.0`-era base (`3dad844`).

### 1. X11 with real hardware acceleration

The stock `fbdev` X path pushes every pixel through the 320 MHz soft-float
CPU: 15.7 fps for a full-screen fill. This fork adds `xf86-video-s31`, an
EXA driver that maps:

| EXA op | Engine | Measured |
| --- | --- | --- |
| Solid (rect fill) | PPA fill | ~213 MB/s, full-screen fill **89 fps** |
| Copy (screen-to-screen) | DMA2D M2M | ~106 MB/s, scroll **256 fps** |
| Composite | deliberately **not** hooked | PPA blend has fixed per-plane alpha; the a8 glyph masks of a compositor-less desktop cannot be expressed. The full analysis lives in `s31_exa.c`. |

The desktop ships with IceWM, a wallpaper, `xclock`, the GT1151 touch panel
(i2c-gpio polling) and USB keyboard/mouse; `startde` brings it all up at
boot.

### 2. The F-only FPU kernel patch

Upstream's Limits section says the hardware FPU is not used, because the
hart has F but no D and Linux wants both. This fork carries a kernel patch
(`linux/patches/0015-riscv-f-only-fpu.patch`) that teaches the kernel
`rv32imafc`: FPU context switching and the `hwcap` plumbing. Userspace
stays soft-float `ilp32`, so existing binaries keep working; programs that
explicitly use F instructions can now. `/usr/bin/fputest` prints
`FPUTEST_PASS`.

### 3. The calm-desktop fixes, or: fitting a 14 MB working set into 10.6 MiB

Linux gets 10.6 MiB of the 16 MiB PSRAM (kernel, OpenSBI and framebuffer
take the rest). The X desktop working set is ~14 MB. Bringing this up on a
headless server made every failure mode visible:

- **The Wi-Fi auth storm hides in three places.** The ESP-Hosted
  auth-retry loop against an unreachable AP starves the whole 10 MiB
  userspace (~8 minutes to a frozen board, X included). This fork ships
  **no `wpa_supplicant.conf` at all** — neither the Buildroot package
  default (an open-network block that associates with anything) nor a
  template with real credentials on SD p1. Wi-Fi is opt-in: drop your own
  conf on p1, `S40wifi` prefers it. The `NOX` console escape hatch stays.
- **kswapd reclaim livelock.** With the 6 MiB zram cap, anonymous pages
  filled zram, kswapd then reclaimed page cache, and code pages kept
  streaming back from the SD card: 40% iowait, idle 0, load > 5, console
  dead — while every process still existed. Raising zram to 12 MiB (lz4
  compresses ~3x, pool grows on demand) and starting `xterm` only when a
  USB keyboard is attached turned load 5.85 into ~1.0, and the desktop
  came back to life.
- **The RGB status lamp is default-off.** It leaks light into the webcam
  that watches the board (glare and a color cast). `touch RGB` on SD p1
  opts back in. `S70diag` writes 15 minutes of load/cpu/mem samples plus
  an SD read-speed test to p1 at every boot, so the card tells the story
  even when the console is wedged.

### 4. Smaller things

- `icewm` `SKIP_TRANSLATIONS=ON`: the upstream po machinery installs `.mo`
  files with absolute destinations, which host cmake >= 3.28 turns into
  out-of-target pollution (rightly rejected by the buildroot scanner).
- Hardware JPEG codecs with desktop manners (`jpegdec`/`jpegenc` save and
  restore the PPA staging registers, and do not fight X for the screen).
- `fputest-abi`, `s31view`, `scripts/cam-shot.sh` (timestamped webcam shots
  of the board with a fixed-focus profile), `scripts/flash-sd-linux.sh` (the
  macOS-only flash scripts made portable, with SHA-256 readback verify),
  and a fully native non-Docker build path for x86 Linux servers
  (`scripts/soft-icewm-native-build.sh`).

## Relationship to upstream

This fork diverged before upstream's `v0.6.0` and **merged upstream
v0.7.1** (2026-10-23): the Function-CoreBoard-1 with Ethernet, the native
I2C master driver (the GT1151 touch now runs on hardware I2C instead of
i2c-gpio bitbang), the pin-controller rework, the tmux console desktop
(which coexists with ours: console side theirs, graphical side ours), and
the kernel-source-copy integration. See
[their README](docs/README-upstream.md) for the core-port story. Upstream
still has no X11 and no FPU support — that is what this fork is for.

Notable intentional differences:

- **Wi-Fi is off by default** (see the storm above). Upstream ships a
  template conf; we ship none.
- **The RGB LED is off by default** (`RGB` file on p1 opts in).
- **Swap**: zram 12 MiB, zram-only by default (the card p3 swap machinery
  is retained).
- **Host builds are tested on Linux x64** (Ubuntu 24.04, native, no
  container). The upstream macOS/container flow is still present but not
  exercised by this fork.

## Building (Korvo-1, on a Linux x64 host)

Tested on Ubuntu 24.04, native — no container, no cross-toolchain to
install by hand (Buildroot builds its own).

**Host packages:**

```sh
sudo apt install build-essential bc bison flex gettext rsync unzip cpio \
    patch perl tar python3 file libssl-dev wget git
```

**Build** (first run downloads ~1 GB of sources into `dl/`; on a
metered or censored connection budget for that. Cold build ≈ 25 min on
96 jobs, proportionally longer on smaller machines):

```sh
scripts/soft-icewm-native-build.sh
# → build/artifacts/sdcard-soft-icewm.img  (644 MiB, SHA-256 printed)
```

The script configures Buildroot from `br2-external/configs/korvo1_defconfig`,
verifies the ABI and package set (soft-float ilp32, X11, IceWM, the S31
Xorg driver), builds everything, then re-verifies the produced target
root (Xorg/IceWM/touch/codec binaries present, no `wpa_supplicant.conf`,
LED default-off) before exporting.

**Flash the card** (any 8 GB+ card; whole-disk, erase-all):

```sh
sudo scripts/flash-sd-linux.sh /dev/sdX build/artifacts/sdcard-soft-icewm.img
# or dd the image and run the SHA-256 check yourself
```

**First-time SPI flash.** The card carries the kernel; the SPI flash
carries the second-stage loader, OpenSBI and the Wi-Fi/BT radio firmware.
A board that has never been flashed needs those once:

- Easiest: flash the `flash.bin` from the
  [upstream releases](https://github.com/annoyedmilk/esp32-s31-linux/releases)
  (`esptool --chip esp32s31 -p PORT -b 921600 write-flash 0x0 flash.bin`).
  The upstream loader boots `IMAGE.BIN` from card p1, which is exactly how
  this fork is used.
- Or build it yourself: `make bootloader` (needs ESP-IDF v6.1 with the
  ESP-Hosted patches; see
  [docs/README-upstream.md](docs/README-upstream.md), Host requirements).

**Daily updates are card-only**: write the new image, done. The loader
reads `IMAGE.BIN` from card p1 in preference to the flash kernel.

**Verify after boot** (serial console 115200 baud, root, no password):

```sh
fastfetch        # OS/Kernel/Memory/Swap lines — Swap shows zram at work
fputest          # FPUTEST_PASS (the F-only FPU patch)
startde          # already ran at boot; the LCD shows the IceWM desktop
```

**Check you are running the card kernel**: `dmesg | grep version` — the
build host and date must match your build. If the loader's SD init times
out (rare, seen once in a day of boots) it falls back to the flash kernel
**silently**: the login prompt looks identical and `fputest` will fail
with Illegal instruction. Power-cycle and retry; a loader reflash makes
the fallback disappear.

### Known sharp edges

- Some USB devices wedge the DWC2 host controller in an interrupt-retry
  storm (~8k IRQ/s, system frozen). One specific device is confirmed bad;
  leave USB empty unless you need the keyboard, and watch
  `grep dwc2 /proc/interrupts` when you plug something.
- First Buildroot download can be slow; the `dl/` cache makes later
  builds incremental.
- If you re-run the build as a different user after a container/root run,
  `chown -R` your `dl/` — Buildroot fails with a bare Permission denied.

Serial console 115200 baud, root, no password on the console; SSH is
\nAll the pitfalls, fixes, techniques and design decisions we hit are
written up in [docs/notes.md](docs/notes.md) (Chinese:
[docs/notes-cn.md](docs/notes-cn.md)) — the Wi-Fi storm, the kswapd
livelock, the DWC2 device storm, the EXA decisions, the lot.
key-only. `docs/README-upstream.md` documents the loader, the memory map,
the flash layout and the kernel patch set in depth — all of that still
applies.

## Roadmap

- [x] Merge upstream v0.7.1 (done 2026-10-23; the touch panel now runs on
      hardware I2C, freeing the bitbang CPU slice)
- [ ] FPU on the merged kernel: the F-only patch needs a rework — on the
      upstream base the kernel reports `rv32imac` again (the resolve path
      drops `f` somewhere new); under investigation
- [ ] ilp32f (hardware-F ABI) rebuild; the survey and smoke test are done
- [ ] The dwc2 split-error retry storm on certain USB devices (patch 0014
      needs a companion)
- [ ] The Wi-Fi auth timeout root cause (unfixed upstream too)

## License

MIT, same as upstream (`LICENSE`); component licenses are listed at the
end of [docs/README-upstream.md](docs/README-upstream.md).

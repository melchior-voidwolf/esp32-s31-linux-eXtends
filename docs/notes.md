# Field notes: pitfalls, fixes, techniques, design decisions

Everything we hit bringing this fork up, in one place. The Chinese twin
of this file is [notes-cn.md](notes-cn.md); the two carry the same
content. Corrections welcome. Also mirrored in Chinese below.

(中文版见 README-cn.md 与 docs/notes-cn.md。)

## The three hiding places of the Wi-Fi auth storm

The ESP-Hosted firmware + host driver loop retries authentication against
an unreachable AP forever. On a 10.6 MiB system the retry work starves
everything else: ~8 minutes after boot the console dies (kernel still
echoes, shell never answers), X included. We found the storm alive three
times, each time behind a different `wpa_supplicant.conf`:

1. **The Buildroot package default** `/etc/wpa_supplicant.conf` contains an
   open-network block (`key_mgmt=NONE`): wpa associates with *any* open AP
   and retries. It hides under your overlay copy until you remove the
   overlay.
2. **A template with real credentials on SD p1.** `S40wifi` prefers
   `wpa_supplicant.conf` on p1 over the rootfs copy, so "we deleted the
   rootfs conf" fixed nothing while p1 still had one. This is how the
   storm survived our first fix attempt.
3. **The rootfs copy itself**, when a user network block is added at
   runtime.

This fork therefore ships **no conf at all**, and `S40wifi` fails cleanly
without one. Wi-Fi is opt-in: `wpa_passphrase ssid pass >
/mnt/sd/wpa_supplicant.conf` and reboot. `NOX`, `NOSWAP`, `NOWIFI`,
`NOWALLPAPER` and `RGB` on p1 are the switch family.

Diagnosis, if you suspect it: the console freezes while the kernel keeps
echoing; `dmesg` fills with `auth timeout` / `Failed to send assoc`. The
only cure once frozen is a power cycle.

## kswapd reclaim livelock: a 14 MB working set in 10.6 MiB

Linux gets 10.6 MiB of the 16 MiB PSRAM (kernel ~5 MB, OpenSBI 0.35 MB,
framebuffer 0.75 MB are the rest). The X desktop working set measures
~14 MB. The failure mode when memory runs out is *not* the OOM killer —
it is this:

1. anonymous pages fill zram (12 MiB cap, lz4, ~3x compression);
2. kswapd starts reclaiming page cache — including the *code pages of
   everything running*;
3. those pages fault back in from the SD card, evicting others;
4. load climbs past 5, iowait hits 40%, idle goes to 0, and the console
   answers a command every few minutes — **while every process still
   exists and `dmesg` stays clean**.

Fixes, in order of leverage:

- **zram 12 MiB** (`S05swap.sh`): costs nothing until used (pool grows on
  demand), compresses ~3x, and keeps the *anonymous* pressure off the page
  cache.
- **xterm only with a USB keyboard attached** (`startde`): ~1.5 MB RSS.
- Kill the silent daemons: `post-build.sh` stops dropbear/bluetoothd/dbus
  from autostarting (~3 MB combined working set).

Accounting trick that found this: `free` shows `used` far larger than the
sum of RSS — the difference is the swap pressure and kernel slab. `ps`
RSS sort + `grep '^cpu ' /proc/stat` twice (watch `system` and `iowait`
grow while `idle` stays 0) is the two-minute diagnosis.

## The DWC2 USB device storm

One specific USB device (left plugged into the board) wedges the DWC2
host controller: `grep dwc2 /proc/interrupts` climbs at ~8k IRQ/s, the
log fills with `usb 1-1: clear tt 1 (...) error -71` (split-transaction
failures through the hub), and the system saturates in the kernel
(`system` 75%+, `idle` 0). Unplug it and the box comes back to life
mid-boot. Patch `0014` decimated the SOF scheduler cost but does not
cover this error path — a companion patch is on the roadmap.

The nasty part: the symptom is a black screen with a frozen cursor, which
looks exactly like the reclaim livelock above. Check
`/proc/interrupts` *before* theorizing.

## The silent flash-kernel fallback

The loader prefers `IMAGE.BIN` on card p1; **if the card read fails it
silently falls back to the kernel in flash**. The failure is rare (seen
once: `SD mount failed: ESP_ERR_TIMEOUT`) and the two boots look
identical from the login prompt. We measured "FPU regression" and
"isa=rv32imac" on the wrong kernel because of this.

Rule: after every boot you care about, check the build host and date:

```sh
dmesg | grep version
```

`kail@kail-epyc ... Oct 23` is the card kernel; anything else is flash.
A loader reflash removes the fallback entirely.

## F-only FPU: the design, and the open regression

The hart is `rv32imafc` — F without D. Mainline riscv kernel rejects
that (validate hook + elf_hwcap mask both drop F). Patch
`0017-riscv-f-only-fpu.patch`:

- `fpu.S`: the context switch uses `fsw/flw` (32-bit stores) instead of
  `fsd/fl`;
- `cpufeature.c`: the validate hook accepts F-only; the elf_hwcap mask
  is gone;
- `switch_to.h`: `has_fpu()` gates on `RISCV_ISA_EXT_F`;
- kernel C code stays soft-float (`KBUILD_BASE_ISA` without f/d), the
  only FP instructions in the kernel are the hand-written switch.

Userspace is built soft-float `ilp32` regardless, so existing binaries
never touch the FPU; programs that want F instructions now work.
`/usr/bin/fputest` runs an arithmetic + concurrency check and prints
`FPUTEST_PASS`.

**Open issue**: on the merged v0.7.1 base the kernel reports
`rv32imac` again. Verified so far: the patch hunks are in the built tree,
`riscv_ext_f_validate` disassembles to "return 0", CONFIG_FPU=y, the DTB
carries `"f"` in `riscv,isa-extensions` — yet the resolved bitmap drops
it. The loss happens somewhere else in the resolve pipeline; digging
continues.

## EXA: what is accelerated, and why Composite is not

| EXA op | Engine | Measured |
| --- | --- | --- |
| Solid | PPA fill | ~213 MB/s, 89 fps full-screen |
| Copy | DMA2D M2M blit | ~106 MB/s, 256 fps scroll |
| Composite | not hooked | see below |

fbdev reference: 15.7 / 9.1 fps. The S31 Xorg driver lives in
`br2-external/package/xdriver-xf86-video-s31/src/` (EXA PrepareSolid/
Copy → PPA/DMA2D register sequences; overlapping Copy falls back to a
safe direction).

Composite is deliberately left unaccelerated: the PPA blend engine
merges two framebuffers with a *fixed per-plane alpha*, while a
compositor-less desktop issues Render ops that are solid color modulated
by a per-pixel **a8 mask** (antialiased glyphs). RGB565 pixmaps carry no
alpha, and the engines only address the framebuffer physical range, so
mask pixmaps in RAM are out of reach. Hooking PrepareComposite would buy
diagnostics, not frames.

Channel discipline: the JPEG codecs (`jpegdec`/`jpegenc`) also use DMA2D
channel 0 — they save/restore the staging registers and never run
concurrently with X.

## icewm + cmake ≥ 3.28: out-of-target pollution

icewm's po machinery installs `.mo` files with **absolute** DESTINATIONs
(`po/playground`), which older cmake ignored under `DESTDIR` but cmake
3.28 prefixes with it — the files landed inside the Buildroot target and
the scanner rejected the package. `-DSKIP_TRANSLATIONS=ON` turns the
whole po processing off (we are English-only anyway). Watch for this
pattern in any CMake package that ships translations.

## The kernel size trap: 48 NET_VENDOR_* families

Turning `CONFIG_ETHERNET=y` on (needed by the Function-CoreBoard-1)
makes `olddefconfig` default **every** `NET_VENDOR_*` family to y — 48
x86-era vendor families built into an RV32 kernel. On this SoC they are
mostly empty shells (they want PCI/IO ports), so the size win of
disabling them is small — but the hygiene matters on any size-sensitive
kernel, and `linux.config` now pins them off explicitly.
`scripts/slim-kernel-rebuild.sh` is the incremental rebuild for this
class of change.

## Memory map, and the deployment model

| Range | Use |
| --- | --- |
| `0x50000000` | kernel image (resident, ~5 MB) |
| middle | **10.6 MiB usable RAM** (`free` total) |
| `0x50f00000` | OpenSBI (0.35 MB) |
| `0x50f40000` | framebuffer (0.75 MiB, uncached) |

Deployment: the **card carries the kernel** (`IMAGE.BIN` on p1, loader
boots it in preference to flash), so a card write is a full system
update. The SPI flash (loader/OpenSBI/radio firmware) is flashed once —
the upstream release `flash.bin` works — and only changes when the
loader itself changes.

## Techniques that paid for themselves

- **Framebuffer screenshots without a camera**:
  `dd if=/dev/fb0 bs=1600 count=480 | gzip | base64 -w 76`, pull the text
  over the serial console, md5-check, then
  `ffmpeg -f rawvideo -pix_fmt rgb565le -s 800x480` → PNG. 768 KB of
  framebuffer compresses to ~9 KB; the serial link only carries it
  reliably when the system is idle — X running costs bytes (UART FIFO
  overruns).
- **Diagnosis to the card, not the console** (`S70diag`): every boot
  samples load/cpu/mem/swap for 15 minutes plus an 8 MB SD read-speed
  test, into `diag.txt` on p1. The console can be wedged; the card still
  tells the story.
- **`grep dwc2 /proc/interrupts`** before any theory about a frozen
  board; **`dmesg | grep version`** before any benchmark.
- **Webcam shots of the board**: BRIO at manual focus 75 (`focus_absolute`
  maps larger = farther), 1280x720 MJPG via ffmpeg, 30 frames to settle.
- **Serial console resets**: opening the port with DTR/RTS asserted
  resets the board — the console daemon exploits this; beware of it when
  a script merely wants to read.
- **Never `pkill -f <thing>` from a shell whose own command line contains
  `<thing>`** — it matches itself and kills your pipeline. `fuser
  /dev/ttyUSB0` to free a serial port is the reliable way.
- **Pushing to GitHub from a censored network**: large pushes die with
  GnuTLS -110; `git -c http.proxy=... push` through the local proxy goes
  through. And a **shallow clone cannot push** (remote `unpack failed:
  index-pack failed`): `git fetch origin --unshallow` first.
- **`dl/` permission churn**: after mixing container/root and user runs,
  Buildroot fails with a bare Permission denied deep in a download —
  `chown -R` the cache.

# Korvo-1 audio porting boundary (audio tests blocked)

This document tracks **source/host preparation only**. Until the user explicitly authorizes audio testing, do not probe the codec, enable a sound-card node, enumerate audio devices, play/record, change mixer/volume, or operate PA_CTRL.

## Hardware and existing references

- ES8389 stereo codec at `0x10`, two analog microphones and two NS4150B PAs. GPIO7 controls both PAs together.
- Official guide: I2S GPIO2/3/4/5/6, I2C GPIO0/1, TDM required for ADC/DAC.
- Official hardware guide: <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html>
- ESP-IDF references:
  - `components/esp_driver_i2s/i2s_tdm.c`
  - `components/esp_hal_i2s/esp32s31/i2s_periph.c`
  - `components/soc/esp32s31/register/soc/i2s_struct.h`
  - `components/esp_hal_dma/esp32s31/include/hal/gdma_channel.h`
- Linux 7.3 already has `sound/soc/codecs/es8389.c` and `everest,es8389.yaml`; `simple-audio-card` and `simple-amplifier` are possible building blocks.

The official BSP currently uses I2S1 and defines MCLK as not connected, while the hardware pin table lists GPIO2 MCLK. The hardware guide also requires TDM while current BSP code uses a standard-I2S API. These are unresolved facts, not permission to guess a DSP_A/DSP_B mode, slot width, BCLK or MCLK setting.

## Missing Linux pieces

1. S31 I2S CPU DAI and PCM/GDMA platform support. The IDF reference uses I2S1 at `0x20388000`, interrupt source 7; verify those values against the actual TRM/revision before writing the Linux node.
2. I2S clock/reset entries and GPIO-matrix pinctrl signals in Linux bindings/drivers.
3. A dedicated, cache-audited DMA design. Do not steal the shared `linux,dma-default` pool used by USB/SD or the final fbaccel page. Review rev0 burst requirements and descriptor placement.
4. ES8389 TDM corrections: TX/RX masks and slot width, total-slots × slot-width × rate BCLK calculation, and the `everest,mclk-src` binding property.
5. A Korvo DAI link and conservative PA policy. Keep codec/sound nodes disabled in the active DTS. Confirm PA power-on defaults and polarity from the schematic before describing enable-gpios; do not use an always-on regulator or automatic ALSA restore.

## Current source-level patch

`linux/patches/0018-es8389-tdm-clock-and-binding.patch` is now included in the mainline patch series. It records TDM slot count/width so BCLK is derived as `total_slots × slot_width × rate` instead of always using `channels × sample_width × rate`, and adds the `everest,mclk-src` uint32 enum (0=MCLK pin, 1=SCLK) to the DT binding. The patch has passed a clean-tree application check and `es8389.o` compilation; the build tree source was restored after the test.

This does not complete audio support: TX/RX mask semantics, I2S/GDMA, clock/pinctrl, the machine link and PA policy remain open. No codec/I2S/sound-card node is enabled in the active DTS, and no audio hardware operation was performed.

## Host-only implementation order

1. Add clock/reset/pinctrl and disabled DT descriptions; run DTC/`dtbs_check`.
2. Implement only I2S1 + GDMA channel 0 + S16_LE stereo playback at 16 kHz, with focused unit tests for TDM/BCLK calculations. Capture, other rates, PDM and multichannel are later work.
3. Patch the codec driver/binding and use `simple-audio-card` only if its clocking and routing can express the verified design; otherwise add a small machine driver.
4. Cross-compile all pieces and label the result **host-compilable/source-verified**, never board-verified.
5. Stop before any operation that initializes audio hardware. The user must explicitly unlock the later probe/playback/record stages.

Local PCM fixtures are in `assets/audio/` with 16 kHz stereo/mono and 48 kHz stereo WAVs and a SHA-256 manifest. They are host-only and are not part of the flash image.

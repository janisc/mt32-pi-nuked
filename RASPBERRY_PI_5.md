# Raspberry Pi 5 support and general fixes

This branch adds Raspberry Pi 5 support to [mt32-pi-nuked](https://github.com/odiaboeeu/mt32-pi-nuked) v1.1.0, plus a set of fixes and options that are useful on any Raspberry Pi. It is offered as is: take whatever is useful, in any form. No further maintenance is promised.

The hard work is not ours. mt32-pi-nuked is odiaboeeu's work on top of Dale Whinham's mt32-pi; the synthesizers are by nukeykt, the Munt team and the FluidSynth team; and Circle by Rene Stange is what makes a bare-metal kernel on a Raspberry Pi 5 possible at all. This branch only connects the pieces on one more board. See [Credits and licenses](#credits-and-licenses).

## What is new

Each row is one commit on top of mt32-pi-nuked v1.1.0, in this order. Rows with an empty last column can be cherry-picked on their own; the others also need the rows listed there.

### For every Raspberry Pi model

| # | Change | Why it matters | Needs |
| --- | --- | --- | --- |
| 1 | Build with GCC 13 and newer | `<circle/new.h>` clashes with libstdc++, `-ffreestanding` rejects hosted C++ headers, and GCC 15 rejects a `strncpy()`. Built with Arm GNU Toolchain 15.2. |  |
| 2 | `make` builds the Nuked-SC55 core | Without it, a fresh clone has no `libnukedsc55_core.a` to link until the script is run by hand. Both core scripts take the CPU flags from the Makefile. | 1 |
| 3 | SSD1306: set the RAM address for every frame, resend failed frames | One interrupted I2C transfer used to shift the picture until the next power cycle. |  |
| 4 | Check firmware throttling once per second | `CPower::Update()` made a firmware mailbox request on every pass of the main loop. Main core load on a Pi 5 while playing SC-55: 16% before, 2-3% after. |  |
| 5 | Munt is named in its status message | Munt and Nuked-MT32 showed the same control ROM name after a synth switch. |  |
| 6 | `[sc55] buffer_ms` | Makes the SC-55 latency configurable. `0` keeps the v1.1.0 values on earlier models, which were validated with `chunk_size = 64`. By calculation the margin is smaller at the default of 256, where a larger buffer may help. |  |
| 7 | `gain` in `[nuked_mt32]` and `[sc55]` | Output level of the two Nuked engines, like the existing options for Munt and FluidSynth. MT-32 music is quiet, and the engines differ in level. | 6 |
| 8 | Audio dropout and overheating warnings | "Audio underrun!", "SC-55 underrun!" and "CPU hot! NN C" on the display. The firmware throttles for heat too, and the old message always blamed the power supply. | 1, 2, 6 |
| 9 | `[system] log_file` and `DIAG=1` | Troubleshooting without a serial console or HDMI: log to the SD card (temperature, clock, throttling, load per core, underruns, I2C errors, output level) or counters on the display. | 3, 8 |
| 10 | Idle CPU cores rest | All task loops polled at full speed. They now rest in WFE between polls, which lowered the temperature of a Pi 5 at 1800 MHz from 85 C to 70 C (AArch64; no effect on AArch32). | 9 |
| 11 | Output level in the log | Peak and average level and samples over full scale per synth, for setting the gains. | 9, 10 |
| 12 | `[control] mister_extended_synths` | Optional: the existing MiSTer menu entries select all four engines, without changes to MiSTer cores. Off by default. | 9 |

### For the Raspberry Pi 5

| # | Change | Why it matters | Needs |
| --- | --- | --- | --- |
| 13 | `BOARD=pi5` | Builds `kernel_2712.img` for the Cortex-A76 (AArch64 only), with a `[pi5]` section in `config.txt`. | 1, 2 |
| 14 | UART0 for GPIO MIDI, I2C initialization | Circle's default serial device on the Pi 5 is the debug UART connector, so MIDI on GPIO 15 was never received. The RP1 I2C controller needs `Initialize()`. | 13 |
| 15 | Button polling from the main loop | Circle has no `CUserTimer` on the Pi 5. | 13 |
| 16 | I2C bus recovery and MiSTer reads (Circle patch) | See [Technical notes](#technical-notes). Without it, the display stops and the MiSTer menu does not work as soon as a core with an MT32-pi runs. | 9, 10, 14 |

The Circle changes are patches that the Makefile applies when Circle is configured. The bus recovery in `circle-51-rp1-i2c-fixes.patch` may be interesting for Circle itself.

Two bugs that this work also ran into were fixed in mt32-pi-nuked first, and its version is used here: the size of the audio task's integer buffer, and the saturation of the 24-bit output (samples over full scale wrapped around, heard as cracks).

## Building

```bash
make BOARD=pi5
```

This produces `kernel_2712.img`. In a build tree that was configured before, run `make mrproper` first, so that Circle is patched and configured again.

## SD card

The Pi 5 keeps its firmware in EEPROM, so besides `kernel_2712.img` only these files from Circle's `boot/` directory are needed (`make -C external/circle-stdlib/libs/circle/boot firmware` downloads them):

```text
bcm2712-rpi-5-b.dtb
bcm2712d0-rpi-5-b.dtb
overlays/bcm2712d0.dtbo
```

`config.txt` needs a `[pi5]` section (included in `sdcard/config.txt`):

```ini
[pi5]
kernel=kernel_2712.img
kernel_address=0x80000
```

The card must be formatted FAT32. Cards over 32 GB come formatted as exFAT, which neither the Raspberry Pi bootloader nor mt32-pi can read; Raspberry Pi Imager's "Erase" option formats a card as FAT32.

## Configuration notes

- **I2C speed with a MiSTer HAT:** set `i2c_baud_rate = 100000`. The Pi 5's I2C controller uses a fixed, short fast-mode clock for any rate other than exactly 100000 or 1000000 Hz, which the MiSTer cannot read (the MT32-pi OSD menu does not appear).
- **Audio:** the Pi 5 has no headphone jack; use `output_device = i2s`.
- **Cooling without a fan:** `arm_freq=1500` (the Pi 5's minimum) in the `[pi5]` section of `config.txt` is enough for Nuked-SC55. Firmware throttling starts at 80-85 C. When changing the clock, watch for the underrun warnings on the display.
- **SC-55 delay:** `buffer_ms` in `[sc55]` sets how far the emulation runs ahead of playback. `0` is automatic: 30 ms on the Pi 5, 512 native frames (about 8 ms) on earlier models. Increase it (20 to 1000) if "SC-55 underrun!" appears on the display.
- **Troubleshooting log:** `log_file = on` in `[system]` writes the system log to `mt32-pi-log.txt` on the SD card. Building with `DIAG=1` shows counters and the latest log message on the display instead of the normal UI.
- **MiSTer core reset:** when the core resets, the MiSTer's OSD selection (MT-32 or SoundFont) is applied again, as on earlier models, so a synth chosen with the buttons (e.g. SC-55) is replaced.

## Suggested gains

The engines differ in level. These gains bring them to about the same loudness with a little headroom:

| Engine | Option | Suggested | Default | Median loudness | Songs reaching full scale |
| --- | --- | ---: | ---: | ---: | ---: |
| Munt | `[mt32emu] gain` | 1.1 | 1.0 | -20.2 LUFS | 11% |
| Nuked-MT32 | `[nuked_mt32] gain` | 1.1 | 1.0 | -21.0 LUFS (1.07), -19.8 (2.04) | 7% (1.07), 20% (2.04, by at most 0.8 dB) |
| Nuked-SC55 MkII | `[sc55] gain` | 1.5 | 1.0 | -21.2 LUFS | 10% |
| FluidSynth | `[fluidsynth] gain` | 0.35 | 0.2 | -21.5 LUFS | 10% |

They come from 54 MT-32 and 164 General MIDI songs of DOS games: up to 60 seconds of each, rendered on a PC with the same engine code (FluidSynth 2.2 instead of 2.6, with the 8MBGM Enhanced 1.8 SoundFont; other SoundFonts differ), and measured with BS.1770 integrated loudness. At gain 1.0 the MT-32 engines are within about 1 dB of each other, and SC-55 is 3.6 dB quieter than MT-32 in games that support both. In about 35 minutes of music from real games on the MiSTer at gain 1.0 (0.4 for FluidSynth), the highest peaks were +0.1 dBFS (Munt), -1.5 (Nuked-MT32), -5.3 (SC-55) and -3.0 (FluidSynth). Games differ far more than engines, by up to 16 dB, so a quiet game stays quiet. The loudest SC-55 and FluidSynth songs exceed full scale by several dB and clip.

## Test status

Hardware: Raspberry Pi 5 (2 GB) under a MiSTer user port HAT with a PCM5102A I2S DAC, SSD1306 128x64 OLED and simple buttons; small heatsink, no fan; MiSTer ao486 and z486 cores.

| Tested | Result |
| --- | --- |
| This version (v1.1.0 base): Munt, FluidSynth, Nuked-SC55 MkII, Nuked-MT32 1.07 with RV2 reverb, at `arm_freq=1500`, three logged sessions of 9, 19 and 76 minutes (the last one 78 automatic core starts) | No dropouts after startup, no throttling, at most 59 C; SC-55 core about 71% busy, Nuked-MT32 about 35%, Munt up to 10% |
| Buttons, OLED, MiSTer menu in both directions | Work |
| Earlier version: SC-55 for 2 hours, `arm_freq=1500`, `buffer_ms=30` | 60-62 C, SC-55 core about 70% busy, no dropouts, buffer never below full |
| Earlier version: SC-55 for 90 minutes at 1800 MHz, before the throttling-check fix | 78-80 C, no dropouts |
| Earlier version: Munt at 1800 MHz, no heatsink, before the idle rest | 85-86 C within 5 minutes, firmware throttling |

"Earlier version" is the same work based on mt32-pi-nuked of 2026-09-27, before the native RV2 reverb and the SC-55 filters and resampler.

Known issue: switching SoundFonts logs heap warnings (`zoneallocator`: already-freed block, bad magic number). Playback continued in the tests; the cause has not been investigated.

Not tested at all on the Pi 5: USB, networking, HDMI audio, SC-55 MkI. The changes for earlier models have been compiled (`pi4-64` only), but not run on them.

## Technical notes

- MIDI over GPIO uses UART0 (GPIO 14/15). On the Pi 5, Circle's default serial device is the separate debug UART connector.
- The MiSTer's I2C slave acknowledges the last byte of a read, colliding with the master's NACK. The Pi 5's DesignWare controller treats this as lost arbitration, aborts, drops that byte and leaves the slave holding SDA low; all later transfers fail. The Circle patch recovers the bus and returns the bytes received, and mt32-pi reads one spare byte. The BSC controller of earlier models ignores the collision.
- WFE alone does not keep a core asleep after it has taken a spin lock (the exclusive store sets its own event register), so idle rests are timed against the generic timer counter. The timer's event stream wakes each core about every 20 us.
- Sending a full OLED frame takes about 90 ms at 100 kHz. On the Pi 5, I2C transfers wait for events instead of polling (part of the Circle patch, enabled by `g_bI2CMasterIdleWait`).

## Credits and licenses

| Component | Authors | License |
| --- | --- | --- |
| [mt32-pi](https://github.com/dwhinham/mt32-pi) | Dale Whinham and contributors | GPL-3.0-or-later |
| [mt32-pi-nuked](https://github.com/odiaboeeu/mt32-pi-nuked) | odiaboeeu | GPL-3.0 |
| [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) | nukeykt and contributors; optimization work by J.C. Moyer (per the mt32-pi-nuked README) | GPL-2.0-or-later, see below |
| [Nuked-MT32](https://github.com/nukeykt/Nuked-MT32) | nukeykt; MCS-96 core from MAME by Olivier Galibert and R. Belmont; [fork](https://github.com/keithadler/Nuked-MT32) by Keith Adler | GPL-2.0-or-later; MAME parts BSD-3-Clause |
| [Munt](https://github.com/munt/munt) (mt32emu, SRCTools) | Dean Beeler, Jerome Fisher, Sergey V. Mikayev | LGPL-2.1-or-later |
| [FluidSynth](https://github.com/FluidSynth/fluidsynth) | Peter Hanappe and others | LGPL-2.1-or-later |
| [Circle](https://github.com/rsta2/circle), [circle-stdlib](https://github.com/smuehlst/circle-stdlib) | Rene Stange; Stephan Mühlstrasser | GPL-3.0-or-later; GPL-3.0. newlib consists of code under several free licenses |
| [inih](https://github.com/benhoyt/inih) | Ben Hoyt | BSD-3-Clause |

The changes in this branch are under the same license as the files they modify; new files are GPL-3.0-or-later like mt32-pi. They were made with the help of an AI coding assistant (see the commit trailers) and tested on the hardware above.

**Nuked-SC55 license notices:** nukeykt relicensed Nuked-SC55 from the original MAME license (non-commercial) to GPL-2.0-or-later on 2026-09-03. The submodule commit used by mt32-pi-nuked (`3dafca6`) was branched off before that, so its files still carry the old notices. Merging `nukeykt/Nuked-SC55` `master` into it is conflict-free, updates every notice, and also brings in a fix of two values in the PCM interpolation table. Until the submodule points to such a commit, this branch distributes source code only, no kernel images.

No Roland ROMs, SoundFonts or other copyrighted data are included; see the README for the files that users must supply.

# T-DSP Software

Firmware and software for the [T-DSP](https://www.t-dsp.com) audio hardware ecosystem.

T-DSP is open-source audio hardware built around the Teensy Audio Library — Teensy 4.1 + ESP32 + the TAC5212 codec, with swappable I/O modules. This repository hosts the firmware that runs on each T-DSP product, plus shared libraries and applications built on top.

> Looking for the hardware? See [t-dsp.com](https://www.t-dsp.com) and the KiCad repos linked below.

## The mix-kit firmware

[`firmware/mix-kit/`](firmware/mix-kit/) is the flagship: **one configurable firmware** that consolidates every synth engine, mixer, and board variant into a single source tree. Instead of a separate firmware per board, you flash the right *capabilities + synth + defaults* to each unit — configurable down to the serial number — so you never run out of Teensy memory carrying features a given board doesn't use.

- **Synth engine** is a build-time choice (memory-exclusive, one per binary): Dexed (DX7 FM), Plaits, OPLL / ymfm FM, Rings, virtual-analog, SF2 / TSF sampled General-MIDI, and more — each a PlatformIO env in [`firmware/mix-kit/platformio.ini`](firmware/mix-kit/platformio.ini).
- **Board profile** — one reviewable header per physical board ([`firmware/mix-kit/include/boards/`](firmware/mix-kit/include/boards/)) declares that board's capabilities (Bluetooth, S/PDIF, DIN vs USB MIDI, mic preamp, line vs balanced in, headphone vs line out), roles, and power-on defaults (master volume, filters, tempo). Select one with `-D TDSP_BOARD_HEADER="boards/<board>.h"`; anything a header leaves unset falls through to firmware defaults.
- **Serial-targeted flashing** — [`tools/boards.tsv`](tools/boards.tsv) maps each board's USB serial number to its env + board profile, and `python tools/flash.py --serial <n> --upload` builds and flashes the matching image.
- **Lean vs dev builds** — developer bench diagnostics (self-tests, capture probes, sweeps) are opt-in; a product build sets `-D TDSP_DIAGNOSTICS=0` to compile them out.

```bash
cd firmware/mix-kit
pio run -e teensy41_dexed_pool                       # build a synth env
python ../../tools/flash.py --list                   # show the serial -> firmware map
python ../../tools/flash.py --serial 18402920 --upload   # flash the matching board
```

## Repository layout

```
t-dsp_software/
├── firmware/                 ← promoted, "ready" firmware
│   └── mix-kit/              ← the flagship configurable firmware
│       ├── src/                  synth backends, players, control protocol
│       ├── include/boards/       one profile header per physical board
│       └── platformio.ini        the synth/env build matrix
├── projects/                 ← SPIKES: experiments, board bring-up, feature proofs
│   ├── hello-world/              setup guide + sanity check — start here
│   └── ...                       per-feature spikes (S/PDIF, ESP32 BT, synth bring-ups…)
├── app/tdsp-control/         ← Expo control app (BLE / USB Web-Serial UI)
├── lib/                      ← shared TDsp* libraries (used across projects)
├── tools/                    ← flash.py, boards.tsv, asset sync, fetchers
├── LICENSE                   ← MIT
└── README.md
```

**Workflow: spike, then graduate.** `projects/` is the testing ground — prototype a
feature or bring up a board as a self-contained PlatformIO spike there. Once it's
proven, fold it into `firmware/mix-kit` behind a build flag and/or board profile.
Shared code lives at the repo root in `lib/` and is referenced via `lib_extra_dirs`
in each project's `platformio.ini`.

## Getting Started

First time here? Head to [hello-world](projects/hello-world/) for a complete setup
walkthrough — installing PlatformIO, configuring VS Code, and flashing your first
Teensy.

For the mix-kit, the workflow is:

```bash
cd firmware/mix-kit
python -m platformio run -e teensy41_dexed_pool     # build a synth env
python -m platformio run -e teensy41_dexed_pool -t upload   # or: tools/flash.py --serial <n> --upload
python -m platformio device monitor                 # serial monitor
```

## T-DSP Hardware Repositories

Hardware schematics, PCBs, and BOMs are in their own KiCad repos under the [T-DSP organization](https://github.com/t-dsp). The active hardware projects:

| Repository | Description |
|------------|-------------|
| [t-dsp_core](https://github.com/t-dsp/t-dsp_core) | 4-layer audio backplane — Teensy 4.1 + ESP32 + TAC5212 codec module |
| [t-dsp_desktop_pro](https://github.com/t-dsp/t-dsp_desktop_pro) | 8-layer desktop backplane with USB Audio, MIDI, and S/PDIF |
| [t-dsp_tac5212_pro_audio_module](https://github.com/t-dsp/t-dsp_tac5212_pro_audio_module) | TAC5212 stereo codec module — ADC/DAC, mic preamp, headphone amp |
| [t-dsp_tac5212_audio_shield_adaptor](https://github.com/t-dsp/t-dsp_tac5212_audio_shield_adaptor) | Teensy Audio Shield-style adaptor for the TAC5212 module |
| [t-dsp_io_2x2_combo](https://github.com/t-dsp/t-dsp_io_2x2_combo) | Balanced 2-in/2-out interface — XLR outputs, combo XLR/TRS inputs |
| [t-dsp_mic_array_module](https://github.com/t-dsp/t-dsp_mic_array_module) | Microphone array module |

The current dev target for this repository is the **Teensy 4.1** paired with the **TAC5212 Pro Audio Module**, on the **TAC5212 Audio Shield Adaptor** carrier.

## License

[MIT](LICENSE) — © 2026 Jay Shoemaker / T-DSP

The hardware designs in the sibling KiCad repositories are licensed under CC BY-NC-SA 4.0; the software in this repository is MIT.

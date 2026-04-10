# T-DSP Software

Firmware and software for the [T-DSP](https://www.t-dsp.com) audio hardware ecosystem.

T-DSP is open-source audio hardware built around the Teensy Audio Library — Teensy 4.1 + ESP32 + the TAC5212 codec, with swappable I/O modules. This repository hosts the firmware that runs on each T-DSP product, plus shared libraries and applications built on top.

> Looking for the hardware? See [t-dsp.com](https://www.t-dsp.com) and the KiCad repos linked below.

## Projects

| Project | Description |
|---------|-------------|
| [hello-world](projects/hello-world/) | First-time setup guide and a minimal Teensy 4.1 sketch to verify your dev environment, board, and toolchain. **Start here.** |
| [t-dsp_tac5212_audio_shield_adaptor](projects/t-dsp_tac5212_audio_shield_adaptor/) | Firmware for the TAC5212 Audio Shield Adaptor — Teensy 4.1 carrier for the T-DSP TAC5212 Pro Audio Module. USB Audio class device with stereo DAC playback, PDM mic + line-in capture, host volume tracking, and live monitoring. |

## Repository Structure

```
t-dsp_software/
├── projects/                 ← individual PlatformIO projects per board/product
│   ├── hello-world/          ← setup guide + sanity check
│   └── t-dsp_tac5212_audio_shield_adaptor/
│       ├── src/
│       └── platformio.ini
├── lib/                      ← shared libraries (used across projects)
├── LICENSE                   ← MIT
└── README.md
```

Each project under `projects/` is a self-contained PlatformIO project with its own `platformio.ini`, `src/`, and `README.md`. Shared code lives at the repo root in `lib/` and is referenced via `lib_extra_dirs` in each project's `platformio.ini`.

## Getting Started

If this is your first time, head to [hello-world](projects/hello-world/) for a complete setup walkthrough — installing PlatformIO, configuring VS Code, and flashing your first Teensy.

For experienced PlatformIO users, the workflow is:

```bash
cd projects/<project-name>
python -m platformio run                    # build
python -m platformio run --target upload    # upload to Teensy
python -m platformio device monitor         # serial monitor
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

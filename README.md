# T-DSP Software

Firmware for the [T-DSP](https://www.t-dsp.com) product ecosystem. Each board/product has its own PlatformIO project under `projects/`, with shared libraries in `lib/`.

## Repository Structure

```
t-dsp_software/
├── projects/          ← individual PlatformIO projects per board/product
│   └── hello-world/   ← Teensy 4.1 hello world
├── lib/               ← shared T-DSP libraries
└── .gitignore
```

## Getting Started

### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) (`pip install platformio`)
- If `pio` is not on your PATH, use `python -m platformio` instead

### Build & Upload

```bash
cd projects/hello-world
python -m platformio run                          # build
python -m platformio run --target upload          # upload to Teensy
```

### Serial Monitor

```bash
cd projects/hello-world
python -m platformio device monitor
```

Or in VS Code: `Ctrl+Shift+P` → **PlatformIO: Serial Monitor**

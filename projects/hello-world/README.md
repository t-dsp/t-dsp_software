# T-DSP Hello World — Setup Guide

This is the **first project** to build for any new T-DSP development environment. It does two things:

1. Walks you through installing the toolchain (PlatformIO + VS Code) on a fresh machine
2. Verifies your Teensy 4.1 is connected and working — the onboard LED blinks and serial prints a heartbeat

If this project builds, uploads, and prints to the serial monitor, you're ready to move on to a real T-DSP project like [t-dsp_tac5212_audio_shield_adaptor](../t-dsp_tac5212_audio_shield_adaptor/).

---

## What you need

- A **Teensy 4.1** board
- A **micro-USB** cable (data, not just power)
- A computer running **Windows**, **macOS**, or **Linux**
- About 15 minutes for first-time setup

---

## 1. Install Python (if you don't have it)

PlatformIO ships as a Python package. If you don't have Python:

- **Windows:** Install from [python.org](https://www.python.org/downloads/) or via the Microsoft Store. Make sure to check "Add Python to PATH" during installation.
- **macOS:** `brew install python` or use the system Python.
- **Linux:** Use your distro's package manager (e.g., `sudo apt install python3 python3-pip`).

Verify with:

```bash
python --version
pip --version
```

## 2. Install PlatformIO Core (CLI)

```bash
pip install platformio
```

Verify:

```bash
python -m platformio --version
```

You should see `PlatformIO Core, version 6.x.x` or later.

> If `pio` is not on your PATH after install, just use `python -m platformio` everywhere — that always works regardless of PATH setup.

## 3. Install VS Code + the PlatformIO IDE extension (recommended)

You can use any editor, but VS Code with the PlatformIO IDE extension gives you syntax highlighting, IntelliSense, build/upload buttons, and a built-in serial monitor.

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Open VS Code and install the **PlatformIO IDE** extension from the marketplace (`Ctrl+Shift+X` → search "PlatformIO IDE" → Install)
3. Restart VS Code when prompted
4. The PlatformIO toolbar appears at the bottom of the window with build/upload/monitor icons

VS Code with the PlatformIO extension will automatically detect this project's `platformio.ini` when you open the folder.

## 4. Install Teensy drivers (Windows only)

On Windows, the Teensy uses HID/serial drivers that are usually installed automatically the first time you plug it in. If you have problems, install [Teensyduino](https://www.pjrc.com/teensy/td_download.html) which includes the drivers.

On macOS and Linux, no drivers are needed.

## 5. Clone this repo

```bash
git clone https://github.com/t-dsp/t-dsp_software.git
cd t-dsp_software/projects/hello-world
```

## 6. Plug in your Teensy

Connect the Teensy 4.1 to your computer with a micro-USB cable. The orange power LED on the Teensy should light up.

## 7. Build and upload

From this folder (`projects/hello-world`):

```bash
python -m platformio run                    # build only
python -m platformio run --target upload    # build and upload
```

Or in VS Code: click the **→** (Upload) icon in the PlatformIO toolbar at the bottom.

The first build will take a couple of minutes — PlatformIO downloads the Teensy platform, GCC ARM toolchain, and Arduino framework. Subsequent builds are much faster.

You should see:

```
========================= [SUCCESS] Took N seconds =========================
```

After upload, the orange LED on the Teensy starts blinking once per second.

## 8. Open the serial monitor

```bash
python -m platformio device monitor
```

Or in VS Code: `Ctrl+Shift+P` → **PlatformIO: Serial Monitor**.

You should see:

```
=============================
  T-DSP Hello World
  Teensy 4.1 is alive!
=============================
T-DSP uptime: 2 seconds
T-DSP uptime: 4 seconds
...
```

**Press `Ctrl+C` to exit the monitor** (or `Ctrl+T` then `Ctrl+H` for the help menu).

## You're ready

If you got the heartbeat in the serial monitor, your dev environment is fully set up. Move on to a real project like [t-dsp_tac5212_audio_shield_adaptor](../t-dsp_tac5212_audio_shield_adaptor/).

---

## Troubleshooting

### Upload fails with "Access is denied" (Windows)

The serial monitor is holding the COM port open. The Teensy Loader cannot reboot the board into program mode while the port is busy.

**Fix:** Close the serial monitor (`Ctrl+C` in that terminal, or close the VS Code monitor tab) before uploading. Or just press the physical **Program** button on the Teensy to manually trigger upload.

### "No Teensy boards were found"

- Verify the USB cable is a **data** cable, not power-only. (Power-only cables are common; if the orange LED is on but the device doesn't show in your USB device list, that's the smoking gun.)
- Try a different USB port.
- Press the program button on the Teensy to force it into bootloader mode.

### `pio` command not found

Use `python -m platformio` instead. The `pip install platformio` command sometimes installs `pio` to a directory that's not on your PATH.

### Build fails on first run

The first build downloads ~200MB of toolchain. If your network drops mid-download, you'll get a corrupted partial install. Fix:

```bash
python -m platformio platform uninstall teensy
python -m platformio run    # downloads fresh
```

---

## What this code does

[src/main.cpp](src/main.cpp) is intentionally minimal:

- Initializes USB serial at 115200 baud
- Waits up to 3 seconds for the host to connect (blinks the LED while waiting)
- Prints a banner
- In `loop()`: blinks the LED at 1 Hz and prints `T-DSP uptime: N seconds` every 2 seconds

If you want to experiment, edit `src/main.cpp`, save, and run `python -m platformio run --target upload` again. PlatformIO only recompiles changed files so iterations are fast.

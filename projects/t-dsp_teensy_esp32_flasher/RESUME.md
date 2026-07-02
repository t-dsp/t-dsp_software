# RESUME: Flash ESP32 "Hello World" through the Teensy — and monitor it without the ESP32's USB

> Paste this whole file back to Claude Code to pick up where we left off.
> **RESOLVED 2026-07-02: touch-free flashing WORKS** with a Teensy 4.1 + the
> `lib/TDspEsp32` driver (DTR/RTS-debounce auto-reset). Validated: 5/5 esptool
> connects + full write-flash w/ hash verify, no buttons, ESP32 USB unplugged.
> This sketch now just instantiates that driver. History below kept for context.

---

## Goal
On the `t-dsp_dual_preamp_lite` / "T-DSP" board (Teensy 4.x + ESP32-DevKitC), do two things **without touching the board and without using the ESP32's own USB**:
1. **Flash** a "Hello World" firmware onto the ESP32 *through the Teensy* (Teensy = USB-serial adapter + auto-reset controller).
2. **Read** the ESP32's serial "Hello World" output back through the Teensy.

## Current status
- ✅ ESP32 hello-world firmware **built and confirmed working** (flashed earlier via the ESP32's own USB / CP210x on COM9; saw `Hello World! #NNN` at 115200).
- ✅ Teensy "smart bridge" firmware **written, builds, flashes**. Bridges USB↔Serial7 and emulates auto-reset on EN/IO0. Source is in this folder (`src/main.cpp`).
- ❌ **Blocked:** a **Teensy 4.0** is installed, but the board is designed for a **Teensy 4.1**. The ESP32↔Teensy nets (pins 28/29/36/37) live on the 4.1's extended rear section, which the shorter 4.0 lacks → those nets connect to nothing.

## THE FIX (when you return)
**Install a Teensy 4.1.** The bridge firmware in this folder should then just work — no code changes.

---

## Root cause (why the 4.0 can't work)
- Teensy **4.1** = long board; pins **24–41** are through-holes on the two **extended rear edges** (28/29 on one edge, 36/37 on the other). All four ESP32-interface nets land there.
- Teensy **4.0** = short board; pins 0–23 match the front, but GPIOs **28, 29, 36, 37 exist only as underside SMD pads**, not edge through-holes in the 4.1 positions.
- 4.0 in a 4.1 footprint: pins 0–23 connect (Teensy runs, USB works), but **28/29 (ESP32 UART) and 36/37 (EN/IO0) connect to nothing.**
- Symptoms explained: Serial7 (pin 28) read **0 bytes** while ESP32 was clearly transmitting; esptool-through-bridge got **"No serial data received."**

---

## VERIFIED pin mapping (read from schematic "Print Schematic.pdf/.tif" — the old doc table was WRONG, it said Serial2 pins 7/8)

| Signal | ESP32 (U16 DevKitC) | Net | Teensy 4.1 digital pin | Teensy port |
|---|---|---|---|---|
| ESP32 **TX** | U0TXD / GPIO1 | `ESP32_IO1` | **28 (RX7)** | **Serial7 RX** |
| ESP32 **RX** | U0RXD / GPIO3 | `ESP32_IO3` | **29 (TX7)** | **Serial7 TX** |
| Reset | EN / CHIP_PU | `ESP32_EN` | **37** (`37_CS`) | driven as GPIO |
| Boot strap | GPIO0 / BOOT | `ESP32_IO0` | **36** (`36_CS`) | driven as GPIO |
| GND | — | GND | GND | shared |

TX/RX are a correct crossover (NOT swapped). Also on these nets: buttons SW3 (BOOT/IO0) + SW4 (EN), caps C31/C32 (0.1µF), and a 6-pin EPROG1 header (GND, IO0, EN, 3.3V, IO1, IO3).
Schematic gotcha: red numbers (20/21/28/29) are symbol pin ordinals; the **teal names** (`28_RX7`, `29_TX7`, `36_CS`, `37_CS`) are the real Teensy digital pins — use the teal names.

---

## Environment / toolchain (Windows 11, PowerShell)
- esptool: `python -m pip install --user esptool` (v5.3.1). Invoke `python -m esptool ...`.
- PlatformIO: `C:\Users\jaysh\AppData\Roaming\Python\Python313\Scripts\pio.exe`.
- Teensy loaders: `C:\Users\jaysh\.platformio\packages\tool-teensy\teensy_loader_cli.exe` and `teensy_reboot.exe`.
- Teensyduino core: `C:\Users\jaysh\AppData\Local\Arduino15\packages\teensy\hardware\avr\1.59.0` (`Serial.dtr()` + `Serial.rts()` both exist — needed for auto-reset emulation).

### COM ports observed
- **COM9** = ESP32 DevKitC CP210x (VID_10C4). Its DTR→IO0 auto-program line does NOT work → flashing over COM9 hits `Wrong boot mode detected (0x13)` unless you do the manual BOOT dance. (Reason we use the Teensy path.)
- **COM8** = Teensy USB serial (VID_16C0). Single "Serial" USB type enumerates as a composite (CDC + vendor/HID) — normal.

### Flash the Teensy (no button press)
```
pio run -d C:\github\t-dsp\t-dsp_software\projects\t-dsp_teensy_esp32_flasher
& "C:\Users\jaysh\.platformio\packages\tool-teensy\teensy_reboot.exe"
& "C:\Users\jaysh\.platformio\packages\tool-teensy\teensy_loader_cli.exe" --mcu=TEENSY41 -w "C:\github\t-dsp\t-dsp_software\projects\t-dsp_teensy_esp32_flasher\.pio\build\teensy41\firmware.hex"
```
(`pio run -t upload` alone opened the GUI loader but didn't push the hex; the reboot+loader_cli two-step is reliable.)

---

## Resume steps (once a Teensy 4.1 is installed)
1. Confirm ports: Teensy on COM8 (VID_16C0); ESP32 CP210x on COM9 (VID_10C4) if its USB is plugged.
2. Build + flash the bridge firmware in this folder onto the Teensy (commands above).
3. **Sanity check monitoring:** open COM8 @115200 with DTR/RTS OFF; look for `Hello World! #NNN` from the ESP32 via Serial7.
4. **Touch-free flash test** through the bridge:
   ```
   python -m esptool --port COM8 --chip esp32 --before default-reset --after hard-reset --baud 115200 write-flash \
     0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
   ```
5. Re-open COM8 to confirm `Hello World` streaming — with the ESP32's own USB unplugged. Done.

### ESP32 binaries / offsets (classic ESP32, board=esp32dev)
- `0x1000` bootloader.bin, `0x8000` partitions.bin, `0xe000` boot_app0.bin
  (`C:\Users\jaysh\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin`), `0x10000` firmware.bin
- Chip: **ESP32-D0WD rev 1.0**, MAC `c4:dd:57:ca:b4:c8`.
- Flaky USB cable earlier caused mid-write "Invalid head of packet"/"No more data" — use a known-good data cable; `--baud 115200` is safest across the bridge.

---

## Original project locations (may be under C:\tmp — sources duplicated below/in-folder)
- Teensy bridge: **this folder** (`projects/t-dsp_teensy_esp32_flasher/`) — also mirrored at `C:\tmp\teensy-esp-flasher\`.
- ESP32 hello-world: `C:\tmp\esp32-hello\` (source below).

## SOURCE — ESP32 hello-world firmware
`platformio.ini`:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_speed = 460800
```
`src/main.cpp`:
```cpp
#include <Arduino.h>
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
uint32_t count = 0;
void setup() { Serial.begin(115200); pinMode(LED_BUILTIN, OUTPUT); delay(200);
  Serial.println(); Serial.println("=== ESP32 Hello World ==="); }
void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.printf("Hello World! #%lu\n", (unsigned long)count++);
  delay(500); digitalWrite(LED_BUILTIN, LOW); delay(500);
}
```

---

## Key gotchas learned
- ESP32 DevKitC on this board: **DTR→IO0 auto-program does NOT work** over its own USB → `0x13`. Manual entry = hold BOOT(SW3), tap EN(SW4), release BOOT. Motivates the Teensy path.
- The ESP32's EN/IO0 are driven by the Teensy (GPIO37/36), not the DevKitC USB-serial.
- Reading ESP32 serial on COM9 directly: set `DtrEnable=$false; RtsEnable=$false` so you don't reset it.
- Teensy `Serial` (USB) baud is virtual; only `Serial7` needs real 115200. Keep esptool at `--baud 115200` so no baud-change step is needed across the bridge.
- Diagnostic tip: a "scan all UARTs" sketch (begin Serial1..Serial8 @115200, print per-port RX counts) proves the physical link — a correctly-wired 4.1 shows bytes on Serial7 (RX pin 28).

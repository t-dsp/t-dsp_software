# TDspProgrammingKit

Drop-in, **touch-free** control of the on-board ESP32 from a Teensy 4.1: boot it, hold
it, reset it, and **reflash it (and the Teensy itself) with no buttons and no
power-cycle**. Header-only; wraps the low-level `TDspEsp32` pin/UART driver with the
field-proven workflow developed in `projects/spike_esp32_bt_spdif_mix` (see that
project's `HANDOFF.md` for the full root-cause story).

Working reference project: **`projects/spike_tdsp_programming_kit`** (minimal, no audio).

---

## 1. Hardware requirement (read this first)

The Teensy must have a **solid connection on all four control lines**:

| Teensy pin | ESP32 pin | purpose |
|---|---|---|
| 37 | EN (CHIP_PU) | reset / hold-run |
| 36 | GPIO0 / BOOT | download strap |
| 28 (RX7) | U0TXD | ESP32 → Teensy serial |
| 29 (TX7) | U0RXD | Teensy → ESP32 serial |

⚠️ **On the current `teensy41_digital_audio_board`, `ESP32_EN` needs a jumper wire from
Teensy pin 37 to the ESP32 EN pin.** The PCB trace is a bad joint — driving pin 37 low
only pulled EN to ~1.8V (above the ~1V reset threshold), so without the jumper the
Teensy can boot/hold the ESP32 but **cannot reset or flash** it. With the jumper,
everything works. (Next spin: solid EN trace + 10k EN pull-up — see
`projects/spike_esp32_bt_spdif_mix/ESP32_WROOM32_DESIGN.md`.)

Requires a **Teensy 4.1** (pins 28/29/36/37 are only on the 4.1's rear edges).

---

## 2. Add it to a project

`platformio.ini` (the repo shares `lib/` across projects):

```ini
[env:teensy41]
platform = teensy
framework = arduino
board = teensy41
monitor_speed = 115200
build_flags = -std=gnu++17
build_unflags = -std=gnu++14
extra_scripts = ../../tools/cores_overlay.py
lib_extra_dirs = ../../lib
lib_ignore = OpenAudio_ArduinoLibrary
```

Then in code:

```cpp
#include <Arduino.h>
#include <TDspProgrammingKit.h>

TDspProgrammingKit kit;   // defaults: EN=37, IO0=36, Serial7 @115200, LED_BUILTIN

void setup() {
  Serial.begin(115200);
  kit.begin();            // resets the ESP32 into its app and HOLDS EN+IO0 high
}

void loop() {
  if (kit.service(Serial)) return;            // flash-mode passthrough owns the loop
  if (Serial.available()) {
    int c = Serial.read();
    if (kit.handleChar(Serial, c)) return;    // g / r / U handled by the kit
    // ...your own command bytes here...
  }
  while (kit.uart().available())              // mirror ESP32 serial to USB
    Serial.write((uint8_t)kit.uart().read());
}
```

That is the whole integration. `service()` returns `true` only while flashing (so your
app pauses); otherwise it returns `false` after ticking the run-mode LED heartbeat.

---

## 3. The commands (over the Teensy USB serial, 115200)

| you send | what happens |
|---|---|
| `g` | Teensy drives the ESP32 into ROM **download** (self-verifying, retries), halts via `onFlashEnter` hook, becomes a raw USB↔ESP32 **passthrough**. LED blinks fast. |
| `@BOOTAPP@` | leave flash mode: Teensy **soft-reboots** (`SCB_AIRCR`) → `setup()` → `kit.begin()` boots the ESP32 into its app. **No power-cycle.** |
| `U` | Teensy jumps into **HalfKay** (program mode) so `teensy_loader_cli -w` uploads with **no PROGRAM button**. |
| `r` | reset the ESP32 into its app. |

**LED:** slow blink (~1.4 s) = running · fast blink (~7 Hz) = flash mode.

---

## 4. Flash the ESP32 (the loop the next agent will use most)

```
1. Send  g   to the Teensy COM port.
2. python -m esptool --chip esp32 --port <TeensyCOM> --baud 115200 \
          --before no_reset --after no_reset write_flash <offsets> <bins>
3. Send  @BOOTAPP@   to the Teensy COM port  (ESP32 back in its app).
```

- **`--before no_reset` is required** — the Teensy already put the chip in download; do
  NOT let esptool do its own reset (its DTR/RTS emulation is unreliable across the bridge).
- **Keep `--baud 115200`** — Serial7 is a fixed-rate UART; esptool's higher-baud
  renegotiation can't cross the passthrough.
- Sanity check without writing: `... flash_id` → returns `ESP32-D0WD ... 4MB`.
- esptool.py lives at `~/.platformio/packages/tool-esptoolpy/esptool.py`; run it with a
  python that has `pyserial` (e.g. `C:\Python313\python.exe` on this box).

## 5. Flash the Teensy (no button)

```
1. Send  U   to the Teensy COM port.
2. teensy_loader_cli --mcu=TEENSY41 -w firmware.hex
```

---

## 6. Gotchas (things that will bite the next agent)

- **Exiting flash mode:** once in `g`, the Teensy no longer echoes commands — every byte
  goes to the ESP32. The *only* ways out are `@BOOTAPP@` or a Teensy reset.
- **`U` under heavy CPU load:** if the host `loop()` is starved (e.g. an audio graph at
  ~200% CPU), command processing lags, so `U` can take several seconds to fire. It still
  works; just wait, or press PROGRAM once. A light loop (like the demo spike) is instant.
- **Flaky USB write:** `teensy_loader_cli` sometimes prints `error writing to Teensy` —
  it already found HalfKay; just run the same command again. (Same as with the button.)
- **Stale A2DP bond after reflashing the ESP32:** the phone won't reconnect. Clear it —
  add `esp.uart().write('f')` behind your own command, and "forget T-DSP" on the phone.
- **Audio projects:** pass `AudioNoInterrupts` so the passthrough isn't starved into
  dropping bytes:
  ```cpp
  kit.onFlashEnter([]{ AudioNoInterrupts(); });
  ```
- Everything is Teensy-4-specific (`_reboot_Teensyduino_`, `SCB_AIRCR`, `IRQ_LPUART7`).

---

## 7. Config (override defaults via the constructor)

```cpp
TDspProgrammingKit::Config c;
c.esp.uart   = &Serial7;   // ESP32 UART0
c.esp.enPin  = 37;
c.esp.io0Pin = 36;
c.ledPin     = LED_BUILTIN;
c.uartIrq    = IRQ_LPUART7; // raised above audio DMA during flash (match your UART)
c.escape     = "@BOOTAPP@"; // soft-reboot token
TDspProgrammingKit kit(c);
```

Verified end to end on 2026-07-04: `kit.begin()` boots the ESP32; `g` →
`reset attempt 1: 129 ROM bytes` → esptool `flash_id` = `ESP32-D0WD / c4:dd:57:ca:b4:c8 /
4MB`; `@BOOTAPP@` → ESP32 back in its app (`boot:0x13`), "T-DSP" advertising.

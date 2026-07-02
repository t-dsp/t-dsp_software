# How the Teensy connects to (and programs) the ESP32

Board: Teensy 4.1 + ESP32-DevKitC (classic ESP32). ESP32 module pin numbers below
are DevKitC header pins.

> **Working flasher + full resume notes:** the touch-free Teensy bridge firmware and
> the verified bring-up log live in
> [`projects/t-dsp_teensy_esp32_flasher/`](../../t-dsp_teensy_esp32_flasher/) — see its
> `RESUME.md`. **Current blocker: a Teensy 4.0 is in the socket; the board needs a 4.1**
> (the ESP32-interface pins 28/29/36/37 only exist as edge through-holes on the 4.1).
>
> **Correction:** an earlier version of the table below had the UART wrong (it said
> Serial2 / pins 7/8). The verified mapping is **Serial7 / pins 28 (RX7) & 29 (TX7)**.

## 1. The whole ESP32 ↔ Teensy interface

The two chips are wired together in four groups:

| Group | Purpose | Lines |
|-------|---------|-------|
| **Programming/control** | Teensy resets the ESP32 into its ROM bootloader and flashes it | EN, IO0/BOOT, UART0 |
| **I2S audio** | ESP32 (A2DP) streams audio to the Teensy | BCLK, LRCLK, DOUT, (DIN), MCLK |
| **VSPI** | control/display bus (ESP32 was also designed to drive an ILI9341) | MOSI, MISO, SCK, CS |
| **I2C** | shared control bus | SDA, SCL |

This doc covers the **programming/control** group — everything needed for the
Teensy to flash the ESP32 over serial. (Audio wiring is in the project README.)

## 2. Programming interface — pinout

The ESP32 ROM bootloader is entered by manipulating two strapping/reset pins and
then talking the **esptool serial protocol** over UART0. All three are wired to
the Teensy:

| Signal | ESP32 (DevKitC) | Net name | Teensy 4.1 pin | Also on board |
|--------|-----------------|----------|----------------|----------------|
| **EN / RESET** | EN/CHIP_PU | `ESP32_EN`  | **pin 37** (`37_CS`) | button **SW4** + C31 0.1 µF to GND |
| **IO0 / BOOT** | GPIO0/BOOT | `ESP32_IO0` | **pin 36** (`36_CS`) | button **SW3** + C32 0.1 µF to GND |
| **ESP32 TXD0** | U0TXD / GPIO1 | `ESP32_IO1` | **pin 28 = RX7** (Serial7 RX) | EPROG1 header |
| **ESP32 RXD0** | U0RXD / GPIO3 | `ESP32_IO3` | **pin 29 = TX7** (Serial7 TX) | EPROG1 header |
| GND | — | GND | GND | common ground |

Notes:
- **UART is crossed** (TX↔RX), as it must be: ESP32 TXD0 → Teensy **RX7 (pin 28)**;
  ESP32 RXD0 ← Teensy **TX7 (pin 29)**. On the Teensy that's the **`Serial7`** port.
- Read the **teal pin names** on the schematic (`28_RX7`, `29_TX7`, `36_CS`,
  `37_CS`) — the red numbers are just symbol pin ordinals, not Teensy pins.
- **These four nets (28/29/36/37) live on the Teensy 4.1's extended rear edges.**
  A Teensy **4.0** lacks those edge through-holes, so on a 4.0 they connect to
  nothing — the ESP32 interface is dead. **This board requires a 4.1.**
- **GPIO0 is a clean strap here** — it is *not* shared with the I2S MCLK
  (`ESP32_MCLK` is a separate net). So driving IO0 for boot mode does not disturb
  the audio clocks.
- `36_CS` / `37_CS`: these Teensy pins were named as SPI chip-selects. **If the
  Teensy runs firmware that drives them as idle-HIGH outputs, they will hold
  IO0/EN high and fight both the SW3/SW4 buttons and any USB-serial auto-reset.**
  Firmware that flashes the ESP32 must own these pins deliberately (drive them),
  and firmware that does *not* want to interfere must leave them as INPUT (hi-Z).

## 3. How flashing works (esptool download mode)

To flash an ESP32 you must get it into the **serial download / ROM-bootloader**
mode, which the chip latches **at reset based on the level of GPIO0**:

- GPIO0 **HIGH** at the rising edge of EN → normal boot (run app).  ← default
- GPIO0 **LOW**  at the rising edge of EN → **serial download mode**.

So the reset-into-bootloader dance is:

```
1. Drive IO0 (GPIO36) LOW          # select download mode
2. Pulse EN  (GPIO37) LOW → HIGH   # reset the chip while IO0 is held low
3. Release IO0 (GPIO36) HIGH/hi-Z  # chip is now in the ROM bootloader
4. Speak the esptool protocol over Serial2:
   - SLIP-framed sync, then flash-begin / flash-data / flash-end commands
   - typically upload a small "stub" first, then write the app image
5. Pulse EN again to reboot into the freshly-flashed app
```

On a normal USB dev board a USB-serial chip does steps 1–3 automatically using its
**DTR/RTS** lines (RTS→EN, DTR→IO0). On this board those two lines are instead
brought to **Teensy GPIO37 (EN) and GPIO36 (IO0)** — so **the Teensy plays the role
of the USB-serial chip's DTR/RTS**, plus there are manual buttons (SW3/SW4) for
doing it by hand.

### Manual (by hand) entry
Hold **SW3 (BOOT/IO0)**, tap **SW4 (EN/RESET)**, release SW3 → download mode.

### Teensy-driven entry (Phase 2 firmware)
```cpp
// Teensy 4.1
constexpr int ESP_EN  = 37;   // ESP32 EN  (active-low reset)
constexpr int ESP_IO0 = 36;   // ESP32 GPIO0/BOOT (LOW = download mode)

void esp32EnterBootloader() {
  pinMode(ESP_EN,  OUTPUT);
  pinMode(ESP_IO0, OUTPUT);
  digitalWrite(ESP_IO0, LOW);   // 1. select download mode
  digitalWrite(ESP_EN,  LOW);   // 2. assert reset
  delay(50);
  digitalWrite(ESP_EN,  HIGH);  //    release reset with IO0 still low
  delay(50);
  digitalWrite(ESP_IO0, HIGH);  // 3. release boot strap
  // Serial7 now talks to the ESP32 ROM bootloader.
}

void esp32Run() {                // reboot into the app
  pinMode(ESP_IO0, INPUT);       // let IO0 float high (normal boot)
  digitalWrite(ESP_EN, LOW); delay(50); digitalWrite(ESP_EN, HIGH);
}
```

`Serial7` on the Teensy 4.1 is **pin 29 = TX7, pin 28 = RX7** — already wired to the
ESP32's RXD0/TXD0. Use 115200 baud (the ROM bootloader default); keeping esptool at
`--baud 115200` avoids a baud-change step across the bridge.

## 4. Implementing the esptool protocol on the Teensy

The actual flash transfer (SLIP framing + the bootloader commands) is a chunk of
code, not a few lines. Don't reinvent it — port a known-good implementation:

- **collin80/GEVCU7** → `src/devices/esp32` — a Teensy 4.x project that already
  flashes an ESP32 over a UART using exactly this EN/IO0 + serial approach.
  https://github.com/collin80/GEVCU7/tree/main/src/devices/esp32
- PJRC forum thread 64005 "Program esp32 from teensy" — background/discussion.
- The firmware image to send is this project's build output
  (`.pio/build/esp32dev/firmware.bin`, plus bootloader + partition table at their
  flash offsets). The Teensy would store these on the SD card or embed them.

## 5. Gotchas that bit us during bring-up

**#1 — Teensy 4.0 vs 4.1 (the current blocker).** The ESP32-interface pins
28/29/36/37 are on the 4.1's extended rear edges. With a **4.0** installed, the
Teensy boots and its USB works (pins 0–23 line up), but Serial7 reads **0 bytes**
while the ESP32 is clearly transmitting, and esptool-through-the-bridge reports
**"No serial data received"** — because those nets connect to nothing. **Fix:
install a Teensy 4.1;** the bridge firmware needs no changes.

**#2 — `0x13` when flashing over the DevKitC USB.** The DevKitC's own
**DTR→IO0 auto-program does not work** on this board, so USB uploads fail with
`Wrong boot mode detected (0x13)` unless you enter download mode by hand
(hold SW3/BOOT, tap SW4/EN, release SW3). This is the whole reason the
Teensy-driven flasher is the intended path.

**#3 — flaky cable mid-write.** A marginal USB data cable caused
"Invalid head of packet" / "No more data" partway through writes. Use a known-good
data cable and keep `--baud 115200` across the bridge.

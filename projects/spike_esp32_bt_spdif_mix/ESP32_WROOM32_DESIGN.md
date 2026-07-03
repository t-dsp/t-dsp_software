# Next spin: replace the ESP32-DevKitC with a bare ESP32-WROOM-32 module

Exact, buildable design. It deletes the DevKitC's onboard circuitry (CP210x,
auto-reset transistors, AMS1117, its strap network) that caused every ESP32
problem in this project, and replaces it with the standard Espressif reference
circuit where the **Teensy is the single master** for reset/boot/programming.

**Firmware is UNCHANGED** — a WROOM-32 is the same ESP32. Same GPIOs, same
`projects/t-dsp_esp32_bt_receiver` A2DP+BLE firmware, same Teensy `TDspEsp32`
bridge driver. Only the board around the module changes (and gets simpler).

---

## 1. Module
- **ESP32-WROOM-32E-N4** (4 MB flash) or **-N8** (8 MB). "-32E" is the current part;
  "-32D"/"-32U" also fine. U = external antenna (skip unless you need range).
- 38-pin castellated module. Keep the antenna end over a **board edge with RF
  keep-out** (no copper/ground pour under the antenna, per Espressif keep-out).

## 2. Power — dedicated 3.3V, sized for BT TX bursts
The DevKitC's AMS1117 + shared 5V rail was marginal. Give the module its OWN rail:

- **LDO: AP2112K-3.3 (600 mA, 250 mV dropout, SOT-23-5)** — preferred (low dropout),
  OR **TLV75733P (500 mA)** if you keep BT TX modest, OR **AMS1117-3.3** only if you
  guarantee ≥4.5 V in (1.1 V dropout is why the DevKitC struggled).
- Fed from the board **5V** rail. Add **R (0Ω/ferrite)** in the 5V feed.
- **Decoupling at the module VDD (pin 2, 3V3):** `10µF` (X5R 0805) **+** `0.1µF`
  (0402/0603) right at the pin, **+** a **22µF bulk** on the LDO output.
- LDO input: `1µF` + `0.1µF`.
- **Bump the 5V rail bulk too** — on this board C14 was only 4.7µF for the whole
  rail; add **100–470µF** near the ESP32 LDO input.

## 3. Reset / boot straps — THE fix (this is what was broken)
Standard Espressif circuit. Both straps get a **real 10k pull-up** (the DevKitC's
IO0 had none — that's the cold-boot "download mode" race).

```
3V3 ── R_EN 10k ──┬── EN (module pin 3, CHIP_PU)
                  ├── C_EN 1µF ── GND        (power-on reset delay)
                  └── ESP32_EN  ── Teensy pin 37   (Teensy drives reset)
       SW_EN (momentary) : EN ── GND          (manual reset button, optional)

3V3 ── R_IO0 10k ─┬── GPIO0 (module pin 25, BOOT strap)
                  ├── C_IO0 0.1µF ── GND      (debounce; OK because IO0 now has a real pull-up)
                  └── ESP32_IO0 ── Teensy pin 36   (Teensy drives boot select)
       SW_BOOT (momentary): GPIO0 ── GND       (manual download button, optional)
```
- **R_IO0 10k is the critical add.** With it, IO0 rises as fast as EN at power-up →
  the ESP32 latches IO0=HIGH → boots the app. No more cold-boot download mode.
- Keep the caps SMALL/matched. Do NOT put a large cap on IO0.

## 4. Programming path — Teensy bridge only, NO CP210x
The Teensy already programs the ESP32 (esptool over the Teensy USB → Serial7,
emulating auto-reset on EN/IO0). Keep that; **omit the CP210x/USB entirely.**

- **UART0:** `GPIO1 (U0TXD)` → **Teensy pin 28 (RX7)**; `GPIO3 (U0RXD)` ← **Teensy
  pin 29 (TX7)**. (Serial7 @115200.)
- **EN** ← Teensy pin 37, **GPIO0** ← Teensy pin 36 (section 3).
- Since the Teensy is the only thing on EN/IO0/UART, there is **no contention** —
  this is what fixes the flaky flashing, the `0x13`, and the dead-CP210x TX loading.
- (Optional) bring EN/IO0/U0TXD/U0RXD/3V3/GND to a **6-pin EPROG header** as a
  fallback for a USB-UART dongle during bring-up. If you populate a header, still
  no permanent CP210x on the board.

## 5. All strapping pins — leave them correct at boot
WROOM-32 samples these at reset. Wrong state = wrong boot. Requirements:
| Pin | Strap | Requirement | Notes |
|-----|-------|-------------|-------|
| GPIO0  | boot mode | **10k pull-up** (see §3) | high=app, low=download |
| GPIO2  | boot mode | must be **low/floating** at boot | OK to use as output after boot (LED heartbeat). Do NOT put a strong pull-up on it. |
| GPIO5  | timing    | internal pull-up; leave | avoid strong external pull-down |
| GPIO12 (MTDI) | **flash voltage** | must be **LOW** at boot | internal pull-down; **never add a pull-up.** In this design GPIO12 = Teensy VSPI MISO — that's fine (idle low-ish), but verify nothing pulls it high. |
| GPIO15 (MTDO) | boot-log | 10k pull-up (or leave) | high=normal |

## 6. Signal connections (identical to today — firmware unchanged)
**Audio I2S — ESP32 = I2S MASTER → Teensy SAI2 slave:**
| ESP32 GPIO | Signal | Teensy pin |
|---|---|---|
| GPIO26 | BCLK (bit clock)  | 4 (BCLK2) |
| GPIO16 | WS / LRCLK        | 3 (LRCLK2) |
| GPIO25 | DOUT (audio →Teensy) | 5 (IN2) |
| GPIO33 | DIN (future TX)   | 2 (OUT2), optional |
- **Do NOT use GPIO0 for MCLK** (it's the BOOT strap). The Teensy SAI2 slave needs
  no MCLK, so leave MCLK unconnected. (This was a latent conflict on the old board.)

**Control (populate only what you use):**
| ESP32 GPIO | Function | To |
|---|---|---|
| GPIO21 / GPIO22 | I2C SDA / SCL | shared control bus (2.2k pull-ups to 3V3) |
| GPIO23/19/18/17 | VSPI MOSI/MISO/SCK/CS | display/control if kept |
| GPIO36/39/27 | rotary encoder A/B/SW | if kept |

**Power/GND:** module pin 1 = GND, pin 2 = 3V3 (from §2 LDO), pin 15/38 = GND, big
ground pour + thermal pad to GND.

## 7. LED heartbeat / status
- GPIO2 heartbeat works (output after boot) but it's a strap — no strong pull-up.
- For a real status LED, prefer a **spare non-strap GPIO** (e.g., GPIO32 free) with a
  resistor+LED, so it's independent of boot straps. Update `LED_PIN` in the ESP32
  firmware if you move it.

## 8. Bill of materials (the ESP32 sub-circuit)
- 1× ESP32-WROOM-32E-N4
- 1× AP2112K-3.3 (or TLV75733P) LDO, SOT-23-5
- Caps: 10µF + 0.1µF (module VDD), 22µF (LDO out), 1µF + 0.1µF (LDO in),
  1µF (C_EN), 0.1µF (C_IO0), 100–470µF (5V bulk)
- R: 10k (R_EN), 10k (R_IO0), 10k (R_GPIO15 opt), 0Ω/ferrite (5V feed),
  2.2k×2 (I2C pull-ups if used)
- 2× momentary SW (EN reset, BOOT) — optional, nice for bring-up
- Optional 6-pin EPROG header (EN, IO0, U0TXD, U0RXD, 3V3, GND)

## 9. What this deletes vs the DevKitC
Gone: CP210x + its USB-C/micro, the 2-transistor auto-reset network, the AMS1117,
the DevKitC's own decoupling/regulator path, and the dual-master strap contention.
Result: **one master (Teensy), deterministic boot, clean flashing, stiff power.**

## 10. Firmware notes (no code changes needed)
- ESP32: `projects/t-dsp_esp32_bt_receiver` runs as-is (GPIO map unchanged).
- Teensy: `projects/spike_esp32_bt_spdif_mix` runs as-is. With the 10k on IO0 you can
  drop the `espBootApp` double-reset workaround back to a single clean reset (or keep
  it — harmless). `TDspEsp32` bridge/reset driver unchanged (pins 36/37/28/29).
- Programming: Teensy bridge (`b` cmd) or the optional EPROG header. `--baud 115200`.

See HANDOFF.md for the full bring-up status and the pending 10k-jumper confirmation test.

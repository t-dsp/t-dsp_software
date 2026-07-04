# T-DSP Bluetooth bring-up — HANDOFF

## ✅ RESOLVED 2026-07-04 — Teensy-power BT audio works + touch-free flash loop works

Powered **entirely from the Teensy USB** (the hard requirement): boots, advertises
"T-DSP", pairs, streams to the TAC5212. And the ESP32 + Teensy both reflash with
**no buttons and no power-cycle.**

### Root cause (found by metering, after a long hunt)
The `ESP32_EN` net had a **bad on-board connection between Teensy pin 37 and the ESP32
EN pin** — a cold joint / high-resistance trace. Measured as a ~12k divider: driving
pin 37 low only pulled EN to **~1.8V**, which is *above* the ESP32 EN reset threshold
(~1V), so **the Teensy could never reset the ESP32** (proved: pin37 low for 5s never
interrupted the A2DP stream). The manual EN button works because it shorts EN straight
to GND. Compounding it, **EN has no working pull-up on this board**, so releasing the
pin let EN collapse to 0V → ESP32 back in reset. (The "1.8V drift" the user saw is the
EN cap C31 + this weak/absent pull-up, NOT an inverting gate — inversion was ruled out.)
- **HW FIX:** jumper **Teensy pin 37 → ESP32 EN** (bypasses the bad trace). Instantly
  fixed boot, hold, AND reset. Next spin: solid EN trace + 10k EN pull-up (see
  `ESP32_WROOM32_DESIGN.md`).
- **FW FIX:** `espBootApp` now **drives EN+IO0 HIGH and HOLDS them** (never `release()`s)
  so the ESP32 stays out of reset / in its app.
- **IO0 (pin36 → ESP32 GPIO0) is native/good** — no jumper needed.
- `flash_id` over the bridge confirms the ESP32: **ESP32-D0WD, MAC c4:dd:57:ca:b4:c8, 4MB.**

### Touch-free flash loop (Teensy COM11 @115200; no BOOT/EN/PROGRAM, no power-cycle)
- **Flash the ESP32:**
  1. `g` → Teensy drives the ESP32 into ROM download (self-verifying: re-pulses EN until
     the ROM banner responds — robust against a loose jumper), halts audio
     (`AudioNoInterrupts`, else the 213% audio load starves the passthrough → dropped
     bytes), and becomes a raw USB↔ESP32 passthrough. **LED blinks fast = flash mode.**
  2. `esptool --chip esp32 --port COM11 --baud 115200 --before no_reset --after no_reset
     write_flash <offsets> <bins>`
  3. `@BOOTAPP@` → Teensy soft-reboots (`SCB_AIRCR`) → `espBootApp` boots the ESP32 into
     the new app → audio back. **No power-cycle.**
- **Flash the Teensy:** `U` → jumps to HalfKay (program mode) → `teensy_loader_cli -w`
  (no PROGRAM button; retry on the occasional flaky-USB "error writing").
- Stale bond after a flash → `F` on the Teensy + "forget T-DSP" on the phone, then pair.

### Why the old bridge (`b`, esptool DTR/RTS auto-reset) was unreliable
The DTR/RTS reset emulation never landed cleanly; the deterministic `g` path (Teensy
resets the ESP32 itself, then just pipes bytes) is what made flashing reliable.

---

Self-contained brief for continuing the ESP32 A2DP Bluetooth work on the
`teensy41_digital_audio_board`. Historical bring-up detail below.

## The system
`phone --A2DP("T-DSP")--> ESP32-DevKitC (I2S master, 44.1k) --SAI2--> Teensy 4.1
--> async resampler --> TAC5212 DAC --> OUT1/OUT2`. Both chips are on one board.
The Teensy owns the ESP32's reset/boot straps (pin37=EN, pin36=IO0) and its UART0
(Serial7, pins 28/29), so it can program + reset the ESP32. The ESP32 also has its
own USB (CP210x, ~COM9). Board schematic: `C:\github\kicad\teensy41_digital_audio_board`.

## Firmware (all builds clean; pio at reference_pio_cli path)
- **Teensy:** `projects/spike_esp32_bt_spdif_mix/` — BT + S/PDIF-loopback mix into
  the TAC5212, PLUS ESP32 reset/bridge control. THIS is the active Teensy firmware.
- **ESP32:** `projects/t-dsp_esp32_bt_receiver/` — A2DP sink + BLE control +
  persistent pairing + on-demand forget + auto_reconnect + LED heartbeat (GPIO2).
- Committed on branch `cloud-relay-prototype`: `2cb321c`, `21aef2c`, `4e4b95e`.
  (The ESP32 BLE service + `app/tdsp-control` Expo app are ANOTHER agent's — don't
  touch `app/`. Coordinate before committing `t-dsp_esp32_bt_receiver`.)

## STATUS: what works and what doesn't
- ✅ **WORKS in "ESP32-USB" power config** (ESP32 on its own USB = COM9, Teensy runs
  off the board rail): ESP32 boots, "T-DSP" advertises, phone pairs, **audio plays
  end-to-end**. Proven many times.
- ❌ **FAILS in "Teensy-USB" power config** (Teensy on USB = COM11, ESP32 on the board
  rail): the ESP32 never advertises "T-DSP", regardless of firmware. **This config is
  the user's hard REQUIREMENT** (power via one connector; ESP32 fed from the rail).

## ROOT CAUSES FOUND (2 confirmed, 1 suspected)

### 1. IO0 boot-strap RC race at cold power-up  ← the key finding, schematic-confirmed
`ESP32_IO0` has **no external pull-up** (only the ESP32's weak internal ~45k) but
carries **C32 = 0.1µF** to GND (button debounce for SW3). `ESP32_EN` has a **10k**
pull-up + C31 (0.1µF). So at COLD power-up IO0 rises ~4.5ms while EN rises ~1ms →
the ESP32 **samples IO0 LOW at the EN rising edge → cold-boots into ROM DOWNLOAD
mode** (silent, LED "solid", no "T-DSP"). A reset at steady state (IO0 already 3.3V)
boots the app fine. **This is exactly the user's "reset works, power-cycle fails."**
- **THE FIX: add a 10kΩ pull-up from `ESP32_IO0` to 3.3V** (or remove C32). Standard
  ESP32 boot circuit. Prevents download mode at the original POR.

### 2. Stale A2DP bond breaks reconnect after any reboot
After a reboot the phone auto-reconnects with its stored bond; the ESP32's reconnect
fails ("connecting… stop") and the sink bounces connected(non-discoverable)/
disconnected so it "never goes back to pairing mode." **FIX = clear the ESP32 bond:
serial `f` on the ESP32, or Teensy `F` (forwards it), or BLE FORGET = `clean_last_connection()`
+ re-enter pairing; the phone must Forget too.** `set_auto_reconnect(true)` is in the
ESP32 firmware (ESP32 initiates the reconnect) but is unverified on the shared rail.

### 3. (SUSPECTED, unconfirmed) board-rail supply integrity
The firmware IO0-recovery (drive IO0 high, settle 400ms, EN-reset ×2) was verified to
RUN but did **not** bring the ESP32 up in Teensy-rail mode. So either the post-boot
re-reset doesn't stick, OR the ESP32 3V3 sags on the board rail. Note: **C14 = 4.7µF
is the ONLY bulk on the whole 5V rail** (POWER sheet), and the DevKitC's onboard
AMS1117 needs ≥~4.4V in. `R14 = 1k` on the TPS2113 ILIM is outside its 20k–210k range
— verify.

## THE RESET RULE (critical, non-obvious)
The ESP32 boots its app ONLY when **IO0 is HIGH at the EN rising edge**. esptool's
working COM9 reset holds **DTR=False (IO0 HIGH)** and pulses **RTS (EN low→high)**.
Several manual resets set DTR=True → IO0 LOW → download mode (silent). In PowerShell:
`$p.DtrEnable=$false; $p.RtsEnable=$true; sleep 200ms; $p.RtsEnable=$false` = boot app.

## PENDING TESTS (do these when back, in order)
1. **10k pull-up on IO0** — jumper 10kΩ (anything 4.7k–47k) across **EPROG1 header
   pin 2 (`ESP32_IO0`) ↔ pin 5 (`ESP32_3.3V`)**. Then power-cycle in **Teensy-USB**
   mode, check phone for "T-DSP". Comes up → root cause #1 confirmed, done (10k next
   spin). Still nothing → root cause #3, go to test 2.
2. **Meter the ESP32 3V3 pin** in Teensy-rail mode, idle vs when the phone connects
   (BT burst). Stiff ~3.3V → not power. Sags <3.1V → confirmed supply issue → dedicated
   ESP32 3.3V LDO + 470–1000µF bulk next spin.
3. **Scope** ESP32 EN, IO0, 3V3 during a power-cycle to see the strap race / ramp directly.

## HOW TO OPERATE (tooling)
- **Ports:** Teensy = "USB Serial Device (COMxx)" (COM11 recently). ESP32 = "CP210x
  (COM9)". Only ONE USB at a time by the user's rule.
- **Flash Teensy:** `teensy_loader_cli --mcu=TEENSY41 -w -v firmware.hex`, press
  PROGRAM. Recurs "error writing" — just retry / press PROGRAM again; if it degrades
  hard, REBOOT THE PC (Windows USB stack). teensy_reboot serial-reset does NOT work.
- **Flash ESP32 — reliable path = DIRECT COM9** (ESP32 on its own USB): manual
  **BOOT+EN** (hold BOOT, tap EN, release BOOT), then esptool `--before no-reset
  --after hard-reset --baud 115200`. Offsets: 0x1000 bootloader, 0x8000 partitions,
  0xe000 boot_app0, 0x10000 firmware.bin (or just firmware at 0x10000). erase-all wipes
  the NVS bond.
- **Flash ESP32 — bridge (Teensy COM11):** send `b` to enter bridge, then esptool on
  the Teensy COM `--baud 115200`. **UNRELIABLE in Teensy-rail mode (drops mid-write)**;
  and in bridge mode the Teensy drives the straps so BOOT+EN fights it. Prefer COM9.
- **Serial mirror is UNRELIABLE in Teensy mode** (unpowered CP210x loads the ESP32 TX
  line) — the phone/BT is the ground truth, not `[esp]` logs.
- **Teensy USB commands (COM11 @115200):** `t`=DAC tone, `a`=BT+SPDIF, `s`=SPDIF-only,
  `m`=BT-only, `x`=toggle SPDIF tone, `r`=reset ESP32 into app, `b`=bridge,
  `P`=ESP32 enter-pairing, `F`=ESP32 forget-bond, `+/-`=vol, `d`=codec dump.
- **ESP32 UART commands (COM9 @115200, or forwarded via Teensy):** `p`=pairing,
  `f`=forget bond + pair, `x`=disconnect, `s`=status.

## NEXT-SPIN HARDWARE CHANGES (summary)
1. **10k pull-up on ESP32_IO0 to 3.3V** (fixes the strap race) — highest priority.
2. **Dedicated ESP32 3.3V LDO** (≥500mA, low-dropout) + **470–1000µF bulk**, off the
   Teensy's heavy loads; feed the ESP32 3V3 pin directly (bypass the DevKitC AMS1117).
3. **Isolate the EN/IO0 reset straps to ONE master** (the Teensy bridge programs the
   ESP32, so the DevKitC's CP210x/auto-reset is redundant and causes contention).
4. Bump the 5V bulk (C14) and verify the TPS2113 ILIM resistor (R14=1k looks wrong).

## Memory pointers (this session's findings)
`project_esp32_bt_link`, `project_esp32_power_contention`, `reference_async_i2s_resampler`.

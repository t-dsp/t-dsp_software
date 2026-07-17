# t-dsp_esp32_bt_receiver

Bluetooth receiver firmware for the **ESP32-DevKitC** on the
`teensy41_digital_audio_board`. The ESP32 is a Bluetooth Classic **A2DP sink**:
a phone connects over Bluetooth, the ESP32 decodes the audio and streams it to
the Teensy 4.1 over I2S, and the Teensy plays it through the TAC5212 DAC.

Alongside the audio, a **control front-end** lets a companion app command the
receiver (pairing / disconnect / forget) and drive the Teensy's `@`-protocol.
The control transport is chosen at **build time** — **BLE** or **WiFi**, never
both. **A2DP audio is present in both builds.**

This is the only **ESP32** project in the repo — it builds with
`platform = espressif32` instead of `platform = teensy`.

## Control transport (build-time choice: BLE *or* WiFi)

| Env | Flag | Control transport | Bluetooth mode |
|-----|------|-------------------|----------------|
| `esp32dev` (default) | `-D TDSP_CTRL_BLE` | BLE GATT service (UUIDs/opcodes in [src/main.cpp](src/main.cpp)) | `BTDM` (dual: A2DP Classic + BLE) |
| `esp32dev_wifi` | `-D TDSP_CTRL_WIFI` | LAN **WebSocket** server at `tdsp.local:81` | `CLASSIC_BT` (A2DP only; BLE RAM freed) |

Exactly one flag must be defined — `src/main.cpp` `#error`s if neither or both are.

Why WiFi drops BLE: the ESP32 has one radio and limited RAM. The WiFi build needs
the WiFi/lwIP/WebSocket stacks, so it starts Bluetooth in **classic-only** mode,
which releases the BLE controller RAM. WiFi and A2DP (Classic) then share the
radio; the coexistence arbiter is biased toward BT (`esp_coex_preference_set(
ESP_COEX_PREFER_BT)`) to protect audio.

**WiFi modem sleep MUST stay enabled** while Bluetooth is up — the arbiter time-slices
the radio using those sleep windows. `WiFi.setSleep(false)` makes IDF `abort()` at WiFi
start ("Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled") — a
boot-loop, verified on hardware. Don't trade it for WS latency.

### Code shape

A transport-agnostic core (A2DP + I2S setup, the `@`-line relays to the Teensy,
and the local A2DP verbs) is shared. The two front-ends sit behind a small
`ControlTransport` interface (`begin` / `loop` / `sendToApp` / `pushStatus` /
`pushSources`), selected by `#if` and reached via the `controlTransport()`
singleton.

## WiFi build

### Setting credentials

Credentials are **secrets** — they live in a **gitignored `.env`** next to
`platformio.ini`, never in the tracked build flags:

```bash
cd projects/t-dsp_esp32_bt_receiver
cp .env.example .env      # then edit:
#   TDSP_WIFI_SSID=MyNetwork
#   TDSP_WIFI_PASS=MyPassword
```

[`tools/load_env.py`](../../tools/load_env.py) (a `pre:` extra_script) reads `.env` at
build time and injects each `KEY=VALUE` as `-DKEY="VALUE"` — always as a C *string*
literal, so a purely-numeric password can't become an integer macro. It logs key names
only, never values. No `.env` = still builds, but emits a `#warning` and the device won't
join a network. Credentials are baked into the image (runtime provisioning is a later
phase), so re-flash to change networks.

Optional overrides (plain build flags, not secrets): `TDSP_WS_PORT` (default `81`),
`TDSP_MDNS_HOST` (default `tdsp`).

> Must be a **2.4 GHz** network — the classic ESP32 has no 5 GHz radio.

### Wire contract

The device is discoverable via mDNS at **`tdsp.local`**, advertising `_ws._tcp`
on port **81**. The app opens a WebSocket and exchanges **TEXT frames**:

**Inbound (app → ESP32)**

| Frame | Meaning |
|-------|---------|
| `@...` | Relayed **verbatim** to the Teensy over UART (same `@`-protocol as Web Serial / BLE `CMD_RELAY_LINE`). e.g. `@VOL=50`, `@SONG=3`, `@DXVOICE=7`, `@GETCAT` |
| `!pair` | Enter A2DP pairing mode (discoverable + connectable) |
| `!reconnect` | **"Connect Bluetooth Audio"** — dial the last bonded phone. Required: nothing auto-reconnects (see *Explicit-only* below), so this is how audio gets started |
| `!forget` | Clear the stored bond, then enter pairing mode |
| `!disconnect` | Drop the current A2DP source |
| `!status` | Reply with the status JSON to the requesting client |
| anything else | Ignored (logged) |

**Outbound (ESP32 → app)**

- Every `@`-line from the Teensy is sent to all clients and **terminated with `\n`**.
  A long line is split into **~1 KB chunks across several WS frames** — so
  **`\n` is the only frame boundary; a client must accumulate until it sees one** and
  must NOT treat one frame as one line.
- There is still **no `0x1e` framing and no reassembly protocol** (unlike BLE) — the
  chunks are just a byte stream; whole-line semantics are restored by the `\n`.
- Status is a plain JSON line, e.g.
  `{"conn":1,"disc":0,"vol":50,"hpf":0,"mpe":0,"rg":1,"peer":"Pixel 7"}`
- The paired-sources list is broadcast as `@SOURCES=<json array>`.

> **Why chunked (hardware-verified, do not "optimize" back):** sending a whole line in
> one frame **wedges the socket**. `@INSTR` (Dexed's 320-voice list) is ~6.7 KB, which
> overruns the ESP32's lwIP TCP send buffer (~5.7 KB); `WiFiClient::write()` then fails
> with `errno 11 EAGAIN` ("No more processes") and the connection **never writes again** —
> inbound commands still reach the Teensy, but no reply ever comes back. See `wsSendLine()`.

> In the WiFi build the app sends `@`-lines directly, so the ESP32 does not track
> `vol`/`hpf`/`mpe`/`rg` — those status fields report firmware defaults and the app
> owns that state. `conn`/`disc`/`peer` are always accurate.

## Audio path

```
phone --A2DP/Bluetooth--> ESP32 (this firmware, I2S MASTER, 44.1k/16-bit)
                                |
                     I2S out (BCK/WS/DOUT)
                                v
             #ESP32_I2S1 5-pin header on the board
                                v
        Teensy 4.1 SAI2 slave input (AudioInputI2S2slave_F32, pin 5)
                                v
        44.1k -> 48k async resampler (lib/Audio, alex6679)
                                v
                        TAC5212 DAC --> OUT jack
```

## ESP32 <-> Teensy I2S wiring (fixed by the board)

The `#ESP32_I2S1` header ties the ESP32's I2S to the Teensy's **SAI2**. The
ESP32 is I2S **master** (generates BCLK/LRCLK); the Teensy is the slave.

| Signal | ESP32 GPIO | Teensy 4.1 pin (SAI2) | Direction |
|--------|-----------|-----------------------|-----------|
| BCK (bit clock)   | GPIO26 | 4  (BCLK2)  | ESP32 → Teensy |
| WS (LR clock)     | GPIO16 | 3  (LRCLK2) | ESP32 → Teensy |
| DOUT (audio data) | GPIO25 | 5  (IN2)    | **ESP32 → Teensy** (the A2DP audio) |
| DIN               | GPIO33 | 2  (OUT2)   | Teensy → ESP32 (unused; future BT transmit) |
| MCLK              | GPIO0  | 33 (MCLK2)  | unused (Teensy slave needs no MCLK) |

> **Note:** ESP32 `GPIO0` doubles as the boot-strapping pin used when the Teensy
> flashes the ESP32 over serial (a later phase). It is left unconfigured here.

## Build & flash (USB, for now)

```bash
cd projects/t-dsp_esp32_bt_receiver

# BLE control (default/legacy)
python -m platformio run -e esp32dev
python -m platformio run -e esp32dev --target upload

# WiFi WebSocket control (cp .env.example .env and set your creds first)
python -m platformio run -e esp32dev_wifi
python -m platformio run -e esp32dev_wifi --target upload

python -m platformio device monitor         # 115200 baud
```

Both envs share the pinned platform, the `huge_app.csv` 3 MB partition, the
115200 bridge-safe upload speed, and the A2DP library — see
[platformio.ini](platformio.ini) for why each is pinned.

Pair your phone with the Bluetooth device **"T-DSP"** and play audio. The device
name is set by `BT_DEVICE_NAME` in [src/main.cpp](src/main.cpp). Note the sink
boots **idle** (explicit-only): connect it from the app — over WiFi use `!pair`
for a new phone or `!reconnect` for one already bonded (over BLE, the pairing /
reconnect opcodes) — or send `p` over UART.

### Image sizes (3 MB `huge_app` partition)

| Env | Flash | RAM (static) |
|-----|-------|--------------|
| `esp32dev` (BLE) | 1,182,957 B — 37.6% | 48,856 B — 14.9% |
| `esp32dev_wifi` | 1,591,205 B — 50.6% | 70,996 B — 21.7% |

The WiFi build drops the BLE GATT stack but adds WiFi + lwIP + WebSocket + mDNS,
netting ~400 KB more flash. Both fit the 3 MB partition with room to spare.

## Status

- [x] A2DP sink → I2S master scaffold (mirrors the proven
      [esp32_T4_bt_music_receiver](https://github.com/JayShoe/esp32_T4_bt_music_receiver))
- [x] BLE GATT control service (dual-mode BTDM alongside A2DP)
- [x] Build-time selectable control transport (`TDSP_CTRL_BLE` | `TDSP_CTRL_WIFI`)
      behind a `ControlTransport` seam; both envs compile
- [x] WiFi station + WebSocket control server + mDNS (`tdsp.local`), A2DP kept
- [x] **Hardware-validated on jay-mint (2026-07-17)**: joins WiFi, mDNS resolves,
      WS control round-trips (`@VOL=77` → `@STATE` reports `"vol":77`), and the 6.7 KB
      `@INSTR` catalog streams. Two real bugs were found ONLY by flashing:
      `WiFi.setSleep(false)` → IDF abort/boot-loop (modem sleep is mandatory with BT),
      and one-frame-per-line → socket wedge (EAGAIN). Both fixed.
- [ ] **A2DP audio under WiFi load** — still unverified: no phone was paired during the
      test, so radio coexistence has NOT been proven with audio actually streaming.
- [x] App-side `WiFiTransport` — `app/tdsp-control/src/transport.wifi.ts`, selected via
      `createTransport('wifi', host?)`. Speaks this wire contract; typechecks clean.
      Not yet exercised against real hardware, and no UI picker wires it up yet.
- [ ] Runtime WiFi provisioning (creds are build flags today)
- [ ] Cloud-relay agent (remote control beyond the LAN)
- [ ] Phase 2: Teensy programs this firmware over UART2 + EN + IO0
      (esptool protocol; ref [collin80/GEVCU7](https://github.com/collin80/GEVCU7/tree/main/src/devices/esp32))

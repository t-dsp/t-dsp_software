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

### Runtime Wi-Fi settings (no reflash)

The networks the device joins and its own network's name/password live in NVS and are edited
from the control app: **Settings › Device Wi-Fi** (visible when connected over Wi-Fi). Up to 5
saved networks; the device joins the strongest one in range. `.env` values are only
**first-boot seeds**: `TDSP_WIFI_SSID/PASS` is imported once, and `TDSP_AP_SSID/PASS` apply
until the app saves its own. Code: [`src/WifiNetManager.h`](src/WifiNetManager.h).

**Connection policy (why phones on the device network don't stutter).** The AP and the station
share one radio, and a station scan hops channels away from the AP. The old policy
(`WiFi.reconnect()` every 5 s plus arduino-esp32 AutoReconnect, which re-`begin()`s instantly
on "network not found") scanned almost continuously whenever the home network was absent.
Measured with a laptop on the AP and the home network absent (60 s each):

| | ping loss | longest loss | WS control p95 | WS max | WS timeouts |
|---|---|---|---|---|---|
| old policy | 22.8% | 4.2 s | 680 ms | 1510 ms | 7 |
| new policy (phone held) | 0% | none | 87 ms | 121 ms | 0 |

The new policy: AutoReconnect off; one short async scan, then connect straight to the best saved
network by channel+BSSID; exponential backoff 10 s → 5 min when none is in range; **no scanning
and no auto-connect while any phone is on the device network** (the app's *Connect now* and
*Save network* override that on purpose).

HTTP API (form-encoded POSTs, `auth` = device network password or `TDSP_UI_TOKEN`; CORS `*`):
`GET /api/wifi`, `GET|POST /api/wifi/scan`, `POST /api/wifi/save` (ssid, pass),
`/api/wifi/forget` (ssid), `/api/wifi/connect`, `/api/wifi/ap` (ssid, pass). Passwords are
never returned.

**First connect from a phone.** The Android app has the device network name/password built in (EAS
env vars `EXPO_PUBLIC_TDSP_AP_SSID` / `EXPO_PUBLIC_TDSP_AP_PASS`, preview environment, kept equal to
this `.env`; native bundle only, never the web page the device serves). On Android 10+ tapping
**T-DSP network** joins it for the app alone (local module `app/tdsp-control/modules/tdsp-wifi`); on
older builds, iOS or other phones, **Show the Wi-Fi password** shows it plus a QR code any camera can
scan. Change `TDSP_AP_PASS` here and you must update the EAS variable and publish an update too.

**Locked out** (forgot the device network password and no LAN access)? Erase the settings
namespace by erasing NVS through the Teensy bridge, which restores the `.env` defaults:
`esptool.py ... erase_region 0x9000 0x5000` (same `g` passthrough recipe as flashing).

### Access point + hosted web UI (no app, no internet)

**HTTP server is async (ESPAsyncWebServer).** The synchronous arduino `WebServer` served one
connection at a time and waited 5 s on every idle browser preconnect, so a phone's "Sign in to
T-DSP" page never loaded. Measured on the device over the LAN, 5 concurrent requests:

| idle preconnect sockets open | sync WebServer | async server |
|---|---|---|
| 2 | 10 s per file, empty bodies | all small files < 0.7 s |
| 6 | (not tried) | all small files < 1.3 s |

The 259 KB bundle is throughput-limited (~50 KB/s over this LAN, where the device's round trip
averages 50 ms; lwIP's 5.7 KB send buffer is fixed in the arduino-esp32 2.x libs), so it takes 5-7 s
there and should be faster for a phone on the access point (18 ms median round trip).
Note: while Android shows "Sign in", only that window (and the app's own join) reach the device;
Chrome is routed over mobile data.

The WiFi build runs **AP+STA**: it always raises its own access point and *additionally*
joins a saved network when one is in range (see Runtime Wi-Fi settings above). A build without
`.env` falls back to the **public** password `tdsp1234` and warns at compile time; the app shows
a warning until you change it.

It also serves the control app's web export from a **1.375 MB LittleFS** partition over
plain HTTP (port 80), with a captive-portal DNS so a phone that joins the AP is bounced
straight to `http://192.168.4.1/` (also `http://tdsp.local/`). The page defaults to the
Wi-Fi transport at its own host and auto-connects — full UI, zero setup, no app install.

Put a UI on the device (and update it later — no reflash) with
[`tools/push_ui.py`](../../tools/push_ui.py): it runs `expo export --platform web`,
gzips the files (~250 KB total) and POSTs them to `/ui` with the `X-Token` from
`TDSP_UI_TOKEN` in `.env`. `--list` shows what is hosted; `--host tdsp.local` for LAN.

Routes: `GET /*` static (gz twin served with `Content-Encoding: gzip`, unknown →
302 `/`), `GET /ui/list`, `POST /ui/clear` (token), `POST /ui` multipart (token, streamed
into LittleFS). Partition table: [`partitions_ui.csv`](partitions_ui.csv) (2.5 MB app +
LittleFS; NVS offset unchanged so the A2DP bond survives).

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
| `!status` | Reply with the status JSON to the requesting client (includes `"fw":"<build date time>"`) |
| `!fxflash` / `!fxend` | FlasherX tunnel: raw WS<->UART0 byte pipe + `@FXUP` to the Teensy (Teensy self-update over Wi-Fi, `tools/fxflash_wifi.py`) |
| `!tunnel` / `!fxend` | The same raw byte pipe WITHOUT `@FXUP`: the client talks to the Teensy directly, e.g. `@WB` to stage a file on the SD card (`tools/esp32_ota_wifi.py`) |
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

**Reflashing BOTH chips over Wi-Fi (no USB host).** Teensy: `tools/fxflash_wifi.py` streams an
Intel-hex through `!fxflash` into FlasherX (`@FXUP`, Teensy built with `TDSP_FLASHERX`). ESP32:
`tools/esp32_ota_wifi.py` stages the `.bin` on the Teensy's SD card through `!tunnel` + `@WB`,
verifies it with `@CRC`, then `@ESPUP=<path><hexoffset>` has the Teensy reset this chip into
its ROM bootloader and burn the file over the UART (Teensy built with `TDSP_ESP32_SDFLASH`, see
`firmware/mix-kit/src/Esp32SdFlash.inc.h`). The WebSocket drops while the ESP32 is being
written; the tool waits for the AP to come back and confirms via `!status`'s `fw` stamp and the
Teensy's `@ESPUP?` result. Both paths run at the fixed 115200 UART: ~2.5 min per 1.7 MB stage
and again to burn.

**AP-first station policy (2026-10-01).** The access point is the connection that must not
wobble; joining a LAN is a bonus. Because there is one radio, every station scan/attempt pulls
the softAP off its channel, so `WifiNetManager` now: waits 45 s after boot before the first LAN
round (lets the phone join first); never scans/joins while a phone is on the AP (HOLD); after 3
failed rounds in a row pauses LAN attempts for 30 min (`"paused":true` in `/api/wifi`; "Connect
now" / "Save" override); and after every failed round re-applies the AP config so the AP returns
to `TDSP_AP_CHANNEL` (default 1) instead of staying parked on the LAN's channel. Measured on the
jay-mint box with a WPA3-only router refusing association: AP ends on channel 1 (2412 MHz) after
the rounds, 3 rounds then quiet. If you want the LAN to actually join, the router must offer
WPA2 (WPA2/WPA3 mixed); the ESP32 advertises PMF-capable but this router rejects association.

**Liveness (both ends).** The server pings every client every 15 s and drops it after
two unanswered pings (`enableHeartbeat(15000, 4000, 2)` in `WifiControlTransport::begin`);
browsers and React Native answer WS pings in the stack. The app, for its part, sends
`!status` after ~10 s of silence and declares the link dead if nothing at all arrives
within 5 s more, and probes the same way the moment it returns to the foreground
(`app/tdsp-control/src/transport.wifi.ts`). Reason: a phone that slept or roamed left a
**half-open** socket on both sides — the ESP32 kept the zombie in one of its 5 client slots
(and wrote every broadcast to it), while the app saw `readyState OPEN` and silently lost
every command until the OS eventually noticed. Now both notice within seconds, and the app
reconnects in place (no catalog reload) instead of falling back to the connect screen.

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

### Image sizes

| Env | Partition | Flash | RAM (static) |
|-----|-----------|-------|--------------|
| `esp32dev` (BLE) | `huge_app` 3 MB | 1,182,957 B — 37.6% | 48,856 B — 14.9% |
| `esp32dev_wifi` | `partitions_ui` 2.5 MB | 1,671,709 B — 63.8% | 71,900 B — 21.9% |

The WiFi build drops the BLE GATT stack but adds WiFi + lwIP + WebSocket + mDNS + the
HTTP/LittleFS UI host, netting ~500 KB more flash; it gives up 0.5 MB of app partition
to the LittleFS that holds the web UI.

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

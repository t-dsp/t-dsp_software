# Thin-shell app: ship UI without shipping a new app

Goal: new features in tdsp-control without a store release, and a UI that works
with **no internet** (festival). Decided 2026-09-13.

## Facts that shape the answer

- The app is already ~95% web. Native-only pieces: `react-native-ble-plx` (BLE)
  and `react-native-zeroconf` (mDNS discovery). Everything else runs under
  react-native-web today (jay-mint runs it in Chrome).
- `WiFiTransport` (`src/transport.wifi.ts`) already speaks the full `@`-line
  protocol over a WebSocket to the ESP32 (`ws://tdsp.local:81`) from both native
  and browser. The ESP32 WiFi build (`esp32dev_wifi`) is **station mode only**
  today and has no HTTP server.
- `npx expo export --platform web` of the current app = one JS bundle.
  Measured 2026-09-13: 899 KB raw, **241 KB gzipped**. Fits in a ~1 MB
  ESP32 LittleFS partition or trivially on the Teensy SD.
- A page served over **https cannot open `ws://` to a LAN device** (mixed
  content, Chrome/Safari). A Railway-hosted UI therefore cannot control the
  device unless the ESP32 does TLS. That rules Railway out as the runtime host.

## Decision: two delivery paths, one codebase

### Path 1 — EAS Update for the store app (do first, ~1 day)

`expo-updates` is the built-in answer to "update app contents without a new
binary". The app launches the last bundle it has (offline-safe), checks EAS's
CDN when online, swaps on next launch.

1. `npx expo install expo-updates`, then in `app.json`:
   `"updates": { "url": "https://u.expo.dev/<projectId>" }` and
   `"runtimeVersion": { "policy": "fingerprint" }`.
2. One new EAS build (`preview` APK / iOS internal) with updates baked in.
3. Ship features with `eas update --branch preview --message "..."`.
4. Rebuild the binary **only** when native deps change (ble-plx, zeroconf,
   SDK bump). The fingerprint policy refuses a mismatched bundle, so that
   can't be gotten wrong.

Offline behaviour: `checkAutomatically: ON_LOAD` with `fallbackToCacheTimeout: 0`
means no wait at boot when there's no network; the cached bundle runs.

### Path 2 — device-hosted web UI for festivals (no app, no internet)

The ESP32 serves the same web export over its **own access point**. Any phone,
tablet or laptop joins "T-DSP", opens `http://tdsp.local` (captive portal
pushes it), and the page talks to the existing WebSocket relay. Updating the UI
= copying ~250 KB of files to the device, never a reflash.

Firmware work (`projects/t-dsp_esp32_bt_receiver`, WiFi env):
- `WiFi.mode(WIFI_AP_STA)`: AP always up (SSID `T-DSP`, WPA2), STA optional
  for home LAN. DNSServer captive-portal redirect to `tdsp.local`.
- Partition table: replace `huge_app.csv` with a custom table, ~2.5 MB app +
  ~1.3 MB LittleFS. Verify the WiFi env still fits (A2DP is off in this env,
  so it is smaller than the BLE build).
- `ESPAsyncWebServer` (or `WebServer`) serving `/` from LittleFS with
  pre-gzipped assets (`Content-Encoding: gzip`, `Cache-Control: no-cache`
  on `index.html`, immutable on the hashed JS).
- Upload route `POST /update-ui` (multipart, auth token) writing into LittleFS,
  plus `tools/push_ui.py` wrapping `expo export` + upload. Optional: also
  copy the export to the Teensy SD under `/ui/` via `sync_assets.py` so a
  bare-ESP32 can fetch it from the Teensy over `@READ` on first boot.
- Keep the hard rule: A2DP off in every WiFi env (`#error` guard stays).
  Festival WiFi mode has no Bluetooth phone audio; that is the existing
  WiFi-control roadmap trade-off (audio moves to WiFi later / single S3).

App work (small):
- `expo export --platform web` output must be relative-path (`experiments.baseUrl`
  unset, works today) and default the web transport to `wifi` when served from
  the device (`location.host` is `tdsp.local` or `192.168.4.1`).
- Add a `manifest.json` + service worker so the page installs as a PWA;
  after one visit it also opens with the AP down (UI only, no device).

### Rejected

- **WebView shell** (native app loads a remote page): BLE would need a
  JS↔native bridge through the WebView; strictly worse than EAS Update.
- **Railway / any https host as the runtime UI**: blocked by mixed content
  against `ws://` on the LAN; EAS Update already hosts bundles for the app.
- **Pure PWA on phones over BLE**: Web Bluetooth is Android-Chrome only,
  absent on iOS.

## Status (2026-09-13)

- Path 1 **DONE**: expo-updates wired (commit 9de6bfa), channel/branch `preview`, two OTA
  updates published, EAS Android preview APK built (runtime fingerprint `616ec22a…`).
- Path 2 **DONE + HARDWARE-VERIFIED on the jay-mint board**: `esp32dev_wifi` flashed over
  the Teensy `g` bridge. Verified live: AP `T-DSP` broadcasting (BSS `c4:dd:57:ca:b4:c9`),
  STA joined the LAN, `tdsp.local` → 10.0.0.154, mDNS `_http._tcp`, `tools/push_ui.py`
  pushed the 250 KB UI (4 files) which serves gzipped, unknown paths 302 to the AP address
  (captive portal), hosted files survive an app reflash, and WebSocket control on :81 still
  returns `@STATE` from the Teensy alongside the HTTP server.
- **Runtime Device Wi-Fi DONE (commit fc60126), flash-verified:** saved networks + device network
  name/password in NVS, edited in the app at Settings › Device Wi-Fi; station never scans while a
  phone is on the AP. Old policy measured at 22.8% ping loss / 4.2 s outages for a phone on the AP
  when the home network is absent; the new-policy run's data is still on jay-mint (offline).
- **Not yet tested:** a phone actually joining the `T-DSP` AP and the captive portal popping
  (both bench machines are Wi-Fi-only, so joining the AP would have cut the SSH link).
- PWA manifest/service worker: not started, and arguably pointless — the page only has a job
  when the device is powered, and then the device serves it.

### Flashing the ESP32 from jay-mint (recipe that works)

One SSH session, never split: `stty` once, prove the Teensy answers (`@STATE` returns
bytes), `printf 'g'`, `flash_id`, then `write_flash`, with a `trap` sending `@BOOTAPP@`.
**Never run esptool unless `g` has actually taken:** esptool's sync frame is 32 × `0x55`,
and `0x55` is ASCII `U` — the mix-kit's jump-to-HalfKay command. Sending it to a Teensy
that is NOT in passthrough drops the board into its bootloader (recover with
`~/.platformio/packages/tool-teensy/teensy_loader_cli --mcu=imxrt1062 -w <hex>`).

## Order of work

1. EAS Update wiring + one new build (unblocks feature shipping now).
2. ESP32: AP+STA + captive portal + LittleFS static server + upload route.
3. `tools/push_ui.py`; document in README next to `sync_assets.py`.
4. PWA manifest/service worker (nice-to-have).

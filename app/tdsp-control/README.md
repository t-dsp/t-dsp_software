# T-DSP Control

Cross-platform (Android + iOS) phone app to control the T-DSP ESP32 Bluetooth
receiver over **BLE**, while A2DP audio keeps streaming. First capability:
put the receiver into **pairing mode** from your phone.

- Firmware side of the contract: `../../projects/t-dsp_esp32_bt_receiver/src/main.cpp`
- BLE protocol + React hook: [`src/tdspBle.ts`](./src/tdspBle.ts)
- UI: [`App.tsx`](./App.tsx)

## BLE contract (must match firmware)

| Item | UUID | Notes |
|------|------|-------|
| Service | `7a9c0001-4a6e-4b7d-8f1a-2d3c4e5f6a70` | advertised; app scans by this |
| Command (write) | `7a9c0002-…` | 1 byte opcode |
| Status (read/notify) | `7a9c0003-…` | JSON `{"conn":,"disc":,"peer":}` |

Opcodes: `0x01` enter pairing mode · `0x02` end pairing · `0x03` disconnect
source · `0x04` forget device.

## Important: this needs a custom build, NOT Expo Go

`react-native-ble-plx` is a native module, so **Expo Go cannot run this app**.
You build a small custom client once, then iterate normally (`npx expo start`).

### Build an installable Android APK (recommended, works from Windows)

Uses EAS Build (Expo's cloud) — no local Android SDK / Xcode needed.

```bash
cd app/tdsp-control
# one-time: authenticate. Either interactively:
eas login
#   ...or headless with a robot token (do NOT commit it):
#   $env:EXPO_TOKEN = "<token>"     # PowerShell
export EXPO_TOKEN=<token>           # bash

eas init            # creates/links the Expo project (writes extra.eas.projectId)
eas build -p android --profile preview   # -> APK download link; sideload it
```

Install the APK on the phone, open it, turn on Bluetooth, tap **Connect to
T-DSP**, then **Enter Pairing Mode**.

### iOS (later)

```bash
eas build -p ios --profile preview     # needs an Apple Developer account
```

Same JS/TS code — iOS works once Apple signing/credentials are set up.

### Local dev loop (after you have a dev build installed)

```bash
npx expo start --dev-client
```

## Notes

- Node ≥ 20.19.4 recommended (this repo was scaffolded on 20.18.1; EAS uses its
  own Node in the cloud, so cloud builds are unaffected).
- Android 12+ runtime permissions (`BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`) are
  requested on first Connect; the `neverForLocation` flag avoids the location
  permission. iOS uses `NSBluetoothAlwaysUsageDescription` from `app.json`.

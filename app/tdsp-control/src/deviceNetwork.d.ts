// Type declaration so TS resolves `./deviceNetwork` (Metro picks the .native.ts / .web.ts implementation
// at bundle time; this only provides the shared signature).
//
// The T-DSP's own Wi-Fi network (its access point): the name + password this app can show, share as a
// QR code, and (on Android) join by itself. The password is built into the NATIVE app only, from the EAS
// environment variables EXPO_PUBLIC_TDSP_AP_SSID / EXPO_PUBLIC_TDSP_AP_PASS (kept equal to TDSP_AP_SSID /
// TDSP_AP_PASS in projects/t-dsp_esp32_bt_receiver/.env). The web build never contains it: a browser page
// that can reach the device is already on its network. A password changed in Settings > Device Wi-Fi is
// remembered on the phone and wins over the built-in one.
export interface DeviceNetworkCreds { ssid: string; pass: string; source: 'saved' | 'app' }

export function loadDeviceNetworkCreds(): Promise<DeviceNetworkCreds | null>;
export function rememberDeviceNetworkCreds(ssid: string, pass: string): Promise<void>;

// Opens the system Wi-Fi picker (Android). False where unsupported.
export const canOpenWifiSettings: boolean;
export function openWifiSettings(): Promise<boolean>;

// tdsp-wifi — join the T-DSP's own Wi-Fi from the app (Android 10+). See android/.../TdspWifiModule.kt.
//
// Optional native module: builds without it (older APKs, iOS, web) get `wifiJoinSupported === false` and
// the app falls back to showing the password / QR code, so this file is safe to ship over the air.
import { requireOptionalNativeModule } from 'expo';
import { PermissionsAndroid, Platform } from 'react-native';

export type WifiSeen = { ssid: string; rssi: number };

type NativeTdspWifi = {
  isSupported(): boolean;
  join(ssid: string, passphrase: string, timeoutMs: number): Promise<void>;
  release(): Promise<void>;
  scan?(match: string): Promise<WifiSeen[]>;
};

const native = requireOptionalNativeModule<NativeTdspWifi>('TdspWifi');

export const wifiJoinSupported: boolean = (() => {
  try { return !!native && native.isSupported(); } catch { return false; }
})();

// Resolves once the app is bound to the network. Rejects with a plain-language message (out of range,
// declined, timed out). A newer join or releaseWifi() cancels an earlier one.
export async function joinWifi(ssid: string, passphrase: string, timeoutMs = 45000): Promise<void> {
  if (!native) throw new Error("This version of the app can't join Wi-Fi by itself.");
  await native.join(ssid, passphrase, timeoutMs);
}

// Nearby networks whose name contains `match`, strongest first. [] when unsupported.
export async function scanWifi(match: string): Promise<WifiSeen[]> {
  if (!native?.scan) return [];
  return native.scan(match);
}

// Ask (once per app launch) for the permission Android needs to list nearby networks: NEARBY_WIFI_DEVICES
// on 13+, location on 10/11. Android 12 has no permission this app declares for it, so detection is off
// there (joining still works). Resolves true when scanning is allowed.
let askedScanPermission = false;
export async function ensureWifiScanPermission(): Promise<boolean> {
  if (Platform.OS !== 'android' || !native?.scan) return false;
  const v = Number(Platform.Version);
  const perm = v >= 33 ? PermissionsAndroid.PERMISSIONS.NEARBY_WIFI_DEVICES
    : v <= 30 ? PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION : null;
  if (!perm) return false;
  try {
    if (await PermissionsAndroid.check(perm)) return true;
    if (askedScanPermission) return false;
    askedScanPermission = true;
    const r = await PermissionsAndroid.request(perm, {
      title: 'Find T-DSP devices',
      message: 'Lets the app spot a T-DSP’s own Wi-Fi nearby so you can connect with one tap.',
      buttonPositive: 'Allow',
    });
    return r === PermissionsAndroid.RESULTS.GRANTED;
  } catch {
    return false;
  }
}

// Leave the app-only network (no-op when not joined).
export async function releaseWifi(): Promise<void> {
  try { await native?.release(); } catch { /* nothing to release */ }
}

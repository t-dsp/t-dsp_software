// deviceNetwork.native.ts — see deviceNetwork.d.ts. Native only, so the built-in password never ships in
// the web bundle the device serves.
import AsyncStorage from '@react-native-async-storage/async-storage';
import { Linking, Platform } from 'react-native';
import type { DeviceNetworkCreds } from './deviceNetwork.d';

const KEY = 'tdsp.deviceNetwork';

// Inlined at build/update time (babel-preset-expo). Must be read as `process.env.EXPO_PUBLIC_…` literally.
const BUILT_IN_SSID = process.env.EXPO_PUBLIC_TDSP_AP_SSID || 'T-DSP';
const BUILT_IN_PASS = process.env.EXPO_PUBLIC_TDSP_AP_PASS || '';

export async function loadDeviceNetworkCreds(): Promise<DeviceNetworkCreds | null> {
  try {
    const v = await AsyncStorage.getItem(KEY);
    if (v) {
      const j = JSON.parse(v);
      if (j && typeof j.ssid === 'string' && typeof j.pass === 'string' && j.ssid && j.pass) return { ssid: j.ssid, pass: j.pass, source: 'saved' };
    }
  } catch { /* fall back to the built-in credentials */ }
  return BUILT_IN_PASS ? { ssid: BUILT_IN_SSID, pass: BUILT_IN_PASS, source: 'app' } : null;
}

export async function rememberDeviceNetworkCreds(ssid: string, pass: string): Promise<void> {
  try { await AsyncStorage.setItem(KEY, JSON.stringify({ ssid, pass })); } catch {}
}

export const canOpenWifiSettings = Platform.OS === 'android';

export async function openWifiSettings(): Promise<boolean> {
  if (Platform.OS !== 'android') return false;
  // Android 10+ shows a Wi-Fi panel over the app; older versions open the full settings page.
  for (const action of ['android.settings.panel.action.WIFI', 'android.settings.WIFI_SETTINGS']) {
    try { await Linking.sendIntent(action); return true; } catch { /* try the next one */ }
  }
  return false;
}

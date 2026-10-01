// appUpdates.ts — thin wrapper over expo-updates (EAS Update / OTA JS bundles).
//
// The store binary is a shell: it boots the last JS bundle it has cached (offline-safe, so a
// festival with no internet still runs), and when online it checks EAS's CDN on launch and swaps
// the new bundle in on the NEXT launch (app.json: updates.checkAutomatically = ON_LOAD,
// fallbackToCacheTimeout = 0 so a dead network never delays boot). Ship UI with
//   eas update --branch preview --message "..."
// and rebuild the binary only when native deps change (runtimeVersion policy = fingerprint).
//
// On web (jay-mint / Web Serial) and in a dev-client, expo-updates is disabled: every call here
// degrades to "not available" instead of throwing, so the Firmware page can render unconditionally.
import * as Updates from 'expo-updates';
import { DevSettings, Platform } from 'react-native';

function withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
  return Promise.race([p, new Promise<never>((_, rej) => setTimeout(() => rej(new Error('timeout')), ms))]);
}

// Pull-to-refresh: restart the app like a browser refresh. A newer published bundle is looked for
// for at most 4 s (so a festival with no internet never stalls) and run if it downloads in time.
// Reconnecting is App's boot path: back to the last device, unless the user disconnected on purpose.
export async function refreshApp(): Promise<void> {
  if (Platform.OS === 'web') { (globalThis as any).location?.reload(); return; }
  if (Updates.isEnabled) {
    try {
      const r = await withTimeout(Updates.checkForUpdateAsync(), 4000);
      if (r.isAvailable) await withTimeout(Updates.fetchUpdateAsync(), 15000);
    } catch { /* offline or slow: reload the bundle we already have */ }
    try { await Updates.reloadAsync(); return; } catch { /* fall through (dev client) */ }
  }
  try { DevSettings.reload(); } catch {}
}

export interface AppUpdateInfo {
  enabled: boolean;            // false on web / dev-client / builds without updates configured
  channel: string;             // EAS channel the binary was built for (preview / production)
  runtimeVersion: string;      // fingerprint the bundle must match
  updateId: string;            // id of the running bundle ('' = embedded bundle from the binary)
  createdAt: string;           // when that bundle was published
  isEmbedded: boolean;
}

export function appUpdateInfo(): AppUpdateInfo {
  try {
    return {
      enabled: !!Updates.isEnabled,
      channel: Updates.channel || '',
      runtimeVersion: Updates.runtimeVersion || '',
      updateId: Updates.updateId || '',
      createdAt: Updates.createdAt ? new Date(Updates.createdAt).toISOString().slice(0, 16).replace('T', ' ') : '',
      isEmbedded: !!Updates.isEmbeddedLaunch,
    };
  } catch {
    return { enabled: false, channel: '', runtimeVersion: '', updateId: '', createdAt: '', isEmbedded: true };
  }
}

// Check EAS now; if a newer bundle exists, download it and relaunch into it immediately.
// Resolves to a one-line status for the UI. Never throws.
// onDeviceNetwork: the phone is joined to the T-DSP's own Wi-Fi via the app (Android binds ALL of the
// app's sockets to that network, which has no internet), so the update server cannot be reached no
// matter what else the phone is connected to. Say that instead of a raw native error.
export async function checkAndApplyUpdate(onDeviceNetwork = false): Promise<string> {
  if (!Updates.isEnabled) return 'Updates not available in this build';
  if (onDeviceNetwork)
    return 'No internet while connected to the T-DSP network (the app\u2019s traffic is pinned to it). '
         + 'Disconnect App, make sure the phone has internet, then check again \u2014 or just relaunch the app '
         + 'on home Wi-Fi: it fetches updates at startup and applies them on the next launch.';
  try {
    const r = await Updates.checkForUpdateAsync();
    if (!r.isAvailable) return 'Up to date';
    await Updates.fetchUpdateAsync();
    await Updates.reloadAsync();
    return 'Restarting into new version\u2026';
  } catch (e: any) {
    const raw = e?.message || String(e);
    const offline = /network|internet|unreachable|timed? ?out|host|manifest|ENOTFOUND|ECONN/i.test(raw);
    return offline
      ? 'Update check failed: no route to the update server. Make sure this phone has internet (not only the T-DSP network), then try again.'
      : 'Update check failed: ' + raw;
  }
}

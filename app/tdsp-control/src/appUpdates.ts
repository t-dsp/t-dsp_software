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
export async function checkAndApplyUpdate(): Promise<string> {
  if (!Updates.isEnabled) return 'Updates not available in this build';
  try {
    const r = await Updates.checkForUpdateAsync();
    if (!r.isAvailable) return 'Up to date';
    await Updates.fetchUpdateAsync();
    await Updates.reloadAsync();
    return 'Restarting into new version…';
  } catch (e: any) {
    return 'Update check failed: ' + (e?.message || String(e));
  }
}

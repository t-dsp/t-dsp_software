// keepAwake.ts — keep the screen on while the app is connected to a T-DSP.
//
// Why: a phone that dims and sleeps mid-session tears the Wi-Fi link down (Android parks the radio and
// suspends the app's socket; a local-only "T-DSP" network can be dropped outright), and waking back up
// then means a reconnect. Holding a screen wake lock while connected removes the sleep in the first
// place. The user can turn it off in Settings › Connection (see App.tsx); default on.
//
// Native: the ExpoKeepAwake module. It is ALREADY compiled into the APK because `expo` depends on
// expo-keep-awake, but it is nested under node_modules/expo/node_modules, so it isn't importable by
// name here, and adding it to package.json would move it and change the native fingerprint (which
// would orphan the OTA channel). requireOptionalNativeModule() reaches the installed module by its
// registered name without any dependency change, so this ships over the air; a build without it just
// gets a no-op.
// Web: the Screen Wake Lock API (Chrome/Edge/Safari 16.4+; https only, or localhost). The browser
// releases the lock when the tab is hidden, so we re-request it on visibilitychange.
import { requireOptionalNativeModule } from 'expo';
import { Platform } from 'react-native';

type NativeKeepAwake = { activate(tag: string): Promise<void>; deactivate(tag: string): Promise<void> };
const native = Platform.OS === 'web' ? null : requireOptionalNativeModule<NativeKeepAwake>('ExpoKeepAwake');
const TAG = 'tdsp-connected';

let wanted = false;           // the app's current intent
let nativeHeld = false;
let webLock: any = null;      // WakeLockSentinel
let webVisHooked = false;

async function applyNative() {
  if (!native) return;
  try {
    if (wanted && !nativeHeld) { await native.activate(TAG); nativeHeld = true; }
    else if (!wanted && nativeHeld) { nativeHeld = false; await native.deactivate(TAG); }
  } catch { /* unavailable on this build/device: nothing to hold */ }
}

async function applyWeb() {
  if (typeof navigator === 'undefined' || !('wakeLock' in navigator)) return;
  const nav = navigator as any;
  try {
    if (wanted && !webLock) {
      if (typeof document !== 'undefined' && document.visibilityState !== 'visible') return;   // request() throws when hidden
      const lock = await nav.wakeLock.request('screen');
      webLock = lock;
      lock.addEventListener?.('release', () => { if (webLock === lock) webLock = null; });
      if (!wanted) { try { await lock.release(); } catch {} }   // raced with a setKeepAwake(false)
    } else if (!wanted && webLock) {
      const lock = webLock; webLock = null;
      try { await lock.release(); } catch {}
    }
  } catch { /* denied (battery saver, insecure origin): the page just behaves as before */ }
  if (!webVisHooked && typeof document !== 'undefined') {
    webVisHooked = true;
    // The browser drops the lock whenever the tab is hidden; take it back when the tab returns.
    document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible' && wanted && !webLock) void applyWeb(); });
  }
}

// Idempotent: call with the desired state as often as you like.
export function setKeepAwake(on: boolean): void {
  wanted = on;
  if (Platform.OS === 'web') void applyWeb(); else void applyNative();
}

export const keepAwakeSupported: boolean = Platform.OS === 'web'
  ? (typeof navigator !== 'undefined' && 'wakeLock' in navigator)
  : !!native;

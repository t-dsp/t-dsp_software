// constants.ts — non-visual domain constants, formatters, small types, and pure helpers used
// across the T-DSP control surface. Extracted from App.tsx (Phase 1 split). No React/component
// imports (only RN's Platform/Alert for notify + AsyncStorage for the catalog cache), so this is
// safe to import from primitives and App alike.
import { Platform, Alert } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import type { DirPage } from '../transport';
import type { CatalogCache } from '../catalog';

export const EMPTY_DIR: DirPage = { path: '', page: 0, npages: 1, folders: [], carts: [] };

// One /dexed search hit (the device's @dxfind reply, one line per matching cart): `rel` is the
// cart path relative to /dexed (keeps .syx, ready for @DXPICK/@DXVL), `name` the display label,
// `voices` its 32 voice names. The app decides WHICH rows to show (which voices matched vs a
// folder/cart-name-only match), so the firmware stays a dumb line filter.
export type DexHit = { rel: string; name: string; voices: string[] };
export function parseDexFind(text: string): DexHit[] {
  const out: DexHit[] = [];
  for (const l of text.split('\n')) {
    if (!l) continue;
    const t1 = l.indexOf('\t');
    const t2 = t1 >= 0 ? l.indexOf('\t', t1 + 1) : -1;
    if (t1 < 0 || t2 < 0) continue;
    out.push({ rel: l.slice(0, t1), name: l.slice(t1 + 1, t2), voices: l.slice(t2 + 1).split('\x1f') });
  }
  return out;
}
// A rendered search row: a matched folder (tap navigates in), a matched bank/cart file (tap opens
// its voice list), or a matched voice (tap loads it). All three kinds appear together in the list.
export type DexRow =
  | { kind: 'folder'; path: string; name: string }
  | { kind: 'cart'; rel: string; name: string }
  | { kind: 'voice'; rel: string; name: string; voice: number; vn: string };

// Persistent catalog cache (AsyncStorage → localStorage on web). loadCatalog() short-circuits
// the whole NDJSON download when the device's index.ndjson is byte-identical to last time, so a
// reconnect (e.g. after the phone comes out of your pocket) is near-instant instead of a full
// re-download. Keyed per engine so switching boards doesn't cross-contaminate. See catalog.ts.
const CATALOG_KEY = 'tdsp.catalog.v1';
export const catalogCache: CatalogCache = {
  get: () => AsyncStorage.getItem(CATALOG_KEY),
  set: (v: string) => AsyncStorage.setItem(CATALOG_KEY, v),
};
// Display name for a groove SD path (basename minus .mid) — the drum-track card's "value".
export const grooveDisp = (p: string | null | undefined) => (p ? (p.split('/').pop() || '').replace(/\.mid$/i, '') : '');
export const kb = (n: number) => (n / 1024).toFixed(1);   // bytes -> "12.3" KB, for the load progress readout

// Friendly names for Transport.name (the wire values are 'USB' | 'BLE' | 'WIFI').
export const TP_LABEL: Record<string, string> = { USB: 'USB', BLE: 'Bluetooth', WIFI: 'Wi-Fi' };
// What the 'default' transport actually is on this platform — Metro picks the factory
// (Web Serial on desktop, BLE on native), so the picker label has to match.
export const DEFAULT_TP_LABEL = Platform.OS === 'web' ? 'USB' : 'Bluetooth';
// TAC5212 DAC high-pass filter presets (@HPF mode). 0 = off (all-pass); the rest are
// sub-audio cutoffs that block DC/rumble. Index === the firmware mode number.
export const HPF_MODES = [
  { mode: 0, label: 'Off' },
  { mode: 1, label: '1 Hz' },
  { mode: 2, label: '12 Hz' },
  { mode: 3, label: '96 Hz' },
];
// The header VOL / TAC5212 output slider maps 1..100% → -60..0 dB on the DAC (0 = mute),
// mirroring the firmware's setMasterVolumePct. Shown as a dB readout in the codec section.
export const volDb = (pct: number) => (pct <= 0 ? '-∞' : (-60 + 0.60 * pct).toFixed(1));

// What the MIDI player does when the current song finishes. The header button cycles
// through these; default 'stop'. Only 'repeat' uses the firmware's seamless loop
// (tp.songLoop); 'continue'/'shuffle' advance the song app-side when it ends (@SONGP=-1).
export type EndMode = 'shuffle' | 'repeat' | 'continue' | 'stop';
export const END_MODES: { key: EndMode; icon: string; label: string }[] = [
  { key: 'shuffle',  icon: '🔀', label: 'Shuffle'    },  // twisted arrows → random next song
  { key: 'repeat',   icon: '🔁', label: 'Repeat'     },  // spinning arrows → loop this song
  { key: 'continue', icon: '➡',  label: 'Continue'   },  // arrow → play the next song
  { key: 'stop',     icon: '◻',  label: 'Stop after' },  // hollow square → stop when it ends (default)
];

// Loop-recorder states, indexed by the firmware's 0..4 state code (@RECP / @STATE rec.st*).
// Module scope so both the per-player record rows and the standalone Loop Recorder card use one list.
export const REC_STATES = ['Idle', 'Armed — play a note', 'Recording', 'Overdubbing', 'Looping'];

// The opaque app-owned state we persist on the device (@APP=) so a reload/reconnect restores
// it. Keep it small (device RAM buffer is fixed) and JSON-serializable; grow it as more
// firmware-invisible UI settings need to survive a reconnect.
export type AppState = { end: EndMode; end2?: EndMode; groove?: string };   // end2 = MIDI Player 2's end-of-song mode; groove = last-selected drum groove path (the device only reports the kit, not the picked groove)
export const isEndMode = (v: any): v is EndMode => END_MODES.some(m => m.key === v);

export function notify(msg: string) { if (Platform.OS === 'web') (globalThis as any).alert?.(msg); else Alert.alert('T-DSP', msg); }

export const ROW_H = 41;   // fixed list-row height so FlatList.scrollToIndex is reliable
export type VItem = { key: string; label: string; i: number };

// injectFolders adds synthetic folders at the ROOT level of <FolderBrowser> whose leaves play by
// their own arg (used for the baked "tests" songs, which live in flash, not on the card).
export type InjectFolder = { name: string; leaves: { name: string; arg: string }[] };
export const stripExt = (name: string, ext?: string) => {
  const suf = ext ? '.' + ext : '.mid';
  return name.toLowerCase().endsWith(suf.toLowerCase()) ? name.slice(0, -suf.length) : name;
};

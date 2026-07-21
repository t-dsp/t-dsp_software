// catalog.ts — the DB is the source of truth. Fetch /tdsp/*.ndjson (built on the Teensy
// by @REINDEX) via the transport's readFile, parse NDJSON, and hand the UI ready lists.
// No @INSTR/@DXLS runtime bursts — browsing/search/filter all happen locally over this.

import type { Transport } from './transport';

export interface Cart { path: string; folder: string; name: string; voices: string[]; }
export interface Groove { path: string; name: string; }
// Songs are catalog rows scanned straight off the card (+ baked built-ins) — no capped
// registry, no index. Play by NAME via @SONGF: `file` (the SD filename) when present,
// else `name` for a built-in. `songArg()` picks the right one.
export interface Song { name: string; file?: string; }
export const songArg = (s: Song): string => s.file ?? s.name;
export interface Soundfont { path: string; name: string; bytes: number; }
export interface Instrument { i: number; name: string; }
export interface DrumKit { name: string; prog: number; }

export interface Catalog {
  engine: string;
  hasDrums: boolean;   // does the built engine render ch10 drums? (hides the Drums section when false)
  drumEngine: string;  // parallel drum-voice label (e.g. "OPLL", "TSF"); '' if the synth does its own
  hasBt: boolean;      // does the board have the ESP32 Bluetooth-audio receiver? (hides the Bluetooth section when false)
  hasSf: boolean;      // can the engine load an SD /sf2 font as the MAIN synth? (SF2/TSF) — else don't fetch/show soundfonts
  hasDexed: boolean;   // does the engine use the /dexed DX7 cart library? (Dexed variants) — else don't browse it
  builtMs: number;
  instruments: Instrument[];
  dexed: Cart[];
  grooves: Groove[];
  songs: Song[];
  soundfonts: Soundfont[];
  drumkits: DrumKit[];
}

export const EMPTY_CATALOG: Catalog = {
  engine: '', hasDrums: false, drumEngine: '', hasBt: true, hasSf: true, hasDexed: true, builtMs: 0, instruments: [], dexed: [], grooves: [], songs: [], soundfonts: [], drumkits: [],
};

// Persistent catalog cache. The device's /tdsp/index.ndjson IS the catalog manifest — it
// carries the schema version, build time, engine name, and every per-type file's byte size.
// So the raw index bytes are a perfect fingerprint: if they're byte-identical to a previous
// load, nothing on the device changed and the (multi-hundred-KB) NDJSON download can be
// skipped entirely. A @REINDEX or reflash rewrites the index (new build time / sizes), which
// busts the cache automatically. This is the differential check — the tiny index is the diff
// sentinel; no TTL needed (a TTL would only ever re-fetch identical data).
//
// The app supplies a tiny string KV (backed by AsyncStorage) so this module stays platform-
// and storage-agnostic. Bump CACHE_VER whenever the Catalog shape changes, so an old cached
// blob from a previous app build is ignored instead of deserialized into the new shape.
export interface CatalogCache { get(): Promise<string | null>; set(v: string): Promise<void>; }
const CACHE_VER = 1;
interface CachedCatalog { cv: number; ix: string; catalog: Catalog; }

function parseNdjson<T = any>(text: string): T[] {
  const out: T[] = [];
  for (const raw of (text || '').split('\n')) {
    const s = raw.trim();
    if (!s) continue;
    try { out.push(JSON.parse(s)); } catch { /* skip a malformed row, keep the rest */ }
  }
  return out;
}

/** True if the device has a built catalog (index.ndjson present + parseable). */
export async function hasCatalog(t: Transport): Promise<boolean> {
  try {
    const ix = parseNdjson(await t.readFile('/tdsp/index.ndjson'));
    return ix.some((r: any) => r && r.v);
  } catch { return false; }
}

// Progress reported to the UI during loadCatalog so it can show a real load bar instead
// of a half-populated (broken-looking) homepage. `det` = determinate: true when the device
// announced per-file byte sizes (index.ndjson `files[].bytes`) so `done/total` is a byte
// fraction; false on old firmware, where total/done fall back to a plain file count.
export interface LoadProgress { done: number; total: number; label: string; index: number; count: number; det: boolean; }

// The catalog files fetched on connect, in order. /dexed + /samples are deliberately NOT
// here (the SD library is browsed live — see below), so they don't count toward the total.
const FETCH: { type: keyof Catalog; path: string; label: string }[] = [
  { type: 'instruments', path: '/tdsp/instruments.ndjson', label: 'Instruments' },
  { type: 'grooves',     path: '/tdsp/grooves.ndjson',     label: 'Grooves' },
  { type: 'songs',       path: '/tdsp/songs.ndjson',       label: 'Songs' },
  { type: 'soundfonts',  path: '/tdsp/soundfonts.ndjson',  label: 'Soundfonts' },
  { type: 'drumkits',    path: '/tdsp/drumkits.ndjson',    label: 'Drum kits' },
];

/** Load the full catalog. Throws if index.ndjson is missing (caller can offer @REINDEX).
 *  onProgress (optional) fires before + after each file so the UI can show a load bar.
 *  cache (optional) short-circuits the whole NDJSON download when the device's index.ndjson
 *  is unchanged since the last load — see CatalogCache above. */
export async function loadCatalog(t: Transport, onProgress?: (p: LoadProgress) => void, cache?: CatalogCache): Promise<Catalog> {
  // Throttle progress emits. onProgress -> setState -> a full re-render of the app; firing
  // it on EVERY 360-byte @FD chunk (hundreds per load) floods the main thread and actually
  // SLOWS the transfer (it stalls). Emit at most ~every 100 ms; `force` for phase changes.
  let lastEmit = 0;
  const emit = (p: LoadProgress, force = false) => {
    if (!onProgress) return;
    const now = Date.now();
    if (!force && now - lastEmit < 100) return;
    lastEmit = now; onProgress(p);
  };
  emit({ done: 0, total: 0, label: 'Catalog', index: 0, count: FETCH.length, det: false }, true);   // indeterminate until the index arrives
  // Retry the index read a couple times — the board can be briefly busy right after connect
  // (boot / SD settle), and a single transient failure shouldn't look like "no catalog".
  let ixText = '';
  for (let attempt = 0; ; attempt++) {
    try {
      ixText = await t.readFile('/tdsp/index.ndjson', (recv, tot) =>
        emit({ done: recv, total: tot, label: 'Reading index', index: 0, count: FETCH.length, det: tot > 0 }));
      break;
    }
    catch (e) { if (attempt >= 2) throw e; await new Promise(r => setTimeout(r, 700)); }
  }
  // Cache hit: identical index bytes → the device's catalog is unchanged since we last loaded
  // it, so skip the multi-file download and return the stored catalog immediately. Any read /
  // parse / version mismatch just falls through to a normal full load (the cache is a pure
  // optimization — never a source of truth).
  if (cache) {
    try {
      const raw = await cache.get();
      if (raw) {
        const c = JSON.parse(raw) as CachedCatalog;
        if (c && c.cv === CACHE_VER && c.ix === ixText && c.catalog) {
          emit({ done: 1, total: 1, label: 'Catalog (cached)', index: FETCH.length, count: FETCH.length, det: false }, true);
          return c.catalog;
        }
      }
    } catch { /* corrupt / stale cache — do a full load */ }
  }
  const ix = parseNdjson(ixText);
  const meta = ix.find((r: any) => r && r.v) || {};
  // Fetch ONLY the catalogs this flashed engine can actually use (meta capability flags) —
  // not everything on the card. Absent flag → assume present (old firmware, back-compat).
  // instruments + songs are always relevant (current voices / any engine plays MIDI).
  const wantSf = meta.sf !== false;        // soundfonts: only SF2/TSF can load an SD .sf2 as the main synth
  const wantDrums = meta.drums !== false;  // grooves + drum kits: only a drum-capable engine
  const fetch = FETCH.filter(f =>
    f.type === 'soundfonts' ? wantSf
      : (f.type === 'grooves' || f.type === 'drumkits') ? wantDrums
        : true);
  // The device announces each file's size in files[] (added so the client knows the total
  // up front). Old firmware omits `bytes` → det=false, fall back to a file count.
  const bytesOf: Record<string, number> = {};
  if (Array.isArray(meta.files)) for (const f of meta.files) if (f && f.type) bytesOf[f.type] = (f.bytes | 0);
  const totalBytes = fetch.reduce((s, f) => s + (bytesOf[f.type] || 0), 0);
  const det = totalBytes > 0;
  const total = det ? totalBytes : fetch.length;

  // Read a file, retrying the WHOLE transfer on a transient failure (timeout / dropped frame /
  // length mismatch — see CATALOG_TRANSPORT.md "whole-file retry"). A retry starts from a clean
  // transfer id, so it can't inherit stale frames from the aborted attempt. Don't retry a
  // genuinely-absent file ("not found") — that just wastes three round-trips.
  const readWithRetry = async (path: string, onRecv: (recv: number) => void): Promise<string> => {
    for (let attempt = 0; ; attempt++) {
      try { return await t.readFile(path, (recv) => onRecv(recv)); }
      catch (e) {
        const msg = String((e as any)?.message ?? e ?? '');
        if (attempt >= 2 || /not found/i.test(msg)) throw e;
        await new Promise(r => setTimeout(r, 300));
      }
    }
  };

  // The transport allows ONE @READ in flight at a time, so fetch sequentially (Promise.all
  // would make 4 of 5 reads reject with "read in progress" and come back empty).
  const out: Partial<Record<keyof Catalog, any[]>> = {};
  let done = 0;
  for (let k = 0; k < fetch.length; k++) {
    const f = fetch[k];
    const base = done;
    emit({ done: base, total, label: f.label, index: k + 1, count: fetch.length, det }, true);   // phase change → force
    let txt = '';
    try {
      // Live intra-file progress (throttled): add THIS file's received bytes to the running
      // base so the bar keeps moving during a big/slow read (e.g. grooves), not just between files.
      txt = await readWithRetry(f.path, (recv) => {
        if (det) emit({ done: Math.min(total, base + recv), total, label: f.label, index: k + 1, count: fetch.length, det });
      });
    } catch { txt = ''; }
    out[f.type] = parseNdjson(txt);
    done = Math.min(total, base + (det ? (bytesOf[f.type] || 0) : 1));   // advance by the announced size → the bar reaches 100%
    emit({ done, total, label: f.label, index: k + 1, count: fetch.length, det }, true);
  }
  // NOTE: /dexed is NOT bulk-loaded. With 11k carts x 32 inline voice names the NDJSON is
  // ~6 MB — too big to @READ on every connect (it timed out and the library came back empty).
  // The SD library is browsed LIVE instead, folder-by-folder, via transport.browseDir()/
  // cartVoices() (@DXLS/@DXVL). `dexed` stays [] in the catalog.
  const catalog: Catalog = {
    engine: meta.engine || '',
    hasDrums: meta.drums !== false,   // absent (old firmware) -> assume drums; explicit false -> hide
    drumEngine: meta.drumEngine || '',
    hasBt: meta.bt !== false,         // absent (old firmware) -> assume BT present; explicit false -> hide
    hasSf: wantSf,                    // engine can load an SD soundfont as the main synth
    hasDexed: meta.dexed !== false,   // engine uses the /dexed DX7 cart library
    builtMs: meta.built || 0,
    instruments: out.instruments || [], dexed: [],
    grooves: out.grooves || [], songs: out.songs || [], soundfonts: out.soundfonts || [], drumkits: out.drumkits || [],
  };
  // Persist keyed on the exact index bytes so the next connect can short-circuit (see above).
  if (cache) { try { await cache.set(JSON.stringify({ cv: CACHE_VER, ix: ixText, catalog } as CachedCatalog)); } catch { /* best effort */ } }
  return catalog;
}

/** Cart path relative to /dexed (what @DXPICK expects). */
export function cartRel(c: Cart): string { return c.path.replace(/^\/dexed\//, ''); }

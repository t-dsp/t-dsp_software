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
  builtMs: number;
  instruments: Instrument[];
  dexed: Cart[];
  grooves: Groove[];
  songs: Song[];
  soundfonts: Soundfont[];
  drumkits: DrumKit[];
}

export const EMPTY_CATALOG: Catalog = {
  engine: '', hasDrums: false, drumEngine: '', builtMs: 0, instruments: [], dexed: [], grooves: [], songs: [], soundfonts: [], drumkits: [],
};

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
 *  onProgress (optional) fires before + after each file so the UI can show a load bar. */
export async function loadCatalog(t: Transport, onProgress?: (p: LoadProgress) => void): Promise<Catalog> {
  onProgress?.({ done: 0, total: 0, label: 'Catalog', index: 0, count: FETCH.length, det: false });   // indeterminate until the index arrives
  // Retry the index read a couple times — the board can be briefly busy right after connect
  // (boot / SD settle), and a single transient failure shouldn't look like "no catalog".
  let ixText = '';
  for (let attempt = 0; ; attempt++) {
    try {
      ixText = await t.readFile('/tdsp/index.ndjson', (recv, tot) =>
        onProgress?.({ done: recv, total: tot, label: 'Reading index', index: 0, count: FETCH.length, det: tot > 0 }));
      break;
    }
    catch (e) { if (attempt >= 2) throw e; await new Promise(r => setTimeout(r, 700)); }
  }
  const ix = parseNdjson(ixText);
  const meta = ix.find((r: any) => r && r.v) || {};
  // The device announces each file's size in files[] (added so the client knows the total
  // up front). Old firmware omits `bytes` → det=false, fall back to a file count.
  const bytesOf: Record<string, number> = {};
  if (Array.isArray(meta.files)) for (const f of meta.files) if (f && f.type) bytesOf[f.type] = (f.bytes | 0);
  const totalBytes = FETCH.reduce((s, f) => s + (bytesOf[f.type] || 0), 0);
  const det = totalBytes > 0;
  const total = det ? totalBytes : FETCH.length;

  // The transport allows ONE @READ in flight at a time, so fetch sequentially (Promise.all
  // would make 4 of 5 reads reject with "read in progress" and come back empty).
  const out: Partial<Record<keyof Catalog, any[]>> = {};
  let done = 0;
  for (let k = 0; k < FETCH.length; k++) {
    const f = FETCH[k];
    const base = done;
    onProgress?.({ done: base, total, label: f.label, index: k + 1, count: FETCH.length, det });
    let txt = '';
    try {
      // Live intra-file progress: add THIS file's received bytes to the running base so
      // the bar keeps moving during a big/slow read (e.g. grooves), not just between files.
      txt = await t.readFile(f.path, (recv) => {
        if (det) onProgress?.({ done: Math.min(total, base + recv), total, label: f.label, index: k + 1, count: FETCH.length, det });
      });
    } catch { txt = ''; }
    out[f.type] = parseNdjson(txt);
    done = Math.min(total, base + (det ? (bytesOf[f.type] || 0) : 1));   // advance by the announced size → the bar reaches 100%
    onProgress?.({ done, total, label: f.label, index: k + 1, count: FETCH.length, det });
  }
  // NOTE: /dexed is NOT bulk-loaded. With 11k carts x 32 inline voice names the NDJSON is
  // ~6 MB — too big to @READ on every connect (it timed out and the library came back empty).
  // The SD library is browsed LIVE instead, folder-by-folder, via transport.browseDir()/
  // cartVoices() (@DXLS/@DXVL). `dexed` stays [] in the catalog.
  return {
    engine: meta.engine || '',
    hasDrums: meta.drums !== false,   // absent (old firmware) -> assume drums; explicit false -> hide
    drumEngine: meta.drumEngine || '',
    builtMs: meta.built || 0,
    instruments: out.instruments || [], dexed: [],
    grooves: out.grooves || [], songs: out.songs || [], soundfonts: out.soundfonts || [], drumkits: out.drumkits || [],
  } as Catalog;
}

/** Cart path relative to /dexed (what @DXPICK expects). */
export function cartRel(c: Cart): string { return c.path.replace(/^\/dexed\//, ''); }

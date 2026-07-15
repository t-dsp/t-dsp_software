// catalog.ts — the DB is the source of truth. Fetch /tdsp/*.ndjson (built on the Teensy
// by @REINDEX) via the transport's readFile, parse NDJSON, and hand the UI ready lists.
// No @INSTR/@DXLS runtime bursts — browsing/search/filter all happen locally over this.

import type { Transport } from './transport';

export interface Cart { path: string; folder: string; name: string; voices: string[]; }
export interface Groove { path: string; name: string; }
// Songs are indexed by the firmware's play registry (g_songs) — `i` is what @SONG= expects
// (built-ins + SD, NOT a bare /songs file list). Built from the bundled writer, not flatScan.
export interface Song { i: number; name: string; }
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

async function readOrEmpty(t: Transport, path: string): Promise<any[]> {
  try { return parseNdjson(await t.readFile(path)); } catch { return []; }
}

/** True if the device has a built catalog (index.ndjson present + parseable). */
export async function hasCatalog(t: Transport): Promise<boolean> {
  try {
    const ix = parseNdjson(await t.readFile('/tdsp/index.ndjson'));
    return ix.some((r: any) => r && r.v);
  } catch { return false; }
}

/** Load the full catalog. Throws if index.ndjson is missing (caller can offer @REINDEX). */
export async function loadCatalog(t: Transport): Promise<Catalog> {
  // Retry the index read a couple times — the board can be briefly busy right after connect
  // (boot / SD settle), and a single transient failure shouldn't look like "no catalog".
  let ixText = '';
  for (let attempt = 0; ; attempt++) {
    try { ixText = await t.readFile('/tdsp/index.ndjson'); break; }
    catch (e) { if (attempt >= 2) throw e; await new Promise(r => setTimeout(r, 700)); }
  }
  const ix = parseNdjson(ixText);
  const meta = ix.find((r: any) => r && r.v) || {};
  // The transport allows ONE @READ in flight at a time, so fetch sequentially (Promise.all
  // would make 5 of 6 reads reject with "read in progress" and come back empty).
  const instruments = await readOrEmpty(t, '/tdsp/instruments.ndjson');
  // NOTE: /dexed is NOT bulk-loaded. With 11k carts x 32 inline voice names the NDJSON is
  // ~6 MB — too big to @READ on every connect (it timed out and the library came back empty).
  // The SD library is browsed LIVE instead, folder-by-folder, via transport.browseDir()/
  // cartVoices() (@DXLS/@DXVL). `dexed` stays [] in the catalog.
  const dexed: Cart[] = [];
  const grooves = await readOrEmpty(t, '/tdsp/grooves.ndjson');
  const songs = await readOrEmpty(t, '/tdsp/songs.ndjson');
  const soundfonts = await readOrEmpty(t, '/tdsp/soundfonts.ndjson');
  const drumkits = await readOrEmpty(t, '/tdsp/drumkits.ndjson');
  return {
    engine: meta.engine || '',
    hasDrums: meta.drums !== false,   // absent (old firmware) -> assume drums; explicit false -> hide
    drumEngine: meta.drumEngine || '',
    builtMs: meta.built || 0,
    instruments, dexed, grooves, songs, soundfonts, drumkits,
  };
}

/** Cart path relative to /dexed (what @DXPICK expects). */
export function cartRel(c: Cart): string { return c.path.replace(/^\/dexed\//, ''); }

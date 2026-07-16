// transport.ts — the one interface both platforms implement.
//
//   desktop (react-native-web) -> transport.web.ts   (Web Serial + @-line protocol)
//   app     (native)           -> transport.native.ts (BLE, react-native-ble-plx)
//
// Metro resolves the platform-specific file automatically (`.web.ts` / `.native.ts`).
// The UI talks ONLY to this interface, so it is transport-agnostic. Catalog browsing is
// local (see catalog.ts); the transport carries file reads (@READ) + high-level ACTIONS.

import type { SeqStep, ArpWireParams } from './arpSeq';   // type-only: erased at compile time, so no runtime resolution issue

export type LineHandler = (line: string) => void;

// One level of the /dexed library, fetched LIVE (lazy) via @DXLS — never bulk-downloaded.
// The full catalog would be ~6 MB (11k carts x 32 inline voice names), too big to @READ on
// every connect, so /dexed is browsed folder-by-folder on demand. `rel` is the cart path
// relative to /dexed (what @DXVL / @DXPICK expect); `name` is the display label (no .syx).
export interface DirPage {
  path: string;                              // the folder this listing is for (rel to /dexed, '' = root)
  page: number;
  npages: number;
  folders: string[];                         // subfolder names at this level
  carts: { name: string; rel: string }[];    // carts sitting directly at this level
}

// parseDxls (the @DXLS reply parser) lives in ./dxls, NOT here: this module has
// platform siblings (transport.web.ts / transport.native.ts), so a runtime value
// imported from './transport' inside a platform file resolves back to that file, not
// this one. Types below are erased at compile time, so importing them is safe.

export interface Transport {
  readonly name: 'USB' | 'BLE';
  isConnected(): boolean;

  connect(): Promise<void>;
  disconnect(): Promise<void>;

  // Subscribe to raw device lines (BT status, etc.). Returns an unsubscribe fn.
  onLine(cb: LineHandler): () => void;

  // Fetch an SD file (the catalog transport): @READ -> reassembled text. onProgress
  // (optional) fires as bytes stream in (received, total) so the UI can show a live bar
  // during a large/slow transfer — `total` is the device-reported file size (0 if unknown).
  readFile(path: string, onProgress?: (received: number, total: number) => void): Promise<string>;

  // Lazy /dexed browse (the SD library is too big to ship in the catalog): list one
  // folder level (@DXLS) or fetch one cart's 32 voice names (@DXVL). `path`/`cartRel`
  // are relative to /dexed.
  browseDir(path: string, page?: number): Promise<DirPage>;
  cartVoices(cartRel: string): Promise<string[]>;

  // Rebuild the on-device catalog DB (/tdsp/*.ndjson).
  reindex(): Promise<void>;

  // Ask the device for its current settings (@STATE). The reply arrives as an
  // "@STATE={…}" line via onLine(), which the UI parses to hydrate every card on connect.
  // The device also emits an "@APP={…}" line (the opaque app-owned state, see saveAppState).
  requestState(): void;

  // Persist an opaque app-owned settings blob on the device so it survives an app
  // reload/reconnect (@APP=). The firmware never interprets it — it only stores + echoes it;
  // the app owns the schema. Serialized here as compact JSON. Hydrated back via the "@APP="
  // line delivered through onLine(). RAM-only on the device (does not survive a reboot).
  saveAppState(state: unknown): void;

  // ---- actions (map to @-lines on web, to BLE opcodes on native) ----
  masterVolume(pct: number): void;                    // header master volume (@VOL=, 0..100) — the TAC5212 OUT1/OUT2 DAC level
  dacHpf(mode: number): void;                         // TAC5212 DAC high-pass filter (@HPF=): 0=off, 1=1Hz, 2=12Hz, 3=96Hz
  masterBpm(bpm: number): void;                       // master tempo (@BPM=) — song + drums lock to it
  dxVoice(index: number): void;                       // select a bundled voice
  dxPick(cartRel: string, voice: number): void;       // load /dexed cart voice
  drumKit(index: number): void;
  playGrooveFile(name: string): void;
  stopDrums(): void;
  songPlay(arg: string): void;                        // play by name/filename (@SONGF=) — mirrors playGrooveFile
  stopSong(): void;
  songLoop(on: boolean): void;                        // loop the current song (@LOOP=)
  launchQuantize(on: boolean): void;                  // defer song/groove starts to the next bar (@QUANTIZE=)
  arpOn(on: boolean): void;
  arpPattern(i: number): void;
  arpRate(i: number): void;                // i = firmware Rate index (see ARP_RATES[].fw)
  arpGate(pct: number): void;              // gate length % (@ARPGATE=, 5..150)
  arpSwing(pct: number): void;             // swing % (@ARPSWING=, 50..85; 50 = straight)
  arpOctaves(n: number): void;
  arpLatch(on: boolean): void;
  arpSequence(steps: SeqStep[]): void;   // upload the User Sequence step table (@ARPSEQ=)
  arpPreset(params: ArpWireParams): void; // apply a whole preset atomically (@ARPPRESET=)
  espPair(): void;
  espReconnect(): void;      // (re)connect A2DP audio to the last paired source
  espDisconnect(): void;     // drop the current A2DP audio source
  espForget(): void;
}

// createTransport() is provided by the platform files; this module only re-exports the
// type. Import the concrete factory from './transportFactory' (also platform-split).

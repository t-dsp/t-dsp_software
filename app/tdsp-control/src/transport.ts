// transport.ts — the one interface both platforms implement.
//
//   desktop (react-native-web) -> transport.web.ts   (Web Serial + @-line protocol)
//   app     (native)           -> transport.native.ts (BLE, react-native-ble-plx)
//
// Metro resolves the platform-specific file automatically (`.web.ts` / `.native.ts`).
// The UI talks ONLY to this interface, so it is transport-agnostic. Catalog browsing is
// local (see catalog.ts); the transport carries file reads (@READ) + high-level ACTIONS.

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

  // Fetch an SD file (the catalog transport): @READ -> reassembled text.
  readFile(path: string): Promise<string>;

  // Lazy /dexed browse (the SD library is too big to ship in the catalog): list one
  // folder level (@DXLS) or fetch one cart's 32 voice names (@DXVL). `path`/`cartRel`
  // are relative to /dexed.
  browseDir(path: string, page?: number): Promise<DirPage>;
  cartVoices(cartRel: string): Promise<string[]>;

  // Rebuild the on-device catalog DB (/tdsp/*.ndjson).
  reindex(): Promise<void>;

  // Ask the device for its current settings (@STATE). The reply arrives as an
  // "@STATE={…}" line via onLine(), which the UI parses to hydrate every card on connect.
  requestState(): void;

  // ---- actions (map to @-lines on web, to BLE opcodes on native) ----
  masterVolume(pct: number): void;                    // header master volume (@VOL=, 0..100)
  masterBpm(bpm: number): void;                       // master tempo (@BPM=) — song + drums lock to it
  dxVoice(index: number): void;                       // select a bundled voice
  dxPick(cartRel: string, voice: number): void;       // load /dexed cart voice
  drumKit(index: number): void;
  playGrooveFile(name: string): void;
  stopDrums(): void;
  playSong(index: number): void;
  stopSong(): void;
  songLoop(on: boolean): void;                        // loop the current song (@LOOP=)
  arpOn(on: boolean): void;
  arpPattern(i: number): void;
  arpRate(i: number): void;
  arpOctaves(n: number): void;
  arpLatch(on: boolean): void;
  espPair(): void;
  espReconnect(): void;      // (re)connect A2DP audio to the last paired source
  espDisconnect(): void;     // drop the current A2DP audio source
  espForget(): void;
}

// createTransport() is provided by the platform files; this module only re-exports the
// type. Import the concrete factory from './transportFactory' (also platform-split).

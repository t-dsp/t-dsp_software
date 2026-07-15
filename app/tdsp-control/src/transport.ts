// transport.ts — the one interface both platforms implement.
//
//   desktop (react-native-web) -> transport.web.ts   (Web Serial + @-line protocol)
//   app     (native)           -> transport.native.ts (BLE, react-native-ble-plx)
//
// Metro resolves the platform-specific file automatically (`.web.ts` / `.native.ts`).
// The UI talks ONLY to this interface, so it is transport-agnostic. Catalog browsing is
// local (see catalog.ts); the transport carries file reads (@READ) + high-level ACTIONS.

export type LineHandler = (line: string) => void;

export interface Transport {
  readonly name: 'USB' | 'BLE';
  isConnected(): boolean;

  connect(): Promise<void>;
  disconnect(): Promise<void>;

  // Subscribe to raw device lines (BT status, etc.). Returns an unsubscribe fn.
  onLine(cb: LineHandler): () => void;

  // Fetch an SD file (the catalog transport): @READ -> reassembled text.
  readFile(path: string): Promise<string>;

  // Rebuild the on-device catalog DB (/tdsp/*.ndjson).
  reindex(): Promise<void>;

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
  espReconnect(): void;
  espForget(): void;
}

// createTransport() is provided by the platform files; this module only re-exports the
// type. Import the concrete factory from './transportFactory' (also platform-split).

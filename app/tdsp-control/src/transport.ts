// transport.ts — the one interface every transport implements.
//
//   desktop (react-native-web) -> transport.web.ts   (Web Serial + @-line protocol)
//   app     (native)           -> transport.native.ts (BLE, react-native-ble-plx)
//   either platform            -> transport.wifi.ts   (LAN WebSocket to the ESP32)
//
// Metro resolves the platform-specific file automatically (`.web.ts` / `.native.ts`).
// transport.wifi.ts has NO platform siblings — it works on both (browser + RN WebSocket)
// and is selected explicitly via createTransport('wifi').
// The UI talks ONLY to this interface, so it is transport-agnostic. Catalog browsing is
// local (see catalog.ts); the transport carries file reads (@READ) + high-level ACTIONS.

import type { SeqStep, ArpWireParams } from './arpSeq';   // type-only: erased at compile time, so no runtime resolution issue

export type LineHandler = (line: string) => void;

// Which transport createTransport() should build. 'default' = the platform's built-in
// (USB/Web Serial on desktop, BLE on native); 'wifi' = the LAN WebSocket transport,
// available on both platforms.
export type TransportKind = 'default' | 'wifi';

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
  readonly name: 'USB' | 'BLE' | 'WIFI';
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

  // Generic recursive SD folder browse (@LS): list ONE directory level — subdirs + files,
  // the latter filtered by `ext` (e.g. 'mid', no dot; omit for all files). The device streams
  // in filesystem order; the client sorts (see sortEntries in ./browse). Used by <FolderBrowser>
  // for /midi/songs, /midi/drums, etc. Absolute SD path (leading '/'); '..' is rejected device-side.
  browse(path: string, ext?: string): Promise<import('./browse').BrowseResult>;

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
  drumVol(pct: number): void;                         // drum-player level (@DRUMVOL=, 0..150 %), independent of the master @VOL
  // ---- Runtime drum-font swap (build-flag gated; shown only when @STATE caps.drumfontsel → the sampled
  // drum TSF build with >1 SF2 on the card). See planning/drums-from-mars/RUNTIME_FONT_SWAP.md. The list
  // arrives as a "@FONTS=" line via onLine() (parse with catalog.parseFonts); the swap's completion is
  // acked with a "@DRUMFONT=<path>" line (the app reloads the catalog for the new font's kits). ----
  requestFonts(): void;                               // ask for the available drum fonts (@FONTS)
  drumFont(path: string): void;                       // swap the resident drum SF2 (@DRUMFONT=<path>)
  songPlay(arg: string): void;                        // play by name/filename (@SONGF=) — mirrors playGrooveFile
  songRestart(arg: string): void;                     // hard restart from the top on a fresh downbeat (@SONGRESTART=): zeroes the clock, ignores launch-quantize
  stopSong(): void;
  songVol(pct: number): void;                         // MIDI-player level (@SONGVOL=, 0..150 %), independent of the master @VOL
  songLoop(on: boolean): void;                        // loop the current song (@LOOP=)
  launchQuantize(on: boolean): void;                  // defer song/groove starts to the next bar (@QUANTIZE=)
  metronome(on: boolean): void;                       // MASTER TRANSPORT play/stop (the metronome is the clock) (@METRO=)
  metronomeMute(muted: boolean): void;                // is the click AUDIBLE? default muted; transport runs either way (@METROMUTE=)
  metronomeSig(bpb: number): void;                    // metronome/idle time signature = N beats/bar (@METROSIG=)
  metronomeVol(pct: number): void;                    // metronome click level (@METROVOL=, 0..150 %), independent of the master @VOL
  metronomeLock(on: boolean): void;                   // tempo lock: when ON, loading content stops auto-setting the master BPM (@METROLOCK=)
  // ---- FX reverb master insert (build-flag gated; shown only when @STATE caps.fx). One generic
  // method like trk(): the card builds '@FX.<cmd>' — fx('ON=1'), fx('MIX=50'), fx('SIZE=70') (plate)
  // / fx('TIME=55') (spring). Which params exist depends on @STATE.fx.type (plate|spring). ----
  fx(cmd: string): void;                              // @FX.<cmd>
  arpOn(on: boolean): void;
  arpRestart(): void;                                 // re-trigger the running arp cycle from step 0 (@ARPRESTART)
  arpPattern(i: number): void;
  arpRate(i: number): void;                // i = firmware Rate index (see ARP_RATES[].fw)
  arpGate(pct: number): void;              // gate length % (@ARPGATE=, 5..150)
  arpSwing(pct: number): void;             // swing % (@ARPSWING=, 50..85; 50 = straight)
  arpOctaves(n: number): void;
  arpLatch(on: boolean): void;
  arpSequence(steps: SeqStep[]): void;   // upload the User Sequence step table (@ARPSEQ=)
  arpPreset(params: ArpWireParams): void; // apply a whole preset atomically (@ARPPRESET=)
  // ---- Runtime pool partition (4-voice pool builds; @STATE.pool) ----
  poolPreset(preset: number): void;                   // redistribute the 8 engines: 0=4voices 1=2voices 2=1voice 3=4+2+2 (@POOL=)
  // ---- Voices 2 (build-flag gated; the app shows these only when @STATE caps.voice2) ----
  voice2Enable(on: boolean): void;                    // split the pool so a USB keyboard gets its own voice (@VOICE2=)
  voice2Vol(pct: number): void;                       // Voices-2 level (@VOICE2VOL=, 0..150 %)
  dxVoice2(index: number): void;                      // bundled voice for the keyboard half (@DXVOICE2=)
  dxPick2(cartRel: string, voice: number): void;      // /dexed cart voice for the keyboard half (@DXPICK2=)
  arp2On(on: boolean): void;                          // keyboard-path arp on/off (@ARP2ON=) — gated by caps.arp2
  arp2Restart(): void;                                // @ARP2RESTART
  arp2Pattern(i: number): void;
  arp2Rate(i: number): void;                          // i = firmware Rate index
  arp2Gate(pct: number): void;                        // @ARP2GATE=
  arp2Swing(pct: number): void;                       // @ARP2SWING=
  arp2Octaves(n: number): void;
  arp2Latch(on: boolean): void;
  arp2Sequence(steps: SeqStep[]): void;               // @ARP2SEQ=
  arp2Preset(params: ArpWireParams): void;            // @ARP2PRESET=
  // ---- MIDI Player 2 (a second, independent song player routed to the voice-2 synth side, so
  // two songs can play at once; gated by @STATE caps.voice2). Mirrors the songPlay/… set but on
  // @SONG2* / @LOOP2, with its own position feed @SONG2P=. Player-2 output level rides the
  // voice-2 bus (voice2Vol / @VOICE2VOL) — there is no separate @SONG2VOL. ----
  song2Play(arg: string): void;                       // play by name/filename on player 2 (@SONG2F=)
  song2Restart(arg: string): void;                    // hard restart from the top on a fresh downbeat (@SONG2RESTART=)
  stopSong2(): void;                                  // stop player 2 (@SONG2=stop)
  song2Loop(on: boolean): void;                       // loop player 2's current song (@LOOP2=)
  // ---- Track-indexed interface (Phase 3): @TRK<i>.<cmd> drives ANY track uniformly. `cmd` is the
  // full suffix incl any '=arg' — e.g. 'SRC=usb', 'SRCCH=3' (live-MIDI input subscription, Thread C),
  // 'ARPPAT=2', 'DXPICK=<rel>\t<v>', 'PLAY=<name>'. One method for every per-track control, so a new
  // synth voice needs no new API — the app drives its card from the @STATE tracks[] entry. ----
  trk(index: number, cmd: string): void;              // @TRK<index>.<cmd>
  // ---- Loop recorder (build-flag gated; shown only when @STATE caps.rec) ----
  recVoice(v: number): void;                          // which voice the record controls target (@RECV=, 1|2)
  recBars(n: number): void;                           // loop length in bars (@RECBARS=, 1|2|4|8)
  recSig(bpb: number): void;                          // record/master time signature = N beats/bar (@RECSIG=)
  recArm(on: boolean): void;                          // arm a fresh (replace) recording / stop (@REC=)
  recOverdub(on: boolean): void;                      // arm an overdub onto the existing clip / stop (@RECDUB=)
  recPlay(on: boolean): void;                         // resume a stopped clip / stop (@RECPLAY=)
  recClear(): void;                                   // wipe the selected voice's clip (@RECCLR)
  // ---- Note editor (build-flag gated; shown only when @STATE caps.recedit). See
  // planning/midi-editor/DESIGN.md. Round-trips a recorded loop as raw LoopClip bytes. ----
  recDump(v: number): Promise<Uint8Array>;            // fetch voice v's clip (@RECDUMP -> @FB/@FD/@FE)
  recLoad(v: number, bytes: Uint8Array): Promise<void>; // stream an edited clip back (@RECLOAD/@RD/@RECEND)
  // ---- Audio loop recorder (records the MASTER MIX as looping audio; N independent
  // loops. Shown only when @STATE caps.audioloop > 0 — it reports how many loops actually
  // allocated, which is RAM/PSRAM dependent). See planning/audio-looper/DESIGN.md. ----
  audioLoopSel(i: number): void;                      // select the loop the controls target (@ALSEL=)
  audioLoopBars(n: number): void;                     // loop length in bars (@ALBARS=, 1|2|4|8)
  audioLoopMono(on: boolean): void;                   // mono storage = 2x loop length (@ALMONO=)
  audioLoopFollow(on: boolean): void;                 // track master tempo, pitch shifts (@ALFOLLOW=)
  audioLoopLevel(pct: number): void;                  // loop return level 0..100 (@ALLEVEL=)
  audioLoopArm(on: boolean): void;                    // arm a fresh (replace) take / stop (@AL=)
  audioLoopOverdub(on: boolean): void;                // layer onto the playing loop / stop (@ALDUB=)
  audioLoopPlay(on: boolean): void;                   // resume a stopped loop / stop (@ALPLAY=)
  audioLoopClear(): void;                             // wipe the selected loop (@ALCLR)
  audioLoopSave(name: string): void;                  // save as /loops/<name>.wav (@ALSAVE=)
  // ---- USB Audio interface (build-flag gated; shown only when @STATE caps.usbaudio → the
  // TDSP_USB_AUDIO 24-bit/48k sound-card build). The host owns its own output level via the UAC
  // Feature Unit; this sets the device-side USB-in RETURN level into the mix bus. ----
  usbAudioGain(pct: number): void;                    // USB-in return level into the mix (@USBGAIN=, 0..150 %)
  // On USB these are single chars relayed Teensy -> ESP32; on BLE they are opcodes; over
  // WiFi they are the ESP32's local '!' commands. Same semantics either way.
  espPair(): void;
  espReconnect(): void;      // (re)connect A2DP audio to the last paired source
  espDisconnect(): void;     // drop the current A2DP audio source
  espForget(): void;
}

// createTransport() is provided by the platform files; this module only re-exports the
// type. Import the concrete factory from './transportFactory' (also platform-split).

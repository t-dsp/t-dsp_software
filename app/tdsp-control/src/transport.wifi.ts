// transport.wifi.ts — control over WiFi (LAN), via the ESP32's WebSocket server.
//
// Speaks the SAME @-line protocol as transport.web.ts, but over a WebSocket to the
// ESP32 instead of a serial port to the Teensy. The ESP32 relays every @-line verbatim
// to the Teensy and broadcasts every Teensy line back verbatim, so the framing/parsing
// below is identical to the Web Serial path — only the pipe differs.
//
// Firmware side: projects/t-dsp_esp32_bt_receiver built with -D TDSP_CTRL_WIFI.
//   * discovery: tdsp.local, _ws._tcp, port 81
//   * inbound  : "@..." -> relayed verbatim to the Teensy
//                "!pair" | "!reconnect" | "!disconnect" | "!forget" | "!status" -> local A2DP verbs
//   * outbound : every Teensy @-line verbatim (NO BLE 0x1e chunking / 512 B splitting),
//                plus the ESP32's own status as a bare JSON line (same shape the BLE
//                Status characteristic emitted, so the shared UI handler works unchanged).
//
// NOTE this file has NO platform siblings (.web/.native), so importing it as a runtime
// value from the factories is safe. It works on both web (browser WebSocket) and native
// (React Native WebSocket). See transport.ts for why ./dxls is imported instead of ./transport.

import { parseDxls } from './dxls';
import { parseLb, parseLd, parseLe, parseLerr } from './browse';
import { encodeSequence, encodeArpParams } from './arpSeq';
import { rdFrames, base64ToBytes } from './loopXfer';
import type { SeqStep, ArpWireParams } from './arpSeq';
import type { BrowseEntry, BrowseResult } from './browse';
import type { Transport, LineHandler, DirPage } from './transport';

// mDNS name the firmware advertises (TDSP_MDNS_HOST / TDSP_WS_PORT in main.cpp).
// Callers can pass a bare host/IP instead — see the constructor.
export const TDSP_WS_DEFAULT = 'ws://tdsp.local:81/';

interface FilePending { path: string; parts: Record<number, string>; resolve: (t: any) => void; reject: (e: any) => void; timer: any; onProgress?: (r: number, t: number) => void; total: number; received: number; bytes?: boolean; }
// Decoded byte count of a base64 chunk (for streaming progress; avoids decoding mid-stream).
const b64bytes = (s: string) => Math.max(0, Math.floor(s.replace(/=+$/, '').length * 3 / 4));
interface DirPending { path: string; resolve: (d: DirPage) => void; reject: (e: any) => void; timer: any; }
interface VoicesPending { rel: string; resolve: (v: string[]) => void; reject: (e: any) => void; timer: any; }
// One in-flight @LS: matched by echoed path (@LB), then id for the @LD/@LE stream.
interface BrowsePending { path: string; id: number; entries: BrowseEntry[]; resolve: (r: BrowseResult) => void; reject: (e: any) => void; timer: any; }

// Accept "tdsp.local", "192.168.1.42", "tdsp.local:81" or a full "ws://host:port/" URL.
function toWsUrl(target?: string): string {
  if (!target) return TDSP_WS_DEFAULT;
  if (/^wss?:\/\//i.test(target)) return target;
  return 'ws://' + target + (/:\d+$/.test(target) ? '' : ':81') + '/';
}

export class WiFiTransport implements Transport {
  readonly name = 'WIFI' as const;
  private ws: WebSocket | null = null;
  private url: string;
  private buf = '';
  private handlers = new Set<LineHandler>();
  private file: FilePending | null = null;
  private dir: DirPending | null = null;
  private voices: VoicesPending | null = null;
  private ls: BrowsePending | null = null;

  // `target` may be a host ("tdsp.local", "192.168.1.42"), "host:port", or a ws:// URL.
  // mDNS caveat: .local resolution is reliable on iOS/macOS + desktop browsers, but
  // Android (React Native) often can NOT resolve .local without NSD — pass the IP there.
  constructor(target?: string) { this.url = toWsUrl(target); }

  isConnected() { return !!this.ws && this.ws.readyState === 1 /* OPEN */; }

  connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      let settled = false;
      const done = (fn: () => void) => { if (settled) return; settled = true; clearTimeout(deadline); fn(); };
      // Bound the whole connect: an unreachable host (wrong LAN, device asleep, .local not
      // resolving on Android) can otherwise leave the UI's Connect button wedged.
      const deadline = setTimeout(() => done(() => { try { ws.close(); } catch {} ; this.ws = null; reject(new Error(`No T-DSP at ${this.url} (timed out)`)); }), 8000);

      let ws: WebSocket;
      try { ws = new WebSocket(this.url); }
      catch (e) { clearTimeout(deadline); reject(e); return; }

      this.buf = '';
      ws.onopen = () => { this.ws = ws; done(resolve); };
      // Fires for a failed connect AND for a mid-session drop; only the former rejects
      // (done() is a no-op once we've resolved).
      ws.onerror = () => done(() => { this.ws = null; reject(new Error(`WebSocket error connecting to ${this.url}`)); });
      ws.onclose = () => { done(() => { this.ws = null; reject(new Error(`Connection to ${this.url} closed`)); }); if (this.ws === ws) this.teardown(); };
      ws.onmessage = (ev: MessageEvent) => {
        if (typeof ev.data !== 'string') return;   // firmware only sends TEXT frames
        // Accumulate until '\n' — a line is NOT necessarily one frame. The firmware splits
        // long lines (a catalog @INSTR is ~7 KB) into ~1 KB chunks and terminates each line
        // with '\n', because a single oversized frame overruns the ESP32's TCP send buffer
        // and permanently wedges the socket (hardware-verified: errno 11 EAGAIN, replies
        // stop forever). So '\n' is the only frame boundary we trust — never treat an
        // arriving frame as a complete line on its own.
        this.buf += ev.data;
        let i: number;
        while ((i = this.buf.indexOf('\n')) >= 0) { this.pump(this.buf.slice(0, i)); this.buf = this.buf.slice(i + 1); }
      };
    });
  }

  // Guard per line: a handler throw must not kill the socket (one bad frame shouldn't
  // tear down the connection).
  private pump(raw: string) {
    const line = raw.replace(/\r$/, '');
    if (!line) return;
    try { this.onDeviceLine(line); } catch (e) { console.warn('[tdsp] line handler error:', e); }
  }

  async disconnect(): Promise<void> {
    const ws = this.ws;
    this.teardown();
    try { ws?.close(); } catch {}
  }

  private teardown() {
    // Don't leave reads "in progress" — the UI would spin forever.
    if (this.file) { clearTimeout(this.file.timer); this.file.reject('disconnected'); this.file = null; }
    if (this.dir) { clearTimeout(this.dir.timer); this.dir.reject('disconnected'); this.dir = null; }
    if (this.voices) { clearTimeout(this.voices.timer); this.voices.reject('disconnected'); this.voices = null; }
    if (this.ls) { clearTimeout(this.ls.timer); this.ls.reject('disconnected'); this.ls = null; }
    this.ws = null; this.buf = '';
  }

  private send(line: string) { if (this.isConnected()) this.ws!.send(line); }   // firmware's relayLine() adds the '\n'

  onLine(cb: LineHandler): () => void { this.handlers.add(cb); return () => this.handlers.delete(cb); }

  // (Re)arm the read watchdog. It's an IDLE timeout: as long as frames keep arriving we
  // keep waiting, so a big file (e.g. a 3,700-cart /dexed) that streams for a while
  // completes instead of being cut off mid-transfer.
  private armFileTimer(f: FilePending) {
    clearTimeout(f.timer);
    f.timer = setTimeout(() => { if (this.file === f) { this.file = null; f.reject('timeout'); } }, 15000);
  }
  // Idle watchdog for an @LS browse — re-armed on every @LB/@LD frame.
  private armLsTimer(s: BrowsePending) {
    clearTimeout(s.timer);
    s.timer = setTimeout(() => { if (this.ls === s) { this.ls = null; s.reject('timeout'); } }, 12000);
  }

  private onDeviceLine(line: string) {
    // @READ frame transport. @FB=<id>\x1f<path>\x1f<bytes> begins a file; the byte total
    // lets us report a live progress fraction as @FD chunks arrive.
    if (line.startsWith('@FB=')) {
      if (this.file) { const p = line.slice(4).split('\x1f'); this.file.parts = {}; this.file.total = +p[2] || 0; this.file.received = 0; this.armFileTimer(this.file); this.file.onProgress?.(0, this.file.total); }
      return;
    }
    if (line.startsWith('@FD=')) {
      const p = line.slice(4).split('\x1f');
      if (this.file && p.length === 3) { this.file.parts[+p[1]] = p[2]; this.file.received += b64bytes(p[2]); this.armFileTimer(this.file); this.file.onProgress?.(this.file.received, this.file.total); }
      return;
    }
    if (line.startsWith('@FE=')) {
      const f = this.file;
      if (f) {
        const b64 = Object.keys(f.parts).map(Number).sort((a, b) => a - b).map(k => f.parts[k]).join('');
        clearTimeout(f.timer); this.file = null;
        if (f.bytes) { f.resolve(base64ToBytes(b64)); return; }   // @RECDUMP path: raw clip bytes
        let txt: string;
        try { txt = decodeURIComponent(escape(atob(b64))); } catch { txt = atob(b64); }
        f.resolve(txt);
      }
      return;
    }
    if (line.startsWith('@FERR=')) { const f = this.file; if (f) { clearTimeout(f.timer); this.file = null; f.reject(line.slice(6)); } return; }
    // Lazy /dexed browse replies. Match the echoed path so a stale reply for a folder
    // we've navigated away from is ignored (the newer request has its own pending slot).
    if (line.startsWith('@DXLS=')) {
      const d = this.dir; if (d) { const dp = parseDxls(line.slice(6)); if (dp.path === d.path) { clearTimeout(d.timer); this.dir = null; d.resolve(dp); } }
      return;
    }
    if (line.startsWith('@DXVL=')) {
      const v = this.voices; if (v) { const p = line.slice(6).split('|'); const rc = p.shift(); if (rc === v.rel) { clearTimeout(v.timer); this.voices = null; v.resolve(p); } }
      return;
    }
    // Generic @LS folder browse (frames relayed verbatim over the WebSocket). Match @LB by
    // echoed path, accumulate @LD by id, resolve on @LE / reject on @LERR.
    if (line.startsWith('@LB=')) { const b = parseLb(line.slice(4)); const s = this.ls; if (s && b.path === s.path) { s.id = b.id; s.entries = []; this.armLsTimer(s); } return; }
    if (line.startsWith('@LD=')) { const e = parseLd(line.slice(4)); const s = this.ls; if (s && e && e.id === s.id) { s.entries.push({ type: e.type, name: e.name }); this.armLsTimer(s); } return; }
    if (line.startsWith('@LE=')) { const e = parseLe(line.slice(4)); const s = this.ls; if (s && e.id === s.id) { clearTimeout(s.timer); this.ls = null; s.resolve({ path: s.path, entries: s.entries }); } return; }
    if (line.startsWith('@LERR=')) { const e = parseLerr(line.slice(6)); const s = this.ls; if (s && (e.id === s.id || s.id < 0)) { clearTimeout(s.timer); this.ls = null; s.reject(e.reason); } return; }
    // everything else -> subscribers (@STATE/@APP, the ESP32 status JSON, @SOURCES=, etc.)
    this.handlers.forEach(h => h(line));
  }

  readFile(path: string, onProgress?: (received: number, total: number) => void): Promise<string> {
    return new Promise((resolve, reject) => {
      if (this.file) { reject('a file read is in progress'); return; }
      const f: FilePending = { path, parts: {}, resolve, reject, timer: null, onProgress, total: 0, received: 0 };
      this.file = f;
      this.armFileTimer(f);   // idle watchdog; re-armed on every @FB/@FD frame
      this.send('@READ=' + path);
    });
  }

  browseDir(path: string, page = 0): Promise<DirPage> {
    return new Promise((resolve, reject) => {
      if (this.dir) { clearTimeout(this.dir.timer); this.dir.reject('superseded'); }
      const d: DirPending = { path, resolve, reject, timer: null };
      this.dir = d;
      d.timer = setTimeout(() => { if (this.dir === d) { this.dir = null; reject('timeout'); } }, 8000);
      this.send('@DXLS=' + path + (page ? '\t' + page : ''));
    });
  }

  cartVoices(cartRel: string): Promise<string[]> {
    return new Promise((resolve, reject) => {
      if (this.voices) { clearTimeout(this.voices.timer); this.voices.reject('superseded'); }
      const v: VoicesPending = { rel: cartRel, resolve, reject, timer: null };
      this.voices = v;
      v.timer = setTimeout(() => { if (this.voices === v) { this.voices = null; reject('timeout'); } }, 8000);
      this.send('@DXVL=' + cartRel);
    });
  }

  browse(path: string, ext?: string): Promise<BrowseResult> {
    return new Promise((resolve, reject) => {
      if (this.ls) { clearTimeout(this.ls.timer); this.ls.reject('superseded'); }
      const s: BrowsePending = { path, id: -1, entries: [], resolve, reject, timer: null };
      this.ls = s;
      this.armLsTimer(s);
      this.send('@LS=' + path + (ext ? '\x1f' + ext : ''));
    });
  }

  reindex(): Promise<void> {
    // Wait for the firmware's @REINDEXED reply (a full /dexed scan can take minutes),
    // not a fixed delay. Falls back after 3 min so the UI never hangs forever.
    return new Promise<void>(resolve => {
      const off = this.onLine(l => { if (l.indexOf('@REINDEXED') >= 0) { clearTimeout(timer); off(); resolve(); } });
      const timer = setTimeout(() => { off(); resolve(); }, 180000);
      this.send('@REINDEX');
    });
  }

  requestState() { this.send('@STATE'); }
  saveAppState(state: unknown) { this.send('@APP=' + JSON.stringify(state)); }   // opaque app-owned blob; device stores + echoes

  // ---- actions (@-lines, relayed verbatim to the Teensy) ----
  masterVolume(pct: number) { this.send('@VOL=' + Math.max(0, Math.min(100, Math.round(pct)))); }
  dacHpf(mode: number) { this.send('@HPF=' + Math.max(0, Math.min(3, Math.round(mode)))); }
  masterBpm(bpm: number) { this.send('@BPM=' + Math.max(20, Math.min(300, Math.round(bpm)))); }
  dxVoice(i: number) { this.send('@DXVOICE=' + i); }
  dxPick(cartRel: string, voice: number) { this.send('@DXPICK=' + cartRel + '\t' + voice); }
  drumKit(i: number) { this.send('@DRUMKIT=' + i); }
  playGrooveFile(name: string) { this.send('@DRUMF=' + name); }
  stopDrums() { this.send('@DRUM=stop'); }   // unconditional stop (NOT 'D' — that toggles: a double-tap/state-mismatch would restart drums)
  drumVol(pct: number) { this.send('@DRUMVOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  songPlay(arg: string) { this.send('@SONGF=' + arg); }
  songRestart(arg: string) { this.send('@SONGRESTART=' + arg); }
  stopSong() { this.send('@SONG=stop'); }
  songVol(pct: number) { this.send('@SONGVOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  songLoop(on: boolean) { this.send('@LOOP=' + (on ? 1 : 0)); }
  launchQuantize(on: boolean) { this.send('@QUANTIZE=' + (on ? 1 : 0)); }
  metronome(on: boolean) { this.send('@METRO=' + (on ? 1 : 0)); }
  metronomeMute(muted: boolean) { this.send('@METROMUTE=' + (muted ? 1 : 0)); }
  metronomeSig(bpb: number) { this.send('@METROSIG=' + Math.max(1, Math.min(16, Math.round(bpb)))); }
  metronomeVol(pct: number) { this.send('@METROVOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  arpOn(on: boolean) { this.send('@ARPON=' + (on ? 1 : 0)); }
  arpRestart() { this.send('@ARPRESTART'); }
  arpPattern(i: number) { this.send('@ARPPAT=' + i); }
  arpRate(i: number) { this.send('@ARPRATE=' + i); }
  arpGate(pct: number) { this.send('@ARPGATE=' + Math.max(5, Math.min(150, Math.round(pct)))); }
  arpSwing(pct: number) { this.send('@ARPSWING=' + Math.max(50, Math.min(85, Math.round(pct)))); }
  arpOctaves(n: number) { this.send('@ARPOCT=' + n); }
  arpLatch(on: boolean) { this.send('@ARPLATCH=' + (on ? 1 : 0)); }
  arpSequence(steps: SeqStep[]) { this.send('@ARPSEQ=' + encodeSequence(steps)); }
  arpPreset(params: ArpWireParams) { this.send('@ARPPRESET=' + encodeArpParams(params)); }
  // ---- Voices 2 / Arp 2 (caps-gated in the UI; the wire lines are unconditional) ----
  voice2Enable(on: boolean) { this.send('@VOICE2=' + (on ? 1 : 0)); }
  voice2Vol(pct: number) { this.send('@VOICE2VOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  dxVoice2(i: number) { this.send('@DXVOICE2=' + i); }
  dxPick2(cartRel: string, voice: number) { this.send('@DXPICK2=' + cartRel + '\t' + voice); }
  arp2On(on: boolean) { this.send('@ARP2ON=' + (on ? 1 : 0)); }
  arp2Restart() { this.send('@ARP2RESTART'); }
  arp2Pattern(i: number) { this.send('@ARP2PAT=' + i); }
  arp2Rate(i: number) { this.send('@ARP2RATE=' + i); }
  arp2Gate(pct: number) { this.send('@ARP2GATE=' + Math.max(5, Math.min(150, Math.round(pct)))); }
  arp2Swing(pct: number) { this.send('@ARP2SWING=' + Math.max(50, Math.min(85, Math.round(pct)))); }
  arp2Octaves(n: number) { this.send('@ARP2OCT=' + n); }
  arp2Latch(on: boolean) { this.send('@ARP2LATCH=' + (on ? 1 : 0)); }
  arp2Sequence(steps: SeqStep[]) { this.send('@ARP2SEQ=' + encodeSequence(steps)); }
  arp2Preset(params: ArpWireParams) { this.send('@ARP2PRESET=' + encodeArpParams(params)); }
  // ---- MIDI Player 2 (level rides the voice-2 bus — there is no @SONG2VOL) ----
  song2Play(arg: string) { this.send('@SONG2F=' + arg); }
  song2Restart(arg: string) { this.send('@SONG2RESTART=' + arg); }
  stopSong2() { this.send('@SONG2=stop'); }
  song2Loop(on: boolean) { this.send('@LOOP2=' + (on ? 1 : 0)); }
  trk(index: number, cmd: string) { this.send('@TRK' + index + '.' + cmd); }
  // ---- Loop recorder ----
  recVoice(v: number) { this.send('@RECV=' + (v === 2 ? 2 : 1)); }
  recBars(n: number) { this.send('@RECBARS=' + n); }
  recSig(bpb: number) { this.send('@RECSIG=' + Math.max(1, Math.min(16, Math.round(bpb)))); }
  recArm(on: boolean) { this.send('@REC=' + (on ? 1 : 0)); }
  recOverdub(on: boolean) { this.send('@RECDUB=' + (on ? 1 : 0)); }
  recPlay(on: boolean) { this.send('@RECPLAY=' + (on ? 1 : 0)); }
  recClear() { this.send('@RECCLR'); }
  // ---- Note editor: clip dump / load (@STATE caps.recedit) ----
  recDump(v: number): Promise<Uint8Array> {
    return new Promise((resolve, reject) => {
      if (this.file) { reject('a file read is in progress'); return; }
      this.file = { path: 'mem:/loop' + v, parts: {}, resolve, reject, timer: null, total: 0, received: 0, bytes: true };
      this.armFileTimer(this.file);
      this.send('@RECDUMP=' + (v === 2 ? 2 : 1));
    });
  }
  async recLoad(v: number, bytes: Uint8Array): Promise<void> {
    v = v === 2 ? 2 : 1;
    await this.awaitReply('@RECLOAD=' + v + '\x1f' + bytes.length, '@RECOK=' + v, '@RECERR=' + v);
    for (const fr of rdFrames(v, bytes)) { this.send(fr); await new Promise(r => setTimeout(r, 30)); }  // pace the ESP32 relay
    await this.awaitReply('@RECEND=' + v, '@RECE=' + v, '@RECERR=' + v);
  }
  private awaitReply(send: string, okPrefix: string, errPrefix: string, timeoutMs = 12000): Promise<string> {
    return new Promise<string>((resolve, reject) => {
      const off = this.onLine(l => {
        if (l.startsWith(okPrefix)) { clearTimeout(t); off(); resolve(l); }
        else if (l.startsWith(errPrefix)) { clearTimeout(t); off(); reject(new Error(l)); }
      });
      const t = setTimeout(() => { off(); reject(new Error(send + ' timed out')); }, timeoutMs);
      this.send(send);
    });
  }
  // ---- Audio loop recorder ----
  audioLoopSel(i: number) { this.send('@ALSEL=' + Math.max(0, Math.round(i))); }
  audioLoopBars(n: number) { this.send('@ALBARS=' + n); }
  audioLoopMono(on: boolean) { this.send('@ALMONO=' + (on ? 1 : 0)); }
  audioLoopFollow(on: boolean) { this.send('@ALFOLLOW=' + (on ? 1 : 0)); }
  audioLoopLevel(pct: number) { this.send('@ALLEVEL=' + Math.max(0, Math.min(100, Math.round(pct)))); }
  audioLoopArm(on: boolean) { this.send('@AL=' + (on ? 1 : 0)); }
  audioLoopOverdub(on: boolean) { this.send('@ALDUB=' + (on ? 1 : 0)); }
  audioLoopPlay(on: boolean) { this.send('@ALPLAY=' + (on ? 1 : 0)); }
  audioLoopClear() { this.send('@ALCLR'); }
  audioLoopSave(name: string) { this.send('@ALSAVE=' + name); }

  // ---- local A2DP verbs (handled ON the ESP32, not relayed to the Teensy) ----
  // The USB path sends single chars to the Teensy, which relays them on to the ESP32;
  // over WiFi we talk to the ESP32 directly, so we use its '!' command set instead.
  espPair() { this.send('!pair'); }
  espReconnect() { this.send('!reconnect'); }   // "Connect Bluetooth Audio" — nothing auto-reconnects
  espDisconnect() { this.send('!disconnect'); }
  espForget() { this.send('!forget'); }
}

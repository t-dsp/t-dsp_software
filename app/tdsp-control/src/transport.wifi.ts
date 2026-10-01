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
import type { Transport, LineHandler, DirPage, ConnectOptions } from './transport';

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
  // A socket still opening. A newer connect() or a disconnect() abandons it, so a slow, cancelled
  // attempt can never land late and replace (or null out) the live connection.
  private opening: WebSocket | null = null;
  private url: string;
  private buf = '';
  private handlers = new Set<LineHandler>();
  private dropHandlers = new Set<() => void>();
  // Liveness. lastRx = when the last frame arrived on the live socket. The heartbeat below asks the
  // ESP32 for its status ('!status' is answered locally, no Teensy round trip) whenever the link has
  // been silent for a while, and declares the link dead if that goes unanswered — a half-open socket
  // (phone back from sleep, device rebooted, AP restarted) otherwise sits at readyState OPEN forever,
  // every send silently vanishing, with nothing to tell the UI.
  private lastRx = 0;
  private hbTimer: any = null;
  private hbSentAt = 0;                 // when the pending liveness probe went out (0 = none pending)
  private probeWaiters: ((alive: boolean) => void)[] = [];
  private file: FilePending | null = null;
  private dir: DirPending | null = null;
  private voices: VoicesPending | null = null;
  private ls: BrowsePending | null = null;

  // `target` may be a host ("tdsp.local", "192.168.1.42"), "host:port", or a ws:// URL.
  // mDNS caveat: .local resolution is reliable on iOS/macOS + desktop browsers, but
  // Android (React Native) often can NOT resolve .local without NSD — pass the IP there.
  constructor(target?: string) { this.url = toWsUrl(target); }

  isConnected() { return !!this.ws && this.ws.readyState === 1 /* OPEN */; }

  connect(opts?: ConnectOptions): Promise<void> {
    this.abandonOpening();
    const timeoutMs = Math.max(500, opts?.timeoutMs ?? 8000);
    return new Promise((resolve, reject) => {
      let settled = false;
      const done = (fn: () => void) => { if (settled) return; settled = true; clearTimeout(deadline); fn(); };
      const mine = () => this.opening === ws;
      // Bound the whole connect: an unreachable host (wrong LAN, device asleep, .local not
      // resolving on Android) can otherwise leave the UI's Connect button wedged.
      const deadline = setTimeout(() => done(() => { if (mine()) this.opening = null; try { ws.close(); } catch {} ; reject(new Error(`No T-DSP at ${this.url} (timed out)`)); }), timeoutMs);

      let ws: WebSocket;
      try { ws = new WebSocket(this.url); }
      catch (e) { clearTimeout(deadline); reject(e); return; }
      this.opening = ws;

      ws.onopen = () => {
        if (!mine()) { try { ws.close(); } catch {} ; return; }   // abandoned while opening
        this.opening = null; this.buf = ''; this.ws = ws; this.lastRx = Date.now(); this.startHeartbeat(); done(resolve);
      };
      // Fires for a failed connect AND for a mid-session drop; only the former rejects
      // (done() is a no-op once we've resolved).
      ws.onerror = () => done(() => { if (mine()) this.opening = null; reject(new Error(`WebSocket error connecting to ${this.url}`)); });
      ws.onclose = () => { done(() => { if (mine()) this.opening = null; reject(new Error(`Connection to ${this.url} closed`)); }); if (this.ws === ws) this.teardown(true); };
      ws.onmessage = (ev: MessageEvent) => {
        if (this.ws !== ws) return;                // an abandoned socket's frames are not ours
        if (typeof ev.data !== 'string') return;   // firmware only sends TEXT frames
        // Accumulate until '\n' — a line is NOT necessarily one frame. The firmware splits
        // long lines (a catalog @INSTR is ~7 KB) into ~1 KB chunks and terminates each line
        // with '\n', because a single oversized frame overruns the ESP32's TCP send buffer
        // and permanently wedges the socket (hardware-verified: errno 11 EAGAIN, replies
        // stop forever). So '\n' is the only frame boundary we trust — never treat an
        // arriving frame as a complete line on its own.
        this.lastRx = Date.now(); this.hbSentAt = 0; this.settleProbes(true);
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
    this.abandonOpening();
    const ws = this.ws;
    this.teardown(false);
    try { ws?.close(); } catch {}
  }

  onDrop(cb: () => void): () => void { this.dropHandlers.add(cb); return () => this.dropHandlers.delete(cb); }

  // Is the device still there? Sends a cheap local probe and waits for ANY frame back. Used when the
  // app returns to the foreground, where a stale socket must be found out in ~1 s, not whenever the
  // OS eventually notices. On silence the link is declared dead (teardown + onDrop) so the normal
  // reconnect path runs.
  probe(timeoutMs = 1500): Promise<boolean> {
    if (!this.isConnected()) return Promise.resolve(false);
    const ws = this.ws!;
    return new Promise<boolean>(resolve => {
      const sentAt = Date.now();
      const t = setTimeout(() => {
        if (this.ws !== ws) { this.settleProbes(false); return; }   // torn down meanwhile (waiters already settled)
        // Nothing arrived since the probe went out -> dead. A frame would have settled us already.
        if (this.lastRx < sentAt) this.declareDead(ws); else this.settleProbes(true);
      }, timeoutMs);
      this.probeWaiters.push(alive => { clearTimeout(t); resolve(alive); });
      this.sendProbe();
    });
  }

  private settleProbes(alive: boolean) {
    if (!this.probeWaiters.length) return;
    const w = this.probeWaiters; this.probeWaiters = [];
    w.forEach(f => f(alive));
  }

  private sendProbe() {
    if (!this.hbSentAt) this.hbSentAt = Date.now();
    this.send('!status');   // answered by the ESP32 itself as a bare JSON line (the app's status handler already consumes it)
  }

  // Idle-triggered heartbeat: no frame for HB_IDLE_MS -> probe; probe unanswered for HB_WAIT_MS -> dead.
  // The firmware pushes nothing unsolicited while idle, so without this a dead link is indistinguishable
  // from a quiet one. Costs one ~100 B exchange every HB_IDLE_MS while idle; nothing while traffic flows.
  private static readonly HB_TICK_MS = 2500;
  private static readonly HB_IDLE_MS = 10000;
  private static readonly HB_WAIT_MS = 5000;
  private startHeartbeat() {
    this.stopHeartbeat();
    const ws = this.ws;
    if (!ws) return;
    this.hbTimer = setInterval(() => {
      if (this.ws !== ws) { this.stopHeartbeat(); return; }
      const now = Date.now();
      if (this.hbSentAt) { if (now - this.hbSentAt > WiFiTransport.HB_WAIT_MS) this.declareDead(ws); return; }
      if (now - this.lastRx > WiFiTransport.HB_IDLE_MS) this.sendProbe();
    }, WiFiTransport.HB_TICK_MS);
  }
  private stopHeartbeat() { if (this.hbTimer) { clearInterval(this.hbTimer); this.hbTimer = null; } this.hbSentAt = 0; }

  // Give up on a socket that will never speak again: tear down NOW (onDrop fires), then close it
  // best-effort. We don't wait for onclose — on a half-open TCP connection it can take minutes.
  private declareDead(ws: WebSocket) {
    if (this.ws !== ws) return;
    console.warn('[tdsp] wifi link silent, declaring it dead');
    ws.onmessage = null; ws.onclose = null; ws.onerror = null;
    this.teardown(true);
    try { ws.close(); } catch {}
  }

  private abandonOpening() {
    const o = this.opening;
    this.opening = null;
    if (o) { try { o.close(); } catch {} }
  }

  // dropped: the link went away on its own (OS close / heartbeat) rather than via disconnect().
  private teardown(dropped: boolean) {
    const hadLive = !!this.ws;
    this.stopHeartbeat();
    // Don't leave reads "in progress" — the UI would spin forever.
    if (this.file) { clearTimeout(this.file.timer); this.file.reject('disconnected'); this.file = null; }
    if (this.dir) { clearTimeout(this.dir.timer); this.dir.reject('disconnected'); this.dir = null; }
    if (this.voices) { clearTimeout(this.voices.timer); this.voices.reject('disconnected'); this.voices = null; }
    if (this.ls) { clearTimeout(this.ls.timer); this.ls.reject('disconnected'); this.ls = null; }
    this.ws = null; this.buf = '';
    this.settleProbes(false);
    if (dropped && hadLive) this.dropHandlers.forEach(h => { try { h(); } catch (e) { console.warn('[tdsp] drop handler error:', e); } });
  }

  private send(line: string) { if (this.isConnected()) { try { this.ws!.send(line); } catch { /* half-open socket: the heartbeat will declare it dead */ } } }   // firmware's relayLine() adds the '\n'

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
      if (!this.isConnected()) { reject(new Error('not connected')); return; }   // fail fast: a dead link never answers
      if (this.file) { reject('a file read is in progress'); return; }
      const f: FilePending = { path, parts: {}, resolve, reject, timer: null, onProgress, total: 0, received: 0 };
      this.file = f;
      this.armFileTimer(f);   // idle watchdog; re-armed on every @FB/@FD frame
      this.send('@READ=' + path);
    });
  }

  browseDir(path: string, page = 0): Promise<DirPage> {
    return new Promise((resolve, reject) => {
      if (!this.isConnected()) { reject(new Error('not connected')); return; }   // fail fast: a dead link never answers
      if (this.dir) { clearTimeout(this.dir.timer); this.dir.reject('superseded'); }
      const d: DirPending = { path, resolve, reject, timer: null };
      this.dir = d;
      d.timer = setTimeout(() => { if (this.dir === d) { this.dir = null; reject('timeout'); } }, 8000);
      this.send('@DXLS=' + path + (page ? '\t' + page : ''));
    });
  }

  cartVoices(cartRel: string): Promise<string[]> {
    return new Promise((resolve, reject) => {
      if (!this.isConnected()) { reject(new Error('not connected')); return; }   // fail fast: a dead link never answers
      if (this.voices) { clearTimeout(this.voices.timer); this.voices.reject('superseded'); }
      const v: VoicesPending = { rel: cartRel, resolve, reject, timer: null };
      this.voices = v;
      v.timer = setTimeout(() => { if (this.voices === v) { this.voices = null; reject('timeout'); } }, 8000);
      this.send('@DXVL=' + cartRel);
    });
  }

  browse(path: string, ext?: string): Promise<BrowseResult> {
    return new Promise((resolve, reject) => {
      if (!this.isConnected()) { reject(new Error('not connected')); return; }   // fail fast: a dead link never answers
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
    if (!this.isConnected()) return Promise.resolve();   // nothing to rebuild over a dead link
    return new Promise<void>(resolve => {
      const off = this.onLine(l => { if (l.indexOf('@REINDEXED') >= 0) { clearTimeout(timer); off(); resolve(); } });
      const timer = setTimeout(() => { off(); resolve(); }, 180000);
      this.send('@REINDEX');
    });
  }

  requestState() { this.send('@STATE'); }
  saveAppState(state: unknown) { this.send('@APP=' + JSON.stringify(state)); }   // opaque app-owned blob; device stores + echoes
  mpeMonitor(on: boolean) { this.send('@MPEMON=' + (on ? 1 : 0)); }   // live MPE input/chain trace (@MPE= lines) on/off
  midiMode(mpe: boolean) { this.send('@MIDIMODE=' + (mpe ? 1 : 0)); }   // global MPE (bend ±24) vs normal MIDI (bend ±2)

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
  requestFonts() { this.send('@FONTS'); }
  drumFont(path: string) { this.send('@DRUMFONT=' + path); }
  songPlay(arg: string) { this.send('@SONGF=' + arg); }
  songRestart(arg: string) { this.send('@SONGRESTART=' + arg); }
  stopSong() { this.send('@SONG=stop'); }
  songVol(pct: number) { this.send('@SONGVOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  songLoop(on: boolean) { this.send('@LOOP=' + (on ? 1 : 0)); }
  launchQuantize(on: boolean) { this.send('@QUANTIZE=' + (on ? 1 : 0)); }
  panic() { this.send('@PANIC'); }
  metronome(on: boolean) { this.send('@METRO=' + (on ? 1 : 0)); }
  metronomeMute(muted: boolean) { this.send('@METROMUTE=' + (muted ? 1 : 0)); }
  metronomeSig(bpb: number) { this.send('@METROSIG=' + Math.max(1, Math.min(16, Math.round(bpb)))); }
  metronomeVol(pct: number) { this.send('@METROVOL=' + Math.max(0, Math.min(150, Math.round(pct)))); }
  metronomeLock(on: boolean) { this.send('@METROLOCK=' + (on ? 1 : 0)); }
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
  poolPreset(preset: number) { this.send('@POOL=' + (preset | 0)); }
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
  fx(cmd: string) { this.send('@FX.' + cmd); }
  // ---- Loop recorder ----
  recVoice(v: number) { this.send('@RECV=' + (Math.max(1, v))); }
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
      this.send('@RECDUMP=' + (Math.max(1, v)));
    });
  }
  async recLoad(v: number, bytes: Uint8Array): Promise<void> {
    v = Math.max(1, v);
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
  usbAudioGain(pct: number) { this.send('@USBGAIN=' + Math.max(0, Math.min(150, Math.round(pct)))); }

  // ---- local A2DP verbs (handled ON the ESP32, not relayed to the Teensy) ----
  // The USB path sends single chars to the Teensy, which relays them on to the ESP32;
  // over WiFi we talk to the ESP32 directly, so we use its '!' command set instead.
  espPair() { this.send('!pair'); }
  espReconnect() { this.send('!reconnect'); }   // "Connect Bluetooth Audio" — nothing auto-reconnects
  espDisconnect() { this.send('!disconnect'); }
  espForget() { this.send('!forget'); }
}

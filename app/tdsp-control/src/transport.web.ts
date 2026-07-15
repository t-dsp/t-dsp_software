// transport.web.ts — desktop (react-native-web) transport over the Web Serial API.
// Speaks the @-line protocol directly to the Teensy USB CDC port. This IS the new
// control.html. Chromium-only; requires a secure context (localhost / https).

import { parseDxls } from './transport';
import type { Transport, LineHandler, DirPage } from './transport';

interface FilePending { path: string; parts: Record<number, string>; resolve: (t: string) => void; reject: (e: any) => void; timer: any; }
interface DirPending { path: string; resolve: (d: DirPage) => void; reject: (e: any) => void; timer: any; }
interface VoicesPending { rel: string; resolve: (v: string[]) => void; reject: (e: any) => void; timer: any; }

export class WebSerialTransport implements Transport {
  readonly name = 'USB' as const;
  private port: any = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private buf = '';
  private handlers = new Set<LineHandler>();
  private file: FilePending | null = null;
  private dir: DirPending | null = null;
  private voices: VoicesPending | null = null;
  private enc = new TextEncoder();
  private dec = new TextDecoder();

  isConnected() { return !!this.port; }

  async connect(): Promise<void> {
    const nav: any = navigator;
    if (!nav.serial) throw new Error('Web Serial not supported (use Chrome/Edge on desktop).');
    this.port = await nav.serial.requestPort();
    try { await this.port.open({ baudRate: 115200 }); }
    catch (e: any) {
      if (e?.name !== 'InvalidStateError') throw e;
      // Already open — usually a stale handle Chrome kept across a page refresh, whose
      // reader/writer are dead. Close and reopen so we get fresh, working streams.
      try { await this.port.close(); } catch {}
      await this.port.open({ baudRate: 115200 });
    }
    this.buf = '';
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.readLoop();
  }

  async disconnect(): Promise<void> {
    if (this.file) { clearTimeout(this.file.timer); this.file.reject('disconnected'); this.file = null; }  // don't leave a read "in progress"
    if (this.dir) { clearTimeout(this.dir.timer); this.dir.reject('disconnected'); this.dir = null; }
    if (this.voices) { clearTimeout(this.voices.timer); this.voices.reject('disconnected'); this.voices = null; }
    try { await this.reader?.cancel(); this.reader?.releaseLock(); } catch {}
    try { await this.writer?.close(); this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
    this.port = this.writer = this.reader = null; this.buf = '';
  }

  private async readLoop() {
    try {
      for (;;) {
        const { value, done } = await this.reader!.read();
        if (done) break;
        this.buf += this.dec.decode(value, { stream: true });
        let i: number;
        while ((i = this.buf.indexOf('\n')) >= 0) {
          const line = this.buf.slice(0, i).replace(/\r$/, '');
          this.buf = this.buf.slice(i + 1);
          if (line) this.onDeviceLine(line);
        }
      }
    } catch (e) { console.warn('[tdsp] read loop ended:', e); }   // disconnect, or a dead/stale port
  }

  private send(line: string) { this.writer?.write(this.enc.encode(line + '\n')); }

  onLine(cb: LineHandler): () => void { this.handlers.add(cb); return () => this.handlers.delete(cb); }

  // (Re)arm the read watchdog. It's an IDLE timeout: as long as frames keep arriving we
  // keep waiting, so a big file (e.g. a 3,700-cart /dexed) that streams for a while — or
  // trickles over the paced BLE relay — completes instead of being cut off mid-transfer.
  private armFileTimer(f: FilePending) {
    clearTimeout(f.timer);
    f.timer = setTimeout(() => { if (this.file === f) { this.file = null; f.reject('timeout'); } }, 15000);
  }

  private onDeviceLine(line: string) {
    if (line.indexOf('DXLS') >= 0 || line.indexOf('DXVL') >= 0) console.log('[tdsp] rx:', JSON.stringify(line.slice(0, 80)), 'dirPending=', JSON.stringify(this.dir?.path), 'voicesPending=', JSON.stringify(this.voices?.rel));
    // @READ frame transport
    if (line.startsWith('@FB=')) { if (this.file) { this.file.parts = {}; this.armFileTimer(this.file); } return; }
    if (line.startsWith('@FD=')) {
      const p = line.slice(4).split('\x1f');
      if (this.file && p.length === 3) { this.file.parts[+p[1]] = p[2]; this.armFileTimer(this.file); }
      return;
    }
    if (line.startsWith('@FE=')) {
      const f = this.file;
      if (f) {
        const b64 = Object.keys(f.parts).map(Number).sort((a, b) => a - b).map(k => f.parts[k]).join('');
        let txt: string;
        try { txt = decodeURIComponent(escape(atob(b64))); } catch { txt = atob(b64); }
        clearTimeout(f.timer); this.file = null; f.resolve(txt);
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
    // everything else -> subscribers (heartbeats, BT status, etc.)
    this.handlers.forEach(h => h(line));
  }

  readFile(path: string): Promise<string> {
    return new Promise((resolve, reject) => {
      if (this.file) { reject('a file read is in progress'); return; }
      const f: FilePending = { path, parts: {}, resolve, reject, timer: null };
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
      const cmd = '@DXLS=' + path + (page ? '\t' + page : '');
      console.log('[tdsp] tx:', JSON.stringify(cmd), 'writer=', !!this.writer);
      this.send(cmd);
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

  // ---- actions (@-lines) ----
  masterVolume(pct: number) { this.send('@VOL=' + Math.max(0, Math.min(100, Math.round(pct)))); }
  masterBpm(bpm: number) { this.send('@BPM=' + Math.max(20, Math.min(300, Math.round(bpm)))); }
  dxVoice(i: number) { this.send('@DXVOICE=' + i); }
  dxPick(cartRel: string, voice: number) { this.send('@DXPICK=' + cartRel + '\t' + voice); }
  drumKit(i: number) { this.send('@DRUMKIT=' + i); }
  playGrooveFile(name: string) { this.send('@DRUMF=' + name); }
  stopDrums() { this.send('D'); }
  playSong(i: number) { this.send('@SONG=' + i); }
  stopSong() { this.send('@SONG=stop'); }
  songLoop(on: boolean) { this.send('@LOOP=' + (on ? 1 : 0)); }
  arpOn(on: boolean) { this.send('@ARPON=' + (on ? 1 : 0)); }
  arpPattern(i: number) { this.send('@ARPPAT=' + i); }
  arpRate(i: number) { this.send('@ARPRATE=' + i); }
  arpOctaves(n: number) { this.send('@ARPOCT=' + n); }
  arpLatch(on: boolean) { this.send('@ARPLATCH=' + (on ? 1 : 0)); }
  espPair() { this.send('P'); }
  espReconnect() { this.send('r'); }
  espForget() { this.send('F'); }
}

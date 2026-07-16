// transport.native.ts — app (native) transport over BLE (react-native-ble-plx).
//
// Implements the SAME Transport interface as the Web Serial build, so the UI is
// transport-agnostic. The device is the ESP32 receiver (BLE GATT "T-DSP Control");
// it bridges to the Teensy over UART. The new catalog system is file-based (@READ),
// so this transport speaks the identical @-protocol as transport.web.ts:
//
//   * every Teensy control line (@READ / @REINDEX / @DXPICK / @SONG / @DRUMF / @BPM /
//     @ARP* …) is sent VERBATIM via the generic CMD.RELAY_LINE opcode — no per-command
//     opcode, no byte-range limit (so the full /dexed library + 320 bundled voices work);
//   * ESP32-LOCAL Bluetooth control (pairing / reconnect / forget, master volume) uses
//     its own opcodes — those act on the receiver, not the Teensy;
//   * @READ file frames (@FB/@FD/@FE/@FERR) come back on the FILE characteristic, one
//     line per notification; we reassemble them exactly like the web transport does;
//   * the STATUS characteristic's JSON is forwarded to onLine() subscribers so the
//     shared UI handler ({"conn":…,"vol":…}) works unchanged.
//
// react-native-ble-plx is a native module — requires an EAS dev/preview build (NOT
// Expo Go). Mirrors the firmware contract in projects/t-dsp_esp32_bt_receiver/src/main.cpp.

import { PermissionsAndroid, Platform } from 'react-native';
import { BleManager, Device, State, Subscription } from 'react-native-ble-plx';
import { parseDxls } from './dxls';
import type { Transport, LineHandler, DirPage } from './transport';
import { CMD, TDSP_SVC_UUID, TDSP_CMD_UUID, TDSP_STAT_UUID, TDSP_FILE_UUID } from './tdspBle';

const FUS = '\x1f';   // field separator inside @FD/@FE frames (matches the firmware)
const RS = '\x1e';    // record separator for chunked browse replies (matches ESP32 setCatalog)

// --- base64 (react-native-ble-plx characteristic values are base64) ----------
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function bytesToBase64(bytes: number[]): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i], b1 = i + 1 < bytes.length ? bytes[i + 1] : 0, b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
    out += B64[b0 >> 2] + B64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < bytes.length ? B64[b2 & 63] : '=';
  }
  return out;
}
function base64ToString(b64: string): string {
  let out = '';
  const clean = b64.replace(/[^A-Za-z0-9+/]/g, '');
  for (let i = 0; i < clean.length; i += 4) {
    const n0 = B64.indexOf(clean[i]), n1 = B64.indexOf(clean[i + 1]), n2 = B64.indexOf(clean[i + 2]), n3 = B64.indexOf(clean[i + 3]);
    out += String.fromCharCode((n0 << 2) | (n1 >> 4));
    if (n2 >= 0) out += String.fromCharCode(((n1 & 15) << 4) | (n2 >> 2));
    if (n3 >= 0) out += String.fromCharCode(((n2 & 3) << 6) | n3);
  }
  return out;
}

async function ensurePermissions(): Promise<boolean> {
  if (Platform.OS !== 'android') return true;   // iOS prompts via the BLE stack
  const api = typeof Platform.Version === 'number' ? Platform.Version : parseInt(String(Platform.Version), 10);
  try {
    if (api >= 31) {
      const r = await PermissionsAndroid.requestMultiple([
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      ]);
      return r[PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN] === 'granted'
        && r[PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT] === 'granted';
    }
    return (await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION)) === 'granted';
  } catch { return false; }
}

interface FilePending { resolve: (t: string) => void; reject: (e: any) => void; timer: any; onProgress?: (r: number, t: number) => void; total: number; received: number; }
// Decoded byte count of a base64 chunk (for streaming progress; avoids decoding mid-stream).
const b64bytes = (s: string) => Math.max(0, Math.floor(s.replace(/=+$/, '').length * 3 / 4));
interface DirPending { path: string; resolve: (d: DirPage) => void; reject: (e: any) => void; timer: any; }
interface VoicesPending { rel: string; resolve: (v: string[]) => void; reject: (e: any) => void; timer: any; }

export class BleTransport implements Transport {
  readonly name = 'BLE' as const;
  private mgr = new BleManager();
  private device: Device | null = null;
  private subs: Subscription[] = [];
  private handlers = new Set<LineHandler>();
  // one in-flight @READ: assembling frames + the pending promise
  private fileAsm: { id?: string; chunks: (string | undefined)[] } | null = null;
  private filePending: FilePending | null = null;
  private reindexDone: (() => void) | null = null;
  // Lazy /dexed browse: @DXLS/@DXVL replies arrive chunk-framed on the FILE char (a folder
  // page can exceed one MTU), reassembled here then matched to the pending request.
  private browseAsm: { count: number; chunks: (string | undefined)[] } | null = null;
  private dirPending: DirPending | null = null;
  private voicesPending: VoicesPending | null = null;

  isConnected() { return !!this.device; }

  async connect(): Promise<void> {
    if (!(await ensurePermissions())) throw new Error('Bluetooth permission denied');
    await this.waitPoweredOn();
    const dev = await this.scanAndConnect();
    await dev.discoverAllServicesAndCharacteristics();
    if (Platform.OS === 'android') { try { await dev.requestMTU(512); } catch {} }
    this.device = dev;
    dev.onDisconnected(() => this.teardown());
    // File-transfer stream: @FB/@FD/@FE/@FERR + @REINDEXED, one line per notification.
    this.subs.push(dev.monitorCharacteristicForService(TDSP_SVC_UUID, TDSP_FILE_UUID,
      (err, ch) => { if (!err && ch?.value) this.onFileLine(base64ToString(ch.value)); }));
    // Status JSON → forward to subscribers so the shared UI handler works.
    try { const c = await dev.readCharacteristicForService(TDSP_SVC_UUID, TDSP_STAT_UUID); if (c.value) this.emit(base64ToString(c.value)); } catch {}
    this.subs.push(dev.monitorCharacteristicForService(TDSP_SVC_UUID, TDSP_STAT_UUID,
      (err, ch) => { if (!err && ch?.value) this.emit(base64ToString(ch.value)); }));
  }

  async disconnect(): Promise<void> {
    const dev = this.device;
    this.teardown();
    try { await dev?.cancelConnection(); } catch {}
  }

  private teardown() {
    this.subs.forEach(s => { try { s.remove(); } catch {} });
    this.subs = [];
    if (this.filePending) { clearTimeout(this.filePending.timer); this.filePending.reject('disconnected'); this.filePending = null; }
    if (this.dirPending) { clearTimeout(this.dirPending.timer); this.dirPending.reject(new Error('disconnected')); this.dirPending = null; }
    if (this.voicesPending) { clearTimeout(this.voicesPending.timer); this.voicesPending.reject(new Error('disconnected')); this.voicesPending = null; }
    this.fileAsm = null; this.browseAsm = null;
    if (this.reindexDone) { const r = this.reindexDone; this.reindexDone = null; r(); }
    this.device = null;
  }

  private waitPoweredOn(): Promise<void> {
    return new Promise((resolve, reject) => {
      const sub = this.mgr.onStateChange(s => { if (s === State.PoweredOn) { sub.remove(); resolve(); } }, true);
      setTimeout(() => { sub.remove(); reject(new Error('Bluetooth is off')); }, 8000);
    });
  }

  private scanAndConnect(): Promise<Device> {
    return new Promise((resolve, reject) => {
      let done = false;
      this.mgr.startDeviceScan([TDSP_SVC_UUID], null, async (err, dev) => {
        if (err) { if (!done) { done = true; this.mgr.stopDeviceScan(); reject(err); } return; }
        if (!dev || done) return;
        done = true; this.mgr.stopDeviceScan();
        try { resolve(await dev.connect()); } catch (e) { reject(e); }
      });
      setTimeout(() => { if (!done) { done = true; this.mgr.stopDeviceScan(); reject(new Error('No T-DSP device found')); } }, 15000);
    });
  }

  onLine(cb: LineHandler): () => void { this.handlers.add(cb); return () => this.handlers.delete(cb); }
  private emit(line: string) { this.handlers.forEach(h => h(line)); }

  // (Re)arm the read watchdog. IDLE timeout: as long as frames keep arriving we keep
  // waiting, so a big file (e.g. a 3,700-cart /dexed) that trickles over the paced BLE
  // relay completes instead of being cut off mid-transfer.
  private armFileTimer(p: FilePending) {
    clearTimeout(p.timer);
    p.timer = setTimeout(() => { if (this.filePending === p) { this.filePending = null; this.fileAsm = null; p.reject(new Error('timeout')); } }, 15000);
  }

  // Resolve a fully-reassembled @DXLS/@DXVL reply against its pending request.
  private dispatchBrowse(line: string) {
    if (line.startsWith('@DXLS=')) {
      const d = this.dirPending; if (!d) return;
      const dp = parseDxls(line.slice(6));
      if (dp.path === d.path) { clearTimeout(d.timer); this.dirPending = null; d.resolve(dp); }
    } else if (line.startsWith('@DXVL=')) {
      const v = this.voicesPending; if (!v) return;
      const p = line.slice(6).split('|'); const rc = p.shift();
      if (rc === v.rel) { clearTimeout(v.timer); this.voicesPending = null; v.resolve(p); }
    }
  }

  // ---- @READ file transport (frames arrive on the FILE characteristic) --------
  private onFileLine(line: string) {
    // Chunked browse reply ("<seq>\x1e<count>\x1e<payload>"): reassemble, then dispatch. The
    // digit prefix is unambiguous vs the '@'-prefixed file frames sharing this characteristic.
    const r1 = line.indexOf(RS);
    if (r1 > 0 && /^\d+$/.test(line.slice(0, r1))) {
      const r2 = line.indexOf(RS, r1 + 1);
      if (r2 > r1) {
        const seq = parseInt(line.slice(0, r1), 10);
        const count = parseInt(line.slice(r1 + 1, r2), 10);
        const payload = line.slice(r2 + 1);
        if (!this.browseAsm || this.browseAsm.count !== count) this.browseAsm = { count, chunks: [] };
        this.browseAsm.chunks[seq] = payload;
        let full = '', ok = true;
        for (let i = 0; i < count; i++) { if (this.browseAsm.chunks[i] == null) { ok = false; break; } full += this.browseAsm.chunks[i]; }
        if (ok) { this.browseAsm = null; this.dispatchBrowse(full); }
        return;
      }
    }
    if (line.startsWith('@REINDEXED')) { const r = this.reindexDone; this.reindexDone = null; r?.(); return; }
    if (line.startsWith('@FB=')) { const f = line.slice(4).split(FUS); this.fileAsm = { id: f[0], chunks: [] }; if (this.filePending) { this.filePending.total = +f[2] || 0; this.filePending.received = 0; this.armFileTimer(this.filePending); this.filePending.onProgress?.(0, this.filePending.total); } return; }
    if (line.startsWith('@FD=')) { const f = line.slice(4).split(FUS); const a = this.fileAsm; if (a && f[0] === a.id) a.chunks[parseInt(f[1], 10)] = f[2]; if (this.filePending) { this.filePending.received += b64bytes(f[2]); this.armFileTimer(this.filePending); this.filePending.onProgress?.(this.filePending.received, this.filePending.total); } return; }
    if (line.startsWith('@FE=')) {
      const f = line.slice(4).split(FUS), a = this.fileAsm, p = this.filePending;
      if (a && p) {
        const count = parseInt(f[1], 10); let b64 = '', ok = true;
        for (let i = 0; i < count; i++) { if (a.chunks[i] == null) { ok = false; break; } b64 += a.chunks[i]; }
        clearTimeout(p.timer); this.filePending = null; this.fileAsm = null;
        if (ok) { try { p.resolve(decodeURIComponent(escape(base64ToString(b64)))); } catch { p.resolve(base64ToString(b64)); } }
        else p.reject(new Error('missing file chunk'));
      }
      return;
    }
    if (line.startsWith('@FERR=')) { const p = this.filePending; if (p) { clearTimeout(p.timer); this.filePending = null; this.fileAsm = null; p.reject(new Error(line.slice(6))); } return; }
  }

  readFile(path: string, onProgress?: (received: number, total: number) => void): Promise<string> {
    return new Promise((resolve, reject) => {
      if (!this.device) { reject(new Error('not connected')); return; }
      if (this.filePending) { reject(new Error('a file read is in progress')); return; }
      const p: FilePending = { resolve, reject, timer: null, onProgress, total: 0, received: 0 };
      this.filePending = p;
      this.fileAsm = { id: undefined, chunks: [] };
      this.armFileTimer(p);   // idle watchdog; re-armed on every @FB/@FD frame
      this.relay('@READ=' + path).catch(e => { clearTimeout(p.timer); this.filePending = null; reject(e); });
    });
  }

  browseDir(path: string, page = 0): Promise<DirPage> {
    return new Promise((resolve, reject) => {
      if (!this.device) { reject(new Error('not connected')); return; }
      if (this.dirPending) { clearTimeout(this.dirPending.timer); this.dirPending.reject(new Error('superseded')); }
      const d: DirPending = { path, resolve, reject, timer: null };
      this.dirPending = d; this.browseAsm = null;
      d.timer = setTimeout(() => { if (this.dirPending === d) { this.dirPending = null; reject(new Error('timeout')); } }, 12000);
      this.relay('@DXLS=' + path + (page ? '\t' + page : '')).catch(e => { clearTimeout(d.timer); this.dirPending = null; reject(e); });
    });
  }

  cartVoices(cartRel: string): Promise<string[]> {
    return new Promise((resolve, reject) => {
      if (!this.device) { reject(new Error('not connected')); return; }
      if (this.voicesPending) { clearTimeout(this.voicesPending.timer); this.voicesPending.reject(new Error('superseded')); }
      const v: VoicesPending = { rel: cartRel, resolve, reject, timer: null };
      this.voicesPending = v; this.browseAsm = null;
      v.timer = setTimeout(() => { if (this.voicesPending === v) { this.voicesPending = null; reject(new Error('timeout')); } }, 12000);
      this.relay('@DXVL=' + cartRel).catch(e => { clearTimeout(v.timer); this.voicesPending = null; reject(e); });
    });
  }

  reindex(): Promise<void> {
    return new Promise<void>(resolve => {
      const timer = setTimeout(() => { if (this.reindexDone) { this.reindexDone = null; resolve(); } }, 180000);
      this.reindexDone = () => { clearTimeout(timer); resolve(); };
      this.relay('@REINDEX').catch(() => { clearTimeout(timer); this.reindexDone = null; resolve(); });
    });
  }

  // ---- write helpers ----------------------------------------------------------
  private async write(bytes: number[]) {
    const d = this.device; if (!d) return;
    try { await d.writeCharacteristicWithResponseForService(TDSP_SVC_UUID, TDSP_CMD_UUID, bytesToBase64(bytes)); }
    catch (e) { console.warn('[tdsp-ble] write failed:', e); }
  }
  private cmd(op: number) { return this.write([op & 0xff]); }
  private byte(op: number, v: number) { return this.write([op & 0xff, v & 0xff]); }
  // Relay a literal @-line to the Teensy — the generic seam that mirrors Web Serial.
  private relay(line: string): Promise<void> {
    const b = [CMD.RELAY_LINE & 0xff];
    for (let i = 0; i < line.length; i++) b.push(line.charCodeAt(i) & 0xff);
    return this.write(b);
  }

  requestState() { this.relay('@STATE'); }

  // ---- actions (identical @-lines to transport.web.ts, over the relay) --------
  masterVolume(pct: number) { this.byte(CMD.SET_VOLUME, Math.max(0, Math.min(100, Math.round(pct)))); }   // opcode: ESP32 caches for status
  masterBpm(bpm: number) { this.relay('@BPM=' + Math.max(20, Math.min(300, Math.round(bpm)))); }
  dxVoice(i: number) { this.relay('@DXVOICE=' + i); }
  dxPick(cartRel: string, voice: number) { this.relay('@DXPICK=' + cartRel + '\t' + voice); }
  drumKit(i: number) { this.relay('@DRUMKIT=' + i); }
  playGrooveFile(name: string) { this.relay('@DRUMF=' + name); }
  stopDrums() { this.relay('D'); }
  songPlay(arg: string) { this.relay('@SONGF=' + arg); }
  stopSong() { this.relay('@SONG=stop'); }
  songLoop(on: boolean) { this.relay('@LOOP=' + (on ? 1 : 0)); }
  arpOn(on: boolean) { this.relay('@ARPON=' + (on ? 1 : 0)); }
  arpPattern(i: number) { this.relay('@ARPPAT=' + i); }
  arpRate(i: number) { this.relay('@ARPRATE=' + i); }
  arpOctaves(n: number) { this.relay('@ARPOCT=' + n); }
  arpLatch(on: boolean) { this.relay('@ARPLATCH=' + (on ? 1 : 0)); }
  // ESP32-LOCAL Bluetooth control (act on the receiver, not the Teensy) → opcodes.
  espPair() { this.cmd(CMD.PAIRING_MODE); }
  espReconnect() { this.cmd(CMD.RECONNECT); }
  espDisconnect() { this.cmd(CMD.DISCONNECT); }
  espForget() { this.cmd(CMD.FORGET); }
}

// transport.native.ts — app (native) transport over BLE (react-native-ble-plx).
//
// TODO(native BLE): implement against the device's GATT service by adapting the proven
// logic in src/tdspBle.ts (connect/scan, the CMD opcodes, and the FILE-characteristic
// @FB/@FD/@FE reassembly used by readFile). This stub keeps the native build compiling
// while the web target (transport.web.ts) is developed + verified against COM4 first.
// Each action maps to its BLE opcode (SET_DX_VOICE, PLAY_DRUM_FILE, …); readFile/reindex
// use READ_FILE / REFRESH_CAT.

import type { Transport, LineHandler } from './transport';

const TODO = 'native BLE transport not wired yet — use the web build (react-native-web + Web Serial) for now';

export class BleTransport implements Transport {
  readonly name = 'BLE' as const;
  private handlers = new Set<LineHandler>();
  isConnected() { return false; }
  async connect() { throw new Error(TODO); }
  async disconnect() {}
  onLine(cb: LineHandler) { this.handlers.add(cb); return () => this.handlers.delete(cb); }
  async readFile(_path: string): Promise<string> { throw new Error(TODO); }
  async reindex() { throw new Error(TODO); }
  masterVolume(_p: number) {}
  masterBpm(_b: number) {}
  dxVoice(_i: number) {}
  dxPick(_c: string, _v: number) {}
  drumKit(_i: number) {}
  playGrooveFile(_n: string) {}
  stopDrums() {}
  playSong(_i: number) {}
  stopSong() {}
  songLoop(_o: boolean) {}
  arpOn(_o: boolean) {}
  arpPattern(_i: number) {}
  arpRate(_i: number) {}
  arpOctaves(_n: number) {}
  arpLatch(_o: boolean) {}
  espPair() {}
  espReconnect() {}
  espForget() {}
}

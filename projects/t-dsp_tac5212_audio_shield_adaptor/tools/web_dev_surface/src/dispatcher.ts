// Bridges the reactive mixer state to OSC.
//
// Outbound: UI gestures call into setChannelFader/etc. We optimistically update
// the local signal, then send an OSC message. The firmware echoes back; the
// echo lands in handleIncoming() and re-applies (idempotent for matching values).
//
// Inbound: handleIncoming() receives a parsed OscMessage and routes by address
// pattern. Unknown addresses are ignored (the console pane logs them anyway).
//
// Address conventions follow planning/osc-mixer-foundation/02-osc-protocol.md
// (X32-flavored: /ch/NN, /bus/NN, /main/st, faders 0..1, mute = mix/on inverted).

import { encodeMessage, OscArg, OscMessage } from './osc';
import { MixerState } from './state';

const pad2 = (n: number): string => n.toString().padStart(2, '0');

export class Dispatcher {
  constructor(
    private state: MixerState,
    private send: (packet: Uint8Array) => void,
  ) {}

  // ---------- outbound (UI -> firmware) ----------

  setChannelFader(idx: number, v: number): void {
    this.state.channels[idx].fader.set(v);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/fader`, 'f', [v]);
  }

  setChannelOn(idx: number, on: boolean): void {
    this.state.channels[idx].on.set(on);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/on`, 'i', [on ? 1 : 0]);
  }

  setChannelSolo(idx: number, solo: boolean): void {
    this.state.channels[idx].solo.set(solo);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/solo`, 'i', [solo ? 1 : 0]);
  }

  setMainFader(v: number): void {
    this.state.main.fader.set(v);
    this.sendMsg('/main/st/mix/fader', 'f', [v]);
  }

  setMainOn(on: boolean): void {
    this.state.main.on.set(on);
    this.sendMsg('/main/st/mix/on', 'i', [on ? 1 : 0]);
  }

  // /sub addSub i i s — interval ms, lifetime ms, address pattern
  // (per 02-osc-protocol.md "Subscriptions follow the X32 /xremote idiom").
  // The exact wire format will be confirmed when M8 SubscriptionMgr lands;
  // adjust here if it diverges.
  subscribeMeters(): void {
    this.sendMsg('/sub', 'sis', ['addSub', 1000, '/meters/input']);
    this.sendMsg('/sub', 'sis', ['addSub', 1000, '/meters/output']);
    this.state.metersOn.set(true);
  }

  unsubscribeMeters(): void {
    this.sendMsg('/sub', 'ss', ['unsubscribe', '/meters/input']);
    this.sendMsg('/sub', 'ss', ['unsubscribe', '/meters/output']);
    this.state.metersOn.set(false);
    // Reset all meter displays to 0 so the bars don't look frozen at
    // whatever the last sampled value was. The firmware has stopped
    // streaming so no more blobs will arrive to clear them.
    for (const ch of this.state.channels) {
      ch.peak.set(0);
      ch.rms.set(0);
    }
  }

  // Used by the raw OSC input field. Bypasses the typed setters above.
  sendRaw(address: string, types: string, args: OscArg[]): void {
    this.sendMsg(address, types, args);
  }

  // ---------- inbound (firmware -> UI) ----------

  handleIncoming(msg: OscMessage): void {
    const a = msg.address;

    let m = a.match(/^\/ch\/(\d+)\/mix\/fader$/);
    if (m && msg.types === 'f') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.fader.set(msg.args[0] as number);
      return;
    }

    m = a.match(/^\/ch\/(\d+)\/mix\/on$/);
    if (m && msg.types === 'i') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.on.set((msg.args[0] as number) !== 0);
      return;
    }

    m = a.match(/^\/ch\/(\d+)\/mix\/solo$/);
    if (m && msg.types === 'i') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.solo.set((msg.args[0] as number) !== 0);
      return;
    }

    m = a.match(/^\/ch\/(\d+)\/config\/name$/);
    if (m && msg.types === 's') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.name.set(msg.args[0] as string);
      return;
    }

    if (a === '/main/st/mix/fader' && msg.types === 'f') {
      this.state.main.fader.set(msg.args[0] as number);
      return;
    }

    if (a === '/main/st/mix/on' && msg.types === 'i') {
      this.state.main.on.set((msg.args[0] as number) !== 0);
      return;
    }

    // /meters/input b — blob of float32 pairs (peak, rms) per channel,
    // big-endian, in declared channel order. See 02-osc-protocol.md "Meters
    // are blobs, not individual messages."
    if (a === '/meters/input' && msg.types === 'b') {
      const blob = msg.args[0] as Uint8Array;
      const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
      const pairCount = Math.min(this.state.channels.length, Math.floor(blob.length / 8));
      for (let i = 0; i < pairCount; i++) {
        this.state.channels[i].peak.set(dv.getFloat32(i * 8, false));
        this.state.channels[i].rms.set(dv.getFloat32(i * 8 + 4, false));
      }
      return;
    }

    // /meters/output b — blob of 2 float32 pairs for the stereo main
    // bus: [L peak, L rms, R peak, R rms], big-endian, 16 bytes total.
    if (a === '/meters/output' && msg.types === 'b') {
      const blob = msg.args[0] as Uint8Array;
      if (blob.length >= 16) {
        const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
        this.state.main.peakL.set(dv.getFloat32(0,  false));
        this.state.main.rmsL.set( dv.getFloat32(4,  false));
        this.state.main.peakR.set(dv.getFloat32(8,  false));
        this.state.main.rmsR.set( dv.getFloat32(12, false));
      }
      return;
    }
  }

  private sendMsg(address: string, types: string, args: OscArg[]): void {
    this.send(encodeMessage(address, types, args));
  }
}

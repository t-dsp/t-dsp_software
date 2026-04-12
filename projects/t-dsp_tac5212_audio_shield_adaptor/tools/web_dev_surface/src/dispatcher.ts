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
  // Callback sink for /spectrum/main b blobs. Set by the spectrum
  // view's owner so the 1024-byte payload can bypass MixerState and
  // land directly in the spectrum canvas renderer — putting 1 KB of
  // bins through a Signal every 33 ms would fire a lot of listeners
  // for no gain.
  private spectrumSink: ((bytes: Uint8Array) => void) | null = null;

  constructor(
    private state: MixerState,
    private send: (packet: Uint8Array) => void,
  ) {}

  setSpectrumSink(fn: ((bytes: Uint8Array) => void) | null): void {
    this.spectrumSink = fn;
  }

  // ---------- outbound (UI -> firmware) ----------

  // Returns the 0-based partner index for `idx` if the pair is linked,
  // else -1. Only the ODD 1-based channel (idx 0, 2, 4) carries the
  // link flag in the model, so we check the odd side regardless of
  // which end the user touched.
  private linkedPartner(idx: number): number {
    const channels = this.state.channels;
    if ((idx & 1) === 0) {
      // 0-based even == 1-based odd: partner is idx+1 if OUR link is on.
      if (idx + 1 < channels.length && channels[idx].link.get()) return idx + 1;
      return -1;
    } else {
      // 0-based odd == 1-based even: partner is idx-1 if THEIR link is on.
      if (idx - 1 >= 0 && channels[idx - 1].link.get()) return idx - 1;
      return -1;
    }
  }

  setChannelFader(idx: number, v: number): void {
    this.state.channels[idx].fader.set(v);
    const p = this.linkedPartner(idx);
    if (p >= 0) this.state.channels[p].fader.set(v);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/fader`, 'f', [v]);
  }

  setChannelOn(idx: number, on: boolean): void {
    this.state.channels[idx].on.set(on);
    const p = this.linkedPartner(idx);
    if (p >= 0) this.state.channels[p].on.set(on);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/on`, 'i', [on ? 1 : 0]);
  }

  setChannelSolo(idx: number, solo: boolean): void {
    this.state.channels[idx].solo.set(solo);
    const p = this.linkedPartner(idx);
    if (p >= 0) this.state.channels[p].solo.set(solo);
    this.sendMsg(`/ch/${pad2(idx + 1)}/mix/solo`, 'i', [solo ? 1 : 0]);
  }

  // Toggle stereo link on a channel pair. The address only accepts an
  // odd channel (1, 3, 5) — the even side's link flag is implicit.
  setChannelLink(oddIdx: number, linked: boolean): void {
    this.state.channels[oddIdx].link.set(linked);
    // Propagate optimistically to the even partner so its UI
    // disabled-state updates immediately.
    const partner = this.state.channels[oddIdx + 1];
    if (partner && linked) {
      partner.fader.set(this.state.channels[oddIdx].fader.get());
      partner.on.set(this.state.channels[oddIdx].on.get());
      partner.solo.set(this.state.channels[oddIdx].solo.get());
    }
    this.sendMsg(`/ch/${pad2(oddIdx + 1)}/config/link`, 'i', [linked ? 1 : 0]);
  }

  setMainFaderL(v: number): void {
    this.state.main.faderL.set(v);
    if (this.state.main.link.get()) this.state.main.faderR.set(v);
    this.sendMsg('/main/st/mix/faderL', 'f', [v]);
  }

  setMainFaderR(v: number): void {
    this.state.main.faderR.set(v);
    if (this.state.main.link.get()) this.state.main.faderL.set(v);
    this.sendMsg('/main/st/mix/faderR', 'f', [v]);
  }

  setMainLink(linked: boolean): void {
    this.state.main.link.set(linked);
    if (linked) {
      // Snap R to L for visual consistency ahead of the echo.
      this.state.main.faderR.set(this.state.main.faderL.get());
    }
    this.sendMsg('/main/st/mix/link', 'i', [linked ? 1 : 0]);
  }

  setMainOn(on: boolean): void {
    this.state.main.on.set(on);
    this.sendMsg('/main/st/mix/on', 'i', [on ? 1 : 0]);
  }

  setMainHostvolEnable(enable: boolean): void {
    this.state.main.hostvolEnable.set(enable);
    this.sendMsg('/main/st/hostvol/enable', 'i', [enable ? 1 : 0]);
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

  // Spectrum subscription: same /sub verb idiom as meters, bound to
  // the single /spectrum/main blob address. Called on spectrum-tab
  // enter/leave so the firmware only computes and streams when a
  // viewer is actually looking at it.
  subscribeSpectrum(): void {
    this.sendMsg('/sub', 'sis', ['addSub', 1000, '/spectrum/main']);
  }

  unsubscribeSpectrum(): void {
    this.sendMsg('/sub', 'ss', ['unsubscribe', '/spectrum/main']);
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

    m = a.match(/^\/ch\/(\d+)\/config\/link$/);
    if (m && msg.types === 'i') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.link.set((msg.args[0] as number) !== 0);
      return;
    }

    if (a === '/main/st/mix/faderL' && msg.types === 'f') {
      this.state.main.faderL.set(msg.args[0] as number);
      return;
    }

    if (a === '/main/st/mix/faderR' && msg.types === 'f') {
      this.state.main.faderR.set(msg.args[0] as number);
      return;
    }

    if (a === '/main/st/mix/link' && msg.types === 'i') {
      this.state.main.link.set((msg.args[0] as number) !== 0);
      return;
    }

    if (a === '/main/st/mix/on' && msg.types === 'i') {
      this.state.main.on.set((msg.args[0] as number) !== 0);
      return;
    }

    if (a === '/main/st/hostvol/enable' && msg.types === 'i') {
      this.state.main.hostvolEnable.set((msg.args[0] as number) !== 0);
      return;
    }

    if (a === '/main/st/hostvol/value' && msg.types === 'f') {
      this.state.main.hostvolValue.set(msg.args[0] as number);
      return;
    }

    // USB capture-side host volume — read-only mirror of Windows'
    // recording-device slider, broadcast by the firmware on change.
    // Routes to the CAP HOST strip in the output dock. See the
    // teensy4 core patch on branch teensy4-usb-audio-capture-feature-unit
    // for the FU 0x30 descriptor that drives this.
    if (a === '/usb/cap/hostvol/value' && msg.types === 'f') {
      this.state.main.captureHostvolValue.set(msg.args[0] as number);
      return;
    }

    if (a === '/usb/cap/hostvol/mute' && msg.types === 'i') {
      this.state.main.captureHostvolMute.set((msg.args[0] as number) !== 0);
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

    // /meters/host b — 2 float32 pairs for the post-hostvol stereo
    // output (what the DAC actually receives after Windows volume).
    if (a === '/meters/host' && msg.types === 'b') {
      const blob = msg.args[0] as Uint8Array;
      if (blob.length >= 16) {
        const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
        this.state.main.hostPeakL.set(dv.getFloat32(0,  false));
        this.state.main.hostRmsL.set( dv.getFloat32(4,  false));
        this.state.main.hostPeakR.set(dv.getFloat32(8,  false));
        this.state.main.hostRmsR.set( dv.getFloat32(12, false));
      }
      return;
    }

    // /spectrum/main b — 1024-byte blob: 512 L bins then 512 R bins,
    // each as a uint8 dB unit (0 = -80 dB, 255 = 0 dB). Routed
    // directly to the spectrum view via the sink callback so the
    // canvas renderer gets the raw payload without a Signal round-
    // trip. The firmware only streams this when the spectrum tab
    // subscribes, so no work happens if nobody's watching.
    if (a === '/spectrum/main' && msg.types === 'b') {
      const blob = msg.args[0] as Uint8Array;
      if (this.spectrumSink && blob.length >= 1024) this.spectrumSink(blob);
      return;
    }
  }

  private sendMsg(address: string, types: string, args: OscArg[]): void {
    this.send(encodeMessage(address, types, args));
  }
}

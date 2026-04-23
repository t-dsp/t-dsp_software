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

// Listener for incoming codec-panel echoes (e.g. /codec/tac5212/out/1/mode).
// The codec-panel UI module registers one of these per control so the
// firmware's /snapshot reply lands back on the right <select>. The msg
// payload is forwarded raw — the listener decides how to interpret it.
export type CodecListener = (msg: OscMessage) => void;

export class Dispatcher {
  // Address -> listener for /codec/... echoes. Populated by codec-panel.ts
  // at construction time. The dispatcher matches by exact address; the
  // panel doesn't get pattern matching for free.
  private codecListeners = new Map<string, CodecListener>();

  // Callback sink for /spectrum/main b blobs. Set by the spectrum
  // view's owner so the 1024-byte payload can bypass MixerState and
  // land directly in the spectrum canvas renderer — putting 1 KB of
  // bins through a Signal every 33 ms would fire a lot of listeners
  // for no gain.
  private spectrumSink: ((bytes: Uint8Array) => void) | null = null;

  // Callback sink for /midi/note events from the firmware's USB host
  // MIDI bridge. Same pattern as spectrumSink — the keyboard view
  // sets this and toggles its key highlights directly. MIDI state is
  // ephemeral (no "what's the current note?" to mirror), so there's
  // no point routing it through a Signal.
  private midiSink: ((note: number, velocity: number, channel: number) => void) | null = null;

  constructor(
    private state: MixerState,
    private send: (packet: Uint8Array) => void,
  ) {}

  // Register a callback for a specific /codec/... echo address. The
  // codec-panel UI calls this once per enum/toggle control so the
  // firmware's /snapshot reply (or any later echo) updates the UI
  // signal automatically. Returns an unsubscribe function for symmetry,
  // though the panel currently never tears down.
  registerCodecListener(address: string, cb: CodecListener): () => void {
    this.codecListeners.set(address, cb);
    return () => {
      const cur = this.codecListeners.get(address);
      if (cur === cb) this.codecListeners.delete(address);
    };
  }

  setSpectrumSink(fn: ((bytes: Uint8Array) => void) | null): void {
    this.spectrumSink = fn;
  }

  setMidiSink(fn: ((note: number, velocity: number, channel: number) => void) | null): void {
    this.midiSink = fn;
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

  // Per-channel USB record send. Linked-partner propagation mirrors the
  // firmware model so the UI pair stays in sync before the echo arrives.
  // The Rec button is still clickable while main.loopEnable is on — the
  // firmware stores the state and applies it when loop is disengaged —
  // but the UI visually disables the button; enforcement lives there.
  setChannelRecSend(idx: number, on: boolean): void {
    this.state.channels[idx].recSend.set(on);
    const p = this.linkedPartner(idx);
    if (p >= 0) this.state.channels[p].recSend.set(on);
    this.sendMsg(`/ch/${pad2(idx + 1)}/rec/enable`, 'i', [on ? 1 : 0]);
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

  // Main loopback. When on, the post-fader / pre-hostvol main mix is
  // summed into USB capture and per-channel Rec sends are overridden
  // off in the firmware. UI subscribers to loopEnable handle the
  // disabled-styling of the per-channel Rec buttons.
  setMainLoop(enable: boolean): void {
    this.state.main.loopEnable.set(enable);
    this.sendMsg('/main/st/loop', 'i', [enable ? 1 : 0]);
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

  // Ask the firmware to dump all current state. The firmware replies
  // with a bundle of echoes for every field; handleIncoming() processes
  // them just like any other firmware-originated update. Call this on
  // connect so a client that joined mid-session catches up to the live
  // state instead of sitting on zero-initialized signals. The reply
  // also drives codec-panel population via the codecListeners map —
  // see Tac5212Panel::snapshot() on the firmware side.
  requestSnapshot(): void {
    this.sendMsg('/snapshot', '', []);
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

  // MIDI events from the firmware's USB host port (a keyboard plugged
  // into the Teensy's USB host). Interval is unused — the firmware
  // forwards each note-on / note-off as it arrives rather than sampling
  // on a timer — but we still send the 'sis' pattern so the wire format
  // matches meters/spectrum and main.cpp can peek at just the target.
  subscribeMidi(): void {
    this.sendMsg('/sub', 'sis', ['addSub', 0, '/midi/events']);
  }

  unsubscribeMidi(): void {
    this.sendMsg('/sub', 'ss', ['unsubscribe', '/midi/events']);
  }

  // ---------- Dexed synth controls ----------

  // Select a voice from the firmware's bundled DX7 banks. Two ints
  // (bank, voice). The firmware echoes back with the voice name as
  // the third arg, which handleIncoming routes into voiceName. We
  // optimistically update the signals so the dropdown doesn't snap
  // back during the round trip.
  setDexedVoice(bank: number, voice: number): void {
    this.state.dexed.bank.set(bank);
    this.state.dexed.voice.set(voice);
    this.sendMsg('/synth/dexed/voice', 'ii', [bank, voice]);
  }

  setDexedVolume(v: number): void {
    this.state.dexed.volume.set(v);
    this.sendMsg('/synth/dexed/volume', 'f', [v]);
  }

  setDexedMidiChannel(ch: number): void {
    this.state.dexed.midiChannel.set(ch);
    this.sendMsg('/synth/dexed/midi/ch', 'i', [ch]);
  }

  // Fetch the 32 voice names for a bank. Reply lands in handleIncoming
  // and populates state.dexed.voiceNames for the dropdown.
  queryDexedVoiceNames(bank: number): void {
    this.sendMsg('/synth/dexed/voice/names', 'i', [bank]);
  }

  // Fetch the 10 bank names. Called once on connect — bank list is
  // compile-time fixed so no need to re-query.
  queryDexedBankNames(): void {
    this.sendMsg('/synth/dexed/bank/names', '', []);
  }

  // Per-synth FX send into the shared bus. 0..1 linear.
  setDexedFxSend(v: number): void {
    this.state.dexed.fxSend.set(v);
    this.sendMsg('/synth/dexed/fx/send', 'f', [v]);
  }

  // ---------- Shared FX bus (chorus + reverb) ----------

  setFxChorusEnable(on: boolean): void {
    this.state.fx.chorusEnable.set(on);
    this.sendMsg('/fx/chorus/enable', 'i', [on ? 1 : 0]);
  }
  setFxChorusVoices(n: number): void {
    this.state.fx.chorusVoices.set(n);
    this.sendMsg('/fx/chorus/voices', 'i', [n]);
  }
  setFxReverbEnable(on: boolean): void {
    this.state.fx.reverbEnable.set(on);
    this.sendMsg('/fx/reverb/enable', 'i', [on ? 1 : 0]);
  }
  setFxReverbSize(v: number): void {
    this.state.fx.reverbSize.set(v);
    this.sendMsg('/fx/reverb/size', 'f', [v]);
  }
  setFxReverbDamping(v: number): void {
    this.state.fx.reverbDamping.set(v);
    this.sendMsg('/fx/reverb/damping', 'f', [v]);
  }
  setFxReverbReturn(v: number): void {
    this.state.fx.reverbReturn.set(v);
    this.sendMsg('/fx/reverb/return', 'f', [v]);
  }

  // ---------- Main-bus processing (Processing tab) ----------

  setProcShelfEnable(on: boolean): void {
    this.state.processing.shelfEnable.set(on);
    this.sendMsg('/proc/shelf/enable', 'i', [on ? 1 : 0]);
  }
  setProcShelfFreq(hz: number): void {
    this.state.processing.shelfFreqHz.set(hz);
    this.sendMsg('/proc/shelf/freq', 'f', [hz]);
  }
  setProcShelfGain(db: number): void {
    this.state.processing.shelfGainDb.set(db);
    this.sendMsg('/proc/shelf/gain', 'f', [db]);
  }
  setProcLimiterEnable(on: boolean): void {
    this.state.processing.limiterEnable.set(on);
    this.sendMsg('/proc/limiter/enable', 'i', [on ? 1 : 0]);
  }

  // UI-originated note from the on-screen keyboard in the Synth tab.
  // Firmware injects this into the same onMidiNoteOn/Off path as the
  // USB-host keyboard, so the echo comes back as /midi/note and the
  // piano viz lights up identically to a hardware key press. velocity
  // == 0 is the standard note-off sentinel. Default channel 1 is fine
  // for Phase 1 — once per-synth MIDI channel filtering lands, callers
  // can override to target a specific engine.
  sendMidiNote(note: number, velocity: number, channel: number = 1): void {
    this.sendMsg('/midi/note/in', 'iii', [note, velocity, channel]);
  }

  // Used by the raw OSC input field. Bypasses the typed setters above.
  sendRaw(address: string, types: string, args: OscArg[]): void {
    this.sendMsg(address, types, args);
  }

  // ---------- inbound (firmware -> UI) ----------

  handleIncoming(msg: OscMessage): void {
    const a = msg.address;

    // Codec-panel and registered-listener echoes route through the
    // listener registry first. Keyed by exact address (no patterns).
    // Also handles non-/codec/ addresses explicitly registered by UI
    // components (e.g. /line/mode).
    const codecCb = this.codecListeners.get(a);
    if (codecCb) {
      codecCb(msg);
      if (a.startsWith('/codec/')) return;  // codec-only: don't double-match
    }

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

    m = a.match(/^\/ch\/(\d+)\/rec\/enable$/);
    if (m && msg.types === 'i') {
      const idx = parseInt(m[1], 10) - 1;
      const ch = this.state.channels[idx];
      if (ch) ch.recSend.set((msg.args[0] as number) !== 0);
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

    if (a === '/main/st/loop' && msg.types === 'i') {
      this.state.main.loopEnable.set((msg.args[0] as number) !== 0);
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

    // /midi/note i i i — note, velocity, channel. Velocity 0 means
    // note-off (standard MIDI running-status idiom). Forwarded to the
    // Keyboard tab's sink callback; no Signal mirror needed since the
    // visualization is purely driven by the event stream.
    if (a === '/midi/note' && msg.types === 'iii') {
      const note     = msg.args[0] as number;
      const velocity = msg.args[1] as number;
      const channel  = msg.args[2] as number;
      if (this.midiSink) this.midiSink(note, velocity, channel);
      return;
    }

    // Dexed synth echoes.
    if (a === '/synth/dexed/voice' && msg.types === 'iis') {
      this.state.dexed.bank.set(msg.args[0] as number);
      this.state.dexed.voice.set(msg.args[1] as number);
      this.state.dexed.voiceName.set(msg.args[2] as string);
      return;
    }
    if (a === '/synth/dexed/volume' && msg.types === 'f') {
      this.state.dexed.volume.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/dexed/midi/ch' && msg.types === 'i') {
      this.state.dexed.midiChannel.set(msg.args[0] as number);
      return;
    }
    // /synth/dexed/voice/names — first arg is bank index, remaining 32
    // are voice names. We don't use the bank index (the firmware only
    // sends in response to a query, so we trust the reply matches the
    // current bank selection).
    if (a === '/synth/dexed/voice/names' && msg.args.length >= 33 && msg.types.startsWith('i')) {
      const names = msg.args.slice(1).map((x) => String(x));
      this.state.dexed.voiceNames.set(names);
      return;
    }
    if (a === '/synth/dexed/bank/names' && msg.args.length >= 1) {
      this.state.dexed.bankNames.set(msg.args.map((x) => String(x)));
      return;
    }

    // Main-bus processing echoes.
    if (a === '/proc/shelf/enable' && msg.types === 'i') {
      this.state.processing.shelfEnable.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/proc/shelf/freq' && msg.types === 'f') {
      this.state.processing.shelfFreqHz.set(msg.args[0] as number);
      return;
    }
    if (a === '/proc/shelf/gain' && msg.types === 'f') {
      this.state.processing.shelfGainDb.set(msg.args[0] as number);
      return;
    }
    if (a === '/proc/limiter/enable' && msg.types === 'i') {
      this.state.processing.limiterEnable.set((msg.args[0] as number) !== 0);
      return;
    }

    // Shared FX bus + per-synth send echoes.
    if (a === '/synth/dexed/fx/send' && msg.types === 'f') {
      this.state.dexed.fxSend.set(msg.args[0] as number);
      return;
    }
    if (a === '/fx/chorus/enable' && msg.types === 'i') {
      this.state.fx.chorusEnable.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/fx/chorus/voices' && msg.types === 'i') {
      this.state.fx.chorusVoices.set(msg.args[0] as number);
      return;
    }
    if (a === '/fx/reverb/enable' && msg.types === 'i') {
      this.state.fx.reverbEnable.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/fx/reverb/size' && msg.types === 'f') {
      this.state.fx.reverbSize.set(msg.args[0] as number);
      return;
    }
    if (a === '/fx/reverb/damping' && msg.types === 'f') {
      this.state.fx.reverbDamping.set(msg.args[0] as number);
      return;
    }
    if (a === '/fx/reverb/return' && msg.types === 'f') {
      this.state.fx.reverbReturn.set(msg.args[0] as number);
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

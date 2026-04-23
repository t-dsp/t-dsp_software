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
import { MixerState, BEATS_TRACK_COUNT, BEATS_STEP_COUNT } from './state';

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

  // Per-address throttle state for sendThrottled(). Continuous-value
  // controls (sliders) can fire hundreds of input events per second
  // during a drag; the firmware's CDC transport throttles incoming
  // OSC frames to one per 3 ms, so unthrottled bursts queue up and
  // make the UI feel "slow" — every drag position takes a real bridge
  // round-trip to settle. Coalescing client-side to ~30 Hz means the
  // firmware sees a smooth update stream without queue buildup.
  private throttle = new Map<string, {
    lastSentAt: number;
    pendingTimer: number | null;
    pendingTypes: string;
    pendingArgs: OscArg[] | null;
  }>();
  private static readonly THROTTLE_MS = 33;  // ~30 Hz — matches LFO/meter cadence

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
    // The firmware's snapshot emits /beats/step only for ON cells — so
    // any cells that are ON locally but OFF in the firmware's pattern
    // would remain stuck ON after the snapshot lands. Zero the grid
    // client-side first so the snapshot's sparse "on" messages paint
    // the authoritative state onto a blank canvas.
    for (const track of this.state.beats.tracks) {
      for (const s of track.stepsOn) s.set(false);
    }
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
    this.sendThrottled('/synth/dexed/volume', 'f', [v]);
  }

  setDexedOn(on: boolean): void {
    this.state.dexed.on.set(on);
    this.sendMsg('/synth/dexed/on', 'i', [on ? 1 : 0]);
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
    this.sendThrottled('/synth/dexed/fx/send', 'f', [v]);
  }

  // ---------- MPE VA synth controls ----------

  setMpeVolume(v: number): void {
    this.state.mpe.volume.set(v);
    this.sendThrottled('/synth/mpe/volume', 'f', [v]);
  }

  setMpeOn(on: boolean): void {
    this.state.mpe.on.set(on);
    this.sendMsg('/synth/mpe/on', 'i', [on ? 1 : 0]);
  }

  setMpeAttack(s: number): void {
    this.state.mpe.attack.set(s);
    this.sendThrottled('/synth/mpe/attack', 'f', [s]);
  }

  setMpeRelease(s: number): void {
    this.state.mpe.release.set(s);
    this.sendThrottled('/synth/mpe/release', 'f', [s]);
  }

  setMpeWaveform(w: number): void {
    this.state.mpe.waveform.set(w);
    this.sendMsg('/synth/mpe/waveform', 'i', [w]);
  }

  setMpeFilterCutoff(hz: number): void {
    this.state.mpe.filterCutoffHz.set(hz);
    this.sendThrottled('/synth/mpe/filter/cutoff', 'f', [hz]);
  }

  setMpeFilterResonance(q: number): void {
    this.state.mpe.filterResonance.set(q);
    this.sendThrottled('/synth/mpe/filter/resonance', 'f', [q]);
  }

  setMpeLfoRate(hz: number): void {
    this.state.mpe.lfoRate.set(hz);
    this.sendThrottled('/synth/mpe/lfo/rate', 'f', [hz]);
  }

  setMpeLfoDepth(d: number): void {
    this.state.mpe.lfoDepth.set(d);
    this.sendThrottled('/synth/mpe/lfo/depth', 'f', [d]);
  }

  setMpeLfoDest(dest: number): void {
    this.state.mpe.lfoDest.set(dest);
    this.sendMsg('/synth/mpe/lfo/dest', 'i', [dest]);
  }

  setMpeLfoWaveform(w: number): void {
    this.state.mpe.lfoWaveform.set(w);
    this.sendMsg('/synth/mpe/lfo/waveform', 'i', [w]);
  }

  setMpeMasterChannel(ch: number): void {
    this.state.mpe.masterChannel.set(ch);
    this.sendMsg('/synth/mpe/midi/master', 'i', [ch]);
  }

  setMpeFxSend(v: number): void {
    this.state.mpe.fxSend.set(v);
    this.sendThrottled('/synth/mpe/fx/send', 'f', [v]);
  }

  // Subscribe / unsubscribe the voice-telemetry broadcast. The
  // panel opens the firehose on tab-enter and closes it on leave
  // so unwatched tabs don't waste CDC bandwidth.
  subscribeMpeVoices(): void {
    this.sendMsg('/sub', 'sis', ['addSub', 1000, '/synth/mpe/voices']);
  }

  unsubscribeMpeVoices(): void {
    this.sendMsg('/sub', 'ss', ['unsubscribe', '/synth/mpe/voices']);
    // Reset every voice to "released" so stale orbs don't keep
    // glowing after we stop receiving telemetry.
    for (const v of this.state.mpe.voices) {
      v.held.set(false);
      v.pressure.set(0);
    }
  }

  // ---------- Neuro (reese bass) synth controls ----------

  setNeuroVolume(v: number): void {
    this.state.neuro.volume.set(v);
    this.sendThrottled('/synth/neuro/volume', 'f', [v]);
  }

  setNeuroOn(on: boolean): void {
    this.state.neuro.on.set(on);
    this.sendMsg('/synth/neuro/on', 'i', [on ? 1 : 0]);
  }

  setNeuroMidiChannel(ch: number): void {
    this.state.neuro.midiChannel.set(ch);
    this.sendMsg('/synth/neuro/midi/ch', 'i', [ch]);
  }

  setNeuroAttack(s: number): void {
    this.state.neuro.attack.set(s);
    this.sendThrottled('/synth/neuro/attack', 'f', [s]);
  }

  setNeuroRelease(s: number): void {
    this.state.neuro.release.set(s);
    this.sendThrottled('/synth/neuro/release', 'f', [s]);
  }

  setNeuroDetune(cents: number): void {
    this.state.neuro.detuneCents.set(cents);
    this.sendThrottled('/synth/neuro/detune', 'f', [cents]);
  }

  setNeuroSub(level: number): void {
    this.state.neuro.subLevel.set(level);
    this.sendThrottled('/synth/neuro/sub', 'f', [level]);
  }

  setNeuroOsc3(level: number): void {
    this.state.neuro.osc3Level.set(level);
    this.sendThrottled('/synth/neuro/osc3', 'f', [level]);
  }

  setNeuroFilterCutoff(hz: number): void {
    this.state.neuro.filterCutoffHz.set(hz);
    this.sendThrottled('/synth/neuro/filter/cutoff', 'f', [hz]);
  }

  setNeuroFilterResonance(q: number): void {
    this.state.neuro.filterResonance.set(q);
    this.sendThrottled('/synth/neuro/filter/resonance', 'f', [q]);
  }

  setNeuroLfoRate(hz: number): void {
    this.state.neuro.lfoRate.set(hz);
    this.sendThrottled('/synth/neuro/lfo/rate', 'f', [hz]);
  }

  setNeuroLfoDepth(d: number): void {
    this.state.neuro.lfoDepth.set(d);
    this.sendThrottled('/synth/neuro/lfo/depth', 'f', [d]);
  }

  setNeuroLfoDest(dest: number): void {
    this.state.neuro.lfoDest.set(dest);
    this.sendMsg('/synth/neuro/lfo/dest', 'i', [dest]);
  }

  setNeuroLfoWaveform(w: number): void {
    this.state.neuro.lfoWaveform.set(w);
    this.sendMsg('/synth/neuro/lfo/waveform', 'i', [w]);
  }

  setNeuroPortamento(ms: number): void {
    this.state.neuro.portamentoMs.set(ms);
    this.sendThrottled('/synth/neuro/portamento', 'f', [ms]);
  }

  setNeuroFxSend(v: number): void {
    this.state.neuro.fxSend.set(v);
    this.sendThrottled('/synth/neuro/fx/send', 'f', [v]);
  }

  // ---------- Neuro stink chain (Phase 2f) ----------

  setNeuroStinkEnable(on: boolean): void {
    this.state.neuro.stinkEnable.set(on);
    this.sendMsg('/synth/neuro/stink/enable', 'i', [on ? 1 : 0]);
  }
  setNeuroStinkDriveLow(v: number): void {
    this.state.neuro.stinkDriveLow.set(v);
    this.sendThrottled('/synth/neuro/stink/drive_low', 'f', [v]);
  }
  setNeuroStinkDriveMid(v: number): void {
    this.state.neuro.stinkDriveMid.set(v);
    this.sendThrottled('/synth/neuro/stink/drive_mid', 'f', [v]);
  }
  setNeuroStinkDriveHigh(v: number): void {
    this.state.neuro.stinkDriveHigh.set(v);
    this.sendThrottled('/synth/neuro/stink/drive_high', 'f', [v]);
  }
  setNeuroStinkMixLow(v: number): void {
    this.state.neuro.stinkMixLow.set(v);
    this.sendThrottled('/synth/neuro/stink/mix_low', 'f', [v]);
  }
  setNeuroStinkMixMid(v: number): void {
    this.state.neuro.stinkMixMid.set(v);
    this.sendThrottled('/synth/neuro/stink/mix_mid', 'f', [v]);
  }
  setNeuroStinkMixHigh(v: number): void {
    this.state.neuro.stinkMixHigh.set(v);
    this.sendThrottled('/synth/neuro/stink/mix_high', 'f', [v]);
  }
  setNeuroStinkFold(v: number): void {
    this.state.neuro.stinkFold.set(v);
    this.sendThrottled('/synth/neuro/stink/fold', 'f', [v]);
  }
  setNeuroStinkCrush(v: number): void {
    this.state.neuro.stinkCrush.set(v);
    this.sendThrottled('/synth/neuro/stink/crush', 'f', [v]);
  }
  setNeuroStinkMasterCutoff(hz: number): void {
    this.state.neuro.stinkMasterCutoffHz.set(hz);
    this.sendThrottled('/synth/neuro/stink/master_cutoff', 'f', [hz]);
  }
  setNeuroStinkMasterResonance(q: number): void {
    this.state.neuro.stinkMasterResonance.set(q);
    this.sendThrottled('/synth/neuro/stink/master_resonance', 'f', [q]);
  }
  setNeuroStinkLfo2Rate(hz: number): void {
    this.state.neuro.stinkLfo2Rate.set(hz);
    this.sendThrottled('/synth/neuro/stink/lfo2/rate', 'f', [hz]);
  }
  setNeuroStinkLfo2Depth(d: number): void {
    this.state.neuro.stinkLfo2Depth.set(d);
    this.sendThrottled('/synth/neuro/stink/lfo2/depth', 'f', [d]);
  }
  setNeuroStinkLfo2Dest(dest: number): void {
    this.state.neuro.stinkLfo2Dest.set(dest);
    this.sendMsg('/synth/neuro/stink/lfo2/dest', 'i', [dest]);
  }
  setNeuroStinkLfo2Waveform(w: number): void {
    this.state.neuro.stinkLfo2Waveform.set(w);
    this.sendMsg('/synth/neuro/stink/lfo2/waveform', 'i', [w]);
  }

  // ---------- Acid (TB-303) synth controls ----------

  setAcidVolume(v: number): void {
    this.state.acid.volume.set(v);
    this.sendThrottled('/synth/acid/volume', 'f', [v]);
  }
  setAcidOn(on: boolean): void {
    this.state.acid.on.set(on);
    this.sendMsg('/synth/acid/on', 'i', [on ? 1 : 0]);
  }
  setAcidMidiChannel(ch: number): void {
    this.state.acid.midiChannel.set(ch);
    this.sendMsg('/synth/acid/midi/ch', 'i', [ch]);
  }
  setAcidWaveform(w: number): void {
    this.state.acid.waveform.set(w);
    this.sendMsg('/synth/acid/waveform', 'i', [w]);
  }
  setAcidTuning(semi: number): void {
    this.state.acid.tuning.set(semi);
    this.sendMsg('/synth/acid/tuning', 'i', [semi]);
  }
  setAcidCutoff(hz: number): void {
    this.state.acid.cutoffHz.set(hz);
    this.sendThrottled('/synth/acid/cutoff', 'f', [hz]);
  }
  setAcidResonance(q: number): void {
    this.state.acid.resonance.set(q);
    this.sendThrottled('/synth/acid/resonance', 'f', [q]);
  }
  setAcidEnvMod(v: number): void {
    this.state.acid.envMod.set(v);
    this.sendThrottled('/synth/acid/env_mod', 'f', [v]);
  }
  setAcidEnvDecay(s: number): void {
    this.state.acid.envDecay.set(s);
    this.sendThrottled('/synth/acid/env_decay', 'f', [s]);
  }
  setAcidAmpDecay(s: number): void {
    this.state.acid.ampDecay.set(s);
    this.sendThrottled('/synth/acid/amp_decay', 'f', [s]);
  }
  setAcidAccent(v: number): void {
    this.state.acid.accent.set(v);
    this.sendThrottled('/synth/acid/accent', 'f', [v]);
  }
  setAcidSlide(ms: number): void {
    this.state.acid.slideMs.set(ms);
    this.sendThrottled('/synth/acid/slide', 'f', [ms]);
  }

  // ---------- Supersaw (JP-8000 style) controls ----------

  setSupersawVolume(v: number): void {
    this.state.supersaw.volume.set(v);
    this.sendThrottled('/synth/supersaw/volume', 'f', [v]);
  }
  setSupersawOn(on: boolean): void {
    this.state.supersaw.on.set(on);
    this.sendMsg('/synth/supersaw/on', 'i', [on ? 1 : 0]);
  }
  setSupersawMidiChannel(ch: number): void {
    this.state.supersaw.midiChannel.set(ch);
    this.sendMsg('/synth/supersaw/midi/ch', 'i', [ch]);
  }
  setSupersawDetune(cents: number): void {
    this.state.supersaw.detuneCents.set(cents);
    this.sendThrottled('/synth/supersaw/detune', 'f', [cents]);
  }
  setSupersawMixCenter(v: number): void {
    this.state.supersaw.mixCenter.set(v);
    this.sendThrottled('/synth/supersaw/mix_center', 'f', [v]);
  }
  setSupersawCutoff(hz: number): void {
    this.state.supersaw.cutoffHz.set(hz);
    this.sendThrottled('/synth/supersaw/cutoff', 'f', [hz]);
  }
  setSupersawResonance(q: number): void {
    this.state.supersaw.resonance.set(q);
    this.sendThrottled('/synth/supersaw/resonance', 'f', [q]);
  }
  setSupersawAttack(s: number): void {
    this.state.supersaw.attack.set(s);
    this.sendThrottled('/synth/supersaw/attack', 'f', [s]);
  }
  setSupersawDecay(s: number): void {
    this.state.supersaw.decay.set(s);
    this.sendThrottled('/synth/supersaw/decay', 'f', [s]);
  }
  setSupersawSustain(v: number): void {
    this.state.supersaw.sustain.set(v);
    this.sendThrottled('/synth/supersaw/sustain', 'f', [v]);
  }
  setSupersawRelease(s: number): void {
    this.state.supersaw.release.set(s);
    this.sendThrottled('/synth/supersaw/release', 'f', [s]);
  }
  setSupersawChorusDepth(v: number): void {
    this.state.supersaw.chorusDepth.set(v);
    this.sendThrottled('/synth/supersaw/chorus_depth', 'f', [v]);
  }
  setSupersawPortamento(ms: number): void {
    this.state.supersaw.portamentoMs.set(ms);
    this.sendThrottled('/synth/supersaw/portamento', 'f', [ms]);
  }

  // ---------- Chip (NES/Gameboy) controls ----------

  setChipVolume(v: number): void {
    this.state.chip.volume.set(v);
    this.sendThrottled('/synth/chip/volume', 'f', [v]);
  }
  setChipOn(on: boolean): void {
    this.state.chip.on.set(on);
    this.sendMsg('/synth/chip/on', 'i', [on ? 1 : 0]);
  }
  setChipMidiChannel(ch: number): void {
    this.state.chip.midiChannel.set(ch);
    this.sendMsg('/synth/chip/midi/ch', 'i', [ch]);
  }
  setChipPulse1Duty(d: number): void {
    this.state.chip.pulse1Duty.set(d);
    this.sendMsg('/synth/chip/pulse1_duty', 'i', [d]);
  }
  setChipPulse2Duty(d: number): void {
    this.state.chip.pulse2Duty.set(d);
    this.sendMsg('/synth/chip/pulse2_duty', 'i', [d]);
  }
  setChipPulse2Detune(cents: number): void {
    this.state.chip.pulse2Detune.set(cents);
    this.sendThrottled('/synth/chip/pulse2_detune', 'f', [cents]);
  }
  setChipTriLevel(v: number): void {
    this.state.chip.triLevel.set(v);
    this.sendThrottled('/synth/chip/tri_level', 'f', [v]);
  }
  setChipNoiseLevel(v: number): void {
    this.state.chip.noiseLevel.set(v);
    this.sendThrottled('/synth/chip/noise_level', 'f', [v]);
  }
  setChipVoicing(v: number): void {
    this.state.chip.voicing.set(v);
    this.sendMsg('/synth/chip/voicing', 'i', [v]);
  }
  setChipArpeggio(v: number): void {
    this.state.chip.arpeggio.set(v);
    this.sendMsg('/synth/chip/arpeggio', 'i', [v]);
  }
  setChipArpRate(hz: number): void {
    this.state.chip.arpRateHz.set(hz);
    this.sendThrottled('/synth/chip/arp_rate', 'f', [hz]);
  }
  setChipAttack(s: number): void {
    this.state.chip.attack.set(s);
    this.sendThrottled('/synth/chip/attack', 'f', [s]);
  }
  setChipDecay(s: number): void {
    this.state.chip.decay.set(s);
    this.sendThrottled('/synth/chip/decay', 'f', [s]);
  }
  setChipSustain(v: number): void {
    this.state.chip.sustain.set(v);
    this.sendThrottled('/synth/chip/sustain', 'f', [v]);
  }
  setChipRelease(s: number): void {
    this.state.chip.release.set(s);
    this.sendThrottled('/synth/chip/release', 'f', [s]);
  }

  // ---------- Arpeggiator (TDspArp) ----------
  //
  // Outbound setters mirror the /arp/* OSC address scheme. Continuous
  // values (gate, swing) throttle like any other slider; discrete
  // ints / bools fire immediately so preset loads apply together. The
  // firmware echoes every write; incoming handlers below re-set the
  // state signal (idempotent if local optimistic set already matched).

  setArpOn(on: boolean): void {
    this.state.arp.on.set(on);
    this.sendMsg('/arp/on', 'i', [on ? 1 : 0]);
  }
  setArpPattern(p: number): void {
    this.state.arp.pattern.set(p);
    this.sendMsg('/arp/pattern', 'i', [p]);
  }
  setArpRate(r: number): void {
    this.state.arp.rate.set(r);
    this.sendMsg('/arp/rate', 'i', [r]);
  }
  setArpGate(v: number): void {
    this.state.arp.gate.set(v);
    this.sendThrottled('/arp/gate', 'f', [v]);
  }
  setArpSwing(v: number): void {
    this.state.arp.swing.set(v);
    this.sendThrottled('/arp/swing', 'f', [v]);
  }
  setArpOctaveRange(n: number): void {
    this.state.arp.octaveRange.set(n);
    this.sendMsg('/arp/octaveRange', 'i', [n]);
  }
  setArpOctaveMode(m: number): void {
    this.state.arp.octaveMode.set(m);
    this.sendMsg('/arp/octaveMode', 'i', [m]);
  }
  setArpLatch(on: boolean): void {
    this.state.arp.latch.set(on);
    this.sendMsg('/arp/latch', 'i', [on ? 1 : 0]);
  }
  setArpHold(on: boolean): void {
    this.state.arp.hold.set(on);
    this.sendMsg('/arp/hold', 'i', [on ? 1 : 0]);
  }
  setArpVelMode(m: number): void {
    this.state.arp.velMode.set(m);
    this.sendMsg('/arp/velMode', 'i', [m]);
  }
  setArpVelFixed(v: number): void {
    this.state.arp.velFixed.set(v);
    this.sendMsg('/arp/velFixed', 'i', [v]);
  }
  setArpVelAccent(v: number): void {
    this.state.arp.velAccent.set(v);
    this.sendMsg('/arp/velAccent', 'i', [v]);
  }
  // stepMask is a 32-bit pattern; we send as signed int (OSC i is 32-bit)
  // and reinterpret in firmware. Bit N = step N enabled.
  setArpStepMask(mask: number): void {
    this.state.arp.stepMask.set(mask | 0);   // force int32
    this.sendMsg('/arp/stepMask', 'i', [mask | 0]);
  }
  setArpStepLength(n: number): void {
    this.state.arp.stepLength.set(n);
    this.sendMsg('/arp/stepLength', 'i', [n]);
  }
  setArpMpeMode(m: number): void {
    this.state.arp.mpeMode.set(m);
    this.sendMsg('/arp/mpeMode', 'i', [m]);
  }
  setArpOutputChannel(ch: number): void {
    this.state.arp.outputChannel.set(ch);
    this.sendMsg('/arp/outputChannel', 'i', [ch]);
  }
  setArpScatterBase(ch: number): void {
    this.state.arp.scatterBase.set(ch);
    this.sendMsg('/arp/scatterBase', 'i', [ch]);
  }
  setArpScatterCount(n: number): void {
    this.state.arp.scatterCount.set(n);
    this.sendMsg('/arp/scatterCount', 'i', [n]);
  }
  setArpScale(s: number): void {
    this.state.arp.scale.set(s);
    this.sendMsg('/arp/scale', 'i', [s]);
  }
  setArpScaleRoot(r: number): void {
    this.state.arp.scaleRoot.set(r);
    this.sendMsg('/arp/scaleRoot', 'i', [r]);
  }
  setArpTranspose(s: number): void {
    this.state.arp.transpose.set(s);
    this.sendMsg('/arp/transpose', 'i', [s]);
  }
  setArpRepeat(n: number): void {
    this.state.arp.repeat.set(n);
    this.sendMsg('/arp/repeat', 'i', [n]);
  }
  arpPanic(): void {
    this.sendMsg('/arp/panic', '', []);
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
    this.sendThrottled('/fx/reverb/size', 'f', [v]);
  }
  setFxReverbDamping(v: number): void {
    this.state.fx.reverbDamping.set(v);
    this.sendThrottled('/fx/reverb/damping', 'f', [v]);
  }
  setFxReverbReturn(v: number): void {
    this.state.fx.reverbReturn.set(v);
    this.sendThrottled('/fx/reverb/return', 'f', [v]);
  }

  // ---------- Main-bus processing (Processing tab) ----------

  setProcShelfEnable(on: boolean): void {
    this.state.processing.shelfEnable.set(on);
    this.sendMsg('/proc/shelf/enable', 'i', [on ? 1 : 0]);
  }
  setProcShelfFreq(hz: number): void {
    this.state.processing.shelfFreqHz.set(hz);
    this.sendThrottled('/proc/shelf/freq', 'f', [hz]);
  }
  setProcShelfGain(db: number): void {
    this.state.processing.shelfGainDb.set(db);
    this.sendThrottled('/proc/shelf/gain', 'f', [db]);
  }
  setProcLimiterEnable(on: boolean): void {
    this.state.processing.limiterEnable.set(on);
    this.sendMsg('/proc/limiter/enable', 'i', [on ? 1 : 0]);
  }

  // ---------- Synth bus (group fader + mute) ----------
  //
  // Sits downstream of every per-synth volume and upstream of preMix
  // slot 1 on the firmware side. Also taps the looper source mux, so
  // the "Synth" looper source captures whatever this fader lets through.

  setSynthBusVolume(v: number): void {
    this.state.synthBus.volume.set(v);
    this.sendThrottled('/synth/bus/volume', 'f', [v]);
  }

  setSynthBusOn(on: boolean): void {
    this.state.synthBus.on.set(on);
    this.sendMsg('/synth/bus/on', 'i', [on ? 1 : 0]);
  }

  // ---------- Looper ----------
  //
  // Transport actions (/loop/record, /loop/play, /loop/stop, /loop/clear)
  // take no args; the firmware echoes /loop/state and /loop/length after
  // each one so the UI doesn't need to guess state transitions. source
  // and level follow the standard "optimistic mirror + OSC write + echo"
  // pattern used elsewhere.

  setLooperSource(n: number): void {
    this.state.looper.source.set(n);
    this.sendMsg('/loop/source', 'i', [n]);
  }

  setLooperLevel(v: number): void {
    this.state.looper.level.set(v);
    this.sendThrottled('/loop/level', 'f', [v]);
  }

  looperRecord(): void { this.sendMsg('/loop/record', '', []); }
  looperPlay():   void { this.sendMsg('/loop/play',   '', []); }
  looperStop():   void { this.sendMsg('/loop/stop',   '', []); }
  looperClear():  void { this.sendMsg('/loop/clear',  '', []); }

  setLooperQuantize(on: boolean): void {
    this.state.looper.quantize.set(on);
    this.sendMsg('/loop/quantize', 'i', [on ? 1 : 0]);
  }

  // ---------- Clock (shared musical time) ----------
  //
  // The device runs one clock. External mode slaves to incoming MIDI
  // 0xF8 / 0xFA / 0xFB / 0xFC; Internal mode is free-running at a
  // user-set BPM. Writing /clock/bpm only takes effect when Source is
  // Internal — External BPM is measured from ticks, not set.

  setClockSource(s: 'ext' | 'int'): void {
    this.state.clock.source.set(s);
    this.sendMsg('/clock/source', 's', [s]);
  }
  setClockBpm(v: number): void {
    this.state.clock.bpm.set(v);
    this.sendThrottled('/clock/bpm', 'f', [v]);
  }
  setClockBeatsPerBar(n: number): void {
    this.state.clock.beatsPerBar.set(n);
    this.sendMsg('/clock/beatsPerBar', 'i', [n]);
  }
  // Read-only probe — fire to request a /clock/running echo. Useful
  // when the Clock tab first opens so the UI doesn't show stale data.
  queryClockRunning(): void {
    this.sendMsg('/clock/running', '', []);
  }

  // ---------- Beats drum machine ----------

  setBeatsRun(on: boolean): void {
    this.state.beats.running.set(on);
    this.sendMsg('/beats/run', 'i', [on ? 1 : 0]);
  }
  setBeatsBpm(v: number): void {
    this.state.beats.bpm.set(v);
    this.sendMsg('/beats/bpm', 'f', [v]);
  }
  setBeatsSwing(v: number): void {
    this.state.beats.swing.set(v);
    this.sendMsg('/beats/swing', 'f', [v]);
  }
  setBeatsVolume(v: number): void {
    this.state.beats.volume.set(v);
    this.sendMsg('/beats/volume', 'f', [v]);
  }
  setBeatsTrackMute(track: number, muted: boolean): void {
    const t = this.state.beats.tracks[track];
    if (t) t.muted.set(muted);
    this.sendMsg('/beats/mute', 'ii', [track, muted ? 1 : 0]);
  }
  // Toggle or set a single step. Optimistic update on the per-step signal
  // so the UI flips instantly; firmware echo re-applies (idempotent).
  setBeatsStep(track: number, step: number, on: boolean): void {
    const t = this.state.beats.tracks[track];
    if (t && t.stepsOn[step]) t.stepsOn[step].set(on);
    this.sendMsg('/beats/step', 'iii', [track, step, on ? 1 : 0]);
  }
  setBeatsStepVel(track: number, step: number, vel: number): void {
    const t = this.state.beats.tracks[track];
    if (t && t.stepsVel[step]) t.stepsVel[step].set(vel);
    this.sendMsg('/beats/vel', 'iif', [track, step, vel]);
  }
  clearBeatsTrack(track: number): void {
    // -1 clears all tracks; 0..3 clears just that row. Optimistic update
    // so the grid wipes immediately.
    const clearOne = (i: number): void => {
      const t = this.state.beats.tracks[i];
      if (!t) return;
      for (const s of t.stepsOn) s.set(false);
    };
    if (track < 0) {
      for (let i = 0; i < BEATS_TRACK_COUNT; i++) clearOne(i);
    } else {
      clearOne(track);
    }
    this.sendMsg('/beats/clear', 'i', [track]);
  }
  setBeatsSample(track: number, filename: string): void {
    const t = this.state.beats.tracks[track];
    if (t) t.sample.set(filename);
    this.sendMsg('/beats/sample', 'is', [track, filename]);
  }
  setBeatsClockSource(src: 'internal' | 'external'): void {
    this.state.beats.clockSource.set(src);
    this.sendMsg('/beats/clockSource', 's', [src]);
  }

  // Apply a full preset: BPM, swing, and the entire 4×16 pattern grid.
  // Clears all tracks first (one /beats/clear -1) then emits /beats/step
  // only for on-cells, so a sparse pattern generates minimal traffic.
  // Firmware's 3 ms frame throttle paces the burst naturally.
  applyBeatsPreset(preset: {
    bpm: number;
    swing: number;
    pattern: boolean[][];
  }): void {
    this.setBeatsBpm(preset.bpm);
    this.setBeatsSwing(preset.swing);
    this.clearBeatsTrack(-1);
    for (let t = 0; t < preset.pattern.length; t++) {
      const row = preset.pattern[t];
      if (!row) continue;
      for (let s = 0; s < row.length; s++) {
        if (row[s]) this.setBeatsStep(t, s, true);
      }
    }
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
    if (a === '/synth/dexed/on' && msg.types === 'i') {
      this.state.dexed.on.set((msg.args[0] as number) !== 0);
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

    // MPE VA synth echoes — 12 simple scalar params + one voice
    // telemetry array.
    if (a === '/synth/mpe/volume' && msg.types === 'f') {
      this.state.mpe.volume.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/on' && msg.types === 'i') {
      this.state.mpe.on.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/synth/mpe/attack' && msg.types === 'f') {
      this.state.mpe.attack.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/release' && msg.types === 'f') {
      this.state.mpe.release.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/waveform' && msg.types === 'i') {
      this.state.mpe.waveform.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/filter/cutoff' && msg.types === 'f') {
      this.state.mpe.filterCutoffHz.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/filter/resonance' && msg.types === 'f') {
      this.state.mpe.filterResonance.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/lfo/rate' && msg.types === 'f') {
      this.state.mpe.lfoRate.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/lfo/depth' && msg.types === 'f') {
      this.state.mpe.lfoDepth.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/lfo/dest' && msg.types === 'i') {
      this.state.mpe.lfoDest.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/lfo/waveform' && msg.types === 'i') {
      this.state.mpe.lfoWaveform.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/midi/master' && msg.types === 'i') {
      this.state.mpe.masterChannel.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/mpe/fx/send' && msg.types === 'f') {
      this.state.mpe.fxSend.set(msg.args[0] as number);
      return;
    }
    // /synth/mpe/voices i i i f f f × kVoiceCount — 6 args per voice.
    // Firmware sends 4 voices = 24 args. Each group: held, channel,
    // note, pitchSemi, pressure, timbre.
    if (a === '/synth/mpe/voices') {
      const perVoice = 6;
      const n = Math.min(this.state.mpe.voices.length,
                         Math.floor(msg.args.length / perVoice));
      for (let i = 0; i < n; i++) {
        const base = i * perVoice;
        const v = this.state.mpe.voices[i];
        v.held.set     ((msg.args[base + 0] as number) !== 0);
        v.channel.set  (msg.args[base + 1] as number);
        v.note.set     (msg.args[base + 2] as number);
        v.pitchSemi.set(msg.args[base + 3] as number);
        v.pressure.set (msg.args[base + 4] as number);
        v.timbre.set   (msg.args[base + 5] as number);
      }
      return;
    }

    // Neuro (reese bass) echoes — same scalar-param layout as MPE.
    if (a === '/synth/neuro/volume' && msg.types === 'f') {
      this.state.neuro.volume.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/on' && msg.types === 'i') {
      this.state.neuro.on.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/synth/neuro/midi/ch' && msg.types === 'i') {
      this.state.neuro.midiChannel.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/attack' && msg.types === 'f') {
      this.state.neuro.attack.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/release' && msg.types === 'f') {
      this.state.neuro.release.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/detune' && msg.types === 'f') {
      this.state.neuro.detuneCents.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/sub' && msg.types === 'f') {
      this.state.neuro.subLevel.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/osc3' && msg.types === 'f') {
      this.state.neuro.osc3Level.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/filter/cutoff' && msg.types === 'f') {
      this.state.neuro.filterCutoffHz.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/filter/resonance' && msg.types === 'f') {
      this.state.neuro.filterResonance.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/lfo/rate' && msg.types === 'f') {
      this.state.neuro.lfoRate.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/lfo/depth' && msg.types === 'f') {
      this.state.neuro.lfoDepth.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/lfo/dest' && msg.types === 'i') {
      this.state.neuro.lfoDest.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/lfo/waveform' && msg.types === 'i') {
      this.state.neuro.lfoWaveform.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/portamento' && msg.types === 'f') {
      this.state.neuro.portamentoMs.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/fx/send' && msg.types === 'f') {
      this.state.neuro.fxSend.set(msg.args[0] as number);
      return;
    }
    // Stink chain echoes
    if (a === '/synth/neuro/stink/enable' && msg.types === 'i') {
      this.state.neuro.stinkEnable.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/synth/neuro/stink/drive_low' && msg.types === 'f') {
      this.state.neuro.stinkDriveLow.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/drive_mid' && msg.types === 'f') {
      this.state.neuro.stinkDriveMid.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/drive_high' && msg.types === 'f') {
      this.state.neuro.stinkDriveHigh.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/mix_low' && msg.types === 'f') {
      this.state.neuro.stinkMixLow.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/mix_mid' && msg.types === 'f') {
      this.state.neuro.stinkMixMid.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/mix_high' && msg.types === 'f') {
      this.state.neuro.stinkMixHigh.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/fold' && msg.types === 'f') {
      this.state.neuro.stinkFold.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/crush' && msg.types === 'f') {
      this.state.neuro.stinkCrush.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/master_cutoff' && msg.types === 'f') {
      this.state.neuro.stinkMasterCutoffHz.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/master_resonance' && msg.types === 'f') {
      this.state.neuro.stinkMasterResonance.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/lfo2/rate' && msg.types === 'f') {
      this.state.neuro.stinkLfo2Rate.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/lfo2/depth' && msg.types === 'f') {
      this.state.neuro.stinkLfo2Depth.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/lfo2/dest' && msg.types === 'i') {
      this.state.neuro.stinkLfo2Dest.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/neuro/stink/lfo2/waveform' && msg.types === 'i') {
      this.state.neuro.stinkLfo2Waveform.set(msg.args[0] as number);
      return;
    }

    // Acid echoes
    if (a === '/synth/acid/volume' && msg.types === 'f') { this.state.acid.volume.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/on' && msg.types === 'i') { this.state.acid.on.set((msg.args[0] as number) !== 0); return; }
    if (a === '/synth/acid/midi/ch' && msg.types === 'i') { this.state.acid.midiChannel.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/waveform' && msg.types === 'i') { this.state.acid.waveform.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/tuning' && msg.types === 'i') { this.state.acid.tuning.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/cutoff' && msg.types === 'f') { this.state.acid.cutoffHz.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/resonance' && msg.types === 'f') { this.state.acid.resonance.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/env_mod' && msg.types === 'f') { this.state.acid.envMod.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/env_decay' && msg.types === 'f') { this.state.acid.envDecay.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/amp_decay' && msg.types === 'f') { this.state.acid.ampDecay.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/accent' && msg.types === 'f') { this.state.acid.accent.set(msg.args[0] as number); return; }
    if (a === '/synth/acid/slide' && msg.types === 'f') { this.state.acid.slideMs.set(msg.args[0] as number); return; }

    // Supersaw echoes
    if (a === '/synth/supersaw/volume' && msg.types === 'f') { this.state.supersaw.volume.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/on' && msg.types === 'i') { this.state.supersaw.on.set((msg.args[0] as number) !== 0); return; }
    if (a === '/synth/supersaw/midi/ch' && msg.types === 'i') { this.state.supersaw.midiChannel.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/detune' && msg.types === 'f') { this.state.supersaw.detuneCents.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/mix_center' && msg.types === 'f') { this.state.supersaw.mixCenter.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/cutoff' && msg.types === 'f') { this.state.supersaw.cutoffHz.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/resonance' && msg.types === 'f') { this.state.supersaw.resonance.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/attack' && msg.types === 'f') { this.state.supersaw.attack.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/decay' && msg.types === 'f') { this.state.supersaw.decay.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/sustain' && msg.types === 'f') { this.state.supersaw.sustain.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/release' && msg.types === 'f') { this.state.supersaw.release.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/chorus_depth' && msg.types === 'f') { this.state.supersaw.chorusDepth.set(msg.args[0] as number); return; }
    if (a === '/synth/supersaw/portamento' && msg.types === 'f') { this.state.supersaw.portamentoMs.set(msg.args[0] as number); return; }

    // Chip echoes
    if (a === '/synth/chip/volume' && msg.types === 'f') { this.state.chip.volume.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/on' && msg.types === 'i') { this.state.chip.on.set((msg.args[0] as number) !== 0); return; }
    if (a === '/synth/chip/midi/ch' && msg.types === 'i') { this.state.chip.midiChannel.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/pulse1_duty' && msg.types === 'i') { this.state.chip.pulse1Duty.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/pulse2_duty' && msg.types === 'i') { this.state.chip.pulse2Duty.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/pulse2_detune' && msg.types === 'f') { this.state.chip.pulse2Detune.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/tri_level' && msg.types === 'f') { this.state.chip.triLevel.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/noise_level' && msg.types === 'f') { this.state.chip.noiseLevel.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/voicing' && msg.types === 'i') { this.state.chip.voicing.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/arpeggio' && msg.types === 'i') { this.state.chip.arpeggio.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/arp_rate' && msg.types === 'f') { this.state.chip.arpRateHz.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/attack' && msg.types === 'f') { this.state.chip.attack.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/decay' && msg.types === 'f') { this.state.chip.decay.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/sustain' && msg.types === 'f') { this.state.chip.sustain.set(msg.args[0] as number); return; }
    if (a === '/synth/chip/release' && msg.types === 'f') { this.state.chip.release.set(msg.args[0] as number); return; }

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

    // Synth bus echoes — group fader + mute for all synths.
    if (a === '/synth/bus/volume' && msg.types === 'f') {
      this.state.synthBus.volume.set(msg.args[0] as number);
      return;
    }
    if (a === '/synth/bus/on' && msg.types === 'i') {
      this.state.synthBus.on.set((msg.args[0] as number) !== 0);
      return;
    }

    // Looper echoes — source (int), level (float), state (string),
    // length (float seconds). Transport actions on the firmware side
    // always fire /loop/state + /loop/length together so the UI flips
    // button highlights without polling.
    if (a === '/loop/source' && msg.types === 'i') {
      this.state.looper.source.set(msg.args[0] as number);
      return;
    }
    if (a === '/loop/level' && msg.types === 'f') {
      this.state.looper.level.set(msg.args[0] as number);
      return;
    }
    if (a === '/loop/state' && msg.types === 's') {
      this.state.looper.transport.set(msg.args[0] as string);
      return;
    }
    if (a === '/loop/length' && msg.types === 'f') {
      this.state.looper.length.set(msg.args[0] as number);
      return;
    }
    if (a === '/loop/quantize' && msg.types === 'i') {
      this.state.looper.quantize.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/loop/armed' && msg.types === 'i') {
      this.state.looper.armed.set(msg.args[0] as number);
      return;
    }

    // Clock echoes — source (string), bpm (float), running (int),
    // beatsPerBar (int). Source strings are 'ext' / 'int'; clamp to
    // the two legal values so a malformed echo can't poison the UI.
    if (a === '/clock/source' && msg.types === 's') {
      const s = msg.args[0] as string;
      this.state.clock.source.set(s === 'int' ? 'int' : 'ext');
      return;
    }
    if (a === '/clock/bpm' && msg.types === 'f') {
      this.state.clock.bpm.set(msg.args[0] as number);
      return;
    }
    if (a === '/clock/running' && msg.types === 'i') {
      this.state.clock.running.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/clock/beatsPerBar' && msg.types === 'i') {
      this.state.clock.beatsPerBar.set(msg.args[0] as number);
      return;
    }

    // Beats echoes. Per-step messages arrive with type 'iii' — clamp
    // track/step to valid ranges before poking the signal arrays.
    if (a === '/beats/run' && msg.types === 'i') {
      this.state.beats.running.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/beats/bpm' && msg.types === 'f') {
      this.state.beats.bpm.set(msg.args[0] as number);
      return;
    }
    if (a === '/beats/swing' && msg.types === 'f') {
      this.state.beats.swing.set(msg.args[0] as number);
      return;
    }
    if (a === '/beats/volume' && msg.types === 'f') {
      this.state.beats.volume.set(msg.args[0] as number);
      return;
    }
    if (a === '/beats/cursor' && msg.types === 'i') {
      this.state.beats.cursor.set(msg.args[0] as number);
      return;
    }
    if (a === '/beats/sd' && msg.types === 'i') {
      this.state.beats.sdReady.set((msg.args[0] as number) !== 0);
      return;
    }
    if (a === '/beats/clockSource' && msg.types === 's') {
      const s = msg.args[0] as string;
      if (s === 'internal' || s === 'external') this.state.beats.clockSource.set(s);
      return;
    }
    if (a === '/beats/mute' && msg.types === 'ii') {
      const trk = msg.args[0] as number;
      const t = this.state.beats.tracks[trk];
      if (t) t.muted.set((msg.args[1] as number) !== 0);
      return;
    }
    if (a === '/beats/step' && msg.types === 'iii') {
      const trk  = msg.args[0] as number;
      const step = msg.args[1] as number;
      const t = this.state.beats.tracks[trk];
      if (t && step >= 0 && step < BEATS_STEP_COUNT) {
        t.stepsOn[step].set((msg.args[2] as number) !== 0);
      }
      return;
    }
    if (a === '/beats/vel' && msg.types === 'iif') {
      const trk  = msg.args[0] as number;
      const step = msg.args[1] as number;
      const t = this.state.beats.tracks[trk];
      if (t && step >= 0 && step < BEATS_STEP_COUNT) {
        t.stepsVel[step].set(msg.args[2] as number);
      }
      return;
    }
    if (a === '/beats/sample' && msg.types === 'is') {
      const trk = msg.args[0] as number;
      const t = this.state.beats.tracks[trk];
      if (t) t.sample.set(msg.args[1] as string);
      return;
    }
    // Firmware /beats/clear is a write-only echo; the per-step echoes
    // that follow (for any steps that were actually on) drive per-cell
    // signals. Optimistic clear already zeroed the cells on outbound.
    if (a === '/beats/clear' && msg.types === 'i') {
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

    // ---------- Arpeggiator echoes ----------
    if (a === '/arp/on'            && msg.types === 'i') { this.state.arp.on           .set((msg.args[0] as number) !== 0); return; }
    if (a === '/arp/pattern'       && msg.types === 'i') { this.state.arp.pattern      .set(msg.args[0] as number); return; }
    if (a === '/arp/rate'          && msg.types === 'i') { this.state.arp.rate         .set(msg.args[0] as number); return; }
    if (a === '/arp/gate'          && msg.types === 'f') { this.state.arp.gate         .set(msg.args[0] as number); return; }
    if (a === '/arp/swing'         && msg.types === 'f') { this.state.arp.swing        .set(msg.args[0] as number); return; }
    if (a === '/arp/octaveRange'   && msg.types === 'i') { this.state.arp.octaveRange  .set(msg.args[0] as number); return; }
    if (a === '/arp/octaveMode'    && msg.types === 'i') { this.state.arp.octaveMode   .set(msg.args[0] as number); return; }
    if (a === '/arp/latch'         && msg.types === 'i') { this.state.arp.latch        .set((msg.args[0] as number) !== 0); return; }
    if (a === '/arp/hold'          && msg.types === 'i') { this.state.arp.hold         .set((msg.args[0] as number) !== 0); return; }
    if (a === '/arp/velMode'       && msg.types === 'i') { this.state.arp.velMode      .set(msg.args[0] as number); return; }
    if (a === '/arp/velFixed'      && msg.types === 'i') { this.state.arp.velFixed     .set(msg.args[0] as number); return; }
    if (a === '/arp/velAccent'     && msg.types === 'i') { this.state.arp.velAccent    .set(msg.args[0] as number); return; }
    if (a === '/arp/stepMask'      && msg.types === 'i') { this.state.arp.stepMask     .set(msg.args[0] as number); return; }
    if (a === '/arp/stepLength'    && msg.types === 'i') { this.state.arp.stepLength   .set(msg.args[0] as number); return; }
    if (a === '/arp/mpeMode'       && msg.types === 'i') { this.state.arp.mpeMode      .set(msg.args[0] as number); return; }
    if (a === '/arp/outputChannel' && msg.types === 'i') { this.state.arp.outputChannel.set(msg.args[0] as number); return; }
    if (a === '/arp/scatterBase'   && msg.types === 'i') { this.state.arp.scatterBase  .set(msg.args[0] as number); return; }
    if (a === '/arp/scatterCount'  && msg.types === 'i') { this.state.arp.scatterCount .set(msg.args[0] as number); return; }
    if (a === '/arp/scale'         && msg.types === 'i') { this.state.arp.scale        .set(msg.args[0] as number); return; }
    if (a === '/arp/scaleRoot'     && msg.types === 'i') { this.state.arp.scaleRoot    .set(msg.args[0] as number); return; }
    if (a === '/arp/transpose'     && msg.types === 'i') { this.state.arp.transpose    .set(msg.args[0] as number); return; }
    if (a === '/arp/repeat'        && msg.types === 'i') { this.state.arp.repeat       .set(msg.args[0] as number); return; }
    if (a === '/arp/panic') return;  // firmware echo of a triggered panic — nothing to mirror
  }

  private sendMsg(address: string, types: string, args: OscArg[]): void {
    this.send(encodeMessage(address, types, args));
  }

  // Coalesce repeated sends to the same address to at most one per
  // THROTTLE_MS. The latest call's args always "win" — if a drag fires
  // 50 events in 50 ms, we send the first immediately, then the last
  // value 33 ms later, and skip the middle 48. The trailing-edge fire
  // matters: it guarantees the firmware lands on the value the user
  // released the slider on, not on whatever happened to be latest at
  // the start of the throttle window.
  private sendThrottled(address: string, types: string, args: OscArg[]): void {
    const now = performance.now();
    let entry = this.throttle.get(address);
    if (!entry) {
      entry = { lastSentAt: 0, pendingTimer: null, pendingTypes: types, pendingArgs: null };
      this.throttle.set(address, entry);
    }
    const elapsed = now - entry.lastSentAt;
    if (elapsed >= Dispatcher.THROTTLE_MS && entry.pendingTimer === null) {
      // Cold path — fire immediately and arm the throttle window.
      entry.lastSentAt = now;
      this.sendMsg(address, types, args);
      return;
    }
    // Hot path — coalesce. Stash the latest args; the trailing timer
    // will pick them up.
    entry.pendingTypes = types;
    entry.pendingArgs = args;
    if (entry.pendingTimer === null) {
      const delay = Math.max(0, Dispatcher.THROTTLE_MS - elapsed);
      entry.pendingTimer = window.setTimeout(() => {
        const e = this.throttle.get(address);
        if (!e) return;
        if (e.pendingArgs !== null) {
          e.lastSentAt = performance.now();
          this.sendMsg(address, e.pendingTypes, e.pendingArgs);
          e.pendingArgs = null;
        }
        e.pendingTimer = null;
      }, delay);
    }
  }
}

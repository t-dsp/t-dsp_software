// Tiny signal-based reactive store and mixer state model.
//
// "Mixer state" here is the *client-side* mirror that the UI subscribes to. The
// firmware is the source of truth — incoming OSC echoes update these signals,
// and outgoing user gestures both update the signal locally (optimistic UI) and
// send an OSC message that the firmware will echo back.

type Listener<T> = (v: T) => void;

export class Signal<T> {
  private listeners = new Set<Listener<T>>();
  constructor(private value: T) {}

  get(): T {
    return this.value;
  }

  set(v: T): void {
    if (Object.is(this.value, v)) return;
    this.value = v;
    for (const l of this.listeners) l(v);
  }

  subscribe(l: Listener<T>): () => void {
    this.listeners.add(l);
    l(this.value);
    return () => {
      this.listeners.delete(l);
    };
  }
}

export interface ChannelState {
  fader: Signal<number>; // 0..1 normalized
  on: Signal<boolean>; // X32 idiom: true = unmuted (mix/on = 1)
  solo: Signal<boolean>;
  pan: Signal<number>; // 0..1, 0.5 = center
  // X32 stereo link: only the ODD channel of a pair (1,3,5) carries
  // the flag. Even channels (2,4,6) always have link=false on the
  // model; their UI disables itself by subscribing to the odd
  // neighbor's link signal instead.
  link: Signal<boolean>;
  peak: Signal<number>; // 0..1 from /meters/input blob
  rms: Signal<number>;
  name: Signal<string>;
  // Per-channel USB record send. When true, this channel's signal
  // is summed into the USB capture output. Forced to 0 in the firmware
  // binding when main.loopEnable is true; the UI reflects that by
  // disabling (greying) the Rec button so the stored state is preserved
  // and returns when loop goes off.
  recSend: Signal<boolean>;
}

export interface BusState {
  // Stereo main faders. When link=true, writes to either side propagate
  // to the other (handled by the firmware model; the client just echoes
  // both sides back). R slider is visually disabled while linked.
  faderL: Signal<number>;
  faderR: Signal<number>;
  link: Signal<boolean>;
  on: Signal<boolean>;
  // Windows volume ("hostvol") is a bypassable pre-DAC attenuator that
  // lives downstream of the fader stage in the audio graph. hostvolValue
  // is read-only (the firmware echoes whatever usbIn.volume() reports);
  // hostvolEnable is writable and lets the engineer disable the Windows
  // slider so the fader is the only gain stage.
  hostvolEnable: Signal<boolean>;
  hostvolValue: Signal<number>;
  // Stereo meters for the main bus — populated from /meters/output
  // blobs (2 pairs: L peak/rms, R peak/rms). Taps are post-fader /
  // pre-hostvol, so the reading tracks the fader but NOT the Windows
  // volume slider.
  peakL: Signal<number>;
  rmsL: Signal<number>;
  peakR: Signal<number>;
  rmsR: Signal<number>;
  // Host-volume (post-hostvol) meters — populated from /meters/host.
  // Taps are post-hostvol, showing the actual DAC-bound level. Compare
  // against peakL/R above to see Windows volume attenuation.
  hostPeakL: Signal<number>;
  hostRmsL: Signal<number>;
  hostPeakR: Signal<number>;
  hostRmsR: Signal<number>;
  // USB capture-side host volume — driven by Windows' recording-device
  // slider via the FU 0x30 Feature Unit added by the teensy4 core patch.
  // The firmware polls AudioOutputUSB::features and broadcasts on change
  // (/-cap/hostvol/value f, /-cap/hostvol/mute i). The web surface
  // displays this as a read-only "CAP HOST" strip — there's no control
  // path back: only Windows owns this slider.
  captureHostvolValue: Signal<number>;
  captureHostvolMute: Signal<boolean>;
  // Loopback: when true, the post-fader / pre-hostvol main mix is
  // summed into the USB capture output (record what's in the
  // headphones). While on, the firmware forces per-channel recSend
  // amps to 0 so nothing double-counts; the UI greys each channel's
  // Rec button to match.
  loopEnable: Signal<boolean>;
}

// Dexed synth state — mirror of the firmware's AudioSynthDexed controls
// that the Synth tab's Dexed sub-panel renders.
//
// bankNames / voiceNames are populated on demand: the client requests
// /synth/dexed/bank/names once on connect (bank list is compile-time
// fixed in firmware), and /synth/dexed/voice/names for whatever bank is
// selected. Switching banks triggers a new voice-names query so the
// voice dropdown re-populates.
export interface DexedState {
  bank: Signal<number>;        // 0..9
  voice: Signal<number>;       // 0..31
  voiceName: Signal<string>;   // trimmed 10-char voice name
  volume: Signal<number>;      // 0..1 linear
  // X32-style on/off. When false, firmware zeros g_dexedGain but
  // preserves the stored volume so turning back on restores the
  // fader position.
  on: Signal<boolean>;
  // MIDI-Auto mode. When true, this synth's on-state tracks the
  // active sub-tab (switching tabs mutes the outgoing synth and
  // unmutes the incoming one). Client-side only — enforced by
  // enforceMidiAuto. Turning it off keeps the synth sounding even
  // when its tab isn't selected, so you can layer synths.
  midiAuto: Signal<boolean>;
  midiChannel: Signal<number>; // 0 = omni, 1..16
  fxSend: Signal<number>;      // 0..1 send amount into shared FX bus
  bankNames: Signal<string[]>;
  voiceNames: Signal<string[]>; // names for the currently-selected bank
}

// Per-voice MPE state — one entry per physical voice slot (4 slots
// in the firmware's kVoiceCount). The firmware streams these via
// /synth/mpe/voices at 30 Hz when the client subscribes.
//
// Signals, not a plain array, because the voice orb renderer wants
// to subscribe to each voice independently and repaint only when
// that specific voice's state changes.
export interface MpeVoiceState {
  held:      Signal<boolean>;
  channel:   Signal<number>;   // 1..16 MIDI channel; 0 if never used
  note:      Signal<number>;   // 0..127 MIDI note
  pitchSemi: Signal<number>;   // signed semitone offset (bend + LFO)
  pressure:  Signal<number>;   // 0..1
  timbre:    Signal<number>;   // 0..1 (CC#74)
}

// MPE VA synth state — mirror of the firmware's MpeVaSink controls.
// Same read/write/echo pattern as DexedState. `voices` is streamed
// telemetry; everything else is parameter state.
export interface MpeState {
  volume:           Signal<number>;  // 0..1
  on:               Signal<boolean>; // X32-style mix on/off
  midiAuto:         Signal<boolean>; // see DexedState.midiAuto
  attack:           Signal<number>;  // seconds (0..10)
  release:          Signal<number>;  // seconds (0..10)
  waveform:         Signal<number>;  // 0=saw, 1=square, 2=tri, 3=sine
  filterCutoffHz:   Signal<number>;  // 20..20000
  filterResonance:  Signal<number>;  // 0.707..5.0
  lfoRate:          Signal<number>;  // 0..20 Hz
  lfoDepth:         Signal<number>;  // 0..1
  lfoDest:          Signal<number>;  // 0=off, 1=cutoff, 2=pitch, 3=amp
  lfoWaveform:      Signal<number>;  // 0=sine, 1=tri, 2=saw, 3=square
  masterChannel:    Signal<number>;  // 1..16 (MPE master channel)
  fxSend:           Signal<number>;  // 0..1 into shared FX bus
  voices:           MpeVoiceState[]; // fixed-length 4; telemetry-driven
  // Currently-selected preset id ('' = none). Informational only —
  // firmware doesn't know about presets; this is the UI's memory of
  // "which card did the user last click?" for highlight state.
  activePresetId:   Signal<string>;
}

// Neuro (reese/neuro bass) synth state — mirror of the firmware's
// NeuroSink controls. Mono engine, so no per-voice telemetry; every
// field is a simple scalar param.
export interface NeuroState {
  volume:           Signal<number>;  // 0..1 linear
  on:               Signal<boolean>; // X32-style mix on/off
  midiAuto:         Signal<boolean>; // see DexedState.midiAuto
  midiChannel:      Signal<number>;  // 0 = omni, 1..16
  attack:           Signal<number>;  // seconds
  release:          Signal<number>;  // seconds
  detuneCents:      Signal<number>;  // 0..50 (spread between osc1/osc2)
  subLevel:         Signal<number>;  // 0..1 (sine sub level)
  osc3Level:        Signal<number>;  // 0..1 (center saw level)
  filterCutoffHz:   Signal<number>;  // 20..20000
  filterResonance:  Signal<number>;  // 0.707..5.0
  lfoRate:          Signal<number>;  // 0..20 Hz
  lfoDepth:         Signal<number>;  // 0..1
  lfoDest:          Signal<number>;  // 0=off, 1=cutoff, 2=pitch, 3=amp
  lfoWaveform:      Signal<number>;  // 0=sine, 1=tri, 2=saw, 3=square
  portamentoMs:     Signal<number>;  // 0..2000 (0 = snap)
  fxSend:           Signal<number>;  // 0..1 into shared FX bus
  // Stink chain — multiband destruction on the neuro bus (Phase 2f).
  stinkEnable:         Signal<boolean>;
  stinkDriveLow:       Signal<number>;  // 0..10 pre-shape gain per band
  stinkDriveMid:       Signal<number>;
  stinkDriveHigh:      Signal<number>;
  stinkMixLow:         Signal<number>;  // 0..1 band recombine balance
  stinkMixMid:         Signal<number>;
  stinkMixHigh:        Signal<number>;
  stinkFold:           Signal<number>;  // 0..1 wavefolder intensity
  stinkCrush:          Signal<number>;  // 0..1 bitcrush intensity
  stinkMasterCutoffHz: Signal<number>;  // master LP cutoff Hz
  stinkMasterResonance: Signal<number>; // 0.707..5.0
  stinkLfo2Rate:       Signal<number>;  // 0..20 Hz
  stinkLfo2Depth:      Signal<number>;  // 0..1
  stinkLfo2Dest:       Signal<number>;  // 0=off, 1=fold, 2=crush, 3=master, 4=mid drive
  stinkLfo2Waveform:   Signal<number>;  // 0=sine, 1=tri, 2=saw, 3=square
  activePresetId:   Signal<string>;  // UI-only preset highlight
}

// Acid (TB-303 style) synth state. Mono engine — all params scalar.
export interface AcidState {
  volume:       Signal<number>;
  on:           Signal<boolean>;
  midiAuto:     Signal<boolean>;
  midiChannel:  Signal<number>;   // 0 = omni, 1..16 (default 4)
  waveform:     Signal<number>;   // 0 = saw, 1 = square
  tuning:       Signal<number>;   // -24..+24 semitones
  cutoffHz:     Signal<number>;   // 20..20000 base cutoff
  resonance:    Signal<number>;   // 0.707..5.0
  envMod:       Signal<number>;   // 0..1 filter env depth
  envDecay:     Signal<number>;   // seconds
  ampDecay:     Signal<number>;   // seconds
  accent:       Signal<number>;   // 0..1
  slideMs:      Signal<number>;   // 0..1000 ms
  activePresetId: Signal<string>;
}

// Supersaw (JP-8000 style) synth state.
export interface SupersawState {
  volume:       Signal<number>;
  on:           Signal<boolean>;
  midiAuto:     Signal<boolean>;
  midiChannel:  Signal<number>;   // default 5
  detuneCents:  Signal<number>;   // 0..100
  mixCenter:    Signal<number>;   // 0..1 — center vs side balance
  cutoffHz:     Signal<number>;
  resonance:    Signal<number>;
  attack:       Signal<number>;
  decay:        Signal<number>;
  sustain:      Signal<number>;
  release:      Signal<number>;
  chorusDepth:  Signal<number>;   // 0..1
  portamentoMs: Signal<number>;
  activePresetId: Signal<string>;
}

// Chip (NES/Gameboy) synth state.
export interface ChipState {
  volume:        Signal<number>;
  on:            Signal<boolean>;
  midiAuto:      Signal<boolean>;
  midiChannel:   Signal<number>;  // default 6
  pulse1Duty:    Signal<number>;  // 0=12.5%, 1=25%, 2=50%, 3=75%
  pulse2Duty:    Signal<number>;
  pulse2Detune:  Signal<number>;  // cents
  triLevel:      Signal<number>;  // 0..1 triangle sub mix
  noiseLevel:    Signal<number>;  // 0..1 noise layer mix
  voicing:       Signal<number>;  // 0=unison, 1=octave, 2=fifth, 3=thirds
  arpeggio:      Signal<number>;  // 0=off, 1=up, 2=down, 3=random
  arpRateHz:     Signal<number>;  // 0..40 Hz
  attack:        Signal<number>;
  decay:         Signal<number>;
  sustain:       Signal<number>;
  release:       Signal<number>;
  activePresetId: Signal<string>;
}

// Main-bus processing (Processing tab). Post-hostvol, pre-DAC stages
// mirrored here so the UI can render current values without a query.
export interface ProcessingState {
  shelfEnable: Signal<boolean>;
  shelfFreqHz: Signal<number>;
  shelfGainDb: Signal<number>;
  limiterEnable: Signal<boolean>;
}

// Shared FX bus (FX tab). Chorus + reverb chain; every synth taps a
// mono send amount into this bus and the wet stereo return mixes into
// the main output. Per-synth send amounts live on the synth's own
// state block (e.g. DexedState.fxSend), not here.
export interface FxState {
  chorusEnable: Signal<boolean>;
  chorusVoices: Signal<number>;  // 2..8 (quantized "depth")
  reverbEnable: Signal<boolean>;
  reverbSize: Signal<number>;    // 0..1 roomsize
  reverbDamping: Signal<number>; // 0..1
  reverbReturn: Signal<number>;  // 0..1 wet level into main
}

// Mono looper state — mirror of the firmware's tdsp::Looper plus the
// sketch-side source/level bookkeeping. transport is the authoritative
// mode string echoed from /loop/state: "idle" | "rec" | "play" | "stopped".
// length is in seconds (0 while Idle / before the first take finalizes).
export interface LooperState {
  source:    Signal<number>;  // 0 = none, 1..N = channel index
  level:     Signal<number>;  // 0..1 return level
  transport: Signal<string>;  // idle | rec | play | stopped
  length:    Signal<number>;  // seconds
  // Length in beats at the clock's current tempo. 0 when no take /
  // clock stopped / BPM unknown. Non-quantized takes are fractional
  // (e.g. 3.73); quantized takes land on whole numbers because the
  // firmware snaps the recorded length to a multiple of the current
  // samples-per-beat on the record->play transition.
  lengthBeats: Signal<number>;
  // Quantize-to-beat: when true, the firmware defers transport actions
  // (rec/play/stop/clear) to the next clock beat edge instead of firing
  // immediately, AND snaps the recorded length to a whole number of
  // beats on record->play. `armed` echoes which action is pending
  // (0=none, 1=rec, 2=play, 3=stop, 4=clear).
  quantize:  Signal<boolean>;
  armed:     Signal<number>;
  // Cued action — two-stage arm. Pressing CUE REC (etc.) stages an
  // action here without firing or beat-arming it; pressing GO moves
  // it into `armed`, which then fires on the next beat edge. Same
  // encoding as `armed` (0=none, 1=rec, 2=play, 3=stop, 4=clear).
  cued:      Signal<number>;
  // Clock-follow: when true, playback rate = currentBpm / recordedBpm
  // so the loop stays in sync if the clock tempo changes. Pitch shifts
  // with tempo (sample-rate scaling, no time-stretch). recordedBpm is
  // 0 until the first record-finalize; after that it's the clock's BPM
  // at the moment the take was finalized and serves as the reference
  // denominator for the rate calculation.
  clockFollow: Signal<boolean>;
  recordedBpm: Signal<number>;
}

// Beats drum machine state (Beats tab). 4 tracks × 16 steps, per-step
// on/velocity signals so a click on a single cell only notifies
// subscribers watching that cell. Track names are client-fixed —
// firmware doesn't broadcast them.
export const BEATS_TRACK_COUNT = 4;
export const BEATS_STEP_COUNT  = 16;

export interface BeatsTrackState {
  name:     string;                 // fixed; not broadcast
  muted:    Signal<boolean>;
  sample:   Signal<string>;         // filename on SD; empty for synth tracks
  isSample: boolean;                // true for tracks 2/3 in the MVP
  stepsOn:  Signal<boolean>[];      // length 16
  stepsVel: Signal<number>[];       // length 16; 0..1
}

export interface BeatsState {
  running:     Signal<boolean>;
  bpm:         Signal<number>;      // 20..300
  swing:       Signal<number>;      // 0..0.75
  volume:      Signal<number>;      // 0..1 group level
  cursor:      Signal<number>;      // last-fired step (-1 idle)
  sdReady:     Signal<boolean>;
  clockSource: Signal<'internal' | 'external'>;
  tracks:      BeatsTrackState[];   // length BEATS_TRACK_COUNT
}

// Shared musical-time clock — mirror of firmware `tdsp::Clock`. Drives
// every tempo-aware module (looper quantize, future LFO sync, beat
// machine). source=='ext' means the clock is slaved to incoming MIDI
// 0xF8 ticks; 'int' means the firmware is master at `bpm`. `running`
// is echoed from transport (Start / Stop / Continue or stall watchdog).
export interface ClockState {
  source:       Signal<'ext' | 'int'>;
  bpm:          Signal<number>;     // 20..300; last-measured in ext, set point in int
  running:      Signal<boolean>;    // transport running / stopped
  beatsPerBar:  Signal<number>;     // 1..16
  // Metronome — short tone burst on each beat edge, with an accent on
  // beat 1 of the bar. Used as an audible reference while arming a
  // looper take (record on the downbeat, stop on the last beat of the
  // bar). Keyed off the same clock that drives the looper, so it stays
  // in sync. Lives under the clock tree because conceptually it's a
  // clock output, not a mixer feature.
  metroOn:     Signal<boolean>;
  metroLevel:  Signal<number>;      // 0..1 oscillator amplitude
}

// Synth bus — group fader + mute sitting downstream of every per-synth
// volume and upstream of preMix slot 1. Also taps the looper source mux
// (the "Synth" option), so recording the synth source captures whatever
// this bus lets through.
export interface SynthBusState {
  volume: Signal<number>;   // 0..1 linear
  on:     Signal<boolean>;  // X32-style mix-on
}

// Synth slot metadata — one entry per slot in the firmware's
// SynthSwitcher (kMaxSlots=4). Driven by /synth/slots reply, which
// packs each slot as "id|displayName". Empty slots come back as
// "silent|(empty)".
export interface SynthSlotMeta {
  id: string;            // stable short identifier ("dexed", "sampler", "silent")
  displayName: string;   // user-facing label ("Dexed FM", "Sampler", "(empty)")
}

// Single-active-slot synth selection model (Phase 0/2). The firmware
// guarantees exactly one slot is audible at a time; switching panics
// held notes on the outgoing slot and zero-gains it. The /synth/active
// echo lands here; the slot-picker UI subscribes for highlight state.
export interface SynthSlotState {
  active: Signal<number>;             // 0..3, -1 = none / unknown
  slots:  Signal<SynthSlotMeta[]>;    // length 4
}

// Slot 1 — multisample sampler (loads /samples/<bank>/ from SD).
// Mirrors the firmware's MultisampleSlot config + read-only info.
export interface SamplerState {
  on:                Signal<boolean>;
  volume:            Signal<number>;   // X32 0..1
  midiChannel:       Signal<number>;   // 0 = omni, 1..16 = single-channel
  bankName:          Signal<string>;   // e.g. "piano"
  numSamples:        Signal<number>;
  numReleaseSamples: Signal<number>;
}

// Arpeggiator filter state — mirror of firmware tdsp::ArpFilter. All
// enums encoded as ints matching the C++ enum values.
// See lib/TDspArp/src/ArpFilter.h for the canonical definitions.
export interface ArpState {
  on:             Signal<boolean>;  // enabled (false = bypass)
  pattern:        Signal<number>;   // 0..PatCount-1
  rate:           Signal<number>;   // 0..Rate_Count-1 (musical division)
  gate:           Signal<number>;   // 0.05..1.5 of step length
  swing:          Signal<number>;   // 0.5 (straight) .. 0.85
  octaveRange:    Signal<number>;   // 1..4
  octaveMode:     Signal<number>;   // 0=up 1=down 2=updown 3=random
  latch:          Signal<boolean>;
  hold:           Signal<boolean>;
  velMode:        Signal<number>;   // 0..VelCount-1
  velFixed:       Signal<number>;   // 0..127
  velAccent:      Signal<number>;   // 0..127
  stepMask:       Signal<number>;   // uint32; bit N = step N enabled
  stepLength:     Signal<number>;   // 1..32
  mpeMode:        Signal<number>;   // 0=mono 1=scatter 2=exprFollow 3=perNote
  outputChannel:  Signal<number>;   // 1..16
  scatterBase:    Signal<number>;   // 1..16
  scatterCount:   Signal<number>;   // 1..8
  scale:          Signal<number>;   // 0..ScaleCount-1 (0 = off)
  scaleRoot:      Signal<number>;   // 0..11 (C..B)
  transpose:      Signal<number>;   // -24..+24 semitones
  repeat:         Signal<number>;   // 1..8 — repeat each step N times
  activePresetId: Signal<string>;
}

export interface MixerState {
  channels: ChannelState[];
  main: BusState;
  dexed: DexedState;
  mpe: MpeState;
  neuro: NeuroState;
  acid: AcidState;
  supersaw: SupersawState;
  chip: ChipState;
  processing: ProcessingState;
  fx: FxState;
  synthBus: SynthBusState;
  synthSlot: SynthSlotState;
  sampler: SamplerState;
  looper: LooperState;
  clock: ClockState;
  beats: BeatsState;
  arp: ArpState;
  connected: Signal<boolean>;
  metersOn: Signal<boolean>;
  // UI-only state — never echoed to firmware. Drives the per-channel
  // detail surfacing (TUNE workspace, future) and the bottom-strip
  // Sel highlight. 0-based channel index.
  selectedChannel: Signal<number>;
  // UI-only persona toggle. Persisted to localStorage by main.ts.
  // 'engineer' = mix-tier defaults (fader bank in bottom strip, MIX
  // is home). 'musician' = play-tier defaults (keyboard in bottom
  // strip, PLAY is home).
  mode: Signal<'engineer' | 'musician'>;
  // UI-only workspace + inner-tab state. Persisted to localStorage by
  // main.ts so reloads land back where the user left off. See
  // planning/ui-rebuild/02-hierarchy.md for the workspace structure.
  activeWorkspace: Signal<'mix' | 'play' | 'tune' | 'fx' | 'setup'>;
  activePlayTab:   Signal<'synths' | 'arp' | 'beats' | 'loop'>;
  activeFxTab:     Signal<'busfx' | 'processing' | 'spectrum'>;
  activeSetupTab:  Signal<'codec' | 'clock' | 'rawosc' | 'serial'>;
}

// Default channel names must match the small-mixer firmware's defaults in
// lib/TDspMixer/src/MixerModel.cpp — these are what the client shows before
// the firmware echoes back /ch/NN/config/name values during its initial
// state dump.
const DEFAULT_CHANNEL_NAMES = [
  'USB L',
  'USB R',
  'Line L',
  'Line R',
  'Mic L',
  'Mic R',
  'XLR 1',  // 7  TLV320ADC6140 CH1 — external XLR preamp
  'XLR 2',  // 8
  'XLR 3',  // 9
  'XLR 4',  // 10
];

export function createMixerState(channelCount: number): MixerState {
  const channels: ChannelState[] = [];
  for (let i = 0; i < channelCount; i++) {
    const defaultName =
      DEFAULT_CHANNEL_NAMES[i] ?? `Ch ${String(i + 1).padStart(2, '0')}`;
    // Firmware defaults odd channels (1, 3, 5) to link=true. XLR pairs
    // (7-8, 9-10) default unlinked because they're individual mono mics,
    // not natural L/R pairs. Client matches so the UI is correctly slave-
    // disabled before the first echo arrives.
    const defaultLink = i === 0 || i === 2 || i === 4;
    // Firmware defaults recSend per-source to preserve prior USB-out
    // behaviour: Line (idx 2,3), Mic (4,5), and XLR (6..9) ON; USB playback
    // (0,1) OFF. See MixerModel::defaultRecSend. Client mirrors this so
    // the Rec button renders correctly before the snapshot echo arrives.
    const defaultRecSend = i >= 2;
    channels.push({
      fader: new Signal(1.0),  // matches firmware default (MixerModel::reset)
      on: new Signal(true),
      solo: new Signal(false),
      pan: new Signal(0.5),
      link: new Signal(defaultLink),
      peak: new Signal(0),
      rms: new Signal(0),
      name: new Signal(defaultName),
      recSend: new Signal(defaultRecSend),
    });
  }

  return {
    channels,
    main: {
      faderL: new Signal(0.75),
      faderR: new Signal(0.75),
      link: new Signal(true),
      on: new Signal(true),
      hostvolEnable: new Signal(true),
      hostvolValue: new Signal(1.0),
      peakL: new Signal(0),
      rmsL: new Signal(0),
      peakR: new Signal(0),
      rmsR: new Signal(0),
      hostPeakL: new Signal(0),
      hostRmsL: new Signal(0),
      hostPeakR: new Signal(0),
      hostRmsR: new Signal(0),
      captureHostvolValue: new Signal(0),
      captureHostvolMute: new Signal(false),
      loopEnable: new Signal(false),
    },
    dexed: {
      bank: new Signal(0),
      voice: new Signal(0),
      voiceName: new Signal(''),
      volume: new Signal(0.7),       // matches firmware default
      // On by default because Dexed is the default sub-tab; the
      // selectSynthSubtab('dexed') call at app boot would set this
      // anyway, but matching here keeps the first-paint state sane.
      on: new Signal(true),
      midiAuto: new Signal(true),    // follow sub-tab by default
      midiChannel: new Signal(0),    // matches firmware — omni; active-tab enforcer routes
      fxSend: new Signal(0),         // dry by default, matches firmware
      bankNames: new Signal<string[]>([]),
      voiceNames: new Signal<string[]>([]),
    },
    mpe: {
      // Defaults match MpeVaSink's firmware defaults. First
      // /snapshot reply will overwrite with whatever's actually
      // running, but matching here keeps the UI sensible before
      // connect.
      volume:          new Signal(0.7),
      // Matches firmware default (g_mpeOn=true). On app connect, the
      // Auto-mode enforcer in main.ts re-derives the correct on-state
      // from the current sub-tab (mutes MPE if Dexed is the default
      // tab) without creating a mismatch window before /snapshot.
      on:              new Signal(true),
      midiAuto:        new Signal(true),   // follow sub-tab by default
      attack:          new Signal(0.005),
      release:         new Signal(0.300),
      waveform:        new Signal(0),
      filterCutoffHz:  new Signal(8000),
      filterResonance: new Signal(0.707),
      lfoRate:         new Signal(0),
      lfoDepth:        new Signal(0),
      lfoDest:         new Signal(0),
      lfoWaveform:     new Signal(0),
      masterChannel:   new Signal(0),      // 0 = no master, notes on any channel
      fxSend:          new Signal(0),
      voices: Array.from({ length: 4 }, () => ({
        held:      new Signal(false),
        channel:   new Signal(0),
        note:      new Signal(0),
        pitchSemi: new Signal(0),
        pressure:  new Signal(0),
        timbre:    new Signal(0.5),
      })),
      activePresetId:  new Signal(''),
    },
    neuro: {
      // Defaults match NeuroSink's firmware cold-boot (see main.cpp
      // "Neuro (reese bass) init" block). /snapshot echoes overwrite
      // these on connect; matching here keeps the UI sane before then.
      volume:          new Signal(0.7),
      on:              new Signal(true),
      midiAuto:        new Signal(true),   // follow sub-tab by default
      midiChannel:     new Signal(0),      // omni — active-tab enforcer routes
      attack:          new Signal(0.005),
      release:         new Signal(0.200),
      detuneCents:     new Signal(7.0),
      subLevel:        new Signal(0.6),
      osc3Level:       new Signal(0.7),
      filterCutoffHz:  new Signal(600),
      filterResonance: new Signal(2.5),
      lfoRate:         new Signal(0),
      lfoDepth:        new Signal(0.5),
      lfoDest:         new Signal(0),
      lfoWaveform:     new Signal(1),      // triangle
      portamentoMs:    new Signal(0),
      fxSend:          new Signal(0),
      // Stink chain — defaults match firmware cold-boot (enabled by
      // default because reese without grit isn't neuro).
      stinkEnable:          new Signal(true),
      stinkDriveLow:        new Signal(1.5),
      stinkDriveMid:        new Signal(3.0),
      stinkDriveHigh:       new Signal(2.0),
      stinkMixLow:          new Signal(1.0),
      stinkMixMid:          new Signal(1.0),
      stinkMixHigh:         new Signal(0.8),
      stinkFold:            new Signal(0.0),
      stinkCrush:           new Signal(0.0),
      stinkMasterCutoffHz:  new Signal(8000),
      stinkMasterResonance: new Signal(1.2),
      stinkLfo2Rate:        new Signal(0),
      stinkLfo2Depth:       new Signal(0.5),
      stinkLfo2Dest:        new Signal(0),
      stinkLfo2Waveform:    new Signal(1),
      activePresetId:  new Signal(''),
    },
    acid: {
      // Defaults match AcidSink firmware cold-boot.
      volume:       new Signal(0.7),
      on:           new Signal(true),
      midiAuto:     new Signal(true),
      midiChannel:  new Signal(0),
      waveform:     new Signal(0),
      tuning:       new Signal(0),
      cutoffHz:     new Signal(500),
      resonance:    new Signal(3.8),
      envMod:       new Signal(0.6),
      envDecay:     new Signal(0.3),
      ampDecay:     new Signal(0.4),
      accent:       new Signal(0.5),
      slideMs:      new Signal(60),
      activePresetId: new Signal(''),
    },
    supersaw: {
      volume:       new Signal(0.6),
      on:           new Signal(true),
      midiAuto:     new Signal(true),
      midiChannel:  new Signal(0),
      detuneCents:  new Signal(18),
      mixCenter:    new Signal(0.4),
      cutoffHz:     new Signal(9000),
      resonance:    new Signal(1.0),
      attack:       new Signal(0.05),
      decay:        new Signal(0.3),
      sustain:      new Signal(0.8),
      release:      new Signal(0.6),
      chorusDepth:  new Signal(0.5),
      portamentoMs: new Signal(0),
      activePresetId: new Signal(''),
    },
    chip: {
      volume:        new Signal(0.6),
      on:            new Signal(true),
      midiAuto:      new Signal(true),
      midiChannel:   new Signal(0),
      pulse1Duty:    new Signal(2),   // 50%
      pulse2Duty:    new Signal(1),   // 25%
      pulse2Detune:  new Signal(7),
      triLevel:      new Signal(0.5),
      noiseLevel:    new Signal(0),
      voicing:       new Signal(1),   // octave
      arpeggio:      new Signal(0),   // off
      arpRateHz:     new Signal(12),
      attack:        new Signal(0.001),
      decay:         new Signal(0.08),
      sustain:       new Signal(0.5),
      release:       new Signal(0.15),
      activePresetId: new Signal(''),
    },
    processing: {
      shelfEnable:    new Signal(true),    // matches firmware defaults
      shelfFreqHz:    new Signal(3000),
      shelfGainDb:    new Signal(-12),
      limiterEnable:  new Signal(true),
    },
    fx: {
      chorusEnable:  new Signal(false),    // matches firmware defaults
      chorusVoices:  new Signal(3),
      reverbEnable:  new Signal(false),
      reverbSize:    new Signal(0.6),
      reverbDamping: new Signal(0.5),
      reverbReturn:  new Signal(0.6),
    },
    synthBus: {
      // Defaults match main.cpp cold-boot (g_synthBusVolume=0.8, g_synthBusOn=true).
      volume: new Signal(0.8),
      on:     new Signal(true),
    },
    synthSlot: {
      // Defaults: slot 0 active (Dexed audible at boot in main.cpp).
      // 8 slots match the firmware's SynthSwitcher::kMaxSlots. Metadata
      // fills via the /synth/slots reply on connect; until then the
      // labels fall back to "Slot N".
      active: new Signal(0),
      slots:  new Signal<SynthSlotMeta[]>(
        Array.from({ length: 8 }, (_, i) => ({
          id: 'unknown',
          displayName: `Slot ${i}`,
        })),
      ),
    },
    sampler: {
      // Defaults match firmware MultisampleSlot cold-boot. Bank info
      // lands via /snapshot or /synth/sampler/info on connect.
      on:                new Signal(true),
      volume:            new Signal(0.75),
      midiChannel:       new Signal(0),
      bankName:          new Signal(''),
      numSamples:        new Signal(0),
      numReleaseSamples: new Signal(0),
    },
    looper: {
      // Defaults match main.cpp cold-boot (g_looperSource=0, g_looperLevel=1.0,
      // Looper::state()=Idle, length=0). /snapshot echoes overwrite on connect.
      source:      new Signal(0),
      level:       new Signal(1.0),
      transport:   new Signal('idle'),
      length:      new Signal(0),
      lengthBeats: new Signal(0),
      quantize:    new Signal(false),
      armed:       new Signal(0),
      cued:        new Signal(0),
      clockFollow: new Signal(true),
      recordedBpm: new Signal(0),
    },
    clock: {
      // Defaults match tdsp::Clock cold-boot: External source, 120 BPM
      // placeholder (overwritten by first /clock/bpm echo or snapshot),
      // not running until transport fires, 4/4.
      source:       new Signal<'ext' | 'int'>('ext'),
      bpm:          new Signal(120),
      running:      new Signal(false),
      beatsPerBar:  new Signal(4),
      // Matches main.cpp cold-boot (g_metroOn=false, g_metroLevel=0.4).
      metroOn:      new Signal(false),
      metroLevel:   new Signal(0.4),
    },
    beats: createBeatsState(),
    arp: {
      // Defaults match tdsp::ArpFilter cold-boot: bypass enabled, classic
      // up-pattern at 1/16, one octave, 50% gate, straight timing, source
      // velocity passthrough, full-step mask, mono output on ch 1. The
      // first /snapshot echo will overwrite these on connect anyway.
      on:             new Signal(false),
      pattern:        new Signal(0),         // PatUp
      rate:           new Signal(11),        // Rate_1_16
      gate:           new Signal(0.5),
      swing:          new Signal(0.5),
      octaveRange:    new Signal(1),
      octaveMode:     new Signal(0),         // OctUp
      latch:          new Signal(false),
      hold:           new Signal(false),
      velMode:        new Signal(0),         // VelFromSource
      velFixed:       new Signal(100),
      velAccent:      new Signal(127),
      stepMask:       new Signal(-1),        // 0xFFFFFFFF (all on)
      stepLength:     new Signal(16),
      mpeMode:        new Signal(0),         // MpeMono
      outputChannel:  new Signal(1),
      scatterBase:    new Signal(2),
      scatterCount:   new Signal(4),
      scale:          new Signal(0),         // Off
      scaleRoot:      new Signal(0),
      transpose:      new Signal(0),
      repeat:         new Signal(1),
      activePresetId: new Signal(''),
    },
    connected: new Signal(false),
    metersOn: new Signal(true),   // on by default; connect() re-subscribes
    selectedChannel: new Signal(0),
    mode: new Signal<'engineer' | 'musician'>('engineer'),
    activeWorkspace: new Signal<'mix' | 'play' | 'tune' | 'fx' | 'setup'>('mix'),
    activePlayTab:   new Signal<'synths' | 'arp' | 'beats' | 'loop'>('synths'),
    activeFxTab:     new Signal<'busfx' | 'processing' | 'spectrum'>('busfx'),
    activeSetupTab:  new Signal<'codec' | 'clock' | 'rawosc' | 'serial'>('codec'),
  };
}

const BEATS_TRACK_DEFS: Array<{ name: string; isSample: boolean; defaultSample: string }> = [
  { name: 'Kick',  isSample: false, defaultSample: '' },
  { name: 'Snare', isSample: false, defaultSample: '' },
  { name: 'Hat',   isSample: true,  defaultSample: 'HAT.WAV' },
  { name: 'Perc',  isSample: true,  defaultSample: 'CLAP.WAV' },
];

// Default demo pattern mirrors the firmware's setup() preload so the
// grid renders correctly on the very first frame (before /snapshot's
// echoes land). Four-on-the-floor kick + backbeat snare + offbeat hat.
const BEATS_DEFAULT_PATTERN: boolean[][] = [
  /* Kick  */ [true, false,false,false, true, false,false,false, true, false,false,false, true, false,false,false],
  /* Snare */ [false,false,false,false, true, false,false,false, false,false,false,false, true, false,false,false],
  /* Hat   */ [false,false,true, false, false,false,true, false, false,false,true, false, false,false,true, false],
  /* Perc  */ [false,false,false,false, false,false,false,false, false,false,false,false, false,false,false,false],
];

function createBeatsState(): BeatsState {
  const tracks: BeatsTrackState[] = [];
  for (let t = 0; t < BEATS_TRACK_COUNT; t++) {
    const def = BEATS_TRACK_DEFS[t];
    const stepsOn:  Signal<boolean>[] = [];
    const stepsVel: Signal<number>[]  = [];
    for (let s = 0; s < BEATS_STEP_COUNT; s++) {
      stepsOn.push (new Signal(BEATS_DEFAULT_PATTERN[t]?.[s] ?? false));
      stepsVel.push(new Signal(100 / 127));  // matches firmware default vel=100
    }
    tracks.push({
      name: def.name,
      isSample: def.isSample,
      muted:  new Signal(false),
      sample: new Signal(def.defaultSample),
      stepsOn,
      stepsVel,
    });
  }
  return {
    running:     new Signal(false),
    bpm:         new Signal(120),
    swing:       new Signal(0),
    volume:      new Signal(0.7),     // matches firmware default
    cursor:      new Signal(-1),
    sdReady:     new Signal(false),
    clockSource: new Signal<'internal' | 'external'>('internal'),
    tracks,
  };
}

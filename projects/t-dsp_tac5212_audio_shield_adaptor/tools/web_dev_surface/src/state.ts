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
  peak: Signal<number>; // 0..1 from /meters/input blob
  rms: Signal<number>;
  name: Signal<string>;
}

export interface BusState {
  fader: Signal<number>;
  on: Signal<boolean>;
  // Stereo meters for the main bus — populated from /meters/output
  // blobs (2 pairs: L peak/rms, R peak/rms).
  peakL: Signal<number>;
  rmsL: Signal<number>;
  peakR: Signal<number>;
  rmsR: Signal<number>;
}

export interface MixerState {
  channels: ChannelState[];
  main: BusState;
  connected: Signal<boolean>;
  metersOn: Signal<boolean>;
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
];

export function createMixerState(channelCount: number): MixerState {
  const channels: ChannelState[] = [];
  for (let i = 0; i < channelCount; i++) {
    const defaultName =
      DEFAULT_CHANNEL_NAMES[i] ?? `Ch ${String(i + 1).padStart(2, '0')}`;
    channels.push({
      fader: new Signal(1.0),  // matches firmware default (MixerModel::reset)
      on: new Signal(true),
      solo: new Signal(false),
      pan: new Signal(0.5),
      peak: new Signal(0),
      rms: new Signal(0),
      name: new Signal(defaultName),
    });
  }

  return {
    channels,
    main: {
      fader: new Signal(0.75),
      on: new Signal(true),
      peakL: new Signal(0),
      rmsL: new Signal(0),
      peakR: new Signal(0),
      rmsR: new Signal(0),
    },
    connected: new Signal(false),
    metersOn: new Signal(false),
  };
}

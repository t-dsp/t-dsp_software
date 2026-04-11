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
}

export interface MixerState {
  channels: ChannelState[];
  main: BusState;
  connected: Signal<boolean>;
  metersOn: Signal<boolean>;
}

export function createMixerState(channelCount: number): MixerState {
  const channels: ChannelState[] = [];
  for (let i = 0; i < channelCount; i++) {
    channels.push({
      fader: new Signal(0),
      on: new Signal(true),
      solo: new Signal(false),
      pan: new Signal(0.5),
      peak: new Signal(0),
      rms: new Signal(0),
      name: new Signal(`Ch ${String(i + 1).padStart(2, '0')}`),
    });
  }

  return {
    channels,
    main: {
      fader: new Signal(0.75),
      on: new Signal(true),
    },
    connected: new Signal(false),
    metersOn: new Signal(false),
  };
}

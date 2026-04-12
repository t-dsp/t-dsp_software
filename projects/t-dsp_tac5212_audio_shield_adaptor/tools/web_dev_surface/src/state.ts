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
  // (/usb/cap/hostvol/value f, /usb/cap/hostvol/mute i). The web surface
  // displays this as a read-only "CAP HOST" strip — there's no control
  // path back: only Windows owns this slider.
  captureHostvolValue: Signal<number>;
  captureHostvolMute: Signal<boolean>;
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
    // Firmware defaults odd channels (1, 3, 5) to link=true. Client
    // matches so the UI is correctly slave-disabled before the first
    // echo arrives.
    const defaultLink = i === 0 || i === 2 || i === 4;
    channels.push({
      fader: new Signal(1.0),  // matches firmware default (MixerModel::reset)
      on: new Signal(true),
      solo: new Signal(false),
      pan: new Signal(0.5),
      link: new Signal(defaultLink),
      peak: new Signal(0),
      rms: new Signal(0),
      name: new Signal(defaultName),
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
    },
    connected: new Signal(false),
    metersOn: new Signal(false),
  };
}

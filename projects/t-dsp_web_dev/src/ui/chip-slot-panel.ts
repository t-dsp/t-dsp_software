// Chip slot panel — config UI for slot 7 (NES/Gameboy chiptune).
//
// New panel built for the slot-architecture rebuild. Layout follows
// sampler-panel.ts / mpe-slot-panel.ts: ON/OFF header, then a vertical
// stack of labeled rows for the slot housekeeping (volume, MIDI channel)
// and the engine knobs (pulse duties / detune / triangle+noise levels /
// voicing / arpeggio / ADSR).
//
// All controls round-trip through the firmware via OSC; optimistic
// local-signal updates make the UI feel responsive, and incoming
// echoes through dispatcher.handleIncoming() re-apply (idempotent
// when the value matches).
//
// The legacy chip-panel.ts (richer — preset grid, midi-auto follow) is
// left in place untouched per the parallel-agent contract. Once the
// user confirms this slim replacement is what they want, the legacy
// file can be deleted; if presets are missed, port them in here
// following the same row builders.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';

const DUTY_LABELS    = ['12.5%', '25%', '50%', '75%'] as const;
const VOICING_LABELS = ['Unison', 'Octave', '5th', '3rd'] as const;
const ARP_LABELS     = ['Off', 'Up', 'Down', 'Random'] as const;

export function chipSlotPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'chip-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  state.chip.on.subscribe((on) => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute Chip output' : 'Click to un-mute Chip output';
  });
  onBtn.addEventListener('click', () => {
    dispatcher.setChipOn(!state.chip.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Chip';
  onRow.append(onBtn, onLabel);

  // ---- Volume slider --------------------------------------------------
  const volRow = buildSliderRow(
    'Volume',
    state.chip.volume,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setChipVolume(v),
  );

  // ---- MIDI channel selector ------------------------------------------
  const midiRow = document.createElement('div');
  midiRow.className = 'sampler-control-row';
  const midiLabel = document.createElement('label');
  midiLabel.textContent = 'MIDI';
  const midiSelect = document.createElement('select');
  midiSelect.className = 'sampler-midi-channel';
  const omniOpt = document.createElement('option');
  omniOpt.value = '0';
  omniOpt.textContent = 'Omni (all channels)';
  midiSelect.appendChild(omniOpt);
  for (let ch = 1; ch <= 16; ++ch) {
    const opt = document.createElement('option');
    opt.value = String(ch);
    opt.textContent = `Channel ${ch}`;
    midiSelect.appendChild(opt);
  }
  state.chip.midiChannel.subscribe((ch) => { midiSelect.value = String(ch); });
  midiSelect.addEventListener('change', () => {
    dispatcher.setChipMidiChannel(parseInt(midiSelect.value, 10));
  });
  midiRow.append(midiLabel, midiSelect);

  // ---- Pulse 1 / Pulse 2 duty (segmented) -----------------------------
  const pulse1Row = buildSegmentedRow(
    'P1 Duty', state.chip.pulse1Duty, DUTY_LABELS,
    (i) => dispatcher.setChipPulse1Duty(i),
  );
  const pulse2Row = buildSegmentedRow(
    'P2 Duty', state.chip.pulse2Duty, DUTY_LABELS,
    (i) => dispatcher.setChipPulse2Duty(i),
  );

  // ---- Pulse 2 detune (cents) -----------------------------------------
  const detuneRow = buildSliderRow(
    'P2 Detune',
    state.chip.pulse2Detune,
    -100, 100,
    (v) => `${v.toFixed(1)} ¢`,
    (v) => dispatcher.setChipPulse2Detune(v),
  );

  // ---- Triangle + noise mix levels ------------------------------------
  const triRow = buildSliderRow(
    'Tri Level',
    state.chip.triLevel,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setChipTriLevel(v),
  );
  const noiseRow = buildSliderRow(
    'Noise',
    state.chip.noiseLevel,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setChipNoiseLevel(v),
  );

  // ---- Voicing (segmented) --------------------------------------------
  const voicingRow = buildSegmentedRow(
    'Voicing', state.chip.voicing, VOICING_LABELS,
    (i) => dispatcher.setChipVoicing(i),
  );

  // ---- Arpeggio mode + rate -------------------------------------------
  const arpModeRow = buildSegmentedRow(
    'Arp', state.chip.arpeggio, ARP_LABELS,
    (i) => dispatcher.setChipArpeggio(i),
  );
  const arpRateRow = buildSliderRow(
    'Arp Rate',
    state.chip.arpRateHz,
    1, 40,
    (v) => `${v.toFixed(1)} Hz`,
    (v) => dispatcher.setChipArpRate(v),
  );

  // ---- ADSR envelope --------------------------------------------------
  const attackRow = buildSliderRow(
    'Attack',
    state.chip.attack,
    0.0005, 1.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setChipAttack(v),
    { log: true },
  );
  const decayRow = buildSliderRow(
    'Decay',
    state.chip.decay,
    0.005, 2.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setChipDecay(v),
    { log: true },
  );
  const sustainRow = buildSliderRow(
    'Sustain',
    state.chip.sustain,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setChipSustain(v),
  );
  const releaseRow = buildSliderRow(
    'Release',
    state.chip.release,
    0.005, 2.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setChipRelease(v),
    { log: true },
  );

  root.append(
    onRow,
    volRow,
    midiRow,
    pulse1Row,
    pulse2Row,
    detuneRow,
    triRow,
    noiseRow,
    voicingRow,
    arpModeRow,
    arpRateRow,
    attackRow,
    decayRow,
    sustainRow,
    releaseRow,
  );
  return root;
}

// ---- helpers --------------------------------------------------------

interface SliderRowOpts {
  log?: boolean;
}

function buildSliderRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  format: (v: number) => string,
  onChange: (v: number) => void,
  opts: SliderRowOpts = {},
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'sampler-control-row';

  const label = document.createElement('label');
  label.textContent = labelText;

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.className = 'sampler-volume-slider';

  const logMap = opts.log === true;
  if (logMap) {
    slider.min = '0';
    slider.max = '1000';
    slider.step = '1';
  } else {
    slider.min = String(min);
    slider.max = String(max);
    slider.step = String((max - min) / 1000);
  }

  const readout = document.createElement('span');
  readout.className = 'sampler-volume-readout';

  const toSlider = (value: number): number => {
    if (!logMap) return value;
    const lo = Math.log(min);
    const hi = Math.log(max);
    const v  = Math.log(Math.max(min, Math.min(max, value)));
    return ((v - lo) / (hi - lo)) * 1000;
  };
  const fromSlider = (pos: number): number => {
    if (!logMap) return pos;
    const lo = Math.log(min);
    const hi = Math.log(max);
    return Math.exp(lo + (pos / 1000) * (hi - lo));
  };

  signal.subscribe((v) => {
    slider.value = String(toSlider(v));
    readout.textContent = format(v);
  });
  slider.addEventListener('input', () => {
    onChange(fromSlider(parseFloat(slider.value)));
  });

  row.append(label, slider, readout);
  return row;
}

function buildSegmentedRow(
  labelText: string,
  signal: Signal<number>,
  labels: readonly string[],
  onClick: (idx: number) => void,
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'sampler-control-row';

  const label = document.createElement('label');
  label.textContent = labelText;

  const group = document.createElement('div');
  group.className = 'mpe-segmented';
  const btns: HTMLButtonElement[] = [];
  for (let i = 0; i < labels.length; ++i) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = labels[i];
    b.addEventListener('click', () => onClick(i));
    btns.push(b);
    group.appendChild(b);
  }
  signal.subscribe((v) => {
    btns.forEach((b, i) => b.classList.toggle('active', i === v));
  });
  row.append(label, group);
  return row;
}

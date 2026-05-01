// Plaits slot panel — config UI for slot 2 (Plaits-inspired macro
// oscillator).
//
// Layout follows mpe-slot-panel.ts: ON/OFF header, slot housekeeping
// (volume, MPE master toggle), then the five engine controls — model
// picker (5 segmented tiles), HARMONICS, TIMBRE, MORPH, DECAY,
// resonance. No preset grid in this first cut — Plaits' identity is
// the macros, and a starter set of presets can be added later if the
// user finds themselves dialing the same destinations repeatedly.
//
// All controls round-trip through the firmware via OSC; optimistic
// local-signal updates make the UI feel responsive, and incoming
// echoes through dispatcher.handleIncoming() re-apply (idempotent
// when the value matches).

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';

const MODEL_LABELS = ['VA Saw', 'Square', 'FM Sine', 'Hollow', 'Pulse'] as const;

// HARMONICS snap labels — match PlaitsSink::harmonicsToSemitones()
// anchors at 0, 0.25, 0.5, 0.75, 1.0 → unison, +5th, +oct, +oct+5th, +2oct.
const HARMONICS_SNAPS = [
  { v: 0.0,  label: 'Unison' },
  { v: 0.25, label: '+5th'   },
  { v: 0.5,  label: '+Oct'   },
  { v: 0.75, label: '+Oct+5' },
  { v: 1.0,  label: '+2Oct'  },
] as const;

export function plaitsSlotPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  // Reuse .mpe-panel selector for shared styling (segmented controls,
  // sliders). Plaits-specific overrides hang off .plaits-panel.
  root.className = 'mpe-panel plaits-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  state.plaits.on.subscribe((on) => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute Plaits' : 'Click to un-mute Plaits';
  });
  onBtn.addEventListener('click', () => {
    dispatcher.setPlaitsOn(!state.plaits.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Plaits';
  onRow.append(onBtn, onLabel);

  // ---- Volume slider --------------------------------------------------
  const volRow = buildSliderRow(
    'Volume',
    state.plaits.volume,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setPlaitsVolume(v),
  );

  // ---- MPE Mode toggle + master channel picker -----------------------
  // Same pattern as MPE: Off → master=0 (omni / plain-keyboard friendly).
  // On → master=N (default 1, standard MPE behaviour).
  const mpeModeRow = document.createElement('div');
  mpeModeRow.className = 'sampler-control-row';
  const mpeModeLabel = document.createElement('label');
  mpeModeLabel.textContent = 'MPE Mode';
  mpeModeLabel.title = 'On: standard MPE (per-finger expression on member channels). Off: every channel allocates voices — works with any keyboard, including the on-screen one.';
  const mpeModeToggle = document.createElement('input');
  mpeModeToggle.type = 'checkbox';
  mpeModeToggle.className = 'sampler-toggle';
  mpeModeRow.append(mpeModeLabel, mpeModeToggle);

  const masterRow = document.createElement('div');
  masterRow.className = 'sampler-control-row';
  const masterLabel = document.createElement('label');
  masterLabel.textContent = 'Master ch';
  masterLabel.title = 'MPE master channel — global mod wheel / sustain go here, notes on this channel are ignored.';
  const masterSelect = document.createElement('select');
  masterSelect.className = 'sampler-midi-channel';
  for (let ch = 1; ch <= 16; ++ch) {
    const opt = document.createElement('option');
    opt.value = String(ch);
    opt.textContent = `Channel ${ch}`;
    masterSelect.appendChild(opt);
  }
  masterRow.append(masterLabel, masterSelect);

  let lastMaster = 1;
  state.plaits.masterChannel.subscribe((ch) => {
    const mpeOn = ch !== 0;
    mpeModeToggle.checked = mpeOn;
    masterRow.style.display = mpeOn ? '' : 'none';
    if (mpeOn) {
      lastMaster = ch;
      masterSelect.value = String(ch);
    }
  });
  mpeModeToggle.addEventListener('change', () => {
    dispatcher.setPlaitsMasterChannel(mpeModeToggle.checked ? lastMaster : 0);
  });
  masterSelect.addEventListener('change', () => {
    dispatcher.setPlaitsMasterChannel(parseInt(masterSelect.value, 10));
  });

  // ---- Model picker (5 segmented tiles) ------------------------------
  const modelRow = document.createElement('div');
  modelRow.className = 'sampler-control-row';
  const modelLabel = document.createElement('label');
  modelLabel.textContent = 'Model';
  const modelGroup = document.createElement('div');
  modelGroup.className = 'mpe-segmented';
  const modelBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < MODEL_LABELS.length; ++i) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = MODEL_LABELS[i];
    b.addEventListener('click', () => dispatcher.setPlaitsModel(i));
    modelBtns.push(b);
    modelGroup.appendChild(b);
  }
  state.plaits.model.subscribe((m) => {
    modelBtns.forEach((b, i) => b.classList.toggle('active', i === m));
  });
  modelRow.append(modelLabel, modelGroup);

  // ---- HARMONICS slider with snap-label readout ----------------------
  // Slider is 0..1 like the firmware; readout shows the nearest snap
  // anchor's name so the user sees "+Oct" rather than "0.51".
  const harmonicsRow = buildSliderRow(
    'Harmonics',
    state.plaits.harmonics,
    0, 1,
    (v) => {
      let best: typeof HARMONICS_SNAPS[number] = HARMONICS_SNAPS[0];
      let bestDist = Math.abs(v - best.v);
      for (const s of HARMONICS_SNAPS) {
        const d = Math.abs(v - s.v);
        if (d < bestDist) { best = s; bestDist = d; }
      }
      return best.label;
    },
    (v) => dispatcher.setPlaitsHarmonics(v),
  );

  // ---- TIMBRE slider --------------------------------------------------
  // Maps to filter cutoff (200 Hz..16 kHz exponential) inside the firmware;
  // the readout shows the dialed Hz value so the user has real units.
  const timbreRow = buildSliderRow(
    'Timbre',
    state.plaits.timbre,
    0, 1,
    (v) => `${Math.round(200 * Math.pow(16000 / 200, v))} Hz`,
    (v) => dispatcher.setPlaitsTimbre(v),
  );

  // ---- MORPH slider ---------------------------------------------------
  const morphRow = buildSliderRow(
    'Morph',
    state.plaits.morph,
    0, 1,
    (v) => v < 0.05 ? 'Osc 1' : v > 0.95 ? 'Osc 2' : `${Math.round((1 - v) * 100)}/${Math.round(v * 100)}`,
    (v) => dispatcher.setPlaitsMorph(v),
  );

  // ---- DECAY slider — readout in ms, exponential to match firmware ---
  const decayRow = buildSliderRow(
    'Decay',
    state.plaits.decay,
    0, 1,
    (v) => {
      const ms = 50 * Math.pow(3000 / 50, v);
      return ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s`;
    },
    (v) => dispatcher.setPlaitsDecay(v),
  );

  // ---- Resonance ------------------------------------------------------
  const resoRow = buildSliderRow(
    'Q',
    state.plaits.resonance,
    0.707, 5.0,
    (v) => v.toFixed(2),
    (v) => dispatcher.setPlaitsResonance(v),
  );

  root.append(
    onRow,
    volRow,
    mpeModeRow,
    masterRow,
    modelRow,
    harmonicsRow,
    timbreRow,
    morphRow,
    decayRow,
    resoRow,
  );
  return root;
}

// ---- helpers --------------------------------------------------------

function buildSliderRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  format: (v: number) => string,
  onChange: (v: number) => void,
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'sampler-control-row';

  const label = document.createElement('label');
  label.textContent = labelText;

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.className = 'sampler-volume-slider';
  slider.min = String(min);
  slider.max = String(max);
  slider.step = String((max - min) / 1000);

  const readout = document.createElement('span');
  readout.className = 'sampler-volume-readout';

  signal.subscribe((v) => {
    slider.value = String(v);
    readout.textContent = format(v);
  });
  slider.addEventListener('input', () => {
    onChange(parseFloat(slider.value));
  });

  row.append(label, slider, readout);
  return row;
}

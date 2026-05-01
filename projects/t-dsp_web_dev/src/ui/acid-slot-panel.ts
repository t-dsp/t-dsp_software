// Acid slot panel — config UI for slot 5 (TB-303-style mono bass).
//
// New panel built for the slot-architecture rebuild. Layout follows
// sampler-panel.ts / mpe-slot-panel.ts: ON/OFF header, preset dropdown
// (ported from the legacy acid-panel.ts), then a vertical stack of
// labeled rows for the slot housekeeping (volume, MIDI channel) and
// the engine knobs (waveform, tuning, cutoff, resonance, env mod,
// env decay, amp decay, accent, slide).
//
// All controls round-trip through the firmware via OSC. Optimistic
// local-signal updates make the UI feel responsive; the echoes that
// land back through dispatcher.handleIncoming() re-apply (idempotent
// when the value matches).
//
// The legacy acid-panel.ts (richer grid layout, MIDI Auto checkbox)
// is left in place untouched per the parallel-agent contract. Once
// the user confirms this replacement is what they want, the legacy
// file can be deleted.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../lib/TDspAcid/presets.json';

interface PresetParams {
  waveform: number;
  tuning: number;
  cutoff_hz: number;
  resonance: number;
  env_mod: number;
  env_decay_sec: number;
  amp_decay_sec: number;
  accent: number;
  slide_ms: number;
  volume: number;
}
interface Preset {
  id: string;
  name: string;
  category: string;
  description: string;
  params: PresetParams;
}
interface PresetDoc {
  categories: Record<string, { color: string; label: string }>;
  presets: Preset[];
}
const PRESETS = presetDoc as PresetDoc;

const WAVEFORM_LABELS = ['Saw', 'Square'] as const;

export function acidSlotPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'acid-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  state.acid.on.subscribe((on) => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute Acid output' : 'Click to un-mute Acid output';
  });
  onBtn.addEventListener('click', () => {
    dispatcher.setAcidOn(!state.acid.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Acid';
  onRow.append(onBtn, onLabel);

  // ---- Preset dropdown -----------------------------------------------
  // Ports the curated TB-303 preset bank from lib/TDspAcid/presets.json
  // (Classic 303, Squelchy, Deep Acid, Rave Hoover, ...). Selecting a
  // preset pushes every parameter through the dispatcher so firmware
  // re-applies waveform, tuning, cutoff, resonance, env mod / decay,
  // amp decay, accent, slide, and volume in one shot. The active
  // preset is tracked in state.acid.activePresetId so a reconnect
  // restores it; tweaking individual knobs leaves the selection alone
  // (matches legacy acid-panel.ts behaviour). Categories are surfaced
  // as <optgroup>s so the semantic grouping (classic / squelch / rave
  // / dub) survives the dropdown.
  const presetRow = document.createElement('div');
  presetRow.className = 'sampler-control-row';
  const presetLabel = document.createElement('label');
  presetLabel.textContent = 'Preset';
  const presetSelect = document.createElement('select');
  presetSelect.className = 'sampler-midi-channel';

  const noneOpt = document.createElement('option');
  noneOpt.value = '';
  noneOpt.textContent = '— Select preset —';
  presetSelect.appendChild(noneOpt);
  for (const [catKey, cat] of Object.entries(PRESETS.categories)) {
    const group = document.createElement('optgroup');
    group.label = cat.label;
    for (const p of PRESETS.presets) {
      if (p.category !== catKey) continue;
      const opt = document.createElement('option');
      opt.value = p.id;
      opt.textContent = p.name;
      opt.title = p.description;
      group.appendChild(opt);
    }
    if (group.childElementCount > 0) presetSelect.appendChild(group);
  }
  state.acid.activePresetId.subscribe((id) => { presetSelect.value = id; });
  presetSelect.addEventListener('change', () => {
    const preset = PRESETS.presets.find((p) => p.id === presetSelect.value);
    if (!preset) {
      state.acid.activePresetId.set('');
      return;
    }
    const p = preset.params;
    dispatcher.setAcidWaveform(p.waveform);
    dispatcher.setAcidTuning(p.tuning);
    dispatcher.setAcidCutoff(p.cutoff_hz);
    dispatcher.setAcidResonance(p.resonance);
    dispatcher.setAcidEnvMod(p.env_mod);
    dispatcher.setAcidEnvDecay(p.env_decay_sec);
    dispatcher.setAcidAmpDecay(p.amp_decay_sec);
    dispatcher.setAcidAccent(p.accent);
    dispatcher.setAcidSlide(p.slide_ms);
    dispatcher.setAcidVolume(p.volume);
    state.acid.activePresetId.set(preset.id);
  });
  presetRow.append(presetLabel, presetSelect);

  // ---- Volume slider --------------------------------------------------
  const volRow = buildSliderRow(
    'Volume',
    state.acid.volume,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setAcidVolume(v),
  );

  // ---- MIDI channel selector -----------------------------------------
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
  state.acid.midiChannel.subscribe((ch) => { midiSelect.value = String(ch); });
  midiSelect.addEventListener('change', () => {
    dispatcher.setAcidMidiChannel(parseInt(midiSelect.value, 10));
  });
  midiRow.append(midiLabel, midiSelect);

  // ---- Waveform picker (segmented Saw / Square) ----------------------
  const waveRow = document.createElement('div');
  waveRow.className = 'sampler-control-row';
  const waveLabel = document.createElement('label');
  waveLabel.textContent = 'Wave';
  const waveGroup = document.createElement('div');
  waveGroup.className = 'mpe-segmented';
  const waveBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < WAVEFORM_LABELS.length; ++i) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = WAVEFORM_LABELS[i];
    b.addEventListener('click', () => dispatcher.setAcidWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.acid.waveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });
  waveRow.append(waveLabel, waveGroup);

  // ---- Engine knobs ---------------------------------------------------
  const tuningRow = buildSliderRow(
    'Tuning',
    state.acid.tuning,
    -24, 24,
    (v) => `${v >= 0 ? '+' : ''}${v} st`,
    (v) => dispatcher.setAcidTuning(Math.round(v)),
  );
  const cutoffRow = buildSliderRow(
    'Cutoff',
    state.acid.cutoffHz,
    50, 16000,
    (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setAcidCutoff(v),
    { log: true },
  );
  const resoRow = buildSliderRow(
    'Q',
    state.acid.resonance,
    0.707, 5.0,
    (v) => v.toFixed(2),
    (v) => dispatcher.setAcidResonance(v),
  );
  const envModRow = buildSliderRow(
    'Env Mod',
    state.acid.envMod,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setAcidEnvMod(v),
  );
  const envDecayRow = buildSliderRow(
    'Env Decay',
    state.acid.envDecay,
    0.01, 3.0,
    (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setAcidEnvDecay(v),
    { log: true },
  );
  const ampDecayRow = buildSliderRow(
    'Amp Decay',
    state.acid.ampDecay,
    0.01, 3.0,
    (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setAcidAmpDecay(v),
    { log: true },
  );
  const accentRow = buildSliderRow(
    'Accent',
    state.acid.accent,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setAcidAccent(v),
  );
  const slideRow = buildSliderRow(
    'Slide',
    state.acid.slideMs,
    0, 500,
    (v) => v < 1 ? 'Off' : `${Math.round(v)} ms`,
    (v) => dispatcher.setAcidSlide(v),
  );

  root.append(
    onRow,
    presetRow,
    volRow,
    midiRow,
    waveRow,
    tuningRow,
    cutoffRow,
    resoRow,
    envModRow,
    envDecayRow,
    ampDecayRow,
    accentRow,
    slideRow,
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
    const lo = Math.log(Math.max(min, 1e-6));
    const hi = Math.log(max);
    const v  = Math.log(Math.max(min, Math.min(max, value)));
    return ((v - lo) / (hi - lo)) * 1000;
  };
  const fromSlider = (pos: number): number => {
    if (!logMap) return pos;
    const lo = Math.log(Math.max(min, 1e-6));
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

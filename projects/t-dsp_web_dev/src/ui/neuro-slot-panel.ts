// Neuro slot panel — config UI for slot 4 (reese / DnB neuro bass).
//
// New panel built for the slot-architecture rebuild. Layout follows
// mpe-slot-panel.ts / chip-slot-panel.ts: ON/OFF header, preset
// dropdown (ported from the legacy lib/TDspNeuro/presets.json), then
// a vertical stack of labeled rows for the slot housekeeping (volume,
// MIDI channel) and the engine knobs (oscillator balance, filter
// cutoff/Q, ADSR-equivalent A/R, LFO, portamento).
//
// All controls round-trip through the firmware via OSC; optimistic
// local-signal updates make the UI feel responsive, and incoming
// echoes through dispatcher.handleIncoming() re-apply (idempotent
// when the value matches).
//
// The legacy neuro-panel.ts (richer grid layout + stink-chain section)
// is left in place untouched per the parallel-agent contract. Once the
// user confirms this slim replacement is what they want, the legacy
// file can be deleted; if the stink chain is missed, port it back in
// here following the same sub-component shape.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../lib/TDspNeuro/presets.json';

// Preset shape — mirrors lib/TDspNeuro/presets.json. The stink_* fields
// are present in the JSON for legacy compatibility but the slot rebuild
// is intentionally core-only, so the load handler ignores them. When the
// stink chain comes back as a follow-up, those fields can start driving
// dispatcher.setNeuroStink* calls without touching this file's schema.
interface PresetParams {
  volume: number;
  attack_sec: number;
  release_sec: number;
  detune_cents: number;
  sub_level: number;
  osc3_level: number;
  filter_cutoff_hz: number;
  filter_resonance: number;
  lfo_rate_hz: number;
  lfo_depth: number;
  lfo_dest: number;
  lfo_waveform: number;
  portamento_ms: number;
}
interface Preset {
  id: string;
  name: string;
  category: string;
  description: string;
  params: PresetParams;  // stink_* fields ignored at load — see header
}
interface PresetDoc {
  categories: Record<string, { color: string; label: string }>;
  presets: Preset[];
}
const PRESETS = presetDoc as unknown as PresetDoc;

const LFO_DEST_LABELS = ['Off', 'Cutoff', 'Pitch', 'Amp'] as const;
const LFO_WAVE_LABELS = ['Sine', 'Tri', 'Saw', 'Square'] as const;

export function neuroSlotPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'neuro-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  state.neuro.on.subscribe((on) => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute Neuro output' : 'Click to un-mute Neuro output';
  });
  onBtn.addEventListener('click', () => {
    dispatcher.setNeuroOn(!state.neuro.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Neuro';
  onRow.append(onBtn, onLabel);

  // ---- Preset dropdown -----------------------------------------------
  // Curated reese / neuro bass voicings ported from the legacy
  // lib/TDspNeuro/presets.json. Selecting a preset pushes every CORE
  // parameter through the dispatcher so firmware re-applies the
  // oscillator balance, filter, envelope, LFO, and portamento in one
  // shot. Stink-chain fields in the JSON are skipped — that part of
  // the engine isn't wired in this slot rebuild — so stink-heavy
  // presets ("Full Face", "Talkbox", "Metallic") will sound cleaner
  // than the legacy. Once the stink chain comes back as a follow-up,
  // adding the dispatcher.setNeuroStink* calls here is a one-line edit
  // per field.
  //
  // state.neuro.activePresetId tracks the active selection so a
  // reconnect highlights the right preset; tweaking an individual knob
  // afterwards leaves the selection alone (matches the chip-panel
  // behaviour).
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
  state.neuro.activePresetId.subscribe((id) => { presetSelect.value = id; });
  presetSelect.addEventListener('change', () => {
    const preset = PRESETS.presets.find((p) => p.id === presetSelect.value);
    if (!preset) {
      state.neuro.activePresetId.set('');
      return;
    }
    const p = preset.params;
    dispatcher.setNeuroVolume         (p.volume);
    dispatcher.setNeuroAttack         (p.attack_sec);
    dispatcher.setNeuroRelease        (p.release_sec);
    dispatcher.setNeuroDetune         (p.detune_cents);
    dispatcher.setNeuroSub            (p.sub_level);
    dispatcher.setNeuroOsc3           (p.osc3_level);
    dispatcher.setNeuroFilterCutoff   (p.filter_cutoff_hz);
    dispatcher.setNeuroFilterResonance(p.filter_resonance);
    dispatcher.setNeuroLfoRate        (p.lfo_rate_hz);
    dispatcher.setNeuroLfoDepth       (p.lfo_depth);
    dispatcher.setNeuroLfoDest        (p.lfo_dest);
    dispatcher.setNeuroLfoWaveform    (p.lfo_waveform);
    dispatcher.setNeuroPortamento     (p.portamento_ms);
    state.neuro.activePresetId.set(preset.id);
  });
  presetRow.append(presetLabel, presetSelect);

  // ---- Volume slider --------------------------------------------------
  const volRow = buildSliderRow(
    'Volume',
    state.neuro.volume,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroVolume(v),
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
  state.neuro.midiChannel.subscribe((ch) => { midiSelect.value = String(ch); });
  midiSelect.addEventListener('change', () => {
    dispatcher.setNeuroMidiChannel(parseInt(midiSelect.value, 10));
  });
  midiRow.append(midiLabel, midiSelect);

  // ---- Oscillator balance --------------------------------------------
  // Detune spread (0..50 cents — the reese beat lives here), sub sine
  // level, and middle-saw level. Three parallel sliders.
  const detuneRow = buildSliderRow(
    'Detune',
    state.neuro.detuneCents,
    0, 50,
    (v) => `${v.toFixed(1)}¢`,
    (v) => dispatcher.setNeuroDetune(v),
  );
  const subRow = buildSliderRow(
    'Sub',
    state.neuro.subLevel,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroSub(v),
  );
  const osc3Row = buildSliderRow(
    'Osc3',
    state.neuro.osc3Level,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroOsc3(v),
  );

  // ---- Envelope (attack / release) ------------------------------------
  const attackRow = buildSliderRow(
    'Attack',
    state.neuro.attack,
    0.0005, 2.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setNeuroAttack(v),
    { log: true },
  );
  const releaseRow = buildSliderRow(
    'Release',
    state.neuro.release,
    0.001, 3.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setNeuroRelease(v),
    { log: true },
  );

  // ---- Filter (cutoff / resonance) ------------------------------------
  const cutoffRow = buildSliderRow(
    'Cutoff',
    state.neuro.filterCutoffHz,
    20, 20000,
    (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setNeuroFilterCutoff(v),
    { log: true },
  );
  const resoRow = buildSliderRow(
    'Q',
    state.neuro.filterResonance,
    0.707, 5.0,
    (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroFilterResonance(v),
  );

  // ---- LFO row (rate + depth + dest + waveform) -----------------------
  const lfoBlock = buildLfoBlock(state, dispatcher);

  // ---- Portamento (mono-bass glide) -----------------------------------
  // 0 = snap, otherwise milliseconds of glide. Log-scaled so 0..200 ms
  // (the musically useful zone) gets generous slider real estate.
  const portRow = buildSliderRow(
    'Glide',
    state.neuro.portamentoMs,
    0, 2000,
    (v) => v < 1 ? 'snap' : `${Math.round(v)} ms`,
    (v) => dispatcher.setNeuroPortamento(v),
  );

  root.append(
    onRow,
    presetRow,
    volRow,
    midiRow,
    detuneRow,
    subRow,
    osc3Row,
    cutoffRow,
    resoRow,
    attackRow,
    releaseRow,
    lfoBlock,
    portRow,
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

  const logMap = opts.log === true && min > 0;
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

function buildLfoBlock(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'mpe-lfo-block';

  // Rate slider (0..20 Hz, linear).
  wrap.append(buildSliderRow(
    'LFO rate',
    state.neuro.lfoRate,
    0, 20,
    (v) => `${v.toFixed(1)} Hz`,
    (v) => dispatcher.setNeuroLfoRate(v),
  ));
  // Depth slider (0..1, linear).
  wrap.append(buildSliderRow(
    'LFO depth',
    state.neuro.lfoDepth,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroLfoDepth(v),
  ));

  // Destination — segmented picker.
  const destRow = document.createElement('div');
  destRow.className = 'sampler-control-row';
  const destLabel = document.createElement('label');
  destLabel.textContent = 'LFO dest';
  const destGroup = document.createElement('div');
  destGroup.className = 'mpe-segmented';
  const destBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_DEST_LABELS.length; ++i) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_DEST_LABELS[i];
    b.addEventListener('click', () => dispatcher.setNeuroLfoDest(i));
    destBtns.push(b);
    destGroup.appendChild(b);
  }
  state.neuro.lfoDest.subscribe((d) => {
    destBtns.forEach((b, i) => b.classList.toggle('active', i === d));
  });
  destRow.append(destLabel, destGroup);
  wrap.append(destRow);

  // Waveform — segmented picker.
  const waveRow = document.createElement('div');
  waveRow.className = 'sampler-control-row';
  const waveLabel = document.createElement('label');
  waveLabel.textContent = 'LFO wave';
  const waveGroup = document.createElement('div');
  waveGroup.className = 'mpe-segmented';
  const waveBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_WAVE_LABELS.length; ++i) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_WAVE_LABELS[i];
    b.addEventListener('click', () => dispatcher.setNeuroLfoWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.neuro.lfoWaveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });
  waveRow.append(waveLabel, waveGroup);
  wrap.append(waveRow);

  return wrap;
}

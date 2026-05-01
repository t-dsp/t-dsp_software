// Acid (TB-303 style) sub-tab — mono 1-osc bass with filter envelope,
// accent and slide. Reuses the .mpe-* CSS classes for consistency.

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

export interface AcidPanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function acidPanel(state: MixerState, dispatcher: Dispatcher): AcidPanel {
  const root = document.createElement('div');
  root.className = 'mpe-panel';

  root.append(
    buildOnRow(state, dispatcher),
    buildPresetGrid(state, dispatcher),
    buildControls(state, dispatcher),
  );

  return { element: root, setActive: () => { /* mono, no telemetry */ } };
}

function buildOnRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'synth-on-row';
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'synth-on-btn';
  state.acid.on.subscribe((on) => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
  });
  btn.addEventListener('click', () => {
    if (state.acid.midiAuto.get()) state.acid.midiAuto.set(false);
    dispatcher.setAcidOn(!state.acid.on.get());
  });
  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'Acid';
  const autoWrap = document.createElement('label');
  autoWrap.className = 'synth-auto';
  const autoBox = document.createElement('input');
  autoBox.type = 'checkbox';
  state.acid.midiAuto.subscribe((a) => { autoBox.checked = a; });
  autoBox.addEventListener('change', () => state.acid.midiAuto.set(autoBox.checked));
  const autoText = document.createElement('span');
  autoText.textContent = 'MIDI Auto';
  autoWrap.append(autoBox, autoText);
  row.append(btn, label, autoWrap);
  return row;
}

function buildPresetGrid(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-presets';
  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Presets';
  section.appendChild(title);

  const filterRow = document.createElement('div');
  filterRow.className = 'mpe-preset-filter';
  let activeCategory = 'all';
  const filterBtns: HTMLButtonElement[] = [];
  const keys = ['all', ...Object.keys(PRESETS.categories)];
  for (const key of keys) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-preset-filter-btn';
    if (key === 'all') b.textContent = 'All';
    else {
      const cat = PRESETS.categories[key];
      b.textContent = cat.label;
      b.style.setProperty('--cat-color', cat.color);
    }
    if (key === activeCategory) b.classList.add('active');
    b.addEventListener('click', () => {
      activeCategory = key;
      for (const f of filterBtns) f.classList.toggle('active', f === b);
      renderGrid();
    });
    filterBtns.push(b);
    filterRow.appendChild(b);
  }
  section.appendChild(filterRow);

  const grid = document.createElement('div');
  grid.className = 'mpe-preset-grid';
  section.appendChild(grid);

  const renderGrid = (): void => {
    grid.innerHTML = '';
    const matches = activeCategory === 'all'
      ? PRESETS.presets
      : PRESETS.presets.filter((p) => p.category === activeCategory);
    for (const preset of matches) {
      grid.appendChild(buildPresetCard(preset, state, dispatcher));
    }
  };
  renderGrid();
  return section;
}

function buildPresetCard(preset: Preset, state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const card = document.createElement('button');
  card.type = 'button';
  card.className = 'mpe-preset-card';
  const cat = PRESETS.categories[preset.category];
  if (cat) card.style.setProperty('--cat-color', cat.color);
  const name = document.createElement('div');
  name.className = 'mpe-preset-name';
  name.textContent = preset.name;
  card.appendChild(name);
  card.title = preset.description;

  card.addEventListener('click', () => {
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
  state.acid.activePresetId.subscribe((id) => card.classList.toggle('active', id === preset.id));
  return card;
}

function buildControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls';
  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Parameters';
  section.appendChild(title);

  // Waveform picker.
  const wr = document.createElement('div');
  wr.className = 'mpe-row';
  const wl = document.createElement('label'); wl.textContent = 'Wave'; wr.appendChild(wl);
  const wtiles: HTMLButtonElement[] = [];
  for (let i = 0; i < 2; i++) {
    const t = document.createElement('button');
    t.type = 'button';
    t.className = 'mpe-wave-tile';
    t.textContent = WAVEFORM_LABELS[i];
    t.addEventListener('click', () => dispatcher.setAcidWaveform(i));
    wtiles.push(t);
    wr.appendChild(t);
  }
  state.acid.waveform.subscribe((w) => wtiles.forEach((t, i) => t.classList.toggle('active', i === w)));
  section.appendChild(wr);

  section.appendChild(paramRow('Tuning', state.acid.tuning,
    -24, 24, (v) => `${v >= 0 ? '+' : ''}${v} st`,
    (v) => dispatcher.setAcidTuning(Math.round(v))));
  section.appendChild(paramRow('Cutoff', state.acid.cutoffHz,
    50, 16000, (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setAcidCutoff(v), { log: true }));
  section.appendChild(paramRow('Resonance', state.acid.resonance,
    0.707, 5.0, (v) => v.toFixed(2),
    (v) => dispatcher.setAcidResonance(v)));
  section.appendChild(paramRow('Env Mod', state.acid.envMod,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setAcidEnvMod(v)));
  section.appendChild(paramRow('Env Decay', state.acid.envDecay,
    0.01, 3.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setAcidEnvDecay(v), { log: true }));
  section.appendChild(paramRow('Amp Decay', state.acid.ampDecay,
    0.01, 3.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setAcidAmpDecay(v), { log: true }));
  section.appendChild(paramRow('Accent', state.acid.accent,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setAcidAccent(v)));
  section.appendChild(paramRow('Slide', state.acid.slideMs,
    0, 500, (v) => v < 1 ? 'Off' : `${Math.round(v)} ms`,
    (v) => dispatcher.setAcidSlide(v)));
  section.appendChild(paramRow('Volume', state.acid.volume,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setAcidVolume(v)));

  // MIDI channel picker.
  section.appendChild(midiChannelRow(state, dispatcher));

  return section;
}

function paramRow(
  label: string, signal: Signal<number>,
  min: number, max: number,
  format: (v: number) => string,
  onChange: (v: number) => void,
  opts: { log?: boolean } = {},
): HTMLElement {
  const row = document.createElement('div'); row.className = 'mpe-row';
  const lab = document.createElement('label'); lab.textContent = label; row.appendChild(lab);
  const slider = document.createElement('input'); slider.type = 'range';
  const logMap = opts.log === true;
  if (logMap) { slider.min = '0'; slider.max = '1000'; slider.step = '1'; }
  else { slider.min = String(min); slider.max = String(max); slider.step = String((max - min) / 1000); }
  const readout = document.createElement('span'); readout.className = 'mpe-readout';
  const toSlider = (v: number) => {
    if (!logMap) return v;
    const lo = Math.log(min), hi = Math.log(max);
    return ((Math.log(Math.max(min, Math.min(max, v))) - lo) / (hi - lo)) * 1000;
  };
  const fromSlider = (p: number) => {
    if (!logMap) return p;
    const lo = Math.log(min), hi = Math.log(max);
    return Math.exp(lo + (p / 1000) * (hi - lo));
  };
  signal.subscribe((v) => {
    slider.value = String(toSlider(v));
    readout.textContent = format(v);
  });
  slider.addEventListener('input', () => onChange(fromSlider(parseFloat(slider.value))));
  row.append(slider, readout);
  return row;
}

function midiChannelRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div'); row.className = 'mpe-row';
  const lab = document.createElement('label'); lab.textContent = 'MIDI Ch'; row.appendChild(lab);
  const pills = document.createElement('div'); pills.className = 'mpe-channel-pills';
  const btns: HTMLButtonElement[] = [];
  for (let ch = 0; ch <= 16; ch++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-channel-pill';
    b.textContent = ch === 0 ? 'Any' : String(ch);
    b.addEventListener('click', () => dispatcher.setAcidMidiChannel(ch));
    btns.push(b); pills.appendChild(b);
  }
  state.acid.midiChannel.subscribe((c) => btns.forEach((b, i) => b.classList.toggle('active', i === c)));
  row.appendChild(pills);
  return row;
}

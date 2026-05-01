// Supersaw (JP-8000 style) sub-tab — 5-voice detuned saw lead.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../lib/TDspSupersaw/presets.json';

interface PresetParams {
  volume: number;
  detune_cents: number;
  mix_center: number;
  cutoff_hz: number;
  resonance: number;
  attack_sec: number;
  decay_sec: number;
  sustain: number;
  release_sec: number;
  portamento_ms: number;
  chorus_depth: number;
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

export interface SupersawPanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function supersawPanel(state: MixerState, dispatcher: Dispatcher): SupersawPanel {
  const root = document.createElement('div');
  root.className = 'mpe-panel';
  root.append(
    buildOnRow(state, dispatcher),
    buildPresetGrid(state, dispatcher),
    buildControls(state, dispatcher),
  );
  return { element: root, setActive: () => { /* mono */ } };
}

function buildOnRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'synth-on-row';
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'synth-on-btn';
  state.supersaw.on.subscribe((on) => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
  });
  btn.addEventListener('click', () => {
    if (state.supersaw.midiAuto.get()) state.supersaw.midiAuto.set(false);
    dispatcher.setSupersawOn(!state.supersaw.on.get());
  });
  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'Supersaw';
  const autoWrap = document.createElement('label');
  autoWrap.className = 'synth-auto';
  const autoBox = document.createElement('input');
  autoBox.type = 'checkbox';
  state.supersaw.midiAuto.subscribe((a) => { autoBox.checked = a; });
  autoBox.addEventListener('change', () => state.supersaw.midiAuto.set(autoBox.checked));
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
    dispatcher.setSupersawVolume(p.volume);
    dispatcher.setSupersawDetune(p.detune_cents);
    dispatcher.setSupersawMixCenter(p.mix_center);
    dispatcher.setSupersawCutoff(p.cutoff_hz);
    dispatcher.setSupersawResonance(p.resonance);
    dispatcher.setSupersawAttack(p.attack_sec);
    dispatcher.setSupersawDecay(p.decay_sec);
    dispatcher.setSupersawSustain(p.sustain);
    dispatcher.setSupersawRelease(p.release_sec);
    dispatcher.setSupersawPortamento(p.portamento_ms);
    dispatcher.setSupersawChorusDepth(p.chorus_depth);
    state.supersaw.activePresetId.set(preset.id);
  });
  state.supersaw.activePresetId.subscribe((id) => card.classList.toggle('active', id === preset.id));
  return card;
}

function buildControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls';
  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Parameters';
  section.appendChild(title);

  section.appendChild(paramRow('Detune', state.supersaw.detuneCents,
    0, 100, (v) => `${v.toFixed(1)} ¢`,
    (v) => dispatcher.setSupersawDetune(v)));
  section.appendChild(paramRow('Mix', state.supersaw.mixCenter,
    0, 1, (v) => v < 0.1 ? 'Sides' : v > 0.9 ? 'Center' : v.toFixed(2),
    (v) => dispatcher.setSupersawMixCenter(v)));
  section.appendChild(paramRow('Cutoff', state.supersaw.cutoffHz,
    50, 20000, (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setSupersawCutoff(v), { log: true }));
  section.appendChild(paramRow('Resonance', state.supersaw.resonance,
    0.707, 5.0, (v) => v.toFixed(2),
    (v) => dispatcher.setSupersawResonance(v)));
  section.appendChild(paramRow('Attack', state.supersaw.attack,
    0.001, 3.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setSupersawAttack(v), { log: true }));
  section.appendChild(paramRow('Decay', state.supersaw.decay,
    0.001, 3.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setSupersawDecay(v), { log: true }));
  section.appendChild(paramRow('Sustain', state.supersaw.sustain,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setSupersawSustain(v)));
  section.appendChild(paramRow('Release', state.supersaw.release,
    0.001, 5.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setSupersawRelease(v), { log: true }));
  section.appendChild(paramRow('Portamento', state.supersaw.portamentoMs,
    0, 1000, (v) => v < 1 ? 'Off' : `${Math.round(v)} ms`,
    (v) => dispatcher.setSupersawPortamento(v)));
  section.appendChild(paramRow('Chorus', state.supersaw.chorusDepth,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setSupersawChorusDepth(v)));
  section.appendChild(paramRow('Volume', state.supersaw.volume,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setSupersawVolume(v)));
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
    b.addEventListener('click', () => dispatcher.setSupersawMidiChannel(ch));
    btns.push(b); pills.appendChild(b);
  }
  state.supersaw.midiChannel.subscribe((c) => btns.forEach((b, i) => b.classList.toggle('active', i === c)));
  row.appendChild(pills);
  return row;
}

// Chip (NES/Gameboy) sub-tab — 2 pulses + triangle sub + noise.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../../../lib/TDspChip/presets.json';

interface PresetParams {
  volume: number;
  pulse1_duty: number;
  pulse2_duty: number;
  pulse2_detune: number;
  tri_level: number;
  noise_level: number;
  voicing: number;
  arpeggio: number;
  arp_rate_hz: number;
  attack_sec: number;
  decay_sec: number;
  sustain: number;
  release_sec: number;
}
interface Preset {
  id: string; name: string; category: string; description: string; params: PresetParams;
}
interface PresetDoc {
  categories: Record<string, { color: string; label: string }>;
  presets: Preset[];
}
const PRESETS = presetDoc as PresetDoc;

const DUTY_LABELS  = ['12.5%', '25%', '50%', '75%'] as const;
const VOICING_LABELS = ['Unison', 'Octave', '5th', '3rd'] as const;
const ARP_LABELS   = ['Off', 'Up', 'Down', 'Random'] as const;

export interface ChipPanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function chipPanel(state: MixerState, dispatcher: Dispatcher): ChipPanel {
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
  state.chip.on.subscribe((on) => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
  });
  btn.addEventListener('click', () => {
    if (state.chip.midiAuto.get()) state.chip.midiAuto.set(false);
    dispatcher.setChipOn(!state.chip.on.get());
  });
  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'Chip';
  const autoWrap = document.createElement('label');
  autoWrap.className = 'synth-auto';
  const autoBox = document.createElement('input');
  autoBox.type = 'checkbox';
  state.chip.midiAuto.subscribe((a) => { autoBox.checked = a; });
  autoBox.addEventListener('change', () => state.chip.midiAuto.set(autoBox.checked));
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
    dispatcher.setChipVolume(p.volume);
    dispatcher.setChipPulse1Duty(p.pulse1_duty);
    dispatcher.setChipPulse2Duty(p.pulse2_duty);
    dispatcher.setChipPulse2Detune(p.pulse2_detune);
    dispatcher.setChipTriLevel(p.tri_level);
    dispatcher.setChipNoiseLevel(p.noise_level);
    dispatcher.setChipVoicing(p.voicing);
    dispatcher.setChipArpeggio(p.arpeggio);
    dispatcher.setChipArpRate(p.arp_rate_hz);
    dispatcher.setChipAttack(p.attack_sec);
    dispatcher.setChipDecay(p.decay_sec);
    dispatcher.setChipSustain(p.sustain);
    dispatcher.setChipRelease(p.release_sec);
    state.chip.activePresetId.set(preset.id);
  });
  state.chip.activePresetId.subscribe((id) => card.classList.toggle('active', id === preset.id));
  return card;
}

function buildControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls';
  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Parameters';
  section.appendChild(title);

  section.appendChild(segmentedRow('Pulse 1 Duty',
    state.chip.pulse1Duty, DUTY_LABELS, (i) => dispatcher.setChipPulse1Duty(i)));
  section.appendChild(segmentedRow('Pulse 2 Duty',
    state.chip.pulse2Duty, DUTY_LABELS, (i) => dispatcher.setChipPulse2Duty(i)));
  section.appendChild(paramRow('P2 Detune', state.chip.pulse2Detune,
    -100, 100, (v) => `${v.toFixed(1)} ¢`,
    (v) => dispatcher.setChipPulse2Detune(v)));
  section.appendChild(paramRow('Tri Level', state.chip.triLevel,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setChipTriLevel(v)));
  section.appendChild(paramRow('Noise', state.chip.noiseLevel,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setChipNoiseLevel(v)));
  section.appendChild(segmentedRow('Voicing',
    state.chip.voicing, VOICING_LABELS, (i) => dispatcher.setChipVoicing(i)));
  section.appendChild(segmentedRow('Arpeggio',
    state.chip.arpeggio, ARP_LABELS, (i) => dispatcher.setChipArpeggio(i)));
  section.appendChild(paramRow('Arp Rate', state.chip.arpRateHz,
    1, 40, (v) => `${v.toFixed(1)} Hz`,
    (v) => dispatcher.setChipArpRate(v)));
  section.appendChild(paramRow('Attack', state.chip.attack,
    0.0005, 1.0, (v) => `${Math.round(v * 1000)}ms`,
    (v) => dispatcher.setChipAttack(v), { log: true }));
  section.appendChild(paramRow('Decay', state.chip.decay,
    0.005, 2.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setChipDecay(v), { log: true }));
  section.appendChild(paramRow('Sustain', state.chip.sustain,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setChipSustain(v)));
  section.appendChild(paramRow('Release', state.chip.release,
    0.005, 2.0, (v) => v < 1 ? `${Math.round(v * 1000)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setChipRelease(v), { log: true }));
  section.appendChild(paramRow('Volume', state.chip.volume,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setChipVolume(v)));
  section.appendChild(midiChannelRow(state, dispatcher));
  return section;
}

function segmentedRow(
  label: string, signal: Signal<number>, labels: readonly string[],
  onClick: (idx: number) => void,
): HTMLElement {
  const row = document.createElement('div'); row.className = 'mpe-row';
  const lab = document.createElement('label'); lab.textContent = label; row.appendChild(lab);
  const group = document.createElement('div'); group.className = 'mpe-segmented';
  const btns: HTMLButtonElement[] = [];
  for (let i = 0; i < labels.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = labels[i];
    b.addEventListener('click', () => onClick(i));
    btns.push(b);
    group.appendChild(b);
  }
  signal.subscribe((v) => btns.forEach((b, i) => b.classList.toggle('active', i === v)));
  row.appendChild(group);
  return row;
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
    b.addEventListener('click', () => dispatcher.setChipMidiChannel(ch));
    btns.push(b); pills.appendChild(b);
  }
  state.chip.midiChannel.subscribe((c) => btns.forEach((b, i) => b.classList.toggle('active', i === c)));
  row.appendChild(pills);
  return row;
}

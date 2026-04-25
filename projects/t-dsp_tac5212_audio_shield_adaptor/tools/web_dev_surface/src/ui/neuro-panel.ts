// Neuro (reese bass) sub-tab — mono engine, no voice orbs (one voice).
//
// Layout (top to bottom):
//   1. On/off header (same pattern as dexed/mpe panels)
//   2. Preset grid — 8 presets across 4 categories (classic, growl,
//      wobble, sub). Clicking a card fires the setNeuro* writes and
//      plays a short preview note on the current MIDI channel.
//   3. Control rows: oscillator balance (detune/sub/osc3), envelope,
//      filter, LFO, portamento, volume + FX send, MIDI channel.
//
// Reuses the .mpe-* CSS classes from mpe-panel so styling is
// consistent across the two synth sub-tabs — the look is identical,
// only the controls differ.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../../../lib/TDspNeuro/presets.json';

// Shape of one preset in presets.json — narrow to what the UI needs.
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
  // Stink chain (optional — presets authored before Phase 2f omit these
  // and the loader leaves the current stink state alone).
  stink_enable?: number;
  stink_drive_low?: number;
  stink_drive_mid?: number;
  stink_drive_high?: number;
  stink_mix_low?: number;
  stink_mix_mid?: number;
  stink_mix_high?: number;
  stink_fold?: number;
  stink_crush?: number;
  stink_master_cutoff_hz?: number;
  stink_master_resonance?: number;
  stink_lfo2_rate_hz?: number;
  stink_lfo2_depth?: number;
  stink_lfo2_dest?: number;
  stink_lfo2_waveform?: number;
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

const LFO_WAVEFORM_LABELS = ['Sine', 'Tri', 'Saw', 'Square'] as const;
const LFO_DEST_LABELS     = ['Off', 'Cutoff', 'Pitch', 'Amp'] as const;

// ---- public entry point ---------------------------------------------

export interface NeuroPanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function neuroPanel(state: MixerState, dispatcher: Dispatcher): NeuroPanel {
  const root = document.createElement('div');
  root.className = 'mpe-panel';  // reuse MPE CSS

  root.append(
    buildOnRow(state, dispatcher),
    buildPresetGrid(state, dispatcher),
    buildControls(state, dispatcher),
    buildStinkControls(state, dispatcher),
  );

  // Neuro has no voice telemetry (mono, no subscription needed), so
  // setActive is a no-op — the hook exists only for symmetry with
  // MpePanel so main.ts can drive all sub-tabs the same way.
  const setActive = (_on: boolean): void => { /* no-op */ };

  return { element: root, setActive };
}

// ---- on/off header --------------------------------------------------

function buildOnRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'synth-on-row';

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'synth-on-btn';
  state.neuro.on.subscribe((on) => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
    btn.title = on ? 'Click to mute Neuro output' : 'Click to un-mute Neuro output';
  });
  // Temporary mute-while-on-this-tab. Switching tabs re-enables via
  // main.ts's sub-tab enforcer — "only active-tab synth plays" is
  // unconditional.
  btn.addEventListener('click', () => {
    dispatcher.setNeuroOn(!state.neuro.on.get());
  });

  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'Neuro';

  row.append(btn, label);
  return row;
}

// ---- preset grid ----------------------------------------------------

function buildPresetGrid(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-presets';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Presets';
  section.appendChild(title);

  const filterRow = document.createElement('div');
  filterRow.className = 'mpe-preset-filter';
  let activeCategory: string = 'all';
  const filterBtns: HTMLButtonElement[] = [];

  const categoryKeys = ['all', ...Object.keys(PRESETS.categories)];
  for (const key of categoryKeys) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-preset-filter-btn';
    if (key === 'all') {
      b.textContent = 'All';
    } else {
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

function buildPresetCard(
  preset: Preset,
  state: MixerState,
  dispatcher: Dispatcher,
): HTMLElement {
  const card = document.createElement('button');
  card.type = 'button';
  card.className = 'mpe-preset-card';
  card.dataset.presetId = preset.id;
  const cat = PRESETS.categories[preset.category];
  if (cat) card.style.setProperty('--cat-color', cat.color);

  const name = document.createElement('div');
  name.className = 'mpe-preset-name';
  name.textContent = preset.name;

  const glyphs = document.createElement('div');
  glyphs.className = 'mpe-preset-glyphs';

  // Envelope glyph — Neuro's identity is always 3 saws + sub sine, so
  // showing the waveform tile would just be "Saw" on every card. The
  // envelope + LFO glyphs are the distinguishing visuals.
  const envGlyph = envelopeGlyph(preset.params.attack_sec, preset.params.release_sec, 28, 12);
  envGlyph.classList.add('mpe-preset-glyph');
  glyphs.appendChild(envGlyph);

  if (preset.params.lfo_dest !== 0 && preset.params.lfo_depth > 0) {
    const lfoGlyph = document.createElement('span');
    lfoGlyph.className = 'mpe-preset-glyph mpe-preset-glyph-lfo';
    lfoGlyph.textContent = 'LFO';
    lfoGlyph.title = `LFO → ${LFO_DEST_LABELS[preset.params.lfo_dest]} @ ${preset.params.lfo_rate_hz} Hz`;
    glyphs.appendChild(lfoGlyph);
  }

  if (preset.params.portamento_ms > 0) {
    const portGlyph = document.createElement('span');
    portGlyph.className = 'mpe-preset-glyph mpe-preset-glyph-lfo';
    portGlyph.textContent = 'PORT';
    portGlyph.title = `Portamento ${preset.params.portamento_ms} ms`;
    glyphs.appendChild(portGlyph);
  }

  card.append(name, glyphs);
  card.title = preset.description;

  card.addEventListener('click', () => {
    loadPreset(preset, dispatcher);
    state.neuro.activePresetId.set(preset.id);
  });

  state.neuro.activePresetId.subscribe((id) => {
    card.classList.toggle('active', id === preset.id);
  });

  return card;
}

function loadPreset(preset: Preset, dispatcher: Dispatcher): void {
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
  // Stink fields are optional on presets. Only overwrite the firmware
  // state if the preset explicitly defines them, so a pre-stink preset
  // keeps whatever the user has currently dialled in.
  if (p.stink_enable            !== undefined) dispatcher.setNeuroStinkEnable(p.stink_enable !== 0);
  if (p.stink_drive_low         !== undefined) dispatcher.setNeuroStinkDriveLow(p.stink_drive_low);
  if (p.stink_drive_mid         !== undefined) dispatcher.setNeuroStinkDriveMid(p.stink_drive_mid);
  if (p.stink_drive_high        !== undefined) dispatcher.setNeuroStinkDriveHigh(p.stink_drive_high);
  if (p.stink_mix_low           !== undefined) dispatcher.setNeuroStinkMixLow(p.stink_mix_low);
  if (p.stink_mix_mid           !== undefined) dispatcher.setNeuroStinkMixMid(p.stink_mix_mid);
  if (p.stink_mix_high          !== undefined) dispatcher.setNeuroStinkMixHigh(p.stink_mix_high);
  if (p.stink_fold              !== undefined) dispatcher.setNeuroStinkFold(p.stink_fold);
  if (p.stink_crush             !== undefined) dispatcher.setNeuroStinkCrush(p.stink_crush);
  if (p.stink_master_cutoff_hz  !== undefined) dispatcher.setNeuroStinkMasterCutoff(p.stink_master_cutoff_hz);
  if (p.stink_master_resonance  !== undefined) dispatcher.setNeuroStinkMasterResonance(p.stink_master_resonance);
  if (p.stink_lfo2_rate_hz      !== undefined) dispatcher.setNeuroStinkLfo2Rate(p.stink_lfo2_rate_hz);
  if (p.stink_lfo2_depth        !== undefined) dispatcher.setNeuroStinkLfo2Depth(p.stink_lfo2_depth);
  if (p.stink_lfo2_dest         !== undefined) dispatcher.setNeuroStinkLfo2Dest(p.stink_lfo2_dest);
  if (p.stink_lfo2_waveform     !== undefined) dispatcher.setNeuroStinkLfo2Waveform(p.stink_lfo2_waveform);
}

// ---- glyphs ---------------------------------------------------------

function envelopeGlyph(attackSec: number, releaseSec: number, w: number, h: number): SVGSVGElement {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('width',  String(w));
  svg.setAttribute('height', String(h));
  svg.setAttribute('viewBox', `0 0 ${w} ${h}`);

  const attackNorm  = Math.min(1, Math.sqrt(attackSec / 1.0));
  const releaseNorm = Math.min(1, Math.sqrt(releaseSec / 2.0));
  const attackPx  = 1 + attackNorm  * (w * 0.4);
  const releasePx = w - 1 - releaseNorm * (w * 0.4);
  const sustain   = 0.3 * h;

  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('stroke', 'currentColor');
  path.setAttribute('stroke-width', '1.5');
  path.setAttribute('fill', 'none');
  path.setAttribute('d',
    `M 0 ${h} L ${attackPx} 1` +
    ` L ${attackPx + 2} ${sustain}` +
    ` L ${releasePx} ${sustain}` +
    ` L ${w} ${h}`
  );
  svg.appendChild(path);
  return svg;
}

// ---- controls -------------------------------------------------------

function buildControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Parameters';
  section.appendChild(title);

  // Oscillator balance — detune, sub level, osc3 level.
  section.appendChild(buildParamRow('Detune',  state.neuro.detuneCents,
    0, 50, (v) => `${v.toFixed(1)} ¢`,
    (v) => dispatcher.setNeuroDetune(v)));
  section.appendChild(buildParamRow('Sub',     state.neuro.subLevel,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroSub(v)));
  section.appendChild(buildParamRow('Osc3',    state.neuro.osc3Level,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroOsc3(v)));

  // Envelope.
  section.appendChild(buildParamRow('Attack',  state.neuro.attack,
    0.0005, 2.0, (v) => formatSeconds(v),
    (v) => dispatcher.setNeuroAttack(v), { log: true }));
  section.appendChild(buildParamRow('Release', state.neuro.release,
    0.001, 3.0, (v) => formatSeconds(v),
    (v) => dispatcher.setNeuroRelease(v), { log: true }));

  // Filter.
  section.appendChild(buildParamRow('Filter',  state.neuro.filterCutoffHz,
    50, 20000, (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setNeuroFilterCutoff(v), { log: true }));
  section.appendChild(buildParamRow('Q',       state.neuro.filterResonance,
    0.707, 5.0, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroFilterResonance(v)));

  // LFO.
  section.appendChild(buildLfoRow(state, dispatcher));

  // Portamento.
  section.appendChild(buildParamRow('Portamento', state.neuro.portamentoMs,
    0, 2000, (v) => v < 1 ? 'Off' : `${Math.round(v)} ms`,
    (v) => dispatcher.setNeuroPortamento(v)));

  // Volume + FX send.
  section.appendChild(buildParamRow('Volume',  state.neuro.volume,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroVolume(v)));
  section.appendChild(buildParamRow('FX Send', state.neuro.fxSend,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroFxSend(v)));

  // MIDI channel picker — 17 pills (0 = omni, 1..16).
  section.appendChild(buildMidiChannelPicker(state, dispatcher));

  return section;
}

interface ParamRowOpts {
  log?: boolean;
}

function buildParamRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  format: (v: number) => string,
  onChange: (v: number) => void,
  opts: ParamRowOpts = {},
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = labelText;
  const slider = document.createElement('input');
  slider.type = 'range';

  const logMap = opts.log === true;
  if (logMap) {
    slider.min = '0';
    slider.max = '1000';
    slider.step = '1';
  } else {
    slider.min = String(min);
    slider.max = String(max);
    const range = max - min;
    slider.step = String(range / 1000);
  }

  const readout = document.createElement('span');
  readout.className = 'mpe-readout';

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
    const v  = lo + (pos / 1000) * (hi - lo);
    return Math.exp(v);
  };

  signal.subscribe((v) => {
    slider.value = String(toSlider(v));
    readout.textContent = format(v);
  });
  slider.addEventListener('input', () => {
    const v = fromSlider(parseFloat(slider.value));
    onChange(v);
  });

  row.append(label, slider, readout);
  return row;
}

function buildLfoRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row mpe-lfo-row';
  const label = document.createElement('label');
  label.textContent = 'LFO';
  row.appendChild(label);

  const rate = document.createElement('input');
  rate.type = 'range';
  rate.min = '0';
  rate.max = '20';
  rate.step = '0.1';
  state.neuro.lfoRate.subscribe((v) => { rate.value = String(v); });
  rate.addEventListener('input', () => dispatcher.setNeuroLfoRate(parseFloat(rate.value)));
  const rateReadout = document.createElement('span');
  rateReadout.className = 'mpe-readout';
  state.neuro.lfoRate.subscribe((v) => { rateReadout.textContent = `${v.toFixed(1)} Hz`; });

  const depth = document.createElement('input');
  depth.type = 'range';
  depth.min = '0';
  depth.max = '1';
  depth.step = '0.01';
  state.neuro.lfoDepth.subscribe((v) => { depth.value = String(v); });
  depth.addEventListener('input', () => dispatcher.setNeuroLfoDepth(parseFloat(depth.value)));
  const depthReadout = document.createElement('span');
  depthReadout.className = 'mpe-readout';
  state.neuro.lfoDepth.subscribe((v) => { depthReadout.textContent = v.toFixed(2); });

  const destGroup = document.createElement('div');
  destGroup.className = 'mpe-segmented';
  const destBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_DEST_LABELS.length; i++) {
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

  const waveGroup = document.createElement('div');
  waveGroup.className = 'mpe-segmented';
  const waveBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_WAVEFORM_LABELS.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_WAVEFORM_LABELS[i];
    b.title = LFO_WAVEFORM_LABELS[i];
    b.addEventListener('click', () => dispatcher.setNeuroLfoWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.neuro.lfoWaveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });

  const sub1 = document.createElement('div');
  sub1.className = 'mpe-lfo-sub';
  sub1.append(label, rate, rateReadout, depth, depthReadout);

  const sub2 = document.createElement('div');
  sub2.className = 'mpe-lfo-sub mpe-lfo-sub-pickers';
  sub2.append(destGroup, waveGroup);

  row.appendChild(sub1);
  row.appendChild(sub2);
  row.classList.add('mpe-row-col');
  return row;
}

function buildMidiChannelPicker(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = 'MIDI Ch';
  label.title = 'Listen channel. "Any" (0) = omni — the voice sounds on any incoming channel.';
  row.appendChild(label);

  const pills = document.createElement('div');
  pills.className = 'mpe-channel-pills';
  const pillBtns: HTMLButtonElement[] = [];
  // 0 = omni, 1..16 = channel. 17 pills total.
  for (let ch = 0; ch <= 16; ch++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-channel-pill';
    b.textContent = ch === 0 ? 'Any' : String(ch);
    b.addEventListener('click', () => dispatcher.setNeuroMidiChannel(ch));
    pillBtns.push(b);
    pills.appendChild(b);
  }
  state.neuro.midiChannel.subscribe((c) => {
    pillBtns.forEach((b, i) => b.classList.toggle('active', i === c));
  });
  row.appendChild(pills);
  return row;
}

// ---- formatters -----------------------------------------------------

function formatSeconds(v: number): string {
  if (v < 0.01) return `${Math.round(v * 1000)}ms`;
  if (v < 1) return `${(v * 1000).toFixed(0)}ms`;
  return `${v.toFixed(2)}s`;
}

// ---- stink chain controls -------------------------------------------
//
// Phase 2f — multiband destruction. Distinct section so the UI can
// visually separate "clean reese" (upper controls) from "the grit"
// (these controls). On/off toggle at the top lets you A/B the whole
// chain against the bare reese.

const STINK_LFO_DEST_LABELS = ['Off', 'Fold', 'Crush', 'Master', 'Mid Drive'] as const;

function buildStinkControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Stink Chain';
  section.appendChild(title);

  // Enable toggle row — same look as the main on/off button but scoped
  // to the stink chain only.
  section.appendChild(buildStinkEnableRow(state, dispatcher));

  // Per-band drive.
  section.appendChild(buildParamRow('Drive Low',  state.neuro.stinkDriveLow,
    0, 10, (v) => v.toFixed(2) + '×',
    (v) => dispatcher.setNeuroStinkDriveLow(v)));
  section.appendChild(buildParamRow('Drive Mid',  state.neuro.stinkDriveMid,
    0, 10, (v) => v.toFixed(2) + '×',
    (v) => dispatcher.setNeuroStinkDriveMid(v)));
  section.appendChild(buildParamRow('Drive High', state.neuro.stinkDriveHigh,
    0, 10, (v) => v.toFixed(2) + '×',
    (v) => dispatcher.setNeuroStinkDriveHigh(v)));

  // Per-band mix (recombine balance).
  section.appendChild(buildParamRow('Mix Low',   state.neuro.stinkMixLow,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkMixLow(v)));
  section.appendChild(buildParamRow('Mix Mid',   state.neuro.stinkMixMid,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkMixMid(v)));
  section.appendChild(buildParamRow('Mix High',  state.neuro.stinkMixHigh,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkMixHigh(v)));

  // Master chain.
  section.appendChild(buildParamRow('Fold',   state.neuro.stinkFold,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkFold(v)));
  section.appendChild(buildParamRow('Crush',  state.neuro.stinkCrush,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkCrush(v)));
  section.appendChild(buildParamRow('Master', state.neuro.stinkMasterCutoffHz,
    50, 20000, (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setNeuroStinkMasterCutoff(v), { log: true }));
  section.appendChild(buildParamRow('Master Q', state.neuro.stinkMasterResonance,
    0.707, 5.0, (v) => v.toFixed(2),
    (v) => dispatcher.setNeuroStinkMasterResonance(v)));

  // LFO2.
  section.appendChild(buildStinkLfoRow(state, dispatcher));

  return section;
}

function buildStinkEnableRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = 'Stink';
  label.title = 'Enable the multiband destruction chain. Off = clean reese only.';
  row.appendChild(label);

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'synth-on-btn';
  state.neuro.stinkEnable.subscribe((on) => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
  });
  btn.addEventListener('click', () => {
    dispatcher.setNeuroStinkEnable(!state.neuro.stinkEnable.get());
  });

  row.appendChild(btn);
  return row;
}

function buildStinkLfoRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row mpe-lfo-row';
  const label = document.createElement('label');
  label.textContent = 'LFO2';
  label.title = 'Second LFO, dedicated to stink chain modulation. Composes with Sink\'s LFO.';
  row.appendChild(label);

  const rate = document.createElement('input');
  rate.type = 'range';
  rate.min = '0';
  rate.max = '20';
  rate.step = '0.1';
  state.neuro.stinkLfo2Rate.subscribe((v) => { rate.value = String(v); });
  rate.addEventListener('input', () => dispatcher.setNeuroStinkLfo2Rate(parseFloat(rate.value)));
  const rateReadout = document.createElement('span');
  rateReadout.className = 'mpe-readout';
  state.neuro.stinkLfo2Rate.subscribe((v) => { rateReadout.textContent = `${v.toFixed(1)} Hz`; });

  const depth = document.createElement('input');
  depth.type = 'range';
  depth.min = '0';
  depth.max = '1';
  depth.step = '0.01';
  state.neuro.stinkLfo2Depth.subscribe((v) => { depth.value = String(v); });
  depth.addEventListener('input', () => dispatcher.setNeuroStinkLfo2Depth(parseFloat(depth.value)));
  const depthReadout = document.createElement('span');
  depthReadout.className = 'mpe-readout';
  state.neuro.stinkLfo2Depth.subscribe((v) => { depthReadout.textContent = v.toFixed(2); });

  const destGroup = document.createElement('div');
  destGroup.className = 'mpe-segmented';
  const destBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < STINK_LFO_DEST_LABELS.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = STINK_LFO_DEST_LABELS[i];
    b.addEventListener('click', () => dispatcher.setNeuroStinkLfo2Dest(i));
    destBtns.push(b);
    destGroup.appendChild(b);
  }
  state.neuro.stinkLfo2Dest.subscribe((d) => {
    destBtns.forEach((b, i) => b.classList.toggle('active', i === d));
  });

  const waveGroup = document.createElement('div');
  waveGroup.className = 'mpe-segmented';
  const waveBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_WAVEFORM_LABELS.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_WAVEFORM_LABELS[i];
    b.addEventListener('click', () => dispatcher.setNeuroStinkLfo2Waveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.neuro.stinkLfo2Waveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });

  const sub1 = document.createElement('div');
  sub1.className = 'mpe-lfo-sub';
  sub1.append(label, rate, rateReadout, depth, depthReadout);

  const sub2 = document.createElement('div');
  sub2.className = 'mpe-lfo-sub mpe-lfo-sub-pickers';
  sub2.append(destGroup, waveGroup);

  row.appendChild(sub1);
  row.appendChild(sub2);
  row.classList.add('mpe-row-col');
  return row;
}

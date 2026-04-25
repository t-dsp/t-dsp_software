// Arp top-level tab — preset browser + full parameter surface for the
// firmware tdsp::ArpFilter.
//
// Layout top to bottom:
//   1. Header row — ON/Bypass toggle, Latch, Hold, Panic
//   2. Preset grid — 230+ presets across 14 categories with filter strip
//   3. Controls section:
//        a. Pattern picker (25 tiles)
//        b. Rate picker (15 tiles)
//        c. Gate + Swing sliders
//        d. Octave range + octave mode
//        e. Step length + 32-cell step-mask editor
//        f. Velocity mode + fixed/accent velocities
//        g. MPE mode + output channel pills + scatter range
//        h. Scale + scale root + transpose + repeat
//
// Reuses the .mpe-panel CSS family where possible; Arp-specific tweaks
// live in their own .arp-* classes when the MPE ones don't fit.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../../../lib/TDspArp/presets.json';

// ---- preset shape ----------------------------------------------------

interface PresetParams {
  pattern: number;
  rate: number;
  gate: number;
  swing: number;
  octaveRange: number;
  octaveMode: number;
  latch: boolean;
  hold: boolean;
  velMode: number;
  velFixed: number;
  velAccent: number;
  stepMask: number;
  stepLength: number;
  mpeMode: number;
  outputChannel: number;
  scatterBase: number;
  scatterCount: number;
  scale: number;
  scaleRoot: number;
  transpose: number;
  repeat: number;
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

// ---- enum-label tables ----------------------------------------------

const PATTERN_LABELS = [
  'Up', 'Down', 'Up-Dn', 'Dn-Up', 'Up-Dn+', 'Dn-Up+',
  'Played', 'Played-R', 'Random', 'Walk',
  'Chord', 'Chord+Up', 'Stab',
  'Converge', 'Diverge',
  'Thumb', 'Thumb-UD', 'Pinky', 'Pinky-UD',
  'Group3', 'Group4', 'Crab', 'Stair', 'Spiral', 'Euclid',
] as const;

const RATE_LABELS = [
  '1/1', '1/2.', '1/2', '1/2T',
  '1/4.', '1/4', '1/4T',
  '1/8.', '1/8', '1/8T',
  '1/16.', '1/16', '1/16T',
  '1/32', '1/32T',
] as const;

const OCTAVE_MODE_LABELS = ['Up', 'Down', 'Up-Dn', 'Rnd'] as const;

const VEL_MODE_LABELS = [
  'Source', 'Flat', 'Alt', 'Ramp+', 'Ramp-', 'Random', 'Acc N',
] as const;

const MPE_MODE_LABELS = ['Mono', 'Scatter', 'Expr Follow', 'Per-Note'] as const;

const SCALE_LABELS = [
  'Off', 'Chroma', 'Major', 'Minor', 'Dorian', 'Phryg', 'Lydian',
  'Mixo', 'Locri', 'Harm Min', 'Mel Min', 'Min Pent', 'Maj Pent',
  'Blues', 'Hiraj', 'In', 'Yo', 'Whole', 'Dim', 'Phryg Dom',
] as const;

const NOTE_LABELS = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'] as const;

// ---- public entry point ---------------------------------------------

export interface ArpPanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function arpPanel(state: MixerState, dispatcher: Dispatcher): ArpPanel {
  const root = document.createElement('div');
  root.className = 'mpe-panel arp-panel';

  root.append(
    buildHeaderRow(state, dispatcher),
    buildPresetGrid(state, dispatcher),
    buildControls(state, dispatcher),
  );

  // No telemetry streams — setActive is a no-op, kept for API symmetry
  // with panels that do subscribe (e.g. MPE voice orbs).
  const setActive = (_on: boolean): void => { /* no-op */ };

  return { element: root, setActive };
}

// ---- header row ------------------------------------------------------
//
// Big ON button (the arp-bypass toggle), plus Latch / Hold / Panic.

function buildHeaderRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'synth-on-row arp-header';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  const updateOn = (on: boolean): void => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ARP ACTIVE' : 'BYPASS';
    onBtn.title = on ? 'Click to bypass — keyboard plays synths directly' : 'Click to engage — keyboard feeds the arpeggiator';
  };
  state.arp.on.subscribe(updateOn);
  onBtn.addEventListener('click', () => dispatcher.setArpOn(!state.arp.on.get()));

  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'Arpeggiator';

  const latchBtn = makeToggle('Latch', state.arp.latch, (on) => dispatcher.setArpLatch(on));
  const holdBtn  = makeToggle('Hold',  state.arp.hold,  (on) => dispatcher.setArpHold(on));

  const panicBtn = document.createElement('button');
  panicBtn.type = 'button';
  panicBtn.className = 'arp-panic';
  panicBtn.textContent = 'Panic';
  panicBtn.title = 'Release everything and reset step position';
  panicBtn.addEventListener('click', () => dispatcher.arpPanic());

  row.append(onBtn, label, latchBtn, holdBtn, panicBtn);
  return row;
}

function makeToggle(label: string, sig: Signal<boolean>, onChange: (on: boolean) => void): HTMLButtonElement {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'arp-toggle';
  b.textContent = label;
  sig.subscribe((on) => b.classList.toggle('on', on));
  b.addEventListener('click', () => onChange(!sig.get()));
  return b;
}

// ---- preset grid -----------------------------------------------------

function buildPresetGrid(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-presets arp-presets';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = `Presets — ${PRESETS.presets.length}`;
  section.appendChild(title);

  const filterRow = document.createElement('div');
  filterRow.className = 'mpe-preset-filter arp-preset-filter';
  let activeCategory: string = 'all';
  const filterBtns: HTMLButtonElement[] = [];

  // Category counts — useful label suffix for "how many in each bucket?"
  const counts = new Map<string, number>();
  for (const p of PRESETS.presets) counts.set(p.category, (counts.get(p.category) ?? 0) + 1);

  const categoryKeys = ['all', ...Object.keys(PRESETS.categories)];
  for (const key of categoryKeys) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-preset-filter-btn';
    if (key === 'all') {
      b.textContent = `All · ${PRESETS.presets.length}`;
    } else {
      const cat = PRESETS.categories[key];
      b.textContent = `${cat.label} · ${counts.get(key) ?? 0}`;
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

  // Search field — 230+ presets means free-text filter earns its keep.
  const searchWrap = document.createElement('div');
  searchWrap.className = 'arp-preset-search';
  const searchInput = document.createElement('input');
  searchInput.type = 'search';
  searchInput.placeholder = 'Filter by name or description…';
  searchInput.addEventListener('input', renderGrid);
  searchWrap.appendChild(searchInput);
  section.appendChild(searchWrap);

  const grid = document.createElement('div');
  grid.className = 'mpe-preset-grid arp-preset-grid';
  section.appendChild(grid);

  function renderGrid(): void {
    grid.innerHTML = '';
    const needle = searchInput.value.trim().toLowerCase();
    const matches = PRESETS.presets.filter((p) => {
      if (activeCategory !== 'all' && p.category !== activeCategory) return false;
      if (!needle) return true;
      return p.name.toLowerCase().includes(needle) ||
             p.description.toLowerCase().includes(needle);
    });
    for (const preset of matches) {
      grid.appendChild(buildPresetCard(preset, state, dispatcher));
    }
  }
  renderGrid();

  return section;
}

function buildPresetCard(preset: Preset, state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const card = document.createElement('button');
  card.type = 'button';
  card.className = 'mpe-preset-card arp-preset-card';
  card.dataset.presetId = preset.id;
  const cat = PRESETS.categories[preset.category];
  if (cat) card.style.setProperty('--cat-color', cat.color);

  const name = document.createElement('div');
  name.className = 'mpe-preset-name';
  name.textContent = preset.name;

  const glyphs = document.createElement('div');
  glyphs.className = 'mpe-preset-glyphs arp-preset-glyphs';

  // Pattern short-label badge
  const patGlyph = document.createElement('span');
  patGlyph.className = 'arp-glyph arp-glyph-pat';
  patGlyph.textContent = PATTERN_LABELS[preset.params.pattern] ?? '?';
  glyphs.appendChild(patGlyph);

  // Rate badge
  const rateGlyph = document.createElement('span');
  rateGlyph.className = 'arp-glyph arp-glyph-rate';
  rateGlyph.textContent = RATE_LABELS[preset.params.rate] ?? '?';
  glyphs.appendChild(rateGlyph);

  // Octave-range badge (only when >1)
  if (preset.params.octaveRange > 1) {
    const oct = document.createElement('span');
    oct.className = 'arp-glyph arp-glyph-oct';
    oct.textContent = `${preset.params.octaveRange}×`;
    oct.title = `${preset.params.octaveRange} octaves · ${OCTAVE_MODE_LABELS[preset.params.octaveMode]}`;
    glyphs.appendChild(oct);
  }

  // MPE badge (only for non-Mono)
  if (preset.params.mpeMode !== 0) {
    const mpe = document.createElement('span');
    mpe.className = 'arp-glyph arp-glyph-mpe';
    mpe.textContent = MPE_MODE_LABELS[preset.params.mpeMode];
    glyphs.appendChild(mpe);
  }

  // Scale badge (only when not Off)
  if (preset.params.scale !== 0) {
    const sc = document.createElement('span');
    sc.className = 'arp-glyph arp-glyph-scale';
    sc.textContent = SCALE_LABELS[preset.params.scale];
    glyphs.appendChild(sc);
  }

  card.append(name, glyphs);
  card.title = preset.description;

  card.addEventListener('click', () => {
    loadPreset(preset, dispatcher);
    state.arp.activePresetId.set(preset.id);
  });

  state.arp.activePresetId.subscribe((id) => {
    card.classList.toggle('active', id === preset.id);
  });

  return card;
}

function loadPreset(preset: Preset, dispatcher: Dispatcher): void {
  const p = preset.params;
  dispatcher.setArpPattern      (p.pattern);
  dispatcher.setArpRate         (p.rate);
  dispatcher.setArpGate         (p.gate);
  dispatcher.setArpSwing        (p.swing);
  dispatcher.setArpOctaveRange  (p.octaveRange);
  dispatcher.setArpOctaveMode   (p.octaveMode);
  // Latch and Hold are intentionally NOT loaded from the preset — they
  // are performance state (what the player's fingers are doing right
  // now), not sound-design state. Preserving them across preset swaps
  // lets you latch a chord once and audition every preset against it
  // without having to re-latch each time. Toggle them from the header
  // buttons when you actually want to change their state.
  dispatcher.setArpVelMode      (p.velMode);
  dispatcher.setArpVelFixed     (p.velFixed);
  dispatcher.setArpVelAccent    (p.velAccent);
  dispatcher.setArpStepMask     (p.stepMask);
  dispatcher.setArpStepLength   (p.stepLength);
  dispatcher.setArpMpeMode      (p.mpeMode);
  dispatcher.setArpOutputChannel(p.outputChannel);
  dispatcher.setArpScatterBase  (p.scatterBase);
  dispatcher.setArpScatterCount (p.scatterCount);
  dispatcher.setArpScale        (p.scale);
  dispatcher.setArpScaleRoot    (p.scaleRoot);
  dispatcher.setArpTranspose    (p.transpose);
  dispatcher.setArpRepeat       (p.repeat);
}

// ---- controls --------------------------------------------------------

function buildControls(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-controls arp-controls';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Parameters';
  section.appendChild(title);

  section.appendChild(buildSegmentRow('Pattern', PATTERN_LABELS, state.arp.pattern,
    (i) => dispatcher.setArpPattern(i), 'arp-patterns'));
  section.appendChild(buildSegmentRow('Rate', RATE_LABELS, state.arp.rate,
    (i) => dispatcher.setArpRate(i), 'arp-rates'));

  section.appendChild(buildSliderRow('Gate', state.arp.gate, 0.05, 1.5,
    (v) => `${(v * 100).toFixed(0)}%`, (v) => dispatcher.setArpGate(v)));
  section.appendChild(buildSliderRow('Swing', state.arp.swing, 0.5, 0.85,
    (v) => `${((v - 0.5) * 200).toFixed(0)}%`, (v) => dispatcher.setArpSwing(v)));

  section.appendChild(buildSegmentRow('Octaves', ['1', '2', '3', '4'], state.arp.octaveRange,
    (i) => dispatcher.setArpOctaveRange(i), 'arp-octrange', 1));
  section.appendChild(buildSegmentRow('Oct Mode', OCTAVE_MODE_LABELS, state.arp.octaveMode,
    (i) => dispatcher.setArpOctaveMode(i), 'arp-octmode'));

  section.appendChild(buildStepLengthRow(state, dispatcher));
  section.appendChild(buildStepMaskRow(state, dispatcher));

  section.appendChild(buildSegmentRow('Vel', VEL_MODE_LABELS, state.arp.velMode,
    (i) => dispatcher.setArpVelMode(i), 'arp-velmode'));
  section.appendChild(buildIntSliderRow('Fixed', state.arp.velFixed, 1, 127,
    (v) => dispatcher.setArpVelFixed(v)));
  section.appendChild(buildIntSliderRow('Accent', state.arp.velAccent, 1, 127,
    (v) => dispatcher.setArpVelAccent(v)));

  section.appendChild(buildSegmentRow('MPE', MPE_MODE_LABELS, state.arp.mpeMode,
    (i) => dispatcher.setArpMpeMode(i), 'arp-mpemode'));
  section.appendChild(buildChannelPillsRow('Output Ch', state.arp.outputChannel, 1, 16,
    (ch) => dispatcher.setArpOutputChannel(ch)));
  section.appendChild(buildChannelPillsRow('Scatter From', state.arp.scatterBase, 1, 16,
    (ch) => dispatcher.setArpScatterBase(ch)));
  section.appendChild(buildIntSliderRow('Scatter Span', state.arp.scatterCount, 1, 8,
    (n) => dispatcher.setArpScatterCount(n)));

  section.appendChild(buildSegmentRow('Scale', SCALE_LABELS, state.arp.scale,
    (i) => dispatcher.setArpScale(i), 'arp-scale'));
  section.appendChild(buildSegmentRow('Root', NOTE_LABELS, state.arp.scaleRoot,
    (i) => dispatcher.setArpScaleRoot(i), 'arp-root'));
  section.appendChild(buildIntSliderRow('Transpose', state.arp.transpose, -24, 24,
    (v) => dispatcher.setArpTranspose(v)));
  section.appendChild(buildIntSliderRow('Repeat', state.arp.repeat, 1, 8,
    (v) => dispatcher.setArpRepeat(v)));

  return section;
}

// ---- shared rows -----------------------------------------------------
//
// A "segmented" row is a label + a strip of small buttons, one per enum
// value. .active highlight follows the signal. Extra className is folded
// into the root so we can target pattern vs rate vs scale rows in CSS.

function buildSegmentRow<T extends ReadonlyArray<string>>(
  labelText: string,
  options: T,
  signal: Signal<number>,
  onChange: (i: number) => void,
  extraClass: string = '',
  indexBase: number = 0,
): HTMLElement {
  const row = document.createElement('div');
  row.className = `mpe-row mpe-row-col arp-seg-row ${extraClass}`;

  const label = document.createElement('label');
  label.textContent = labelText;
  row.appendChild(label);

  const seg = document.createElement('div');
  seg.className = 'mpe-segmented arp-segmented';
  const btns: HTMLButtonElement[] = [];
  for (let i = 0; i < options.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = options[i];
    const value = i + indexBase;
    b.addEventListener('click', () => onChange(value));
    btns.push(b);
    seg.appendChild(b);
  }
  signal.subscribe((v) => {
    const active = v - indexBase;
    btns.forEach((b, i) => b.classList.toggle('active', i === active));
  });
  row.appendChild(seg);
  return row;
}

function buildSliderRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  format: (v: number) => string,
  onChange: (v: number) => void,
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = labelText;
  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = String(min);
  slider.max = String(max);
  slider.step = String((max - min) / 1000);
  const readout = document.createElement('span');
  readout.className = 'mpe-readout';
  signal.subscribe((v) => { slider.value = String(v); readout.textContent = format(v); });
  slider.addEventListener('input', () => onChange(parseFloat(slider.value)));
  row.append(label, slider, readout);
  return row;
}

function buildIntSliderRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  onChange: (v: number) => void,
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = labelText;
  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = String(min);
  slider.max = String(max);
  slider.step = '1';
  const readout = document.createElement('span');
  readout.className = 'mpe-readout';
  signal.subscribe((v) => { slider.value = String(v); readout.textContent = String(v); });
  slider.addEventListener('input', () => onChange(parseInt(slider.value, 10)));
  row.append(label, slider, readout);
  return row;
}

function buildChannelPillsRow(
  labelText: string,
  signal: Signal<number>,
  min: number,
  max: number,
  onChange: (ch: number) => void,
): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = labelText;
  row.appendChild(label);
  const pills = document.createElement('div');
  pills.className = 'mpe-channel-pills arp-channel-pills';
  const btns: HTMLButtonElement[] = [];
  for (let ch = min; ch <= max; ch++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-channel-pill';
    b.textContent = String(ch);
    b.addEventListener('click', () => onChange(ch));
    btns.push(b);
    pills.appendChild(b);
  }
  signal.subscribe((cur) => {
    btns.forEach((b, i) => b.classList.toggle('active', i + min === cur));
  });
  row.appendChild(pills);
  return row;
}

// ---- step length + step mask ----------------------------------------
//
// Step mask is a 32-bit signed int. Bit 0 = step 1, bit N-1 = step N. We
// render 32 clickable cells; step-length slider grays out cells past the
// active length (the firmware only consults bits 0..stepLength-1).

function buildStepLengthRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = 'Step Length';
  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = '1';
  slider.max = '32';
  slider.step = '1';
  const readout = document.createElement('span');
  readout.className = 'mpe-readout';
  state.arp.stepLength.subscribe((v) => { slider.value = String(v); readout.textContent = `${v} steps`; });
  slider.addEventListener('input', () => dispatcher.setArpStepLength(parseInt(slider.value, 10)));
  row.append(label, slider, readout);
  return row;
}

function buildStepMaskRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row mpe-row-col arp-mask-row';
  const label = document.createElement('label');
  label.textContent = 'Steps';
  row.appendChild(label);

  // Quick-fill buttons row — common patterns from a glance.
  const quick = document.createElement('div');
  quick.className = 'arp-mask-quick';
  const addQuick = (txt: string, mask: number): void => {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = txt;
    b.addEventListener('click', () => dispatcher.setArpStepMask(mask));
    quick.appendChild(b);
  };
  addQuick('All',       -1);           // 0xFFFFFFFF
  addQuick('None',      0);
  addQuick('Alt',       0x55555555 | 0);
  addQuick('Alt-Rev',   0xAAAAAAAA | 0);
  addQuick('Half',      0xFFFF0000 | 0);
  addQuick('Euclid 5/16', 0x1111);
  addQuick('Euclid 7/16', 0x5555);
  // Invert needs to read the current mask, so it's wired directly
  // rather than through the static-mask addQuick() helper.
  const invertBtn = document.createElement('button');
  invertBtn.type = 'button';
  invertBtn.textContent = 'Invert';
  invertBtn.addEventListener('click', () => dispatcher.setArpStepMask((~state.arp.stepMask.get()) | 0));
  quick.appendChild(invertBtn);

  row.appendChild(quick);

  const grid = document.createElement('div');
  grid.className = 'arp-mask-grid';
  const cells: HTMLButtonElement[] = [];
  for (let i = 0; i < 32; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'arp-mask-cell';
    b.textContent = String(i + 1);
    b.dataset.step = String(i);
    b.addEventListener('click', () => {
      const cur = state.arp.stepMask.get();
      const next = (cur ^ (1 << i)) | 0;
      dispatcher.setArpStepMask(next);
    });
    cells.push(b);
    grid.appendChild(b);
  }
  const redraw = (): void => {
    const mask = state.arp.stepMask.get();
    const len  = state.arp.stepLength.get();
    for (let i = 0; i < 32; i++) {
      const active = ((mask >>> i) & 1) === 1;
      const inRange = i < len;
      cells[i].classList.toggle('on', active);
      cells[i].classList.toggle('out-of-range', !inRange);
    }
  };
  state.arp.stepMask.subscribe(redraw);
  state.arp.stepLength.subscribe(redraw);

  row.appendChild(grid);
  return row;
}

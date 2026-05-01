// MPE VA sub-tab — voice orbs + controls + preset browser.
//
// Layout (top to bottom):
//   1. Voice orbs   — 4 live circles, one per firmware voice. Pitch →
//                     horizontal position, pressure → radius, channel →
//                     hue, held → brightness. Motion-blur trail for
//                     recent expression. Subscribe-gated: the panel
//                     asks the firmware for /synth/mpe/voices when
//                     the MPE sub-tab is active, unsubscribes when it
//                     isn't, so unused tabs don't eat CDC bandwidth.
//   2. Preset grid  — 25 presets across 4 category bands (retro,
//                     emulating, abstract, modern). Clicking a card
//                     fires the 10 OSC writes for that preset plus
//                     an auto-preview note (a 400 ms middle-C on ch 2).
//   3. Control rows — waveform picker / A+R sliders / filter /
//                     LFO / volume + FX send / master channel picker.
//
// The panel subscribes to MpeState signals and keeps the UI in sync
// with the firmware. UI gestures go through Dispatcher which
// optimistically updates the signal and sends the corresponding OSC.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';
import presetDoc from '../../../../lib/TDspMPE/presets.json';

// Shape of one preset in presets.json — narrow to what the UI needs.
interface PresetParams {
  waveform: number;
  attack_sec: number;
  release_sec: number;
  volume: number;
  filter_cutoff_hz: number;
  filter_resonance: number;
  lfo_rate_hz: number;
  lfo_depth: number;
  lfo_dest: number;
  lfo_waveform: number;
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

// Waveform labels used in the picker tiles. Match the firmware's OSC
// code ordering (0=saw, 1=square, 2=tri, 3=sine).
const WAVEFORM_LABELS = ['Saw', 'Square', 'Tri', 'Sine'] as const;
const LFO_WAVEFORM_LABELS = ['Sine', 'Tri', 'Saw', 'Square'] as const;
const LFO_DEST_LABELS = ['Off', 'Cutoff', 'Pitch', 'Amp'] as const;

// ---- public entry point ---------------------------------------------

// onActiveChange is fired by the hosting tab-switcher so the panel
// knows when to subscribe / unsubscribe the voice telemetry stream.
// Passing it here (instead of reading document visibility) keeps the
// panel decoupled from the subnav implementation.
export interface MpePanel {
  element: HTMLElement;
  setActive: (on: boolean) => void;
}

export function mpePanel(state: MixerState, dispatcher: Dispatcher): MpePanel {
  const root = document.createElement('div');
  root.className = 'mpe-panel';

  root.append(
    buildOnRow(state, dispatcher),
    buildVoiceOrbs(state, dispatcher),
    buildPresetGrid(state, dispatcher),
    buildControls(state, dispatcher),
  );

  let active = false;
  const setActive = (on: boolean): void => {
    if (on === active) return;
    active = on;
    if (on) dispatcher.subscribeMpeVoices();
    else    dispatcher.unsubscribeMpeVoices();
  };

  return { element: root, setActive };
}

// ---- on/off header --------------------------------------------------

function buildOnRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'synth-on-row';

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'synth-on-btn';
  const update = (on: boolean): void => {
    btn.classList.toggle('on', on);
    btn.textContent = on ? 'ON' : 'OFF';
    btn.title = on ? 'Click to mute MPE output' : 'Click to un-mute MPE output';
  };
  state.mpe.on.subscribe(update);
  // Temporary mute-while-on-this-tab. Switching tabs re-enables via
  // main.ts's sub-tab enforcer — "only active-tab synth plays" is
  // unconditional.
  btn.addEventListener('click', () => {
    dispatcher.setMpeOn(!state.mpe.on.get());
  });

  const label = document.createElement('span');
  label.className = 'synth-on-label';
  label.textContent = 'MPE VA';

  row.append(btn, label);
  return row;
}

// ---- voice orbs -----------------------------------------------------
//
// Canvas-rendered. Each orb is a circle whose position, radius, and
// glow are driven by the corresponding MpeVoiceState. A short trail
// tracks recent positions so sustained notes leave a visible streak.
//
// Why canvas: 4 orbs × 60 fps × motion blur + trails is a lot of
// paint operations. Canvas is one draw call per frame; DOM would
// force re-layout on every pressure change.

function buildVoiceOrbs(state: MixerState, _dispatcher: Dispatcher): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'mpe-orbs';

  const canvas = document.createElement('canvas');
  canvas.className = 'mpe-orbs-canvas';
  // Logical size; actual pixel size is set in resize() below to
  // account for devicePixelRatio on HiDPI screens.
  canvas.width = 720;
  canvas.height = 160;
  wrap.appendChild(canvas);

  const legend = document.createElement('div');
  legend.className = 'mpe-orbs-legend';
  legend.innerHTML =
    '<span>Pitch → X</span>' +
    '<span>Pressure → size</span>' +
    '<span>Channel → color</span>';
  wrap.appendChild(legend);

  const ctx = canvas.getContext('2d');
  if (!ctx) return wrap;

  const resize = (): void => {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    if (rect.width === 0) return;
    canvas.width  = Math.floor(rect.width  * dpr);
    canvas.height = Math.floor(rect.height * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  };
  window.addEventListener('resize', resize);
  // Defer initial resize until after mount so offsetParent-based
  // rect reads work. A microtask is plenty.
  queueMicrotask(resize);

  // Per-voice trail history: last N frame snapshots of (x, y, r).
  // Short enough that released tails fade quickly, long enough that
  // a finger-wiggle leaves a visible streak.
  const TRAIL_LEN = 24;
  interface TrailPt { x: number; y: number; r: number; alpha: number; }
  const trails: TrailPt[][] = Array.from({ length: 4 }, () => []);

  const voiceColor = (ch: number): string => {
    // Channel-1 master rarely plays notes; chans 2..16 are members.
    // Use a 15-slot hue wheel offset by ch so adjacent channels are
    // visually distinct. Saturation+lightness chosen for HiDPI
    // visibility on both dark and light themes.
    const hue = ((ch - 1) * 24) % 360;
    return `hsl(${hue} 85% 60%)`;
  };

  const noteToX = (note: number, pitchSemi: number, w: number): number => {
    // Map MIDI 21 (A0) .. 108 (C8) across the canvas width. Pitch
    // bend nudges inside that range.
    const effectiveNote = note + pitchSemi;
    const lo = 21, hi = 108;
    const t = (effectiveNote - lo) / (hi - lo);
    return Math.max(10, Math.min(w - 10, t * w));
  };

  const draw = (): void => {
    const w = canvas.width  / (window.devicePixelRatio || 1);
    const h = canvas.height / (window.devicePixelRatio || 1);

    // Background — solid dark with a subtle horizon line to anchor
    // the orbs vertically.
    ctx.fillStyle = '#0b0f14';
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();

    for (let i = 0; i < 4; i++) {
      const v = state.mpe.voices[i];
      const held = v.held.get();
      const ch = v.channel.get();

      // Advance trail history every frame regardless of held state
      // so release tails fade smoothly.
      const trail = trails[i];
      if (held && ch > 0) {
        const x = noteToX(v.note.get(), v.pitchSemi.get(), w);
        const r = 6 + v.pressure.get() * 18;     // 6..24 px
        const y = h * 0.45;
        trail.unshift({ x, y, r, alpha: 1.0 });
      } else {
        // Released: let the head alpha decay but keep last position.
        if (trail.length > 0) {
          const head = trail[0];
          trail.unshift({ x: head.x, y: head.y, r: head.r * 0.9, alpha: head.alpha * 0.85 });
        }
      }
      while (trail.length > TRAIL_LEN) trail.pop();

      // Paint trail from tail to head so the head overdraws.
      const color = voiceColor(ch > 0 ? ch : 2);
      for (let j = trail.length - 1; j >= 0; j--) {
        const pt = trail[j];
        const tailAlpha = pt.alpha * (1 - j / TRAIL_LEN);
        ctx.globalAlpha = tailAlpha * (held ? 1 : 0.5);
        ctx.fillStyle = color;
        ctx.beginPath();
        ctx.arc(pt.x, pt.y, pt.r, 0, Math.PI * 2);
        ctx.fill();
      }

      // Voice index label inside the head.
      if (trail.length > 0 && trail[0].alpha > 0.2) {
        const head = trail[0];
        ctx.globalAlpha = 1.0;
        ctx.fillStyle = '#0b0f14';
        ctx.font = '12px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(String(i), head.x, head.y);
      }
    }
    ctx.globalAlpha = 1.0;
  };

  // 30 Hz redraw — matches the firmware's telemetry cadence. We
  // don't drive the draw off a per-signal subscribe because all four
  // voices animate together and the trails need frame advancement
  // even when nothing changes.
  let rafHandle = 0;
  let lastDraw = 0;
  const FRAME_MS = 33;
  const loop = (t: number): void => {
    if (t - lastDraw >= FRAME_MS) {
      draw();
      lastDraw = t;
    }
    rafHandle = requestAnimationFrame(loop);
  };
  rafHandle = requestAnimationFrame(loop);
  // We never tear this down — the panel lives for the page lifetime.
  void rafHandle;

  return wrap;
}

// ---- preset grid ----------------------------------------------------

function buildPresetGrid(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const section = document.createElement('section');
  section.className = 'mpe-presets';

  const title = document.createElement('h3');
  title.className = 'mpe-section-title';
  title.textContent = 'Presets';
  section.appendChild(title);

  // Category filter strip + "all" button.
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

  const waveGlyph = waveformGlyph(preset.params.waveform, 20, 12);
  waveGlyph.classList.add('mpe-preset-glyph');
  glyphs.appendChild(waveGlyph);

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

  card.append(name, glyphs);
  card.title = preset.description;

  // Clicking loads the preset. State signals update optimistically
  // on each setter call; firmware echoes come back and either confirm
  // or clamp. No auto-preview — the user plays the keyboard to audition.
  card.addEventListener('click', () => {
    loadPreset(preset, dispatcher);
    state.mpe.activePresetId.set(preset.id);
  });

  // Active-preset highlight — re-checked every time the signal changes.
  state.mpe.activePresetId.subscribe((id) => {
    card.classList.toggle('active', id === preset.id);
  });

  return card;
}

function loadPreset(preset: Preset, dispatcher: Dispatcher): void {
  const p = preset.params;
  dispatcher.setMpeWaveform       (p.waveform);
  dispatcher.setMpeAttack         (p.attack_sec);
  dispatcher.setMpeRelease        (p.release_sec);
  dispatcher.setMpeVolume         (p.volume);
  dispatcher.setMpeFilterCutoff   (p.filter_cutoff_hz);
  dispatcher.setMpeFilterResonance(p.filter_resonance);
  dispatcher.setMpeLfoRate        (p.lfo_rate_hz);
  dispatcher.setMpeLfoDepth       (p.lfo_depth);
  dispatcher.setMpeLfoDest        (p.lfo_dest);
  dispatcher.setMpeLfoWaveform    (p.lfo_waveform);
}

// ---- glyphs (tiny inline SVGs) --------------------------------------

function waveformGlyph(wave: number, w: number, h: number): SVGSVGElement {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('width',  String(w));
  svg.setAttribute('height', String(h));
  svg.setAttribute('viewBox', `0 0 ${w} ${h}`);

  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('stroke', 'currentColor');
  path.setAttribute('stroke-width', '1.5');
  path.setAttribute('fill', 'none');
  const mid = h / 2;
  let d = '';
  switch (wave) {
    case 0: // saw — rises linearly then drops
      d = `M 0 ${h} L ${w / 2} 0 L ${w / 2} ${h} L ${w} 0`;
      break;
    case 1: // square
      d = `M 0 ${h} L 0 0 L ${w / 2} 0 L ${w / 2} ${h} L ${w} ${h} L ${w} 0`;
      break;
    case 2: { // triangle
      d = `M 0 ${mid} L ${w / 4} 0 L ${(3 * w) / 4} ${h} L ${w} ${mid}`;
      break;
    }
    case 3: { // sine
      const steps = 12;
      d = `M 0 ${mid}`;
      for (let i = 1; i <= steps; i++) {
        const x = (i / steps) * w;
        const y = mid - Math.sin((i / steps) * Math.PI * 2) * (mid * 0.85);
        d += ` L ${x} ${y}`;
      }
      break;
    }
  }
  path.setAttribute('d', d);
  svg.appendChild(path);
  return svg;
}

function envelopeGlyph(attackSec: number, releaseSec: number, w: number, h: number): SVGSVGElement {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('width',  String(w));
  svg.setAttribute('height', String(h));
  svg.setAttribute('viewBox', `0 0 ${w} ${h}`);

  // Stylized ADSR shape — A ramps up, short D, flat sustain at 0.7,
  // R tapers. Time axis is logarithmic so a slow attack still fits.
  const attackNorm  = Math.min(1, Math.sqrt(attackSec / 1.0));
  const releaseNorm = Math.min(1, Math.sqrt(releaseSec / 2.0));
  const attackPx  = 1 + attackNorm  * (w * 0.4);
  const releasePx = w - 1 - releaseNorm * (w * 0.4);
  const sustain   = 0.3 * h;
  const peak      = 1;

  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('stroke', 'currentColor');
  path.setAttribute('stroke-width', '1.5');
  path.setAttribute('fill', 'none');
  const topPx = peak;
  path.setAttribute('d',
    `M 0 ${h} L ${attackPx} ${topPx}` +
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

  // Row 1: waveform picker (4 tiles).
  section.appendChild(buildWaveformPicker(state, dispatcher));

  // Row 2: A / R sliders — each row has a label, slider, and readout.
  section.appendChild(buildParamRow('Attack',  state.mpe.attack,
    0.0005, 2.0, (v) => formatSeconds(v),
    (v) => dispatcher.setMpeAttack(v), { log: true }));
  section.appendChild(buildParamRow('Release', state.mpe.release,
    0.001, 3.0, (v) => formatSeconds(v),
    (v) => dispatcher.setMpeRelease(v), { log: true }));

  // Row 3: Filter cutoff / resonance.
  section.appendChild(buildParamRow('Filter',  state.mpe.filterCutoffHz,
    50, 20000, (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setMpeFilterCutoff(v), { log: true }));
  section.appendChild(buildParamRow('Q',       state.mpe.filterResonance,
    0.707, 5.0, (v) => v.toFixed(2),
    (v) => dispatcher.setMpeFilterResonance(v)));

  // Row 4: LFO.
  section.appendChild(buildLfoRow(state, dispatcher));

  // Row 5: Volume + FX send.
  section.appendChild(buildParamRow('Volume',  state.mpe.volume,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setMpeVolume(v)));
  section.appendChild(buildParamRow('FX Send', state.mpe.fxSend,
    0, 1, (v) => v.toFixed(2),
    (v) => dispatcher.setMpeFxSend(v)));

  // Row 6: Master channel picker — 16 pills with a crown on active.
  section.appendChild(buildMasterChannelPicker(state, dispatcher));

  return section;
}

function buildWaveformPicker(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row mpe-wave-row';
  const label = document.createElement('label');
  label.textContent = 'Wave';
  row.appendChild(label);

  const tiles: HTMLButtonElement[] = [];
  for (let i = 0; i < 4; i++) {
    const tile = document.createElement('button');
    tile.type = 'button';
    tile.className = 'mpe-wave-tile';
    tile.title = WAVEFORM_LABELS[i];
    tile.appendChild(waveformGlyph(i, 32, 20));
    tile.addEventListener('click', () => dispatcher.setMpeWaveform(i));
    tiles.push(tile);
    row.appendChild(tile);
  }
  state.mpe.waveform.subscribe((w) => {
    tiles.forEach((t, i) => t.classList.toggle('active', i === w));
  });
  return row;
}

interface ParamRowOpts {
  log?: boolean;   // logarithmic slider (attack/release/filter cutoff)
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
  // For log sliders: slider 0..1000 maps to log-spaced range. Plain
  // sliders use the raw min..max with small step.
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

  // Rate slider.
  const rate = document.createElement('input');
  rate.type = 'range';
  rate.min = '0';
  rate.max = '20';
  rate.step = '0.1';
  state.mpe.lfoRate.subscribe((v) => { rate.value = String(v); });
  rate.addEventListener('input', () => dispatcher.setMpeLfoRate(parseFloat(rate.value)));
  const rateReadout = document.createElement('span');
  rateReadout.className = 'mpe-readout';
  state.mpe.lfoRate.subscribe((v) => { rateReadout.textContent = `${v.toFixed(1)} Hz`; });

  // Depth slider.
  const depth = document.createElement('input');
  depth.type = 'range';
  depth.min = '0';
  depth.max = '1';
  depth.step = '0.01';
  state.mpe.lfoDepth.subscribe((v) => { depth.value = String(v); });
  depth.addEventListener('input', () => dispatcher.setMpeLfoDepth(parseFloat(depth.value)));
  const depthReadout = document.createElement('span');
  depthReadout.className = 'mpe-readout';
  state.mpe.lfoDepth.subscribe((v) => { depthReadout.textContent = v.toFixed(2); });

  // Dest segmented selector.
  const destGroup = document.createElement('div');
  destGroup.className = 'mpe-segmented';
  const destBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_DEST_LABELS.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_DEST_LABELS[i];
    b.addEventListener('click', () => dispatcher.setMpeLfoDest(i));
    destBtns.push(b);
    destGroup.appendChild(b);
  }
  state.mpe.lfoDest.subscribe((d) => {
    destBtns.forEach((b, i) => b.classList.toggle('active', i === d));
  });

  // LFO waveform (smaller — just 4 tiles).
  const waveGroup = document.createElement('div');
  waveGroup.className = 'mpe-segmented';
  const waveBtns: HTMLButtonElement[] = [];
  for (let i = 0; i < LFO_WAVEFORM_LABELS.length; i++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = LFO_WAVEFORM_LABELS[i];
    b.title = LFO_WAVEFORM_LABELS[i];
    b.addEventListener('click', () => dispatcher.setMpeLfoWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.mpe.lfoWaveform.subscribe((w) => {
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
  // Swap to column layout via class; parent .mpe-row is row-flex,
  // so we reset here.
  row.classList.add('mpe-row-col');
  return row;
}

function buildMasterChannelPicker(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'mpe-row';
  const label = document.createElement('label');
  label.textContent = 'Master';
  label.title = 'MPE master channel — 1..16: global messages (mod/sustain) go here, notes ignored. None: no master, notes on any channel play the synth (for non-MPE controllers)';
  row.appendChild(label);

  const pills = document.createElement('div');
  pills.className = 'mpe-channel-pills';
  // Pill 0 = "None" (no master channel). Then 1..16 for standard MPE.
  const pillBtns: HTMLButtonElement[] = [];
  const noneBtn = document.createElement('button');
  noneBtn.type = 'button';
  noneBtn.className = 'mpe-channel-pill';
  noneBtn.textContent = '—';
  noneBtn.title = 'No master channel — all channels allocate voices';
  noneBtn.addEventListener('click', () => dispatcher.setMpeMasterChannel(0));
  pillBtns.push(noneBtn);
  pills.appendChild(noneBtn);
  for (let ch = 1; ch <= 16; ch++) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'mpe-channel-pill';
    b.textContent = String(ch);
    b.addEventListener('click', () => dispatcher.setMpeMasterChannel(ch));
    pillBtns.push(b);
    pills.appendChild(b);
  }
  state.mpe.masterChannel.subscribe((c) => {
    // pillBtns[0] is the "None" (0) pill; pillBtns[i] for i>=1 is channel i.
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

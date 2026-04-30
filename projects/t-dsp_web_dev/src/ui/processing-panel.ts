// Processing tab — main-bus output treatment. Shelf + peak limiter
// today; room for future modules (full EQ, reverb send, multi-band
// compressor) on the same page.
//
// Signal flow (firmware side):
//   mainAmp → hostvolAmp → shelf → limiter → DAC
// USB capture taps off before hostvol, so recordings are unaffected
// by these controls — they only protect the physical DAC / head-
// phones output.

import { Dispatcher } from '../dispatcher';
import { MixerState } from '../state';

// Shelf presets — ordered lightest to heaviest. Gain is cut (negative
// dB); positive values would add brightness, which is the opposite of
// what this control exists to do. Frequencies were picked to target
// the parts of the spectrum most often responsible for fatigue on FM
// and bright digital sources:
//   ~6 kHz : DX-era sizzle + cymbal hash
//   ~5 kHz : general harshness (presence band)
//   ~4 kHz : "in your face" midrange
//   ~3 kHz : aggressive darkening for fatigued ears
interface ShelfPreset {
  name:   string;
  freqHz: number;
  gainDb: number;
}
const SHELF_PRESETS: ShelfPreset[] = [
  { name: 'Gentle', freqHz: 6000, gainDb: -2  },
  { name: 'Warm',   freqHz: 5000, gainDb: -4  },
  { name: 'Dark',   freqHz: 4000, gainDb: -8  },
  { name: 'Dull',   freqHz: 3000, gainDb: -12 },
];

function module_(title: string): { section: HTMLElement; body: HTMLElement } {
  const section = document.createElement('section');
  section.className = 'proc-module';
  const header = document.createElement('h3');
  header.textContent = title;
  const body = document.createElement('div');
  body.className = 'proc-module-body';
  section.append(header, body);
  return { section, body };
}

function row(label: string, control: HTMLElement, readout?: HTMLElement): HTMLElement {
  const r = document.createElement('div');
  r.className = 'proc-row';
  const l = document.createElement('label');
  l.textContent = label;
  r.append(l, control);
  if (readout) r.append(readout);
  return r;
}

function toggle(label: string): HTMLInputElement {
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.className = 'proc-toggle';
  input.setAttribute('aria-label', label);
  return input;
}

function slider(min: number, max: number, step: number): HTMLInputElement {
  const s = document.createElement('input');
  s.type = 'range';
  s.min = String(min);
  s.max = String(max);
  s.step = String(step);
  return s;
}

export function processingPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'processing-panel';

  // ---- High-shelf module ----
  const shelf = module_('High Shelf (tame brightness)');
  const shelfEnable = toggle('Enable high-shelf EQ');
  const shelfFreq   = slider(500, 18000, 50);
  const shelfGain   = slider(-18, 0, 0.5);
  const freqReadout = document.createElement('span');
  const gainReadout = document.createElement('span');
  freqReadout.className = 'proc-readout';
  gainReadout.className = 'proc-readout';
  // Preset row — one-click tone curves. Clicking a button fires both
  // setProcShelfFreq and setProcShelfGain so the firmware applies the
  // pair atomically from its perspective; the echoes come back and
  // drive the sliders. The button for the currently-matching preset
  // gets highlighted via `.active` so the user can see which preset
  // (if any) their current slider positions correspond to.
  const presetRow = document.createElement('div');
  presetRow.className = 'proc-preset-row';
  const presetLabel = document.createElement('label');
  presetLabel.textContent = 'Preset';
  const presetButtons = document.createElement('div');
  presetButtons.className = 'proc-preset-buttons';
  const presetEls = SHELF_PRESETS.map((p) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'proc-preset';
    b.textContent = p.name;
    b.title = `${p.gainDb.toFixed(0)} dB @ ${p.freqHz < 1000 ? p.freqHz + ' Hz' : (p.freqHz / 1000) + ' kHz'}`;
    b.addEventListener('click', () => {
      dispatcher.setProcShelfFreq(p.freqHz);
      dispatcher.setProcShelfGain(p.gainDb);
    });
    presetButtons.appendChild(b);
    return { preset: p, el: b };
  });
  presetRow.append(presetLabel, presetButtons);

  // Recompute the "active" button whenever the freq or gain changes.
  // Floating-point equality is safe here because the presets are
  // integer-valued and the firmware echoes floats at the same exact
  // values we sent — but we use a small epsilon anyway in case the
  // slider gets the user to an adjacent value.
  const syncPresetHighlight = (): void => {
    const f = state.processing.shelfFreqHz.get();
    const g = state.processing.shelfGainDb.get();
    for (const { preset, el } of presetEls) {
      const matches = Math.abs(preset.freqHz - f) < 1 && Math.abs(preset.gainDb - g) < 0.1;
      el.classList.toggle('active', matches);
    }
  };

  shelf.body.append(
    row('Enable', shelfEnable),
    presetRow,
    row('Frequency', shelfFreq, freqReadout),
    row('Gain', shelfGain, gainReadout),
  );

  state.processing.shelfEnable.subscribe((v) => {
    shelfEnable.checked = v;
    shelfFreq.disabled = !v;
    shelfGain.disabled = !v;
  });
  state.processing.shelfFreqHz.subscribe((v) => {
    shelfFreq.value = String(v);
    freqReadout.textContent = v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${v.toFixed(0)} Hz`;
    syncPresetHighlight();
  });
  state.processing.shelfGainDb.subscribe((v) => {
    shelfGain.value = String(v);
    gainReadout.textContent = `${v.toFixed(1)} dB`;
    syncPresetHighlight();
  });

  shelfEnable.addEventListener('change', () => dispatcher.setProcShelfEnable(shelfEnable.checked));
  shelfFreq.addEventListener('input', () => dispatcher.setProcShelfFreq(parseFloat(shelfFreq.value)));
  shelfGain.addEventListener('input', () => dispatcher.setProcShelfGain(parseFloat(shelfGain.value)));

  // ---- Peak limiter module ----
  const lim = module_('Peak Limiter (ear-safety ceiling)');
  const limEnable = toggle('Enable peak limiter');
  const limNote = document.createElement('p');
  limNote.className = 'proc-note';
  limNote.textContent =
    'Soft-clip ceiling at about -3 dBFS. Caps the DAC output so sudden loud ' +
    'notes can\'t spike past the limit regardless of upstream fader or ' +
    'Windows volume. Leaves USB capture unaffected.';
  lim.body.append(row('Enable', limEnable), limNote);

  state.processing.limiterEnable.subscribe((v) => {
    limEnable.checked = v;
  });
  limEnable.addEventListener('change', () => dispatcher.setProcLimiterEnable(limEnable.checked));

  root.append(shelf.section, lim.section);
  return root;
}

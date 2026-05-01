// MPE slot panel — config UI for slot 3 (MPE-aware virtual analog).
//
// New panel built for the slot-architecture rebuild. Layout follows
// sampler-panel.ts: ON/OFF header, then a vertical stack of labeled
// rows for the slot housekeeping (volume, MPE master channel) and the
// engine knobs (waveform, attack/release, filter cutoff/Q, LFO).
//
// All controls round-trip through the firmware via OSC; optimistic
// local-signal updates make the UI feel responsive, and incoming
// echoes through dispatcher.handleIncoming() re-apply (idempotent
// when the value matches).
//
// The legacy mpe-panel.ts (richer — voice orbs + preset grid) is left
// in place untouched per the parallel-agent contract. Once the user
// confirms this slim replacement is what they want, the legacy file
// can be deleted; if the orbs/presets are missed, port them in here
// following the same sub-component shape.

import { Dispatcher } from '../dispatcher';
import { MixerState, Signal } from '../state';

const WAVEFORM_LABELS = ['Saw', 'Square', 'Tri', 'Sine'] as const;
const LFO_DEST_LABELS = ['Off', 'Cutoff', 'Pitch', 'Amp'] as const;
const LFO_WAVE_LABELS = ['Sine', 'Tri', 'Saw', 'Square'] as const;

export function mpeSlotPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'mpe-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  state.mpe.on.subscribe((on) => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute MPE output' : 'Click to un-mute MPE output';
  });
  onBtn.addEventListener('click', () => {
    dispatcher.setMpeOn(!state.mpe.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'MPE VA';
  onRow.append(onBtn, onLabel);

  // ---- Volume slider --------------------------------------------------
  const volRow = buildSliderRow(
    'Volume',
    state.mpe.volume,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setMpeVolume(v),
  );

  // ---- MPE Mode toggle + collapsible master channel picker -----------
  // Off  → master=0: every channel 1..16 allocates voices (plain-keyboard
  //                  friendly; per-channel pitch bend / pressure / CC74
  //                  still routes per voice, so a real MPE controller
  //                  also works — just with no "global" channel).
  // On   → master=N (default 1): standard MPE — notes on N ignored,
  //                  only mod wheel + sustain there.
  // Channel picker is hidden until MPE Mode is on; even then defaults
  // to ch 1 since virtually every MPE controller uses ch 1.
  const mpeModeRow = document.createElement('div');
  mpeModeRow.className = 'sampler-control-row';
  const mpeModeLabel = document.createElement('label');
  mpeModeLabel.textContent = 'MPE Mode';
  mpeModeLabel.title = 'On: standard MPE (per-finger expression on member channels, master channel reserved for global mod/sustain). Off: every channel allocates voices — works with any keyboard, including the on-screen one.';
  const mpeModeToggle = document.createElement('input');
  mpeModeToggle.type = 'checkbox';
  mpeModeToggle.className = 'sampler-toggle';
  mpeModeRow.append(mpeModeLabel, mpeModeToggle);

  const masterRow = document.createElement('div');
  masterRow.className = 'sampler-control-row';
  const masterLabel = document.createElement('label');
  masterLabel.textContent = 'Master ch';
  masterLabel.title = 'MPE master channel — global mod wheel / sustain go here, notes on this channel are ignored. Almost every MPE controller uses channel 1.';
  const masterSelect = document.createElement('select');
  masterSelect.className = 'sampler-midi-channel';
  for (let ch = 1; ch <= 16; ++ch) {
    const opt = document.createElement('option');
    opt.value = String(ch);
    opt.textContent = `Channel ${ch}`;
    masterSelect.appendChild(opt);
  }
  masterRow.append(masterLabel, masterSelect);

  // Track the user's last MPE master selection so toggling Off→On
  // restores their pick (default ch 1 if they've never set one).
  let lastMpeMaster = 1;
  state.mpe.masterChannel.subscribe((ch) => {
    const mpeOn = ch !== 0;
    mpeModeToggle.checked = mpeOn;
    masterRow.style.display = mpeOn ? '' : 'none';
    if (mpeOn) {
      lastMpeMaster = ch;
      masterSelect.value = String(ch);
    }
  });
  mpeModeToggle.addEventListener('change', () => {
    dispatcher.setMpeMasterChannel(mpeModeToggle.checked ? lastMpeMaster : 0);
  });
  masterSelect.addEventListener('change', () => {
    dispatcher.setMpeMasterChannel(parseInt(masterSelect.value, 10));
  });

  // ---- Waveform picker (4 segmented tiles) ---------------------------
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
    b.addEventListener('click', () => dispatcher.setMpeWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.mpe.waveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });
  waveRow.append(waveLabel, waveGroup);

  // ---- Envelope (attack / release) ------------------------------------
  const attackRow = buildSliderRow(
    'Attack',
    state.mpe.attack,
    0.0005, 2.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setMpeAttack(v),
    { log: true },
  );
  const releaseRow = buildSliderRow(
    'Release',
    state.mpe.release,
    0.001, 3.0,
    (v) => v < 1 ? `${(v * 1000).toFixed(0)}ms` : `${v.toFixed(2)}s`,
    (v) => dispatcher.setMpeRelease(v),
    { log: true },
  );

  // ---- Filter (cutoff / resonance) ------------------------------------
  const cutoffRow = buildSliderRow(
    'Cutoff',
    state.mpe.filterCutoffHz,
    50, 20000,
    (v) => `${Math.round(v)} Hz`,
    (v) => dispatcher.setMpeFilterCutoff(v),
    { log: true },
  );
  const resoRow = buildSliderRow(
    'Q',
    state.mpe.filterResonance,
    0.707, 5.0,
    (v) => v.toFixed(2),
    (v) => dispatcher.setMpeFilterResonance(v),
  );

  // ---- LFO row (rate + depth + dest + waveform) -----------------------
  const lfoRow = buildLfoRow(state, dispatcher);

  root.append(
    onRow,
    volRow,
    mpeModeRow,
    masterRow,
    waveRow,
    attackRow,
    releaseRow,
    cutoffRow,
    resoRow,
    lfoRow,
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

function buildLfoRow(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'mpe-lfo-block';

  // Rate slider (0..20 Hz, linear).
  wrap.append(buildSliderRow(
    'LFO rate',
    state.mpe.lfoRate,
    0, 20,
    (v) => `${v.toFixed(1)} Hz`,
    (v) => dispatcher.setMpeLfoRate(v),
  ));
  // Depth slider (0..1, linear).
  wrap.append(buildSliderRow(
    'LFO depth',
    state.mpe.lfoDepth,
    0, 1,
    (v) => v.toFixed(2),
    (v) => dispatcher.setMpeLfoDepth(v),
  ));

  // Destination + waveform — segmented pickers in a single row each.
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
    b.addEventListener('click', () => dispatcher.setMpeLfoDest(i));
    destBtns.push(b);
    destGroup.appendChild(b);
  }
  state.mpe.lfoDest.subscribe((d) => {
    destBtns.forEach((b, i) => b.classList.toggle('active', i === d));
  });
  destRow.append(destLabel, destGroup);
  wrap.append(destRow);

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
    b.addEventListener('click', () => dispatcher.setMpeLfoWaveform(i));
    waveBtns.push(b);
    waveGroup.appendChild(b);
  }
  state.mpe.lfoWaveform.subscribe((w) => {
    waveBtns.forEach((b, i) => b.classList.toggle('active', i === w));
  });
  waveRow.append(waveLabel, waveGroup);
  wrap.append(waveRow);

  return wrap;
}

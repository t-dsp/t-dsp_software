// Beats drum machine panel.
//
// Layout:
//   ┌ Transport bar ────────────────────────────────────────────────┐
//   │ [▶/■]  BPM [120] —  Swing [0.00] —  Vol [0.70] —  SD ✓ ─ Clk  │
//   ├ Track rows ───────────────────────────────────────────────────┤
//   │ ██ Kick  [M]          │ [s0][s1][s2][s3] [s4]… 16 cells       │
//   │ ██ Snare [M]          │ …                                     │
//   │ ██ Hat   [M] [HAT.WAV]│ …                                     │
//   │ ██ Perc  [M] [CLAP.  ]│ …                                     │
//   ├ Bottom ───────────────────────────────────────────────────────┤
//   │ [Clear All]                                                   │
//   └───────────────────────────────────────────────────────────────┘
//
// Step cells:
//   * Click to toggle on/off.
//   * Right-click opens an inline velocity scrubber for that step
//     (slider from 0..1, echoes as /beats/vel).
//   * Active cells are orange-filled; velocity shades the fill
//     brightness (vel=0 → barely visible, vel=1 → full intensity).
//   * The playhead (state.beats.cursor) gets a white border ring.
//   * Every 4 cells: a ~4px gap separator for beat markers.
//
// Transport:
//   * ▶ toggles /beats/run. Button shows ■ while running.
//   * BPM / Swing / Volume are live-dragged sliders with numeric readouts.
//   * SD indicator is read-only (✓ or ✗).
//   * Clock source is a two-button toggle: [Int] [Ext].

import { Dispatcher } from '../dispatcher';
import { MixerState, BEATS_STEP_COUNT, Signal } from '../state';
import { BEATS_PRESETS, BeatsPreset } from './beats-presets';

export function beatsPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'beats-panel';

  // -------- Transport bar --------
  const transport = document.createElement('div');
  transport.className = 'beats-transport';

  const playBtn = document.createElement('button');
  playBtn.type = 'button';
  playBtn.className = 'beats-play';
  const updatePlayBtn = (running: boolean): void => {
    playBtn.textContent = running ? '■' : '▶';
    playBtn.classList.toggle('active', running);
    playBtn.title = running ? 'Stop (/beats/run 0)' : 'Play (/beats/run 1)';
  };
  state.beats.running.subscribe(updatePlayBtn);
  playBtn.addEventListener('click', () => {
    dispatcher.setBeatsRun(!state.beats.running.get());
  });

  const bpmGroup    = numericSlider('BPM',    20,   300,  1,    state.beats.bpm,
    (v) => dispatcher.setBeatsBpm(v),    (v) => v.toFixed(0));
  const swingGroup  = numericSlider('Swing',  0,    0.75, 0.01, state.beats.swing,
    (v) => dispatcher.setBeatsSwing(v),  (v) => v.toFixed(2));
  const volGroup    = numericSlider('Vol',    0,    1,    0.01, state.beats.volume,
    (v) => dispatcher.setBeatsVolume(v), (v) => v.toFixed(2));

  const sdIndicator = document.createElement('span');
  sdIndicator.className = 'beats-sd';
  sdIndicator.title = 'SD card status (read-only).';
  state.beats.sdReady.subscribe((r) => {
    sdIndicator.textContent = r ? 'SD ✓' : 'SD ✗';
    sdIndicator.classList.toggle('ok', r);
    sdIndicator.classList.toggle('missing', !r);
  });

  const clkGroup = document.createElement('div');
  clkGroup.className = 'beats-clock';
  const clkLabel = document.createElement('span');
  clkLabel.className = 'beats-label';
  clkLabel.textContent = 'Clk';
  const clkInt = document.createElement('button');
  clkInt.type = 'button';
  clkInt.className = 'beats-clock-btn';
  clkInt.textContent = 'Int';
  clkInt.title = 'Internal clock — firmware BPM drives steps.';
  const clkExt = document.createElement('button');
  clkExt.type = 'button';
  clkExt.className = 'beats-clock-btn';
  clkExt.textContent = 'Ext';
  clkExt.title = 'External MIDI clock — slaves to F8 pulses on USB host.';
  state.beats.clockSource.subscribe((src) => {
    clkInt.classList.toggle('active', src === 'internal');
    clkExt.classList.toggle('active', src === 'external');
  });
  clkInt.addEventListener('click', () => dispatcher.setBeatsClockSource('internal'));
  clkExt.addEventListener('click', () => dispatcher.setBeatsClockSource('external'));
  clkGroup.append(clkLabel, clkInt, clkExt);

  transport.append(playBtn, bpmGroup, swingGroup, volGroup, sdIndicator, clkGroup);

  // -------- Preset bar --------
  // Click a button to load the whole pattern + BPM + swing. The D&B
  // presets sit in a separated group on the right, divided from the
  // 8 general-style presets by a visual gap (CSS: .beats-preset-divider).
  const presetBar = document.createElement('div');
  presetBar.className = 'beats-presets';
  const presetLabel = document.createElement('span');
  presetLabel.className = 'beats-label';
  presetLabel.textContent = 'Presets';
  presetBar.appendChild(presetLabel);

  // Count the non-D&B presets so we know where to place the divider.
  const dnbStart = BEATS_PRESETS.findIndex((p) => p.id.startsWith('dnb-'));

  for (let i = 0; i < BEATS_PRESETS.length; i++) {
    if (i === dnbStart && dnbStart > 0) {
      const divider = document.createElement('span');
      divider.className = 'beats-preset-divider';
      presetBar.appendChild(divider);
    }
    presetBar.appendChild(presetButton(BEATS_PRESETS[i], dispatcher));
  }

  // -------- Track rows --------
  const grid = document.createElement('div');
  grid.className = 'beats-grid';

  for (let t = 0; t < state.beats.tracks.length; t++) {
    grid.appendChild(trackRow(t, state, dispatcher));
  }

  // -------- Bottom bar --------
  const bottom = document.createElement('div');
  bottom.className = 'beats-bottom';
  const clearAll = document.createElement('button');
  clearAll.type = 'button';
  clearAll.className = 'beats-clear-all';
  clearAll.textContent = 'Clear All';
  clearAll.title = 'Clear every step on every track.';
  clearAll.addEventListener('click', () => dispatcher.clearBeatsTrack(-1));
  bottom.appendChild(clearAll);

  root.append(transport, presetBar, grid, bottom);
  return root;
}

// One preset button. Label = preset.name, tooltip shows BPM + swing.
function presetButton(preset: BeatsPreset, dispatcher: Dispatcher): HTMLElement {
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'beats-preset';
  btn.textContent = preset.name;
  btn.title = `${preset.name} — ${preset.bpm} BPM${preset.swing > 0 ? `, swing ${preset.swing.toFixed(2)}` : ''}`;
  btn.addEventListener('click', () => dispatcher.applyBeatsPreset(preset));
  return btn;
}

// One track row: track header + 16 step cells.
function trackRow(track: number, state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const row = document.createElement('div');
  row.className = 'beats-row';

  const header = document.createElement('div');
  header.className = 'beats-track-header';

  const nameEl = document.createElement('span');
  nameEl.className = 'beats-track-name';
  nameEl.textContent = state.beats.tracks[track].name;
  header.appendChild(nameEl);

  const muteBtn = document.createElement('button');
  muteBtn.type = 'button';
  muteBtn.className = 'beats-mute';
  muteBtn.textContent = 'M';
  muteBtn.title = 'Mute this track.';
  state.beats.tracks[track].muted.subscribe((m) => {
    muteBtn.classList.toggle('active', m);
  });
  muteBtn.addEventListener('click', () => {
    dispatcher.setBeatsTrackMute(track, !state.beats.tracks[track].muted.get());
  });
  header.appendChild(muteBtn);

  const clearBtn = document.createElement('button');
  clearBtn.type = 'button';
  clearBtn.className = 'beats-track-clear';
  clearBtn.textContent = '∅';
  clearBtn.title = 'Clear this track.';
  clearBtn.addEventListener('click', () => dispatcher.clearBeatsTrack(track));
  header.appendChild(clearBtn);

  // Sample filename input (only for sample tracks; synth tracks skip it)
  if (state.beats.tracks[track].isSample) {
    const sampleInput = document.createElement('input');
    sampleInput.type = 'text';
    sampleInput.className = 'beats-sample';
    sampleInput.placeholder = 'FILE.WAV';
    sampleInput.title = 'SD filename for this track. Press Enter to apply.';
    state.beats.tracks[track].sample.subscribe((s) => {
      if (document.activeElement !== sampleInput) sampleInput.value = s;
    });
    sampleInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        dispatcher.setBeatsSample(track, sampleInput.value.trim());
        sampleInput.blur();
      }
    });
    header.appendChild(sampleInput);
  }

  row.appendChild(header);

  // Step cells
  const cells = document.createElement('div');
  cells.className = 'beats-cells';
  for (let s = 0; s < BEATS_STEP_COUNT; s++) {
    cells.appendChild(stepCell(track, s, state, dispatcher));
  }
  row.appendChild(cells);

  return row;
}

// One step cell. Left-click = toggle on/off; right-click (or long-press on
// touch) opens an inline velocity scrubber that mutates stepsVel[s].
function stepCell(track: number, step: number, state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const cell = document.createElement('button');
  cell.type = 'button';
  cell.className = 'beats-cell';
  // Visual beat-marker gap every 4 cells via a class on the 4th/8th/12th cell
  if (step > 0 && step % 4 === 0) cell.classList.add('beats-cell-beatgap');
  cell.dataset.track = String(track);
  cell.dataset.step  = String(step);

  const trk = state.beats.tracks[track];

  // Render state -> visual: background opacity scales with velocity.
  const render = (): void => {
    const on  = trk.stepsOn[step].get();
    const vel = trk.stepsVel[step].get();
    cell.classList.toggle('on', on);
    if (on) {
      // Floor at 0.25 so even a low-velocity "on" cell is clearly
      // distinguishable from an off cell.
      const alpha = Math.max(0.25, vel);
      cell.style.setProperty('--cell-alpha', String(alpha));
    } else {
      cell.style.removeProperty('--cell-alpha');
    }
  };
  trk.stepsOn[step].subscribe(render);
  trk.stepsVel[step].subscribe(render);

  // Playhead highlight — drawn on whichever cell matches cursor.
  state.beats.cursor.subscribe((c) => {
    cell.classList.toggle('playhead', c === step);
  });

  cell.addEventListener('click', (e) => {
    e.preventDefault();
    dispatcher.setBeatsStep(track, step, !trk.stepsOn[step].get());
  });

  // Right-click or shift-click opens a velocity slider popup.
  cell.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    openVelocityPopup(cell, track, step, state, dispatcher);
  });

  return cell;
}

function openVelocityPopup(
  anchor: HTMLElement,
  track: number,
  step: number,
  state: MixerState,
  dispatcher: Dispatcher,
): void {
  // Close any existing popup (singleton).
  document.querySelector('.beats-vel-popup')?.remove();

  const popup = document.createElement('div');
  popup.className = 'beats-vel-popup';

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = '0';
  slider.max = '1';
  slider.step = '0.01';
  slider.value = String(state.beats.tracks[track].stepsVel[step].get());

  const readout = document.createElement('span');
  readout.className = 'beats-vel-readout';
  readout.textContent = slider.value;

  slider.addEventListener('input', () => {
    const v = parseFloat(slider.value);
    readout.textContent = v.toFixed(2);
    dispatcher.setBeatsStepVel(track, step, v);
  });

  popup.append(slider, readout);
  document.body.appendChild(popup);

  // Position below the anchor cell.
  const rect = anchor.getBoundingClientRect();
  popup.style.left = `${rect.left + window.scrollX}px`;
  popup.style.top  = `${rect.bottom + window.scrollY + 4}px`;

  // Close on click-outside or ESC.
  const closer = (e: MouseEvent): void => {
    if (!popup.contains(e.target as Node) && e.target !== anchor) {
      popup.remove();
      document.removeEventListener('mousedown', closer);
      document.removeEventListener('keydown', keyCloser);
    }
  };
  const keyCloser = (e: KeyboardEvent): void => {
    if (e.key === 'Escape') {
      popup.remove();
      document.removeEventListener('mousedown', closer);
      document.removeEventListener('keydown', keyCloser);
    }
  };
  // Defer adding the listeners so the click that opened the popup
  // doesn't immediately close it.
  setTimeout(() => {
    document.addEventListener('mousedown', closer);
    document.addEventListener('keydown', keyCloser);
  }, 0);

  slider.focus();
}

// Helper: labeled slider + numeric readout row. The slider mirrors the
// given Signal; input events call onChange. Mounted as a flex group so
// several of these sit nicely in the transport bar.
function numericSlider(
  label: string,
  min: number,
  max: number,
  step: number,
  signal: Signal<number>,
  onChange: (v: number) => void,
  format: (v: number) => string,
): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'beats-slider-group';

  const lbl = document.createElement('label');
  lbl.className = 'beats-label';
  lbl.textContent = label;

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = String(min);
  slider.max = String(max);
  slider.step = String(step);
  slider.className = 'beats-range';

  const readout = document.createElement('span');
  readout.className = 'beats-readout';

  signal.subscribe((v) => {
    // Don't clobber the slider if the user is dragging it (their input
    // already drove this update). Checked by document.activeElement.
    if (document.activeElement !== slider) slider.value = String(v);
    readout.textContent = format(v);
  });

  slider.addEventListener('input', () => {
    onChange(parseFloat(slider.value));
  });

  wrap.append(lbl, slider, readout);
  return wrap;
}

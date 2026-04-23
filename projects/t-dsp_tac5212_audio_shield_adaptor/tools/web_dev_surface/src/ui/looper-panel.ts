// Loop tab — mono sample looper fed off a selectable pre-fader channel
// tap. One recorded take, up to the firmware's buffer capacity (~2 s on
// the stock build; minutes once PSRAM is soldered).
//
// Signal flow (firmware side):
//   ch[1..6] pre-fader ─ loopSrcMux ─ tdsp::Looper ─ postMix[1] ─ main
// The live channel signal still reaches the main mix via the normal
// strip — the looper's output is additive. Fader down the strip while
// a loop plays and you hear only the recorded take.
//
// Transport semantics:
//   REC       arm & start recording from the selected source
//   PLAY      finalize length if still recording, loop from 0
//   STOP      halt playback (buffer preserved)
//   CLEAR     wipe the take, back to Idle
// All four actions are idempotent on the firmware side, so clicking
// them in any order is safe.

import { Dispatcher } from '../dispatcher';
import { MixerState } from '../state';

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

export function looperPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'processing-panel';

  const mod = module_('Looper');

  // ---- Source dropdown ----
  //
  // Options mirror state.channels[*].name live so a firmware rename
  // echo updates the label without re-rendering the dropdown.
  const sel = document.createElement('select');
  sel.className = 'looper-source';
  const noneOpt = document.createElement('option');
  noneOpt.value = '0';
  noneOpt.textContent = '— none —';
  sel.appendChild(noneOpt);
  for (let i = 0; i < state.channels.length; i++) {
    const opt = document.createElement('option');
    opt.value = String(i + 1);
    const idx = i + 1;
    opt.textContent = `${idx}. ${state.channels[i].name.get()}`;
    sel.appendChild(opt);
    state.channels[i].name.subscribe((n) => {
      opt.textContent = `${idx}. ${n}`;
    });
  }
  // "Synth" — aggregate synth bus (Dexed + MPE + FX wet return), tapped
  // post-fader. Value 7 on the wire; firmware's applyLooperSource()
  // routes loopSrcB slot 3 from synthAmpL.
  const synthOpt = document.createElement('option');
  synthOpt.value = '7';
  synthOpt.textContent = '7. Synth bus';
  sel.appendChild(synthOpt);
  state.looper.source.subscribe((s) => {
    sel.value = String(s);
  });
  sel.addEventListener('change', () => {
    dispatcher.setLooperSource(parseInt(sel.value, 10));
  });

  // ---- Transport buttons ----
  const bar = document.createElement('div');
  bar.className = 'looper-transport';
  const mkBtn = (label: string, kind: string): HTMLButtonElement => {
    const b = document.createElement('button');
    b.className = `looper-btn looper-btn-${kind}`;
    b.textContent = label;
    return b;
  };
  const recBtn   = mkBtn('REC',   'rec');
  const playBtn  = mkBtn('PLAY',  'play');
  const stopBtn  = mkBtn('STOP',  'stop');
  const clearBtn = mkBtn('CLEAR', 'clear');
  bar.append(recBtn, playBtn, stopBtn, clearBtn);
  recBtn.addEventListener('click',   () => dispatcher.looperRecord());
  playBtn.addEventListener('click',  () => dispatcher.looperPlay());
  stopBtn.addEventListener('click',  () => dispatcher.looperStop());
  clearBtn.addEventListener('click', () => dispatcher.looperClear());

  // ---- Status readout (state + length) ----
  const status = document.createElement('div');
  status.className = 'looper-status';
  const stateLabel = document.createElement('span');
  stateLabel.className = 'looper-state';
  const lengthLabel = document.createElement('span');
  lengthLabel.className = 'looper-length';
  status.append(stateLabel, lengthLabel);

  state.looper.transport.subscribe((st) => {
    stateLabel.textContent = st.toUpperCase();
    stateLabel.className = `looper-state looper-state-${st}`;
    recBtn.classList.toggle('active',  st === 'rec');
    playBtn.classList.toggle('active', st === 'play');
  });
  state.looper.length.subscribe((s) => {
    lengthLabel.textContent = s > 0 ? `${s.toFixed(2)} s` : '—';
  });

  // ---- Return level slider ----
  const level = document.createElement('input');
  level.type = 'range';
  level.min = '0';
  level.max = '1';
  level.step = '0.01';
  const levelRO = document.createElement('span');
  levelRO.className = 'proc-readout';
  state.looper.level.subscribe((v) => {
    level.value = String(v);
    levelRO.textContent = v.toFixed(2);
  });
  level.addEventListener('input', () => {
    dispatcher.setLooperLevel(parseFloat(level.value));
  });

  const note = document.createElement('p');
  note.className = 'proc-note';
  note.textContent =
    'Pre-fader source tap — the loop plays independently of the channel fader, ' +
    'so bringing the strip down leaves only the recorded take audible. ' +
    'Buffer caps around 2 s on the stock Teensy 4.1; soldering an APS6404L ' +
    'PSRAM into the first footprint unlocks tens of seconds.';

  mod.body.append(
    row('Source', sel),
    bar,
    status,
    row('Return', level, levelRO),
    note,
  );

  root.appendChild(mod.section);
  return root;
}

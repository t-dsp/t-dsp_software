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

  // ---- Cue/Go row ----
  //
  // CUE REC stages a record without firing; GO commits whichever action
  // is cued (rec/play/stop/clear), and the firmware aligns to the next
  // beat edge if the clock is running. Clicking CUE REC a second time
  // with REC already cued cancels the cue.
  const cueBar = document.createElement('div');
  cueBar.className = 'looper-transport looper-cue-bar';
  const cueRecBtn = mkBtn('CUE REC', 'cue-rec');
  const cuePlayBtn = mkBtn('CUE PLAY', 'cue-play');
  const goBtn    = mkBtn('GO',      'go');
  cueBar.append(cueRecBtn, cuePlayBtn, goBtn);
  cueRecBtn.addEventListener('click', () => {
    // Toggle — if REC is already cued, clicking CUE REC cancels.
    const current = state.looper.cued.get();
    dispatcher.looperCue(current === 1 ? 0 : 1);
  });
  cuePlayBtn.addEventListener('click', () => {
    const current = state.looper.cued.get();
    dispatcher.looperCue(current === 2 ? 0 : 2);
  });
  goBtn.addEventListener('click', () => dispatcher.looperGo());

  // Highlight the currently-cued action; disable GO when nothing is cued
  // (visual cue that the button is a no-op in that state).
  state.looper.cued.subscribe((c) => {
    cueRecBtn.classList.toggle('active',  c === 1);
    cuePlayBtn.classList.toggle('active', c === 2);
    goBtn.classList.toggle('ready', c !== 0);
    goBtn.disabled = c === 0;
  });

  // ---- Status readout (state + length in seconds + length in beats) ----
  //
  // Beats readout uses the clock's live BPM — shows "—" when there's no
  // take or the clock isn't running. With Sync on, quantized loops should
  // land on a whole-number count (e.g. "4 beats"); free-recorded loops
  // read fractional.
  const status = document.createElement('div');
  status.className = 'looper-status';
  const stateLabel = document.createElement('span');
  stateLabel.className = 'looper-state';
  const lengthLabel = document.createElement('span');
  lengthLabel.className = 'looper-length';
  const beatsLabel = document.createElement('span');
  beatsLabel.className = 'looper-beats';
  status.append(stateLabel, lengthLabel, beatsLabel);

  state.looper.transport.subscribe((st) => {
    stateLabel.textContent = st.toUpperCase();
    stateLabel.className = `looper-state looper-state-${st}`;
    recBtn.classList.toggle('active',  st === 'rec');
    playBtn.classList.toggle('active', st === 'play');
  });
  state.looper.length.subscribe((s) => {
    lengthLabel.textContent = s > 0 ? `${s.toFixed(2)} s` : '—';
  });
  state.looper.lengthBeats.subscribe((b) => {
    if (b <= 0) {
      beatsLabel.textContent = '';
      return;
    }
    // Whole-number beats render without decimals — nicer to read when
    // the loop is snapped. Fractional beats get one decimal.
    const rounded = Math.round(b);
    const isWhole = Math.abs(b - rounded) < 0.02;
    beatsLabel.textContent = isWhole
      ? `${rounded} beat${rounded === 1 ? '' : 's'}`
      : `${b.toFixed(1)} beats`;
  });

  // ---- Sync (beat-aware) toggle ----
  //
  // When on, the firmware (1) arms REC/PLAY/STOP/CLEAR to fire on the
  // next beat edge, and (2) snaps the recorded length to a whole number
  // of beats on the record->play transition. Both behaviors require the
  // MIDI clock to be running; if it's stopped the firmware falls through
  // to immediate fire so the UI still responds.
  const sync = document.createElement('input');
  sync.type = 'checkbox';
  sync.className = 'looper-sync';
  state.looper.quantize.subscribe((on) => { sync.checked = on; });
  sync.addEventListener('change', () => {
    dispatcher.setLooperQuantize(sync.checked);
  });

  const armedLabel = document.createElement('span');
  armedLabel.className = 'looper-armed';
  const ARMED_NAMES: Record<number, string> = {
    1: 'REC', 2: 'PLAY', 3: 'STOP', 4: 'CLEAR',
  };
  // Show whichever of cued/armed is active. Armed wins if both are set
  // (shouldn't normally happen — GO moves cued -> armed — but if it does
  // the armed action is the more imminent one to surface).
  const refreshArmedLabel = () => {
    const armed = state.looper.armed.get();
    const cued  = state.looper.cued.get();
    if (armed > 0) {
      armedLabel.textContent = `armed: ${ARMED_NAMES[armed] ?? '?'}`;
    } else if (cued > 0) {
      armedLabel.textContent = `cued: ${ARMED_NAMES[cued] ?? '?'}`;
    } else {
      armedLabel.textContent = '';
    }
  };
  state.looper.armed.subscribe(refreshArmedLabel);
  state.looper.cued.subscribe(refreshArmedLabel);

  // ---- Clock-follow toggle ----
  //
  // When on and the clock tempo changes, playback rate scales so the
  // loop stays in sync with the new tempo. Pitch shifts with it (no
  // time-stretch — this is sample-rate scaling). The readout shows the
  // BPM at which the take was recorded so you can see what the current
  // ratio is relative to.
  const follow = document.createElement('input');
  follow.type = 'checkbox';
  follow.className = 'looper-follow';
  state.looper.clockFollow.subscribe((on) => { follow.checked = on; });
  follow.addEventListener('change', () => {
    dispatcher.setLooperClockFollow(follow.checked);
  });

  const recordedLabel = document.createElement('span');
  recordedLabel.className = 'looper-recorded';
  const updateRecordedLabel = () => {
    const rec = state.looper.recordedBpm.get();
    const cur = state.clock.bpm.get();
    if (rec <= 0) {
      recordedLabel.textContent = '';
      return;
    }
    const ratio = cur > 0 ? cur / rec : 1;
    const isUnity = Math.abs(ratio - 1) < 0.005;
    recordedLabel.textContent = isUnity
      ? `rec @ ${rec.toFixed(0)} BPM`
      : `rec @ ${rec.toFixed(0)} · ${ratio.toFixed(2)}×`;
  };
  state.looper.recordedBpm.subscribe(updateRecordedLabel);
  state.clock.bpm.subscribe(updateRecordedLabel);

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
    'PSRAM into the first footprint unlocks tens of seconds. ' +
    'Sync quantizes REC/PLAY to beat edges and snaps the recorded length ' +
    'to a whole number of beats (needs a running MIDI clock). ' +
    'CUE REC stages a record without firing; press GO at the musical ' +
    'moment and recording starts on the next beat. ' +
    'Follow clock scales playback rate with tempo changes — pitch shifts ' +
    'with it (like a DJ turntable; no time-stretch).';

  mod.body.append(
    row('Source', sel),
    bar,
    cueBar,
    status,
    row('Sync', sync, armedLabel),
    row('Follow clock', follow, recordedLabel),
    row('Return', level, levelRO),
    note,
  );

  root.appendChild(mod.section);
  return root;
}

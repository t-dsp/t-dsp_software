// Clock tab — the device's shared musical-time reference.
//
// One clock drives every tempo-aware consumer on the device: the looper's
// quantize-to-beat arm, tempo-synced LFOs (when wired), arpeggiators, and
// the beats drum machine (once its firmware honors the shared clock).
// Source selection:
//   ext    — slaved to an upstream sequencer's MIDI Timing Clock
//            (0xF8 at 24 PPQN). BPM reads as last-measured tempo; you
//            can't set it from here because the upstream is master.
//   int    — the device is master. Set BPM with the slider and every
//            tempo-aware module tracks that number. Handy when no
//            external controller is attached.
// Transport (running / stopped) reflects the last 0xFA / 0xFC / stall
// watchdog state.

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

export function clockPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'processing-panel';

  // ---- Status module --------------------------------------------------
  //
  // Big BPM readout + transport LED + beat-pulse indicator. Large text so
  // the engineer can read it across the room.
  const status = module_('Status');

  const bpmBig = document.createElement('div');
  bpmBig.className = 'clock-bpm-big';
  bpmBig.textContent = '—';
  state.clock.bpm.subscribe((v) => {
    // Show integer when tempo is steady (External often sits within 0.1
    // BPM of an integer target); show one decimal otherwise.
    const rounded = Math.round(v * 10) / 10;
    bpmBig.textContent = Number.isFinite(rounded) && v > 0
      ? (Math.abs(rounded - Math.round(rounded)) < 0.05
          ? String(Math.round(rounded))
          : rounded.toFixed(1))
      : '—';
  });

  const bpmSuffix = document.createElement('div');
  bpmSuffix.className = 'clock-bpm-suffix';
  bpmSuffix.textContent = 'BPM';

  const bpmBlock = document.createElement('div');
  bpmBlock.className = 'clock-bpm-block';
  bpmBlock.append(bpmBig, bpmSuffix);

  const transport = document.createElement('div');
  transport.className = 'clock-transport';
  const led = document.createElement('span');
  led.className = 'clock-led';
  const tLabel = document.createElement('span');
  tLabel.className = 'clock-transport-label';
  state.clock.running.subscribe((on) => {
    led.classList.toggle('on', on);
    tLabel.textContent = on ? 'RUNNING' : 'STOPPED';
  });
  transport.append(led, tLabel);

  status.body.append(bpmBlock, transport);

  // ---- Source module --------------------------------------------------
  //
  // Two radio-style buttons. External is the default on boot — if a
  // MIDI controller is sending 0xF8, the clock follows it. Switching
  // to Internal takes the device's BPM setpoint as authoritative.

  const src = module_('Source');

  const srcBar = document.createElement('div');
  srcBar.className = 'clock-source-bar';
  const mkSrc = (label: string, kind: 'ext' | 'int', note: string): HTMLButtonElement => {
    const b = document.createElement('button');
    b.className = 'clock-source-btn';
    b.dataset.kind = kind;
    const top = document.createElement('span');
    top.className = 'clock-source-label';
    top.textContent = label;
    const sub = document.createElement('span');
    sub.className = 'clock-source-sub';
    sub.textContent = note;
    b.append(top, sub);
    return b;
  };
  const extBtn = mkSrc('External', 'ext', 'Slave to MIDI clock');
  const intBtn = mkSrc('Internal', 'int', 'This device is master');
  srcBar.append(extBtn, intBtn);

  state.clock.source.subscribe((s) => {
    extBtn.classList.toggle('active', s === 'ext');
    intBtn.classList.toggle('active', s === 'int');
  });
  extBtn.addEventListener('click', () => dispatcher.setClockSource('ext'));
  intBtn.addEventListener('click', () => dispatcher.setClockSource('int'));

  src.body.append(srcBar);

  // ---- Tempo module ---------------------------------------------------
  //
  // BPM slider + nudge buttons. Active only in Internal mode — in
  // External the slider is disabled (but still displays the current
  // measured tempo) because the upstream is authoritative.

  const tempo = module_('Tempo');

  const bpm = document.createElement('input');
  bpm.type = 'range';
  bpm.min = '20';
  bpm.max = '300';
  bpm.step = '0.1';
  const bpmRO = document.createElement('span');
  bpmRO.className = 'proc-readout';
  state.clock.bpm.subscribe((v) => {
    bpm.value = String(v);
    bpmRO.textContent = v.toFixed(1);
  });
  state.clock.source.subscribe((s) => {
    const disabled = s !== 'int';
    bpm.disabled = disabled;
    bpm.classList.toggle('disabled', disabled);
  });
  bpm.addEventListener('input', () => {
    dispatcher.setClockBpm(parseFloat(bpm.value));
  });

  // ± 1 BPM nudges — finer than dragging the slider, same control that
  // every standalone drum machine shows. Also disabled in External mode.
  const nudge = document.createElement('div');
  nudge.className = 'clock-nudge';
  const mkNudge = (delta: number, label: string): HTMLButtonElement => {
    const b = document.createElement('button');
    b.className = 'clock-nudge-btn';
    b.textContent = label;
    b.addEventListener('click', () => {
      const cur = state.clock.bpm.get();
      const next = Math.max(20, Math.min(300, cur + delta));
      dispatcher.setClockBpm(next);
    });
    return b;
  };
  const minus1 = mkNudge(-1, '–1');
  const plus1  = mkNudge( 1, '+1');
  const minus5 = mkNudge(-5, '–5');
  const plus5  = mkNudge( 5, '+5');
  nudge.append(minus5, minus1, plus1, plus5);
  state.clock.source.subscribe((s) => {
    const disabled = s !== 'int';
    for (const b of nudge.querySelectorAll('.clock-nudge-btn')) {
      (b as HTMLButtonElement).disabled = disabled;
    }
  });

  tempo.body.append(row('BPM', bpm, bpmRO), nudge);

  // ---- Meter module ---------------------------------------------------
  //
  // Beats-per-bar selector. Downstream of the transport: a bar edge is
  // just every Nth beat edge, and quantize-to-bar consumers key off
  // this. Most patterns are 4/4; offering 3/4, 6/8 (as 6/4 at the
  // tick level), etc. keeps the clock useful outside common-time.

  const meter = module_('Meter');

  const bpb = document.createElement('input');
  bpb.type = 'number';
  bpb.className = 'clock-bpb';
  bpb.min = '1';
  bpb.max = '16';
  bpb.step = '1';
  state.clock.beatsPerBar.subscribe((n) => {
    bpb.value = String(n);
  });
  bpb.addEventListener('change', () => {
    const n = Math.max(1, Math.min(16, parseInt(bpb.value, 10) || 4));
    dispatcher.setClockBeatsPerBar(n);
  });

  meter.body.append(row('Beats/bar', bpb));

  // ---- Metronome module ----------------------------------------------
  //
  // Audible click on each beat edge with an accent on beat 1 of the
  // bar. Fires only while the clock is running. Use it to arm a looper
  // take in time — hit REC on the downbeat click, PLAY on the click
  // that closes out your bar, and Sync keeps the recording boundary
  // on the grid.

  const metro = module_('Metronome');

  const metroOn = document.createElement('input');
  metroOn.type = 'checkbox';
  metroOn.className = 'metro-on';
  state.clock.metroOn.subscribe((on) => { metroOn.checked = on; });
  metroOn.addEventListener('change', () => {
    dispatcher.setMetroOn(metroOn.checked);
  });

  const metroLevel = document.createElement('input');
  metroLevel.type = 'range';
  metroLevel.min = '0';
  metroLevel.max = '1';
  metroLevel.step = '0.01';
  const metroLevelRO = document.createElement('span');
  metroLevelRO.className = 'proc-readout';
  state.clock.metroLevel.subscribe((v) => {
    metroLevel.value = String(v);
    metroLevelRO.textContent = v.toFixed(2);
  });
  metroLevel.addEventListener('input', () => {
    dispatcher.setMetroLevel(parseFloat(metroLevel.value));
  });

  metro.body.append(row('Click', metroOn), row('Level', metroLevel, metroLevelRO));

  // ---- Help text ------------------------------------------------------

  const note = document.createElement('p');
  note.className = 'proc-note';
  note.textContent =
    'One shared clock drives every tempo-aware module: the looper ' +
    'quantize-to-beat arm, and any future LFO-sync / arpeggiator / ' +
    'beat-machine work. In External mode the clock follows the first ' +
    'controller that sends MIDI Timing Clock (0xF8). Switch to ' +
    'Internal and this device is master — the slider BPM is what ' +
    'everything downstream locks to.';

  root.append(status.section, src.section, tempo.section, meter.section, metro.section, note);

  // Seed: on first render, ask the firmware where it is. The snapshot
  // reply at connect time covers the main params, but /clock/running
  // isn't part of the snapshot — this explicit query fills the gap.
  queueMicrotask(() => dispatcher.queryClockRunning());

  return root;
}

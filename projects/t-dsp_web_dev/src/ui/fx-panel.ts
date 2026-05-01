// FX tab — shared send bus with chorus + reverb. Every synth has its
// own per-synth "Send" slider (in its sub-tab) that feeds into this
// bus; the wet return mixes back into the main output.
//
// Signal flow (firmware side):
//   synthA ─┐
//   synthB ─┼─ fxSendBus → chorus → freeverb-stereo → return(L,R) → preMix[3]
//   …      ─┘
//
// Both effects default to OFF at boot. When off, chorus bypasses
// (voices=0) and reverb mutes its return amp — the engine keeps
// running in the background either way so toggling on/off mid-note
// doesn't chop tails.

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

export function fxPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'processing-panel';   // reuse processing-panel styles

  // ---- Chorus module ----
  const chorus = module_('Chorus');
  const choEnable = toggle('Enable chorus');
  const choVoices = slider(2, 8, 1);
  const choReadout = document.createElement('span');
  choReadout.className = 'proc-readout';
  const choNote = document.createElement('p');
  choNote.className = 'proc-note';
  choNote.textContent =
    'Classic DX7-style chorus — thickens any synth feeding the bus. ' +
    'More voices = wider / lusher at the cost of a few extra CPU %.';
  chorus.body.append(
    row('Enable', choEnable),
    row('Voices', choVoices, choReadout),
    choNote,
  );

  state.fx.chorusEnable.subscribe((v) => {
    choEnable.checked = v;
    choVoices.disabled = !v;
  });
  state.fx.chorusVoices.subscribe((v) => {
    choVoices.value = String(v);
    choReadout.textContent = String(v);
  });
  choEnable.addEventListener('change', () => dispatcher.setFxChorusEnable(choEnable.checked));
  choVoices.addEventListener('input', () => dispatcher.setFxChorusVoices(parseInt(choVoices.value, 10)));

  // ---- Reverb module ----
  const reverb = module_('Reverb');
  const revEnable  = toggle('Enable reverb');
  const revSize    = slider(0, 1, 0.01);
  const revDamp    = slider(0, 1, 0.01);
  const revReturn  = slider(0, 1, 0.01);
  const revSizeRO   = document.createElement('span');
  const revDampRO   = document.createElement('span');
  const revReturnRO = document.createElement('span');
  revSizeRO.className   = 'proc-readout';
  revDampRO.className   = 'proc-readout';
  revReturnRO.className = 'proc-readout';
  const revNote = document.createElement('p');
  revNote.className = 'proc-note';
  revNote.textContent =
    'Mono-in / stereo-out Freeverb. Size sets room scale, damping tames ' +
    'bright reflections, return controls wet level into the main mix.';
  reverb.body.append(
    row('Enable', revEnable),
    row('Size', revSize, revSizeRO),
    row('Damping', revDamp, revDampRO),
    row('Return', revReturn, revReturnRO),
    revNote,
  );

  state.fx.reverbEnable.subscribe((v) => {
    revEnable.checked = v;
    revSize.disabled = !v;
    revDamp.disabled = !v;
    revReturn.disabled = !v;
  });
  state.fx.reverbSize.subscribe((v) => {
    revSize.value = String(v);
    revSizeRO.textContent = v.toFixed(2);
  });
  state.fx.reverbDamping.subscribe((v) => {
    revDamp.value = String(v);
    revDampRO.textContent = v.toFixed(2);
  });
  state.fx.reverbReturn.subscribe((v) => {
    revReturn.value = String(v);
    revReturnRO.textContent = v.toFixed(2);
  });
  revEnable.addEventListener('change', () => dispatcher.setFxReverbEnable(revEnable.checked));
  revSize.addEventListener('input', () => dispatcher.setFxReverbSize(parseFloat(revSize.value)));
  revDamp.addEventListener('input', () => dispatcher.setFxReverbDamping(parseFloat(revDamp.value)));
  revReturn.addEventListener('input', () => dispatcher.setFxReverbReturn(parseFloat(revReturn.value)));

  root.append(chorus.section, reverb.section);
  return root;
}

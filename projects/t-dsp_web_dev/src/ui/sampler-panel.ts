// Sampler panel — config UI for slot 1 (multisample SD-streaming).
//
// Layout mirrors dexed-panel for visual consistency: ON/OFF header,
// then a vertical stack of labeled rows for the standard slot controls
// (volume, MIDI channel) and a read-only bank-info readout.
//
// All controls round-trip through the firmware via OSC. Optimistic
// local-signal updates make the UI feel responsive; the echoes that
// land back through dispatcher.handleIncoming() re-apply, idempotent
// when the value matches.

import { Dispatcher } from '../dispatcher';
import { MixerState } from '../state';

export function samplerPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'sampler-panel';

  // ---- ON/OFF header --------------------------------------------------
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';

  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  const updateOnBtn = (on: boolean): void => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute the sampler' : 'Click to un-mute the sampler';
  };
  state.sampler.on.subscribe(updateOnBtn);
  onBtn.addEventListener('click', () => {
    dispatcher.setSamplerOn(!state.sampler.on.get());
  });

  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Sampler';
  onRow.append(onBtn, onLabel);

  // ---- Bank info (read-only) -----------------------------------------
  const bankRow = document.createElement('div');
  bankRow.className = 'sampler-info-row';

  const bankLabel = document.createElement('label');
  bankLabel.textContent = 'Bank';
  const bankValue = document.createElement('span');
  bankValue.className = 'sampler-info-value';
  const refreshBank = (): void => {
    const name = state.sampler.bankName.get();
    bankValue.textContent = name
      ? name.charAt(0).toUpperCase() + name.slice(1)
      : '(no bank loaded)';
  };
  state.sampler.bankName.subscribe(refreshBank);
  bankRow.append(bankLabel, bankValue);

  const samplesRow = document.createElement('div');
  samplesRow.className = 'sampler-info-row';
  const samplesLabel = document.createElement('label');
  samplesLabel.textContent = 'Samples';
  const samplesValue = document.createElement('span');
  samplesValue.className = 'sampler-info-value';
  const refreshSamples = (): void => {
    const n = state.sampler.numSamples.get();
    const r = state.sampler.numReleaseSamples.get();
    samplesValue.textContent = r > 0 ? `${n} + ${r} release` : `${n}`;
  };
  state.sampler.numSamples.subscribe(refreshSamples);
  state.sampler.numReleaseSamples.subscribe(refreshSamples);
  samplesRow.append(samplesLabel, samplesValue);

  // ---- Volume slider --------------------------------------------------
  const volRow = document.createElement('div');
  volRow.className = 'sampler-control-row';

  const volLabel = document.createElement('label');
  volLabel.textContent = 'Volume';

  const volSlider = document.createElement('input');
  volSlider.type = 'range';
  volSlider.className = 'sampler-volume-slider';
  volSlider.min = '0';
  volSlider.max = '1';
  volSlider.step = '0.001';
  state.sampler.volume.subscribe((v) => { volSlider.value = String(v); });
  volSlider.addEventListener('input', () => {
    dispatcher.setSamplerVolume(parseFloat(volSlider.value));
  });

  const volReadout = document.createElement('span');
  volReadout.className = 'sampler-volume-readout';
  state.sampler.volume.subscribe((v) => {
    // X32 fader 0..1 -> approximate dB at unity (0.75) = 0 dB; full = +10 dB; 0 = -inf.
    if (v <= 0) { volReadout.textContent = '-∞ dB'; return; }
    const db = 20 * Math.log10(v / 0.75);  // crude but useful
    const sign = db >= 0 ? '+' : '';
    volReadout.textContent = `${sign}${db.toFixed(1)} dB`;
  });

  volRow.append(volLabel, volSlider, volReadout);

  // ---- MIDI channel selector -----------------------------------------
  const midiRow = document.createElement('div');
  midiRow.className = 'sampler-control-row';

  const midiLabel = document.createElement('label');
  midiLabel.textContent = 'MIDI';

  const midiSelect = document.createElement('select');
  midiSelect.className = 'sampler-midi-channel';
  // 0 = omni; 1..16 = single-channel.
  const omniOpt = document.createElement('option');
  omniOpt.value = '0';
  omniOpt.textContent = 'Omni (all channels)';
  midiSelect.appendChild(omniOpt);
  for (let ch = 1; ch <= 16; ++ch) {
    const opt = document.createElement('option');
    opt.value = String(ch);
    opt.textContent = `Channel ${ch}`;
    midiSelect.appendChild(opt);
  }
  state.sampler.midiChannel.subscribe((ch) => { midiSelect.value = String(ch); });
  midiSelect.addEventListener('change', () => {
    dispatcher.setSamplerMidiChannel(parseInt(midiSelect.value, 10));
  });

  midiRow.append(midiLabel, midiSelect);

  // ---- Sustain note (informational, not a control) -------------------
  const sustainRow = document.createElement('div');
  sustainRow.className = 'sampler-info-row sampler-info-note';
  const sustainLabel = document.createElement('label');
  sustainLabel.textContent = 'Sustain';
  const sustainValue = document.createElement('span');
  sustainValue.className = 'sampler-info-value';
  sustainValue.textContent = 'Hold spacebar (or MIDI CC#64)';
  sustainRow.append(sustainLabel, sustainValue);

  root.append(onRow, bankRow, samplesRow, volRow, midiRow, sustainRow);
  return root;
}

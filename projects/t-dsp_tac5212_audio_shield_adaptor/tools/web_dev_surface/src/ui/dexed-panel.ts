// Dexed sub-tab panel — bank + voice dropdowns, volume slider, MIDI
// channel selector. Mirrors state.dexed via Signal subscriptions; all
// writes go through the Dispatcher which round-trips through firmware.
//
// Bank name list is populated by the firmware on connect
// (/synth/dexed/bank/names). Voice name list is (re-)populated every
// time the bank changes by calling queryDexedVoiceNames() — the
// dropdown shows a generic "Voice N" label while the reply is in
// flight, which is typically 1-2 round-trips of the bridge (~ms).

import { Dispatcher } from '../dispatcher';
import { MixerState } from '../state';

const NUM_VOICES_PER_BANK = 32;
const NUM_BANKS = 10;

export function dexedPanel(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'dexed-panel';

  // ---- On/off header row ----
  const onRow = document.createElement('div');
  onRow.className = 'synth-on-row';
  const onBtn = document.createElement('button');
  onBtn.type = 'button';
  onBtn.className = 'synth-on-btn';
  const updateOnBtn = (on: boolean): void => {
    onBtn.classList.toggle('on', on);
    onBtn.textContent = on ? 'ON' : 'OFF';
    onBtn.title = on ? 'Click to mute Dexed output' : 'Click to un-mute Dexed output';
  };
  state.dexed.on.subscribe(updateOnBtn);
  onBtn.addEventListener('click', () => dispatcher.setDexedOn(!state.dexed.on.get()));
  const onLabel = document.createElement('span');
  onLabel.className = 'synth-on-label';
  onLabel.textContent = 'Dexed FM';
  onRow.append(onBtn, onLabel);

  // ---- Bank row ----
  const bankRow = document.createElement('div');
  bankRow.className = 'dexed-row';
  const bankLabel = document.createElement('label');
  bankLabel.textContent = 'Bank';
  const bankSelect = document.createElement('select');
  bankSelect.className = 'dexed-bank';
  bankRow.append(bankLabel, bankSelect);

  // ---- Voice row ----
  // Layout: [label] [dropdown] [◀ prev] [next ▶]
  // Prev/next step through voices for quick A/B auditioning. Wraps
  // across bank boundaries: voice 31 → next → bank+1 voice 0; bank 0
  // voice 0 → prev → bank 9 voice 31. Gives a single linear walk of
  // all 320 bundled voices without touching the bank dropdown.
  const voiceRow = document.createElement('div');
  voiceRow.className = 'dexed-row dexed-row-voice';
  const voiceLabel = document.createElement('label');
  voiceLabel.textContent = 'Voice';
  const voiceSelect = document.createElement('select');
  voiceSelect.className = 'dexed-voice';
  const prevBtn = document.createElement('button');
  prevBtn.type = 'button';
  prevBtn.className = 'dexed-step';
  prevBtn.textContent = '◀';
  prevBtn.title = 'Previous voice';
  const nextBtn = document.createElement('button');
  nextBtn.type = 'button';
  nextBtn.className = 'dexed-step';
  nextBtn.textContent = '▶';
  nextBtn.title = 'Next voice';
  voiceRow.append(voiceLabel, voiceSelect, prevBtn, nextBtn);

  // Advance by delta (+1 / -1) through the full voice space with
  // bank wrap-around. When the bank changes as a side effect, we
  // also re-query the bank's voice names so the dropdown labels
  // reflect the new bank.
  const stepVoice = (delta: number): void => {
    const bank  = state.dexed.bank.get();
    const voice = state.dexed.voice.get();
    const linear = bank * NUM_VOICES_PER_BANK + voice;
    const total  = NUM_BANKS * NUM_VOICES_PER_BANK;
    const next   = ((linear + delta) % total + total) % total;
    const newBank  = Math.floor(next / NUM_VOICES_PER_BANK);
    const newVoice = next % NUM_VOICES_PER_BANK;
    if (newBank !== bank) {
      dispatcher.queryDexedVoiceNames(newBank);
    }
    dispatcher.setDexedVoice(newBank, newVoice);
  };
  prevBtn.addEventListener('click', () => stepVoice(-1));
  nextBtn.addEventListener('click', () => stepVoice(+1));

  // Global arrow-key shortcuts for fast voice auditioning + volume.
  // Scope:
  //   * only when the Synth tab is visible (panel has a rendered
  //     layout box — offsetParent is null when any ancestor is
  //     display:none);
  //   * only when the focused element isn't a form control, so arrow
  //     keys still behave normally inside the bank/voice dropdowns,
  //     volume slider, raw-OSC text field, and serial console;
  //   * ignored with any modifier held (ctrl/alt/meta/shift), so
  //     OS- and browser-level shortcuts aren't hijacked.
  //
  // Bindings:
  //   ← / →   : previous / next voice (wraps across banks)
  //   ↓ / ↑   : volume -5 / +5 % (clamped to 0..1)
  const isFormControl = (el: Element | null): boolean => {
    if (!el) return false;
    const tag = el.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
    if ((el as HTMLElement).isContentEditable) return true;
    return false;
  };
  const VOL_STEP = 0.05;
  const stepVolume = (delta: number): void => {
    const cur  = state.dexed.volume.get();
    const next = Math.max(0, Math.min(1, cur + delta));
    if (next === cur) return;
    dispatcher.setDexedVolume(next);
  };
  window.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
    if (!root.offsetParent) return;              // Synth tab not visible
    if (isFormControl(document.activeElement)) return;
    switch (e.key) {
      case 'ArrowLeft':  e.preventDefault(); stepVoice(-1);          break;
      case 'ArrowRight': e.preventDefault(); stepVoice(+1);          break;
      case 'ArrowDown':  e.preventDefault(); stepVolume(-VOL_STEP);  break;
      case 'ArrowUp':    e.preventDefault(); stepVolume(+VOL_STEP);  break;
    }
  });

  // ---- Volume row ----
  const volRow = document.createElement('div');
  volRow.className = 'dexed-row';
  const volLabel = document.createElement('label');
  volLabel.textContent = 'Volume';
  const volSlider = document.createElement('input');
  volSlider.type = 'range';
  volSlider.min = '0';
  volSlider.max = '1';
  volSlider.step = '0.01';
  const volReadout = document.createElement('span');
  volReadout.className = 'dexed-readout';
  volRow.append(volLabel, volSlider, volReadout);

  // ---- FX Send row ----
  const sendRow = document.createElement('div');
  sendRow.className = 'dexed-row';
  const sendLabel = document.createElement('label');
  sendLabel.textContent = 'FX Send';
  sendLabel.title = 'Amount of this synth routed into the shared FX bus (chorus / reverb)';
  const sendSlider = document.createElement('input');
  sendSlider.type = 'range';
  sendSlider.min = '0';
  sendSlider.max = '1';
  sendSlider.step = '0.01';
  const sendReadout = document.createElement('span');
  sendReadout.className = 'dexed-readout';
  sendRow.append(sendLabel, sendSlider, sendReadout);

  // ---- MIDI channel row ----
  const chRow = document.createElement('div');
  chRow.className = 'dexed-row';
  const chLabel = document.createElement('label');
  chLabel.textContent = 'MIDI Ch';
  const chSelect = document.createElement('select');
  chSelect.className = 'dexed-channel';
  const omniOpt = document.createElement('option');
  omniOpt.value = '0';
  omniOpt.textContent = 'Omni';
  chSelect.appendChild(omniOpt);
  for (let i = 1; i <= 16; i++) {
    const o = document.createElement('option');
    o.value = String(i);
    o.textContent = String(i);
    chSelect.appendChild(o);
  }
  chRow.append(chLabel, chSelect);

  root.append(onRow, bankRow, voiceRow, volRow, sendRow, chRow);

  // ---- Populate dropdowns from state signals ----

  // Bank names: set once, typically during snapshot. While the list is
  // empty we show numeric fallbacks so the dropdown isn't blank if the
  // firmware is slow to reply.
  const populateBankOptions = (names: string[]): void => {
    const selected = bankSelect.value;
    bankSelect.innerHTML = '';
    const count = names.length > 0 ? names.length : 10;
    for (let i = 0; i < count; i++) {
      const o = document.createElement('option');
      o.value = String(i);
      o.textContent = names[i] ?? `Bank ${i}`;
      bankSelect.appendChild(o);
    }
    if (selected !== '') bankSelect.value = selected;
  };
  populateBankOptions([]);
  state.dexed.bankNames.subscribe((n) => populateBankOptions(n));

  const populateVoiceOptions = (names: string[]): void => {
    const selected = voiceSelect.value;
    voiceSelect.innerHTML = '';
    for (let i = 0; i < NUM_VOICES_PER_BANK; i++) {
      const o = document.createElement('option');
      o.value = String(i);
      const name = names[i] ?? '';
      o.textContent = name ? `${i + 1}. ${name}` : `Voice ${i + 1}`;
      voiceSelect.appendChild(o);
    }
    if (selected !== '') voiceSelect.value = selected;
  };
  populateVoiceOptions([]);
  state.dexed.voiceNames.subscribe((n) => populateVoiceOptions(n));

  // ---- Signal -> UI ----

  state.dexed.bank.subscribe((b) => {
    bankSelect.value = String(b);
  });
  state.dexed.voice.subscribe((v) => {
    voiceSelect.value = String(v);
  });
  state.dexed.volume.subscribe((v) => {
    volSlider.value = String(v);
    volReadout.textContent = v.toFixed(2);
  });
  state.dexed.midiChannel.subscribe((c) => {
    chSelect.value = String(c);
  });
  state.dexed.fxSend.subscribe((v) => {
    sendSlider.value = String(v);
    sendReadout.textContent = v.toFixed(2);
  });

  // ---- UI -> Dispatcher ----

  bankSelect.addEventListener('change', () => {
    const bank = parseInt(bankSelect.value, 10);
    // Fire the voice-names query first so the dropdown repopulates as
    // the reply arrives; the voice-select command follows with (bank, 0)
    // — switching banks resets to the first voice, which matches the
    // behavior of most hardware synths.
    dispatcher.queryDexedVoiceNames(bank);
    dispatcher.setDexedVoice(bank, 0);
  });
  voiceSelect.addEventListener('change', () => {
    dispatcher.setDexedVoice(state.dexed.bank.get(), parseInt(voiceSelect.value, 10));
  });
  volSlider.addEventListener('input', () => {
    dispatcher.setDexedVolume(parseFloat(volSlider.value));
  });
  chSelect.addEventListener('change', () => {
    dispatcher.setDexedMidiChannel(parseInt(chSelect.value, 10));
  });
  sendSlider.addEventListener('input', () => {
    dispatcher.setDexedFxSend(parseFloat(sendSlider.value));
  });

  return root;
}

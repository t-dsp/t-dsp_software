// Synth slot picker — horizontal row of 4 buttons, one per slot.
//
// Phase 0/2 introduced a slot-based synth architecture: the firmware
// runs N synth engines (currently Dexed at slot 0, multisample sampler
// at slot 1) but only one is audible at a time. Switching panics held
// notes on the outgoing slot and ramps the incoming slot's gain up
// to its stored fader. This widget surfaces the picker.
//
// The button labels come from the firmware via /synth/slots (each
// "id|displayName" string parsed by the dispatcher into state.synthSlot
// .slots). Empty slots ("silent" id) are rendered disabled.

import { Dispatcher } from '../dispatcher';
import { MixerState } from '../state';

export function synthSlotPicker(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'synth-slot-picker';

  const SLOT_COUNT = 8;
  const buttons: HTMLButtonElement[] = [];
  for (let i = 0; i < SLOT_COUNT; i++) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'synth-slot-btn';
    btn.dataset.slot = String(i);
    btn.textContent = `Slot ${i}`;  // overwritten by /synth/slots reply
    btn.addEventListener('click', () => {
      // Optimistic + send. The firmware echoes /synth/active back, which
      // re-applies the active state through state.synthSlot.active —
      // idempotent if the click hit the right button.
      dispatcher.setSynthActive(i);
    });
    buttons.push(btn);
    root.append(btn);
  }

  // Update labels + disabled state from /synth/slots metadata.
  state.synthSlot.slots.subscribe((slots) => {
    for (let i = 0; i < SLOT_COUNT; i++) {
      const meta = slots[i];
      if (!meta) continue;
      const empty = meta.id === 'silent' || meta.id === 'unknown';
      buttons[i].textContent = meta.displayName || `Slot ${i}`;
      buttons[i].disabled = empty;
      buttons[i].classList.toggle('empty', empty);
    }
  });

  // Highlight the active slot.
  state.synthSlot.active.subscribe((active) => {
    for (let i = 0; i < SLOT_COUNT; i++) {
      buttons[i].classList.toggle('active', i === active);
    }
  });

  return root;
}

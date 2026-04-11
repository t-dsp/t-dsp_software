// HOST strip: visualizes the Windows volume slider and exposes its
// bypass toggle. The fader is read-only (always `disabled`) because the
// value is owned by usbIn.volume() on the firmware side — changing it
// from here wouldn't push back to Windows. The ENABLE button is the
// only control: when off, the hostvol stage bypasses (gain=1.0) so the
// main fader is the only attenuator. No meter — this strip is strictly
// a gain-stage monitor, and the "real" level is on the MAIN strip's
// pre-hostvol meters.

import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import { formatFaderDb } from './util';

export function hostStrip(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip host-strip';

  const name = document.createElement('div');
  name.className = 'strip-name';
  name.textContent = 'HOST VOL';

  // Spacer that reserves the same vertical room as a meter-wrap so the
  // slider thumb lines up with the other strips' sliders.
  const meterSpacer = document.createElement('div');
  meterSpacer.className = 'meter-wrap meter-spacer';

  const fader = document.createElement('input');
  fader.type = 'range';
  fader.className = 'fader';
  fader.min = '0';
  fader.max = '1';
  fader.step = '0.001';
  fader.disabled = true;  // read-only reflection of usbIn.volume()
  bus.hostvolValue.subscribe((v) => (fader.value = String(v)));

  const fv = document.createElement('div');
  fv.className = 'fader-value';
  bus.hostvolValue.subscribe((v) => (fv.textContent = formatFaderDb(v)));

  // The fader visually dims when bypassed to emphasize that the value
  // is ignored by the audio path.
  bus.hostvolEnable.subscribe((on) => {
    root.classList.toggle('bypassed', !on);
  });

  const enBtn = document.createElement('button');
  enBtn.className = 'enable-btn';
  bus.hostvolEnable.subscribe((on) => {
    enBtn.classList.toggle('active', on);
    enBtn.textContent = on ? 'ON' : 'BYP';
    enBtn.title = on
      ? 'Windows volume attenuates main output — click to bypass'
      : 'Windows volume bypassed — click to enable';
  });
  enBtn.addEventListener('click', () =>
    dispatcher.setMainHostvolEnable(!bus.hostvolEnable.get()),
  );

  root.append(name, meterSpacer, fader, fv, enBtn);
  return root;
}

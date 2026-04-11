import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import { formatFaderDb } from './util';

export function mainBus(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip main-strip';

  const name = document.createElement('div');
  name.className = 'strip-name';
  name.textContent = 'MAIN';

  const fader = document.createElement('input');
  fader.type = 'range';
  fader.className = 'fader';
  fader.min = '0';
  fader.max = '1';
  fader.step = '0.001';
  bus.fader.subscribe((v) => (fader.value = String(v)));
  fader.addEventListener('input', () => dispatcher.setMainFader(parseFloat(fader.value)));

  const fv = document.createElement('div');
  fv.className = 'fader-value';
  bus.fader.subscribe((v) => (fv.textContent = formatFaderDb(v)));

  const mute = document.createElement('button');
  mute.className = 'mute';
  mute.textContent = 'M';
  bus.on.subscribe((on) => mute.classList.toggle('active', !on));
  mute.addEventListener('click', () => dispatcher.setMainOn(!bus.on.get()));

  root.append(name, fader, fv, mute);
  return root;
}

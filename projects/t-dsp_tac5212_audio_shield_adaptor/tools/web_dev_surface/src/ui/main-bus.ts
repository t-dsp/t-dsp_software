import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import { formatFaderDb } from './util';

export function mainBus(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip main-strip';

  const name = document.createElement('div');
  name.className = 'strip-name';
  name.textContent = 'MAIN';

  // Stereo meter: two vertical bars side-by-side, driven by
  // /meters/output (main.peakL/rmsL, main.peakR/rmsR).
  const meterPair = document.createElement('div');
  meterPair.className = 'meter-pair';
  const makeMeter = (
    peakSig: typeof bus.peakL,
    rmsSig: typeof bus.rmsL,
  ): HTMLElement => {
    const wrap = document.createElement('div');
    wrap.className = 'meter-wrap';
    const peakFill = document.createElement('div');
    peakFill.className = 'meter-fill peak';
    const rmsFill = document.createElement('div');
    rmsFill.className = 'meter-fill rms';
    wrap.append(rmsFill, peakFill);
    peakSig.subscribe((p) => {
      peakFill.style.height = `${Math.min(100, Math.max(0, p * 100))}%`;
    });
    rmsSig.subscribe((r) => {
      rmsFill.style.height = `${Math.min(100, Math.max(0, r * 100))}%`;
    });
    return wrap;
  };
  meterPair.append(makeMeter(bus.peakL, bus.rmsL), makeMeter(bus.peakR, bus.rmsR));

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

  root.append(name, meterPair, fader, fv, mute);
  return root;
}

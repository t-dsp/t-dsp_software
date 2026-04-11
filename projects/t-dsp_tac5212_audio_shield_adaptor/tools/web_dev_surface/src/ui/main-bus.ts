// Stereo main bus strip: two faders (L, R) with per-side meters,
// stereo link toggle, and shared mute. When link=true the R slider is
// visually disabled and follows L (the firmware propagates writes).
//
// The meter taps are post-fader / pre-hostvol (see main.cpp), so these
// bars track the fader moves but are unaffected by the Windows volume
// slider. Windows volume lives in the separate HOST strip.

import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import { Signal } from '../state';
import { formatFaderDb } from './util';

export function mainBus(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip main-strip';

  const name = document.createElement('div');
  name.className = 'strip-name';
  name.textContent = 'MAIN';

  // Build one side (L or R) as a meter + fader + dB label column.
  // `isMaster` means this side always tracks its own signal; the slave
  // side gets visually disabled when `bus.link` is true.
  const buildSide = (
    label: string,
    peakSig: Signal<number>,
    rmsSig: Signal<number>,
    faderSig: Signal<number>,
    onInput: (v: number) => void,
    isMaster: boolean,
  ): HTMLElement => {
    const col = document.createElement('div');
    col.className = 'main-side';

    const sideLabel = document.createElement('div');
    sideLabel.className = 'side-label';
    sideLabel.textContent = label;

    const meterWrap = document.createElement('div');
    meterWrap.className = 'meter-wrap';
    const peakFill = document.createElement('div');
    peakFill.className = 'meter-fill peak';
    const rmsFill = document.createElement('div');
    rmsFill.className = 'meter-fill rms';
    meterWrap.append(rmsFill, peakFill);
    peakSig.subscribe((p) => {
      peakFill.style.height = `${Math.min(100, Math.max(0, p * 100))}%`;
    });
    rmsSig.subscribe((r) => {
      rmsFill.style.height = `${Math.min(100, Math.max(0, r * 100))}%`;
    });

    const fader = document.createElement('input');
    fader.type = 'range';
    fader.className = 'fader';
    fader.min = '0';
    fader.max = '1';
    fader.step = '0.001';
    faderSig.subscribe((v) => (fader.value = String(v)));
    fader.addEventListener('input', () => onInput(parseFloat(fader.value)));

    const fv = document.createElement('div');
    fv.className = 'fader-value';
    faderSig.subscribe((v) => (fv.textContent = formatFaderDb(v)));

    // Slave-disable: when link is on, the R side goes disabled/greyed.
    // The L side is always interactive.
    if (!isMaster) {
      bus.link.subscribe((linked) => {
        fader.disabled = linked;
        col.classList.toggle('linked-slave', linked);
      });
    }

    col.append(sideLabel, meterWrap, fader, fv);
    return col;
  };

  const sides = document.createElement('div');
  sides.className = 'main-sides';
  sides.append(
    buildSide(
      'L',
      bus.peakL,
      bus.rmsL,
      bus.faderL,
      (v) => dispatcher.setMainFaderL(v),
      true,
    ),
    buildSide(
      'R',
      bus.peakR,
      bus.rmsR,
      bus.faderR,
      (v) => dispatcher.setMainFaderR(v),
      false,
    ),
  );

  const linkBtn = document.createElement('button');
  linkBtn.className = 'link-btn';
  linkBtn.textContent = 'LINK';
  linkBtn.title = 'Stereo link L/R (X32: writes propagate to both sides)';
  bus.link.subscribe((linked) => linkBtn.classList.toggle('active', linked));
  linkBtn.addEventListener('click', () => dispatcher.setMainLink(!bus.link.get()));

  const mute = document.createElement('button');
  mute.className = 'mute';
  mute.textContent = 'M';
  mute.title = 'Main mute (shared L/R)';
  bus.on.subscribe((on) => mute.classList.toggle('active', !on));
  mute.addEventListener('click', () => dispatcher.setMainOn(!bus.on.get()));

  root.append(name, sides, linkBtn, mute);
  return root;
}

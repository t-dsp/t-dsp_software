import { ChannelState } from '../state';
import { Dispatcher } from '../dispatcher';
import { formatFaderDb } from './util';

export function channelStrip(
  idx: number,
  ch: ChannelState,
  dispatcher: Dispatcher,
): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip';

  const name = document.createElement('div');
  name.className = 'strip-name';
  ch.name.subscribe((n) => (name.textContent = n));

  const meterWrap = document.createElement('div');
  meterWrap.className = 'meter-wrap';
  const peakFill = document.createElement('div');
  peakFill.className = 'meter-fill peak';
  const rmsFill = document.createElement('div');
  rmsFill.className = 'meter-fill rms';
  meterWrap.append(rmsFill, peakFill);
  ch.peak.subscribe((p) => {
    peakFill.style.height = `${Math.min(100, Math.max(0, p * 100))}%`;
  });
  ch.rms.subscribe((r) => {
    rmsFill.style.height = `${Math.min(100, Math.max(0, r * 100))}%`;
  });

  const fader = document.createElement('input');
  fader.type = 'range';
  fader.className = 'fader';
  fader.min = '0';
  fader.max = '1';
  fader.step = '0.001';
  ch.fader.subscribe((v) => (fader.value = String(v)));
  fader.addEventListener('input', () => {
    dispatcher.setChannelFader(idx, parseFloat(fader.value));
  });

  const fv = document.createElement('div');
  fv.className = 'fader-value';
  ch.fader.subscribe((v) => (fv.textContent = formatFaderDb(v)));

  const mute = document.createElement('button');
  mute.className = 'mute';
  mute.textContent = 'M';
  mute.title = 'Mute (mix/on inverted — X32 idiom)';
  ch.on.subscribe((on) => mute.classList.toggle('active', !on));
  mute.addEventListener('click', () => dispatcher.setChannelOn(idx, !ch.on.get()));

  const solo = document.createElement('button');
  solo.className = 'solo';
  solo.textContent = 'S';
  solo.title = 'Solo (SIP)';
  ch.solo.subscribe((s) => solo.classList.toggle('active', s));
  solo.addEventListener('click', () => dispatcher.setChannelSolo(idx, !ch.solo.get()));

  root.append(name, meterWrap, fader, fv, mute, solo);
  return root;
}

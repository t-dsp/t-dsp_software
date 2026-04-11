import { ChannelState, Signal } from '../state';
import { Dispatcher } from '../dispatcher';
import { formatFaderDb } from './util';

// Render one channel strip. `idx` is the 0-based channel index.
//
// Stereo linking:
//   - Odd channels (idx 0, 2, 4 == ch 1, 3, 5) carry the link flag
//     and show a LINK toggle button.
//   - Even channels (idx 1, 3, 5 == ch 2, 4, 6) become visually
//     "slaves" when their odd neighbor has link=true: fader/mute/solo
//     get disabled and the whole strip dims. The firmware propagates
//     writes so the slave's values still update via echo.
//
// `partnerLink` is the link signal of the odd neighbor (or undefined
// if this IS the odd channel). It's used only by even channels.
export function channelStrip(
  idx: number,
  ch: ChannelState,
  dispatcher: Dispatcher,
  partnerLink: Signal<boolean> | undefined,
): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip';

  const isOdd = (idx & 1) === 0;  // idx 0 == ch 1 == odd in 1-based

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

  // Link button: only on odd channels. Toggling updates the model;
  // the even partner's strip subscribes to the same link signal and
  // re-renders as disabled/slave.
  let linkBtn: HTMLElement | undefined;
  if (isOdd && idx + 1 < 6) {
    const btn = document.createElement('button');
    btn.className = 'link-btn';
    btn.textContent = 'L';
    btn.title = 'Stereo link with next channel';
    ch.link.subscribe((linked) => btn.classList.toggle('active', linked));
    btn.addEventListener('click', () => dispatcher.setChannelLink(idx, !ch.link.get()));
    linkBtn = btn;
  }

  // Slave-disable: when this is an even channel AND the odd partner's
  // link flag is true, dim everything and disable the controls.
  if (!isOdd && partnerLink) {
    partnerLink.subscribe((linked) => {
      fader.disabled = linked;
      mute.disabled = linked;
      solo.disabled = linked;
      root.classList.toggle('linked-slave', linked);
    });
  }

  const children: HTMLElement[] = [name, meterWrap, fader, fv, mute, solo];
  if (linkBtn) children.push(linkBtn);
  root.append(...children);
  return root;
}

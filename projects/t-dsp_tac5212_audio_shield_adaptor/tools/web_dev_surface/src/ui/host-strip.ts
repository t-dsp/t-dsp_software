// HOST strip: Windows volume monitor. Built with the same 7-row
// structure as channel pairs and main, so every row (name/meter/fader/
// fader-value/mute/solo/link) lines up horizontally.
//
//   row 1: HOST VOL label
//   row 2: meter spacer (no meter — this is a gain-stage monitor)
//   row 3: fader (disabled; reflects usbIn.volume() echoes)
//   row 4: fader value (dB)
//   row 5: mute spacer
//   row 6: solo spacer
//   row 7: ENABLE button (bypass toggle) — sits where pair LINK lives
//
// The enable button in the link-row slot is the only control on this
// strip: toggling it off bypasses the hostvol stage so the main fader
// is the sole attenuator.

import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import {
  makeRow,
  makeStaticName,
  makeMeter,
  makeDisabledFader,
  makeFaderValue,
  makeCellSpacer,
} from './cells';

export function hostStrip(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper host-wrapper';

  // Row 1: name
  const rowName = makeRow('row-name');
  rowName.append(makeStaticName('HOST VOL'));

  // Row 2: stereo meter — post-hostvol (what the DAC actually sees).
  // Driven by /meters/host from the firmware.
  const rowMeter = makeRow('row-meter');
  rowMeter.append(
    makeMeter(bus.hostPeakL, bus.hostRmsL),
    makeMeter(bus.hostPeakR, bus.hostRmsR),
  );

  // Row 3: fader (disabled, read-only reflection of usbIn.volume())
  const rowFader = makeRow('row-fader');
  const fader = makeDisabledFader(bus.hostvolValue);
  rowFader.append(fader);

  // Row 4: fader value
  const rowFv = makeRow('row-fv');
  rowFv.append(makeFaderValue(bus.hostvolValue));

  // Row 5: mute spacer
  const rowMute = makeRow('row-mute');
  rowMute.append(makeCellSpacer('mute'));

  // Row 6: solo spacer
  const rowSolo = makeRow('row-solo');
  rowSolo.append(makeCellSpacer('solo'));

  // Row 7: ENABLE button (sits in the link row slot for visual alignment)
  const rowLink = makeRow('row-link');
  const enBtn = document.createElement('button');
  enBtn.className = 'enable-btn cell';
  bus.hostvolEnable.subscribe((on) => {
    enBtn.classList.toggle('active', on);
    enBtn.textContent = on ? 'ON' : 'BYP';
    enBtn.title = on
      ? 'Windows volume attenuates main output — click to bypass'
      : 'Windows volume bypassed — click to enable';
    root.classList.toggle('bypassed', !on);
  });
  enBtn.addEventListener('click', () =>
    dispatcher.setMainHostvolEnable(!bus.hostvolEnable.get()),
  );
  rowLink.append(enBtn);

  // Row 8: rec spacer — this strip has no capture control of its own.
  // The spacer preserves vertical alignment with the channel pairs and
  // main strip's REC/LOOP rows.
  const rowRec = makeRow('row-rec');
  rowRec.append(makeCellSpacer('rec'));

  root.append(rowName, rowMeter, rowFader, rowFv, rowMute, rowSolo, rowLink, rowRec);
  return root;
}

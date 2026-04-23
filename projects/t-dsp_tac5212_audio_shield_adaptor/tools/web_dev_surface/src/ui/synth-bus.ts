// Synth bus: group fader + mute for all synths (Dexed + MPE + shared
// FX wet return), shown as a compact strip in the mixer row between
// the input channel pairs and the output dock.
//
// One fader, one mute — synthAmpL and synthAmpR share the same gain
// on the firmware side (L+R trim together), so a single control is
// both accurate and keeps the strip narrow. Uses the shared 7-row
// layout (via `strip-row` classes + cell spacers) so its rows align
// horizontally with the channel pairs / main bus.
//
// The bus also feeds the looper source mux — when "Synth" is picked
// as the looper source, whatever this fader lets through is what the
// looper records.

import { SynthBusState } from '../state';
import { Dispatcher } from '../dispatcher';
import {
  makeRow,
  makeStaticName,
  makeMeterSpacer,
  makeFaderValue,
  makeCellSpacer,
} from './cells';

export function synthBusStrip(bus: SynthBusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper synth-bus-wrapper';

  // Row 1: name — "SYNTH" centered in a wide (span-2) cell.
  const rowName = makeRow('row-name');
  const nameCell = makeStaticName('SYNTH');
  nameCell.classList.add('span-2');
  rowName.append(nameCell);

  // Row 2: no meter yet — two spacer cells to hold the row at the
  // same height as channel pairs' / main-bus' meter row.
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeterSpacer(), makeMeterSpacer());

  // Row 3: single fader, visually spanning both slots. The strip-row
  // rule already centers its children, so one wide fader sits in the
  // middle of the wrapper.
  const rowFader = makeRow('row-fader');
  const fader = document.createElement('input');
  fader.type = 'range';
  fader.className = 'fader wide cell';
  fader.min = '0';
  fader.max = '1';
  fader.step = '0.001';
  bus.volume.subscribe((v) => { fader.value = String(v); });
  fader.addEventListener('input', () =>
    dispatcher.setSynthBusVolume(parseFloat(fader.value))
  );
  rowFader.append(fader);

  // Row 4: fader value readout (dB-formatted via makeFaderValue).
  const rowFv = makeRow('row-fv');
  const fv = makeFaderValue(bus.volume);
  fv.classList.add('span-2');
  rowFv.append(fv);

  // Row 5: shared mute (wide) — toggles bus.on. When off, synthAmpL/R
  // go to 0 without touching the stored volume.
  const rowMute = makeRow('row-mute');
  const muteBtn = document.createElement('button');
  muteBtn.className = 'mute wide cell';
  muteBtn.textContent = 'M';
  muteBtn.title = 'Synth bus mute (all synths)';
  bus.on.subscribe((on) => muteBtn.classList.toggle('active', !on));
  muteBtn.addEventListener('click', () => dispatcher.setSynthBusOn(!bus.on.get()));
  rowMute.append(muteBtn);

  // Rows 6..8: spacers so the strip is exactly the same height as a
  // channel pair / main-bus. Solo / link / rec don't apply here.
  const rowSolo = makeRow('row-solo');
  const soloSp = makeCellSpacer('solo');
  soloSp.classList.add('span-2');
  rowSolo.append(soloSp);

  const rowLink = makeRow('row-link');
  const linkSp = makeCellSpacer('link');
  linkSp.classList.add('span-2');
  rowLink.append(linkSp);

  const rowRec = makeRow('row-rec');
  const recSp = makeCellSpacer('rec');
  recSp.classList.add('span-2');
  rowRec.append(recSp);

  root.append(rowName, rowMeter, rowFader, rowFv, rowMute, rowSolo, rowLink, rowRec);
  return root;
}

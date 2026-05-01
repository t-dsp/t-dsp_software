// Main bus: stereo L/R laid out as the same 7-row structure as a
// channel pair so the rows line up horizontally across the mixer.
//
//   row 1: MAIN label (spans both sides via a single name cell)
//   row 2: two meters (post-fader / pre-hostvol)
//   row 3: two faders
//   row 4: two fader values
//   row 5: one shared mute button (main has a single mute, not per-side)
//   row 6: solo spacer (main has no solo — keeps row alignment)
//   row 7: LINK button (stereo link toggle)
//
// When linked, the R side dims and its fader is disabled; the firmware
// propagates writes so dragging L moves R via echo.

import { BusState } from '../state';
import { Dispatcher } from '../dispatcher';
import {
  makeRow,
  makeStaticName,
  makeMeter,
  makeFader,
  makeFaderValue,
  makeCellSpacer,
} from './cells';

export function mainBus(bus: BusState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper main-wrapper';

  // Row 1: name — "MAIN" centered, spans both sides (single wide cell).
  const rowName = makeRow('row-name');
  const nameCell = makeStaticName('MAIN');
  nameCell.classList.add('span-2');
  rowName.append(nameCell);

  // Row 2: L + R meters
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeter(bus.peakL, bus.rmsL), makeMeter(bus.peakR, bus.rmsR));

  // Row 2.5: gain spacers — main bus has no analog input gain stage,
  // but the row exists so XLR-strip knobs line up with the rest of the
  // mixer's fader row.
  const rowGain = makeRow('row-gain');
  rowGain.append(makeCellSpacer('gain'), makeCellSpacer('gain'));

  // Row 3: L + R faders
  const rowFader = makeRow('row-fader');
  const faderL = makeFader(bus.faderL, (v) => dispatcher.setMainFaderL(v));
  const faderR = makeFader(bus.faderR, (v) => dispatcher.setMainFaderR(v));
  rowFader.append(faderL, faderR);

  // Row 4: L + R fader values
  const rowFv = makeRow('row-fv');
  rowFv.append(makeFaderValue(bus.faderL), makeFaderValue(bus.faderR));

  // Row 5: shared mute (single button, spans both sides)
  const rowMute = makeRow('row-mute');
  const muteBtn = document.createElement('button');
  muteBtn.className = 'mute wide cell';
  muteBtn.textContent = 'M';
  muteBtn.title = 'Main mute (shared L/R)';
  bus.on.subscribe((on) => muteBtn.classList.toggle('active', !on));
  muteBtn.addEventListener('click', () => dispatcher.setMainOn(!bus.on.get()));
  rowMute.append(muteBtn);

  // Row 6: solo spacer — main has no solo, but the row still exists so
  // the link row below it lines up with channel pairs' link rows.
  const rowSolo = makeRow('row-solo');
  rowSolo.append(makeCellSpacer('solo'));

  // Row 7: LINK (stereo link). Both L and R faders remain draggable
  // when linked — the dispatcher propagates writes to the partner
  // optimistically and the firmware echoes both sides for convergence.
  const rowLink = makeRow('row-link');
  const linkBtn = document.createElement('button');
  linkBtn.className = 'link-btn wide cell';
  linkBtn.textContent = 'LINK';
  linkBtn.title = 'Stereo link L/R — writes to either side move both';
  bus.link.subscribe((linked) => linkBtn.classList.toggle('active', linked));
  linkBtn.addEventListener('click', () => dispatcher.setMainLink(!bus.link.get()));
  rowLink.append(linkBtn);
  void faderR;  // intentionally live in both unlinked and linked modes

  // Row 8: LOOP — tap the post-fader main mix into USB capture so the
  // host can record what's in the headphones. While armed, the per-
  // channel REC buttons grey out (firmware forces their sends to 0 so
  // sources already in the main mix don't double-count).
  const rowRec = makeRow('row-rec');
  const loopBtn = document.createElement('button');
  loopBtn.className = 'loop-btn wide cell';
  loopBtn.textContent = 'LOOP';
  loopBtn.title =
    'Loopback: record the main mix over USB (post-fader, pre-Windows-volume).';
  bus.loopEnable.subscribe((on) => loopBtn.classList.toggle('active', on));
  loopBtn.addEventListener('click', () => dispatcher.setMainLoop(!bus.loopEnable.get()));
  rowRec.append(loopBtn);

  root.append(rowName, rowMeter, rowGain, rowFader, rowFv, rowMute, rowSolo, rowLink, rowRec);
  return root;
}

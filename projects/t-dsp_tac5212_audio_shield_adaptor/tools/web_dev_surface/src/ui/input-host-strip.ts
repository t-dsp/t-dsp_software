// CAP HOST strip: read-only mirror of Windows' recording-device volume
// slider, driven by the USB Audio Class Feature Unit 0x30 we added to
// the teensy4 core. Sister of host-strip.ts, which mirrors the playback
// (Speakers) slider via FU 0x31. Both faders are disabled — Windows owns
// these values, the firmware just reports what it received via SET_CUR.
//
// Same 7-row layout as the rest of the mixer so rows align horizontally:
//
//   row 1: CAP HOST label  (spans both sides)
//   row 2: meter spacers   (no capture-side host meter exists yet)
//   row 3: stereo disabled faders (both bound to captureHostvolValue —
//                                  the firmware tracks one mono value
//                                  because the SET_CUR dispatch only
//                                  parses CS=Volume L; the strip shows
//                                  it twice for visual stereo parity)
//   row 4: stereo fader-value labels (same value, both columns)
//   row 5: MUTE indicator (read-only, lights when captureHostvolMute is on)
//   row 6: solo spacer
//   row 7: link spacer (no enable button — there's nothing to bypass:
//                       this is a pure monitor of Windows' state)

import { BusState } from '../state';
import {
  makeRow,
  makeStaticName,
  makeMeterSpacer,
  makeDisabledFader,
  makeFaderValue,
  makeCellSpacer,
} from './cells';

export function inputHostStrip(bus: BusState): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper host-wrapper input-host-wrapper';

  // Row 1: name — "CAP HOST" centered, spans both sides.
  const rowName = makeRow('row-name');
  const nameCell = makeStaticName('CAP HOST');
  nameCell.classList.add('span-2');
  rowName.append(nameCell);

  // Row 2: meter spacers (no /meters/cap stream exists yet — could add
  // one downstream of the capture mixer if we ever want pre-host metering
  // on the input path).
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeterSpacer(), makeMeterSpacer());

  // Row 3: two disabled faders, both bound to the same value signal.
  // The firmware only tracks one value because the existing SET_CUR
  // dispatch in usb.c at the word1 == 0x02010121 site only matches
  // Volume Left (channel 1); Volume Right is dropped on the floor by
  // the same pre-existing limitation that affects the playback FU.
  // So this is "stereo display, mono data" — honest about the source.
  const rowFader = makeRow('row-fader');
  const faderL = makeDisabledFader(bus.captureHostvolValue);
  const faderR = makeDisabledFader(bus.captureHostvolValue);
  rowFader.append(faderL, faderR);

  // Row 4: stereo fader values (also both bound to the same signal).
  const rowFv = makeRow('row-fv');
  rowFv.append(
    makeFaderValue(bus.captureHostvolValue),
    makeFaderValue(bus.captureHostvolValue),
  );

  // Row 5: MUTE indicator. Read-only — uses the .mute button class so
  // it picks up the same active styling as a regular mute, but has no
  // click handler. It just lights up red when Windows sends mute=1 on
  // the FU 0x30 control.
  const rowMute = makeRow('row-mute');
  const muteIndicator = document.createElement('div');
  muteIndicator.className = 'mute wide cell readonly';
  muteIndicator.textContent = 'M';
  muteIndicator.title = 'Mute state from Windows recording slider (read-only)';
  bus.captureHostvolMute.subscribe((muted) => {
    muteIndicator.classList.toggle('active', muted);
  });
  rowMute.append(muteIndicator);

  // Row 6: solo spacer
  const rowSolo = makeRow('row-solo');
  rowSolo.append(makeCellSpacer('solo'));

  // Row 7: link spacer — no ENABLE button. There's nothing to bypass
  // because we don't (yet) apply this gain to any audio stage; it's
  // purely a monitor of what Windows reports. If/when we wire it into
  // the listenback mixer, an ENABLE button can go here parallel to
  // the playback HOST strip's bypass.
  const rowLink = makeRow('row-link');
  rowLink.append(makeCellSpacer('link'));

  root.append(rowName, rowMeter, rowFader, rowFv, rowMute, rowSolo, rowLink);
  return root;
}

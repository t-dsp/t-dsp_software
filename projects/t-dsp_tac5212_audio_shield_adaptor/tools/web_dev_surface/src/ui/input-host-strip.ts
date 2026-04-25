// CAP HOST strip: read-only mirror of Windows' recording-device volume
// slider, driven by the USB Audio Class Feature Unit 0x30 we added to
// the teensy4 core. Sister of host-strip.ts, which mirrors the playback
// (Speakers) slider via FU 0x31. Single mono fader — the firmware only
// tracks one value because the existing SET_CUR dispatch in usb.c at
// the word1 == 0x02010121 site only matches Volume Left (channel 1);
// Volume Right SET_CURs are dropped by the same pre-existing limitation
// that affects the playback FU. Mono fader is honest about the source.
//
// Same 7-row layout as the rest of the mixer so rows align horizontally:
//
//   row 1: CAP HOST label
//   row 2: meter spacer (no capture-side host meter exists yet)
//   row 3: single disabled fader bound to captureHostvolValue
//   row 4: fader-value label (dB)
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

  // Row 1: name
  const rowName = makeRow('row-name');
  rowName.append(makeStaticName('CAP HOST'));

  // Row 2: meter spacer (no /meters/cap stream exists yet — could add
  // one downstream of the capture mixer if we ever want pre-host metering
  // on the input path).
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeterSpacer());

  // Row 2.5: gain spacer — keeps XLR analog-gain knobs aligned with the
  // fader row across the mixer.
  const rowGain = makeRow('row-gain');
  rowGain.append(makeCellSpacer('gain'));

  // Row 3: single disabled fader.
  const rowFader = makeRow('row-fader');
  rowFader.append(makeDisabledFader(bus.captureHostvolValue));

  // Row 4: fader value (dB)
  const rowFv = makeRow('row-fv');
  rowFv.append(makeFaderValue(bus.captureHostvolValue));

  // Row 5: MUTE indicator. Read-only — uses the .mute button class so
  // it picks up the same active styling as a regular mute, but has no
  // click handler. It just lights up red when Windows sends mute=1 on
  // the FU 0x30 control.
  const rowMute = makeRow('row-mute');
  const muteIndicator = document.createElement('div');
  muteIndicator.className = 'mute cell readonly';
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

  // Row 8: rec spacer — preserves alignment with channel-pair REC and
  // main-bus LOOP rows. No capture-side control of its own.
  const rowRec = makeRow('row-rec');
  rowRec.append(makeCellSpacer('rec'));

  root.append(rowName, rowMeter, rowGain, rowFader, rowFv, rowMute, rowSolo, rowLink, rowRec);
  return root;
}

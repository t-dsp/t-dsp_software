// Channel pair: two channel strips laid out as a 7-row grid with a
// shared LINK button in the last row spanning both columns.
//
// Linking model: only the ODD channel of a pair carries the link flag
// on the firmware side. The pair wrapper subscribes to it and:
//   - flips the LINK button's active state
//   - disables the even (R) column's fader/mute/solo controls
//   - dims the R column visually
//
// When unlinked, both channels behave independently. When linked, the
// firmware model propagates writes to the partner automatically, so
// moving the L fader moves the R fader via echo.

import { MixerState } from '../state';
import { Dispatcher } from '../dispatcher';
import {
  makeRow,
  makeName,
  makeMeter,
  makeFader,
  makeFaderValue,
  makeMute,
  makeSolo,
} from './cells';

export function channelPair(
  oddIdx: number,
  state: MixerState,
  dispatcher: Dispatcher,
): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper pair-wrapper';

  const chL = state.channels[oddIdx];
  const chR = state.channels[oddIdx + 1];

  // Row 1: names (one per channel)
  const rowName = makeRow('row-name');
  rowName.append(makeName(chL.name), makeName(chR.name));

  // Row 2: meters
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeter(chL.peak, chL.rms), makeMeter(chR.peak, chR.rms));

  // Row 3: faders
  const rowFader = makeRow('row-fader');
  const faderL = makeFader(chL.fader, (v) => dispatcher.setChannelFader(oddIdx, v));
  const faderR = makeFader(chR.fader, (v) => dispatcher.setChannelFader(oddIdx + 1, v));
  rowFader.append(faderL, faderR);

  // Row 4: fader values
  const rowFv = makeRow('row-fv');
  rowFv.append(makeFaderValue(chL.fader), makeFaderValue(chR.fader));

  // Row 5: mutes (per channel)
  const rowMute = makeRow('row-mute');
  const muteL = makeMute(chL.on, () => dispatcher.setChannelOn(oddIdx, !chL.on.get()), {
    title: 'Mute (mix/on inverted — X32 idiom)',
  });
  const muteR = makeMute(chR.on, () => dispatcher.setChannelOn(oddIdx + 1, !chR.on.get()), {
    title: 'Mute (mix/on inverted — X32 idiom)',
  });
  rowMute.append(muteL, muteR);

  // Row 6: solos (per channel)
  const rowSolo = makeRow('row-solo');
  const soloL = makeSolo(chL.solo, () => dispatcher.setChannelSolo(oddIdx, !chL.solo.get()));
  const soloR = makeSolo(chR.solo, () => dispatcher.setChannelSolo(oddIdx + 1, !chR.solo.get()));
  rowSolo.append(soloL, soloR);

  // Row 7: pair-wide LINK button. Both sides remain fully interactive
  // when linked — dispatcher propagates writes optimistically and the
  // firmware echoes both back for convergence. The LINK button's active
  // state is the only visual indicator of the link status.
  const rowLink = makeRow('row-link');
  const linkBtn = document.createElement('button');
  linkBtn.className = 'link-btn wide cell';
  linkBtn.textContent = 'LINK';
  linkBtn.title = 'Stereo link — writes to either side move both';
  chL.link.subscribe((linked) => linkBtn.classList.toggle('active', linked));
  linkBtn.addEventListener('click', () =>
    dispatcher.setChannelLink(oddIdx, !chL.link.get()),
  );
  rowLink.append(linkBtn);
  // faderR / muteR / soloR are declared above but the linker-dim code
  // is gone; reference them once to keep linters happy and to document
  // they're intentionally live in both unlinked AND linked modes.
  void faderR; void muteR; void soloR;

  root.append(rowName, rowMeter, rowFader, rowFv, rowMute, rowSolo, rowLink);
  return root;
}

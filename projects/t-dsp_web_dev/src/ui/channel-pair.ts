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

import { MixerState, Signal } from '../state';
import { Dispatcher } from '../dispatcher';
import { OscMessage } from '../osc';
import {
  makeRow,
  makeSelectableName,
  makeMeter,
  makeFader,
  makeFaderValue,
  makeMute,
  makeSolo,
  makeRec,
  makeCellSpacer,
  makeGainKnob,
} from './cells';

// XLR channel indices (0-based) on the mixer map to TLV320ADC6140
// physical channels (1-based): idx 6 = XLR 1, idx 7 = XLR 2, etc.
// Used to wire the per-strip analog-gain knob to /codec/adc6140/ch/N/gain.
function xlrAdcChannel(channelIdx: number): number | null {
  if (channelIdx >= 6 && channelIdx <= 9) return channelIdx - 5;
  return null;
}

// Build the analog-gain knob cell for one channel, registering a
// firmware-echo listener on /codec/adc6140/ch/N/gain. The knob is the
// authoritative local mirror — both this knob and the System tab's
// ADC6140 settings panel subscribe to the same address (multi-listener
// registry in dispatcher.ts), so dragging either one updates the other.
function makeXlrGainKnob(adcCh: number, dispatcher: Dispatcher): HTMLElement {
  // 0-42 dB / 1 dB step matches adc6140-panel-config.ts. Default 0 dB
  // matches firmware POR (Adc6140Panel applies 0 dB on init).
  const value = new Signal<number>(0);
  const address = `/codec/adc6140/ch/${adcCh}/gain`;
  dispatcher.registerCodecListener(address, (msg: OscMessage) => {
    if (msg.types === 'f' && typeof msg.args[0] === 'number') value.set(msg.args[0]);
    else if (msg.types === 'i' && typeof msg.args[0] === 'number') value.set(msg.args[0]);
  });
  return makeGainKnob({
    value, min: 0, max: 42, step: 1, unit: 'dB',
    label: `XLR ${adcCh} analog gain`,
    onChange: (v) => dispatcher.sendRaw(address, 'f', [v]),
  });
}

export function channelPair(
  oddIdx: number,
  state: MixerState,
  dispatcher: Dispatcher,
): HTMLElement {
  const root = document.createElement('div');
  root.className = 'strip-wrapper pair-wrapper';

  const chL = state.channels[oddIdx];
  const chR = state.channels[oddIdx + 1];

  // Row 1: names (one per channel). Each is a Sel button — tapping sets
  // the global selectedChannel signal so the bottom-strip mini-bank
  // highlights this channel and (future) TUNE workspace renders its
  // detail page.
  const rowName = makeRow('row-name');
  rowName.append(
    makeSelectableName(chL.name, oddIdx,     state.selectedChannel),
    makeSelectableName(chR.name, oddIdx + 1, state.selectedChannel),
  );

  // Row 2: meters
  const rowMeter = makeRow('row-meter');
  rowMeter.append(makeMeter(chL.peak, chL.rms), makeMeter(chR.peak, chR.rms));

  // Row 2.5: analog-gain knobs (XLR strips only). Sits above the fader
  // because that's where a console's input-trim pot lives. Non-XLR pairs
  // (USB / Line / Mic) get spacer cells so row alignment holds across
  // the whole mixer row.
  const rowGain = makeRow('row-gain');
  const adcL = xlrAdcChannel(oddIdx);
  const adcR = xlrAdcChannel(oddIdx + 1);
  rowGain.append(
    adcL !== null ? makeXlrGainKnob(adcL, dispatcher) : makeCellSpacer('gain'),
    adcR !== null ? makeXlrGainKnob(adcR, dispatcher) : makeCellSpacer('gain'),
  );

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

  // Row 8: per-channel REC buttons. Each arms the channel's direct send
  // into USB capture. When main.loopEnable is true the main mix is
  // already being recorded, so these grey out — firmware forces them
  // off anyway to prevent double-counting.
  const rowRec = makeRow('row-rec');
  const recL = makeRec(
    chL.recSend,
    state.main.loopEnable,
    () => dispatcher.setChannelRecSend(oddIdx, !chL.recSend.get()),
  );
  const recR = makeRec(
    chR.recSend,
    state.main.loopEnable,
    () => dispatcher.setChannelRecSend(oddIdx + 1, !chR.recSend.get()),
  );
  rowRec.append(recL, recR);

  root.append(rowName, rowMeter, rowGain, rowFader, rowFv, rowMute, rowSolo, rowLink, rowRec);
  return root;
}

// Persistent bottom strip — visible across every tab in Engineer mode.
// Phase 0 of the UI rebuild epic. See planning/ui-rebuild/02-hierarchy.md.
//
// Renders a row of compact mini-strips (one per input channel) followed
// by a main meter+fader on the right. Each mini-strip is a tap target
// for the global selectedChannel signal — tapping selects that channel
// for the bottom-strip Sel highlight (and future TUNE workspace).
//
// Why this and not the existing channel-pair widget: the MIX strips are
// dense (24px buttons, 9px labels) because they pack everything into a
// 26px column. The bottom strip is the "ride levels from anywhere" path
// and gets touch-first sizing (44px+ tap targets, larger fader thumbs).
//
// Lifecycle: same as the rest of the UI — Signal subscribers fire
// imperatively. Meter rates use rafBatch so simultaneous blob writes
// across many meters collapse to one DOM write per frame, matching the
// behavior of the main MIX strip meters.

import { MixerState, Signal } from '../state';
import { Dispatcher } from '../dispatcher';
import { rafBatch } from '../raf-batch';
import { formatFaderDb } from './util';

// Same -60 dBFS floor and log mapping as cells.ts so the bottom-strip
// meters match the MIX strip meters visually for the same level. Two
// meter renderers in two files isn't ideal — promote to a shared helper
// when the third meter site shows up.
const METER_FLOOR_DB = -60;
function levelToScale(v: number): number {
  if (v <= 0) return 0;
  const db = 20 * Math.log10(v);
  if (db <= METER_FLOOR_DB) return 0;
  if (db >= 0) return 1;
  return (db - METER_FLOOR_DB) / -METER_FLOOR_DB;
}

function makeMiniMeter(peak: Signal<number>, rms: Signal<number>): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'bs-meter';
  const peakFill = document.createElement('div');
  peakFill.className = 'bs-meter-fill bs-peak';
  const rmsFill = document.createElement('div');
  rmsFill.className = 'bs-meter-fill bs-rms';
  wrap.append(rmsFill, peakFill);
  let lastPeak = 0;
  let lastRms = 0;
  peak.subscribe((p) => {
    lastPeak = p;
    rafBatch(peakFill, () => {
      peakFill.style.transform = `scaleY(${levelToScale(lastPeak)})`;
    });
  });
  rms.subscribe((r) => {
    lastRms = r;
    rafBatch(rmsFill, () => {
      rmsFill.style.transform = `scaleY(${levelToScale(lastRms)})`;
    });
  });
  return wrap;
}

// Mini-fader: native vertical range with extra-wide thumb via CSS so the
// touch target meets ~44px on the long axis. Future Phase 3 replaces
// this with a custom pointer-event fader; for Phase 0 the native input
// keeps the wiring simple and the dispatch path identical to MIX.
function makeMiniFader(
  faderSig: Signal<number>,
  onInput: (v: number) => void,
): HTMLInputElement {
  const f = document.createElement('input');
  f.type = 'range';
  f.className = 'bs-fader';
  f.min = '0';
  f.max = '1';
  f.step = '0.001';
  faderSig.subscribe((v) => (f.value = String(v)));
  f.addEventListener('input', () => onInput(parseFloat(f.value)));
  return f;
}

function makeMiniStrip(
  channelIdx: number,
  state: MixerState,
  dispatcher: Dispatcher,
): HTMLElement {
  const ch = state.channels[channelIdx];
  const wrap = document.createElement('div');
  wrap.className = 'bs-strip';

  // Top row: Sel name button, full strip width, drives selectedChannel.
  const sel = document.createElement('button');
  sel.className = 'bs-sel';
  sel.title = 'Tap to select';
  ch.name.subscribe((n) => (sel.textContent = n));
  state.selectedChannel.subscribe((s) =>
    wrap.classList.toggle('bs-sel-active', s === channelIdx),
  );
  sel.addEventListener('click', () => state.selectedChannel.set(channelIdx));

  // Middle row: meter + fader side by side. Keeps the dock short
  // (~140px) so it doesn't crowd the active workspace above it.
  const mid = document.createElement('div');
  mid.className = 'bs-mid';
  mid.append(
    makeMiniMeter(ch.peak, ch.rms),
    makeMiniFader(ch.fader, (v) => dispatcher.setChannelFader(channelIdx, v)),
  );

  // Bottom row: dB value + mute side by side.
  const bot = document.createElement('div');
  bot.className = 'bs-bot';

  const value = document.createElement('div');
  value.className = 'bs-value';
  ch.fader.subscribe((v) => (value.textContent = formatFaderDb(v)));

  const mute = document.createElement('button');
  mute.className = 'bs-mute';
  mute.textContent = 'M';
  mute.title = 'Mute';
  ch.on.subscribe((on) => mute.classList.toggle('active', !on));
  mute.addEventListener('click', () =>
    dispatcher.setChannelOn(channelIdx, !ch.on.get()),
  );

  bot.append(value, mute);
  wrap.append(sel, mid, bot);
  return wrap;
}

// Windows playback host volume — readout of what Windows' playback
// device slider is set to, plus the post-hostvol L/R meters showing
// what's actually leaving the device after Windows attenuation. The
// MIX outputDock has the full hostStrip widget; this is the compact
// dock-sized version. The "BP" indicator lights when host-vol is
// bypassed (state.main.hostvolEnable === false), telling the user
// at a glance that the Windows slider isn't being applied.
function makeHostOutSection(state: MixerState): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'bs-aux';

  const label = document.createElement('div');
  label.className = 'bs-aux-label';
  label.textContent = 'WIN OUT';

  const mid = document.createElement('div');
  mid.className = 'bs-mid';
  mid.append(
    makeMiniMeter(state.main.hostPeakL, state.main.hostRmsL),
    makeMiniMeter(state.main.hostPeakR, state.main.hostRmsR),
  );

  const bot = document.createElement('div');
  bot.className = 'bs-bot';

  const value = document.createElement('div');
  value.className = 'bs-value';
  state.main.hostvolValue.subscribe((v) => {
    value.textContent = `${Math.round(v * 100)}%`;
  });

  const bypass = document.createElement('div');
  bypass.className = 'bs-mute bs-readonly';
  bypass.textContent = 'BP';
  bypass.title = 'Windows volume bypassed (host-vol disabled in MIX)';
  state.main.hostvolEnable.subscribe((on) => {
    // hostvolEnable=false means the Windows slider is bypassed —
    // the fader is the only gain stage. Light the BP indicator then.
    bypass.classList.toggle('active', !on);
  });

  bot.append(value, bypass);
  wrap.append(label, mid, bot);
  return wrap;
}

// Windows recording-device level + mute. captureHostvolValue is the
// firmware's read-only mirror of the Windows recording slider; there
// is no input-side meter on the firmware side, so the level bar
// reflects the slider position itself rather than a measured signal.
// captureHostvolMute is the read-only mirror of the Windows recording
// mute toggle. Both update when the user moves the Windows slider or
// hits the mute key on a USB headset, etc.
function makeHostInSection(state: MixerState): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'bs-aux';

  const label = document.createElement('div');
  label.className = 'bs-aux-label';
  label.textContent = 'WIN IN';

  const mid = document.createElement('div');
  mid.className = 'bs-mid';

  // Slider-position bar — same vertical-bar shape as the meters but
  // driven by captureHostvolValue (0..1) directly, not a level
  // measurement. transform: scaleY for compositor-only updates.
  const barWrap = document.createElement('div');
  barWrap.className = 'bs-meter';
  const barFill = document.createElement('div');
  barFill.className = 'bs-meter-fill bs-host-in-fill';
  barWrap.appendChild(barFill);
  state.main.captureHostvolValue.subscribe((v) => {
    barFill.style.transform = `scaleY(${Math.max(0, Math.min(1, v))})`;
  });
  mid.appendChild(barWrap);

  const bot = document.createElement('div');
  bot.className = 'bs-bot';

  const value = document.createElement('div');
  value.className = 'bs-value';
  state.main.captureHostvolValue.subscribe((v) => {
    value.textContent = `${Math.round(v * 100)}%`;
  });

  const muteInd = document.createElement('div');
  muteInd.className = 'bs-mute bs-readonly';
  muteInd.textContent = 'M';
  muteInd.title = 'Windows recording-device mute (read-only echo)';
  state.main.captureHostvolMute.subscribe((muted) => {
    muteInd.classList.toggle('active', muted);
  });

  bot.append(value, muteInd);
  wrap.append(label, mid, bot);
  return wrap;
}

function makeMainSection(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'bs-main';

  const label = document.createElement('div');
  label.className = 'bs-main-label';
  label.textContent = 'MAIN';

  // Middle row: stereo meters + a single fader bound to faderL.
  // Stereo meters (post-fader / pre-hostvol) so the user can see the
  // actual mix level the bus is running at, regardless of what hostvol
  // is doing downstream. The single fader on faderL is a deliberate
  // ergonomics call for "ride the main level from anywhere" — when the
  // main bus is linked (default), firmware echoes R via the link; when
  // unlinked, the MIX strip is where you manipulate L/R independently.
  const mid = document.createElement('div');
  mid.className = 'bs-mid bs-main-mid';
  mid.append(
    makeMiniMeter(state.main.peakL, state.main.rmsL),
    makeMiniMeter(state.main.peakR, state.main.rmsR),
    makeMiniFader(state.main.faderL, (v) => dispatcher.setMainFaderL(v)),
  );

  const bot = document.createElement('div');
  bot.className = 'bs-bot';

  const value = document.createElement('div');
  value.className = 'bs-value';
  state.main.faderL.subscribe((v) => (value.textContent = formatFaderDb(v)));

  const mute = document.createElement('button');
  mute.className = 'bs-mute';
  mute.textContent = 'M';
  mute.title = 'Main mute';
  state.main.on.subscribe((on) => mute.classList.toggle('active', !on));
  mute.addEventListener('click', () => dispatcher.setMainOn(!state.main.on.get()));

  bot.append(value, mute);
  wrap.append(label, mid, bot);
  return wrap;
}

// Engineer variant of the persistent bottom strip. Renders mini-strips
// for every input channel followed by the main meter+fader. Visible on
// every tab via sticky positioning in style.css.
//
// Future Phase 1: paginate to 8-channel banks with swipe (planning/
// ui-rebuild/03-design-system.md). For now, all 10 channels render
// inline — fits comfortably on the Surface (1500-2000px target width).
export function bottomStrip(state: MixerState, dispatcher: Dispatcher): HTMLElement {
  const root = document.createElement('section');
  root.className = 'bottom-strip';

  const bank = document.createElement('div');
  bank.className = 'bs-bank';
  for (let i = 0; i < state.channels.length; i++) {
    bank.appendChild(makeMiniStrip(i, state, dispatcher));
  }

  root.append(
    bank,
    makeMainSection(state, dispatcher),
    makeHostOutSection(state),
    makeHostInSection(state),
  );
  return root;
}

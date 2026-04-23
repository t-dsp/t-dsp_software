// t-dsp web dev surface — TAC5212 audio shield adaptor.
//
// Entry point. Wires:
//   WebSerial port -> StreamDemuxer -> { OSC decoder, text console }
//   UI gestures   -> Dispatcher -> OSC encoder -> SLIP -> WebSerial port
//
// Chromium-only because it depends on the WebSerial API. See README.md.

import './style.css';

import { decodePacket, OscArg } from './osc';
import { slipEncode, StreamDemuxer } from './transport';
import { createMixerState } from './state';
import { Dispatcher } from './dispatcher';
import { tac5212Panel } from './codec-panel-config';

import { connectButton } from './ui/connect';
import { channelPair } from './ui/channel-pair';
import { mainBus } from './ui/main-bus';
import { hostStrip } from './ui/host-strip';
import { inputHostStrip } from './ui/input-host-strip';
import { codecPanel } from './ui/codec-panel';
import { serialConsole } from './ui/serial-console';
import { rawOsc } from './ui/raw-osc';
import { spectrumView } from './ui/spectrum';
import { keyboardView } from './ui/keyboard';
import { dexedPanel } from './ui/dexed-panel';
import { mpePanel } from './ui/mpe-panel';
import { neuroPanel } from './ui/neuro-panel';
import { acidPanel } from './ui/acid-panel';
import { supersawPanel } from './ui/supersaw-panel';
import { chipPanel } from './ui/chip-panel';
import { processingPanel } from './ui/processing-panel';
import { fxPanel } from './ui/fx-panel';
import { looperPanel } from './ui/looper-panel';
import { clockPanel } from './ui/clock-panel';
import { beatsPanel } from './ui/beats-panel';
import { arpPanel } from './ui/arp-panel';
import { synthBusStrip } from './ui/synth-bus';

// Channel count for the small mixer v1 — 6 channels (USB L/R, Line L/R,
// Mic L/R) matching tdsp::kChannelCount in lib/TDspMixer/src/MixerModel.h.
// Stereo-linked pairs by default: (1,2), (3,4), (5,6).
const CHANNEL_COUNT = 6;

const state = createMixerState(CHANNEL_COUNT);
const console_ = serialConsole({ onSubmit: (line) => sendText(line) });
const log = (line: string): void => console_.append(line);

// Transport: WebSocket bridge to a Node.js serial-bridge process.
// The bridge opens the COM port via `serialport` (which doesn't disrupt
// USB Audio) and relays raw bytes over a local WebSocket. The browser
// never touches WebSerial, avoiding Chrome renderer-process USB
// contention that causes audio buzz on composite devices.
const WS_URL = 'ws://localhost:8765';
let ws: WebSocket | null = null;

// Addresses whose traffic is high-frequency and should NOT be logged to
// the serial console pane. Meter blobs stream at 30 Hz and the channel
// strips display them visually — logging every one saturates the main
// thread and fills the scrollback with noise. Dispatcher still handles
// them; we just don't write them to the log.
const LOG_BLOCKED_ADDRESSES = new Set<string>([
  '/meters/input',
  '/meters/output',
  '/meters/host',
  '/meters/gr',
  // Spectrum blobs stream at ~30 Hz at 1 KB apiece — the rendered
  // line in the console would be both enormous and floodworthy, and
  // we learned twice already (see commit de74db28) that any high-
  // rate OSC address needs to be on this list or it saturates the
  // console pane.
  '/spectrum/main',
  // /main/st/hostvol/value can also fire frequently when the user drags
  // the Windows volume slider; allow it through for now but add here if
  // it becomes a problem.
]);

const demuxer = new StreamDemuxer(
  (frame) => {
    try {
      const messages = decodePacket(frame);
      for (const m of messages) {
        dispatcher.handleIncoming(m);
        if (!LOG_BLOCKED_ADDRESSES.has(m.address)) {
          log(`< ${m.address}${m.types ? ' ' + m.types : ''} ${m.args.map(formatArg).join(' ')}`);
        }
      }
    } catch (e) {
      log(`! decode error: ${(e as Error).message}`);
    }
  },
  (line) => log(line),
);

function formatArg(a: OscArg): string {
  if (a instanceof Uint8Array) return `<blob:${a.length}>`;
  if (typeof a === 'number') return Number.isInteger(a) ? a.toString() : a.toFixed(3);
  if (a === null) return 'null';
  return String(a);
}

const send = (packet: Uint8Array): void => {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    log('! not connected');
    return;
  }
  ws.send(slipEncode(packet));
};

// Plain-text CLI write. Bypasses SLIP — see firmware's SlipOscTransport.
const sendText = (line: string): void => {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    log('! not connected');
    return;
  }
  ws.send(new TextEncoder().encode(line + '\n'));
};

const dispatcher = new Dispatcher(state, send);

async function connect(): Promise<void> {
  try {
    if (ws && ws.readyState === WebSocket.OPEN) {
      log('! already connected');
      return;
    }
    log(`connecting to bridge at ${WS_URL}...`);
    const socket = new WebSocket(WS_URL);
    socket.binaryType = 'arraybuffer';

    await new Promise<void>((resolve, reject) => {
      socket.onopen = () => resolve();
      socket.onerror = () => reject(new Error(
        `cannot reach bridge at ${WS_URL} — is serial-bridge running? (pnpm bridge)`));
    });

    ws = socket;
    state.connected.set(true);
    log('-- connected via bridge --');

    socket.onmessage = (ev) => {
      demuxer.feed(new Uint8Array(ev.data as ArrayBuffer));
    };

    socket.onclose = () => {
      log('-- bridge disconnected --');
      ws = null;
      state.connected.set(false);
    };

    socket.onerror = () => {
      log('! bridge error');
    };

    // Snapshot + re-subscribe after a short settle delay. The firmware's
    // transport layer throttles incoming OSC frames (one per 3ms) to
    // prevent CDC burst contention with USB Audio, so it's safe to send
    // all commands at once — they'll be processed with natural spacing.
    setTimeout(() => {
      dispatcher.requestSnapshot();
      // Dexed bank list is compile-time fixed; one-shot query on connect.
      // Voice names are bank-scoped — fetch for the currently-selected
      // bank so the dropdown is populated on first Synth tab open
      // (bank signal defaults to 0, which is what the firmware boots to).
      dispatcher.queryDexedBankNames();
      dispatcher.queryDexedVoiceNames(state.dexed.bank.get());
      if (state.metersOn.get()) {
        dispatcher.subscribeMeters();
      }
      if (spectrum.isRunning()) {
        dispatcher.subscribeSpectrum();
      }
      if (keyboard.isRunning()) {
        dispatcher.subscribeMidi();
      }
    }, 150);
  } catch (e) {
    log(`! connect failed: ${(e as Error).message}`);
  }
}

async function disconnect(): Promise<void> {
  try {
    if (ws) {
      ws.close();
      ws = null;
    }
  } catch (e) {
    log(`! disconnect error: ${(e as Error).message}`);
  } finally {
    state.connected.set(false);
    log('-- disconnected --');
  }
}

// ---------- layout ----------

const app = document.getElementById('app');
if (!app) throw new Error('missing #app');

const header = document.createElement('header');
header.className = 'header';

const title = document.createElement('h1');
title.textContent = 't-dsp web dev surface — TAC5212';
header.appendChild(title);

const meterToggle = document.createElement('button');
meterToggle.className = 'meter-toggle';
state.metersOn.subscribe((on) => {
  meterToggle.textContent = on ? 'Meters: ON' : 'Meters: OFF';
  meterToggle.classList.toggle('active', on);
});
meterToggle.addEventListener('click', () => {
  if (state.metersOn.get()) dispatcher.unsubscribeMeters();
  else dispatcher.subscribeMeters();
});
header.appendChild(meterToggle);

header.appendChild(connectButton(state.connected, connect, disconnect));

// --- View tab bar -------------------------------------------------
//
// Two top-level views share the content area: the mixer (channel
// strips + codec panel + raw OSC) and the stereo spectrum analyzer.
// The tab bar toggles display between two wrapper sections. Serial
// console stays below both — it's useful in either view.

const viewTabs = document.createElement('nav');
viewTabs.className = 'view-tabs';
const mixerTab = document.createElement('button');
mixerTab.className = 'view-tab active';
mixerTab.dataset.view = 'mixer';
mixerTab.textContent = 'Mixer';
const spectrumTab = document.createElement('button');
spectrumTab.className = 'view-tab';
spectrumTab.dataset.view = 'spectrum';
spectrumTab.textContent = 'Spectrum';
const synthTab = document.createElement('button');
synthTab.className = 'view-tab';
synthTab.dataset.view = 'synth';
synthTab.textContent = 'Synth';
const fxTab = document.createElement('button');
fxTab.className = 'view-tab';
fxTab.dataset.view = 'fx';
fxTab.textContent = 'FX';
const processingTab = document.createElement('button');
processingTab.className = 'view-tab';
processingTab.dataset.view = 'processing';
processingTab.textContent = 'Processing';
const loopTab = document.createElement('button');
loopTab.className = 'view-tab';
loopTab.dataset.view = 'loop';
loopTab.textContent = 'Loop';
const beatsTab = document.createElement('button');
beatsTab.className = 'view-tab';
beatsTab.dataset.view = 'beats';
beatsTab.textContent = 'Beats';
const clockTab = document.createElement('button');
clockTab.className = 'view-tab';
clockTab.dataset.view = 'clock';
clockTab.textContent = 'Clock';
const arpTab = document.createElement('button');
arpTab.className = 'view-tab';
arpTab.dataset.view = 'arp';
arpTab.textContent = 'Arp';
viewTabs.append(mixerTab, spectrumTab, synthTab, fxTab, processingTab, loopTab, beatsTab, clockTab, arpTab);

// --- Mixer view section (wraps existing mixer content) ------------

const mixerView = document.createElement('section');
mixerView.className = 'view view-mixer';

const mixerRow = document.createElement('section');
mixerRow.className = 'mixer-row';
// 3 stereo pairs (1/2, 3/4, 5/6) on the left, then the SYNTH group
// strip, then MAIN + HOST docked to the right edge via `margin-left:
// auto` on the output dock. Every wrapper uses the shared 7-row
// layout so buttons and sliders align across the mixer.
for (let oddIdx = 0; oddIdx < CHANNEL_COUNT; oddIdx += 2) {
  mixerRow.appendChild(channelPair(oddIdx, state, dispatcher));
}
// Synth group bus — sits between the input strips and the output
// dock so it visually belongs with the input-side column of faders.
mixerRow.appendChild(synthBusStrip(state.synthBus, dispatcher));

const outputDock = document.createElement('div');
outputDock.className = 'output-dock';
outputDock.append(
  mainBus(state.main, dispatcher),
  hostStrip(state.main, dispatcher),
  inputHostStrip(state.main),
);
mixerRow.appendChild(outputDock);

const codecSection = document.createElement('section');
codecSection.className = 'codec-section';
codecSection.appendChild(codecPanel(tac5212Panel, dispatcher));

const rawSection = document.createElement('section');
rawSection.className = 'raw-section';
const rawLabel = document.createElement('h4');
rawLabel.textContent = 'Raw OSC';
rawSection.append(rawLabel, rawOsc(dispatcher, log));

mixerView.append(mixerRow, codecSection, rawSection);

// --- Spectrum view section ----------------------------------------

const spectrum = spectrumView();
dispatcher.setSpectrumSink((bytes) => spectrum.update(bytes));

const spectrumSection = document.createElement('section');
spectrumSection.className = 'view view-spectrum';
spectrumSection.style.display = 'none';
spectrumSection.appendChild(spectrum.element);

// --- Synth view section -------------------------------------------
//
// Three stacked rows inside the Synth view:
//   1. synth-subnav    — sub-tab bar for individual synth engines
//                        (empty placeholder; Dexed + future synths land here).
//   2. synth-content   — active engine's config panel (voice select,
//                        volume, MIDI channel, etc). Empty placeholder
//                        until Phase 4 wires Dexed in.
//   3. synth-keyboard  — the 88-key piano, shared across all synth
//                        sub-tabs so every engine plays from the same
//                        keyboard. Clicking a key sends /midi/note/in;
//                        the firmware echoes back /midi/note which
//                        lights the same key up (round-trip proof).

const keyboard = keyboardView();
dispatcher.setMidiSink((note, velocity, channel) => {
  // velocity > 0 → note-on; velocity == 0 → note-off (standard MIDI
  // running-status). All channels light the same keyboard — the per-
  // channel breakdown is surfaced via the banner readout in keyboardView.
  keyboard.setNote(note, velocity > 0, channel);
  // Auto-follow: on note-on, switch the active sub-tab to whichever
  // synth claims this channel. Lets the user pick up an unknown-channel
  // keyboard (or an MPE one that spreads notes across channels) without
  // hunting for the right tab. Resolver + selectSynthSubtab are defined
  // below; the closure resolves them at call time, post-init.
  if (velocity > 0 && channel >= 1 && channel <= 16) {
    const target = resolveAutoTarget(channel);
    if (target && target !== activeSynthSubtab) selectSynthSubtab(target);
  }
});

// Active synth sub-tab dictates which MIDI channel the on-screen
// keyboard targets. Dexed listens on channel 1 (its default); MPE
// treats channel 1 as its master channel (notes silently dropped per
// MPE spec) so we send on channel 2, a member channel MpeVaSink will
// actually allocate a voice for.
let activeSynthSubtab: 'dexed' | 'mpe' | 'neuro' | 'acid' | 'supersaw' | 'chip' = 'dexed';
const keyboardChannelForSubtab = (): number => {
  // Per-synth default channels: Dexed=1, MPE member=2 (ch 1 is its
  // master/ignored channel per MPE spec), Neuro=3, Acid=4, Supersaw=5,
  // Chip=6. On-screen keys always reach the currently-visible synth.
  switch (activeSynthSubtab) {
    case 'mpe':      return 2;
    case 'neuro':    return 3;
    case 'acid':     return 4;
    case 'supersaw': return 5;
    case 'chip':     return 6;
    default:         return 1;
  }
};
keyboard.onPress((note, down) => {
  // Fixed velocity 100 for Phase 1 — velocity-from-gesture is a
  // follow-on. Channel routes by active sub-tab so the key you press
  // always reaches the synth you're looking at.
  dispatcher.sendMidiNote(note, down ? 100 : 0, keyboardChannelForSubtab());
});

const synthSection = document.createElement('section');
synthSection.className = 'view view-synth';
synthSection.style.display = 'none';

// Synth sub-tabs — Dexed (FM) and MPE (VA). One content container
// swaps its visible panel on sub-tab click; both panels stay mounted
// so their subscribe/unsubscribe cycles are triggered by the
// setActive hooks rather than DOM add/remove (keeps canvas scroll
// state, form values, etc. stable across switches).
const synthSubnav = document.createElement('nav');
synthSubnav.className = 'synth-subnav';

// Each sub-tab button has a label + a "playing" dot that lights
// up when the synth's on state is true. Keeps the on/off state
// visible even when the sub-tab isn't selected.
const makeSubtab = (label: string): HTMLButtonElement => {
  const b = document.createElement('button');
  b.className = 'synth-subnav-tab';
  const dot = document.createElement('span');
  dot.className = 'synth-subnav-dot';
  const text = document.createElement('span');
  text.textContent = label;
  b.append(dot, text);
  return b;
};
const dexedSubtab = makeSubtab('Dexed');
dexedSubtab.classList.add('active');
state.dexed.on.subscribe((on) => dexedSubtab.classList.toggle('playing', on));
const mpeSubtab = makeSubtab('MPE');
state.mpe.on.subscribe((on) => mpeSubtab.classList.toggle('playing', on));
const neuroSubtab = makeSubtab('Neuro');
state.neuro.on.subscribe((on) => neuroSubtab.classList.toggle('playing', on));
const acidSubtab = makeSubtab('Acid');
state.acid.on.subscribe((on) => acidSubtab.classList.toggle('playing', on));
const supersawSubtab = makeSubtab('Supersaw');
state.supersaw.on.subscribe((on) => supersawSubtab.classList.toggle('playing', on));
const chipSubtab = makeSubtab('Chip');
state.chip.on.subscribe((on) => chipSubtab.classList.toggle('playing', on));
synthSubnav.append(dexedSubtab, mpeSubtab, neuroSubtab, acidSubtab, supersawSubtab, chipSubtab);

const synthContent = document.createElement('div');
synthContent.className = 'synth-content';
const dexedPanelEl = dexedPanel(state, dispatcher);
const mpe = mpePanel(state, dispatcher);
const neuro = neuroPanel(state, dispatcher);
const acid = acidPanel(state, dispatcher);
const supersaw = supersawPanel(state, dispatcher);
const chip = chipPanel(state, dispatcher);
mpe.element.style.display = 'none';
neuro.element.style.display = 'none';
acid.element.style.display = 'none';
supersaw.element.style.display = 'none';
chip.element.style.display = 'none';
synthContent.append(dexedPanelEl, mpe.element, neuro.element, acid.element, supersaw.element, chip.element);

type SynthSubtab = 'dexed' | 'mpe' | 'neuro' | 'acid' | 'supersaw' | 'chip';

// MIDI-Auto enforcer — only touches on-states for synths whose
// midiAuto flag is TRUE. Gives each synth an opt-out: unchecking
// MIDI Auto on (say) Dexed means the Dexed on-state stays put even
// as you switch tabs, so you can layer Dexed + Supersaw on the
// same performance. Only fires when state.connected is true —
// otherwise the OSC writes are thrown away into a disconnected
// bridge and the firmware's defaults stay intact.
const enforceMidiAuto = (): void => {
  if (!state.connected.get()) return;
  if (state.dexed.midiAuto.get()) {
    const want = activeSynthSubtab === 'dexed';
    if (state.dexed.on.get() !== want) dispatcher.setDexedOn(want);
  }
  if (state.mpe.midiAuto.get()) {
    const want = activeSynthSubtab === 'mpe';
    if (state.mpe.on.get() !== want) dispatcher.setMpeOn(want);
  }
  if (state.neuro.midiAuto.get()) {
    const want = activeSynthSubtab === 'neuro';
    if (state.neuro.on.get() !== want) dispatcher.setNeuroOn(want);
  }
  if (state.acid.midiAuto.get()) {
    const want = activeSynthSubtab === 'acid';
    if (state.acid.on.get() !== want) dispatcher.setAcidOn(want);
  }
  if (state.supersaw.midiAuto.get()) {
    const want = activeSynthSubtab === 'supersaw';
    if (state.supersaw.on.get() !== want) dispatcher.setSupersawOn(want);
  }
  if (state.chip.midiAuto.get()) {
    const want = activeSynthSubtab === 'chip';
    if (state.chip.on.get() !== want) dispatcher.setChipOn(want);
  }
};

// Channel → synth resolver for the auto-follow-the-keyboard behaviour.
// Priority: explicit per-synth midiChannel matches first (Dexed 1,
// Neuro 3, Acid 4, Supersaw 5, Chip 6), then MPE as the catch-all so
// MPE keyboards (notes spread across member channels 2..16) and
// single-channel keyboards on an unclaimed channel both land on MPE.
// A claimed channel is claimed regardless of that synth's Auto state
// — turning Dexed Auto off doesn't cause channel 1 traffic to bleed
// into MPE; it just means we won't switch to Dexed for it.
const resolveAutoTarget = (channel: number): SynthSubtab | null => {
  const dexedCh    = state.dexed.midiChannel.get();
  const neuroCh    = state.neuro.midiChannel.get();
  const acidCh     = state.acid.midiChannel.get();
  const supersawCh = state.supersaw.midiChannel.get();
  const chipCh     = state.chip.midiChannel.get();
  if (dexedCh    !== 0 && dexedCh    === channel) return state.dexed.midiAuto.get()    ? 'dexed'    : null;
  if (neuroCh    !== 0 && neuroCh    === channel) return state.neuro.midiAuto.get()    ? 'neuro'    : null;
  if (acidCh     !== 0 && acidCh     === channel) return state.acid.midiAuto.get()     ? 'acid'     : null;
  if (supersawCh !== 0 && supersawCh === channel) return state.supersaw.midiAuto.get() ? 'supersaw' : null;
  if (chipCh     !== 0 && chipCh     === channel) return state.chip.midiAuto.get()     ? 'chip'     : null;
  return state.mpe.midiAuto.get() ? 'mpe' : null;
};

const selectSynthSubtab = (which: SynthSubtab): void => {
  activeSynthSubtab = which;
  dexedSubtab.classList.toggle('active', which === 'dexed');
  mpeSubtab.classList.toggle('active',   which === 'mpe');
  neuroSubtab.classList.toggle('active', which === 'neuro');
  acidSubtab.classList.toggle('active',  which === 'acid');
  supersawSubtab.classList.toggle('active', which === 'supersaw');
  chipSubtab.classList.toggle('active',  which === 'chip');
  dexedPanelEl.style.display  = which === 'dexed'    ? '' : 'none';
  mpe.element.style.display   = which === 'mpe'      ? '' : 'none';
  neuro.element.style.display = which === 'neuro'    ? '' : 'none';
  acid.element.style.display  = which === 'acid'     ? '' : 'none';
  supersaw.element.style.display = which === 'supersaw' ? '' : 'none';
  chip.element.style.display  = which === 'chip'     ? '' : 'none';
  // MPE is the only panel with active-hook telemetry; Acid/Supersaw/
  // Chip are mono with no voice orbs.
  mpe.setActive(which === 'mpe');
  neuro.setActive(which === 'neuro');
  acid.setActive(which === 'acid');
  supersaw.setActive(which === 'supersaw');
  chip.setActive(which === 'chip');
  enforceMidiAuto();
};

// Re-apply on connect so the firmware's cold-boot defaults (all synths
// on) converge to the UI's sub-tab-follows layout. Re-apply whenever
// any synth's midiAuto toggles ON — flipping Auto on immediately
// enforces its rule.
state.connected.subscribe((c) => { if (c) enforceMidiAuto(); });
state.dexed.midiAuto   .subscribe((on) => { if (on) enforceMidiAuto(); });
state.mpe.midiAuto     .subscribe((on) => { if (on) enforceMidiAuto(); });
state.neuro.midiAuto   .subscribe((on) => { if (on) enforceMidiAuto(); });
state.acid.midiAuto    .subscribe((on) => { if (on) enforceMidiAuto(); });
state.supersaw.midiAuto.subscribe((on) => { if (on) enforceMidiAuto(); });
state.chip.midiAuto    .subscribe((on) => { if (on) enforceMidiAuto(); });

dexedSubtab   .addEventListener('click', () => selectSynthSubtab('dexed'));
mpeSubtab     .addEventListener('click', () => selectSynthSubtab('mpe'));
neuroSubtab   .addEventListener('click', () => selectSynthSubtab('neuro'));
acidSubtab    .addEventListener('click', () => selectSynthSubtab('acid'));
supersawSubtab.addEventListener('click', () => selectSynthSubtab('supersaw'));
chipSubtab    .addEventListener('click', () => selectSynthSubtab('chip'));

const synthKeyboardDock = document.createElement('div');
synthKeyboardDock.className = 'synth-keyboard-dock';
synthKeyboardDock.appendChild(keyboard.element);

synthSection.append(synthSubnav, synthContent, synthKeyboardDock);

// --- FX view section ----------------------------------------------
//
// Shared send-bus FX (chorus + reverb). Sibling of the per-synth
// tabs — any synth routes into this bus via its own FX Send slider.
// Lives next to Processing (also an audio-processing tab) so the two
// stay visually grouped in the nav.

const fxSection = document.createElement('section');
fxSection.className = 'view view-fx';
fxSection.style.display = 'none';
fxSection.appendChild(fxPanel(state, dispatcher));

// --- Processing view section --------------------------------------
//
// Main-bus output processing: high-shelf EQ + peak limiter. Both
// default to ON in the firmware (g_procShelfEnable / g_procLimiter-
// Enable = true) so that a first-time user isn't fatigued by raw
// DX7 sizzle or an accidentally-loud transient before they've found
// this tab. Toggles here flip the firmware state via OSC.

const processingSection = document.createElement('section');
processingSection.className = 'view view-processing';
processingSection.style.display = 'none';
processingSection.appendChild(processingPanel(state, dispatcher));

// --- Loop view section --------------------------------------------
//
// Mono sample looper fed off a selectable pre-fader channel tap.
// Sits alongside FX / Processing because it's another main-bus
// audio-path feature rather than a synth engine.
const loopSection = document.createElement('section');
loopSection.className = 'view view-loop';
loopSection.style.display = 'none';
loopSection.appendChild(looperPanel(state, dispatcher));

// --- Beats view section -------------------------------------------
//
// 4-track × 16-step drum machine. Synth drums on tracks 0/1, SD WAV
// samples on tracks 2/3. See ui/beats-panel.ts for the full layout.
const beatsSection = document.createElement('section');
beatsSection.className = 'view view-beats';
beatsSection.style.display = 'none';
beatsSection.appendChild(beatsPanel(state, dispatcher));

// --- Clock view section -------------------------------------------
//
// Shared musical clock — one source for every tempo-aware module. See
// ui/clock-panel.ts. No live subscription: the four clock echoes fire
// on change, and the panel's one-shot queryClockRunning() fills in the
// transport state when the tab opens.
const clockSection = document.createElement('section');
clockSection.className = 'view view-clock';
clockSection.style.display = 'none';
clockSection.appendChild(clockPanel(state, dispatcher));

// --- Arp view section ---------------------------------------------
//
// Top-level tab (not a Synth sub-tab) because the arp is a MIDI
// processor that sits between the router and the synth sinks — it
// intercepts keystrokes bound for Dexed/MPE/Neuro/Acid/Supersaw/Chip
// alike, so it doesn't belong under any one engine's subnav.
const arp = arpPanel(state, dispatcher);
const arpSection = document.createElement('section');
arpSection.className = 'view view-arp';
arpSection.style.display = 'none';
arpSection.appendChild(arp.element);

// --- Tab switching -------------------------------------------------

type ViewName = 'mixer' | 'spectrum' | 'synth' | 'fx' | 'processing' | 'loop' | 'beats' | 'clock' | 'arp';

function selectView(name: ViewName): void {
  mixerTab.classList.toggle('active',      name === 'mixer');
  spectrumTab.classList.toggle('active',   name === 'spectrum');
  synthTab.classList.toggle('active',      name === 'synth');
  fxTab.classList.toggle('active',         name === 'fx');
  processingTab.classList.toggle('active', name === 'processing');
  loopTab.classList.toggle('active',       name === 'loop');
  beatsTab.classList.toggle('active',      name === 'beats');
  clockTab.classList.toggle('active',      name === 'clock');
  arpTab.classList.toggle('active',        name === 'arp');
  mixerView.style.display         = name === 'mixer'      ? '' : 'none';
  spectrumSection.style.display   = name === 'spectrum'   ? '' : 'none';
  synthSection.style.display      = name === 'synth'      ? '' : 'none';
  fxSection.style.display         = name === 'fx'         ? '' : 'none';
  processingSection.style.display = name === 'processing' ? '' : 'none';
  loopSection.style.display       = name === 'loop'       ? '' : 'none';
  beatsSection.style.display      = name === 'beats'      ? '' : 'none';
  clockSection.style.display      = name === 'clock'      ? '' : 'none';
  arpSection.style.display        = name === 'arp'        ? '' : 'none';

  // Toggle the body-level class that makes #app break out of its
  // 1200px max-width and go full viewport in spectrum mode. Only the
  // spectrum tab uses this full-bleed layout; the synth view happily
  // fits inside the normal column.
  document.body.classList.toggle('spectrum-active', name === 'spectrum');

  // Leaving spectrum: stop the render loop + unsubscribe so the
  // firmware stops computing FFTs nobody will see.
  if (name !== 'spectrum' && spectrum.isRunning()) {
    spectrum.stop();
    dispatcher.unsubscribeSpectrum();
  }
  // Leaving synth: clear keyboard state, unsubscribe so the firmware
  // stops forwarding MIDI events over USB CDC. Also turn off MPE
  // voice telemetry if the MPE sub-tab was active.
  if (name !== 'synth' && keyboard.isRunning()) {
    keyboard.stop();
    dispatcher.unsubscribeMidi();
    mpe.setActive(false);
  }

  if (name === 'spectrum' && !spectrum.isRunning()) {
    dispatcher.subscribeSpectrum();
    spectrum.start();
  }
  if (name === 'synth' && !keyboard.isRunning()) {
    dispatcher.subscribeMidi();
    keyboard.start();
    // If the MPE sub-tab was the last active one, re-subscribe its
    // telemetry stream. Default sub-tab is Dexed so first visit is a
    // no-op.
    if (mpeSubtab.classList.contains('active')) mpe.setActive(true);
  }
}
mixerTab.addEventListener('click',      () => selectView('mixer'));
spectrumTab.addEventListener('click',   () => selectView('spectrum'));
synthTab.addEventListener('click',      () => selectView('synth'));
fxTab.addEventListener('click',         () => selectView('fx'));
processingTab.addEventListener('click', () => selectView('processing'));
loopTab.addEventListener('click',       () => selectView('loop'));
beatsTab.addEventListener('click',      () => selectView('beats'));
clockTab.addEventListener('click',      () => selectView('clock'));
arpTab.addEventListener('click',        () => selectView('arp'));

app.append(header, viewTabs, mixerView, spectrumSection, synthSection, fxSection, processingSection, loopSection, beatsSection, clockSection, arpSection, console_.element);

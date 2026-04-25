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
import { adc6140Panel } from './adc6140-panel-config';

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
import { bottomStrip } from './ui/bottom-strip';

// Channel count — 10 channels matching tdsp::kChannelCount in firmware.
//   1  USB L         } stereo-linked by default
//   2  USB R         }
//   3  Line L        } stereo-linked by default
//   4  Line R        }
//   5  Mic L         } stereo-linked by default
//   6  Mic R         }
//   7  XLR 1         } shown as a pair purely for layout (firmware
//   8  XLR 2         } defaults link=false so they behave as independent
//   9  XLR 3         } mono channels — each XLR strip's fader/mute/solo
//  10  XLR 4         } moves on its own).
const CHANNEL_COUNT = 10;

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
  // Diagnostic: log outgoing /codec/* traffic so we can tell whether the
  // client side actually hands a packet to the bridge. Silence per-control
  // chatter (mix/fader, sliders) so we don't drown the console.
  // Decoding the address out of the OSC packet: address is a NUL-padded
  // string at offset 0, ending at the first 0x00 byte.
  let end = 0;
  while (end < packet.length && packet[end] !== 0) end++;
  const addr = new TextDecoder().decode(packet.subarray(0, end));
  if (addr.startsWith('/codec/')) {
    log(`> ${addr}`);
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

// Persona toggle — swaps the persistent bottom strip between the
// Engineer fader-bank variant and the Musician keyboard variant.
// Persists to localStorage so reloads land in the user's preferred
// mode. Phase 0 of the UI rebuild epic; in Phase 1 this also drives
// default-landing-workspace and tab order. See planning/ui-rebuild/.
const MODE_LS_KEY = 't-dsp.ui.mode';
const savedMode = localStorage.getItem(MODE_LS_KEY);
if (savedMode === 'engineer' || savedMode === 'musician') {
  state.mode.set(savedMode);
}
state.mode.subscribe((m) => {
  try { localStorage.setItem(MODE_LS_KEY, m); } catch { /* private mode etc. */ }
});

const modeToggle = document.createElement('button');
modeToggle.className = 'mode-toggle';
modeToggle.title = 'Toggle Engineer / Musician mode';
state.mode.subscribe((m) => {
  modeToggle.textContent = m === 'engineer' ? 'Mode: Engineer' : 'Mode: Musician';
  modeToggle.classList.toggle('musician', m === 'musician');
});
modeToggle.addEventListener('click', () => {
  state.mode.set(state.mode.get() === 'engineer' ? 'musician' : 'engineer');
});
header.appendChild(modeToggle);

header.appendChild(connectButton(state.connected, connect, disconnect));

// --- Workspace tab bar (Phase 1 of UI rebuild) -------------------
//
// Five top-level workspaces grouped by signal-flow role:
//   MIX     — channel strips, main bus, sends-on-faders (Phase 6)
//   PLAY    — synth engines, arp, beats, looper (musician-tier)
//   TUNE    — selected-channel processing detail (Phase 5; stub now)
//   FX      — bus FX, main processing, spectrum analyzer
//   SETUP   — codec panels, clock, raw OSC, serial console
//
// Inner sub-tabs live inside each workspace's container (built below).
// See planning/ui-rebuild/02-hierarchy.md.

type Workspace = 'mix' | 'play' | 'tune' | 'fx' | 'setup';
type PlayTab   = 'synths' | 'arp' | 'beats' | 'loop';
type FxTab     = 'busfx' | 'processing' | 'spectrum';
type SetupTab  = 'codec' | 'clock' | 'rawosc' | 'serial';

const viewTabs = document.createElement('nav');
viewTabs.className = 'view-tabs';

function makeWorkspaceTab(label: string, ws: Workspace): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = 'view-tab workspace-tab';
  b.dataset.workspace = ws;
  b.textContent = label;
  return b;
}
const mixWsTab   = makeWorkspaceTab('MIX',   'mix');
const playWsTab  = makeWorkspaceTab('PLAY',  'play');
const tuneWsTab  = makeWorkspaceTab('TUNE',  'tune');
const fxWsTab    = makeWorkspaceTab('FX',    'fx');
const setupWsTab = makeWorkspaceTab('SETUP', 'setup');
viewTabs.append(mixWsTab, playWsTab, tuneWsTab, fxWsTab, setupWsTab);

// Helper for inner sub-tab buttons (PLAY / FX / SETUP).
function makeSubnavTab(label: string, key: string): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = 'subnav-tab';
  b.dataset.subtab = key;
  b.textContent = label;
  return b;
}

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

// Raw OSC widget — moves to SETUP > Raw OSC in Phase 1 of the UI
// rebuild. The section is defined here next to the other in-mixer
// widgets but appended into setupWorkspace later. Only the mixer-row
// goes into MIX now.
const rawOscSection = document.createElement('section');
rawOscSection.className = 'raw-section view view-rawosc';
const rawLabel = document.createElement('h4');
rawLabel.textContent = 'Raw OSC';
rawOscSection.append(rawLabel, rawOsc(dispatcher, log));

mixerView.append(mixerRow);

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
  // The active sub-tab is sticky: whichever synth tab the user has open
  // receives the MIDI (via midiAuto muting the others). We deliberately
  // do NOT auto-switch tabs on note-on — that fought the user's explicit
  // selection when a fixed-channel keyboard didn't match the open tab.
  keyboard.setNote(note, velocity > 0, channel);
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
  // Always force the active tab's synth ON and every other synth with
  // midiAuto=true OFF. Send unconditionally (no local on-state diff check)
  // so firmware reboots, snapshot races, or any other UI/firmware drift
  // can't leave a stale synth audible. midiAuto=false opts a synth out
  // of the enforcer entirely — that's the layering escape hatch.
  const force = (which: SynthSubtab, midiAuto: boolean, set: (on: boolean) => void) => {
    if (which === activeSynthSubtab) { set(true); return; }
    if (midiAuto) set(false);
  };
  force('dexed',    state.dexed.midiAuto.get(),    (v) => dispatcher.setDexedOn(v));
  force('mpe',      state.mpe.midiAuto.get(),      (v) => dispatcher.setMpeOn(v));
  force('neuro',    state.neuro.midiAuto.get(),    (v) => dispatcher.setNeuroOn(v));
  force('acid',     state.acid.midiAuto.get(),     (v) => dispatcher.setAcidOn(v));
  force('supersaw', state.supersaw.midiAuto.get(), (v) => dispatcher.setSupersawOn(v));
  force('chip',     state.chip.midiAuto.get(),     (v) => dispatcher.setChipOn(v));
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

// --- System view section ------------------------------------------
//
// Hardware-level chip configuration. Two codec chips live on this
// board — TAC5212 (DAC + onboard line/PDM ADC) and TLV320ADC6140
// (external XLR mic preamp × 4). Each gets its own panel rendered
// from a panel-config descriptor. A left-rail submenu switches
// between them so the chip-level controls don't crowd the mixer.

const systemSection = document.createElement('section');
systemSection.className = 'view view-system';
systemSection.style.display = 'none';

const systemSubnav = document.createElement('nav');
systemSubnav.className = 'system-subnav';

const tacSubtab = document.createElement('button');
tacSubtab.className = 'system-subnav-tab active';
tacSubtab.textContent = 'TAC5212';

const adcSubtab = document.createElement('button');
adcSubtab.className = 'system-subnav-tab';
adcSubtab.textContent = 'TLV320ADC6140';

systemSubnav.append(tacSubtab, adcSubtab);

const systemContent = document.createElement('div');
systemContent.className = 'system-content';

const tacChipPanel = document.createElement('div');
tacChipPanel.className = 'system-chip-panel';
const tacChipHeader = document.createElement('h3');
tacChipHeader.className = 'codec-section-header';
tacChipHeader.textContent = 'TAC5212 (DAC + Line/PDM ADC)';
tacChipPanel.append(tacChipHeader, codecPanel(tac5212Panel, dispatcher));

const adcChipPanel = document.createElement('div');
adcChipPanel.className = 'system-chip-panel';
adcChipPanel.style.display = 'none';
const adcChipHeader = document.createElement('h3');
adcChipHeader.className = 'codec-section-header';
adcChipHeader.textContent = 'TLV320ADC6140 (XLR Mic Preamp × 4)';
adcChipPanel.append(adcChipHeader, codecPanel(adc6140Panel, dispatcher));

systemContent.append(tacChipPanel, adcChipPanel);
systemSection.append(systemSubnav, systemContent);

type SystemSubtab = 'tac5212' | 'adc6140';
const selectSystemSubtab = (which: SystemSubtab): void => {
  tacSubtab.classList.toggle('active', which === 'tac5212');
  adcSubtab.classList.toggle('active', which === 'adc6140');
  tacChipPanel.style.display = which === 'tac5212' ? '' : 'none';
  adcChipPanel.style.display = which === 'adc6140' ? '' : 'none';
};
tacSubtab.addEventListener('click', () => selectSystemSubtab('tac5212'));
adcSubtab.addEventListener('click', () => selectSystemSubtab('adc6140'));

// --- Serial console section --------------------------------------
//
// Phase 1 of the UI rebuild: the always-on serial console moves from
// the permanent bottom slot into SETUP > Serial. The widget itself
// is still console_ (created at module scope so log() keeps working
// from anywhere); we just wrap it in a section element for the new
// home in setupWorkspace.
const serialSection = document.createElement('section');
serialSection.className = 'view view-serial';
serialSection.appendChild(console_.element);

// --- TUNE workspace stub -----------------------------------------
//
// Phase 1 lands TUNE as a workspace placeholder. Per planning/ui-
// rebuild/02-hierarchy.md, TUNE is the per-channel-detail surface
// (HPF / parametric EQ / dynamics / pan / sends matrix) for whichever
// channel is Sel'd. Most of those are blocked on firmware exposing
// per-channel processing — see 05-roadmap.md "Firmware dependencies".
// For now we render a "what this is" panel that displays the Sel'd
// channel name and a list of features-pending, so the workspace is
// reachable from the tab bar without dead-ending the user.
const tuneSection = document.createElement('section');
tuneSection.className = 'view view-tune';
const tuneInner = document.createElement('div');
tuneInner.className = 'tune-stub';

const tuneHeader = document.createElement('div');
tuneHeader.className = 'tune-stub-header';
const tuneSelLabel = document.createElement('div');
tuneSelLabel.className = 'tune-stub-sel';
const tuneTitle = document.createElement('h2');
tuneTitle.textContent = 'TUNE — selected channel detail';
tuneHeader.append(tuneTitle, tuneSelLabel);

const tuneNote = document.createElement('p');
tuneNote.className = 'tune-stub-note';
tuneNote.innerHTML =
  'This workspace will host per-channel HPF, parametric EQ, dynamics, pan, '
  + 'and a sends matrix for whichever channel is Sel’d. Most of those '
  + 'controls are blocked on firmware exposing per-channel processing — '
  + 'see <code>planning/ui-rebuild/05-roadmap.md</code> Phase 5.';

const tuneList = document.createElement('ul');
tuneList.className = 'tune-stub-list';
[
  ['HPF',         'pending firmware support'],
  ['Parametric EQ', 'pending firmware support'],
  ['Dynamics',    'pending firmware support'],
  ['Pan',         'addressable today; widget pending'],
  ['Sends matrix', 'partial — Main only until aux buses land'],
].forEach(([name, status]) => {
  const li = document.createElement('li');
  const n = document.createElement('strong');
  n.textContent = name + ': ';
  const s = document.createElement('span');
  s.textContent = status;
  li.append(n, s);
  tuneList.appendChild(li);
});

// Live binding to selectedChannel — readable hint that Sel state is
// global, even before TUNE has real widgets.
state.selectedChannel.subscribe((idx) => {
  const ch = state.channels[idx];
  const name = ch?.name.get() ?? `Ch ${idx + 1}`;
  tuneSelLabel.textContent = `Selected: ${name} (idx ${idx})`;
});
// Also update if the channel name itself changes (firmware rename echo).
for (let i = 0; i < state.channels.length; i++) {
  state.channels[i].name.subscribe(() => {
    if (state.selectedChannel.get() === i) {
      tuneSelLabel.textContent = `Selected: ${state.channels[i].name.get()} (idx ${i})`;
    }
  });
}

tuneInner.append(tuneHeader, tuneNote, tuneList);
tuneSection.appendChild(tuneInner);

// --- Workspace wrappers (Phase 1) --------------------------------
//
// Each top-level workspace is a section that holds:
//   1. its inner subnav (where applicable)
//   2. all of its sub-sections (existing section objects, reparented)
//
// Visibility is driven by applyVisibility() below, which reads the
// activeWorkspace and active*Tab signals and toggles display: none on
// each section. Hidden workspaces leave their state intact (canvas
// scrollback, sub-tab selections, etc.) — same as the previous
// display-toggle pattern.

// MIX workspace — single content (channel strips + main + bottom strip
// from Phase 0 sits below the workspace). No inner subnav.
const mixWorkspace = document.createElement('section');
mixWorkspace.className = 'workspace workspace-mix';
mixWorkspace.appendChild(mixerView);

// PLAY workspace — Synths / Arp / Beats / Loop. Order is musician-
// flow + signal-flow per planning/ui-rebuild/02-hierarchy.md.
const playSubnav = document.createElement('nav');
playSubnav.className = 'subnav';
const playSynthsTab = makeSubnavTab('Synths', 'synths');
const playArpTab    = makeSubnavTab('Arp',    'arp');
const playBeatsTab  = makeSubnavTab('Beats',  'beats');
const playLoopTab   = makeSubnavTab('Loop',   'loop');
playSubnav.append(playSynthsTab, playArpTab, playBeatsTab, playLoopTab);

const playWorkspace = document.createElement('section');
playWorkspace.className = 'workspace workspace-play';
playWorkspace.append(playSubnav, synthSection, arpSection, beatsSection, loopSection);

// FX workspace — Bus FX / Main Processing / Spectrum. All post-bus.
const fxSubnav = document.createElement('nav');
fxSubnav.className = 'subnav';
const fxBusFxTab      = makeSubnavTab('Bus FX',          'busfx');
const fxProcessingTab = makeSubnavTab('Main Processing', 'processing');
const fxSpectrumTab   = makeSubnavTab('Spectrum',        'spectrum');
fxSubnav.append(fxBusFxTab, fxProcessingTab, fxSpectrumTab);

const fxWorkspace = document.createElement('section');
fxWorkspace.className = 'workspace workspace-fx';
fxWorkspace.append(fxSubnav, fxSection, processingSection, spectrumSection);

// SETUP workspace — Codec / Clock / Raw OSC / Serial. Rarely-touched.
const setupSubnav = document.createElement('nav');
setupSubnav.className = 'subnav';
const setupCodecTab  = makeSubnavTab('Codec',   'codec');
const setupClockTab  = makeSubnavTab('Clock',   'clock');
const setupRawOscTab = makeSubnavTab('Raw OSC', 'rawosc');
const setupSerialTab = makeSubnavTab('Serial',  'serial');
setupSubnav.append(setupCodecTab, setupClockTab, setupRawOscTab, setupSerialTab);

const setupWorkspace = document.createElement('section');
setupWorkspace.className = 'workspace workspace-setup';
setupWorkspace.append(setupSubnav, systemSection, clockSection, rawOscSection, serialSection);

// TUNE workspace — single content for now.
const tuneWorkspace = document.createElement('section');
tuneWorkspace.className = 'workspace workspace-tune';
tuneWorkspace.appendChild(tuneSection);

// --- Persistent bottom dock --------------------------------------
//
// Phase 0 of the UI rebuild epic. Two variants share the bottom of
// every tab: the Engineer fader-bank (mini-strips per channel + main)
// and the Musician keyboard. The keyboard element is reparented
// between the Synth view dock and the bottom Musician dock when mode
// flips, so a single keyboard instance handles both layouts. See
// planning/ui-rebuild/02-hierarchy.md.
const bottomEngineerDock = document.createElement('section');
bottomEngineerDock.className = 'bottom-dock bottom-dock-engineer';
bottomEngineerDock.appendChild(bottomStrip(state, dispatcher));

const bottomMusicianDock = document.createElement('section');
bottomMusicianDock.className = 'bottom-dock bottom-dock-musician';
// Initially empty; keyboard is reparented in here when mode flips
// to musician. CSS hides the empty container in engineer mode.

const applyMode = (m: 'engineer' | 'musician'): void => {
  const isMus = m === 'musician';
  bottomEngineerDock.style.display = isMus ? 'none' : '';
  bottomMusicianDock.style.display = isMus ? '' : 'none';
  if (isMus) {
    if (keyboard.element.parentElement !== bottomMusicianDock) {
      bottomMusicianDock.appendChild(keyboard.element);
    }
    // Keyboard runs whenever Musician mode is active, regardless of
    // which tab is showing. MIDI subscription only fires when the
    // bridge is connected — handled by the state.connected listener
    // below for the late-connect case.
    if (!keyboard.isRunning()) keyboard.start();
    if (state.connected.get()) dispatcher.subscribeMidi();
  } else {
    if (keyboard.element.parentElement !== synthKeyboardDock) {
      synthKeyboardDock.appendChild(keyboard.element);
    }
    // Hand control back to applyVisibility's tab-driven lifecycle. If
    // we are not on PLAY > Synths and the keyboard is running, stop it.
    const onSynthsTab =
      state.activeWorkspace.get() === 'play' &&
      state.activePlayTab.get() === 'synths';
    if (!onSynthsTab && keyboard.isRunning()) {
      keyboard.stop();
      if (state.connected.get()) dispatcher.unsubscribeMidi();
    }
  }
};
state.mode.subscribe(applyMode);

// Re-subscribe MIDI on connect when in Musician mode — the boot-time
// applyMode runs before the bridge is connected, so the initial
// subscribeMidi() either no-ops or fails silently. This catches up
// once the bridge is live.
state.connected.subscribe((c) => {
  if (c && state.mode.get() === 'musician') dispatcher.subscribeMidi();
});

// --- Workspace + sub-tab visibility -------------------------------
//
// Single derive function that reads the four active-* signals and
// toggles display: none on every workspace and sub-section. Lifecycle
// hooks (spectrum.start/stop, keyboard, mpe.setActive) fire here too,
// gated on whether their owning workspace+subtab is now active.
//
// Hidden sections retain their internal state (canvas scrollback,
// form inputs, sub-sub-tab selections) — same display-toggle pattern
// as the pre-rebuild code.

function applyVisibility(): void {
  const ws       = state.activeWorkspace.get();
  const playTab  = state.activePlayTab.get();
  const fxTab    = state.activeFxTab.get();
  const setupTab = state.activeSetupTab.get();

  // Top-tab active class
  mixWsTab  .classList.toggle('active', ws === 'mix');
  playWsTab .classList.toggle('active', ws === 'play');
  tuneWsTab .classList.toggle('active', ws === 'tune');
  fxWsTab   .classList.toggle('active', ws === 'fx');
  setupWsTab.classList.toggle('active', ws === 'setup');

  // Workspace container visibility
  mixWorkspace  .style.display = ws === 'mix'   ? '' : 'none';
  playWorkspace .style.display = ws === 'play'  ? '' : 'none';
  tuneWorkspace .style.display = ws === 'tune'  ? '' : 'none';
  fxWorkspace   .style.display = ws === 'fx'    ? '' : 'none';
  setupWorkspace.style.display = ws === 'setup' ? '' : 'none';

  // PLAY inner subtab active class + section visibility
  playSynthsTab.classList.toggle('active', playTab === 'synths');
  playArpTab   .classList.toggle('active', playTab === 'arp');
  playBeatsTab .classList.toggle('active', playTab === 'beats');
  playLoopTab  .classList.toggle('active', playTab === 'loop');
  synthSection.style.display = playTab === 'synths' ? '' : 'none';
  arpSection  .style.display = playTab === 'arp'    ? '' : 'none';
  beatsSection.style.display = playTab === 'beats'  ? '' : 'none';
  loopSection .style.display = playTab === 'loop'   ? '' : 'none';

  // FX inner subtab active class + section visibility
  fxBusFxTab     .classList.toggle('active', fxTab === 'busfx');
  fxProcessingTab.classList.toggle('active', fxTab === 'processing');
  fxSpectrumTab  .classList.toggle('active', fxTab === 'spectrum');
  fxSection        .style.display = fxTab === 'busfx'      ? '' : 'none';
  processingSection.style.display = fxTab === 'processing' ? '' : 'none';
  spectrumSection  .style.display = fxTab === 'spectrum'   ? '' : 'none';

  // SETUP inner subtab active class + section visibility
  setupCodecTab .classList.toggle('active', setupTab === 'codec');
  setupClockTab .classList.toggle('active', setupTab === 'clock');
  setupRawOscTab.classList.toggle('active', setupTab === 'rawosc');
  setupSerialTab.classList.toggle('active', setupTab === 'serial');
  systemSection .style.display = setupTab === 'codec'  ? '' : 'none';
  clockSection  .style.display = setupTab === 'clock'  ? '' : 'none';
  rawOscSection .style.display = setupTab === 'rawosc' ? '' : 'none';
  serialSection .style.display = setupTab === 'serial' ? '' : 'none';

  // Spectrum-active body class — only when FX > Spectrum is the
  // current visible pane. Previously gated on ws === 'spectrum'; now
  // gated on workspace+subtab combo so the spectrum-fullscreen mode
  // only triggers when the user is actually looking at the analyzer.
  const spectrumActive = ws === 'fx' && fxTab === 'spectrum';
  document.body.classList.toggle('spectrum-active', spectrumActive);

  // Spectrum lifecycle: subscribe + start when FX > Spectrum is
  // visible; otherwise stop and unsubscribe so the firmware doesn't
  // compute FFTs nobody is looking at.
  if (!spectrumActive && spectrum.isRunning()) {
    spectrum.stop();
    dispatcher.unsubscribeSpectrum();
  } else if (spectrumActive && !spectrum.isRunning()) {
    dispatcher.subscribeSpectrum();
    spectrum.start();
  }

  // Keyboard / MPE lifecycle:
  //   - Engineer mode: keyboard runs only when PLAY > Synths is open.
  //   - Musician mode: keyboard always runs (handled by applyMode);
  //     don't stop it here even when the user navigates elsewhere.
  // MPE voice telemetry is tab-bound regardless of mode — it costs
  // bandwidth and only matters when the MPE sub-tab is visible.
  const synthsActive = ws === 'play' && playTab === 'synths';
  const mpeSubActive = synthsActive && mpeSubtab.classList.contains('active');
  const isMusician = state.mode.get() === 'musician';

  if (synthsActive && !keyboard.isRunning()) {
    dispatcher.subscribeMidi();
    keyboard.start();
  } else if (!synthsActive && keyboard.isRunning() && !isMusician) {
    keyboard.stop();
    dispatcher.unsubscribeMidi();
  }

  // MPE active hook independent of keyboard running — telemetry
  // subscribes/unsubscribes here.
  mpe.setActive(mpeSubActive);
}

// Wire the four active-* signals to the derive function. Each signal
// fires immediately on subscribe with its current value, so the very
// first applyVisibility call below sets up all visibility correctly
// on boot.
state.activeWorkspace.subscribe(applyVisibility);
state.activePlayTab  .subscribe(applyVisibility);
state.activeFxTab    .subscribe(applyVisibility);
state.activeSetupTab .subscribe(applyVisibility);

// Workspace tab clicks
mixWsTab  .addEventListener('click', () => state.activeWorkspace.set('mix'));
playWsTab .addEventListener('click', () => state.activeWorkspace.set('play'));
tuneWsTab .addEventListener('click', () => state.activeWorkspace.set('tune'));
fxWsTab   .addEventListener('click', () => state.activeWorkspace.set('fx'));
setupWsTab.addEventListener('click', () => state.activeWorkspace.set('setup'));

// Inner subtab clicks — also set the workspace in case the user
// somehow clicks an inner subnav button while it's hidden (shouldn't
// happen, but defensive).
playSynthsTab.addEventListener('click', () => { state.activePlayTab.set('synths'); state.activeWorkspace.set('play'); });
playArpTab   .addEventListener('click', () => { state.activePlayTab.set('arp');    state.activeWorkspace.set('play'); });
playBeatsTab .addEventListener('click', () => { state.activePlayTab.set('beats');  state.activeWorkspace.set('play'); });
playLoopTab  .addEventListener('click', () => { state.activePlayTab.set('loop');   state.activeWorkspace.set('play'); });

fxBusFxTab     .addEventListener('click', () => { state.activeFxTab.set('busfx');      state.activeWorkspace.set('fx'); });
fxProcessingTab.addEventListener('click', () => { state.activeFxTab.set('processing'); state.activeWorkspace.set('fx'); });
fxSpectrumTab  .addEventListener('click', () => { state.activeFxTab.set('spectrum');   state.activeWorkspace.set('fx'); });

setupCodecTab .addEventListener('click', () => { state.activeSetupTab.set('codec');  state.activeWorkspace.set('setup'); });
setupClockTab .addEventListener('click', () => { state.activeSetupTab.set('clock');  state.activeWorkspace.set('setup'); });
setupRawOscTab.addEventListener('click', () => { state.activeSetupTab.set('rawosc'); state.activeWorkspace.set('setup'); });
setupSerialTab.addEventListener('click', () => { state.activeSetupTab.set('serial'); state.activeWorkspace.set('setup'); });

// --- Workspace + tab persistence ---------------------------------
//
// Persist the four active-* signals to localStorage so reloads land
// back where the user left off. Mode-driven default landing only
// applies when no saved value exists (see below).

const WS_LS_KEY        = 't-dsp.ui.workspace';
const PLAY_TAB_LS_KEY  = 't-dsp.ui.playTab';
const FX_TAB_LS_KEY    = 't-dsp.ui.fxTab';
const SETUP_TAB_LS_KEY = 't-dsp.ui.setupTab';

const isWs = (s: unknown): s is Workspace =>
  s === 'mix' || s === 'play' || s === 'tune' || s === 'fx' || s === 'setup';

// Restore from localStorage. If nothing saved, derive from mode:
//   engineer → MIX, musician → PLAY.
const savedWs = localStorage.getItem(WS_LS_KEY);
if (isWs(savedWs)) {
  state.activeWorkspace.set(savedWs);
} else {
  state.activeWorkspace.set(state.mode.get() === 'musician' ? 'play' : 'mix');
}

const savedPlayTab = localStorage.getItem(PLAY_TAB_LS_KEY);
if (savedPlayTab === 'synths' || savedPlayTab === 'arp' || savedPlayTab === 'beats' || savedPlayTab === 'loop') {
  state.activePlayTab.set(savedPlayTab);
}
const savedFxTab = localStorage.getItem(FX_TAB_LS_KEY);
if (savedFxTab === 'busfx' || savedFxTab === 'processing' || savedFxTab === 'spectrum') {
  state.activeFxTab.set(savedFxTab);
}
const savedSetupTab = localStorage.getItem(SETUP_TAB_LS_KEY);
if (savedSetupTab === 'codec' || savedSetupTab === 'clock' || savedSetupTab === 'rawosc' || savedSetupTab === 'serial') {
  state.activeSetupTab.set(savedSetupTab);
}

// Write-back on change. Wrapped in try/catch in case localStorage is
// unavailable (private mode etc).
state.activeWorkspace.subscribe((w) => { try { localStorage.setItem(WS_LS_KEY,        w); } catch { /* ignore */ } });
state.activePlayTab  .subscribe((t) => { try { localStorage.setItem(PLAY_TAB_LS_KEY,  t); } catch { /* ignore */ } });
state.activeFxTab    .subscribe((t) => { try { localStorage.setItem(FX_TAB_LS_KEY,    t); } catch { /* ignore */ } });
state.activeSetupTab .subscribe((t) => { try { localStorage.setItem(SETUP_TAB_LS_KEY, t); } catch { /* ignore */ } });

// --- Final layout ------------------------------------------------
//
// Workspace wrappers are appended in the same order as their tabs in
// viewTabs. Bottom dock comes last so it's always at the visible
// bottom. The serial console is now inside setupWorkspace via
// serialSection — no permanent bottom slot.
app.append(
  header,
  viewTabs,
  mixWorkspace,
  playWorkspace,
  tuneWorkspace,
  fxWorkspace,
  setupWorkspace,
  bottomEngineerDock,
  bottomMusicianDock,
);

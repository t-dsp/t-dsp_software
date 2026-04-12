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

// Channel count for the small mixer v1 — 6 channels (USB L/R, Line L/R,
// Mic L/R) matching tdsp::kChannelCount in lib/TDspMixer/src/MixerModel.h.
// Stereo-linked pairs by default: (1,2), (3,4), (5,6).
const CHANNEL_COUNT = 6;

const state = createMixerState(CHANNEL_COUNT);
const console_ = serialConsole({ onSubmit: (line) => sendText(line) });
const log = (line: string): void => console_.append(line);

let port: SerialPort | null = null;
let writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
let readLoopAborted = false;

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
  if (!writer) {
    log('! not connected');
    return;
  }
  writer.write(slipEncode(packet)).catch((e) => log(`! write error: ${e.message}`));
};

// Plain-text CLI write. Intentionally bypasses SLIP: the firmware's
// SlipOscTransport demuxes on the first byte of each chunk — 0xC0
// starts a SLIP/OSC frame, anything else accumulates into a text-
// line buffer that dispatches on CR/LF. ASCII never starts with 0xC0
// so the two streams coexist on the same serial pipe.
const sendText = (line: string): void => {
  if (!writer) {
    log('! not connected');
    return;
  }
  writer.write(new TextEncoder().encode(line + '\n'))
    .catch((e) => log(`! write error: ${e.message}`));
};

const dispatcher = new Dispatcher(state, send);

async function connect(): Promise<void> {
  try {
    if (!('serial' in navigator)) {
      log('! WebSerial not supported in this browser. Use Chrome, Edge, Brave, or another Chromium-based browser.');
      return;
    }
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    if (!port.writable || !port.readable) {
      throw new Error('port has no readable/writable streams');
    }
    writer = port.writable.getWriter();
    reader = port.readable.getReader();
    state.connected.set(true);
    log('-- connected --');
    void readLoop();
    // Ask the firmware for a full state dump so the UI populates with
    // the current values instead of whatever the signals were initialized
    // to. The small delay lets the serial port finish settling and
    // the firmware's own boot chatter (if any) drain into our reader
    // before our first write goes out.
    setTimeout(() => dispatcher.requestSnapshot(), 150);
  } catch (e) {
    log(`! connect failed: ${(e as Error).message}`);
  }
}

async function readLoop(): Promise<void> {
  readLoopAborted = false;
  try {
    while (!readLoopAborted && reader) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) demuxer.feed(value);
    }
  } catch (e) {
    log(`! read error: ${(e as Error).message}`);
  }
}

async function disconnect(): Promise<void> {
  readLoopAborted = true;
  try {
    if (reader) {
      await reader.cancel().catch(() => {});
      reader.releaseLock();
    }
    if (writer) {
      writer.releaseLock();
    }
    if (port) {
      await port.close();
    }
  } catch (e) {
    log(`! disconnect error: ${(e as Error).message}`);
  } finally {
    reader = null;
    writer = null;
    port = null;
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
viewTabs.append(mixerTab, spectrumTab);

// --- Mixer view section (wraps existing mixer content) ------------

const mixerView = document.createElement('section');
mixerView.className = 'view view-mixer';

const mixerRow = document.createElement('section');
mixerRow.className = 'mixer-row';
// 3 stereo pairs (1/2, 3/4, 5/6) on the left, then MAIN + HOST docked
// to the right edge via `margin-left: auto`. Every wrapper uses the
// shared 7-row layout so buttons and sliders align across the mixer.
for (let oddIdx = 0; oddIdx < CHANNEL_COUNT; oddIdx += 2) {
  mixerRow.appendChild(channelPair(oddIdx, state, dispatcher));
}
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

// --- Tab switching -------------------------------------------------

function selectView(name: 'mixer' | 'spectrum'): void {
  const onMixer = name === 'mixer';
  mixerTab.classList.toggle('active', onMixer);
  spectrumTab.classList.toggle('active', !onMixer);
  mixerView.style.display    = onMixer ? '' : 'none';
  spectrumSection.style.display = onMixer ? 'none' : '';

  // Toggle the body-level class that makes #app break out of its
  // 1200px max-width and go full viewport in spectrum mode. The CSS
  // rules in body.spectrum-active do the layout work; we just flip
  // the class. spectrum.start() below calls resize() which reads the
  // new layout via getBoundingClientRect, so the canvas picks up the
  // bigger dimensions on its first frame.
  document.body.classList.toggle('spectrum-active', !onMixer);

  if (onMixer) {
    // Leaving the spectrum view — stop the render loop AND
    // unsubscribe so the firmware stops computing FFTs nobody
    // will see. Same pattern as the Meters ON/OFF toggle.
    spectrum.stop();
    dispatcher.unsubscribeSpectrum();
  } else {
    // Entering the spectrum view — start render loop and sub.
    dispatcher.subscribeSpectrum();
    spectrum.start();
  }
}
mixerTab.addEventListener('click', () => selectView('mixer'));
spectrumTab.addEventListener('click', () => selectView('spectrum'));

app.append(header, viewTabs, mixerView, spectrumSection, console_.element);

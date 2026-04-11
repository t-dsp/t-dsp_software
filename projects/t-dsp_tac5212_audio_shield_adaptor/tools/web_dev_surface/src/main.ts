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
import { channelStrip } from './ui/channel-strip';
import { mainBus } from './ui/main-bus';
import { codecPanel } from './ui/codec-panel';
import { serialConsole } from './ui/serial-console';
import { rawOsc } from './ui/raw-osc';

// Channel count for the small mixer v1 — 6 channels (USB L/R, Line L/R,
// Mic L/R) matching tdsp::kChannelCount in lib/TDspMixer/src/MixerModel.h.
// Stereo-linked pairs by default: (1,2), (3,4), (5,6).
const CHANNEL_COUNT = 6;

const state = createMixerState(CHANNEL_COUNT);
const console_ = serialConsole();
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
  '/meters/gr',
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

const mixerRow = document.createElement('section');
mixerRow.className = 'mixer-row';
state.channels.forEach((ch, i) => mixerRow.appendChild(channelStrip(i, ch, dispatcher)));
mixerRow.appendChild(mainBus(state.main, dispatcher));

const codecSection = document.createElement('section');
codecSection.className = 'codec-section';
codecSection.appendChild(codecPanel(tac5212Panel, dispatcher));

const rawSection = document.createElement('section');
rawSection.className = 'raw-section';
const rawLabel = document.createElement('h4');
rawLabel.textContent = 'Raw OSC';
rawSection.append(rawLabel, rawOsc(dispatcher, log));

app.append(header, mixerRow, codecSection, rawSection, console_.element);

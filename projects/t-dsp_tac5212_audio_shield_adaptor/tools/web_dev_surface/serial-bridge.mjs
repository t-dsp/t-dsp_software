// serial-bridge.mjs — Node.js WebSocket-to-serial bridge.
//
// Opens the Teensy's COM port via the `serialport` package (which does
// NOT disrupt USB Audio) and relays raw bytes to/from a local WebSocket
// so the browser never touches WebSerial.
//
// Supports multiple WebSocket clients — serial data is fanned out to
// all connected clients, and any client can send commands to the
// firmware. When the last client disconnects, the bridge sends
// "unsub_all" to the firmware so it stops computing meter/spectrum
// data that nobody is watching.
//
// Auto-reconnects when the device is disconnected or rebooted — polls
// SerialPort.list() (host-side only, no USB transfers) until the COM
// port reappears, then re-opens the serial connection.
//
// Usage:  node serial-bridge.mjs [COM_PORT] [BAUD] [WS_PORT]
//         node serial-bridge.mjs              — auto-detect Teensy by VID/PID
//         node serial-bridge.mjs COM4 115200 8765

import { SerialPort } from 'serialport';
import { WebSocketServer } from 'ws';

// Teensy 4.x USB VID + known PIDs for composite modes
const TEENSY_VID = '16C0';
const TEENSY_PIDS = [
  '0483',  // USB Serial
  '0489',  // USB MIDI + Audio + Serial (USB_MIDI_AUDIO_SERIAL)
  '048A',  // USB Audio + Serial
  '048B',  // USB MIDI + Serial
];

const comPort  = process.argv[2] || null;   // null = auto-detect
const baud     = parseInt(process.argv[3] || '115200', 10);
const wsPort   = parseInt(process.argv[4] || '8765', 10);

const POLL_INTERVAL_MS = 2000;
// Minimum gap between serial writes (ms). Prevents CDC bulk OUT bursts
// from disrupting USB Audio isochronous transfers on the Teensy's shared
// USB controller. 100ms = max 10 writes/sec — reliable with Audio,
// responsive enough for faders, meters can lag.
const TX_GAP_MS = 20;

let serial = null;
let reconnecting = false;
const clients = new Set();

// --- Throttled serial write queue ---
const txQueue = [];
let txTimer = null;
let lastTxMs = 0;

function enqueueWrite(data) {
  txQueue.push(data);
  drainQueue();
}

function drainQueue() {
  if (txTimer) return;  // already scheduled
  if (txQueue.length === 0) return;
  if (!serial || !serial.isOpen) { txQueue.length = 0; return; }

  const now = Date.now();
  const elapsed = now - lastTxMs;
  if (elapsed >= TX_GAP_MS) {
    serial.write(txQueue.shift());
    lastTxMs = Date.now();
    if (txQueue.length > 0) {
      txTimer = setTimeout(() => { txTimer = null; drainQueue(); }, TX_GAP_MS);
    }
  } else {
    txTimer = setTimeout(() => { txTimer = null; drainQueue(); }, TX_GAP_MS - elapsed);
  }
}

// --- Serial lifecycle ---

// Resolved port path — set by auto-detect or CLI arg
let resolvedPort = comPort;

function openSerial() {
  // Use the same constructor-auto-open pattern as the original working
  // bridge. autoOpen:true is the default and matches the timing/DTR
  // behavior that the Teensy expects.
  const port = new SerialPort({
    path: resolvedPort,
    baudRate: baud,
    // Do NOT assert DTR/RTS on open — sending SET_CONTROL_LINE_STATE
    // to the Teensy's CDC endpoint disrupts USB Audio isochronous
    // transfers on the same composite device, causing audio buzz.
    hupcl: false,
    rtscts: false,
  });

  port.on('open', () => {
    console.log(`serial: ${resolvedPort} @ ${baud}`);
    serial = port;
    reconnecting = false;
  });

  port.on('error', (err) => {
    console.error(`serial error: ${err.message}`);
    // If we never got to 'open', this is a failed connect attempt.
    if (!serial || serial !== port) {
      pollForPort();
    }
  });

  port.on('close', () => {
    console.log('serial: port closed — will poll for reconnect...');
    if (serial === port) serial = null;
    if (!reconnecting) pollForPort();
  });

  // Serial -> all WebSocket clients (fan-out)
  port.on('data', (buf) => {
    for (const ws of clients) {
      if (ws.readyState === 1) {
        ws.send(buf);
      }
    }
  });
}

// Find a Teensy port from the enumerated list.
// If the user gave an explicit COM port, match by path; otherwise match
// by VID/PID so the bridge works regardless of which COM number Windows
// assigns.
function findTeensyPort(ports) {
  if (comPort) {
    // Explicit port requested — match by path
    return ports.find(
      (p) => p.path.toLowerCase() === comPort.toLowerCase()
    );
  }
  // Auto-detect by Teensy VID + any known PID
  return ports.find(
    (p) =>
      p.vendorId &&
      p.vendorId.toLowerCase() === TEENSY_VID.toLowerCase() &&
      p.productId &&
      TEENSY_PIDS.some((pid) => pid.toLowerCase() === p.productId.toLowerCase())
  );
}

// Poll SerialPort.list() until the target device is enumerated.
// list() is a host-side OS query — it does NOT send any USB control
// transfers to the device, so it can't disrupt Audio isochronous.
function pollForPort() {
  reconnecting = true;
  const label = comPort || `Teensy (VID ${TEENSY_VID})`;
  console.log(`serial: scanning for ${label}...`);
  const check = async () => {
    try {
      const ports = await SerialPort.list();
      const match = findTeensyPort(ports);
      if (match) {
        resolvedPort = match.path;
        console.log(`serial: ${resolvedPort} detected — opening...`);
        openSerial();
        return;
      }
    } catch (err) {
      console.error(`serial: list error — ${err.message}`);
    }
    setTimeout(check, POLL_INTERVAL_MS);
  };
  setTimeout(check, POLL_INTERVAL_MS);
}

// --- WebSocket server (stays up across serial reconnects) ---

const wss = new WebSocketServer({ port: wsPort });

wss.on('connection', (ws) => {
  clients.add(ws);
  console.log(`ws: client connected (${clients.size} total)`);

  ws.on('message', (data) => {
    enqueueWrite(data);
  });

  ws.on('close', () => {
    clients.delete(ws);
    console.log(`ws: client disconnected (${clients.size} remaining)`);
    if (clients.size === 0) {
      console.log('ws: no clients — sending unsub_all');
      enqueueWrite('unsub_all\n');
    }
  });

  ws.on('error', (err) => {
    console.error(`ws error: ${err.message}`);
    clients.delete(ws);
  });
});

wss.on('listening', () => {
  console.log(`ws: listening on ws://localhost:${wsPort}`);
});

// Clean shutdown
process.on('SIGINT', () => {
  console.log('\nshutting down...');
  if (serial && serial.isOpen) serial.close();
  wss.close();
  process.exit(0);
});

// Start — scan for the device first if no explicit port was given
async function start() {
  if (comPort) {
    resolvedPort = comPort;
    openSerial();
  } else {
    // Try to detect immediately before falling back to polling
    try {
      const ports = await SerialPort.list();
      if (ports.length) {
        console.log('serial: available ports:');
        for (const p of ports) {
          console.log(`  ${p.path}  vid=${p.vendorId || '?'}  pid=${p.productId || '?'}  ${p.manufacturer || ''}`);
        }
      } else {
        console.log('serial: no ports found');
      }
      const match = findTeensyPort(ports);
      if (match) {
        resolvedPort = match.path;
        console.log(`serial: auto-detected ${resolvedPort}`);
        openSerial();
        return;
      }
    } catch (_) { /* fall through to poll */ }
    pollForPort();
  }
}
start();

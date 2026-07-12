// T-DSP mixer agent — laptop side.
//
// Same job as serial-bridge.mjs, but instead of LISTENING on a local
// WebSocket it DIALS OUT to the cloud relay. Outbound-only: nothing is
// opened on the laptop, so there's no firewall rule and no NAT config —
// which is exactly the property the ESP32 will need when it eventually
// takes over this role and talks to the same /agent endpoint.
//
// serial bytes  <-->  wss://<relay>/agent  (raw, never parsed here)
//
// The TX throttle stays HERE (on the serial side), because it exists to
// protect the Teensy's USB Audio from CDC write bursts — the relay is
// dumb and must not need to know about it.
//
// Env / args:
//   RELAY_URL     wss://your-app.up.railway.app   (REQUIRED)
//   DEVICE_TOKEN  shared secret matching the relay (REQUIRED)
//   DEVICE_ID     logical id (default "mixer-1")
//   COM_PORT      explicit port (default: auto-detect Teensy by VID/PID)
//   BAUD          default 115200

import { WebSocket } from 'ws';

// serialport is an optional dep so the relay can deploy without native
// builds; the agent needs it. Fail with a clear message if it's missing.
let SerialPort;
try {
  ({ SerialPort } = await import('serialport'));
} catch {
  console.error('agent: `serialport` is not installed. Run: pnpm add serialport');
  process.exit(1);
}

const RELAY_URL = process.env.RELAY_URL || process.argv[2];
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || process.argv[3];
const DEVICE_ID = process.env.DEVICE_ID || 'mixer-1';
const comPort = process.env.COM_PORT || null;
const baud = parseInt(process.env.BAUD || '115200', 10);

if (!RELAY_URL || !DEVICE_TOKEN) {
  console.error('agent: set RELAY_URL and DEVICE_TOKEN (env or argv).');
  console.error('  e.g. RELAY_URL=wss://app.up.railway.app DEVICE_TOKEN=xxx node agent.mjs');
  process.exit(1);
}

const TEENSY_VID = '16C0';
const TEENSY_PIDS = ['0483', '0489', '048A', '048B'];
const POLL_INTERVAL_MS = 2000;
const TX_GAP_MS = 20; // see serial-bridge.mjs — protects USB Audio isoch

let serial = null;
let resolvedPort = comPort;
let cloud = null;
let cloudReconnectMs = 1000;

// --- throttled serial write queue (cloud -> serial) ---
const txQueue = [];
let txTimer = null;
let lastTxMs = 0;
function enqueueWrite(data) { txQueue.push(data); drainQueue(); }
function drainQueue() {
  if (txTimer) return;
  if (txQueue.length === 0) return;
  if (!serial || !serial.isOpen) { txQueue.length = 0; return; }
  const elapsed = Date.now() - lastTxMs;
  if (elapsed >= TX_GAP_MS) {
    serial.write(txQueue.shift());
    lastTxMs = Date.now();
    if (txQueue.length > 0) txTimer = setTimeout(() => { txTimer = null; drainQueue(); }, TX_GAP_MS);
  } else {
    txTimer = setTimeout(() => { txTimer = null; drainQueue(); }, TX_GAP_MS - elapsed);
  }
}

// --- cloud link (outbound) ---
function connectCloud() {
  const url = `${RELAY_URL.replace(/\/$/, '')}/agent?deviceId=${encodeURIComponent(DEVICE_ID)}`;
  console.log(`cloud: connecting to ${url}`);
  const ws = new WebSocket(url, { headers: { authorization: `Bearer ${DEVICE_TOKEN}` } });

  ws.on('open', () => {
    cloud = ws;
    cloudReconnectMs = 1000;
    console.log('cloud: connected — relaying serial <-> cloud');
  });

  // cloud -> serial (raw bytes, throttled). Includes the relay's
  // "unsub_all\n" when the last viewer leaves.
  ws.on('message', (data) => {
    enqueueWrite(data);
  });

  ws.on('close', () => {
    if (cloud === ws) cloud = null;
    console.log(`cloud: disconnected — retrying in ${cloudReconnectMs}ms`);
    setTimeout(connectCloud, cloudReconnectMs);
    cloudReconnectMs = Math.min(cloudReconnectMs * 2, 15000); // backoff
  });

  ws.on('error', (err) => {
    console.error(`cloud: error — ${err.message}`);
    // 'close' fires after 'error'; reconnect is scheduled there.
  });
}

// --- serial link (same approach as serial-bridge.mjs) ---
function openSerial() {
  const port = new SerialPort({ path: resolvedPort, baudRate: baud, hupcl: false, rtscts: false });
  port.on('open', () => { console.log(`serial: ${resolvedPort} @ ${baud}`); serial = port; });
  port.on('error', (err) => {
    console.error(`serial error: ${err.message}`);
    if (!serial || serial !== port) pollForPort();
  });
  port.on('close', () => {
    console.log('serial: closed — polling for reconnect...');
    if (serial === port) serial = null;
    pollForPort();
  });
  // serial -> cloud (raw bytes)
  port.on('data', (buf) => {
    if (cloud && cloud.readyState === 1) cloud.send(buf, { binary: true });
  });
}

function findTeensyPort(ports) {
  if (comPort) return ports.find((p) => p.path.toLowerCase() === comPort.toLowerCase());
  return ports.find((p) =>
    p.vendorId && p.vendorId.toLowerCase() === TEENSY_VID.toLowerCase() &&
    p.productId && TEENSY_PIDS.some((pid) => pid.toLowerCase() === p.productId.toLowerCase()));
}

function pollForPort() {
  const label = comPort || `Teensy (VID ${TEENSY_VID})`;
  console.log(`serial: scanning for ${label}...`);
  const check = async () => {
    try {
      const ports = await SerialPort.list();
      const match = findTeensyPort(ports);
      if (match) { resolvedPort = match.path; console.log(`serial: ${resolvedPort} detected — opening...`); openSerial(); return; }
    } catch (err) { console.error(`serial: list error — ${err.message}`); }
    setTimeout(check, POLL_INTERVAL_MS);
  };
  setTimeout(check, POLL_INTERVAL_MS);
}

process.on('SIGINT', () => {
  console.log('\nshutting down...');
  if (serial && serial.isOpen) serial.close();
  if (cloud) cloud.close();
  process.exit(0);
});

async function start() {
  connectCloud();
  if (comPort) { resolvedPort = comPort; openSerial(); return; }
  try {
    const ports = await SerialPort.list();
    const match = findTeensyPort(ports);
    if (match) { resolvedPort = match.path; console.log(`serial: auto-detected ${resolvedPort}`); openSerial(); return; }
  } catch { /* fall through */ }
  pollForPort();
}
start();

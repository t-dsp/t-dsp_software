// T-DSP Remote — minimal cloud control client.
//
// Speaks the SAME wire protocol as the desktop surface: OSC 1.0 messages,
// SLIP-framed, sent as binary over the WebSocket. The relay forwards these
// bytes verbatim to the agent, which writes them to serial. Ported from
// web_dev/src/osc.ts + transport.ts (kept tiny and dependency-free).

// ---------- OSC encode ----------
const enc = new TextEncoder();
const pad4 = (n) => (4 - (n % 4)) % 4;

function encStr(s) {
  const sb = enc.encode(s);
  const out = new Uint8Array(sb.length + 1 + pad4(sb.length + 1));
  out.set(sb, 0);
  return out;
}
function encInt(v) { const o = new Uint8Array(4); new DataView(o.buffer).setInt32(0, v | 0, false); return o; }
function encFloat(v) { const o = new Uint8Array(4); new DataView(o.buffer).setFloat32(0, v, false); return o; }
function concat(arrs) {
  let n = 0; for (const a of arrs) n += a.length;
  const out = new Uint8Array(n); let o = 0;
  for (const a of arrs) { out.set(a, o); o += a.length; }
  return out;
}
function encodeMessage(address, types, args) {
  const parts = [encStr(address), encStr(',' + types)];
  let i = 0;
  for (const t of types) {
    if (t === 'i') parts.push(encInt(args[i++]));
    else if (t === 'f') parts.push(encFloat(args[i++]));
    else if (t === 's') parts.push(encStr(String(args[i++])));
    else throw new Error('unsupported type ' + t);
  }
  return concat(parts);
}

// ---------- SLIP ----------
const SLIP_END = 0xc0, SLIP_ESC = 0xdb, SLIP_ESC_END = 0xdc, SLIP_ESC_ESC = 0xdd;
function slipEncode(payload) {
  const out = [SLIP_END];
  for (const b of payload) {
    if (b === SLIP_END) out.push(SLIP_ESC, SLIP_ESC_END);
    else if (b === SLIP_ESC) out.push(SLIP_ESC, SLIP_ESC_ESC);
    else out.push(b);
  }
  out.push(SLIP_END);
  return new Uint8Array(out);
}

// ---------- incoming demux (just enough to show life) ----------
const dec = new TextDecoder('utf-8', { fatal: false });
function makeDemuxer(onFrame, onText) {
  let state = 'IDLE', frame = [], text = [];
  const flush = () => { if (text.length) { onText(dec.decode(new Uint8Array(text))); text = []; } };
  return (bytes) => {
    for (const b of bytes) {
      if (state === 'IDLE') {
        if (b === SLIP_END) { flush(); state = 'FRAME'; frame = []; }
        else if (b === 0x0a) flush();
        else if (b !== 0x0d) text.push(b);
      } else if (state === 'FRAME') {
        if (b === SLIP_END) { if (frame.length) onFrame(new Uint8Array(frame)); frame = []; state = 'IDLE'; }
        else if (b === SLIP_ESC) state = 'ESC';
        else frame.push(b);
      } else { // ESC
        frame.push(b === SLIP_ESC_END ? SLIP_END : b === SLIP_ESC_ESC ? SLIP_ESC : b);
        state = 'FRAME';
      }
    }
  };
}
function oscAddress(frame) {
  let end = 0; while (end < frame.length && frame[end] !== 0) end++;
  return dec.decode(frame.subarray(0, end));
}

// ---------- DOM helpers ----------
const $ = (id) => document.getElementById(id);
const loginEl = $('login'), surfaceEl = $('surface');
let logSeen = 0;
function log(line) {
  const el = $('log');
  el.textContent += (logSeen++ ? '\n' : '') + line;
  el.scrollTop = el.scrollHeight;
  if (logSeen > 200) { el.textContent = el.textContent.split('\n').slice(-150).join('\n'); }
}

// ---------- session / auth ----------
async function checkAuth() {
  const r = await fetch('/api/me');
  return r.ok;
}
async function doLogin() {
  $('login-err').textContent = '';
  const r = await fetch('/api/login', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ user: $('user').value, pass: $('pass').value }),
  });
  if (!r.ok) { $('login-err').textContent = 'Invalid credentials'; return; }
  show(true);
}
async function doLogout() { await fetch('/api/logout', { method: 'POST' }); location.reload(); }

function show(authed) {
  loginEl.classList.toggle('hidden', authed);
  surfaceEl.classList.toggle('hidden', !authed);
  if (authed) connect();
}

// ---------- websocket transport ----------
let ws = null;
function setLink(state, text) {
  $('link-dot').className = 'dot' + (state === 'ok' ? ' on' : state === 'warn' ? ' warn' : '');
  $('link-text').textContent = text;
}
function setDevice(online) {
  $('dev-dot').className = 'dot' + (online ? ' on' : '');
  $('dev-text').textContent = online ? 'device online' : 'device offline';
}
const demux = makeDemuxer(
  (frame) => { try { log('← ' + oscAddress(frame)); } catch {} },
  (line) => log('· ' + line.slice(0, 120)),
);

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/client`);
  ws.binaryType = 'arraybuffer';
  setLink('warn', 'connecting…');
  ws.onopen = () => { setLink('ok', 'connected'); sendSnapshot(); };
  ws.onclose = () => { setLink('', 'disconnected — retrying'); setDevice(false); setTimeout(connect, 1500); };
  ws.onerror = () => setLink('', 'error');
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      try { const m = JSON.parse(e.data); if (m.type === 'presence') setDevice(m.online); } catch {}
      return;
    }
    demux(new Uint8Array(e.data));
  };
}

function sendOsc(address, types = '', args = []) {
  if (!ws || ws.readyState !== 1) return;
  ws.send(slipEncode(encodeMessage(address, types, args)));
}
function sendSnapshot() { sendOsc('/snapshot'); }

// Coalesce fader drags to ~30 Hz, like the desktop dispatcher.
function throttled(address) {
  let pending = null, timer = null;
  const FLUSH = 33;
  return (v) => {
    pending = v;
    if (timer) return;
    sendOsc(address, 'f', [pending]); pending = null;
    timer = setTimeout(() => { timer = null; if (pending !== null) sendOsc(address, 'f', [pending]); pending = null; }, FLUSH);
  };
}

// ---------- control wiring ----------
function wireFader(rangeId, valId, sender) {
  const r = $(rangeId), out = $(valId);
  r.addEventListener('input', () => { out.textContent = (+r.value).toFixed(2); sender(+r.value); });
}
function wireToggle(btnId, address, onLabel) {
  const b = $(btnId); let on = true;
  b.addEventListener('click', () => {
    on = !on;
    b.classList.toggle('on', on);
    sendOsc(address, 'i', [on ? 1 : 0]);
  });
}

const mainFader = throttled('/main/st/mix/faderL');
wireFader('main-fader', 'main-fader-val', (v) => { mainFader(v); sendOsc('/main/st/mix/faderR', 'f', [v]); });
wireToggle('main-on', '/main/st/mix/on');
wireFader('ch1-fader', 'ch1-fader-val', throttled('/ch/01/mix/fader'));
wireToggle('ch1-on', '/ch/01/mix/on');
$('snapshot').addEventListener('click', sendSnapshot);

$('raw-send').addEventListener('click', () => {
  const addr = $('raw-addr').value.trim();
  if (!addr) return;
  const t = $('raw-type').value;
  if (!t) return sendOsc(addr);
  const raw = $('raw-val').value;
  const arg = t === 'i' ? parseInt(raw, 10) : t === 'f' ? parseFloat(raw) : raw;
  sendOsc(addr, t, [arg]);
  log('→ ' + addr + ' ' + t + ' ' + raw);
});

$('login-btn').addEventListener('click', doLogin);
$('pass').addEventListener('keydown', (e) => { if (e.key === 'Enter') doLogin(); });
$('logout').addEventListener('click', (e) => { e.preventDefault(); doLogout(); });

// ---------- boot ----------
checkAuth().then(show);

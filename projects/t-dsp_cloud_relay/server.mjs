// T-DSP cloud relay — prototype.
//
// A dumb, authenticated byte-relay between a mixer's "agent" (today the
// laptop running the serial bridge; tomorrow an on-board ESP32) and one or
// more browser "clients" (your phone). The relay NEVER parses the audio
// protocol — it forwards raw SLIP/OSC bytes both directions. That's what
// keeps it protocol-agnostic and lets the ESP32 drop into the agent role
// unchanged later.
//
// Topology (one device for the prototype, but keyed by id so N is a Map):
//
//   phone (wss /client, cookie auth) ─┐
//                                      ├─► relay ◄─── laptop agent (wss /agent, token auth) ─► serial ─► Teensy
//   desktop (wss /client) ────────────┘
//
// Auth is deliberately minimal (single user from env, HMAC session cookie)
// because this endpoint is PUBLIC. It is NOT a real identity system — see
// auth.mjs. Swap for real accounts before charging anyone.
//
// Env:
//   PORT            listen port (Railway sets this)
//   AUTH_USER       login username           (default "admin")
//   AUTH_PASS       login password           (REQUIRED in prod)
//   SESSION_SECRET  HMAC secret for cookies   (REQUIRED in prod)
//   DEVICE_TOKEN    shared secret the agent presents (REQUIRED in prod)
//   DEVICE_ID       logical device id         (default "mixer-1")

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { sign, verify, safeEqual } from './auth.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const PORT = parseInt(process.env.PORT || '8080', 10);
const AUTH_USER = process.env.AUTH_USER || 'admin';
const AUTH_PASS = process.env.AUTH_PASS || 'admin';
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'dev-device-token';
const DEVICE_ID = process.env.DEVICE_ID || 'mixer-1';
const SESSION_TTL = 60 * 60 * 24 * 30; // 30 days

if (!process.env.AUTH_PASS || !process.env.SESSION_SECRET || !process.env.DEVICE_TOKEN) {
  console.warn(
    'WARNING: running with insecure default secrets. Set AUTH_PASS, ' +
    'SESSION_SECRET, and DEVICE_TOKEN env vars before exposing this.',
  );
}

const now = () => Math.floor(Date.now() / 1000);

// ---------- device registry (one entry today, Map for scale) ----------

/** @type {Map<string, { agent: import('ws').WebSocket|null, clients: Set<import('ws').WebSocket> }>} */
const devices = new Map();
function deviceSlot(id) {
  let d = devices.get(id);
  if (!d) {
    d = { agent: null, clients: new Set() };
    devices.set(id, d);
  }
  return d;
}

function broadcastPresence(id) {
  const d = devices.get(id);
  if (!d) return;
  const msg = JSON.stringify({ type: 'presence', deviceId: id, online: !!d.agent });
  for (const c of d.clients) if (c.readyState === 1) c.send(msg);
}

// ---------- HTTP: auth API + static client ----------

function parseCookies(req) {
  const out = {};
  const raw = req.headers.cookie;
  if (!raw) return out;
  for (const part of raw.split(';')) {
    const i = part.indexOf('=');
    if (i < 0) continue;
    out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
  }
  return out;
}

function sessionFromReq(req) {
  const sid = parseCookies(req).sid;
  if (!sid) return null;
  return verify(sid, now());
}

function readBody(req) {
  return new Promise((resolve) => {
    let buf = '';
    req.on('data', (c) => {
      buf += c;
      if (buf.length > 1e6) req.destroy(); // basic flood guard
    });
    req.on('end', () => resolve(buf));
  });
}

function send(res, status, body, headers = {}) {
  res.writeHead(status, { 'content-type': 'application/json', ...headers });
  res.end(typeof body === 'string' ? body : JSON.stringify(body));
}

const STATIC_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.webmanifest': 'application/manifest+json',
};

function serveStatic(req, res) {
  let rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (rel === '/') rel = '/index.html';
  // Prevent path traversal — resolve under public/ and verify.
  const filePath = path.join(__dirname, 'public', rel);
  if (!filePath.startsWith(path.join(__dirname, 'public'))) {
    return send(res, 403, { error: 'forbidden' });
  }
  fs.readFile(filePath, (err, data) => {
    if (err) return send(res, 404, { error: 'not found' });
    const type = STATIC_TYPES[path.extname(filePath)] || 'application/octet-stream';
    res.writeHead(200, { 'content-type': type });
    res.end(data);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');

  if (req.method === 'POST' && url.pathname === '/api/login') {
    const body = await readBody(req);
    let creds;
    try { creds = JSON.parse(body); } catch { return send(res, 400, { error: 'bad json' }); }
    const ok = safeEqual(creds.user || '', AUTH_USER) && safeEqual(creds.pass || '', AUTH_PASS);
    if (!ok) return send(res, 401, { error: 'invalid credentials' });
    const token = sign({ user: AUTH_USER, exp: now() + SESSION_TTL });
    const secure = req.headers['x-forwarded-proto'] === 'https' ? ' Secure;' : '';
    res.setHeader('Set-Cookie',
      `sid=${token}; HttpOnly;${secure} SameSite=Lax; Path=/; Max-Age=${SESSION_TTL}`);
    return send(res, 200, { user: AUTH_USER });
  }

  if (req.method === 'POST' && url.pathname === '/api/logout') {
    res.setHeader('Set-Cookie', 'sid=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0');
    return send(res, 200, { ok: true });
  }

  if (req.method === 'GET' && url.pathname === '/api/me') {
    const s = sessionFromReq(req);
    if (!s) return send(res, 401, { error: 'unauthenticated' });
    return send(res, 200, { user: s.user });
  }

  if (req.method === 'GET' && url.pathname === '/api/status') {
    const s = sessionFromReq(req);
    if (!s) return send(res, 401, { error: 'unauthenticated' });
    const d = devices.get(DEVICE_ID);
    return send(res, 200, {
      deviceId: DEVICE_ID,
      online: !!(d && d.agent),
      viewers: d ? d.clients.size : 0,
    });
  }

  if (req.method === 'GET') return serveStatic(req, res);
  return send(res, 405, { error: 'method not allowed' });
});

// ---------- WebSocket: /agent and /client ----------

const agentWss = new WebSocketServer({ noServer: true });
const clientWss = new WebSocketServer({ noServer: true });

// Authenticate + route the upgrade by path before handing to a WSS.
server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://x');

  if (url.pathname === '/agent') {
    const token = (req.headers.authorization || '').replace(/^Bearer\s+/i, '')
      || url.searchParams.get('token') || '';
    if (!safeEqual(token, DEVICE_TOKEN)) {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
      return socket.destroy();
    }
    // device id may be supplied by the agent; defaults to the single device.
    const id = url.searchParams.get('deviceId') || DEVICE_ID;
    agentWss.handleUpgrade(req, socket, head, (ws) => {
      ws._deviceId = id;
      agentWss.emit('connection', ws, req);
    });
    return;
  }

  if (url.pathname === '/client') {
    const s = sessionFromReq(req);
    if (!s) {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
      return socket.destroy();
    }
    // Single-device prototype: every authenticated user sees DEVICE_ID.
    // Real version: look up which devices this user owns.
    const id = url.searchParams.get('deviceId') || DEVICE_ID;
    clientWss.handleUpgrade(req, socket, head, (ws) => {
      ws._deviceId = id;
      ws._user = s.user;
      clientWss.emit('connection', ws, req);
    });
    return;
  }

  socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
  socket.destroy();
});

// Agent (mixer side) — at most one per device for the prototype.
agentWss.on('connection', (ws) => {
  const id = ws._deviceId;
  const d = deviceSlot(id);
  if (d.agent && d.agent.readyState === 1) {
    // Replace a stale agent with the new connection.
    try { d.agent.close(4000, 'replaced by new agent'); } catch {}
  }
  d.agent = ws;
  ws.isAlive = true;
  console.log(`agent connected for ${id} (${d.clients.size} viewers)`);
  broadcastPresence(id);

  ws.on('pong', () => { ws.isAlive = true; });

  // Mixer -> all clients. Always forward as binary (serial bytes).
  ws.on('message', (data, isBinary) => {
    for (const c of d.clients) {
      if (c.readyState === 1) c.send(data, { binary: isBinary });
    }
  });

  ws.on('close', () => {
    if (d.agent === ws) d.agent = null;
    console.log(`agent disconnected for ${id}`);
    broadcastPresence(id);
  });
  ws.on('error', () => {});
});

// Client (phone/browser) — many allowed; fanned out from the agent.
clientWss.on('connection', (ws) => {
  const id = ws._deviceId;
  const d = deviceSlot(id);
  d.clients.add(ws);
  ws.isAlive = true;
  console.log(`client connected for ${id} (${d.clients.size} viewers, user=${ws._user})`);

  // Tell the new client whether its device is live right now.
  ws.send(JSON.stringify({ type: 'presence', deviceId: id, online: !!d.agent }));

  ws.on('pong', () => { ws.isAlive = true; });

  // Client -> mixer. Forward raw to the agent if present.
  ws.on('message', (data, isBinary) => {
    if (d.agent && d.agent.readyState === 1) {
      d.agent.send(data, { binary: isBinary });
    }
  });

  ws.on('close', () => {
    d.clients.delete(ws);
    console.log(`client disconnected for ${id} (${d.clients.size} remaining)`);
    // When the last viewer leaves, ask the firmware to stop streaming
    // meters/spectrum — mirrors the local bridge's behavior. Sent as raw
    // bytes so the agent just writes it to serial.
    if (d.clients.size === 0 && d.agent && d.agent.readyState === 1) {
      d.agent.send(Buffer.from('unsub_all\n'), { binary: true });
    }
  });
  ws.on('error', () => {});
});

// Heartbeat: drop sockets that stop answering pings (cloud LBs love to
// silently eat idle connections).
const HEARTBEAT_MS = 30000;
const heartbeat = setInterval(() => {
  for (const wss of [agentWss, clientWss]) {
    for (const ws of wss.clients) {
      if (ws.isAlive === false) { try { ws.terminate(); } catch {} continue; }
      ws.isAlive = false;
      try { ws.ping(); } catch {}
    }
  }
}, HEARTBEAT_MS);
heartbeat.unref?.();

server.listen(PORT, () => {
  console.log(`relay listening on :${PORT}  (device "${DEVICE_ID}")`);
});

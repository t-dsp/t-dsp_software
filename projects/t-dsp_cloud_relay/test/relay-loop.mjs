// End-to-end relay test — no hardware required.
//
// Boots the relay in-process, connects a mock agent and a mock client,
// and asserts: auth gating, bidirectional byte fan-out, presence, and the
// unsub_all-on-last-client behavior. Run: pnpm test:loop

import assert from 'node:assert';

// Must set secrets BEFORE importing modules that read them at load time.
const PORT = '8199';
process.env.PORT = PORT;
process.env.AUTH_USER = 'admin';
process.env.AUTH_PASS = 'pw';
process.env.SESSION_SECRET = 'test-secret';
process.env.DEVICE_TOKEN = 'devtok';
process.env.DEVICE_ID = 'mixer-1';

const { sign } = await import('../auth.mjs');
await import('../server.mjs'); // starts listening on PORT
const { WebSocket } = await import('ws');

const BASE = `ws://localhost:${PORT}`;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// Buffer every message from socket creation so nothing can be missed by a
// late listener (the relay sends presence the instant a client connects).
function buffered(ws) {
  const q = [];
  const waiters = [];
  ws.on('message', (d) => {
    if (waiters.length) waiters.shift()({ d });
    else q.push({ d });
  });
  return () => (q.length
    ? Promise.resolve(q.shift())
    : new Promise((res, rej) => {
        const t = setTimeout(() => rej(new Error('timeout waiting for message')), 3000);
        waiters.push((v) => { clearTimeout(t); res(v); });
      }));
}
const opened = (ws) => new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); ws.on('unexpected-response', () => rej(new Error('unexpected-response'))); });
const rejected = (ws) => new Promise((res, rej) => { ws.on('open', () => rej(new Error('unexpectedly opened'))); ws.on('error', () => res()); ws.on('unexpected-response', () => res()); });

let failures = 0;
const check = (name, fn) => fn().then(() => console.log(`  ok  ${name}`)).catch((e) => { failures++; console.error(`FAIL  ${name}: ${e.message}`); });

await wait(150); // let server bind

await check('agent rejects bad token', () => rejected(new WebSocket(`${BASE}/agent?token=nope`)));
await check('client rejects missing session', () => rejected(new WebSocket(`${BASE}/client`)));

// Valid agent + client.
const cookie = `sid=${sign({ user: 'admin', exp: Math.floor(Date.now() / 1000) + 3600 })}`;
const agent = new WebSocket(`${BASE}/agent?token=devtok`);
const takeAgent = buffered(agent);
await opened(agent);
const client = new WebSocket(`${BASE}/client`, { headers: { Cookie: cookie } });
const takeClient = buffered(client);
await opened(client);

await check('client receives presence(online=true)', async () => {
  const { d } = await takeClient();
  const m = JSON.parse(d.toString());
  assert.equal(m.type, 'presence');
  assert.equal(m.online, true);
});

await check('agent -> client bytes (binary preserved)', async () => {
  agent.send(Buffer.from([1, 2, 3, 0xc0]), { binary: true });
  const { d } = await takeClient();
  assert.deepEqual([...new Uint8Array(d)], [1, 2, 3, 0xc0]);
});

await check('client -> agent bytes', async () => {
  client.send(Buffer.from([9, 8, 7]), { binary: true });
  const { d } = await takeAgent();
  assert.deepEqual([...new Uint8Array(d)], [9, 8, 7]);
});

await check('unsub_all on last client leave', async () => {
  client.close();
  const { d } = await takeAgent();
  assert.equal(d.toString(), 'unsub_all\n');
});

agent.close();
await wait(100);
console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);

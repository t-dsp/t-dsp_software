// Integration test for the Electron-folded cloud agent.
//
// Wires up: real relay  +  a fake local bridge (stand-in for the serial
// bridge)  +  the real cloud-agent.mjs from t-dsp_web_dev. Then connects a
// "phone" client and asserts bytes flow both ways through the whole chain:
//
//   phone <-> relay <-> cloud-agent <-> fake bridge
//
// No serial/hardware involved. Run from this package: pnpm test:agent

import assert from 'node:assert';

const RELAY_PORT = '8198';
const BRIDGE_PORT = '8766';
process.env.PORT = RELAY_PORT;
process.env.AUTH_USER = 'admin';
process.env.AUTH_PASS = 'pw';
process.env.SESSION_SECRET = 'test-secret';
process.env.DEVICE_TOKEN = 'devtok';
process.env.DEVICE_ID = 'mixer-1';

const { sign } = await import('../auth.mjs');
await import('../server.mjs'); // relay listens on RELAY_PORT
const { WebSocket, WebSocketServer } = await import('ws');
const { startCloudAgent } = await import('../../t-dsp_web_dev/cloud-agent.mjs');

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
function buffered(ws) {
  const q = []; const waiters = [];
  ws.on('message', (d) => { if (waiters.length) waiters.shift()({ d }); else q.push({ d }); });
  return () => (q.length ? Promise.resolve(q.shift())
    : new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), 3000); waiters.push((v) => { clearTimeout(t); res(v); }); }));
}
const opened = (ws) => new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });

let failures = 0;
const check = (name, fn) => fn().then(() => console.log(`  ok  ${name}`)).catch((e) => { failures++; console.error(`FAIL  ${name}: ${e.message}`); });

await wait(150);

// Fake local serial bridge — capture the cloud-agent's connection to it.
const bridge = new WebSocketServer({ port: +BRIDGE_PORT });
let bridgeSock = null;
const bridgeConnected = new Promise((res) => bridge.on('connection', (ws) => { bridgeSock = ws; res(); }));

// Start the real folded-in agent (quietened log).
const agent = startCloudAgent({
  relayUrl: `ws://localhost:${RELAY_PORT}`,
  deviceToken: 'devtok',
  deviceId: 'mixer-1',
  localWsUrl: `ws://localhost:${BRIDGE_PORT}`,
  log: () => {},
});

await bridgeConnected;            // agent reached the fake bridge
const takeBridge = buffered(bridgeSock);
await wait(200);                  // agent also reaches the relay

// Phone client.
const cookie = `sid=${sign({ user: 'admin', exp: Math.floor(Date.now() / 1000) + 3600 })}`;
const phone = new WebSocket(`ws://localhost:${RELAY_PORT}/client`, { headers: { Cookie: cookie } });
const takePhone = buffered(phone);
await opened(phone);

await check('phone sees device online (agent bridged)', async () => {
  const { d } = await takePhone();
  const m = JSON.parse(d.toString());
  assert.equal(m.online, true);
});

await check('phone -> relay -> agent -> bridge', async () => {
  phone.send(Buffer.from([10, 20, 30]), { binary: true });
  const { d } = await takeBridge();
  assert.deepEqual([...new Uint8Array(d)], [10, 20, 30]);
});

await check('bridge -> agent -> relay -> phone', async () => {
  bridgeSock.send(Buffer.from([4, 5, 6, 0xc0]), { binary: true });
  const { d } = await takePhone();
  assert.deepEqual([...new Uint8Array(d)], [4, 5, 6, 0xc0]);
});

agent.stop();
phone.close();
bridge.close();
await wait(100);
console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);

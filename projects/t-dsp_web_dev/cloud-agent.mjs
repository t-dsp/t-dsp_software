// cloud-agent.mjs — folds the "remote control" agent into the desktop app.
//
// The serial port has ONE owner: serial-bridge.mjs (running in-process,
// fanning serial out on ws://localhost:8765). So this agent never touches
// serial. It connects to that local bridge as just another client and
// relays raw bytes to/from the cloud relay:
//
//   local bridge (ws://localhost:8765)  <-->  cloud relay (wss://…/agent)
//
// Everything is raw passthrough — no parsing — so the same bytes the
// desktop UI exchanges with the firmware also reach the phone. Both links
// auto-reconnect independently: the bridge link survives firmware-upload
// reboots; the cloud link backs off on network drops.

import { WebSocket } from 'ws';

export function startCloudAgent({
  relayUrl,
  deviceToken,
  deviceId = 'mixer-1',
  localWsUrl = 'ws://localhost:8765',
  log = console.log,
}) {
  if (!relayUrl || !deviceToken) {
    log('cloud-agent: disabled (no relayUrl/deviceToken)');
    return { stop() {} };
  }

  let local = null;
  let cloud = null;
  let stopped = false;
  let cloudBackoff = 1000;
  const LOCAL_RETRY_MS = 1500;

  const cloudUrl = `${relayUrl.replace(/\/$/, '')}/agent?deviceId=${encodeURIComponent(deviceId)}`;

  // --- local bridge link (loopback; the bridge is the serial owner) ---
  function connectLocal() {
    if (stopped) return;
    const ws = new WebSocket(localWsUrl);
    ws.binaryType = 'nodebuffer';

    ws.on('open', () => { local = ws; log(`cloud-agent: local bridge connected (${localWsUrl})`); });

    // serial (via bridge) -> cloud
    ws.on('message', (data, isBinary) => {
      if (cloud && cloud.readyState === 1) cloud.send(data, { binary: isBinary });
    });

    ws.on('close', () => {
      if (local === ws) local = null;
      if (!stopped) setTimeout(connectLocal, LOCAL_RETRY_MS);
    });
    ws.on('error', () => { /* 'close' handles the retry */ });
  }

  // --- cloud relay link (outbound; nothing opened on this machine) ---
  function connectCloud() {
    if (stopped) return;
    log(`cloud-agent: connecting to ${cloudUrl}`);
    const ws = new WebSocket(cloudUrl, { headers: { authorization: `Bearer ${deviceToken}` } });
    ws.binaryType = 'nodebuffer';

    ws.on('open', () => { cloud = ws; cloudBackoff = 1000; log('cloud-agent: cloud relay connected — remote control live'); });

    // phone (via relay) -> serial (via bridge). Includes the relay's
    // unsub_all when the last viewer leaves.
    ws.on('message', (data, isBinary) => {
      if (local && local.readyState === 1) local.send(data, { binary: isBinary });
    });

    ws.on('close', () => {
      if (cloud === ws) cloud = null;
      if (stopped) return;
      log(`cloud-agent: cloud relay disconnected — retry in ${cloudBackoff}ms`);
      setTimeout(connectCloud, cloudBackoff);
      cloudBackoff = Math.min(cloudBackoff * 2, 15000);
    });
    ws.on('error', (err) => { log(`cloud-agent: cloud error — ${err.message}`); });
  }

  connectLocal();
  connectCloud();

  return {
    stop() {
      stopped = true;
      try { local && local.close(); } catch {}
      try { cloud && cloud.close(); } catch {}
    },
  };
}

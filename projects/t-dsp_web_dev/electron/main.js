// Electron main — CommonJS because Electron's ESM main support is
// fragile around importing the built-in 'electron' module. The serial
// bridge is ESM, so we dynamic-import it from here. It boots itself
// on load (top-level start()), so there is no factory call.

const { app, BrowserWindow } = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const { pathToFileURL } = require('node:url');

import(pathToFileURL(path.join(__dirname, '..', 'serial-bridge.mjs')).href)
  .catch((err) => console.error('bridge failed to start:', err));

// Optional remote-control agent. When configured, the desktop app also
// dials OUT to a cloud relay so a phone can drive this mixer from anywhere
// (see projects/t-dsp_cloud_relay). It taps the local serial bridge over
// ws://localhost:8765 — it never touches the serial port directly, so it
// can't contend with the bridge that owns it.
let cloudAgent = null;

// Config comes from env (dev: `RELAY_URL=… DEVICE_TOKEN=… pnpm app:dev`)
// or, for the packaged app, a JSON file in Electron's userData dir:
//   { "enabled": true, "relayUrl": "wss://…", "deviceToken": "…", "deviceId": "mixer-1" }
function loadCloudConfig() {
  if (process.env.RELAY_URL && process.env.DEVICE_TOKEN) {
    return {
      relayUrl: process.env.RELAY_URL,
      deviceToken: process.env.DEVICE_TOKEN,
      deviceId: process.env.DEVICE_ID || 'mixer-1',
    };
  }
  const cfgPath = path.join(app.getPath('userData'), 'cloud-config.json');
  try {
    if (fs.existsSync(cfgPath)) {
      const c = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
      if (c.enabled && c.relayUrl && c.deviceToken) return c;
    }
  } catch (err) {
    console.error('cloud-agent: bad cloud-config.json —', err.message);
  }
  console.log(`cloud-agent: not configured (set RELAY_URL+DEVICE_TOKEN, or enable ${cfgPath})`);
  return null;
}

async function startCloudAgent(cfg) {
  if (!cfg) return;
  try {
    const mod = await import(pathToFileURL(path.join(__dirname, '..', 'cloud-agent.mjs')).href);
    cloudAgent = mod.startCloudAgent(cfg);
  } catch (err) {
    console.error('cloud-agent: failed to start —', err);
  }
}

// Pass the remote URL to the renderer via a query param (no preload/IPC
// needed) so the surface can show a "scan to control from your phone" QR.
// The relay is reached over wss:// by the agent, but the phone opens it
// over https://, so translate the scheme for the QR.
function createWindow(remoteUrl) {
  const win = new BrowserWindow({
    width: 1400,
    height: 900,
    title: 't-dsp dev surface — TAC5212',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  const search = remoteUrl ? `remote=${encodeURIComponent(remoteUrl)}` : '';
  if (app.isPackaged) {
    win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'), search ? { search } : undefined);
  } else {
    win.loadURL('http://localhost:5173' + (search ? `?${search}` : ''));
    win.webContents.openDevTools({ mode: 'detach' });
  }
}

app.whenReady().then(() => {
  const cfg = loadCloudConfig();
  const remoteUrl = cfg
    ? cfg.relayUrl.replace(/^wss:/, 'https:').replace(/^ws:/, 'http:')
    : null;
  createWindow(remoteUrl);
  startCloudAgent(cfg);
});

app.on('window-all-closed', () => {
  if (cloudAgent) cloudAgent.stop();
  app.quit();
});

// Electron main — CommonJS because Electron's ESM main support is
// fragile around importing the built-in 'electron' module. The serial
// bridge is ESM, so we dynamic-import it from here. It boots itself
// on load (top-level start()), so there is no factory call.

const { app, BrowserWindow, ipcMain } = require('electron');
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
function cloudConfigPath() {
  return path.join(app.getPath('userData'), 'cloud-config.json');
}

function readCloudFile() {
  try {
    const p = cloudConfigPath();
    if (fs.existsSync(p)) return JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch (err) {
    console.error('cloud-agent: bad cloud-config.json —', err.message);
  }
  return null;
}

// Effective config used to start the agent. Env wins (dev convenience);
// otherwise the file written by the in-app Remote panel.
function loadCloudConfig() {
  if (process.env.RELAY_URL && process.env.DEVICE_TOKEN) {
    return {
      relayUrl: process.env.RELAY_URL,
      deviceToken: process.env.DEVICE_TOKEN,
      deviceId: process.env.DEVICE_ID || 'mixer-1',
    };
  }
  const f = readCloudFile();
  if (f && f.enabled && f.relayUrl && f.deviceToken) return f;
  console.log(`cloud-agent: not configured (use the in-app Remote panel, or set ${cloudConfigPath()})`);
  return null;
}

// IPC for the renderer's Remote settings panel. Reads/writes the config
// file and applies changes live by restarting the agent in-process.
function registerCloudIpc() {
  ipcMain.handle('cloud:get', () => {
    const f = readCloudFile() || {};
    return {
      enabled: !!f.enabled,
      relayUrl: f.relayUrl || '',
      deviceToken: f.deviceToken || '',
      deviceId: f.deviceId || 'mixer-1',
      envOverride: !!(process.env.RELAY_URL && process.env.DEVICE_TOKEN),
    };
  });

  ipcMain.handle('cloud:set', (_e, cfg) => {
    const next = {
      enabled: !!cfg.enabled,
      relayUrl: String(cfg.relayUrl || '').trim(),
      deviceToken: String(cfg.deviceToken || '').trim(),
      deviceId: String(cfg.deviceId || 'mixer-1').trim() || 'mixer-1',
    };
    try {
      fs.writeFileSync(cloudConfigPath(), JSON.stringify(next, null, 2));
    } catch (err) {
      return { ok: false, error: err.message };
    }
    // Apply immediately: stop any running agent and restart from the new
    // effective config — no app restart needed.
    if (cloudAgent) { try { cloudAgent.stop(); } catch {} cloudAgent = null; }
    const eff = loadCloudConfig();
    startCloudAgent(eff);
    return { ok: true, effectiveRelayUrl: eff ? eff.relayUrl : null };
  });
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

// The Remote panel (settings + QR) lives in the renderer and talks to the
// main process over the preload bridge (electron/preload.js).
function createWindow() {
  const win = new BrowserWindow({
    width: 1400,
    height: 900,
    title: 't-dsp dev surface — TAC5212',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  if (app.isPackaged) {
    win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  } else {
    win.loadURL('http://localhost:5173');
    win.webContents.openDevTools({ mode: 'detach' });
  }
}

app.whenReady().then(() => {
  registerCloudIpc();
  createWindow();
  startCloudAgent(loadCloudConfig());
});

app.on('window-all-closed', () => {
  if (cloudAgent) cloudAgent.stop();
  app.quit();
});

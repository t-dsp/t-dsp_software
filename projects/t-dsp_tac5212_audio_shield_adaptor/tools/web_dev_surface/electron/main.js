// Electron main — CommonJS because Electron's ESM main support is
// fragile around importing the built-in 'electron' module. The serial
// bridge is ESM, so we dynamic-import it from here. It boots itself
// on load (top-level start()), so there is no factory call.

const { app, BrowserWindow } = require('electron');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

import(pathToFileURL(path.join(__dirname, '..', 'serial-bridge.mjs')).href)
  .catch((err) => console.error('bridge failed to start:', err));

function createWindow() {
  const win = new BrowserWindow({
    width: 1400,
    height: 900,
    title: 't-dsp dev surface — TAC5212',
    webPreferences: {
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

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  app.quit();
});

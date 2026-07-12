// Preload bridge — the renderer runs sandboxed (contextIsolation: true,
// no nodeIntegration), so it can't touch the filesystem. Expose a tiny,
// explicit API for the Remote settings panel to read/write the cloud
// config via IPC. Nothing else is surfaced to the page.

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('tdspCloud', {
  // -> { enabled, relayUrl, deviceToken, deviceId, envOverride }
  get: () => ipcRenderer.invoke('cloud:get'),
  // cfg -> { ok, effectiveRelayUrl|null }
  set: (cfg) => ipcRenderer.invoke('cloud:set', cfg),
});

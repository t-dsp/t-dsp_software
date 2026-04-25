import { defineConfig } from 'vite';
import { resolve } from 'node:path';

// Allow imports from lib/TDspMPE/presets.json (lives 4 directories up,
// at the repo root). Keeps the preset list single-sourced instead of
// forcing a duplicate copy inside web_dev_surface/src.
const repoRoot = resolve(__dirname, '..', '..', '..', '..');

export default defineConfig({
  // Relative asset paths so the build works under both http:// (dev
  // server) and file:// (Electron packaged app loadFile).
  base: './',
  server: {
    host: 'localhost',
    port: 5173,
    strictPort: true,
    fs: {
      allow: [repoRoot],
    },
  },
});

import { defineConfig } from 'vite';
import { resolve } from 'node:path';

// Allow imports from lib/TDspMPE/presets.json (lives 4 directories up,
// at the repo root). Keeps the preset list single-sourced instead of
// forcing a duplicate copy inside web_dev_surface/src.
const repoRoot = resolve(__dirname, '..', '..', '..', '..');

export default defineConfig({
  server: {
    host: 'localhost',
    port: 5173,
    fs: {
      allow: [repoRoot],
    },
  },
});

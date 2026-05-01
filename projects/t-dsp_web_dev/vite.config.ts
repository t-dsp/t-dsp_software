import { defineConfig } from 'vite';
import { resolve } from 'node:path';
import solid from 'vite-plugin-solid';
import tailwindcss from '@tailwindcss/vite';

// Allow imports from lib/TDspMPE/presets.json (lives 4 directories up,
// at the repo root). Keeps the preset list single-sourced instead of
// forcing a duplicate copy inside web_dev_surface/src.
const repoRoot = resolve(__dirname, '..', '..', '..', '..');

export default defineConfig({
  // Relative asset paths so the build works under both http:// (dev
  // server) and file:// (Electron packaged app loadFile).
  base: './',
  // Solid + Tailwind v4 plugins (Phase 2 of UI rebuild). Solid lets
  // us write JSX/TSX components that compile to direct DOM updates
  // alongside the existing vanilla code; Tailwind v4 carries the
  // design tokens via its @theme block. Existing .ts panels keep
  // working as-is — nothing here forces a migration.
  plugins: [solid(), tailwindcss()],
  server: {
    host: 'localhost',
    port: 5173,
    strictPort: true,
    fs: {
      allow: [repoRoot],
    },
  },
});

// sync-arp-library.mjs — copy the canonical arp preset library into the app bundle.
//
// The firmware owns the library at lib/TDspArp/presets.json (238 presets across 15
// genres). The app can't import from outside its own root (no metro watchFolders), so we
// vendor a copy at src/arpLibrary.data.json that Metro bundles. Run this whenever the
// firmware library changes:  npm run sync:arp
//
// Keeping it a generated copy (not a hand-edit) means the source of truth stays singular.

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, '../../../lib/TDspArp/presets.json');
const dst = resolve(here, '../src/arpLibrary.data.json');

const json = JSON.parse(readFileSync(src, 'utf8'));   // parse to validate before writing
const count = json.presets?.length ?? 0;
writeFileSync(dst, JSON.stringify(json) + '\n');
console.log(`synced ${count} arp presets -> src/arpLibrary.data.json`);

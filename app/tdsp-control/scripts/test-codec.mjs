// test-codec.mjs — compile + run the pure-TS codec tests (no hardware, no Metro).
// The codec (src/loopClip.ts) is the wire-format contract shared with the firmware
// (lib/TDspMidiLoop/src/LoopClipIo.h), so this is the off-target correctness gate.
//   node scripts/test-codec.mjs
import { execFileSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const out = mkdtempSync(join(tmpdir(), 'tdsp-codec-'));
const tsc = join('node_modules', '.bin', process.platform === 'win32' ? 'tsc.cmd' : 'tsc');

console.log('compiling codec + tests ->', out);
execFileSync(tsc, [
  'src/loopClip.ts', 'src/loopClip.test.ts',
  '--ignoreConfig', '--outDir', out,
  '--module', 'commonjs', '--target', 'es2020',
  '--moduleResolution', 'node10', '--ignoreDeprecations', '6.0',
  '--skipLibCheck', '--strict',
], { stdio: 'inherit', shell: process.platform === 'win32' });

console.log('running node --test');
execFileSync(process.execPath, ['--test', out], { stdio: 'inherit' });

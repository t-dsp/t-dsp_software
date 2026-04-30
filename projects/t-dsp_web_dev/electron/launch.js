// Spawns electron with a clean environment. Specifically strips
// ELECTRON_RUN_AS_NODE, which VSCode's extension host sets on child
// processes — if it is present, Electron boots as plain Node and
// `require('electron')` returns a path string instead of the
// main-process API. Usage: `node electron/launch.js .`

const { spawn } = require('node:child_process');

const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;

const electronBinary = require('electron');
const args = process.argv.slice(2);

const child = spawn(electronBinary, args, {
  stdio: 'inherit',
  env,
  windowsHide: false,
});

child.on('close', (code, signal) => {
  if (code === null) {
    console.error(electronBinary, 'exited with signal', signal);
    process.exit(1);
  }
  process.exit(code);
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    if (!child.killed) child.kill(sig);
  });
}

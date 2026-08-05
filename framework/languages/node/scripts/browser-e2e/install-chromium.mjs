import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const workspaceRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const executable = process.platform === 'win32' ? 'npx.cmd' : 'npx';
const args = ['playwright', 'install'];
if (process.argv.includes('--with-deps')) args.push('--with-deps');
args.push('chromium');
const result = spawnSync(executable, args, {
  cwd: workspaceRoot,
  stdio: 'inherit',
  env: {
    ...process.env,
    PLAYWRIGHT_BROWSERS_PATH: process.env.PLAYWRIGHT_BROWSERS_PATH
      ?? path.join(workspaceRoot, '.cache/ms-playwright')
  }
});
process.exitCode = result.status ?? 1;

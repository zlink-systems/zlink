import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { browserCachePath } from './browser-cache-path.mjs';

const workspaceRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const executable = process.execPath;
const args = [path.join(workspaceRoot, 'node_modules', 'playwright', 'cli.js'), 'install'];
if (process.argv.includes('--with-deps')) args.push('--with-deps');
args.push('chromium');
const result = spawnSync(executable, args, {
  cwd: workspaceRoot,
  stdio: 'inherit',
  env: {
    ...process.env,
    PLAYWRIGHT_BROWSERS_PATH: browserCachePath()
  }
});
process.exitCode = result.status ?? 1;

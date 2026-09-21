import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { spawnSync } from 'node:child_process';

const clientRoot = fileURLToPath(new URL('..', import.meta.url));
const packageLockPath = join(clientRoot, 'package-lock.json');
const nodeModulesPath = join(clientRoot, 'node_modules');
const lockMarkerPath = join(nodeModulesPath, '.zoneworld-browser-lock');
const npmCli = process.env.npm_execpath
  ?? join(dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js');

function run(command, args) {
  const result = spawnSync(command, args, {
    cwd: clientRoot,
    env: process.env,
    stdio: 'inherit'
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}

const lockHash = createHash('sha256')
  .update(readFileSync(packageLockPath))
  .digest('hex');
const installedLockHash = existsSync(lockMarkerPath)
  ? readFileSync(lockMarkerPath, 'utf8').trim()
  : '';
const dependenciesReady = [
  join(nodeModulesPath, '@playwright', 'test', 'package.json'),
  join(nodeModulesPath, 'vite', 'package.json')
].every((dependencyManifest) => existsSync(dependencyManifest));

if (!existsSync(nodeModulesPath) || !dependenciesReady || installedLockHash !== lockHash) {
  run(process.execPath, [npmCli,
    'ci', '--ignore-scripts', '--no-audit', '--no-fund'
  ]);
  writeFileSync(lockMarkerPath, `${lockHash}\n`);
}

const { chromium } = await import('@playwright/test');
if (!existsSync(chromium.executablePath())) {
  run(process.execPath, [npmCli, 'exec', '--', 'playwright', 'install', 'chromium']);
}

if (!existsSync(chromium.executablePath())) {
  throw new Error(`Playwright Chromium executable is missing at '${chromium.executablePath()}'.`);
}

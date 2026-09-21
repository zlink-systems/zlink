import { createHash } from 'node:crypto';
import {
  existsSync,
  readFileSync,
  readdirSync,
  realpathSync,
  writeFileSync
} from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, isAbsolute, join, relative, sep } from 'node:path';
import { spawnSync } from 'node:child_process';

const clientRoot = fileURLToPath(new URL('..', import.meta.url));
const workspaceRoot = fileURLToPath(new URL('../../../../node/', import.meta.url));
const workspacePackagesRoot = join(workspaceRoot, 'packages');
const packageLockPath = join(clientRoot, 'package-lock.json');
const nodeModulesPath = join(clientRoot, 'node_modules');
const workspaceNodeModulesPath = join(workspaceRoot, 'node_modules');
const lockMarkerPath = join(nodeModulesPath, '.zoneworld-browser-lock');
const npmCli = process.env.npm_execpath
  ?? join(dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js');

function run(command, args, cwd = clientRoot) {
  const result = spawnSync(command, args, {
    cwd,
    env: process.env,
    stdio: 'inherit'
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function collectExportTargets(value, targets = []) {
  if (typeof value === 'string') {
    targets.push(value);
  } else if (value && typeof value === 'object') {
    for (const nested of Object.values(value)) collectExportTargets(nested, targets);
  }
  return targets;
}

function packageEntryPaths(manifest) {
  return [
    ...collectExportTargets(manifest.exports),
    manifest.module,
    manifest.main
  ].filter((entryPath) => typeof entryPath === 'string' && !entryPath.includes('*'));
}

function linkedWorkspacePackages() {
  const scopePath = join(nodeModulesPath, '@zlink-systems');
  if (!existsSync(scopePath)) return [];

  return readdirSync(scopePath).flatMap((packageName) => {
    const linkPath = join(scopePath, packageName);
    let resolvedPath;
    try {
      resolvedPath = realpathSync(linkPath);
    } catch {
      return [];
    }
    const relativePath = relative(workspacePackagesRoot, resolvedPath);
    if (!relativePath || relativePath === '..' || relativePath.startsWith(`..${sep}`)
      || isAbsolute(relativePath) || relativePath.includes(sep)) return [];
    const manifestPath = join(resolvedPath, 'package.json');
    if (!existsSync(manifestPath)) return [];
    return [{ manifest: readJson(manifestPath), root: resolvedPath }];
  });
}

function prepareLinkedWorkspacePackages() {
  const packagesToBuild = linkedWorkspacePackages().filter(({ manifest, root }) =>
    packageEntryPaths(manifest).some((entryPath) => !existsSync(join(root, entryPath)))
  );
  if (packagesToBuild.length === 0) return;

  if (!existsSync(workspaceNodeModulesPath)) {
    run(process.execPath, [npmCli, 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], workspaceRoot);
  }

  const workspaceManifest = readJson(join(workspaceRoot, 'package.json'));
  for (const { manifest } of packagesToBuild) {
    if (manifest.scripts?.build) {
      run(process.execPath, [npmCli, 'run', 'build', '-w', manifest.name], workspaceRoot);
    } else if (workspaceManifest.scripts?.build) {
      run(process.execPath, [npmCli, 'run', 'build'], workspaceRoot);
      break;
    } else {
      throw new Error(`No build entrypoint is defined for workspace package '${manifest.name}'.`);
    }
  }
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

prepareLinkedWorkspacePackages();

const { chromium } = await import('@playwright/test');
if (!existsSync(chromium.executablePath())) {
  run(process.execPath, [npmCli, 'exec', '--', 'playwright', 'install', 'chromium']);
}

if (!existsSync(chromium.executablePath())) {
  throw new Error(`Playwright Chromium executable is missing at '${chromium.executablePath()}'.`);
}

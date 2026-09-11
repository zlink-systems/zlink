#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { findLocalBindingArchive } from './local-binding-package.mjs';

const nodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(nodeRoot, '../../..');
const packageRoot = path.join(nodeRoot, 'node_modules', '@zlink-systems');
const installedPath = path.join(packageRoot, 'zlink');
const backupPath = path.join(packageRoot, '.zlink-registry-package');
const mode = process.argv[2];

if (mode === 'select') {
  if (process.env.ZLINK_NODE_USE_BINDINGS_SOURCE === '1') useSourcePackage();
} else if (mode === 'source') useSourcePackage();
else if (mode === 'package') useRegistryPackage();
else throw new Error('Usage: node scripts/use-bindings-dependency.mjs <select|source|package>');

function useSourcePackage() {
  if (fs.existsSync(backupPath)) {
    if (fs.existsSync(installedPath)) {
      process.stdout.write(`Local bindings package is already selected at ${installedPath}\n`);
      return;
    }
    throw new Error(`A bindings package backup exists without an active package at ${backupPath}. Run npm run use:bindings-package.`);
  }
  const version = readVersion(path.join(repoRoot, 'bindings', 'node', 'VERSION'), 'ZLINK_BINDING_VERSION');
  const archive = findLocalBindingArchive({
    repoRoot,
    version,
    configuredRoot: process.env.ZLINK_LOCAL_PACKAGE_ROOT
  });
  requireInstalledPackage(version);

  fs.renameSync(installedPath, backupPath);
  try {
    fs.mkdirSync(installedPath, { recursive: true });
    const result = spawnSync('tar', ['-xzf', archive, '--strip-components=1', '-C', installedPath], {
      cwd: nodeRoot,
      stdio: 'inherit',
      shell: false
    });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`tar exited with status ${result.status ?? 1}.`);
    const installedVersion = readPackageVersion(installedPath);
    if (installedVersion !== version) {
      throw new Error(`Local bindings archive contains ${installedVersion}; expected ${version}.`);
    }
  } catch (error) {
    fs.rmSync(installedPath, { recursive: true, force: true });
    fs.renameSync(backupPath, installedPath);
    throw error;
  }
  process.stdout.write(`Using local bindings package ${archive}\n`);
}

function useRegistryPackage() {
  if (!fs.existsSync(backupPath)) {
    throw new Error('No saved registry bindings package exists. Run npm run use:bindings-source first.');
  }
  fs.rmSync(installedPath, { recursive: true, force: true });
  fs.renameSync(backupPath, installedPath);
  process.stdout.write(`Restored registry bindings package ${readPackageVersion(installedPath)}\n`);
}

function requireInstalledPackage(version) {
  if (!fs.existsSync(installedPath)) {
    throw new Error(`Registry bindings package is not installed at ${installedPath}. Run npm ci first.`);
  }
  const actual = readPackageVersion(installedPath);
  if (actual !== version) {
    throw new Error(`Installed registry bindings version is ${actual}; expected ${version}. Run npm ci first.`);
  }
}

function readPackageVersion(directory) {
  return JSON.parse(fs.readFileSync(path.join(directory, 'package.json'), 'utf8')).version;
}

function readVersion(file, key) {
  const line = fs.readFileSync(file, 'utf8').split(/\r?\n/u).find((value) => value.startsWith(`${key}=`));
  if (line === undefined) throw new Error(`${key} is missing from ${file}.`);
  return line.slice(key.length + 1).trim();
}

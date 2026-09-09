#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const samplesRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const nodeRoot = path.dirname(samplesRoot);

export function prepareSampleDependencies(sampleRoot) {
  const manifest = readJson(path.join(sampleRoot, 'package.json'));
  const localPackages = localPackageMap();
  const repositoryMode = isRepositorySample(sampleRoot, localPackages)
    && process.env.ZLINK_NODE_SAMPLES_PACKAGE_MODE !== '1';

  if (!repositoryMode) {
    verifyPackageMode(sampleRoot, manifest, localPackages);
    return 'package';
  }

  const dependencies = Object.keys(manifest.dependencies ?? {})
    .filter((name) => localPackages.has(name));
  for (const name of dependencies) {
    linkLocalPackage(sampleRoot, name, localPackages.get(name), manifest.dependencies[name]);
  }
  process.stdout.write(`sample_dependency_mode=repository sample=${manifest.name}\n`);
  return 'repository';
}

function isRepositorySample(sampleRoot, localPackages) {
  if (path.dirname(path.resolve(sampleRoot)) !== samplesRoot) return false;
  const workspaceManifest = readJson(path.join(nodeRoot, 'package.json'));
  return workspaceManifest.name === '@zlink-systems/node-framework-workspace'
    && localPackages.has('@zlink-systems/framework');
}

function localPackageMap() {
  const result = new Map();
  const packagesRoot = path.join(nodeRoot, 'packages');
  if (!fs.existsSync(packagesRoot)) return result;
  for (const entry of fs.readdirSync(packagesRoot, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const packageRoot = path.join(packagesRoot, entry.name);
    const manifestPath = path.join(packageRoot, 'package.json');
    if (!fs.existsSync(manifestPath)) continue;
    const manifest = readJson(manifestPath);
    if (typeof manifest.name === 'string') result.set(manifest.name, packageRoot);
  }
  return result;
}

function linkLocalPackage(sampleRoot, name, target, expectedVersion) {
  const localVersion = readJson(path.join(target, 'package.json')).version;
  if (localVersion !== expectedVersion) {
    throw new Error(`Local package ${name} is ${localVersion}; sample requires ${expectedVersion}.`);
  }
  if (!fs.existsSync(path.join(target, 'dist'))) {
    throw new Error(`Local package ${name} has no dist output. Run npm run build from ${nodeRoot} first.`);
  }
  const link = path.join(sampleRoot, 'node_modules', ...name.split('/'));
  fs.mkdirSync(path.dirname(link), { recursive: true });
  if (fs.existsSync(link)) {
    const current = fs.realpathSync(link);
    if (current === fs.realpathSync(target)) return;
    throw new Error(
      `${link} is not the repository package. Remove the sample node_modules directory before repository-mode execution.`
    );
  }
  fs.symlinkSync(target, link, process.platform === 'win32' ? 'junction' : 'dir');
}

function verifyPackageMode(sampleRoot, manifest, localPackages) {
  for (const name of Object.keys(manifest.dependencies ?? {}).filter((value) => value.startsWith('@zlink-systems/'))) {
    const installed = path.join(sampleRoot, 'node_modules', ...name.split('/'));
    if (!fs.existsSync(installed)) {
      throw new Error(`Package mode requires ${name} in this sample. Run npm install in ${sampleRoot}.`);
    }
    const resolved = fs.realpathSync(installed);
    if ([...localPackages.values()].some((localRoot) => resolved === fs.realpathSync(localRoot))) {
      throw new Error(
        `Package mode resolved ${name} to the repository workspace. Remove the sample node_modules directory and run npm install.`
      );
    }
  }
  process.stdout.write(`sample_dependency_mode=package sample=${manifest.name}\n`);
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

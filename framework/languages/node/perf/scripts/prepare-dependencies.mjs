#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const perfRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const nodeRoot = path.resolve(perfRoot, '..');
const helper = path.resolve(nodeRoot, 'samples/scripts/prepare-sample-dependencies.mjs');
const frameworkVersion = requiredEnvironment('ZLINK_PERF_FRAMEWORK_VERSION');
const packageSource = requiredEnvironment('ZLINK_PERF_PACKAGE_SOURCE');
if (packageSource !== 'published' && packageSource !== 'local') {
  throw new Error(`ZLINK_PERF_PACKAGE_SOURCE must be published or local, got ${packageSource}.`);
}

const manifest = readJson(path.join(perfRoot, 'package.json'));
const frameworkPackages = [
  '@zlink-systems/framework',
  '@zlink-systems/framework-locations-redis',
  '@zlink-systems/nestjs',
  '@zlink-systems/stream-connector'
];
const sharedNestPackages = ['@nestjs/common', '@nestjs/core'];
for (const name of frameworkPackages) {
  if (manifest.dependencies?.[name] !== frameworkVersion) {
    throw new Error(`Perf dependency ${name} must match ZLINK_PERF_FRAMEWORK_VERSION=${frameworkVersion}; package.json has ${manifest.dependencies?.[name]}.`);
  }
}
if (Object.hasOwn(manifest.dependencies ?? {}, '@zlink-systems/zlink')) {
  throw new Error('The binding must be resolved through the Framework package dependency.');
}

if (packageSource === 'local') {
  process.env.ZLINK_NODE_SAMPLES_PACKAGE_MODE = '0';
  for (const name of [...frameworkPackages, '@zlink-systems/zlink', ...sharedNestPackages]) removeInstalledPackage(name);
} else {
  process.env.ZLINK_NODE_SAMPLES_PACKAGE_MODE = '1';
}

const { prepareSampleDependencies } = await import(pathToFileURL(helper).href);
const mode = prepareSampleDependencies(perfRoot);
if (packageSource === 'local' && mode !== 'repository') {
  throw new Error('Local package source requires the Node workspace repository mode.');
}
if (packageSource === 'published' && mode !== 'package') {
  throw new Error('Published package source requires the Node sample package mode.');
}

for (const name of frameworkPackages) {
  const installed = installedPackage(name);
  const actual = readJson(path.join(installed, 'package.json')).version;
  if (actual !== frameworkVersion) {
    throw new Error(`${name} resolved to ${actual}; runner requested Framework ${frameworkVersion}.`);
  }
  if (packageSource === 'local') {
    const expected = fs.realpathSync(path.join(nodeRoot, 'packages', name.slice('@zlink-systems/'.length)));
    if (installed !== expected) throw new Error(`${name} did not resolve to the Node workspace package.`);
  }
}

if (packageSource === 'local') verifyLocalBinding();
if (packageSource === 'local') linkWorkspaceNestPackages();

process.stdout.write(`perf_dependency_source=${packageSource} framework_version=${frameworkVersion}\n`);

function verifyLocalBinding() {
  const framework = readJson(path.join(nodeRoot, 'packages/framework/package.json'));
  const expectedVersion = framework.dependencies?.['@zlink-systems/zlink'];
  if (typeof expectedVersion !== 'string' || expectedVersion.length === 0) {
    throw new Error('The checkout Framework package does not declare its Node binding dependency.');
  }
  const registryBackup = path.join(nodeRoot, 'node_modules/@zlink-systems/.zlink-registry-package');
  if (!fs.existsSync(registryBackup)) {
    throw new Error('The Node workspace is not using its local binding archive. Run scripts/local-package/build-wsl.sh node, npm ci, and npm run use:bindings-source from framework/languages/node.');
  }
  const root = installedPackage('@zlink-systems/zlink', nodeRoot);
  const binding = readJson(path.join(root, 'package.json'));
  if (binding.version !== expectedVersion) {
    throw new Error(`Local Node binding ${binding.version} does not match Framework dependency ${expectedVersion}.`);
  }
  const platform = `${process.platform}-${process.arch}`;
  if (!fs.existsSync(path.join(root, 'prebuilds', platform, 'zlink.node')) ||
      !fs.existsSync(path.join(root, 'provenance/core-package-provenance.json'))) {
    throw new Error(`The local Node binding has no ${platform} addon or Core provenance. Run scripts/local-package/build-wsl.sh node.`);
  }
  process.stdout.write(`local_node_binding=${root} version=${binding.version}\n`);
}

function linkWorkspaceNestPackages() {
  for (const name of sharedNestPackages) {
    const source = installedPackage(name, nodeRoot);
    const actual = readJson(path.join(source, 'package.json')).version;
    if (actual !== manifest.dependencies[name]) {
      throw new Error(`${name} in the Node workspace is ${actual}; perf requires ${manifest.dependencies[name]}.`);
    }
    const target = path.join(perfRoot, 'node_modules', ...name.split('/'));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.symlinkSync(source, target, 'dir');
  }
}

function installedPackage(name, root = perfRoot) {
  const packagePath = path.join(root, 'node_modules', ...name.split('/'));
  if (!fs.existsSync(packagePath)) throw new Error(`Required package ${name} is missing from ${path.dirname(packagePath)}.`);
  return fs.realpathSync(packagePath);
}

function removeInstalledPackage(name) {
  const packagePath = path.join(perfRoot, 'node_modules', ...name.split('/'));
  fs.rmSync(packagePath, { recursive: true, force: true });
}

function requiredEnvironment(name) {
  const value = process.env[name];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`${name} is required for the Node perf build.`);
  return value;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

#!/usr/bin/env node
// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const packageRoot = path.resolve(__dirname, '..');
const prebuildRoot = path.join(packageRoot, 'prebuilds');
const packageVersion = JSON.parse(
  fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8')
).version;
const provenancePath = path.join(packageRoot, 'provenance', 'core-package-provenance.json');
const provenance = fs.existsSync(provenancePath)
  ? JSON.parse(fs.readFileSync(provenancePath, 'utf8'))
  : undefined;
const coreVersion = process.env.ZLINK_CORE_VERSION || provenance?.version;
if (!coreVersion || !/^\d+\.\d+\.\d+$/.test(coreVersion)) {
  throw new Error('Core 11 package provenance or ZLINK_CORE_VERSION=X.Y.Z is required');
}
if (!/^11\./.test(coreVersion)) {
  throw new Error(`prebuild verification requires Core 11.x, found ${coreVersion}`);
}
if (process.env.ZLINK_CORE_VERSION && provenance?.version &&
    process.env.ZLINK_CORE_VERSION !== provenance.version) {
  throw new Error(
    `ZLINK_CORE_VERSION=${process.env.ZLINK_CORE_VERSION} does not match provenance=${provenance.version}`
  );
}
const coreMajor = coreVersion.split('.')[0];

function fail(message) {
  throw new Error(message);
}

function commandOutput(command, args) {
  return execFileSync(command, args, { encoding: 'utf8' });
}

function optionalCommandOutput(command, args) {
  try {
    return commandOutput(command, args);
  } catch {
    return null;
  }
}

function fileDescription(target) {
  return commandOutput('file', [target]).trim();
}

function readElfDynamic(target) {
  return commandOutput('readelf', ['-d', target]);
}

function readPeImports(target) {
  const output = optionalCommandOutput('objdump', ['-p', target]);
  if (!output) return [];
  return [...output.matchAll(/DLL Name:\s*([^\r\n]+)/g)]
    .map((match) => match[1].trim())
    .filter(Boolean);
}

function isWindowsSystemDll(name) {
  const normalized = name.toLowerCase();
  return normalized === 'kernel32.dll' ||
    normalized === 'advapi32.dll' ||
    normalized === 'ws2_32.dll' ||
    normalized === 'iphlpapi.dll' ||
    normalized === 'mswsock.dll' ||
    normalized === 'libnode.dll' ||
    normalized.startsWith('api-ms-win-');
}

function validateLinux(dir, arch) {
  const addon = path.join(dir, 'zlink.node');
  const dynamic = readElfDynamic(addon);
  const expectedSoname = `libzlink.so.${coreMajor}`;
  if (!dynamic.includes(`Shared library: [${expectedSoname}]`)) {
    fail(`${addon} must depend on ${expectedSoname}`);
  }
  const linkedSonames = [...dynamic.matchAll(/Shared library: \[(libzlink\.so\.\d+)\]/g)]
    .map((match) => match[1]);
  if (linkedSonames.some((soname) => soname !== expectedSoname)) {
    fail(`${addon} still depends on a stale libzlink SONAME`);
  }
  if (!dynamic.includes('Library runpath: [$ORIGIN]')) {
    fail(`${addon} must use $ORIGIN runpath`);
  }
  const linuxCoreLib = expectedSoname;
  if (!fs.existsSync(path.join(dir, linuxCoreLib))) {
    fail(`${dir} is missing ${linuxCoreLib}`);
  }
  const exactCoreLibrary = `libzlink.so.${coreVersion}`;
  if (!fs.existsSync(path.join(dir, exactCoreLibrary))) {
    fail(`${dir} is missing exact Core runtime ${exactCoreLibrary}`);
  }
  for (const stale of fs.readdirSync(dir)) {
    if (/^libzlink\.so\.\d+\.\d+\.\d+$/.test(stale) && stale !== exactCoreLibrary) {
      fail(`${dir} contains stale ${stale}`);
    }
  }
  for (const stale of [
    'libzlink_c.so',
    'libzlink_c.so.1',
    'libzlink_c.so.1.0.0',
  ]) {
    if (fs.existsSync(path.join(dir, stale))) {
      fail(`${dir} contains stale ${stale}`);
    }
  }
  const description = fileDescription(addon);
  if (arch === 'x64' && !description.includes('x86-64')) {
    fail(`${addon} is not x86-64: ${description}`);
  }
  if (arch === 'arm64' && !description.includes('aarch64')) {
    fail(`${addon} is not aarch64: ${description}`);
  }
}

function validateDarwin(dir, arch) {
  const addon = path.join(dir, 'zlink.node');
  const description = fileDescription(addon);
  if (arch === 'arm64' && !description.includes('arm64')) {
    fail(`${addon} is not arm64: ${description}`);
  }
  if (arch === 'x64' && !description.includes('x86_64')) {
    fail(`${addon} is not x86_64: ${description}`);
  }
}

function validateWindows(dir, arch) {
  const expected = arch === 'arm64' ? 'Aarch64' : arch === 'x64' ? 'x86-64' : null;
  if (!expected) {
    fail(`unsupported Windows prebuild arch: ${arch}`);
  }

  const bundled = new Set(
    fs.readdirSync(dir)
      .filter((name) => name.endsWith('.dll') || name.endsWith('.node'))
      .map((name) => name.toLowerCase())
  );

  for (const name of fs.readdirSync(dir).sort()) {
    if (!name.endsWith('.dll') && !name.endsWith('.node')) continue;
    const target = path.join(dir, name);
    const description = fileDescription(target);
    if (!description.includes(expected)) {
      fail(`${target} is not ${expected}: ${description}`);
    }
    for (const dllName of readPeImports(target)) {
      const normalized = dllName.toLowerCase();
      if (isWindowsSystemDll(normalized)) continue;
      if (!bundled.has(normalized)) {
        fail(`${target} depends on missing bundled DLL ${dllName}`);
      }
    }
  }
}

function validateDir(entry) {
  const dir = path.join(prebuildRoot, entry);
  const addon = path.join(dir, 'zlink.node');
  const [platform, arch] = entry.split('-');
  if (platform === 'linux') {
    validateLinux(dir, arch);
  } else if (platform === 'darwin') {
    validateDarwin(dir, arch);
  } else if (platform === 'win32') {
    validateWindows(dir, arch);
  } else {
    fail(`unsupported prebuild platform directory: ${entry}`);
  }
}

const entries = fs.readdirSync(prebuildRoot)
  .filter((entry) => fs.statSync(path.join(prebuildRoot, entry)).isDirectory())
  .filter((entry) => fs.existsSync(path.join(prebuildRoot, entry, 'zlink.node')))
  .sort();

if (entries.length === 0) {
  fail('no zlink.node prebuilds found');
}

for (const entry of entries) {
  validateDir(entry);
}

console.log(
  `[prebuilds] verified package ${packageVersion} with Core ${coreVersion}: ${entries.join(', ')}`
);

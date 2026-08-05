#!/usr/bin/env node
// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');

function fail(message) {
  process.stderr.write(`${message}\n`);
  process.exit(2);
}

const query = process.argv[2];
if (!['prefix', 'include', 'library', 'version'].includes(query)) {
  fail('Usage: resolve_core.js <prefix|include|library|version>');
}

const configured = process.env.ZLINK_CORE_INSTALL_PREFIX;
if (!configured || !path.isAbsolute(configured)) {
  fail('ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core 11 package prefix');
}

const prefix = fs.realpathSync(configured);
const manifestPath = path.join(prefix, 'share', 'zlink', 'core-package-provenance.json');
if (!fs.existsSync(manifestPath)) {
  fail(`Core package provenance is missing: ${manifestPath}`);
}

const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
if (manifest.package !== 'zlink-core' || !/^11\.\d+\.\d+$/.test(manifest.version || '')) {
  fail(`Installed Core package must have 11.x provenance: ${manifestPath}`);
}

const includeDir = path.join(prefix, 'include');
const header = path.join(includeDir, 'zlink.h');
if (!fs.existsSync(header)) {
  fail(`Installed Core package is missing zlink.h: ${header}`);
}

const libraryNames = process.platform === 'win32'
  ? ['zlink.lib']
  : process.platform === 'darwin'
    ? ['libzlink.dylib']
    : ['libzlink.so'];
const libraryDirs = [path.join(prefix, 'lib'), path.join(prefix, 'lib64')];
let library;
for (const directory of libraryDirs) {
  for (const name of libraryNames) {
    const candidate = path.join(directory, name);
    if (fs.existsSync(candidate)) {
      library = candidate;
      break;
    }
  }
  if (library) break;
}
if (!library) {
  fail(`Installed Core package is missing ${libraryNames.join(' or ')}`);
}

const values = { prefix, include: includeDir, library, version: manifest.version };
process.stdout.write(values[query]);

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

const coreSource = process.env.ZLINK_CORE_SOURCE || 'release';
let prefix;
let includeDir;
let libraryDirs;
let version;

if (coreSource === 'local') {
  const configuredInclude = process.env.ZLINK_CORE_INCLUDE_DIR;
  const configuredLibrary = process.env.ZLINK_CORE_LIB_DIR;
  if (!configuredInclude || !path.isAbsolute(configuredInclude)
      || !configuredLibrary || !path.isAbsolute(configuredLibrary)) {
    fail('ZLINK_CORE_SOURCE=local requires absolute ZLINK_CORE_INCLUDE_DIR and ZLINK_CORE_LIB_DIR; source bindings/tools/local_core_runtime.sh first');
  }
  includeDir = fs.realpathSync(configuredInclude);
  libraryDirs = [fs.realpathSync(configuredLibrary)];
  version = process.env.ZLINK_CORE_VERSION || '';
} else if (coreSource === 'release') {
  const configured = process.env.ZLINK_CORE_INSTALL_PREFIX
    || process.env.ZLINK_CORE_PACKAGE_PREFIX;
  if (!configured || !path.isAbsolute(configured)) {
    fail('ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core 0.13.0 package prefix');
  }

  prefix = fs.realpathSync(configured);
  const manifestPath = path.join(prefix, 'share', 'zlink', 'core-package-provenance.json');
  if (!fs.existsSync(manifestPath)) {
    fail(`Core package provenance is missing: ${manifestPath}`);
  }

  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (manifest.package !== 'zlink-core' || !/^\d+\.\d+\.\d+$/.test(manifest.version || '')
      || Number(manifest.abiMajor) !== 0) {
    fail(`Installed Core package must have 0.13.0 provenance: ${manifestPath}`);
  }
  includeDir = path.join(prefix, 'include');
  libraryDirs = [path.join(prefix, 'lib'), path.join(prefix, 'lib64')];
  version = manifest.version;
} else {
  fail(`ZLINK_CORE_SOURCE must be release or local: ${coreSource}`);
}

const header = path.join(includeDir, 'zlink.h');
if (!fs.existsSync(header)) {
  fail(`Core headers are missing zlink.h: ${header}`);
}

const libraryNames = process.platform === 'win32'
  ? ['zlink.lib']
  : process.platform === 'darwin'
    ? ['libzlink.dylib']
    : ['libzlink.so'];
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
  fail(`Core library is missing ${libraryNames.join(' or ')} from ${libraryDirs.join(' or ')}`);
}

const values = { prefix: prefix || '', include: includeDir, library, version };
process.stdout.write(values[query]);

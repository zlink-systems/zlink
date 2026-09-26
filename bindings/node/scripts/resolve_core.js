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
    fail('ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core 1.9.0 package prefix');
  }

  prefix = fs.realpathSync(configured);
  const manifestPath = path.join(prefix, 'share', 'zlink', 'core-package-provenance.json');
  if (fs.existsSync(manifestPath)) {
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    if (manifest.package !== 'zlink-core' || !/^\d+\.\d+\.\d+$/.test(manifest.version || '')
        || Number(manifest.abiMajor) !== 0) {
      fail(`Installed Core package must have 1.9.0 provenance: ${manifestPath}`);
    }
    version = manifest.version;
  } else {
    // The public GitHub Release archive (libzlink-<platform>.zip/.tar.gz) never
    // contains this manifest -- it is not a build output, it is synthesized
    // locally, after checksum verification against the release's
    // checksums.txt/release-provenance.txt, by
    // scripts/local-package/core/fetch-release.ps1 (Windows) or
    // fetch-release.sh (Linux/macOS/WSL). Those scripts are what every CI job
    // and scripts/local-package/build-windows.ps1 use to turn a release
    // archive into a prefix this script accepts outright.
    //
    // A prefix built by hand from the raw archive (e.g. extracting
    // libzlink-windows-x64.zip and pointing ZLINK_CORE_INSTALL_PREFIX at it)
    // has no such manifest and therefore no verified version/ABI to check.
    // Warn instead of failing closed so a source build from the public
    // archive is still possible; the header and library existence checks
    // below still run unconditionally.
    process.stderr.write(
      `warning: Core package provenance is missing: ${manifestPath}\n` +
      'warning: run scripts/local-package/core/fetch-release.ps1 (or fetch-release.sh) ' +
      'against this release to get a verified prefix; proceeding without version/ABI ' +
      'verification for this build\n'
    );
    version = '';
  }
  includeDir = path.join(prefix, 'include');
  libraryDirs = [path.join(prefix, 'lib'), path.join(prefix, 'lib64')];
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

// gyp evaluates condition values as Python string literals. Normalize every
// path emitted by this script so Windows path separators cannot become escapes.
function gypPath(value) {
  return value.replace(/\\/g, '/');
}

const values = {
  prefix: gypPath(prefix || ''),
  include: gypPath(includeDir),
  library: gypPath(library),
  version,
};
process.stdout.write(values[query]);

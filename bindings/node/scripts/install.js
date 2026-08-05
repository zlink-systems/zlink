// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const packageRoot = path.resolve(__dirname, '..');
const prebuiltAddon = path.join(
  packageRoot,
  'prebuilds',
  `${process.platform}-${process.arch}`,
  'zlink.node'
);

if (process.env.ZLINK_SKIP_NATIVE_INSTALL === '1') {
  process.exit(0);
}

// Published packages carry platform prebuilds. A source checkout may not, so
// retain node-gyp as the development fallback without rebuilding consumers.
if (fs.existsSync(prebuiltAddon)) {
  process.exit(0);
}

const nodeGyp = process.platform === 'win32' ? 'node-gyp.cmd' : 'node-gyp';
const result = spawnSync(nodeGyp, ['rebuild'], {
  cwd: packageRoot,
  stdio: 'inherit',
  shell: process.platform === 'win32'
});

if (result.error) {
  throw result.error;
}
process.exit(result.status === null ? 1 : result.status);

// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
for (const name of ['dist', 'dist-tools']) {
  fs.rmSync(path.join(root, name), { recursive: true, force: true });
}

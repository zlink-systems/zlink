// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const distTools = path.join(root, 'dist-tools');
const link = path.join(distTools, 'dist');
fs.rmSync(link, { recursive: true, force: true });
fs.symlinkSync('..\\dist', link, 'junction');

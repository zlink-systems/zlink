'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const smokeSource = fs.readFileSync(
  path.resolve(__dirname, '../../cross-language/node_dotnet_smoke.js'),
  'utf8'
);

test('dotnet cross-language host uses the official READY stdout contract', () => {
  assert.match(smokeSource, /function waitForReadySignal\(/);
  assert.match(smokeSource, /JSON\.parse\(readyLine\.slice\('READY:'\.length\)\)/);
  assert.doesNotMatch(smokeSource, /waitForReadyFile/);
  assert.doesNotMatch(smokeSource, /'--ready-file'/);
});

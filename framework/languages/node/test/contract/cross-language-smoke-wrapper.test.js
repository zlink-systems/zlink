'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const wrapper = fs.readFileSync(
  path.resolve(__dirname, '../../cross-language/run_cross_language_smoke.sh'),
  'utf8'
);

test('cross-language wrapper prebuilds the TestHost and uses no-build execution', () => {
  assert.match(wrapper, /dotnet build .*Zlink\.Framework\.TestHost\.csproj/);
  assert.match(wrapper, /--framework net8\.0/);
  assert.match(wrapper, /ZLINK_DOTNET_TESTHOST_NO_BUILD=1 node cross-language\/node_dotnet_smoke\.js/);
});

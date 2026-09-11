'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const wrapper = fs.readFileSync(
  path.resolve(__dirname, '../../cross-language/run_cross_language_smoke.sh'),
  'utf8'
);
const smoke = fs.readFileSync(
  path.resolve(__dirname, '../../cross-language/node_dotnet_smoke.js'),
  'utf8'
);

test('cross-language wrapper prebuilds the TestHost and uses no-build execution', () => {
  assert.match(wrapper, /dotnet build .*Zlink\.Framework\.TestHost\.csproj/);
  assert.match(wrapper, /--framework net8\.0/);
  assert.match(wrapper, /ZLINK_DOTNET_TESTHOST_NO_BUILD=1 node cross-language\/node_dotnet_smoke\.js/);
});

test('cross-language Redis rows restore their own .NET test project', () => {
  const functionStart = smoke.indexOf('async function runDotnetRedisProviderTest(');
  const functionEnd = smoke.indexOf('\n}\n', functionStart);
  assert.notEqual(functionStart, -1);
  assert.notEqual(functionEnd, -1);

  const redisTestRunner = smoke.slice(functionStart, functionEnd);
  assert.match(redisTestRunner, /'test',[\s\S]*dotnetRedisTestsProject/);
  assert.doesNotMatch(redisTestRunner, /--no-restore/);
});

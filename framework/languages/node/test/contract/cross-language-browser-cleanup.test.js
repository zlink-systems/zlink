'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const smoke = fs.readFileSync(
  path.resolve(__dirname, '../../cross-language/node_dotnet_smoke.js'),
  'utf8'
);
const driver = fs.readFileSync(
  path.resolve(__dirname, '../../scripts/browser-e2e/connector-driver.mjs'),
  'utf8'
);

test('cross-language browser stage owns host cleanup when driver creation fails', () => {
  assert.match(smoke, /let instance;[\s\S]*instance = await createBrowserConnectorDriver\(\);/);
  assert.match(smoke, /finally \{[\s\S]*await instance\?\.close\(\);[\s\S]*await host\.stop\(\);/);
});

test('browser connector driver bounds partial and normal cleanup', () => {
  assert.match(driver, /closeBrowser, closeContext, closeServer/);
  assert.match(driver, /catch \(error\) \{[\s\S]*await closeContext\(context\);[\s\S]*await closeBrowser\(browser\);[\s\S]*await closeServer\(server\);/);
  assert.doesNotMatch(driver, /new Promise\(\(resolve\) => server\.close\(resolve\)\)/);
});

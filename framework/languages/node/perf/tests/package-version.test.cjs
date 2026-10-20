const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const perfRoot = path.resolve(__dirname, '..');
const frameworkRoot = path.resolve(perfRoot, '../../..');
const expectedVersion = JSON.parse(
  fs.readFileSync(path.join(frameworkRoot, 'perf/schema/packages.json'), 'utf8')
).node;
const manifest = JSON.parse(fs.readFileSync(path.join(perfRoot, 'package.json'), 'utf8'));
const frameworkPackages = Object.keys(manifest.dependencies).filter((name) =>
  name.startsWith('@zlink-systems/')
);

test('Node perf package.json pins match the common runner version', () => {
  assert.ok(frameworkPackages.length > 0);
  for (const name of frameworkPackages) {
    assert.equal(manifest.dependencies[name], expectedVersion, `${name} in package.json`);
  }
});

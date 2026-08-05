const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '../..');

test('Node-executed sample clients do not require the browser-only connector package root', () => {
  const connectorManifest = JSON.parse(fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'stream-connector', 'package.json'),
    'utf8'
  ));
  assert.equal(connectorManifest.exports['.'].require, undefined);

  const sampleRoot = path.join(workspaceRoot, 'samples', 'ZoneWorld');
  const build = spawnSync('npm', ['run', 'build'], {
    cwd: sampleRoot,
    encoding: 'utf8'
  });
  assert.equal(build.status, 0, build.stderr || build.stdout);

  const clientPath = path.join(sampleRoot, 'dist', 'Client', 'main.js');
  const output = fs.readFileSync(clientPath, 'utf8');
  assert.doesNotMatch(output, /require\(["']@zlink-systems\/stream-connector["']\)/);

  const run = spawnSync(process.execPath, [clientPath], {
    cwd: sampleRoot,
    encoding: 'utf8'
  });
  assert.notEqual(run.status, 0);
  assert.doesNotMatch(`${run.stdout}\n${run.stderr}`, /ERR_PACKAGE_PATH_NOT_EXPORTED/);
  assert.match(`${run.stdout}\n${run.stderr}`, /--config <path>/);
});

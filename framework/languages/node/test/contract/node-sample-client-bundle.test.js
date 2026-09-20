const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
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
  const buildRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-sample-client-bundle-'));
  try {
    // The sample build owns its dist cleanup, so compile a disposable copy instead of the shared sample.
    fs.cpSync(sampleRoot, buildRoot, {
      recursive: true,
      filter(source) {
        const relative = path.relative(sampleRoot, source);
        const parts = relative === '' ? [] : relative.split(path.sep);
        return !parts.includes('dist') && !parts.includes('node_modules');
      }
    });
    fs.symlinkSync(
      path.join(workspaceRoot, 'node_modules'),
      path.join(buildRoot, 'node_modules'),
      process.platform === 'win32' ? 'junction' : 'dir'
    );

    const build = spawnSync('npm', ['run', 'build'], {
      cwd: buildRoot,
      encoding: 'utf8'
    });
    assert.equal(build.status, 0, build.stderr || build.stdout);

    const clientPath = path.join(buildRoot, 'dist', 'Client', 'main.js');
    const output = fs.readFileSync(clientPath, 'utf8');
    assert.doesNotMatch(output, /require\(["']@zlink-systems\/stream-connector["']\)/);
    assert.match(output, /globalThis\.WebSocket \?\?= require\(["']undici["']\)\.WebSocket/);

    const run = spawnSync(process.execPath, [clientPath], {
      cwd: buildRoot,
      encoding: 'utf8'
    });
    assert.notEqual(run.status, 0);
    assert.doesNotMatch(`${run.stdout}\n${run.stderr}`, /ERR_PACKAGE_PATH_NOT_EXPORTED/);
    assert.match(`${run.stdout}\n${run.stderr}`, /--config <path>/);
  } finally {
    fs.rmSync(buildRoot, { recursive: true, force: true });
  }
});

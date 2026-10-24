'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

test('V8 trace records completion owner allocations and an allocating control', (t) => {
  const result = spawnSync(
    process.execPath,
    ['--no-opt', '--expose-gc', path.join(__dirname, 'fixtures/typed-request-allocation-trace.js')],
    { encoding: 'utf8', env: { ...process.env, NODE_TEST_CONTEXT: undefined } }
  );
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  const metrics = JSON.parse(result.stdout.trim());
  assert.equal(metrics.count, 100);
  assert.ok(metrics.control.objects >= metrics.count, 'allocating control is observed');
  assert.ok(metrics.total.objects > metrics.count, 'request allocations are observed');
  assert.ok(metrics.total.bytes > 0, 'request bytes are observed');
  // Compare object counts with closures allocated in this same V8 run. The
  // three completion-owner modules have a 23-container budget per request;
  // periodic registry traversal and native observation are reported separately.
  const observer = metrics.modules['/runtime/backend/node/node-raw-binding-port.js'];
  const completionObjects = metrics.total.objects - observer.objects;
  assert.ok(observer.objects >= metrics.count, 'binding observer container is observed');
  assert.ok(completionObjects >= metrics.count, 'completion owner is observed');
  assert.ok(completionObjects <= 23 * metrics.control.objects,
    `${completionObjects} completion objects exceed ${23 * metrics.control.objects} control containers`);
  t.diagnostic(JSON.stringify(metrics));
});

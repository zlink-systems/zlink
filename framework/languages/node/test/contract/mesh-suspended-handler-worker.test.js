'use strict';

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

test('two suspended Spot handlers return both application workers for another Spot', {
  skip: process.platform !== 'linux' && 'CPU affinity requires Linux'
}, (t) => {
  const allowed = fs.readFileSync('/proc/self/status', 'utf8')
    .match(/^Cpus_allowed_list:\s*(.+)$/m)[1];
  const cpus = allowed.split(',').flatMap(range => {
    const [first, last = first] = range.split('-').map(Number);
    return Array.from({ length: last - first + 1 }, (_, index) => first + index);
  }).slice(0, 2);
  const child = spawnSync('taskset', ['-c', cpus.join(','), process.execPath,
    path.join(__dirname, 'fixtures/mesh-suspended-handler-worker.js')], {
    encoding: 'utf8', timeout: 15000
  });
  assert.ifError(child.error);
  t.diagnostic(child.stdout.trim());
  assert.equal(child.signal, null, child.stdout + child.stderr);
  assert.equal(child.status, 0, child.stdout + child.stderr);
  assert.match(child.stdout, /application-workers=2/);
  assert.match(child.stdout, /two-responses/);
});

const assert = require('node:assert/strict');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

function allocationTrace(mode) {
  const child = spawnSync(process.execPath, [
    // Disable VM inline caches so cache LoadHandlers do not count as JS captures.
    '--no-opt', '--no-use-ic', '--expose-gc', path.join(__dirname, 'receive-allocation-trace.js'), mode
  ], { encoding: 'utf8' });
  assert.equal(child.status, 0, child.stderr || child.stdout);
  return JSON.parse(child.stdout);
}

module.exports = { allocationTrace };

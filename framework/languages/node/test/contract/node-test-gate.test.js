const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { once } = require('node:events');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const lockModule = path.join(workspaceRoot, 'scripts', 'node-test-gate-lock.js');

test('node runtime and coverage gates isolate native test handles and concurrent runs', () => {
  for (const scriptName of ['run_node_runtime_gate.js', 'run_node_coverage_gate.js']) {
    const source = fs.readFileSync(path.join(workspaceRoot, 'scripts', scriptName), 'utf8');
    assert.match(source, /acquireNodeTestGateLock/);
    assert.match(source, /--test-force-exit/);
  }
});

test('node test gate lock rejects a live owner and releases after owner exit', async () => {
  const lockRoot = path.join(workspaceRoot, `.lock-contract-${process.pid}-${Date.now()}`);
  const holder = childProcess.spawn(process.execPath, [
    '-e',
    lockScript('holder', "process.stdout.write('ready\\n'); setInterval(() => {}, 1000);")
  ], {
    cwd: workspaceRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });

  try {
    await once(holder.stdout, 'data');
    const blocked = runLockProcess(lockRoot, 'contender');
    assert.notEqual(blocked.status, 0);
    assert.match(blocked.stderr, /already running/);
  } finally {
    holder.kill('SIGTERM');
    await once(holder, 'exit');
  }

  const reacquired = runLockProcess(lockRoot, 'successor');
  assert.equal(reacquired.status, 0, reacquired.stderr);

  function lockScript(gateName, afterAcquire = '') {
    return [
      `const { acquireNodeTestGateLock } = require(${JSON.stringify(lockModule)});`,
      `const release = acquireNodeTestGateLock(${JSON.stringify(lockRoot)}, ${JSON.stringify(gateName)});`,
      afterAcquire,
      afterAcquire.length === 0 ? 'release();' : ''
    ].join('\n');
  }

  function runLockProcess(root, gateName) {
    return childProcess.spawnSync(process.execPath, [
      '-e',
      [
        `const { acquireNodeTestGateLock } = require(${JSON.stringify(lockModule)});`,
        `const release = acquireNodeTestGateLock(${JSON.stringify(root)}, ${JSON.stringify(gateName)});`,
        'release();'
      ].join('\n')
    ], {
      cwd: workspaceRoot,
      encoding: 'utf8'
    });
  }
});

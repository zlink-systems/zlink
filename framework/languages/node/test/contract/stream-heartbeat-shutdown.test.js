const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const test = require('node:test');

const shutdownTimeoutMs = 5000;

test('SIGINT runs while heartbeat admission is pending in the real binding', async () => {
  const child = fork(path.join(__dirname, 'fixtures/stream-heartbeat-shutdown-process.js'), {
    silent: true
  });
  let stderr = '';
  child.stderr.on('data', (chunk) => {
    stderr += chunk;
  });
  const exited = once(child, 'close');
  let watchdogError;
  const watchdog = setTimeout(() => {
    watchdogError = new Error(`heartbeat shutdown did not complete within ${shutdownTimeoutMs}ms`);
    child.kill('SIGKILL');
  }, shutdownTimeoutMs);
  const messages = [];
  child.on('message', (message) => {
    messages.push(message);
    if (message.type === 'pending') {
      child.send({ type: 'raise-sigint' });
    }
  });
  try {
    const [code, signal] = await exited;
    assert.equal(watchdogError, undefined, stderr);
    assert.equal(signal, null, stderr);
    assert.equal(code, 0, stderr);
    assert.deepEqual(
      messages.map((message) => message.type),
      ['pending', 'closed'],
      stderr
    );
    assert.equal(messages[1].heartbeatCompleted, true, stderr);
  } finally {
    clearTimeout(watchdog);
    if (child.exitCode === null && child.signalCode === null) {
      child.kill('SIGKILL');
      await exited;
    }
  }
});

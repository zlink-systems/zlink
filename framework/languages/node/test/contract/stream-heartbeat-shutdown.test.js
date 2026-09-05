const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const test = require('node:test');

test('SIGINT runs while heartbeat admission is pending in the real binding', async () => {
  const child = fork(path.join(__dirname, 'fixtures/stream-heartbeat-shutdown-process.js'), {
    silent: true
  });
  let stderr = '';
  child.stderr.on('data', chunk => { stderr += chunk; });
  const exited = once(child, 'exit');
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 5000);
  const messages = [];
  child.on('message', message => {
    messages.push(message);
    if (message.type === 'pending') child.kill('SIGINT');
  });
  try {
    const [code, signal] = await exited;
    assert.equal(signal, null, stderr);
    assert.equal(code, 0, stderr);
    assert.deepEqual(messages.map(message => message.type), ['pending', 'closed']);
    assert.equal(messages[1].heartbeatCompleted, true);
  } finally {
    clearTimeout(watchdog);
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
});

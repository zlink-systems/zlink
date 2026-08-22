const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');

const fixture = path.join(__dirname, 'fixtures', 'bound-session-relocation-process.js');

test(
  'two processes retain the post-Join completion until exact command 44 route publication',
  { timeout: 30_000 },
  async (t) => {
    const port = await reservePort();
    const children = [];
    t.after(async () => await Promise.all(children.map(stopChild)));
    const session = startChild(children, 'session', port);
    const sessionReady = await session.ready;
    const target = startChild(children, 'target', port);
    const targetReady = await target.ready;
    assert.notEqual(sessionReady.pid, targetReady.pid);

    const sealed = await target.command('relocate');
    assert.equal(sealed.phase, 'sealed');
    assert.deepEqual(await session.command('status'), {
      writes: 0,
      command42: 1,
      command44: 0
    });
    const completion = await target.command('sendCompletion');
    assert.equal(
      completion.status,
      'submitted',
      'retained completion push has an immediate success terminal'
    );
    assert.deepEqual(
      await session.command('status'),
      { writes: 0, command42: 1, command44: 0 },
      'completion remains retained while the exact Session seal is active'
    );

    assert.deepEqual(await target.command('commit'), {
      phase: 'committed',
      actorNodeRid: 'target'
    });
    assert.deepEqual(await session.command('status'), {
      writes: 1,
      command42: 1,
      command44: 1
    });
  }
);

function startChild(children, role, port) {
  const child = fork(fixture, [], {
    cwd: path.resolve(__dirname, '../..'),
    env: {
      ...process.env,
      ZLINK_TEST_ROLE: role,
      ZLINK_TEST_PORT: String(port)
    },
    stdio: ['ignore', 'pipe', 'pipe', 'ipc']
  });
  children.push(child);
  let stderr = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', chunk => { stderr += chunk; });
  const pending = new Map();
  let nextId = 1;
  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${role} readiness timed out.\n${stderr}`)), 10_000);
    child.on('message', message => {
      if (message?.type === 'ready') {
        clearTimeout(timer);
        resolve(message);
      } else if (message?.type === 'fatal') {
        clearTimeout(timer);
        reject(new Error(message.message));
      }
    });
  });
  child.on('message', message => {
    if ((message?.type !== 'result' && message?.type !== 'error') || !pending.has(message.id)) return;
    const operation = pending.get(message.id);
    pending.delete(message.id);
    clearTimeout(operation.timer);
    if (message.type === 'error') operation.reject(new Error(message.message));
    else operation.resolve(message.value);
  });
  child.once('exit', (code, signal) => {
    const error = new Error(`${role} exited: code=${code} signal=${signal}\n${stderr}`);
    for (const operation of pending.values()) {
      clearTimeout(operation.timer);
      operation.reject(error);
    }
    pending.clear();
  });
  return {
    child,
    ready,
    command(action) {
      const id = nextId++;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          pending.delete(id);
          reject(new Error(`${role} command '${action}' timed out.\n${stderr}`));
        }, 10_000);
        pending.set(id, { resolve, reject, timer });
        child.send({ id, action });
      });
    }
  };
}

async function stopChild(entry) {
  const child = entry.child ?? entry;
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = new Promise(resolve => child.once('exit', resolve));
  if (child.connected) child.send({ type: 'stop' });
  else child.kill('SIGTERM');
  const timer = setTimeout(() => child.kill('SIGKILL'), 3_000);
  await exited;
  clearTimeout(timer);
}

async function reservePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

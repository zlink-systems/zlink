// Contract test for the Unity WebGL UPM adapter's jslib boundary.
//
// Unity is not part of this repository's CI, so the C# compile and the WebGL
// player build are verified by hand (the package README lists what to check).
// Everything below the C# type system is verified here: the jslib functions are
// loaded into an emscripten stand-in and driven the way the P/Invoke stubs drive
// them, against the same STREAM server the browser e2e uses.
const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');
const { createHarness } = require('./helpers/unity-webgl-jslib-harness');
const { JslibConnector, MAX_EVENTS_PER_PUMP } = require('./helpers/unity-webgl-jslib-client');

const workspaceRoot = path.resolve(__dirname, '../..');
const serverScript = path.join(workspaceRoot, 'test/browser/support/stream-server.js');

const decoder = new TextDecoder();
const encoder = new TextEncoder();

test('jslib boundary rejects endpoints the browser sandbox cannot open', () => {
  const harness = createHarness();
  for (const endpoint of ['tcp://127.0.0.1:19000', 'tls://127.0.0.1:19000']) {
    assert.throws(
      () => new JslibConnector(harness, JSON.stringify({ endpoint })),
      (error) => error.code === 'configurationError',
      `${endpoint} must fail as a configuration error`
    );
  }

  // The failure is a create-time configuration error, so nothing is allocated
  // and no handle is handed out.
  assert.equal(harness.allocatedBlocks(), 0);
});

test('jslib boundary drives a real STREAM server over ws', { timeout: 120_000 }, async (t) => {
  const port = await freePort();
  const server = await startStreamServer(`ws://127.0.0.1:${port}`);
  const harness = createHarness();
  let connector;
  t.after(async () => {
    try {
      connector?.destroy();
    } finally {
      await stopStreamServer(server);
    }
  });

  connector = new JslibConnector(harness, JSON.stringify({
    endpoint: `ws://127.0.0.1:${port}`,
    dispatchMode: 'manual',
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  }));

  // Spec 32 section 2.2 and section 6.1: manual dispatch is the game-engine default and
  // the connector starts in Created.
  assert.equal(connector.state, 0);
  assert.equal(connector.diagnosticsLevel, 1);

  await connector.connect();
  assert.equal(connector.isConnected, true);
  assert.equal(connector.state, 2);

  const reply = await connector.request(
    encoder.encode(JSON.stringify({ value: 'jslib-request' })),
    { codec: 1, packetName: 'EchoReq', metadata: { tenant: 'unity' }, timeoutMs: 10_000 }
  );
  assert.equal(JSON.parse(decoder.decode(reply.payload)).value, 'jslib-request');

  // Spec 32 section 10.1.1: with no handler registered, the push waits in the unread
  // queue and the wait surface consumes it directly - no Dispatch in between.
  const observed = await connector.waitFor(
    'EchoPush',
    (message) => JSON.parse(decoder.decode(message.payload.payload)).value === 'jslib-request',
    10_000
  );
  assert.equal(observed.name, 'EchoPush');
  assert.equal(observed.payload.codec, 1);
  assert.equal(connector.receivedCount('EchoPush'), 0);

  // A registered handler takes the message instead, and must not run before Dispatch.
  const pushed = [];
  connector.on('EchoPush', (message) => {
    pushed.push(JSON.parse(decoder.decode(message.payload.payload)).value);
  });

  await connector.send(
    encoder.encode(JSON.stringify({ value: 'jslib-send' })),
    { codec: 1, packetName: 'EchoReq' }
  );
  // Advancing the transport is not dispatching: the jslib pump moves the push
  // across the boundary, and the registered handler still waits for Dispatch.
  await waitUntil(
    () => connector.dispatchQueue.some((item) => item.kind === 'message'),
    () => { connector.startAdvanceIfIdle(); connector.pumpAndTransfer(); }
  );
  assert.deepEqual(pushed, [], 'manual dispatch must hold the handler until Dispatch runs');
  await connector.dispatch();
  assert.deepEqual(pushed, ['jslib-send']);
  assert.equal(connector.pendingDispatchCount, 0);

  await connector.close();
  assert.equal(connector.isConnected, false);
  assert.equal(connector.state, 5);

  await connector.dispatch();
  assert.ok(
    connector.stateChanges.some((change) => change.current === 'closed'),
    `connection state changes reached C#: ${JSON.stringify(connector.stateChanges)}`
  );
  assert.ok(connector.disconnects.length >= 1);
});

test('the pump never re-enters the event sink and frees every boundary buffer', { timeout: 120_000 }, async (t) => {
  const port = await freePort();
  const server = await startStreamServer(`ws://127.0.0.1:${port}`);
  const harness = createHarness();
  let connector;
  t.after(async () => {
    try {
      connector?.destroy();
    } finally {
      await stopStreamServer(server);
    }
  });

  connector = new JslibConnector(harness, JSON.stringify({
    endpoint: `ws://127.0.0.1:${port}`,
    dispatchMode: 'manual',
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  }));
  await connector.connect();

  const reply = await connector.request(
    encoder.encode(JSON.stringify({ value: 'reentrancy' })),
    { codec: 1, packetName: 'EchoReq', timeoutMs: 10_000 }
  );
  assert.equal(JSON.parse(decoder.decode(reply.payload)).value, 'reentrancy');

  // The sink calls ZlinkStreamPump again on every event it receives, which is the
  // worst case: user code re-entering the boundary from inside the callback. Every
  // such nested call must be refused rather than dispatching the same event twice.
  assert.ok(connector.nestedPumpResults.length > 0, 'the sink ran at least once');
  assert.deepEqual(
    [...new Set(connector.nestedPumpResults)],
    [-1],
    'a nested pump must return -1 and dispatch nothing'
  );

  await connector.close();
  await connector.dispatch();
  connector.destroy();
  connector = undefined;

  // Every text and payload buffer the pump allocated is freed when the sink
  // returns, and the last-error string is freed by the C# side.
  assert.equal(harness.allocatedBlocks(), 0);
});

function waitUntil(predicate, tick) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + 15_000;
    const poll = () => {
      tick();
      if (predicate()) return resolve();
      if (Date.now() > deadline) return reject(new Error('condition was not met'));
      setTimeout(poll, 5);
      return undefined;
    };

    poll();
  });
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close((error) => (error ? reject(error) : resolve(port)));
    });
  });
}

function startStreamServer(endpoint) {
  const child = childProcess.spawn(process.execPath, [serverScript, '--endpoint', endpoint], {
    cwd: workspaceRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Stream server start timeout: ${output}`)), 30_000);
    const check = () => {
      if (!output.includes('"event":"ready"')) return;
      clearTimeout(timer);
      child.stdout.off('data', check);
      resolve(child);
    };

    child.stdout.on('data', check);
    child.once('exit', (code) => {
      if (!output.includes('"event":"ready"')) {
        clearTimeout(timer);
        reject(new Error(`Stream server exited ${code}: ${output}`));
      }
    });
  });
}

function stopStreamServer(child) {
  if (!child || child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    child.once('exit', () => resolve());
    child.kill('SIGTERM');
    setTimeout(() => {
      if (child.exitCode === null) child.kill('SIGKILL');
    }, 5_000).unref();
  });
}

assert.ok(MAX_EVENTS_PER_PUMP > 0);

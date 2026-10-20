const assert = require('node:assert/strict');
const test = require('node:test');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { deferred, fixture, closeDuringHello } = require('./fixtures/raw-mesh-close');
const {
  ZLinkRuntimeTaskRunner,
  ZLinkRuntimeTaskErrorSink
} = require('../../packages/framework/dist/runtime/execution');

// Hold the pump at its Promise boundary, without timers or native calls in flight.
test('close joins the active pump before releasing its runtime, including repeated close', async () => {
  const f = fixture();
  const entered = deferred();
  const release = deferred();
  let rounds = 0;
  f.runtime.pumpBatch = async () => {
    rounds++;
    entered.resolve();
    await release.promise;
    return false;
  };
  f.readable(true, false);
  await entered.promise;
  const closing = f.backend.close();
  const repeated = f.backend.close();
  try {
    assert.equal(f.closed(), false, 'runtime closed while pump was still active');
    f.readable(true, true);
    assert.equal(rounds, 1, 'close admitted another pump');
  } finally {
    release.resolve();
    await Promise.all([closing, repeated]);
  }
  assert.deepEqual(f.events, ['host.close']);
});

test('route observation completes before close releases the native host', async (t) => {
  let now = 0;
  t.mock.method(performance, 'now', () => now);
  const events = await closeDuringHello((target) => {
    if (target === 'peer-b') now = 5000;
  });
  assert.deepEqual(events, [
    'submit:peer-a',
    'close:start',
    'submit:peer-b',
    'submit:peer-a',
    'submit:peer-b',
    'host.close',
    'close:complete'
  ]);
});

test('the standalone route observation reproduction exits without an unhandled rejection', async () => {
  const child = spawn(
    process.execPath,
    ['--unhandled-rejections=strict', path.join(__dirname, 'fixtures/raw-mesh-close.js')],
    { stdio: ['ignore', 'pipe', 'pipe'] }
  );
  let output = '';
  child.stdout.on('data', (chunk) => {
    output += chunk;
  });
  child.stderr.on('data', (chunk) => {
    output += chunk;
  });
  const [code, signal] = await once(child, 'exit');
  assert.equal(signal, null, output);
  assert.equal(code, 0, output);
});

test('announcePeer after runtime close declines the announcement', async () => {
  const f = fixture();
  try {
    f.setRoutes([{ routingId: 'peer-a', routeGeneration: 1n }]);
    await f.runtime.observeSelectedRoutes();
    await f.backend.close();
    assert.equal(await f.runtime.announcePeer('peer-a'), false);
  } finally {
    await f.backend.close();
  }
});

test('pump failures reach the existing infrastructure task error owner once', async () => {
  const sink = new ZLinkRuntimeTaskErrorSink();
  const reported = deferred();
  const failures = [];
  sink.onRuntimeTaskException((failure) => {
    failures.push(failure);
    reported.resolve();
  });
  const f = fixture(new ZLinkRuntimeTaskRunner(sink, new AbortController().signal));
  const cause = new Error('pump failed');
  f.runtime.pumpBatch = async () => {
    throw cause;
  };
  try {
    f.readable(false, true);
    await reported.promise;
    await f.backend.close();
    assert.equal(failures.length, 1);
    assert.equal(failures[0].error, cause);
  } finally {
    await f.backend.close();
  }
});

test('pending control admission settles after close without touching the released router', async () => {
  const f = fixture();
  const admitted = deferred();
  const submitted = deferred();
  f.onSubmit = () => {
    submitted.resolve();
    return { result: SubmitResult.Backpressured, admitted: admitted.promise };
  };
  let sendTask;
  try {
    f.setRoutes([{ routingId: 'peer-a', routeGeneration: 1n }]);
    f.readable(false, true);
    await submitted.promise;
    sendTask = [...f.runtime.controlSends.values()][0].task;
    assert.ok(sendTask);
    await f.backend.close();
    assert.equal(f.closed(), true);
  } finally {
    admitted.resolve();
    await sendTask;
    await f.backend.close();
  }
  assert.deepEqual(f.events, ['submit:peer-a', 'host.close']);
});

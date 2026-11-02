const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: nextTurn } = require('node:timers/promises');

process.env.NODE_ENV = 'test';
const { RoutingId } = require('@zlink-systems/zlink');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkAutoConnectLoop
} = require('../../packages/framework/dist/runtime/locations/auto-connect-loop');

const renewTimeoutMs = 25;
const stored = {
  status: internal.ZLinkLocationWriteStatus.Stored,
  generation: 1n,
  updatedAt: new Date(0)
};

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

function fixture() {
  const row = {
    autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'publication',
    role: internal.ZLinkLocationRole.Router,
    nodeRid: RoutingId.from('local'),
    endpoint: 'tcp://127.0.0.1:3569',
    ownerId: 'owner',
    generation: 0n,
    updatedAt: new Date(0),
    weight: 100,
    value: 0n,
    metadata: {}
  };
  const requests = [], snapshots = [], events = [], scheduled = [], order = [];
  const entered = deferred();
  const state = { hold: true, rows: [row] };
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = new internal.ZLinkLocationRuntime({
    ownerId: 'owner',
    options: { ownerLeaseRenewTimeoutMs: renewTimeoutMs },
    stores: {
      locationStore: store, spotStore: store, actorStore: store,
      routeStore: store, ownerLeaseStore: store,
      peerStore: {
        updatePeer(_row, intent, signal) {
          const completion = deferred();
          requests.push({ ...completion, intent, signal });
          order.push(`write:${requests.length}`);
          entered.resolve();
          if (!state.hold) completion.resolve(stored);
          // Deliberately ignore signal, including its timeout/cancellation.
          return completion.promise;
        },
        async removePeer() { return stored; }
      }
    },
    events: { peerRowUpdated() { events.push('published'); }, peerRowRemoved() {} }
  });
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: row, localRow: row, runtime,
    peerResolver: {
      async listLivePeers() {
        snapshots.push(state.rows);
        order.push(`snapshot:${snapshots.length}`);
        return state.rows;
      }
    },
    executor: {
      async connect() { assert.fail('local snapshots have no remote targets'); },
      disconnect() {},
      expectPeers() { order.push('reconciled'); }
    },
    errorSink: { reportRuntimeTaskException(_task, error) { assert.fail(String(error)); } }
  });
  const loop = new ZLinkAutoConnectLoop({
    local: row, reconciler,
    options: { ownerLeaseRenewTimeoutMs: renewTimeoutMs },
    setTimer(callback) { scheduled.push(callback); return callback; },
    clearTimer(handle) { scheduled.splice(scheduled.indexOf(handle), 1); }
  });
  return { ...state, state, entered, requests, snapshots, events, scheduled, order, reconciler, loop };
}

test('publication yields the event loop and queued snapshots keep their order', async () => {
  const f = fixture();
  const starting = f.loop.start();
  await f.entered.promise;
  const periodic = f.loop.tick();
  await nextTurn();
  assert.deepEqual(f.order, ['write:1'], 'no snapshot precedes publication confirmation');
  f.requests[0].resolve(stored);
  await Promise.all([starting, periodic]);
  await f.loop.requestPeerReconcile('publication', 'peer');
  assert.deepEqual(f.order, [
    'write:1', 'snapshot:1', 'reconciled',
    'snapshot:2', 'reconciled', 'snapshot:3', 'reconciled'
  ]);
  await f.loop.stop();
});

test('unconfirmed publication times out and the next round republishes', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const f = fixture();
  let completed = false;
  const tick = f.reconciler.tick().then(() => { completed = true; });
  await f.entered.promise;
  try {
    t.mock.timers.tick(renewTimeoutMs - 1);
    await nextTurn();
    assert.equal(completed, false);
    t.mock.timers.tick(1);
    await nextTurn();
    assert.equal(completed, true, 'publication must end at the renew timeout');
    await tick;
    assert.equal(f.requests[0].signal.aborted, true);
    assert.equal(f.reconciler.storeFailed, true);
    assert.equal(f.reconciler.localPublicationReady, false);
    assert.equal(f.snapshots.length, 0);

    f.state.hold = false;
    await f.reconciler.tick();
    assert.equal(f.requests.length, 2);
    assert.equal(f.reconciler.storeFailed, false);
    assert.equal(f.reconciler.localPublicationReady, true);
    assert.equal(f.snapshots.length, 1);
    f.requests[0].resolve(stored);
    await nextTurn();
    assert.deepEqual(f.events, ['published'], 'expired writes have no late publication event');
    assert.equal(f.requests.length, 2);
  } finally {
    for (const request of f.requests) request.resolve(stored);
    await tick;
  }
});

test('stop cancels startup publication without waiting for an unresponsive Store', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const f = fixture();
  const starting = f.loop.start();
  await f.entered.promise;
  let stopped = false;
  const stopping = f.loop.stop().then(() => { stopped = true; });
  try {
    await nextTurn();
    assert.equal(f.requests[0].signal?.aborted, true, 'stop reaches the initial publication');
    t.mock.timers.tick(renewTimeoutMs);
    await nextTurn();
    assert.equal(stopped, true, 'stop must finish before the Store responds');
    await Promise.all([starting, stopping]);
    f.requests[0].resolve(stored);
    await nextTurn();
    assert.equal(f.requests.length, 1);
    assert.equal(f.snapshots.length, 0);
    assert.deepEqual(f.events, []);
    assert.deepEqual(f.scheduled, [], 'late startup completion cannot restart the loop');
  } finally {
    for (const request of f.requests) request.resolve(stored);
    await Promise.allSettled([starting, stopping]);
    await f.loop.stop();
  }
});

function lifecycleTimerFixture() {
  const ticks = [];
  const tickWaiters = [];
  const timers = new Set();
  const reconciler = {
    localPublicationReady: true,
    storeFailed: false,
    tick() {
      const completion = deferred();
      ticks.push(completion);
      tickWaiters.shift()?.resolve(completion);
      return completion.promise;
    },
    disconnectPeers() {},
    async unpublishLocal() {}
  };
  const loop = new ZLinkAutoConnectLoop({
    local: { meshName: 'publication' },
    reconciler,
    options: { pollingIntervalMs: 1 },
    setTimer(callback) {
      const timer = { callback };
      timers.add(timer);
      return timer;
    },
    clearTimer(timer) { timers.delete(timer); }
  });
  return {
    loop,
    ticks,
    timers,
    waitForTick() {
      const entered = deferred();
      tickWaiters.push(entered);
      return entered.promise;
    }
  };
}

test('one polling timer remains when startup ticks overlap across stop and start', async () => {
  const f = lifecycleTimerFixture();
  const tick1Started = f.waitForTick();
  const starting1 = f.loop.start();
  const tick1 = await tick1Started;

  const tick2Started = f.waitForTick();
  const stopping1 = f.loop.stop();
  const starting2 = f.loop.start();
  tick1.resolve();
  const tick2 = await tick2Started;
  tick2.resolve();
  await Promise.all([starting1, starting2, stopping1]);

  assert.equal(f.timers.size, 1, 'only the current lifecycle may arm a polling timer');
  await f.loop.stop();
  assert.equal(f.timers.size, 0, 'stop clears the current lifecycle timer');
});

test('one polling timer remains when a periodic tick overlaps across stop and start', async () => {
  const f = lifecycleTimerFixture();
  const tick1Started = f.waitForTick();
  const starting1 = f.loop.start();
  const tick1 = await tick1Started;
  tick1.resolve();
  await starting1;
  assert.equal(f.timers.size, 1);

  const [timer] = f.timers;
  f.timers.delete(timer);
  const tick2Started = f.waitForTick();
  timer.callback();
  const tick2 = await tick2Started;

  const tick3Started = f.waitForTick();
  const stopping1 = f.loop.stop();
  const starting2 = f.loop.start();
  tick2.resolve();
  const tick3 = await tick3Started;
  tick3.resolve();
  await Promise.all([starting2, stopping1]);
  await nextTurn();

  assert.equal(f.timers.size, 1, 'only the current lifecycle may arm a polling timer');
  await f.loop.stop();
  assert.equal(f.timers.size, 0, 'stop clears the current lifecycle timer');
});

for (const lateStatus of [
  internal.ZLinkLocationWriteStatus.Stored,
  internal.ZLinkLocationWriteStatus.RejectedConflict
]) {
  test(`stop cancels recovery republish and ignores late status ${lateStatus}`, async t => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const f = fixture();
    f.state.hold = false;
    await f.loop.start();
    f.state.hold = true;
    f.state.rows = [];
    const caller = new AbortController();
    const running = f.loop.tick(caller.signal);
    await nextTurn();
    assert.equal(f.requests.length, 2, 'missing local descriptor starts recovery publication');
    const queued = f.loop.requestPeerReconcile('publication', 'queued');
    let stopped = false;
    const stopping = f.loop.stop().then(() => { stopped = true; });
    try {
      await nextTurn();
      assert.equal(f.requests[1].signal?.aborted, true, 'caller signal cannot hide loop stop');
      assert.equal(caller.signal.aborted, false);
      t.mock.timers.tick(renewTimeoutMs);
      await nextTurn();
      assert.equal(stopped, true, 'stop is independent of provider cooperation');
      await Promise.all([running, queued, stopping]);
      const orderAtStop = [...f.order];
      f.requests[1].resolve({ ...stored, status: lateStatus });
      await nextTurn();
      await f.loop.requestPeerReconcile('publication', 'after-stop');
      assert.deepEqual(f.order, orderAtStop, 'no late snapshot or renewal write');
      assert.equal(f.requests.length, 2);
      assert.equal(f.snapshots.length, 2);
      assert.equal(f.reconciler.localPublicationReady, false);
      assert.deepEqual(f.events, ['published']);
      assert.deepEqual(f.scheduled, []);
    } finally {
      f.state.hold = false;
      for (const request of f.requests) request.resolve(stored);
      await Promise.allSettled([running, queued, stopping]);
      await f.loop.stop();
    }
  });
}

'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { ZLinkChannelReceiveLoop, ZLinkReceiveRoundRobinCoordinator } = require('../../packages/framework/dist/runtime/channels/channel-receive-loops');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');

function queueWithLimit(limit) {
  return new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: BigInt(limit) }, () => 1n));
}

test('one readiness reserves one count for two ordinary claims', async t => {
  t.mock.method(performance, 'now', () => 0);
  const queue = queueWithLimit(64);
  const reservations = [];
  const reserve = queue.reserveSupply.bind(queue);
  t.mock.method(queue, 'reserveSupply', (count, ...args) => { reservations.push(count); return reserve(count, ...args); });
  let claims = 0;
  let beforeSecond;
  let closed = 0;
  let ready = true;
  const loop = new ZLinkChannelReceiveLoop('rotation', {
    recv() {
      claims++;
      if (claims === 2) beforeSecond = [...reservations];
      return claims <= 2 ? { parts: [], close() { closed++; } } : undefined;
    }
  }, { flowEnabled: () => false, dispatch: async () => {} }, undefined, undefined,
  { wait: () => ready, markDrained() { ready = false; }, waitForReadable: async () => false, dispose() {} }, queue);
  try {
    await loop.run();
  } finally {
    await loop.stop();
  }
  assert.equal(beforeSecond.length, 1, 'reservation calls before second claim');
  assert.deepEqual(beforeSecond, [64]);
  assert.equal(closed, 2);
  assert.equal(queue.snapshot().reservedSupplyPermits, 0n);
  assert.equal(queue.snapshot().queuedApplicationJobs, 0n);
});

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

function fixture(queue, actions, options = {}) {
  let ready = true;
  const claims = [];
  const closes = [];
  const errors = [];
  const loop = new ZLinkChannelReceiveLoop('rotation', {
    recv() {
      const action = actions.shift();
      claims.push(action);
      options.claim?.(action);
      if (action instanceof Error) throw action;
      return action === undefined ? undefined : { parts: [], kind: action, close() { closes.push(action); } };
    }
  }, { flowEnabled: () => false, dispatch: options.dispatch ?? (async () => {}) }, undefined,
  options.infrastructure,
  { wait: () => ready, markDrained() { ready = false; }, waitForReadable: async () => false, dispose() {} },
  queue, error => errors.push(error), options.roundRobin);
  return { loop, claims, closes, errors };
}

function unusedReturns(t, queue) {
  const counts = [];
  const release = queue.releaseUnusedReceiveRotation.bind(queue);
  t.mock.method(queue, 'releaseUnusedReceiveRotation', count => {
    if (count > 0) counts.push(count);
    release(count);
  });
  return counts;
}

function empty(queue) {
  assert.equal(queue.snapshot().permitsInUse, 0n);
  assert.equal(queue.snapshot().capacityWaiters, 0n);
}

test('zero permits waits once without claiming; independent completion and stop still progress', async () => {
  const queue = queueWithLimit(1);
  const held = await queue.acquire(undefined, 'local');
  const f = fixture(queue, ['ordinary']);
  const running = f.loop.run();
  assert.equal(queue.snapshot().capacityWaiters, 1n);
  assert.equal(f.claims.length, 0);
  const completions = new ZLinkMeshCompletionTable();
  const operationId = { high: 1n, low: 1n };
  const completion = completions.submit(() => operationId);
  completions.complete({ operationId, terminalResult: 0, parts: [] });
  assert.equal((await completion).terminalResult, 0);
  assert.equal(completions.pendingCount, 0);
  completions.dispose();
  assert.equal(f.claims.length, 0);
  await f.loop.stop();
  await running;
  held.releaseAfterInternalProcessing();
  empty(queue);
});

test('saturated owners wait for capacity without repeated round-robin attempts', async t => {
  const queue = queueWithLimit(1);
  const held = await queue.acquire(undefined, 'local');
  const roundRobin = new ZLinkReceiveRoundRobinCoordinator();
  const attempts = new Map();
  const tryAcquire = roundRobin.tryAcquire.bind(roundRobin);
  t.mock.method(roundRobin, 'tryAcquire', owner => {
    attempts.set(owner, (attempts.get(owner) ?? 0) + 1);
    return tryAcquire(owner);
  });
  const owners = [fixture(queue, ['first'], { roundRobin }), fixture(queue, ['second'], { roundRobin })];
  const running = owners.map(owner => owner.loop.run());
  try {
    for (let turn = 0; turn < 4; turn++) await new Promise(resolve => setImmediate(resolve));
    const before = [...attempts.values()];
    for (let turn = 0; turn < 4; turn++) await new Promise(resolve => setImmediate(resolve));
    const after = [...attempts.values()];
    t.diagnostic(JSON.stringify({ turns: [4, 8], before, after, capacityWaiters: String(queue.snapshot().capacityWaiters), claims: owners.map(owner => owner.claims.length) }));
    assert.deepEqual(after, before, 'capacity wait must suspend round-robin attempts');
    assert.deepEqual(after, [], 'no owner acquires a receive turn before capacity');
    assert.equal(queue.snapshot().capacityWaiters, 2n);
    assert.deepEqual(owners.map(owner => owner.claims.length), [0, 0]);
  } finally {
    await Promise.all(owners.map(owner => owner.loop.stop()));
    await Promise.all(running);
    held.releaseAfterInternalProcessing();
  }
  empty(queue);
});

test('denied receive turn returns its reserved count before yielding', async t => {
  const queue = queueWithLimit(64);
  const counts = unusedReturns(t, queue);
  const roundRobin = new ZLinkReceiveRoundRobinCoordinator();
  const active = roundRobin.register();
  roundRobin.setReady(active, true);
  assert.equal(roundRobin.tryAcquire(active), true);
  const f = fixture(queue, [], { roundRobin });
  const running = f.loop.run();
  try {
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(counts, [64]);
    assert.equal(queue.snapshot().reservedSupplyPermits, 0n);
    assert.equal(f.claims.length, 0);
    roundRobin.setReady(active, false);
    roundRobin.release(active);
    await running;
    assert.deepEqual(counts, [64, 63]);
    assert.equal(f.claims.length, 1, 'the owner proceeds after the active turn is released');
  } finally {
    await f.loop.stop();
    await running;
    roundRobin.unregister(active);
  }
  empty(queue);
});

test('one remaining permit claims only one ordinary record before another reservation', async t => {
  t.mock.method(performance, 'now', () => 0);
  const queue = queueWithLimit(2);
  const held = await queue.acquire(undefined, 'local');
  const reservations = [];
  const reserve = queue.reserveSupply.bind(queue);
  t.mock.method(queue, 'reserveSupply', (count, ...args) => { reservations.push(count); return reserve(count, ...args); });
  const claims = [];
  const f = fixture(queue, ['first', 'second'], { claim(action) { if (action) claims.push([...reservations]); } });
  await f.loop.run();
  await f.loop.stop();
  assert.deepEqual(claims, [[1], [1, 1]]);
  held.releaseAfterInternalProcessing();
  empty(queue);
});

test('rotation waiter shares the single FIFO with other sources', async () => {
  const queue = queueWithLimit(4);
  const held = await Promise.all(Array.from({ length: 4 }, () => queue.acquire(undefined, 'remote')));
  const order = [];
  const first = queue.acquire(undefined, 'local').then(permit => { order.push('first'); return permit; });
  const rotation = queue.createReceiveRotation(64, 'remote');
  const batch = rotation.acquire(new AbortController().signal).then(() => { order.push('rotation'); });
  const last = queue.acquire(undefined, 'local').then(permit => { order.push('last'); return permit; });
  assert.equal(queue.snapshot().capacityWaiters, 3n);
  held[0].releaseAfterInternalProcessing();
  const firstLease = await first;
  assert.equal(queue.snapshot().capacityWaiters, 2n);
  held[1].releaseAfterInternalProcessing();
  await batch;
  assert.equal(queue.snapshot().capacityWaiters, 1n);
  held[2].releaseAfterInternalProcessing();
  const lastLease = await last;
  const leases = [];
  for (let lease; (lease = rotation.take()) !== undefined;) leases.push(lease);
  assert.equal(leases.length, 1, 'grant is min(requested, available) when FIFO head is served');
  assert.deepEqual(order, ['first', 'rotation', 'last']);
  firstLease.releaseAfterInternalProcessing();
  lastLease.releaseAfterInternalProcessing();
  held[3].releaseAfterInternalProcessing();
  leases.forEach(permit => permit.releaseAfterInternalProcessing());
  rotation.releaseUnused();
  empty(queue);
});

for (const [name, actions, returned] of [
  ['no_data', [], 63],
  ['recv error', [new Error('recv failure')], 63]
]) {
  test(`${name} returns the unused count exactly once`, async t => {
    const queue = queueWithLimit(64);
    const counts = unusedReturns(t, queue);
    const f = fixture(queue, [...actions]);
    if (name === 'recv error') await assert.rejects(f.loop.run(), /recv failure/);
    else await f.loop.run();
    await f.loop.stop();
    assert.deepEqual(counts, [returned]);
    empty(queue);
  });
}

test('classification failure reports once and returns unused count', async t => {
  t.mock.method(performance, 'now', () => 0);
  const queue = queueWithLimit(64);
  const counts = unusedReturns(t, queue);
  const f = fixture(queue, ['bad'], { infrastructure() { throw new Error('classification failure'); } });
  await f.loop.run();
  await f.loop.stop();
  assert.deepEqual(counts, [62]);
  assert.deepEqual(f.closes, ['bad']);
  assert.equal(f.errors.length, 1);
  assert.match(f.errors[0].message, /classification failure/);
  empty(queue);
});

test('close after reservation returns all unused count without a claim', async t => {
  const queue = queueWithLimit(64);
  const counts = unusedReturns(t, queue);
  const stop = new AbortController();
  const acquire = queue.acquireCountOrResume.bind(queue);
  t.mock.method(queue, 'acquireCountOrResume', (...args) => {
    const count = acquire(...args);
    stop.abort();
    return count;
  });
  const f = fixture(queue, ['unclaimed']);
  await f.loop.run(stop.signal);
  await f.loop.stop();
  assert.equal(f.claims.length, 0);
  assert.deepEqual(counts, [64]);
  empty(queue);
});

test('close during claim returns its lease and remaining count once', async t => {
  const queue = queueWithLimit(64);
  const counts = unusedReturns(t, queue);
  const stop = new AbortController();
  const f = fixture(queue, ['closing'], { claim() { stop.abort(); } });
  await f.loop.run(stop.signal);
  await f.loop.stop();
  assert.deepEqual(f.closes, ['closing']);
  assert.deepEqual(counts, [63]);
  empty(queue);
});

test('claimed control returns its permit before blocked application dispatch completes', async t => {
  t.mock.method(performance, 'now', () => 0);
  const queue = queueWithLimit(2);
  const entered = deferred();
  const release = deferred();
  const control = deferred();
  const f = fixture(queue, ['application', 'control'], {
    dispatch: async () => { entered.resolve(); await release.promise; },
    infrastructure(received) {
      if (received.kind !== 'control') return false;
      control.resolve();
      return true;
    }
  });
  const running = f.loop.run();
  try {
    await entered.promise;
    await control.promise;
    assert.deepEqual(f.closes, ['control']);
    assert.equal(queue.snapshot().queuedApplicationJobs, 1n);
  } finally {
    release.resolve();
    await running;
    await f.loop.stop();
  }
  empty(queue);
});

test('FIFO batch head receives a count and unused credit reaches the next source', async () => {
  const queue = queueWithLimit(4);
  const holder = queue.createReceiveRotation(4, 'remote');
  holder.reserve();
  const next = queue.createReceiveRotation(64, 'remote');
  const granted = next.acquire(new AbortController().signal);
  const tail = queue.acquire(undefined, 'local');
  assert.equal(queue.snapshot().capacityWaiters, 2n);
  holder.releaseUnused();
  await granted;
  assert.equal(queue.snapshot().reservedSupplyPermits, 4n);
  assert.equal(queue.snapshot().capacityWaiters, 1n);
  const retained = next.take();
  next.releaseUnused();
  const tailPermit = await tail;
  assert.equal(queue.snapshot().permitsInUse, 2n);
  retained.releaseAfterInternalProcessing();
  tailPermit.releaseAfterInternalProcessing();
  empty(queue);
});

'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { fixture, deferred } = require('./fixtures/raw-mesh-close');

for (const count of [0, 1, 64]) {
  test(`synchronous receive batch of ${count} records has no Promise boundary`, async t => {
    const f = fixture();
    await f.backend.pumpTask;
    t.mock.method(performance, 'now', () => 0);
    let receives = 0, ticks = 0;
    f.runtime.receiveOne = (_now, observe) => {
      if (receives++ >= count) return 'noData';
      observe('peer', 1024);
      return 'application';
    };
    f.runtime.tickLiveness = () => ticks++;
    try {
      const batch = f.runtime.pumpBatch(count !== 0);
      const result = await batch;
      assert.equal(result, count === 64);
      assert.equal(receives, count === 0 ? 0 : count === 64 ? 64 : count + 1);
      assert.equal(ticks, 1);
      assert.equal(typeof batch, 'boolean', 'completed receive work must return directly');
    } finally { await f.backend.close(); }
  });
}

test('synchronous batches retain the installed pump task across readiness reentry', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  let owned, rounds = 0;
  f.runtime.pumpBatch = () => {
    owned ??= f.backend.pumpTask;
    assert.ok(owned instanceof Promise, 'join owner is installed before receiving');
    assert.equal(f.backend.pumpTask, owned);
    if (++rounds === 1) f.readable(true, false);
    return false;
  };
  try {
    f.readable(true, false);
    assert.equal(rounds, 2, 'completed rotations stay in the readiness turn');
    await owned;
    assert.equal(rounds, 2);
    assert.equal(f.backend.pumpTask, undefined);
  } finally { await f.backend.close(); }
});

test('suspended receive keeps its rotation until completion and close joins it', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  const entered = deferred(), release = deferred();
  const events = [];
  f.runtime.receiveRotation = {
    reserve() { events.push('reserve'); },
    releaseUnused() { events.push('release'); }
  };
  f.runtime.receiveOne = (_now, observe) => {
    entered.resolve();
    return release.promise.then(() => { observe('peer', 4); return 'application'; });
  };
  try {
    f.readable(true, false);
    await entered.promise;
    const closing = f.backend.close();
    assert.deepEqual(events, ['reserve']);
    assert.equal(f.closed(), false);
    release.resolve();
    await closing;
    assert.deepEqual(events, ['reserve', 'release']);
    assert.equal(f.closed(), true);
  } finally { release.resolve(); await f.backend.close(); }
});

for (const asynchronous of [false, true]) {
  test(`receive failure releases its rotation once (asynchronous=${asynchronous})`, async () => {
    const f = fixture();
    await f.backend.pumpTask;
    const cause = new Error('receive failed');
    let releases = 0, ticks = 0;
    f.runtime.receiveRotation = {
      reserve() {},
      releaseUnused() { releases++; }
    };
    f.runtime.tickLiveness = () => ticks++;
    f.runtime.receiveOne = () => {
      if (asynchronous) return Promise.reject(cause);
      throw cause;
    };
    try {
      await assert.rejects(Promise.resolve().then(() => f.runtime.pumpBatch(true)), error => error === cause);
      assert.equal(releases, 1);
      assert.equal(ticks, 0);
    } finally { await f.backend.close(); }
  });
}

test('suspended receives keep the byte budget and release before maintenance', async t => {
  const f = fixture();
  await f.backend.pumpTask;
  t.mock.method(performance, 'now', () => 0);
  const events = [];
  let receives = 0;
  f.runtime.receiveRotation = {
    reserve() { events.push('reserve'); },
    releaseUnused() { events.push('release'); }
  };
  f.runtime.tickLiveness = () => events.push('tick');
  f.runtime.receiveOne = (_now, observe) => {
    assert.ok(++receives <= 2, 'byte budget must survive the asynchronous boundary');
    observe('peer', 2 * 1024 * 1024);
    return receives === 1 ? Promise.resolve('application') : 'application';
  };
  try {
    assert.equal(await f.runtime.pumpBatch(true), true);
    assert.equal(receives, 2);
    assert.deepEqual(events, ['reserve', 'release', 'tick']);
  } finally { await f.backend.close(); }
});

test('route observation finishes before receive and maintenance', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  const routes = deferred();
  const events = [];
  f.runtime.observeSelectedRoutes = () => { events.push('route'); return routes.promise; };
  f.runtime.receiveOne = () => { events.push('receive'); return 'noData'; };
  f.runtime.tickLiveness = () => events.push('tick');
  try {
    const round = f.runtime.pumpBatch(true, true);
    assert.deepEqual(events, ['route']);
    routes.resolve();
    assert.equal(await round, false);
    assert.deepEqual(events, ['route', 'receive', 'tick']);
  } finally { routes.resolve(); await f.backend.close(); }
});

test('receive time budget ends the rotation without waiting for wall time', async t => {
  const { RAW_MESH_RECEIVE_TIME_BUDGET_MS } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
  const f = fixture();
  await f.backend.pumpTask;
  let clock = 0, receives = 0, ticks = 0;
  t.mock.method(performance, 'now', () => clock);
  f.runtime.receiveOne = (_now, observe) => {
    receives++;
    observe('peer', 4);
    clock = RAW_MESH_RECEIVE_TIME_BUDGET_MS;
    return 'application';
  };
  f.runtime.tickLiveness = () => ticks++;
  try {
    assert.equal(await f.runtime.pumpBatch(true), true);
    assert.equal(receives, 1);
    assert.equal(ticks, 1);
  } finally { await f.backend.close(); }
});

test('readiness posted by a completed receive remains observable', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  const observed = deferred();
  let rounds = 0;
  f.runtime.pumpBatch = () => {
    if (++rounds === 1) queueMicrotask(() => f.readable(true, false));
    else observed.resolve();
    return false;
  };
  try {
    f.readable(true, false);
    await observed.promise;
    await f.backend.pumpTask;
    assert.equal(rounds, 2);
  } finally { await f.backend.close(); }
});

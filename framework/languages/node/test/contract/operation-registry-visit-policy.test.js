const {
  RequestRuntime
} = require('../../packages/framework/dist/runtime/foundation/request-runtime');
('use strict');
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  OperationRegistry
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  waitRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-deadline');
const ROTATION = 256;

test('an empty index schedules no management visit after reply or cancellation', async () => {
  let now = 0;
  const registry = new OperationRegistry({ now: () => now });
  let visits = 0;
  const runUntil = (end) => {
    while (registry.nextDeadlineMs() <= end) {
      now = registry.nextDeadlineMs();
      visits++;
      registry.expire(now, now + 1);
    }
    now = end;
  };
  runUntil(ROTATION);
  const replied = registry.register(10);
  registry.complete(replied.id, 'reply');
  await replied.promise;
  assert.equal(registry.nextDeadlineMs(), Infinity);
  runUntil(2 * ROTATION);
  const cancelled = registry.register(10);
  const terminal = cancelled.promise.catch(() => {});
  registry.cancel(cancelled.id);
  await terminal;
  assert.equal(registry.nextDeadlineMs(), Infinity);
  runUntil(3 * ROTATION);
  assert.equal(visits, 0);
  registry.close();
});

test('distant entries schedule exactly one visit per rotation without a second deadline index', async (t) => {
  let now = 100;
  const registry = new OperationRegistry({ now: () => now });
  const pending = Array.from({ length: 1000 }, (_, i) => registry.register((10 + i) * ROTATION));
  const visits = [];
  try {
    for (let rotation = 1; rotation <= 6; rotation++) {
      assert.equal(registry.nextDeadlineMs(), 100 + rotation * ROTATION);
      now = registry.nextDeadlineMs();
      visits.push(now);
      assert.equal(registry.expire(now, now + 1), 0);
      assert.equal(registry.size, 1000);
    }
    assert.equal(visits.length, 6);
    t.diagnostic(`1000 distant entries: visits=${visits.length}, elapsed rotations=6`);
    const later = registry.register(2 * ROTATION);
    pending.push(later);
    assert.equal(
      registry.nextDeadlineMs(),
      now + ROTATION,
      'another distant entry cannot advance the rotation'
    );
    const earlier = registry.register(7);
    assert.equal(registry.nextDeadlineMs(), now + 7);
    now += 6;
    assert.equal(registry.expire(now, now + 1), 0);
    assert.equal(registry.isPending(earlier.id), true);
    now++;
    const timedOut = earlier.promise.catch((error) => error);
    assert.equal(registry.expire(now, now + 1), 1);
    await timedOut;
  } finally {
    registry.close();
    await Promise.all(pending.map((operation) => operation.promise.catch(() => {})));
  }
});

test('standalone wait preserves one empty reservation and registration restarts visits', async (t) => {
  t.mock.timers.enable({ apis: ['Date', 'setTimeout'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  const expire = OperationRegistry.prototype.expire;
  let visits = 0;
  t.mock.method(OperationRegistry.prototype, 'expire', function (...args) {
    visits++;
    return expire.apply(this, args);
  });
  const reserved = t.mock.method(globalThis, 'setTimeout');
  const cleared = t.mock.method(globalThis, 'clearTimeout');
  const owner = new RequestRuntime();
  assert.equal(
    await owner.request(10, undefined, (entry) =>
      waitRequestReply(Promise.resolve('reply'), 'idle', entry)
    ),
    'reply'
  );
  // Submit and completion §10.1, PR #2889: do not cancel an already reserved visit.
  assert.equal(reserved.mock.callCount(), 1);
  assert.equal(
    cleared.mock.calls.filter((call) => call.arguments[0] === reserved.mock.calls[0].result).length,
    0
  );
  t.mock.timers.tick(ROTATION);
  assert.equal(visits, 1);
  t.mock.timers.tick(ROTATION);
  assert.equal(visits, 1);
  const pending = owner.request(10 * ROTATION, undefined, (entry) =>
    waitRequestReply(new Promise(() => {}), 'distant', entry)
  );
  const controllerResult = pending.catch(() => {});
  for (let rotation = 1; rotation <= 3; rotation++) {
    t.mock.timers.tick(ROTATION);
    assert.equal(visits, rotation + 1);
  }
  t.mock.timers.tick(7 * ROTATION);
  await controllerResult;
  owner.close();
});

test('registration after idle preserves the last actual visit anchor', async () => {
  let now = 0;
  const registry = new OperationRegistry({ now: () => now });
  const first = registry.register(10 * ROTATION);
  now = ROTATION;
  registry.expire(now, now + 1);
  registry.complete(first.id, 'reply');
  await first.promise;
  now = 4 * ROTATION;
  const next = registry.register(10 * ROTATION);
  try {
    assert.equal(registry.nextDeadlineMs(), 2 * ROTATION);
    registry.expire(now, now + 1);
    assert.equal(registry.nextDeadlineMs(), now + ROTATION);
  } finally {
    registry.close();
    await next.promise.catch(() => {});
  }
});

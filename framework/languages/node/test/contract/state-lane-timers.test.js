const assert = require('node:assert/strict');
const { AsyncResource, AsyncLocalStorage } = require('node:async_hooks');
const test = require('node:test');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const { ZLinkManagedTimer } = require('../../packages/framework/dist/runtime/spots/spot-timer');
const { ZLinkTimerOverrunPolicy } = require('../../packages/framework/dist/contracts');

// Node mock timers invoke callbacks in the tick caller's context. Capture the
// registration context explicitly to reproduce real Node timers without waiting.
function contextTimers() {
  const pending = [];
  let now = 0;
  return {
    now: () => now,
    utcNow: () => now,
    setTimeout(callback, delay) {
      const handle = { callback: AsyncResource.bind(callback), delay, unref() {} };
      pending.push(handle);
      return handle;
    },
    clearTimeout(handle) {
      const index = pending.indexOf(handle);
      if (index !== -1) pending.splice(index, 1);
    },
    fire() {
      const handle = pending.shift();
      assert.ok(handle, 'a timer must have been registered');
      now += handle.delay;
      return handle.callback();
    },
    get count() {
      return pending.length;
    }
  };
}

function sealRequest() {
  return {
    relocation: { high: 1n, low: 2n },
    coordinator: {
      nodeRid: 'source',
      nodeGeneration: 2n,
      ownerId: 'owner',
      leaseGeneration: 1n,
      expectedAuthorityStoreVersion: 'v1'
    },
    senderRole: 'source',
    actor: {
      actor: { nodeRid: 'source', actorId: 'absent-actor', generation: 3n },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 4n,
      ownerLeaseGeneration: 5n
    },
    session: {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 7n,
      sessionOwnerId: 'session-owner-id',
      sessionOwnerLeaseGeneration: 8n,
      sessionRid: 'session',
      bindingGeneration: 6n
    }
  };
}

test('bindingAbsent expiry detaches its registration context and submits one lane turn', async (t) => {
  const timers = contextTimers();
  t.mock.method(global, 'setTimeout', timers.setTimeout);
  const registry = new ZLinkActorSessionBindingRegistry();
  const request = sealRequest();
  const first = await registry.prepareSessionRelocationSeal(request);
  assert.equal(first.response.sealResult, 'bindingAbsent');
  assert.strictEqual(await registry.prepareSessionRelocationSeal(request), first);
  assert.equal(timers.count, 1);
  const submissions = t.mock.method(registry.lane, 'run');
  assert.doesNotThrow(() => timers.fire());
  assert.equal(submissions.mock.callCount(), 1, 'expiry is submitted exactly once');
  assert.notStrictEqual(
    await registry.prepareSessionRelocationSeal(request),
    first,
    'expiry must remove the retained result before the next lookup'
  );
  assert.equal(timers.count, 1);
  timers.fire();
  await registry.lane.dispose();
});

for (const kind of ['synchronous admission', 'asynchronous work']) {
  test(`bindingAbsent expiry reports ${kind} failure to its runtime owner`, async (t) => {
    const timers = contextTimers();
    t.mock.method(global, 'setTimeout', timers.setTimeout);
    let report;
    const reported = new Promise((resolve) => {
      report = resolve;
    });
    const registry = new ZLinkActorSessionBindingRegistry(undefined, undefined, () => ({
      reportRuntimeTaskException: (taskName, error) => report({ taskName, error })
    }));
    await registry.prepareSessionRelocationSeal(sealRequest());
    const failure = new Error(`${kind} expiry failure`);
    if (kind === 'synchronous admission') await registry.lane.dispose();
    else
      t.mock.method(registry.absentSessionRelocationResults, 'delete', () => {
        throw failure;
      });
    assert.doesNotThrow(() => timers.fire());
    const result = await reported;
    assert.match(result.taskName, /session relocation.*expir/u);
    if (kind === 'synchronous admission')
      assert.match(result.error.message, /state lane is closed/u);
    else assert.strictEqual(result.error, failure);
    await registry.lane.dispose();
  });
}

test('rescheduled Spot timer observes dispatch failure outside the state lane', async () => {
  const clock = contextTimers();
  const failure = new Error('timer failure reporter rejected');
  let ticks = 0;
  const timer = new ZLinkManagedTimer(
    'context-timer',
    10,
    {
      overrunPolicy: ZLinkTimerOverrunPolicy.DelayNextTick,
      maxCatchUpTicks: 1,
      stopOnUnhandledException: false
    },
    async () => {
      if (++ticks === 2) throw new Error('second tick failed');
    },
    async () => {
      throw failure;
    },
    undefined,
    clock
  );
  try {
    clock.fire();
    await timer.running;
    assert.equal(ticks, 1);
    assert.equal(clock.count, 1, 'first completion must schedule the second tick');
    clock.fire();
    await assert.doesNotReject(timer.running);
    await assert.rejects(timer.cancel(), (error) => error === failure);
    assert.equal(ticks, 2);
    assert.equal(clock.count, 0);
  } finally {
    await timer.cancel().catch(() => undefined);
  }
});

test('serial queue callback submitted by a state lane can return to that lane', async () => {
  const { ZLinkStateLane } = require('../../packages/framework/dist/runtime/execution/state-lane');
  const {
    ZLinkSerialExecutionQueue
  } = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
  const lane = new ZLinkStateLane();
  const queue = new ZLinkSerialExecutionQueue(async (record) => {
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
    }
  });
  let completion;
  await lane.run(() => {
    completion = queue.submit(() => lane.run(() => 42));
  });
  try {
    assert.equal(await completion, 42);
  } finally {
    await queue.whenIdle();
    await lane.dispose();
  }
});

// Serial callbacks may still belong to an Actor/Spot owner or a trace. Only
// the state-lane marker is detached; those independent contexts must survive.
test('state lane detachment preserves unrelated execution context and restores the lane', async () => {
  const {
    ZLinkStateLane,
    startOutsideStateLane
  } = require('../../packages/framework/dist/runtime/execution/state-lane');
  const owner = new AsyncLocalStorage();
  const lane = new ZLinkStateLane();
  const timers = contextTimers();
  const token = {};
  await owner.run(token, () =>
    lane.run(() => {
      startOutsideStateLane(() => {
        assert.equal(ZLinkStateLane.current, undefined);
        assert.strictEqual(owner.getStore(), token);
        timers.setTimeout(() => {
          assert.equal(ZLinkStateLane.current, undefined);
          assert.strictEqual(owner.getStore(), token);
        }, 1);
      });
      assert.strictEqual(ZLinkStateLane.current, lane);
      assert.throws(() => lane.run(() => undefined), /already runs on the state lane/u);
    })
  );
  timers.fire();
  await lane.dispose();
});

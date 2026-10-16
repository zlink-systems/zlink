const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkManagedTimer,
  ZLinkSpotTimerRegistry
} = require('../../packages/framework/dist/runtime/spots/spot-timer');
const {
  ZLinkSpotSerialExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-executor');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkSpotActivation
} = require('../../packages/framework/dist/runtime/spots/spot-activation-state');
const {
  ZLinkSpotRelocationCoordinationMode,
  ZLinkTimerOverrunPolicy,
  ZLinkUserSpotExecutionMode
} = require('../../packages/framework/dist/contracts');
const {
  decodeServiceRelocationEnvelope,
  encodeServiceRelocationEnvelope
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

// Host relocation §12 and Spot timer §1/§2: a Spot timer relocation moves the
// logical registration, the next due time and the pending tick. A pending tick
// is a tick the timer selected whose callback has not started, for example one
// that waits for execution-barrier admission while the source is sealed. The
// relocated target must deliver exactly the tick sequence (DeliveryIndex,
// ScheduledIndex, SkippedTicks, ScheduledAt) that the source would have
// delivered had it continued (the aborted-relocation baseline).

const PERIOD_MS = 100;
const SOURCE_UNIX_ORIGIN = 1_800_000_000_000;
const TARGET_MONOTONIC_ORIGIN = 5_000_000;

function createClock(monotonicOrigin, unixAtZero) {
  let elapsed = 0;
  const scheduled = [];
  return {
    now: () => monotonicOrigin + elapsed,
    utcNow: () => unixAtZero + elapsed,
    setTimeout(callback, delay) {
      const handle = { callback, due: elapsed + (Number(delay) || 0), cleared: false };
      scheduled.push(handle);
      return handle;
    },
    clearTimeout(handle) {
      handle.cleared = true;
    },
    get elapsed() {
      return elapsed;
    },
    get live() {
      return scheduled.filter((handle) => !handle.cleared).length;
    },
    advance(ms) {
      elapsed += ms;
    },
    // Fires the earliest due callback and moves the monotonic clock to it.
    fireNext() {
      const live = scheduled.filter((handle) => !handle.cleared);
      assert.ok(live.length > 0, 'expected a scheduled timer callback');
      const handle = live.reduce((first, next) => (next.due < first.due ? next : first));
      scheduled.splice(scheduled.indexOf(handle), 1);
      elapsed = Math.max(elapsed, handle.due);
      handle.callback();
    }
  };
}

async function settle() {
  for (let index = 0; index < 20; index++) await new Promise((resolve) => setImmediate(resolve));
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((complete, fail) => {
    resolve = complete;
    reject = fail;
  });
  promise.catch(() => undefined);
  return { promise, resolve, reject };
}

function observed(tick) {
  return {
    deliveryIndex: tick.deliveryIndex,
    scheduledIndex: tick.scheduledIndex,
    skippedTicks: tick.skippedTicks,
    scheduledAt: tick.scheduledAt.getTime()
  };
}

/**
 * One timer incarnation. The first callback runs past two periods so the
 * overrun policy decides the second tick; the second tick waits for admission
 * (the source execution barrier is sealed) and has not started at capture.
 */
function createTimer(name, options, clock, delivered, admission, cursor) {
  let started = false;
  let fires = 0;
  const timer = new ZLinkManagedTimer(
    name,
    PERIOD_MS,
    options,
    async (tick) => {
      started = false;
      fires += 1;
      if (admission !== undefined && fires === 2) await admission.promise;
      started = true;
      delivered.push(observed(tick));
      if (tick.deliveryIndex === 1n) clock.advance(2 * PERIOD_MS + 50);
    },
    undefined,
    () => started,
    clock,
    cursor === undefined ? {} : { paused: true, cursor }
  );
  return timer;
}

/** Runs the source until its second tick is pending, then captures it. */
async function captureWithPendingTick(options, pending = true) {
  const clock = createClock(0, SOURCE_UNIX_ORIGIN);
  const delivered = [];
  const admission = pending ? deferred() : undefined;
  const timer = createTimer('heartbeat', options, clock, delivered, admission);
  clock.fireNext();
  await settle();
  clock.fireNext();
  await settle();
  const state = await timer.captureRelocation('HeartbeatHandler');
  // Time passes while the Spot is frozen; the next due tick expires meanwhile.
  clock.advance(3 * PERIOD_MS + 30);
  return { clock, delivered, admission, timer, state };
}

/**
 * Carries a captured timer through relocation-envelope-v1 the way the host
 * relocation runtime maps it on the source and on the target.
 */
function throughWire(state) {
  const encoded = encodeServiceRelocationEnvelope(
    {
      aggregateId: '11111111-1111-4111-8111-111111111111',
      aggregateGeneration: 1n,
      memberships: [],
      participants: [
        {
          key: 'spot:room',
          objectKind: 'user_spot',
          stableType: 'room',
          objectGeneration: 3n,
          authorityOwnerGeneration: 2n,
          applicationState: Buffer.from('state'),
          boundSessionState: Buffer.alloc(0),
          queuedMessages: [],
          timers: [
            {
              timerId: state.name,
              handlerType: state.handlerType,
              startedAtUnixMs: state.startedAtUnixMs,
              dueAtUnixMs: state.nextDueAtUnixMs,
              intervalMs: state.periodMs,
              deliveryIndex: state.deliveryIndex,
              lastScheduledIndex: state.lastScheduledIndex,
              overrunPolicy: state.overrunPolicy,
              maxCatchUpTicks: state.maxCatchUpTicks,
              stopOnUnhandledException: state.stopOnUnhandledException,
              pendingTicks: state.pendingTicks
            }
          ]
        }
      ]
    },
    1n
  );
  const [value] = decodeServiceRelocationEnvelope(encoded, 1n).participants[0].timers;
  return {
    name: value.timerId,
    handlerType: value.handlerType,
    periodMs: value.intervalMs,
    overrunPolicy: value.overrunPolicy,
    maxCatchUpTicks: value.maxCatchUpTicks,
    stopOnUnhandledException: value.stopOnUnhandledException,
    startedAtUnixMs: value.startedAtUnixMs,
    deliveryIndex: value.deliveryIndex,
    lastScheduledIndex: value.lastScheduledIndex,
    nextDueAtUnixMs: value.dueAtUnixMs,
    pendingTicks: value.pendingTicks
  };
}

const FOLLOWING_TICKS = 6;

async function continuedOnSource(options, pending = true) {
  const source = await captureWithPendingTick(options, pending);
  source.timer.resume();
  source.admission?.resolve();
  await settle();
  for (let index = 0; index < FOLLOWING_TICKS; index++) {
    assert.equal(source.clock.live, 1, 'the source keeps exactly one scheduled tick');
    source.clock.fireNext();
    await settle();
  }
  await source.timer.dispose();
  return source.delivered;
}

async function relocated(options, pending = true) {
  const source = await captureWithPendingTick(options, pending);
  // Commit: the barrier rejects the pending admission and the source is disposed.
  source.admission?.reject(new Error('execution barrier committed'));
  await source.timer.dispose().catch(() => undefined);

  const clock = createClock(TARGET_MONOTONIC_ORIGIN, SOURCE_UNIX_ORIGIN + source.clock.elapsed);
  const delivered = [...source.delivered];
  const restored = throughWire(source.state);
  assert.equal(restored.startedAtUnixMs, source.state.startedAtUnixMs);
  const target = createTimer('heartbeat', options, clock, delivered, undefined, restored);
  assert.equal(clock.live, 0, 'a restored timer does not run before Ready');
  target.resume();
  if (pending) {
    // Ready: the restored pending tick is due at once and runs before any new tick.
    assert.equal(clock.live, 1);
    const readyAt = clock.elapsed;
    clock.fireNext();
    assert.equal(clock.elapsed, readyAt);
    await settle();
  }
  for (let index = 0; index < FOLLOWING_TICKS; index++) {
    assert.equal(clock.live, 1, 'the target keeps exactly one scheduled tick');
    clock.fireNext();
    await settle();
  }
  await target.dispose();
  return { delivered, state: source.state };
}

function assertTickContract(delivered) {
  delivered.forEach((tick, index) => {
    assert.equal(tick.deliveryIndex, BigInt(index + 1), 'DeliveryIndex grows by exactly one');
    const previous = index === 0 ? 0n : delivered[index - 1].scheduledIndex;
    assert.ok(tick.scheduledIndex > previous, 'ScheduledIndex increases');
    assert.equal(tick.skippedTicks, tick.scheduledIndex - previous - 1n);
    assert.equal(tick.scheduledAt, SOURCE_UNIX_ORIGIN + Number(tick.scheduledIndex) * PERIOD_MS);
  });
}

const policies = [
  ['SkipLateTicks', { overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks, maxCatchUpTicks: 1 }],
  ['DelayNextTick', { overrunPolicy: ZLinkTimerOverrunPolicy.DelayNextTick, maxCatchUpTicks: 1 }],
  ...[1, 2, 5].map((max) => [
    `CatchUpBounded maxCatchUpTicks=${max}`,
    { overrunPolicy: ZLinkTimerOverrunPolicy.CatchUpBounded, maxCatchUpTicks: max }
  ])
];

for (const [label, policy] of policies) {
  const options = { ...policy, stopOnUnhandledException: false };

  test(`${label}: relocation captures the pending tick and the target delivers the source sequence`, async () => {
    const baseline = await continuedOnSource(options);
    const moved = await relocated(options);

    assert.equal(moved.state.pendingTicks.length, 1, 'the selected, unstarted tick is pending');
    const [pending] = moved.state.pendingTicks;
    assert.equal(moved.state.deliveryIndex, 1n, 'the payload carries the completed delivery index');
    assert.equal(moved.state.lastScheduledIndex, 1n);
    assert.deepEqual(
      {
        deliveryIndex: pending.deliveryIndex,
        scheduledIndex: pending.scheduledIndex,
        skippedTicks: pending.skippedTicks,
        scheduledAt: pending.scheduledAtUnixMs
      },
      baseline[1],
      'the pending tick is the tick the source delivers when it continues'
    );

    assert.equal(baseline.length, 2 + FOLLOWING_TICKS);
    assertTickContract(baseline);
    assert.deepEqual(moved.delivered, baseline);
  });

  test(`${label}: relocation without a pending tick recomputes the source sequence`, async () => {
    const baseline = await continuedOnSource(options, false);
    const moved = await relocated(options, false);

    assert.deepEqual(moved.state.pendingTicks, []);
    assert.equal(moved.state.deliveryIndex, 2n);
    assert.equal(baseline.length, 2 + FOLLOWING_TICKS);
    assertTickContract(baseline);
    assert.deepEqual(moved.delivered, baseline);
  });
}

test('the second tick reflects each overrun policy', async () => {
  const second = async (policy) =>
    (await continuedOnSource({ ...policy, stopOnUnhandledException: false }))[1];
  const pick = (tick) => [tick.scheduledIndex, tick.skippedTicks];
  // The first callback ends at 350 ms, so ticks 2..3 are already due.
  assert.deepEqual(pick(await second(policies[0][1])), [3n, 1n]);
  assert.deepEqual(pick(await second(policies[1][1])), [2n, 0n]);
  assert.deepEqual(pick(await second(policies[2][1])), [3n, 1n]);
  assert.deepEqual(pick(await second(policies[3][1])), [2n, 0n]);
  assert.deepEqual(pick(await second(policies[4][1])), [2n, 0n]);
});

test('the target rejects a pending tick that does not follow the completed cursor', async () => {
  const options = {
    overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks,
    maxCatchUpTicks: 1,
    stopOnUnhandledException: false
  };
  const { state, timer, admission } = await captureWithPendingTick(options);
  admission.reject(new Error('execution barrier committed'));
  await timer.dispose().catch(() => undefined);
  const [pending] = state.pendingTicks;
  const invalid = [
    { ...pending, deliveryIndex: pending.deliveryIndex + 1n },
    { ...pending, scheduledIndex: state.lastScheduledIndex },
    { ...pending, skippedTicks: pending.skippedTicks + 1n },
    { ...pending, scheduledAtUnixMs: pending.scheduledAtUnixMs + 1 }
  ].map((tick) => ({ ...state, pendingTicks: [tick] }));
  invalid.push({ ...state, pendingTicks: [pending, pending] });
  for (const cursor of invalid) {
    const clock = createClock(TARGET_MONOTONIC_ORIGIN, SOURCE_UNIX_ORIGIN);
    assert.throws(
      () => createTimer('heartbeat', options, clock, [], undefined, cursor),
      /relocation state is invalid/u
    );
  }
});

/**
 * A Spot activation whose timer tick expires after the relocation seal: the
 * tick waits for execution-barrier admission when the source captures it.
 */
async function sealedSourceWithPendingTick() {
  const clock = createClock(0, SOURCE_UNIX_ORIGIN);
  const spotSerial = new ZLinkSpotSerialTurnExecutor(false);
  const serialExecutor = new ZLinkSpotSerialExecutor(
    spotSerial,
    ZLinkUserSpotExecutionMode.SpotWide,
    'spot'
  );
  const timers = new ZLinkSpotTimerRegistry(
    undefined,
    () => false,
    undefined,
    undefined,
    (name, operation) => serialExecutor.executeTimer(name, operation),
    (name) => serialExecutor.isTimerExecuting(name),
    clock
  );
  const activation = new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot',
    spotType: class PendingTickSpot {},
    spot: {},
    serial: spotSerial,
    serialExecutor,
    domain: {
      kind: 'user',
      executionMode: ZLinkUserSpotExecutionMode.SpotWide,
      relocationCoordinationMode: ZLinkSpotRelocationCoordinationMode.FrameworkManaged
    },
    timers,
    actorHandlers: {},
    handlers: {}
  });
  const delivered = [];
  const failures = [];
  class HeartbeatHandler {
    async handle(_spot, tick) {
      delivered.push(observed(tick));
    }
  }
  await timers.add(
    'heartbeat',
    PERIOD_MS,
    { overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks, stopOnUnhandledException: true },
    HeartbeatHandler,
    spotSerial,
    {},
    undefined,
    undefined,
    (tick, cause, event) => failures.push({ deliveryIndex: tick.deliveryIndex, event, cause })
  );
  clock.fireNext();
  await settle();
  // Relocation capture: seal, quiesce, then the next tick expires and waits for admission.
  const seal = activation.sealExecution();
  await activation.waitForExecutionQuiescence(seal);
  clock.fireNext();
  await settle();
  const capture = { seal, timers: await timers.captureRelocation() };
  return { activation, capture, clock, delivered, failures, timers };
}

test('a committed source hands its pending tick to the capture without reporting a failure', async () => {
  const source = await sealedSourceWithPendingTick();
  const [state] = source.capture.timers;
  assert.equal(state.deliveryIndex, 1n);
  assert.deepEqual(state.pendingTicks, [
    {
      deliveryIndex: 2n,
      scheduledIndex: 2n,
      scheduledAtUnixMs: SOURCE_UNIX_ORIGIN + 2 * PERIOD_MS,
      skippedTicks: 0n
    }
  ]);

  assert.equal(await source.activation.commitRelocation(source.capture), true);
  await settle();
  assert.deepEqual(
    source.delivered.map((tick) => tick.deliveryIndex),
    [1n]
  );
  assert.deepEqual(source.failures, [], 'the moved tick is not a source handler failure');
});

test('an aborted source delivers its pending tick once and keeps one schedule', async () => {
  const source = await sealedSourceWithPendingTick();
  assert.equal(source.activation.abortRelocation(source.capture), true);
  await settle();
  assert.deepEqual(
    source.delivered.map((tick) => [tick.deliveryIndex, tick.scheduledIndex]),
    [
      [1n, 1n],
      [2n, 2n]
    ]
  );
  assert.equal(source.clock.live, 1, 'the delivered pending tick schedules the next tick once');
  source.clock.fireNext();
  await settle();
  assert.deepEqual(source.delivered.at(-1), {
    deliveryIndex: 3n,
    scheduledIndex: 3n,
    skippedTicks: 0n,
    scheduledAt: SOURCE_UNIX_ORIGIN + 3 * PERIOD_MS
  });
  assert.deepEqual(source.failures, []);
  await source.timers.dispose();
});

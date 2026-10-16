const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');
const {
  ZLinkHostServiceRelocationRuntime,
  relocationFailedFailureCode
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

// Spot timer §1 exception: on a relocation target, Configure and the pre-Ready
// OnInitialize reattach a handler to a key restored from the source payload. The
// registration keeps the restored period, options, cursor and handle. Other
// callbacks follow the ordinary generation rule and the frozen registry rejects them.
// Node.js resolves a restored timer's handler by name among the constructors the
// Spot announced in configure() (addTimer there, or handlers.addTimerHandler), so a
// declared timer survives Ready without re-registration; no match or more than
// one match fails the target restore (Node interface 04-spots, #1767).

const detachedTaskRunner = new ZLinkRuntimeTaskRunner(
  new ZLinkRuntimeTaskErrorSink(),
  new AbortController().signal
);
const MESH = 'mesh';
const SPOT_ID = 'timer-rebind-spot';
const SOURCE_STARTED_AT = 1_800_000_000_000;

function restoredTimer(overrides = {}) {
  return {
    timerId: 'tick',
    handlerType: 'TickHandler',
    startedAtUnixMs: SOURCE_STARTED_AT,
    dueAtUnixMs: SOURCE_STARTED_AT + 8_000,
    intervalMs: 1_000,
    deliveryIndex: 7n,
    lastScheduledIndex: 7n,
    overrunPolicy: framework.ZLinkTimerOverrunPolicy.CatchUpBounded,
    maxCatchUpTicks: 3,
    stopOnUnhandledException: true,
    pendingTicks: [],
    ...overrides
  };
}

function createClock() {
  let now = 0;
  const scheduled = [];
  return {
    now: () => now,
    utcNow: () => SOURCE_STARTED_AT + 7_500 + now,
    setTimeout(callback, delay) {
      const handle = { callback, due: now + (Number(delay) || 0), cleared: false };
      scheduled.push(handle);
      return handle;
    },
    clearTimeout(handle) {
      handle.cleared = true;
    },
    get pending() {
      return scheduled.filter((handle) => !handle.cleared).length;
    },
    // Fires the earliest due callback and moves the monotonic clock to it.
    fireNext() {
      const live = scheduled.filter((handle) => !handle.cleared);
      assert.ok(live.length > 0, 'expected a scheduled timer callback');
      const handle = live.reduce((first, next) => (next.due < first.due ? next : first));
      scheduled.splice(scheduled.indexOf(handle), 1);
      now = Math.max(now, handle.due);
      handle.callback();
    }
  };
}

function createDeferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function relocationTarget(
  StageSpot,
  clock,
  { applicationSignaled = false, objectKind = 'user_spot' } = {}
) {
  const mode = applicationSignaled
    ? framework.ZLinkSpotRelocationCoordinationMode.ApplicationSignaled
    : framework.ZLinkSpotRelocationCoordinationMode.FrameworkManaged;
  const manager = new framework.DefaultZLinkSpotManager(
    {
      detachedTaskRunner,
      spotFactories: [StageSpot],
      userSpotRelocationCoordinationMode: () => mode
    },
    clock
  );
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([
        [
          MESH,
          {
            [objectKind === 'user_spot'
              ? 'spotFactoryRegistrations'
              : 'instanceSpotFactoryRegistrations']: {
              Stage: {
                implementation: StageSpot,
                relocation: { kind: 'recreate' },
                options: { relocationCoordinationMode: mode }
              }
            }
          }
        ]
      ])
    },
    meshNode: () => ({ restoreSpotAuthority() {}, rememberSpotRoute() {} }),
    spotManager: () => manager
  });
  return { manager, runtime };
}

function envelope(timers, objectKind = 'user_spot') {
  return {
    aggregateId: 'aggregate-timer-rebind',
    aggregateGeneration: 1n,
    participants: [
      {
        participantId: 1n,
        key: encodeAuthorityKey(objectKind, SPOT_ID).value,
        objectKind,
        stableType: 'Stage',
        objectGeneration: 3n,
        authorityOwnerGeneration: 3n,
        applicationState: new Uint8Array(),
        boundSessionState: new Uint8Array(),
        queuedMessages: [],
        timers
      }
    ],
    memberships: []
  };
}

const committedAuthority = {
  objectGeneration: 3n,
  authorityOwnerGeneration: 4n,
  ownerId: 'target-owner',
  ownerLeaseGeneration: 1n,
  storeVersion: { value: '1' },
  allocation: {
    objectKind: 'user_spot',
    stableType: 'Stage',
    descriptor: { rid: 'target-node' },
    descriptorLifecycleGeneration: 1n
  }
};

// Target restore before the authority CAS, then the post-CAS publication.
async function restoreTarget(runtime, timers, objectKind = 'user_spot') {
  const stage = await runtime.materializeTargetEnvelope(MESH, envelope(timers, objectKind));
  await stage.owner.restoreSavedWork(stage.staging);
  return stage;
}

function activationOf(stage) {
  return [...stage.staging.hidden.values()][0].activation;
}

function isInvalidOperation(error) {
  return (
    error instanceof framework.ZLinkFrameworkException &&
    error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
  );
}

test('Configure reattaches a restored key with the restored period, options, cursor and handle', async () => {
  const clock = createClock();
  const ticks = [];
  const delivered = createDeferred();
  const handles = [];
  class TickHandler {
    async handle(_spot, tick) {
      ticks.push(tick);
      delivered.resolve();
    }
  }
  class StageSpot {
    async configure() {
      handles.push(
        await this.context.addTimer('tick', 50, TickHandler, {
          overrunPolicy: framework.ZLinkTimerOverrunPolicy.SkipLateTicks
        })
      );
    }
    async onInitialize() {
      handles.push(await this.context.addTimer('tick', 75, TickHandler));
    }
  }
  const { manager, runtime } = relocationTarget(StageSpot, clock);
  const stage = await restoreTarget(runtime, [restoredTimer()]);
  const activation = activationOf(stage);
  try {
    // Nothing runs before Ready.
    assert.equal(clock.pending, 0);
    await stage.owner.publish(stage.staging, committedAuthority);
    assert.equal(handles.length, 2);
    assert.equal(handles[0], handles[1]);

    clock.fireNext();
    await delivered.promise;
    assert.equal(ticks.length, 1);
    assert.equal(ticks[0].deliveryIndex, 8n);
    assert.equal(ticks[0].scheduledIndex, 8n);
    assert.equal(ticks[0].periodMs, 1_000);
    assert.equal(ticks[0].scheduledAt.getTime(), SOURCE_STARTED_AT + 8_000);

    const [captured] = await activation.timers.captureRelocation();
    assert.deepEqual(
      {
        handlerType: captured.handlerType,
        periodMs: captured.periodMs,
        overrunPolicy: captured.overrunPolicy,
        maxCatchUpTicks: captured.maxCatchUpTicks,
        stopOnUnhandledException: captured.stopOnUnhandledException,
        startedAtUnixMs: captured.startedAtUnixMs,
        deliveryIndex: captured.deliveryIndex,
        lastScheduledIndex: captured.lastScheduledIndex
      },
      {
        handlerType: 'TickHandler',
        periodMs: 1_000,
        overrunPolicy: framework.ZLinkTimerOverrunPolicy.CatchUpBounded,
        maxCatchUpTicks: 3,
        stopOnUnhandledException: true,
        startedAtUnixMs: SOURCE_STARTED_AT,
        deliveryIndex: 8n,
        lastScheduledIndex: 8n
      }
    );
  } finally {
    await manager.abortRelocationSpot(activation);
  }
});

test('OnInitialize reattaches a restored key and a new key keeps ordinary registration', async () => {
  const clock = createClock();
  const ticks = [];
  const fresh = [];
  const delivered = createDeferred();
  class TickHandler {
    async handle(_spot, tick) {
      ticks.push(tick);
      if (ticks.length + fresh.length === 2) delivered.resolve();
    }
  }
  class FreshHandler {
    async handle(_spot, tick) {
      fresh.push(tick);
      if (ticks.length + fresh.length === 2) delivered.resolve();
    }
  }
  class StageSpot {
    configure() {
      this.context.handlers.addTimerHandler(TickHandler);
    }
    async onInitialize() {
      this.tick = await this.context.addTimer('tick', 50, TickHandler);
      this.fresh = await this.context.addTimer('fresh', 400, FreshHandler);
    }
  }
  const { manager, runtime } = relocationTarget(StageSpot, clock);
  const stage = await restoreTarget(runtime, [restoredTimer()]);
  const activation = activationOf(stage);
  try {
    assert.equal(clock.pending, 0);
    await stage.owner.publish(stage.staging, committedAuthority);
    assert.equal(clock.pending, 2);
    clock.fireNext();
    clock.fireNext();
    await delivered.promise;
    assert.deepEqual(
      ticks.map((tick) => [tick.deliveryIndex, tick.scheduledIndex, tick.periodMs]),
      [[8n, 8n, 1_000]]
    );
    assert.deepEqual(
      fresh.map((tick) => [tick.deliveryIndex, tick.scheduledIndex, tick.periodMs]),
      [[1n, 1n, 400]]
    );
    const states = await activation.timers.captureRelocation();
    assert.deepEqual(
      states.map((state) => [state.name, state.handlerType, state.periodMs]),
      [
        ['fresh', 'FreshHandler', 400],
        ['tick', 'TickHandler', 1_000]
      ]
    );
  } finally {
    await manager.abortRelocationSpot(activation);
  }
});

test('Configure handler type mismatch propagates and fails the target restore', async () => {
  const clock = createClock();
  class TickHandler {
    async handle() {}
  }
  class OtherHandler {
    async handle() {}
  }
  class StageSpot {
    async configure() {
      await this.context.addTimer('tick', 1_000, OtherHandler);
    }
  }
  void TickHandler;
  const { manager, runtime } = relocationTarget(StageSpot, clock);
  await assert.rejects(() => restoreTarget(runtime, [restoredTimer()]), isInvalidOperation);
  assert.equal(manager.resolveRelocationActivation(MESH, SPOT_ID), undefined);
  assert.equal(clock.pending, 0);
});

test('OnInitialize handler type mismatch fails only that registration after the commit', async () => {
  const clock = createClock();
  const ticks = [];
  const failures = [];
  const delivered = createDeferred();
  class TickHandler {
    async handle(_spot, tick) {
      ticks.push(tick);
      delivered.resolve();
    }
  }
  class OtherHandler {
    async handle() {
      throw new Error('the mismatched handler must not be attached');
    }
  }
  class StageSpot {
    async configure() {
      this.tick = await this.context.addTimer('tick', 1_000, TickHandler);
    }
    async onInitialize() {
      try {
        await this.context.addTimer('tick', 1_000, OtherHandler);
      } catch (error) {
        failures.push(error);
      }
    }
  }
  const { manager, runtime } = relocationTarget(StageSpot, clock);
  const stage = await restoreTarget(runtime, [restoredTimer()]);
  const activation = activationOf(stage);
  try {
    await stage.owner.publish(stage.staging, committedAuthority);
    assert.equal(failures.length, 1);
    assert.equal(isInvalidOperation(failures[0]), true);
    assert.equal(manager.resolveRelocationActivation(MESH, SPOT_ID), activation);
    clock.fireNext();
    await delivered.promise;
    assert.deepEqual(
      ticks.map((tick) => [tick.deliveryIndex, tick.scheduledIndex]),
      [[8n, 8n]]
    );
  } finally {
    await manager.abortRelocationSpot(activation);
  }
});

test('relocation-ready completion registration follows the frozen generation rule', async () => {
  const clock = createClock();
  const completions = [];
  class TickHandler {
    async handle() {}
  }
  class StageSpot {
    configure() {
      this.context.handlers.addTimerHandler(TickHandler);
    }
    async onInitialize() {
      await this.context.addTimer('tick', 1_000, TickHandler);
    }
    async onRelocationReadyCompleted(completion) {
      try {
        await this.context.addTimer('tick', 1_000, TickHandler);
        completions.push({ outcome: completion.outcome, error: undefined });
      } catch (error) {
        completions.push({ outcome: completion.outcome, error });
      }
    }
  }
  const { manager, runtime } = relocationTarget(StageSpot, clock, { applicationSignaled: true });
  const stage = await restoreTarget(runtime, [restoredTimer()]);
  const activation = activationOf(stage);
  try {
    await stage.owner.publish(stage.staging, committedAuthority);
    assert.equal(completions.length, 1);
    assert.equal(completions[0].outcome, framework.ZLinkSpotRelocationReadyOutcome.Relocated);
    assert.equal(isInvalidOperation(completions[0].error), true);
    const [state] = await activation.timers.captureRelocation();
    assert.deepEqual([state.deliveryIndex, state.lastScheduledIndex], [7n, 7n]);
  } finally {
    await manager.abortRelocationSpot(activation);
  }
});

test('invalid registration arguments still fail before a restored key is reattached', async () => {
  const clock = createClock();
  class TickHandler {
    async handle() {}
  }
  class StageSpot {
    async configure() {
      await this.context.addTimer('tick', 0, TickHandler);
    }
  }
  const { manager, runtime } = relocationTarget(StageSpot, clock);
  await assert.rejects(
    () => restoreTarget(runtime, [restoredTimer()]),
    (error) => error instanceof framework.ZLinkConfigurationException
  );
  assert.equal(manager.resolveRelocationActivation(MESH, SPOT_ID), undefined);
});

function isRelocationFailed(error) {
  return (
    internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RelocationFailed &&
    relocationFailedFailureCode(error) === 17
  );
}

for (const [objectKind, declare] of [
  ['user_spot', 'addTimerHandler'],
  ['instance_spot', 'addTimerHandler'],
  ['user_spot', 'configure addTimer']
]) {
  test(`${objectKind} restored timer declared by ${declare} fires after relocation without re-registration`, async () => {
    const clock = createClock();
    const ticks = [];
    const delivered = createDeferred();
    class TickHandler {
      async handle(_spot, tick) {
        ticks.push(tick);
        delivered.resolve();
      }
    }
    class OtherKeyHandler {
      async handle() {}
    }
    class StageSpot {
      async configure() {
        if (declare === 'addTimerHandler') {
          this.context.handlers.addTimerHandler(TickHandler);
          // The same constructor declared twice is one declaration.
          this.context.handlers.addTimerHandler(TickHandler);
        } else {
          // addTimer in configure() announces the constructor for another key.
          await this.context.addTimer('other', 5_000, TickHandler);
          await this.context.addTimer('unrelated', 5_000, OtherKeyHandler);
        }
      }
    }
    const { manager, runtime } = relocationTarget(StageSpot, clock, { objectKind });
    const stage = await restoreTarget(runtime, [restoredTimer()], objectKind);
    const activation = activationOf(stage);
    try {
      assert.equal(clock.pending, 0, 'no restored tick runs before Ready');
      await stage.owner.publish(stage.staging, committedAuthority);
      assert.equal(manager.resolveRelocationActivation(MESH, SPOT_ID), activation);
      clock.fireNext();
      await delivered.promise;
      assert.deepEqual(
        ticks.map((tick) => [tick.name, tick.deliveryIndex, tick.scheduledIndex, tick.periodMs]),
        [['tick', 8n, 8n, 1_000]]
      );
      const states = await activation.timers.captureRelocation();
      const tick = states.find((state) => state.name === 'tick');
      assert.deepEqual(
        [tick.handlerType, tick.deliveryIndex, tick.lastScheduledIndex],
        ['TickHandler', 8n, 8n]
      );
    } finally {
      await manager.abortRelocationSpot(activation);
    }
  });
}

for (const [objectKind, mode] of [
  ['user_spot', 'undeclared'],
  ['instance_spot', 'undeclared'],
  ['user_spot', 'onInitialize only'],
  ['user_spot', 'packet only'],
  ['user_spot', 'duplicate name'],
  ['instance_spot', 'duplicate name']
]) {
  test(`${objectKind} restored timer with ${mode} handler fails the target restore`, async () => {
    const clock = createClock();
    const initialized = [];
    class TickHandler {
      async handle() {}
    }
    const SameNameTickHandler = class TickHandler {
      async handle() {}
    };
    class StageSpot {
      async configure() {
        if (mode === 'packet only') this.context.handlers.addPacket(TickHandler);
        if (mode === 'duplicate name') {
          this.context.handlers.addTimerHandler(TickHandler);
          this.context.handlers.addTimerHandler(SameNameTickHandler);
        }
      }
      async onInitialize() {
        initialized.push('onInitialize');
        if (mode === 'onInitialize only') {
          await this.context.addTimer('tick', 1_000, TickHandler);
        }
      }
    }
    const { manager, runtime } = relocationTarget(StageSpot, clock, { objectKind });
    await assert.rejects(
      () => restoreTarget(runtime, [restoredTimer()], objectKind),
      (error) => {
        assert.equal(isRelocationFailed(error), true, String(error));
        assert.match(error.message, /'tick'/);
        assert.match(
          error.message,
          mode === 'duplicate name' ? /ambiguous/ : /addTimerHandler/
        );
        return true;
      }
    );
    assert.equal(manager.resolveRelocationActivation(MESH, SPOT_ID), undefined);
    assert.deepEqual(initialized, [], 'a failed restore never reaches OnInitialize');
    assert.equal(clock.pending, 0);
  });
}

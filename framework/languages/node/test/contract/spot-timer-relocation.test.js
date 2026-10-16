const assert = require('node:assert/strict');
const test = require('node:test');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkHostServiceRelocationRuntime,
  relocationFailedFailureCode
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const {
  ZLinkRuntimeTaskRunner,
  ZLinkRuntimeTaskErrorSink
} = require('../../packages/framework/dist/runtime/execution');

async function fixture(mode, run) {
  const handles = new Set();
  let now = 0;
  const clock = {
    now: () => now,
    utcNow: () => 1_800_000_000_000 + now,
    setTimeout(callback, delay) {
      const handle = { callback, due: now + delay };
      handles.add(handle);
      return handle;
    },
    clearTimeout(handle) {
      handles.delete(handle);
    }
  };
  let ticks = 0;
  let tickReceived;
  const nextTick = new Promise((resolve) => {
    tickReceived = resolve;
  });
  class BoundaryTick {
    async handle(_spot, tick) {
      ticks++;
      tickReceived(tick);
    }
  }
  const OtherBoundaryTick = class BoundaryTick {
    async handle() {}
  };
  let initialized = 0;
  class ZoneSpot {
    async configure() {
      if (
        mode === 'declared' ||
        mode === 'source-only' ||
        mode === 'ambiguous' ||
        mode === 'shadowed'
      ) {
        this.context.handlers.addTimerHandler(BoundaryTick);
        // Repeating the same constructor is one declaration, not an ambiguity.
        this.context.handlers.addTimerHandler(BoundaryTick);
      }
      if (mode === 'ambiguous') this.context.handlers.addTimerHandler(OtherBoundaryTick);
      if (mode === 'packet-only') this.context.handlers.addPacket(BoundaryTick);
      if (mode === 'configured')
        this.timer = await this.context.addTimer('boundary', 1000, BoundaryTick);
    }
    async onInitialize() {
      initialized++;
      if (
        mode !== 'configured' &&
        !(mode === 'source-only' && this.context.spotId === 'target-zone')
      ) {
        this.timer = await this.context.addTimer(
          'boundary',
          1000,
          mode === 'shadowed' && this.context.spotId === 'target-zone'
            ? OtherBoundaryTick
            : BoundaryTick
        );
      }
    }
  }
  const manager = new DefaultZLinkSpotManager(
    {
      detachedTaskRunner: new ZLinkRuntimeTaskRunner(
        new ZLinkRuntimeTaskErrorSink(),
        new AbortController().signal
      ),
      spotFactories: [ZoneSpot]
    },
    clock
  );
  let source;
  let stage;
  try {
    const created = await manager.create('mesh', ZoneSpot);
    source = manager.resolveRelocationActivation('mesh', created.spotId);
    const states = await source.timers.captureRelocation();
    assert.equal(states.length, 1);
    // A nonzero cursor detects re-registration that silently restarts the timer.
    states[0] = {
      ...states[0],
      deliveryIndex: 7n,
      lastScheduledIndex: 9n,
      nextDueAtUnixMs: states[0].startedAtUnixMs + 10 * states[0].periodMs
    };
    const registration = { implementation: ZoneSpot, relocation: { kind: 'recreate' } };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([['mesh', { spotFactoryRegistrations: { Zone: registration } }]])
      },
      spotManager: () => manager
    });
    const envelope = {
      aggregateId: 'timer-repro',
      aggregateGeneration: 1n,
      memberships: [],
      participants: [
        {
          participantId: 1n,
          key: encodeAuthorityKey('user_spot', 'target-zone').value,
          objectKind: 'user_spot',
          stableType: 'Zone',
          objectGeneration: 1n,
          authorityOwnerGeneration: 1n,
          applicationState: new Uint8Array(),
          boundSessionState: new Uint8Array(),
          queuedMessages: [],
          timers: states.map((s) => ({
            timerId: s.name,
            handlerType: s.handlerType,
            intervalMs: s.periodMs,
            overrunPolicy: s.overrunPolicy,
            maxCatchUpTicks: s.maxCatchUpTicks,
            stopOnUnhandledException: s.stopOnUnhandledException,
            startedAtUnixMs: s.startedAtUnixMs,
            deliveryIndex: s.deliveryIndex,
            lastScheduledIndex: s.lastScheduledIndex,
            dueAtUnixMs: s.nextDueAtUnixMs,
            pendingTicks: s.pendingTicks
          }))
        }
      ]
    };
    stage = await runtime.materializeTargetEnvelope('mesh', envelope);
    const target = [...stage.staging.hidden.values()][0].activation;
    await run({
      restore: () => stage.owner.restoreSavedWork(stage.staging),
      target,
      manager,
      initialized: () => initialized,
      ticks: () => ticks,
      handles,
      nextTick,
      fire() {
        const handle = [...handles][0];
        assert.ok(handle, 'published timer must be scheduled');
        handles.delete(handle);
        now = handle.due;
        handle.callback();
      }
    });
  } finally {
    if (stage) await stage.owner.abort(stage.staging);
    if (source) await manager.abortRelocationSpot(source);
    assert.equal(handles.size, 0);
  }
}

for (const mode of ['declared', 'source-only', 'configured']) {
  test(`${mode} timer restores its cursor and stays frozen until target publication`, async () => {
    await fixture(mode, async (f) => {
      const timerBeforeRestore = f.target.spot.timer;
      await f.restore();
      assert.equal(f.initialized(), 2, 'target initialization completes before Ready');
      assert.equal(f.ticks(), 0);
      assert.equal(f.handles.size, 0, 'hidden target cannot schedule callbacks before Ready');
      const registrations = f.target.handlers.snapshot();
      assert.equal(
        registrations.some((r) => r.kind === 'packet' || r.kind === 'subscribe'),
        false
      );
      await f.manager.publishRelocationSpot(f.target);
      assert.equal(f.initialized(), 2, 'publication must not initialize a second time');
      f.fire();
      const tick = await f.nextTick;
      assert.equal(tick.deliveryIndex, 8n);
      assert.equal(tick.scheduledIndex, 10n);
      if (timerBeforeRestore) assert.strictEqual(f.target.spot.timer, timerBeforeRestore);
    });
  });
}

for (const mode of ['undeclared', 'packet-only', 'ambiguous', 'shadowed']) {
  test(`${mode} timer declaration fails relocation with a useful handler diagnostic`, async () => {
    await fixture(mode, async (f) => {
      await assert.rejects(f.restore(), (error) => {
        assert.equal(error.kind, ZLinkFrameworkErrorKind.InternalFailure);
        assert.equal(relocationFailedFailureCode(error), 17);
        assert.equal(
          internalFrameworkErrorKind(error),
          ZLinkFrameworkInternalErrorKind.RelocationFailed
        );
        assert.match(error.message, /BoundaryTick/);
        assert.match(error.message, mode === 'ambiguous' ? /ambiguous/i : /addTimerHandler/);
        return true;
      });
      assert.equal(f.ticks(), 0);
    });
  });
}

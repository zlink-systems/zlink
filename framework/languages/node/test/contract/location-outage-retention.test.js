'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');

function executionPolicy(runtime, owner) {
  return {
    executionDecision: (_owner, wake) => runtime.executionDecision(owner, wake),
    executionFailure: () => runtime.executionFailure()
  };
}
function fixedPolicy(
  allowed,
  failureKind = internal.ZLinkFrameworkInternalErrorKind.RequestRejected
) {
  return {
    executionDecision: () => (allowed() ? 'allowed' : 'lost'),
    executionFailure: () =>
      internal.createInternalFrameworkException(failureKind, 'Execution policy closed.')
  };
}
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const {
  zlinkDefaultLocationOptions
} = require('../../packages/framework/dist/contracts/Locations/Options');

async function scenario(t, provider, retention = false) {
  let now = 100;
  let clockReads = 0;
  const timers = new Set();
  const store = provider
    ? new internal.ZLinkLocationStoreRepository(
        new internal.ZLinkInMemoryProviderLocationStore(() => new Date(0)),
        () => new Date(0),
        undefined,
        () => now,
        0
      )
    : new internal.ZLinkInMemoryLocationStore(
        () => new Date(0),
        () => now,
        0
      );
  const lease = {
    claimOwnerLease: (...args) => store.claimOwnerLease(...args),
    renewOwnerLease: (...args) => store.renewOwnerLease(...args),
    readOwnerLease: (...args) => store.readOwnerLease(...args),
    releaseOwnerLease: (...args) => store.releaseOwnerLease(...args)
  };
  const options = {
    storeOutageRetention: retention,
    ownerLeaseTtlMs: 1000,
    ownerLeaseFencingMarginMs: 100
  };
  const runtime = new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: lease
    },
    ownerId: 'owner',
    options,
    monotonicNowMs: () => {
      clockReads++;
      return now;
    },
    setTimer(callback, delay) {
      const timer = { callback, at: now + delay };
      timers.add(timer);
      return timer;
    },
    clearTimer(timer) {
      timers.delete(timer);
    }
  });
  await runtime.start({ toString: () => 'node' });
  t.after(async () => {
    await runtime.stop();
    assert.equal(timers.size, 0);
  });
  return {
    runtime,
    clockReads: () => clockReads,
    lease,
    options,
    store,
    snapshot: () => runtime[inspectRuntime](),
    clock: {
      now: () => now,
      utcNow: () => now,
      setTimeout(callback, delay) {
        const timer = { callback, at: now + delay };
        timers.add(timer);
        return timer;
      },
      clearTimeout(timer) {
        timers.delete(timer);
      }
    },
    advance(value) {
      now = value;
      for (const timer of [...timers])
        if (timer.at <= now) {
          timers.delete(timer);
          timer.callback();
        }
    },
    setNow(value) {
      now = value;
    },
    fail() {
      lease.renewOwnerLease = async () => {
        throw new Error('provider unavailable');
      };
    }
  };
}

for (const provider of [false, true]) {
  for (const retention of [false, true]) {
    test(`outage cutoff and scheduled irreversible latch (provider=${provider}, retention=${retention})`, async (t) => {
      const s = await scenario(t, provider, retention);
      s.setNow(200);
      s.fail();
      assert.equal(await s.runtime.renewOwnerLeaseOnce(), false);
      const cutoff = retention ? 1300 : 1000;
      assert.equal(s.snapshot().storeOutage.startedAtMs, 200);
      assert.equal(s.snapshot().storeOutage.cutoffMs, cutoff);
      s.advance(cutoff - 1);
      assert.equal(s.snapshot().storeOutage.discarded, false);
      assert.equal(s.runtime.ownerLeaseUsable, retention ? false : true);
      s.advance(cutoff);
      assert.equal(s.snapshot().storeOutage.discarded, true);
      s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.discarded, true);
      assert.equal(s.runtime.ownerLeaseUsable, true); // Observation has no execution consumer yet.
    });
  }
  test(`successful renewal closes episode before T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage, undefined);
    s.advance(1300);
    assert.equal(s.snapshot().storeOutage, undefined);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1500);
  });
  test(`same owner renewal pending before outage closes it before T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    let complete;
    s.setNow(150);
    s.lease.renewOwnerLease = (...args) =>
      new Promise((resolve) => {
        complete = async () => resolve(await s.store.renewOwnerLease(...args));
      });
    const pending = s.runtime.renewOwnerLeaseOnce();
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    await complete();
    await pending;
    assert.equal(s.snapshot().storeOutage, undefined);
    s.advance(1300);
    assert.equal(s.snapshot().storeOutage, undefined);
  });
  test(`new claim and old pending renewal cannot move original T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    let complete;
    s.setNow(150);
    s.lease.renewOwnerLease = (...args) =>
      new Promise((resolve) => {
        complete = async () => resolve(await s.store.renewOwnerLease(...args));
      });
    const pending = s.runtime.renewOwnerLeaseOnce();
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(1200);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.readOwnerLease = async () => ({ kind: 'missing' });
    s.lease.claimOwnerLease = async () => ({
      kind: 'claimed',
      token: { ownerId: 'owner', leaseGeneration: 2n },
      storeNow: new Date(0),
      leaseExpiresAt: new Date(1000)
    });
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    s.setNow(1300);
    await complete();
    await pending;
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    assert.equal(s.snapshot().storeOutage.discarded, true);
  });
  test(`definite Missing and Conflict do not open an outage (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.claimOwnerLease = async () => ({ kind: 'conflict', storeNow: new Date(0) });
    s.lease.readOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage, undefined);
  });
  test(`renewal response at T latches before scheduled callback (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(1300);
    s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.discarded, true);
  });
  test(`confirmation read provider error opens outage using original lease (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.claimOwnerLease = async () => ({ kind: 'conflict', storeNow: new Date(0) });
    s.lease.readOwnerLease = async () => {
      throw new Error('read unavailable');
    };
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.startedAtMs, 200);
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
  });
  for (const retention of [false, true]) {
    test(`cutoff uses successful request start despite response delay (provider=${provider}, retention=${retention})`, async (t) => {
      const s = await scenario(t, provider, retention);
      s.setNow(200);
      s.lease.renewOwnerLease = async (...args) => {
        const result = await s.store.renewOwnerLease(...args);
        s.setNow(250);
        return result;
      };
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().lastSuccessfulLeaseStartedAtMs, 200);
      s.setNow(400);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.cutoffMs, retention ? 1400 : 1100);
      s.setNow(500);
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.startedAtMs, 400);
      assert.equal(s.snapshot().storeOutage.cutoffMs, retention ? 1400 : 1100);
    });
  }
  test(`renew timeout opens episode with injected time (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    s.setNow(200);
    const response = await s.store.renewOwnerLease(s.runtime.currentOwnerToken, 1000);
    let complete;
    s.lease.renewOwnerLease = () =>
      new Promise((resolve) => {
        complete = () => resolve(response);
      });
    const renewal = s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    t.mock.timers.tick(3000);
    assert.equal(await renewal, false);
    assert.equal(s.snapshot().storeOutage.startedAtMs, 300);
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    s.setNow(400);
    await complete();
    await Promise.resolve();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    assert.equal(s.snapshot().storeOutage.discarded, false);
  });
  test(`runtime copies startup retention option (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider);
    s.options.storeOutageRetention = true;
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1000);
  });
}

test('retention option defaults false, builds and validates boolean values', () => {
  assert.equal(zlinkDefaultLocationOptions.storeOutageRetention, false);
  const registration = internal.createFrameworkRegistrationWithBuilder((builder) => {
    builder.configureLocations().storeOutageRetention(true);
  });
  assert.equal(registration.locations.options.storeOutageRetention, true);
  assert.throws(
    () =>
      internal.createFrameworkRegistrationWithBuilder((builder) => {
        builder.configureLocations().storeOutageRetention('true');
      }),
    /storeOutageRetention.*boolean/
  );
});

for (const provider of [false, true]) {
  for (const retention of [false, true]) {
    test(`execution gate consumes outage latch (provider=${provider}, retention=${retention})`, async (t) => {
      const s = await scenario(t, provider, retention);
      const owner = {};
      let discarded = 0;
      s.runtime.trackExecution(
        owner,
        () => true,
        () => {
          discarded++;
        }
      );
      const gate = new internal.ZLinkExecutionBarrier();
      gate.setExecutionPolicy(() => executionPolicy(s.runtime, owner));
      const serial = new internal.ZLinkSpotSerialTurnExecutor();
      serial.setExecutionBarrier(gate);
      let calls = 0;
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      const cutoff = retention ? 1300 : 1000;
      s.advance(cutoff - 1);
      await serial.execute(() => {
        calls++;
      });
      assert.equal(calls, 1);
      let finish;
      const running = serial.execute(
        () =>
          new Promise((resolve) => {
            finish = resolve;
          })
      );
      for (let i = 0; i < 10 && finish === undefined; i++) await Promise.resolve();
      assert.equal(typeof finish, 'function');
      const queued = serial.execute(() => {
        calls++;
      });
      const refused = assert.rejects(queued);
      s.advance(cutoff);
      assert.equal(discarded, 1, 'latch cleanup does not wait for the running turn');
      finish();
      await running;
      await refused;
      assert.equal(calls, 1);
      s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
      await s.runtime.renewOwnerLeaseOnce();
      await assert.rejects(
        serial.execute(() => {
          calls++;
        })
      );
      assert.equal(calls, 1);
      assert.equal(discarded, 1);
    });
  }
  test(`only pre-outage steady Ready execution is retained (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    const ready = {},
      creating = {},
      moving = {},
      late = {};
    let creatingReady = false;
    const discarded = [];
    for (const [owner, steady] of [
      [ready, () => true],
      [creating, () => creatingReady],
      [moving, () => false]
    ])
      s.runtime.trackExecution(owner, steady, () => discarded.push(owner));
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    creatingReady = true;
    s.runtime.trackExecution(
      late,
      () => true,
      () => discarded.push(late)
    );
    s.advance(1299);
    assert.equal(s.runtime.executionAllowed(ready), true);
    for (const owner of [creating, moving, late])
      assert.equal(s.runtime.executionAllowed(owner), false);
    s.advance(1300);
    assert.deepEqual(discarded, [ready]);
    assert.throws(() => s.runtime.executionAllowed(ready), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
  });
}

async function objectScenario(t, provider, mode, providerResolver, statefulAdmissionOpen) {
  t.after(async () => {
    for (const active of spots.activations.activeActivations())
      s.runtime.discardExecution(active.spot);
    for (const actor of [a, b]) s.runtime.discardExecution(actor);
    await Promise.all(tasks);
  });
  const s = await scenario(t, provider, true);
  const tasks = [];
  const detachedTaskRunner = {
    runDetached(_name, task) {
      tasks.push(Promise.resolve().then(task));
    }
  };
  const admission = new internal.ZLinkRuntimeAdmissionGate(() => s.runtime.ownerLeaseUsable);
  admission.setExecutionRuntime(() => s.runtime);
  admission.register('mesh');
  const lifecycle = new internal.ZLinkLocationLifecycle(
    s.runtime,
    s.store,
    'mesh',
    s.store,
    s.store
  );
  t.after(() => lifecycle.dispose());
  class Room {}
  class Player {
    constructor(context) {
      this.context = context;
    }
  }
  class ActorProbe {
    handle() {
      return 'actor-handled';
    }
  }
  class Ping {
    handle() {
      return 'handled';
    }
  }
  const spots = new internal.DefaultZLinkSpotManager(
    {
      spotFactories: [Room],
      admission,
      locationLifecycle: lifecycle,
      providerResolver,
      actorResolver: (actorId) => actors.getState(actorId)?.actor,
      spotPacketHandlers: [{ spotType: Room, handlerType: Ping, packetName: 'Ping' }],
      spotActorRequestHandlers: [
        { spotType: Room, actorType: Player, handlerType: ActorProbe, packetName: 'Probe' }
      ],
      nodeRid: 'node',
      nodeRidProvider: () => 'node',
      nodeGenerationProvider: () => 1n,
      userSpotExecutionMode: () => mode,
      statefulExecution: {
        admissionOpen: statefulAdmissionOpen ?? (() => s.runtime.ownerLeaseUsable),
        hostState: () => internal.ZLinkFrameworkRuntimeState.Serving
      },
      createNativeSpot: (_mesh, rid) => ({
        routingId: rid,
        lifecycleGeneration: 1n,
        async dispose() {}
      }),
      createTopicMessage: () => ({}),
      detachedTaskRunner
    },
    s.clock
  );
  const actors = new internal.DefaultZLinkActorManager(
    {
      actorFactories: new Map([['Player', { create: (context) => new Player(context) }]]),
      actorMeshNameProvider: () => 'mesh',
      actorCreatedNodeRidProvider: () => 'node',
      locationLifecycle: lifecycle,
      admission,
      discardActorActivation: (actor, error) => spots.discardActorActivation(actor, error)
    },
    detachedTaskRunner
  );
  const result = await spots.create('mesh', Room, {});
  const activation = spots.activations.activationForClose('mesh', result.spotId);
  assert(activation);
  const a = await actors.getOrCreateActor('a', 'Player');
  const b = await actors.getOrCreateActor('b', 'Player');
  activation.commitActorJoin(a);
  activation.commitActorJoin(b);
  for (const id of ['a', 'b'])
    actors.getState(id).setJoinedSpot(activation.spotId, activation.spot, 1n, 1n);
  return { ...s, spots, actors, lifecycle, admission, activation, a, b, tasks, Room };
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`Spot/member lifecycle consumes terminal with shared record identity (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      s.advance(1299);
      assert.equal(
        await s.spots.dispatchRoutedSpotRequest(
          s.activation.spotId,
          'Ping',
          {},
          { channelName: 'mesh' }
        ),
        'handled'
      );
      const parts = actorRequestParts();
      try {
        assert.equal(
          await s.spots.dispatchRoutedActorPacket('local', s.activation.spotId, 'a', parts, true),
          'actor-handled'
        );
      } finally {
        parts.forEach((part) => part.close());
      }
      assert.throws(
        () => s.admission.requireRequest('new relocation', 'mesh'),
        /admission is closed/
      );
      let calls = 0;
      await s.activation.executeActor('a', (serial) =>
        serial.execute(() => {
          calls++;
        })
      );
      await s.activation.executeActor('b', (serial) =>
        serial.execute(() => {
          calls++;
        })
      );
      assert.equal(calls, 2);
      let finish, entered;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const running = s.activation.executeActor('a', (serial) =>
        serial.execute(() => {
          entered();
          return new Promise((resolve) => {
            finish = resolve;
          });
        })
      );
      await started;
      const pending = s.activation.executeActor('a', (serial) =>
        serial.execute(() => {
          calls++;
        })
      );
      const rejected = assert.rejects(pending);
      s.runtime.discardExecution(s.a);
      await rejected;
      finish();
      await running;
      assert.equal(s.actors.getState('a'), undefined, 'Actor memory detached synchronously');
      assert.equal(
        s.activation.resolveJoinedActor('a'),
        undefined,
        'membership releases the disposed Actor object'
      );
      await assert.rejects(async () =>
        s.activation.executeActor('a', (serial) =>
          serial.execute(() => {
            calls++;
          })
        )
      );
      await s.activation.executeActor('b', (serial) =>
        serial.execute(() => {
          calls++;
        })
      );
      await s.activation.serial.execute(() => {
        calls++;
      });
      assert.equal(calls, 4, 'member discard preserves Spot and sibling');
      s.runtime.discardExecution(s.activation.spot);
      assert.equal(s.actors.getState('b'), undefined);
      assert.equal(s.activation.resolveJoinedActor('b'), undefined);
      assert.equal(s.spots.activations.activeActivations().length, 0);
      await assert.rejects(
        s.activation.serial.execute(() => {
          calls++;
        })
      );
      assert.equal(calls, 4);
      await Promise.all(s.tasks);
      assert.equal(s.activation.timers.hasActiveTimers, false);
    });
    test(`Spot timer and queued member/completion terminate at T (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      let ticks = 0;
      class Tick {
        handle() {
          ticks++;
        }
      }
      await s.activation.spot.context.addTimer('tick', 100, Tick);
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      s.advance(1299);
      await s.activation.serialExecutor.executeTimer('barrier', () => undefined);
      for (let i = 0; i < 30 && ticks === 0; i++) await Promise.resolve();
      assert(ticks > 0, 'retained timer runs past admission');
      await assert.rejects(s.spots.create('mesh', s.Room, {}), /admission is closed/);
      const resolvers = new internal.ZLinkStoreLocationResolvers({
        stores: {
          locationStore: s.store,
          authorityStore: s.store,
          peerStore: s.store,
          spotStore: s.store,
          actorStore: s.store,
          routeStore: s.store
        },
        leaseTracker: new internal.ZLinkOwnerLeaseTracker({
          store: s.store,
          monotonicNowMs: s.clock.now
        }),
        admissionOpen: () => s.runtime.ownerLeaseUsable,
        monotonicNowMs: s.clock.now
      });
      await assert.rejects(resolvers.resolveDirectActorRoute('new'), /route lookup/);
      let finish, entered;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const running = s.activation.executeActor('b', (serial) =>
        serial.execute(() => {
          entered();
          return new Promise((resolve) => {
            finish = resolve;
          });
        })
      );
      await started;
      let calls = 0;
      const queued = s.activation.executeActor('b', (serial) =>
        serial.execute(() => {
          calls++;
        })
      );
      const refused = assert.rejects(queued);
      s.advance(1300);
      assert.equal(s.actors.getState('b'), undefined);
      assert.equal(s.spots.activations.activeActivations().length, 0);
      finish();
      await running;
      await refused;
      await assert.rejects(
        s.activation.serial.post(() => {
          calls++;
        })
      );
      await Promise.all(s.tasks);
      const oldTicks = ticks;
      s.advance(1500);
      await Promise.resolve();
      assert.equal(ticks, oldTicks);
      assert.equal(calls, 0);
      assert.equal(s.activation.timers.hasActiveTimers, false);
      s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
      await s.runtime.renewOwnerLeaseOnce();
      s.runtime.trackExecution(
        s.a,
        () => true,
        () => assert.fail('old object registered twice')
      );
      assert.throws(() => s.runtime.executionAllowed(s.a), {
        kind: internal.ZLinkFrameworkErrorKind.NotFound
      });
      await assert.rejects(async () =>
        s.activation.executeActor('a', (serial) =>
          serial.execute(() => {
            calls++;
          })
        )
      );
      assert.equal(calls, 0);
      s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
      s.lease.claimOwnerLease = async () => ({
        kind: 'claimed',
        token: { ownerId: 'owner', leaseGeneration: 2n },
        storeNow: new Date(0),
        leaseExpiresAt: new Date(1000)
      });
      await s.runtime.renewOwnerLeaseOnce();
      assert.throws(() => s.runtime.executionAllowed(s.a), {
        kind: internal.ZLinkFrameworkErrorKind.NotFound
      });
    });
  }
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`confirmed ownership loss discards before T (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      const {
        ZLinkLocationKeyCodec
      } = require('../../packages/framework/dist/runtime/locations/key-codec');
      s.runtime.notifyIfStale(
        { status: internal.ZLinkLocationWriteStatus.IgnoredStale },
        internal.ZLinkLocationKind.Actor,
        ZLinkLocationKeyCodec.encodeActorKey({ meshName: 'mesh', actorId: 'a' })
      );
      assert.equal(s.actors.getState('a'), undefined);
      assert.throws(() => s.runtime.executionAllowed(s.a), {
        kind: internal.ZLinkFrameworkErrorKind.NotFound
      });
      assert.equal(s.runtime.executionAllowed(s.b), true);
      assert.equal(s.runtime.executionAllowed(s.activation.spot), true);
      s.runtime.notifyIfStale(
        { status: internal.ZLinkLocationWriteStatus.IgnoredStale },
        internal.ZLinkLocationKind.Spot,
        ZLinkLocationKeyCodec.encodeSpotKey({ meshName: 'mesh', spotId: s.activation.spotId })
      );
      assert.equal(s.actors.getState('b'), undefined);
      assert.throws(() => s.runtime.executionAllowed(s.activation.spot), {
        kind: internal.ZLinkFrameworkErrorKind.NotFound
      });
      assert.equal(
        s.snapshot().storeOutage.discarded,
        false,
        'object loss is independent of the host cutoff'
      );
      await Promise.all(s.tasks);
    });
    test(`handler provider completion after T cannot start a Spot handler (provider=${provider}, mode=${mode})`, async (t) => {
      let finish, entered;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const resolver = {
        async create(type) {
          if (type.name !== 'Ping') return new type();
          entered();
          return new Promise((resolve) => {
            finish = () => resolve(new type());
          });
        }
      };
      const s = await objectScenario(t, provider, mode, resolver);
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      s.setNow(1299);
      const pending = s.spots.dispatchRoutedSpotRequest(
        s.activation.spotId,
        'Ping',
        {},
        { channelName: 'mesh' }
      );
      const rejected = assert.rejects(pending);
      await started;
      s.advance(1300);
      finish();
      await rejected;
      await Promise.all(s.tasks);
    });
  }
}

function actorRequestParts() {
  const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
  const {
    ZLinkBufferMessage
  } = require('../../packages/framework/dist/runtime/backend/runtime-message');
  return [
    new ZLinkBufferMessage(
      protocol.encodeStreamHeader({
        kind: protocol.ZLinkStreamMessageKind.Request,
        codec: protocol.ZLinkStreamCodec.Json,
        flags: protocol.ZLinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: 1n,
        name: 'Probe',
        metadata: new Map()
      })
    ),
    new ZLinkBufferMessage(Buffer.from('{}'))
  ];
}

test('discarding shared FIFO records releases mailbox notification membership', async () => {
  const serial = new internal.ZLinkSpotSerialTurnExecutor();
  const executor = new internal.ZLinkSpotSerialExecutor(
    serial,
    internal.ZLinkUserSpotExecutionMode.SpotWide,
    'room'
  );
  let finish, entered;
  const started = new Promise((resolve) => {
    entered = resolve;
  });
  const running = executor.executeSpot(() => {
    entered();
    return new Promise((resolve) => {
      finish = resolve;
    });
  });
  await started;
  let calls = 0;
  const pending = executor.executeActor('a', () => {
    calls++;
  });
  const rejected = assert.rejects(pending, /discarded/);
  executor.discardPendingApplication(new Error('discarded'));
  await rejected;
  finish();
  await running;
  await executor.executeActor('a', () => {
    calls++;
  });
  assert.equal(calls, 1);
  await executor.close();
});

for (const provider of [false, true]) {
  for (const phase of ['closing', 'relocation']) {
    test(`existing ${phase} lifecycle stays outside cutoff cleanup (provider=${provider})`, async (t) => {
      const s = await scenario(t, provider, true);
      const owner = {};
      let ready = true,
        discarded = 0;
      s.runtime.trackExecution(
        owner,
        () => ready,
        () => {
          discarded++;
        }
      );
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      ready = false;
      s.advance(1300);
      assert.equal(s.snapshot().storeOutage.discarded, true);
      assert.equal(discarded, 0);
      assert.equal(s.runtime.executionAllowed(owner), false);
      s.runtime.forgetExecution(owner);
    });
  }
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`Spot discard leaves relocating member to its existing lifecycle (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      const source = s.actors.getState('a');
      source.beginMove();
      s.setNow(200);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      s.advance(1300);
      assert.equal(s.spots.activations.activeActivations().length, 0);
      assert.equal(s.actors.getState('a'), source);
      assert.equal(source.actor, s.a);
      assert.equal(s.activation.resolveJoinedActor('a'), s.a);
      assert.equal(s.actors.getState('b'), undefined);
      assert.equal(s.runtime.executionAllowed(s.a), false);
      source.endMove();
      await Promise.all(s.tasks);
    });
  }
}

for (const provider of [false, true]) {
  test(`existing ownership commit registers the replacement Actor object (provider=${provider})`, async (t) => {
    const s = await objectScenario(t, provider, internal.ZLinkUserSpotExecutionMode.SpotWide);
    s.actors.getState('a').setRemoteActorPacketTarget({
      routerChannelId: 'mesh',
      targetNodeRid: 'old-remote',
      spotId: 'old-room'
    });
    const replacement = await s.actors.materializeTransferredActor(
      'a',
      'Player',
      undefined,
      undefined
    );
    assert.notStrictEqual(replacement.actor, s.a);
    const state = s.actors.getState('a');
    state.markLocationOwned();
    s.activation.commitActorJoin(replacement.actor);
    state.setJoinedSpot(s.activation.spotId, s.activation.spot, 1n, 1n);
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.advance(1299);
    assert.throws(
      () => s.runtime.executionAllowed(s.a),
      { kind: internal.ZLinkFrameworkErrorKind.NotFound },
      'retired object must not keep its execution registration'
    );
    assert.equal(s.runtime.executionAllowed(replacement.actor), true);
    await s.activation.executeActor('a', (serial) => serial.execute(() => undefined));
    s.runtime.discardExecution(replacement.actor);
  });
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`Closing owner loss leaves cleanup with the existing lifecycle (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      const seal = s.activation.executionBarrier.seal('close');
      const {
        ZLinkLocationKeyCodec
      } = require('../../packages/framework/dist/runtime/locations/key-codec');
      try {
        s.runtime.notifyIfStale(
          { status: internal.ZLinkLocationWriteStatus.IgnoredStale },
          internal.ZLinkLocationKind.Spot,
          ZLinkLocationKeyCodec.encodeSpotKey({ meshName: 'mesh', spotId: s.activation.spotId })
        );
        assert.throws(() => s.runtime.executionAllowed(s.activation.spot), {
          kind: internal.ZLinkFrameworkErrorKind.NotFound
        });
        assert.equal(
          s.spots.activations.activationForClose('mesh', s.activation.spotId),
          s.activation
        );
        assert.equal(s.actors.getState('a').actor, s.a);
        assert.equal(s.actors.getState('b').actor, s.b);
      } finally {
        s.runtime.discardExecution(s.a);
        s.runtime.discardExecution(s.b);
        s.activation.executionBarrier.abort(seal);
        await s.spots.drainForShutdown('mesh');
      }
    });
  }
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`local cleanup retires execution independently of Store release (provider=${provider}, mode=${mode})`, async (t) => {
      const s = await objectScenario(t, provider, mode);
      s.runtime.discardExecution(s.a);
      s.runtime.discardExecution(s.b);
      const key = { meshName: 'mesh', spotId: s.activation.spotId };
      const before = await s.store.resolveSpot(key);
      assert(before);
      const remove = s.runtime.removeSpot.bind(s.runtime);
      let finish, entered;
      const submitted = new Promise((resolve) => {
        entered = resolve;
      });
      s.runtime.removeSpot = async (...args) => {
        entered();
        await new Promise((resolve) => {
          finish = resolve;
        });
        return remove(...args);
      };
      const closing = s.spots.drainForShutdown('mesh');
      try {
        await submitted;
        s.setNow(200);
        s.fail();
        await s.runtime.renewOwnerLeaseOnce();
        s.advance(1299);
        assert.equal(s.runtime.executionAllowed(s.activation.spot), false);
        assert.deepEqual(await s.store.resolveSpot(key), before);
      } finally {
        s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
        await s.runtime.renewOwnerLeaseOnce();
        finish();
        await closing;
      }
    });
  }
}

for (const missingError of [false, true]) {
  test(`a terminal replay completes its protocol failure without starting an application handler (missing=${missingError})`, async () => {
    let allowed = true,
      entered,
      finish;
    const gate = new internal.ZLinkExecutionBarrier();
    gate.setExecutionPolicy(() => fixedPolicy(() => allowed));
    const serial = new internal.ZLinkSpotSerialTurnExecutor();
    serial.setExecutionBarrier(gate);
    const started = new Promise((resolve) => {
      entered = resolve;
    });
    const holding = serial.postBarrierTurn(() => {
      entered();
      return new Promise((resolve) => {
        finish = resolve;
      });
    });
    await started;
    let calls = 0;
    const terminal = missingError ? undefined : new Error('terminal reply');
    const queued = serial.execute(
      () => {
        calls++;
      },
      undefined,
      (failure) => {
        if (failure !== undefined) throw failure;
        calls++;
      }
    );
    const rejected = assert.rejects(queued, (error) =>
      missingError
        ? error.kind === internal.ZLinkFrameworkErrorKind.InternalFailure
        : error === terminal
    );
    allowed = false;
    serial.rejectPendingReplays(terminal);
    finish();
    await holding;
    await rejected;
    assert.equal(calls, 0);
    serial.closeAdmission();
    await serial.whenIdle();
  });
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`Spot-owned request keeps its authority when its submitting member is discarded (provider=${provider}, mode=${mode})`, async (t) => {
      let finishProvider;
      const providerReady = new Promise((resolve) => {
        finishProvider = resolve;
      });
      const resolver = {
        async create(type) {
          if (type.name === 'Ping') await providerReady;
          return new type();
        }
      };
      const s = await objectScenario(t, provider, mode, resolver);
      const target = await s.spots.create('mesh', s.Room, {});
      let finishActor, submitted, admitted, outcome;
      const actorReady = new Promise((resolve) => {
        submitted = resolve;
      });
      const targetAdmitted = new Promise((resolve) => {
        admitted = resolve;
      });
      const actorDone = new Promise((resolve) => {
        finishActor = resolve;
      });
      const actorTurn = s.activation.executeActor('a', (serial) =>
        serial.execute(() => {
          outcome = s.spots
            .dispatchRoutedSpotRequest(
              target.spotId,
              'Ping',
              {},
              { channelName: 'mesh', onAdmitted: admitted }
            )
            .then(
              (value) => ({ value }),
              (error) => ({ error })
            );
          submitted();
          return actorDone;
        })
      );
      try {
        await actorReady;
        await targetAdmitted;
        s.runtime.discardExecution(s.a);
      } finally {
        finishActor();
        finishProvider();
      }
      await actorTurn;
      const result = await outcome;
      assert.equal(result.error, undefined);
      assert.equal(result.value, 'handled');
      assert.strictEqual(s.actors.getState('b').actor, s.b);
      assert.strictEqual(
        s.spots.activations.activationForClose('mesh', s.activation.spotId),
        s.activation
      );
    });
  }
}

for (const provider of [false, true]) {
  for (const mode of [
    internal.ZLinkUserSpotExecutionMode.PerActor,
    internal.ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    test(`Spot timer keeps its authority after its registering member is discarded (provider=${provider}, mode=${mode})`, async (t) => {
      const { AsyncLocalStorage } = require('node:async_hooks');
      const s = await objectScenario(t, provider, mode);
      const schedule = s.clock.setTimeout.bind(s.clock);
      s.clock.setTimeout = (callback, delay) => schedule(AsyncLocalStorage.bind(callback), delay);
      let ticks = 0;
      class MemberTimer {
        handle() {
          ticks++;
        }
      }
      let finishActor, registered, registrationFailed, memberTimer;
      const registration = new Promise((resolve, reject) => {
        registered = resolve;
        registrationFailed = reject;
      });
      const actorDone = new Promise((resolve) => {
        finishActor = resolve;
      });
      const actorTurn = s.activation.executeActor('a', (serial) =>
        serial.execute(async () => {
          memberTimer = await s.activation.spot.context.addTimer('by-member', 100, MemberTimer);
          registered();
          await actorDone;
        })
      );
      void actorTurn.catch(registrationFailed);
      let finishTimer;
      let holdingTimer = Promise.resolve();
      try {
        await registration;
        if (mode === internal.ZLinkUserSpotExecutionMode.PerActor) {
          let holding;
          const entered = new Promise((resolve) => {
            holding = resolve;
          });
          holdingTimer = s.activation.serialExecutor.executeTimer('by-member', () => {
            holding();
            return new Promise((resolve) => {
              finishTimer = resolve;
            });
          });
          await entered;
        }
        let tickSubmitted;
        const queued = new Promise((resolve) => {
          tickSubmitted = resolve;
        });
        const execute = s.activation.serialExecutor.executeTimer.bind(s.activation.serialExecutor);
        t.mock.method(s.activation.serialExecutor, 'executeTimer', (name, operation, options) => {
          const pending = execute(name, operation, options);
          if (name === 'by-member') tickSubmitted();
          return pending;
        });
        s.advance(200);
        await queued;
        s.runtime.discardExecution(s.a);
      } finally {
        finishTimer?.();
        finishActor();
      }
      try {
        await holdingTimer;
        await actorTurn;
        await s.activation.serialExecutor.executeTimer('by-member', () => undefined);
        assert.equal(ticks, 1);
        assert.strictEqual(s.actors.getState('b').actor, s.b);
      } finally {
        await memberTimer?.cancel();
      }
    });
  }
}

for (const mode of [
  internal.ZLinkUserSpotExecutionMode.PerActor,
  internal.ZLinkUserSpotExecutionMode.SpotWide
]) {
  test(`ordinary Spot Actor and timer turns have no Location clock or policy allocation (mode=${mode})`, async (t) => {
    const policies = new Set();
    const setPolicy = internal.ZLinkExecutionBarrier.prototype.setExecutionPolicy;
    t.mock.method(
      internal.ZLinkExecutionBarrier.prototype,
      'setExecutionPolicy',
      function (capture, captureOwner) {
        return setPolicy.call(
          this,
          () => {
            const policy = capture();
            policies.add(policy);
            return policy;
          },
          captureOwner
        );
      }
    );
    const s = await objectScenario(t, false, mode);
    const actorTurn = () => s.activation.executeActor('a', (serial) => serial.execute(() => {}));
    // Executor construction may allocate its one policy.
    await actorTurn();
    await s.activation.serialExecutor.executeTimer('cost', () => {});
    const readsBefore = s.clockReads();
    const policiesBefore = policies.size;
    for (let n = 0; n < 20; n++) {
      await s.activation.serialExecutor.executeSpot(() => {});
      await actorTurn();
      await s.activation.serialExecutor.executeTimer('cost', () => {});
    }
    assert.deepEqual(
      { clockReads: s.clockReads() - readsBefore, policyClosures: policies.size - policiesBefore },
      { clockReads: 0, policyClosures: 0 },
      'ordinary turns must neither read the Location clock nor create policy closures'
    );
  });
}

test('application records keep the original operation without a policy wrapper', async () => {
  const {
    ZLinkSerialExecutionQueue
  } = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
  let captures = 0;
  const allowed = fixedPolicy(() => true);
  const queue = new ZLinkSerialExecutionQueue(async (record) => {
    assert.equal(record.operation, operation);
    record.resolve(record.operation());
    record.release();
  });
  queue.setExecutionPolicy(() => {
    captures++;
    return allowed;
  });
  const operation = () => 7;
  const before = captures;
  for (let n = 0; n < 20; n++) assert.equal(await queue.submit(operation), 7);
  assert.equal(captures - before, 0, 'records must not capture policies');
});

test('admission rejects unretained work after the owner deadline', async (t) => {
  const s = await scenario(t, false, true);
  const admission = new internal.ZLinkRuntimeAdmissionGate(() => s.runtime.ownerLeaseUsable);
  admission.setExecutionRuntime(() => s.runtime);
  admission.register('mesh');
  s.fail();
  s.setNow(200);
  await s.runtime.renewOwnerLeaseOnce();
  s.setNow(1000);
  await assert.rejects(
    admission.run(
      'mesh',
      'Actor request',
      () => assert.fail('must not be admitted'),
      internal.ReceiveKind.ActorRequest
    ),
    {
      kind: internal.ZLinkFrameworkErrorKind.ShuttingDown
    }
  );
  assert.equal(s.snapshot().storeOutage.discarded, false);
});

test('barrier claims share their execution policy', () => {
  const gate = new internal.ZLinkExecutionBarrier();
  const allowed = fixedPolicy(() => true);
  gate.setExecutionPolicy(() => allowed);
  const starts = new Set();
  for (let n = 0; n < 20; n++) {
    const claim = gate.enter();
    starts.add(claim.executionPolicy);
    claim.release();
  }
  assert.equal(starts.size, 1, 'claim creation must not allocate a policy');
});

test('Actor execution identity lookup reuses its active context', async () => {
  const {
    runZLinkActorExecution,
    currentZLinkActorExecution
  } = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
  await runZLinkActorExecution('a', 'spot', () => {
    const contexts = new Set();
    for (let n = 0; n < 20; n++) contexts.add(currentZLinkActorExecution());
    assert.equal(contexts.size, 1, 'turn policy lookup must not allocate identity views');
    assert.deepEqual(currentZLinkActorExecution(), { actorId: 'a', spotId: 'spot' });
  });
});

for (const mode of [
  internal.ZLinkUserSpotExecutionMode.PerActor,
  internal.ZLinkUserSpotExecutionMode.SpotWide
]) {
  test(`an existing executor follows a replacement Actor object (mode=${mode})`, async (t) => {
    const s = await objectScenario(t, false, mode);
    await s.activation.executeActor('a', (serial) => serial.execute(() => undefined));
    s.actors.getState('a').setRemoteActorPacketTarget({
      routerChannelId: 'mesh',
      targetNodeRid: 'old-remote',
      spotId: 'old-room'
    });
    const replacement = await s.actors.materializeTransferredActor(
      'a',
      'Player',
      undefined,
      undefined
    );
    const state = s.actors.getState('a');
    state.markLocationOwned();
    s.activation.commitActorJoin(replacement.actor);
    state.setJoinedSpot(s.activation.spotId, s.activation.spot, 1n, 1n);
    assert.throws(() => s.runtime.executionAllowed(s.a), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    await s.activation.executeActor('a', (serial) => serial.execute(() => undefined));
    s.runtime.discardExecution(replacement.actor);
  });
}

test('a discarded owner query still applies the existing T latch', async (t) => {
  const s = await scenario(t, false, true);
  const retired = {},
    retained = {};
  let discarded = 0;
  s.runtime.discardExecution(retired);
  s.runtime.trackExecution(
    retained,
    () => true,
    () => {
      discarded++;
    }
  );
  s.setNow(200);
  s.fail();
  await s.runtime.renewOwnerLeaseOnce();
  s.setNow(1300);
  assert.throws(() => s.runtime.executionAllowed(retired), {
    kind: internal.ZLinkFrameworkErrorKind.NotFound
  });
  assert.equal(s.snapshot().storeOutage.discarded, true);
  assert.equal(discarded, 1);
});

test('timer keeps its existing fallback when no Location execution policy exists', async (t) => {
  let fallbackOpen = true;
  const s = await objectScenario(
    t,
    false,
    internal.ZLinkUserSpotExecutionMode.PerActor,
    undefined,
    () => fallbackOpen
  );
  let ticks = 0;
  class Tick {
    handle() {
      ticks++;
    }
  }
  let complete, fail;
  const completed = new Promise((resolve, reject) => {
    complete = resolve;
    fail = reject;
  });
  const execute = s.activation.serialExecutor.executeTimer.bind(s.activation.serialExecutor);
  t.mock.method(s.activation.serialExecutor, 'executeTimer', (name, operation, options) => {
    const pending = execute(name, operation, options);
    if (name === 'fallback') void pending.then(complete, fail);
    return pending;
  });
  const timer = await s.activation.spot.context.addTimer('fallback', 100, Tick);
  s.admission.setExecutionRuntime(() => undefined);
  fallbackOpen = false;
  try {
    s.advance(200);
    await completed;
    assert.equal(ticks, 0, 'absence of a Location policy must keep the existing timer fallback');
  } finally {
    await timer.cancel();
  }
});

for (const provider of [false, true]) {
  for (const terminal of ['T', 'ownership', 'close', 'shutdown']) {
    test(`queued terminal kinds (${provider}/${terminal}) repeated 100 times`, async (t) => {
      for (let iteration = 0; iteration < 100; iteration++) {
        await t.test(String(iteration), async (t) => {
          const s = await objectScenario(t, provider, internal.ZLinkUserSpotExecutionMode.SpotWide);
          const serial = s.activation.serial;
          let closingCalls = 0;
          s.activation.spot.onClosing = () => {
            closingCalls++;
          };
          let completeSource, signalYielded;
          const source = new Promise((resolve) => {
            completeSource = resolve;
          });
          const yielded = new Promise((resolve) => {
            signalYielded = resolve;
          });
          const continuation = serial.execute(async () => {
            signalYielded();
            await serial.yieldPromise(source);
            assert.fail('continuation must not run');
          });
          let finish, signalStarted;
          const started = new Promise((resolve) => {
            signalStarted = resolve;
          });
          const continuationOutcome = Promise.allSettled([continuation]);
          await yielded;
          const running = serial.execute(
            () =>
              new Promise((resolve) => {
                finish = resolve;
                signalStarted();
              })
          );
          await started;
          let signalQueued;
          const allQueued = new Promise((resolve) => {
            signalQueued = resolve;
          });
          let admitted = 0;
          const onAdmitted = () => {
            if (++admitted === 3) signalQueued();
          };
          const execute = serial.execute;
          serial.execute = function (operation, options, ...rest) {
            return execute.call(
              this,
              operation,
              {
                ...options,
                onAdmitted() {
                  options?.onAdmitted?.();
                  onAdmitted();
                }
              },
              ...rest
            );
          };
          const prototype = internal.ZLinkSerialExecutionQueue.prototype;
          const submitContinuation = prototype.submitContinuation;
          prototype.submitContinuation = function (...args) {
            const result = submitContinuation.apply(this, args);
            onAdmitted();
            return result;
          };
          t.after(() => {
            prototype.submitContinuation = submitContinuation;
          });
          const application = serial.execute(() => assert.fail('application must not run'));
          const request = s.spots.dispatchRoutedSpotRequest(
            s.activation.spotId,
            'Ping',
            {},
            { channelName: 'mesh' }
          );
          const outcomes = Promise.allSettled([application, request]);
          completeSource();
          await allQueued;
          serial.execute = execute;
          prototype.submitContinuation = submitContinuation;
          s.setNow(200);
          s.fail();
          await s.runtime.renewOwnerLeaseOnce();
          try {
            if (terminal === 'T') s.advance(1300);
            else if (terminal === 'ownership') {
              const {
                ZLinkLocationKeyCodec
              } = require('../../packages/framework/dist/runtime/locations/key-codec');
              s.runtime.notifyIfStale(
                { status: internal.ZLinkLocationWriteStatus.IgnoredStale },
                internal.ZLinkLocationKind.Spot,
                ZLinkLocationKeyCodec.encodeSpotKey({
                  meshName: 'mesh',
                  spotId: s.activation.spotId
                })
              );
            } else {
              if (terminal === 'shutdown') s.admission.close();
              await s.runtime.stop();
              s.advance(1300);
              assert.throws(() => s.runtime.executionAllowed(s.activation.spot), {
                kind: internal.ZLinkFrameworkErrorKind.ShuttingDown
              });
              assert.equal(closingCalls, 0, 'Location stop must not call object closing');
              assert.equal(
                s.spots.activations.activationForClose('mesh', s.activation.spotId),
                s.activation,
                'Host owns object cleanup after Location stop'
              );
              assert.equal(s.actors.getState('a').actor, s.a);
              assert.equal(s.actors.getState('b').actor, s.b);
            }
          } finally {
            finish();
          }
          await running;
          const settled = [...(await outcomes), ...(await continuationOutcome)];
          for (const result of settled) {
            assert.equal(result.status, 'rejected');
            assert.equal(
              result.reason?.kind,
              terminal === 'T' || terminal === 'ownership'
                ? internal.ZLinkFrameworkErrorKind.NotFound
                : internal.ZLinkFrameworkErrorKind.ShuttingDown
            );
          }
          await Promise.all(s.tasks);
        });
      }
    });
  }
}

for (const provider of [false, true]) {
  test(`yield completion after T cleanup remains NotFound (${provider}) repeated 100 times`, async (t) => {
    for (let iteration = 0; iteration < 100; iteration++) {
      await t.test(String(iteration), async (t) => {
        const s = await objectScenario(t, provider, internal.ZLinkUserSpotExecutionMode.SpotWide);
        let complete, signalYielded;
        const source = new Promise((resolve) => {
          complete = resolve;
        });
        const yielded = new Promise((resolve) => {
          signalYielded = resolve;
        });
        const pending = s.activation.serial.execute(async () => {
          signalYielded();
          await s.activation.serial.yieldPromise(source);
          assert.fail('discarded continuation must not run');
        });
        const outcome = Promise.allSettled([pending]);
        await yielded;
        s.setNow(200);
        s.fail();
        await s.runtime.renewOwnerLeaseOnce();
        s.advance(1300);
        await Promise.all(s.tasks);
        complete();
        const [result] = await outcome;
        assert.equal(result.status, 'rejected');
        assert.equal(result.reason?.kind, internal.ZLinkFrameworkErrorKind.NotFound);
      });
    }
  });
}

test('a yielded turn waits for the same gate to reopen', async () => {
  let allowed = true,
    wake,
    complete,
    signalYielded;
  const source = new Promise((resolve) => {
    complete = resolve;
  });
  const yielded = new Promise((resolve) => {
    signalYielded = resolve;
  });
  const barrier = new internal.ZLinkExecutionBarrier();
  barrier.setExecutionPolicy(() => ({
    executionDecision: (_owner, onWake) => {
      if (allowed) return 'allowed';
      wake = onWake;
      return 'paused';
    },
    executionFailure: () => assert.fail('a pause is not a loss')
  }));
  const serial = new internal.ZLinkSpotSerialTurnExecutor();
  serial.setExecutionBarrier(barrier);
  let calls = 0;
  const pending = serial.execute(async () => {
    signalYielded();
    await serial.yieldPromise(source);
    calls++;
  });
  await yielded;
  allowed = false;
  complete();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(calls, 0);
  assert.equal(typeof wake, 'function');
  allowed = true;
  wake();
  await pending;
  assert.equal(calls, 1);
  await serial.whenIdle();
});

for (const mode of [
  internal.ZLinkUserSpotExecutionMode.PerActor,
  internal.ZLinkUserSpotExecutionMode.SpotWide
]) {
  test(`scheduled admission deadline closes Actor Spot and timer turns until renewal (mode=${mode})`, async (t) => {
    const s = await objectScenario(t, false, mode);
    let calls = 0;
    const submitters = [
      (operation) => s.activation.executeActor('a', (serial) => serial.execute(operation)),
      (operation) => s.activation.serialExecutor.executeSpot(operation),
      (operation) => s.activation.serialExecutor.executeTimer('deadline', operation)
    ];
    const turns = submitters.map(
      (submit) => () =>
        submit(() => {
          calls++;
        })
    );
    s.advance(999);
    for (const turn of turns) await turn();
    assert.equal(calls, 3, 'turns start immediately before the admission deadline');
    const submit = (turn) =>
      s.admission.run('mesh', 'bound request', turn, internal.ReceiveKind.SpotRequest);
    const finishes = [],
      holding = [];
    const holders =
      mode === internal.ZLinkUserSpotExecutionMode.PerActor ? submitters : [submitters[1]];
    for (const hold of holders) {
      let started;
      const ready = new Promise((resolve) => {
        started = resolve;
      });
      holding.push(
        hold(() => {
          started();
          return new Promise((resolve) => {
            finishes.push(resolve);
          });
        })
      );
      await ready;
    }
    let acceptedSettled = 0;
    const accepted = turns.map((turn) =>
      turn().finally(() => {
        acceptedSettled++;
      })
    );
    await new Promise((resolve) => setImmediate(resolve));
    s.advance(1001);
    assert.equal(s.runtime.ownerLeaseHealthy, true, 'heartbeat has not observed expiry');
    assert.equal(s.snapshot().storeOutage, undefined);
    const reads = s.clockReads();
    for (const turn of turns)
      await assert.rejects(submit(turn), { kind: internal.ZLinkFrameworkErrorKind.ShuttingDown });
    for (const finish of finishes) finish();
    await Promise.all(holding);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(acceptedSettled, 0, 'accepted turns wait behind the deadline latch');
    assert.equal(calls, 3);
    assert.equal(s.clockReads(), reads, 'closed admission uses its latch without reading time');
    s.setNow(999);
    for (const turn of turns)
      await assert.rejects(submit(turn), { kind: internal.ZLinkFrameworkErrorKind.ShuttingDown });
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
    await Promise.all(accepted);
    assert.equal(calls, 6, 'renewal resumes every accepted turn once');
    for (const turn of turns) await submit(turn);
    assert.equal(calls, 9);
    s.advance(1899);
    for (const turn of turns)
      await assert.rejects(submit(turn), { kind: internal.ZLinkFrameworkErrorKind.ShuttingDown });
    assert.equal(calls, 9);
  });
}

for (const mode of [
  internal.ZLinkUserSpotExecutionMode.PerActor,
  internal.ZLinkUserSpotExecutionMode.SpotWide
]) {
  test(`deadline latch preserves a managed tick before queue submission (mode=${mode})`, async (t) => {
    const s = await objectScenario(t, false, mode);
    const ticks = [];
    class Tick {
      handle(_spot, tick) {
        ticks.push(tick);
      }
    }
    let submitted = 0,
      complete;
    const delivered = new Promise((resolve) => {
      complete = resolve;
    });
    const execute = s.activation.serialExecutor.executeTimer.bind(s.activation.serialExecutor);
    t.mock.method(s.activation.serialExecutor, 'executeTimer', (name, operation, options) => {
      if (name === 'unqueued') submitted++;
      const result = execute(name, operation, options);
      if (name === 'unqueued') void result.then(complete);
      return result;
    });
    const timer = await s.activation.spot.context.addTimer('unqueued', 100, Tick);
    t.after(() => timer.cancel());
    s.advance(1001);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(submitted, 0, 'closed gate keeps the tick outside the serial queue');
    assert.equal(ticks.length, 0);
    s.setNow(999);
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
    await delivered;
    assert.equal(submitted, 1);
    assert.equal(ticks.length, 1);
    assert.equal(ticks[0].deliveryIndex, 1n);
    await timer.cancel();
  });
}

for (const mode of [
  internal.ZLinkUserSpotExecutionMode.PerActor,
  internal.ZLinkUserSpotExecutionMode.SpotWide
]) {
  test(`an unsubmitted Spot tick does not inherit its creator member authority (mode=${mode})`, async (t) => {
    const {
      runZLinkActorExecution
    } = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
    const s = await objectScenario(t, false, mode);
    let ticks = 0;
    class Tick {
      handle() {
        ticks++;
      }
    }
    const timer = await s.activation.spot.context.addTimer('member-context', 100, Tick);
    t.after(() => timer.cancel());
    s.runtime.discardExecution(s.a);
    await runZLinkActorExecution('a', s.activation.spotId, async () => {
      s.advance(200);
      await new Promise((resolve) => setImmediate(resolve));
    });
    assert.equal(ticks, 1, 'a Spot-owned tick ignores the discarded creator member context');
    await timer.cancel();
  });
}

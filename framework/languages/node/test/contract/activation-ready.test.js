'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');
const {
  ZLinkPublicSpotManager
} = require('../../packages/framework/dist/runtime/spots/spot-manager-public');
const {
  ZLinkUserSpotCreationCoordinator
} = require('../../packages/framework/dist/runtime/host/user-spot-creation-coordinator');
const {
  ZLinkInMemoryAuthorityStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-authority-store');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
function clock() {
  let now = 0;
  const scheduled = new Set();
  return {
    now: () => now,
    utcNow: () => 1800000000000 + now,
    setTimeout(callback, delay) {
      const handle = { callback, at: now + Number(delay) };
      scheduled.add(handle);
      return handle;
    },
    clearTimeout(handle) {
      scheduled.delete(handle);
    },
    advance(ms) {
      now += ms;
    },
    fire() {
      const handle = [...scheduled].sort((a, b) => a.at - b.at)[0];
      assert.ok(handle);
      scheduled.delete(handle);
      now = Math.max(now, handle.at);
      handle.callback();
    }
  };
}
function owner(options = {}) {
  const timerClock = clock();
  const ready = deferred();
  const release = deferred();
  const ticks = [];
  let close;
  let spot;
  let timer;
  let coordinator;
  class Tick {
    handle(room, tick) {
      ticks.push(tick);
      if (options.tickClose) close = room.context.close();
      options.delivered?.resolve();
    }
  }
  function requestClose(room, phase) {
    if (options.phase === phase) close = room.context.close();
  }
  class Room {
    configure() {
      spot = this;
      requestClose(this, 'configure');
    }
    async onCreate() {
      requestClose(this, 'create');
      if (options.timer) {
        timer = await this.context.addTimer('ready', 100, Tick, {
          overrunPolicy: options.policy,
          maxCatchUpTicks: 2
        });
        if (options.dueInCreate) timerClock.fire();
      }
      return { accepted: options.failure !== 'reject' };
    }
    onInitialize() {
      requestClose(this, 'initialize');
      if (options.failure === 'initialize') throw new Error('initialization failed');
    }
  }
  const base = new ZLinkInMemoryAuthorityStore({
    isOwnerLive: () => true,
    isTargetLive: () => true
  });
  const store = Object.create(base);
  store.commit = async (...args) => {
    ready.resolve();
    if (options.hold) await release.promise;
    if (options.failure === 'commit') throw new Error('Ready commit failed');
    return base.commit(...args);
  };
  const manager = new framework.DefaultZLinkSpotManager(
    {
      detachedTaskRunner: new ZLinkRuntimeTaskRunner(
        new ZLinkRuntimeTaskErrorSink(),
        new AbortController().signal
      ),
      spotFactories: [Room],
      userSpotExecutionMode: () => options.mode ?? framework.ZLinkUserSpotExecutionMode.SpotWide,
      beginUserClosingAuthority: (meshName, spotId, objectGeneration, onCommitted) =>
        coordinator.beginOwnerClose(
          { meshName, spotId, objectGeneration, nodeRid: 'node' },
          onCommitted
        )
    },
    timerClock
  );
  coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: 'mesh',
      nodeRid: 'node',
      nodeGeneration: 1n,
      owner: { ownerId: 'owner', leaseGeneration: 1n },
      isLocal: true
    })
  });
  const spots = new ZLinkPublicSpotManager({
    local: manager,
    coordinator,
    factories: new Map([['mesh', new Map([['room', { implementation: Room }]])]]),
    resolver: () => undefined,
    isLocalNode: () => true,
    defaultTimeoutMs: 1000
  });
  return {
    timerClock,
    ticks,
    ready,
    release,
    manager,
    get close() {
      return close;
    },
    get timer() {
      return timer;
    },
    get spot() {
      return spot;
    },
    create: () => spots.getOrCreate('ready-room', 'room').inMesh('mesh').submit(),
    async dispose() {
      release.resolve();
      await timer?.cancel();
      await manager.abortUserSpotPublication('mesh', 'ready-room');
      const activation = manager.resolveRelocationActivation('mesh', 'ready-room');
      if (activation)
        await manager.closeUserWithAuthority('mesh', 'ready-room', (onCommitted) =>
          coordinator.beginOwnerClose(
            {
              meshName: 'mesh',
              spotId: 'ready-room',
              objectGeneration: activation.objectGeneration,
              nodeRid: 'node'
            },
            onCommitted
          )
        );
    }
  };
}
async function drainDue(owner) {
  const activation = owner.manager.activations.activationForClose('mesh', 'ready-room');
  assert.ok(activation);
  // A real due notification must either reach admission or expose the old callback.
  for (
    let i = 0;
    i < 10000 &&
    activation.executionBarrier.admissionWaiters.length === 0 &&
    owner.ticks.length === 0;
    i++
  )
    await Promise.resolve();
  assert.ok(activation.executionBarrier.admissionWaiters.length > 0 || owner.ticks.length > 0);
  await activation.serial.postBarrierTurn(() => undefined);
}
for (const mode of [
  framework.ZLinkUserSpotExecutionMode.SpotWide,
  framework.ZLinkUserSpotExecutionMode.PerActor
]) {
  for (const phase of ['configure', 'create', 'initialize']) {
    for (const failure of [undefined, 'initialize'])
      test(`Ready lifecycle context Close: ${mode}/${phase}/${failure ?? 'success'}`, async (t) => {
        const runtime = owner({
          mode,
          phase,
          failure,
          timer: !!failure,
          dueInCreate: !!failure,
          hold: !failure
        });
        t.after(() => runtime.dispose());
        if (failure)
          await assert.rejects(runtime.create(), /initialization|materialization|creation/i);
        else {
          const creation = runtime.create();
          await runtime.ready.promise;
          let settled = false;
          runtime.close.then(() => {
            settled = true;
          });
          await runtime.manager.activations
            .activationForClose('mesh', 'ready-room')
            .serial.postBarrierTurn(() => undefined);
          assert.equal(settled, false);
          runtime.release.resolve();
          await creation;
        }
        assert.ok(runtime.close);
        assert.equal(await runtime.close, !failure);
        if (failure) {
          assert.equal(runtime.timer.isDisposed, true);
          assert.equal(runtime.ticks.length, 0);
        }
      });
  }
  for (const failure of ['reject', 'commit'])
    test(`Ready failure discards due timer and Close: ${mode}/${failure}`, async (t) => {
      const runtime = owner({ mode, phase: 'create', failure, timer: true, dueInCreate: true });
      t.after(() => runtime.dispose());
      if (failure === 'commit') await assert.rejects(runtime.create(), /Ready commit.*failed/i);
      else await runtime.create();
      assert.equal(await runtime.close, false);
      assert.equal(runtime.timer.isDisposed, true);
      assert.equal(runtime.ticks.length, 0);
    });
  for (const policy of [
    framework.ZLinkTimerOverrunPolicy.DelayNextTick,
    framework.ZLinkTimerOverrunPolicy.SkipLateTicks,
    framework.ZLinkTimerOverrunPolicy.CatchUpBounded
  ])
    test(`Ready commit holds due timer: ${mode}/${policy}`, async (t) => {
      const delivered = deferred();
      const runtime = owner({ mode, timer: true, hold: true, policy, delivered, tickClose: true });
      t.after(() => runtime.dispose());
      const creation = runtime.create();
      await runtime.ready.promise;
      runtime.timerClock.advance(500);
      runtime.timerClock.fire();
      await drainDue(runtime);
      assert.equal(runtime.ticks.length, 0);
      runtime.release.resolve();
      await creation;
      await delivered.promise;
      assert.equal(runtime.ticks.length, 1);
      assert.equal(runtime.ticks[0].deliveryIndex, 1n);
      assert.equal(
        runtime.ticks[0].scheduledIndex,
        policy === framework.ZLinkTimerOverrunPolicy.SkipLateTicks
          ? 5n
          : policy === framework.ZLinkTimerOverrunPolicy.CatchUpBounded
            ? 4n
            : 1n
      );
      assert.equal(await runtime.close, true);
    });
  test(`Ready pending timer cancellation preserves generation: ${mode}`, async (t) => {
    const runtime = owner({ mode, timer: true, hold: true });
    t.after(() => runtime.dispose());
    const creation = runtime.create();
    await runtime.ready.promise;
    runtime.timerClock.fire();
    await drainDue(runtime);
    const stale = runtime.timer;
    await stale.cancel();
    const delivered = deferred();
    class Replacement {
      handle() {
        runtime.ticks.push('replacement');
        delivered.resolve();
      }
    }
    const replacement = await runtime.spot.context.addTimer('ready', 100, Replacement);
    t.after(() => replacement.cancel());
    runtime.timerClock.fire();
    await drainDue(runtime);
    assert.equal(runtime.ticks.length, 0);
    runtime.release.resolve();
    await creation;
    await delivered.promise;
    await stale.timer.running;
    assert.deepEqual(runtime.ticks, ['replacement']);
  });
}

test('Instance initialization Close waits for first handler, durable cursor and recovery pointer release', async (t) => {
  const {
    ZLinkInstanceActivationAuthority
  } = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
  const {
    ServiceStatefulRuntime
  } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
  const {
    ZLinkSpotLocationClaims
  } = require('../../packages/framework/dist/runtime/locations/spot-location-claims');
  const {
    encodeAuthorityKey
  } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
  const {
    decodeServiceInstanceAuthorityPayload
  } = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
  const {
    ApplicationJobQueue,
    resolveApplicationJobQueueConfiguration
  } = require('../../packages/framework/dist/runtime/host/application-job-queue');
  const {
    ApplicationIngressRecordOwner
  } = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
  const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const {
    encodeApplicationPayload
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const errors = [];
  const store = new ZLinkInMemoryAuthorityStore({
    isOwnerLive: () => true,
    isTargetLive: () => true
  });
  const key = encodeAuthorityKey('instance_spot', 'cold-ready');
  const held = { ready: deferred(), cursor: deferred(), pointer: deferred() };
  const release = {
    ready: deferred(),
    cursor: deferred(),
    pointer: deferred(),
    handler: deferred()
  };
  const commit = store.commit.bind(store);
  store.commit = async (...args) => {
    held.ready.resolve();
    await release.ready.promise;
    return commit(...args);
  };
  const compare = store.compareExchangeAuthority.bind(store);
  store.compareExchangeAuthority = async (...args) => {
    const payload =
      args[2].kind === 'put' ? decodeServiceInstanceAuthorityPayload(args[2].payload) : undefined;
    if (payload?.state === 'ready') {
      const stage = payload.activationRecovery === undefined ? 'pointer' : 'cursor';
      held[stage].resolve();
      await release[stage].promise;
    }
    return compare(...args);
  };
  const blobs = new Map();
  const relocationStore = {
    async put(reference, bytes, retention) {
      blobs.set(reference.value, Buffer.from(bytes));
      return { kind: 'stored', storeNow: new Date(), expiresAt: new Date(Date.now() + retention) };
    },
    async read(reference) {
      const bytes = blobs.get(reference.value);
      return bytes === undefined
        ? { kind: 'missing', storeNow: new Date() }
        : { kind: 'found', bytes, storeNow: new Date(), expiresAt: new Date(Date.now() + 60000) };
    },
    async delete(reference) {
      blobs.delete(reference.value);
    }
  };
  let ingress;
  const admitted = deferred();
  const foundation = new ServiceStatefulRuntime(
    {
      topology: {},
      setServiceIngress(handler) {
        ingress = handler;
      },
      mailbox: {
        tryEnqueue(record) {
          admitted.resolve(record);
          return true;
        }
      }
    },
    'node',
    1n
  );
  foundation.setDispatchErrorReporter(
    {
      captureEnabled: () => true,
      flow: { flowCreationEnabled: () => false, accepts: () => false },
      report: (e) => errors.push(String(e.error))
    },
    'mesh'
  );
  const claims = new ZLinkSpotLocationClaims({}, store);
  let close;
  let closing = false;
  class Instance {
    onInitialize() {
      close = this.context.close();
    }
    async onClosing() {
      closing = true;
      const snapshot = await store.readAuthority(key);
      assert.equal(
        decodeServiceInstanceAuthorityPayload(snapshot.payload).activationRecovery,
        undefined
      );
    }
  }
  const manager = new framework.DefaultZLinkSpotManager({
    detachedTaskRunner: new ZLinkRuntimeTaskRunner(
      new ZLinkRuntimeTaskErrorSink(),
      new AbortController().signal
    ),
    spotFactories: [],
    instanceSpotFactories: new Map([['mesh', new Map([['instance', Instance]])]]),
    instanceSpotApplicationTargetProvider: (_, spotId) =>
      foundation.instanceSpotApplicationTarget(spotId),
    beginInstanceClosingAuthority: (...args) => claims.beginInstanceClosing(...args)
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistrationWithBuilder(() => {})
  });
  host.registerInstanceApplicationLifecycle('mesh', foundation, manager);
  foundation.registerAsyncInstanceActivationAuthority(
    new ZLinkInstanceActivationAuthority({
      store,
      relocationStore,
      meshName: 'mesh',
      owner: () => ({ ownerId: 'owner', leaseGeneration: 1n }),
      onReady: (target, route) =>
        claims.trackInstanceAuthority({
          meshName: 'mesh',
          spotId: target.targetSpotId,
          stableType: target.stableType,
          nodeRid: route.targetNodeRid,
          nodeGeneration: route.targetNodeGeneration,
          objectGeneration: route.objectGeneration,
          authorityOwnerGeneration: route.authorityOwnerGeneration,
          ownerId: route.ownerId,
          ownerLeaseGeneration: route.leaseGeneration,
          storeVersion: route.storeVersion
        })
    })
  );
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 1n));
  const job = ApplicationIngressRecordOwner.create(
    queue,
    await queue.acquire(undefined, 'remote'),
    { close() {} }
  );
  t.after(() => {
    for (const gate of Object.values(release)) gate.resolve();
    job.close();
    foundation.close();
  });
  const target = {
    targetSpotId: 'cold-ready',
    stableType: 'instance',
    targetNodeRid: 'node',
    targetNodeGeneration: 1n,
    targetMeshName: 'mesh',
    descriptorVersion: '1'
  };
  ingress({
    parts: [
      wire.encodeInstanceSpotActivationHeader(
        target,
        1n,
        'node',
        undefined,
        'send',
        { high: 1n, low: 2n },
        BigInt(Date.now() + 10000)
      ),
      encodeApplicationPayload({
        packetName: 'Notice',
        contentType: 'application/json',
        payload: Buffer.from('{}')
      })
    ],
    sourceRoutingId: 'node',
    applicationJobOwner: job
  });
  await held.ready.promise;
  let settled = false;
  close.then(() => {
    settled = true;
  });
  assert.equal(settled, false);
  assert.equal(closing, false);
  release.ready.resolve();
  const mailbox = await admitted.promise;
  const handlerEntered = deferred();
  mailbox.stateful.onHandlerTurnStarted?.();
  const handler = manager.executeOnSpot(Instance, 'cold-ready', async () => {
    handlerEntered.resolve();
    await release.handler.promise;
  });
  await handlerEntered.promise;
  assert.equal(settled, false);
  assert.equal(closing, false);
  release.handler.resolve();
  await handler;
  const terminal = mailbox.stateful.onTerminalCompletion();
  await held.cursor.promise;
  assert.equal(settled, false);
  assert.equal(closing, false);
  release.cursor.resolve();
  await held.pointer.promise;
  const cursor = decodeServiceInstanceAuthorityPayload(
    (await store.readAuthority(key)).payload
  ).activationRecovery;
  assert.equal(cursor.replayCursor, cursor.inboxSequence);
  assert.equal(settled, false);
  assert.equal(closing, false);
  release.pointer.resolve();
  await terminal;
  assert.equal(await close, true);
  assert.equal(closing, true);
  assert.equal((await store.readAuthority(key)).kind, 'missing');
  mailbox.applicationJob.close();
  assert.deepEqual(errors, []);
});

for (const mode of [
  framework.ZLinkUserSpotExecutionMode.SpotWide,
  framework.ZLinkUserSpotExecutionMode.PerActor
]) {
  for (const phase of ['configure', 'initialize'])
    for (const members of [0, 1]) {
      test(`Target staging Close stays behind Ready and preserves membership: ${mode}/${phase}/${members}`, async (t) => {
        const timerClock = clock();
        let close;
        let authorityCalls = 0;
        let closing = 0;
        const ticks = [];
        class Tick {
          handle(_spot, tick) {
            ticks.push(tick);
          }
        }
        class Target {
          async configure() {
            if (phase === 'configure') close = this.context.close();
            await this.context.addTimer('restored', 100, Tick);
          }
          async onInitialize() {
            if (phase === 'initialize') close = this.context.close();
            await this.context.addTimer('fresh', 100, Tick);
          }
          onClosing() {
            closing++;
          }
        }
        const manager = new framework.DefaultZLinkSpotManager(
          {
            detachedTaskRunner: new ZLinkRuntimeTaskRunner(
              new ZLinkRuntimeTaskErrorSink(),
              new AbortController().signal
            ),
            spotFactories: [Target],
            userSpotExecutionMode: () => mode,
            beginUserClosingAuthority: async (_mesh, _spot, generation, onCommitted) => {
              assert.equal(generation, 7n);
              authorityCalls++;
              onCommitted();
              return { release: async () => undefined };
            }
          },
          timerClock
        );
        const activation = await manager.prepareRelocationSpot(
          'mesh',
          'user_spot',
          'target',
          Target,
          'target-ready',
          7n,
          9n,
          [
            {
              name: 'restored',
              handlerType: 'Tick',
              periodMs: 100,
              overrunPolicy: framework.ZLinkTimerOverrunPolicy.CatchUpBounded,
              maxCatchUpTicks: 2,
              stopOnUnhandledException: true,
              startedAtUnixMs: timerClock.utcNow() - 700,
              deliveryIndex: 5n,
              lastScheduledIndex: 5n,
              nextDueAtUnixMs: timerClock.utcNow() - 100,
              pendingTicks: [
                {
                  deliveryIndex: 6n,
                  scheduledIndex: 6n,
                  scheduledAtUnixMs: timerClock.utcNow() - 100,
                  skippedTicks: 0n
                }
              ]
            }
          ]
        );
        t.after(() => manager.abortRelocationSpot(activation));
        if (members)
          activation.commitActorJoin({ actorId: 'member', context: { actorId: 'member' } });
        let settled = false;
        if (close)
          close.then(() => {
            settled = true;
          });
        await activation.serial.postBarrierTurn(() => undefined);
        assert.equal(settled, false);
        assert.equal(authorityCalls, 0);
        assert.equal(closing, 0);
        assert.deepEqual(ticks, []);
        const published = manager.publishRelocationSpot(activation, async () => {
          assert.ok(close);
          close.then(() => {
            settled = true;
          });
          assert.equal(settled, false);
          assert.equal(authorityCalls, 0);
          assert.deepEqual(ticks, []);
          const snapshots = await activation.timers.captureRelocation();
          assert.equal(
            snapshots.find((timer) => timer.name === 'restored').pendingTicks[0].deliveryIndex,
            6n
          );
        });
        await published;
        assert.equal(await close, members === 0);
        assert.equal(authorityCalls, members === 0 ? 1 : 0);
        assert.equal(closing, members === 0 ? 1 : 0);
        if (members) {
          timerClock.fire();
          for (let i = 0; i < 10000 && ticks.length === 0; i++) await Promise.resolve();
          assert.equal(ticks[0].deliveryIndex, 6n);
        }
      });
    }
}

test('Cold Instance staging is hidden and failed Ready discards initialization Close', async () => {
  let close;
  let authorityCalls = 0;
  class Instance {
    onInitialize() {
      close = this.context.close();
    }
  }
  const manager = new framework.DefaultZLinkSpotManager({
    detachedTaskRunner: new ZLinkRuntimeTaskRunner(
      new ZLinkRuntimeTaskErrorSink(),
      new AbortController().signal
    ),
    spotFactories: [],
    instanceSpotFactories: new Map([['mesh', new Map([['instance', Instance]])]]),
    beginInstanceClosingAuthority: async () => {
      authorityCalls++;
      assert.fail('unpublished incarnation cannot begin Closing');
    }
  });
  await manager.materializeInstance(
    'mesh',
    'instance',
    'unpublished-cold',
    1n,
    undefined,
    'first-terminal'
  );
  assert.equal(manager.isInstanceMaterialized('mesh', 'instance', 'unpublished-cold'), false);
  await manager.discardInstance('mesh', 'unpublished-cold');
  assert.equal(await close, false);
  assert.equal(authorityCalls, 0);
  assert.equal(manager.activations.activationForClose('mesh', 'unpublished-cold'), undefined);
});

for (const boundary of ['register', 'publish']) {
  test(`Ready publication pushes admission without resolve after ${boundary}`, async (t) => {
    const runtime = owner();
    t.after(() => runtime.dispose());
    const registry = runtime.manager.activations;
    const mutate = registry[boundary].bind(registry);
    let publications = 0;
    registry[boundary] = (...args) => {
      const published = mutate(...args);
      publications++;
      const resolve = registry.resolve;
      registry.resolve = () => assert.fail('Publication must push admission without resolve');
      queueMicrotask(() => {
        registry.resolve = resolve;
      });
      return published;
    };
    await runtime.create();
    assert.equal(publications, 1);
    assert.equal(
      registry.activationForClose('mesh', 'ready-room').serialExecutor.isPreparing,
      false
    );
  });
}

const cancellationCases = [];
for (const mode of [
  framework.ZLinkUserSpotExecutionMode.SpotWide,
  framework.ZLinkUserSpotExecutionMode.PerActor
])
  for (const phase of ['create', 'initialize'])
    for (const native of [false, true])
      cancellationCases.push({ mode, phase, native, instance: false });
cancellationCases.push({
  mode: framework.ZLinkUserSpotExecutionMode.SpotWide,
  phase: 'initialize',
  native: false,
  instance: true
});
for (const { mode, phase, native, instance } of cancellationCases) {
  test(
    `Preparation cancellation releases ownership: ${mode}/${phase}/native=${native}/instance=${instance}`,
    { timeout: 10000 },
    async (t) => {
      const entered = deferred();
      const release = deferred();
      const controller = new AbortController();
      const timerClock = clock();
      let activation;
      let timer;
      let close;
      let closing = 0;
      let authorityCalls = 0;
      let initialized = 0;
      let ticks = 0;
      let messages = 0;
      let nativeDisposes = 0;
      let activeAdmissions = 0;
      class Tick {
        handle() {
          ticks++;
        }
      }
      async function hold(room) {
        timer = await room.context.addTimer('cancel-ready', 100, Tick);
        close = room.context.close();
        close.catch(() => undefined);
        activation = manager.closeOperations.get(`mesh\0${String(room.context.spotId)}`).activation;
        entered.resolve();
        await release.promise;
      }
      class Room {
        async onCreate() {
          if (phase === 'create') await hold(this);
          return { accepted: true };
        }
        async onInitialize() {
          initialized++;
          if (phase === 'initialize') await hold(this);
        }
        onClosing() {
          closing++;
        }
      }
      const manager = new framework.DefaultZLinkSpotManager(
        {
          detachedTaskRunner: new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeTaskErrorSink(),
            new AbortController().signal
          ),
          spotFactories: [Room],
          instanceSpotFactories: new Map([['mesh', new Map([['cancel', Room]])]]),
          userSpotExecutionMode: () => mode,
          activationAdmission: {
            acquire: async () => {
              activeAdmissions++;
              return () => {
                activeAdmissions--;
              };
            }
          },
          beginUserClosingAuthority: async (_mesh, _spot, _generation, onCommitted) => {
            authorityCalls++;
            onCommitted();
            return { release: async () => undefined };
          },
          beginInstanceClosingAuthority: async (_mesh, _spot, onCommitted) => {
            authorityCalls++;
            onCommitted();
            return { release: async () => undefined };
          },
          createNativeSpot: native
            ? (_mesh, spotId) => ({
                routingId: spotId,
                lifecycleGeneration: 1n,
                setDispatchHandler() {},
                setSubscription() {},
                subscribe() {
                  return false;
                },
                recvActorLifecycle() {
                  return null;
                },
                drainReply() {
                  return 0;
                },
                drainChannelReply() {
                  return 0;
                },
                recvRoute() {
                  return false;
                },
                onSendReady() {},
                async dispose() {
                  nativeDisposes++;
                }
              })
            : undefined
        },
        timerClock
      );
      let settled = false;
      const creation = instance
        ? manager.materializeInstance(
            'mesh',
            'cancel',
            'cancel-cold',
            1n,
            controller.signal,
            'first-terminal'
          )
        : manager.create('mesh', Room, undefined, controller.signal);
      const creationOutcome = creation.then(
        (value) => {
          settled = true;
          return { value };
        },
        (error) => {
          settled = true;
          return { error };
        }
      );
      t.after(async () => {
        release.resolve();
        await creationOutcome;
        await close?.catch(() => undefined);
        if (activation) {
          await activation.timers.dispose();
          await activation.serialExecutor.finishPreparation();
          await activation.serialExecutor.close();
          manager.activations.finishClose('mesh', activation.spotId);
        }
      });
      await Promise.race([
        entered.promise,
        creationOutcome.then((outcome) => {
          throw outcome.error ?? new Error('Creation finished before the held callback');
        })
      ]);
      assert.equal(activation.serialExecutor.isPreparing, true);
      if (instance) assert.equal(activeAdmissions, 1);
      timerClock.fire();
      const application = activation.serial.execute(() => {
        messages++;
      });
      const applicationOutcome = application.then(
        (value) => ({ value }),
        (error) => ({ error })
      );
      for (let i = 0; i < 10000 && activation.executionBarrier.admissionWaiters.length < 2; i++)
        await Promise.resolve();
      assert.ok(activation.executionBarrier.admissionWaiters.length >= 2);
      assert.equal(ticks, 0);
      assert.equal(messages, 0);
      controller.abort();
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(settled, true, 'creation must terminate without releasing the held callback');
      const outcome = await creationOutcome;
      assert.equal(outcome.error?.name, 'AbortError');
      assert.equal(activation.serialExecutor.isPreparing, false);
      assert.equal(activeAdmissions, 0);
      assert.equal(await close, false);
      assert.equal(timer.isDisposed, true);
      assert.ok((await applicationOutcome).error);
      assert.equal(authorityCalls, 0);
      assert.equal(closing, 0);
      assert.equal(ticks, 0);
      assert.equal(messages, 0);
      assert.equal(initialized, phase === 'create' ? 0 : 1);
      assert.equal(nativeDisposes, native ? 1 : 0);
      assert.equal(manager.activations.resolve('mesh', activation.spotId), undefined);
      release.resolve();
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(manager.activations.resolve('mesh', activation.spotId), undefined);
      assert.equal(authorityCalls, 0);
      assert.equal(messages, 0);
      assert.equal(ticks, 0);
      assert.equal(initialized, phase === 'create' ? 0 : 1);
    }
  );
}

for (const instance of [false, true]) {
  test(`Pre-cancelled preparation does not construct an activation: instance=${instance}`, async () => {
    const cancellation = new AbortController();
    cancellation.abort();
    let constructed = 0;
    let nativeCreated = 0;
    class Room {
      constructor() {
        constructed++;
      }
    }
    const manager = new framework.DefaultZLinkSpotManager({
      detachedTaskRunner: new ZLinkRuntimeTaskRunner(
        new ZLinkRuntimeTaskErrorSink(),
        new AbortController().signal
      ),
      spotFactories: [Room],
      instanceSpotFactories: new Map([['mesh', new Map([['cancel', Room]])]]),
      createNativeSpot: () => {
        nativeCreated++;
        return undefined;
      }
    });
    await assert.rejects(
      instance
        ? manager.materializeInstance('mesh', 'cancel', 'pre-cancelled', 1n, cancellation.signal)
        : manager.create('mesh', Room, undefined, cancellation.signal),
      { name: 'AbortError' }
    );
    assert.equal(constructed, 0);
    assert.equal(nativeCreated, 0);
  });
}

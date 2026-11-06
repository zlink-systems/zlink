const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const { createMeshHost } = require('./fixtures/mesh-host');
const { createRelayHost } = require('./fixtures/relay-host');
const assert = require('node:assert/strict');
const test = require('node:test');
const { performance } = require('node:perf_hooks');
const { ZLinkInternalMeshNodeWriteIntent } = require('../../packages/framework/dist/runtime/locations/domain-store-contract');
const { resolveModuleProviders } = require('./helpers/nestjs-test-utils');
const {
  fixedRidHostClock,
  finishFixedRidHostStartup
} = require('../helpers/fixed-rid-host-clock');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');
const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const { BoundedReplayMap } = require('../../packages/framework/dist/runtime/host/bounded-replay-map');
const detachedTaskRunner = new ZLinkRuntimeTaskRunner(
  new ZLinkRuntimeTaskErrorSink(),
  new AbortController().signal
);
const nestjs = require('../../packages/nestjs/dist');

for (const emptyFirstPage of [true, false]) {
  test(`Actor placement consumes continuation after ${emptyFirstPage ? 'empty page' : 'candidate and preserves Store failure'}`, async () => {
    const provider = new framework.ZLinkInMemoryProviderLocationStore();
    const { host, meshRuntime: hostMesh } = createMeshHost({
      registration: framework.createFrameworkRegistration({
        locations: { storeInstance: provider },
        spotNodes: { play: { router: { bind: 'tcp://local-play', routingId: 'node-a' }, objectRole: 'client' } }
      })
    });
    const descriptor = {
      meshName: 'play', rid: 'node-a', lifecycleGeneration: 1n,
      state: framework.ZLinkFrameworkRuntimeState.Serving, objectRole: 'server',
      placementWeight: 100, entrySpotId: 'entry', ownerId: 'owner', leaseGeneration: 1n,
      objectCapabilities: [{ objectKind: 'actor', stableType: 'SessionActor', entryAccepted: true }],
      populationCapacity: { actors: { active: 0, reserved: 0, limit: 1000 } }
    };
    const calls = [];
    const failure = new Error('descriptor continuation failed');
    const store = host.locationOwner.locationStore();
    store.listMeshNodes = async (meshName, page, signal) => {
      calls.push({ meshName, page, signal });
      if (page?.continuationToken === 'next') {
        if (!emptyFirstPage) throw failure;
        return { items: [descriptor] };
      }
      return { items: emptyFirstPage ? [] : [descriptor], continuationToken: 'next' };
    };
    host.createPublicSpotManager({});
    Object.assign(hostMesh, { meshNode: () => ({ status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n }) }) });
    const signal = new AbortController().signal;
    const select = () => host.actorPlacement.options.target('play', 'SessionActor', signal);
    if (emptyFirstPage) {
      assert.equal((await select()).nodeRid, 'node-a');
    } else {
      await assert.rejects(select, error => error === failure);
    }
    assert.equal(calls.length, 2);
    assert.equal(calls[1].page.continuationToken, 'next');
    assert.equal(calls[1].signal, signal);
  });
}

test('bounded terminal replay refreshes recency and evicts only the oldest record', () => {
  assert.throws(() => new BoundedReplayMap(0), /positive safe integer/u);
  assert.throws(() => new BoundedReplayMap(1.5), /positive safe integer/u);

  const replay = new BoundedReplayMap(2);
  replay.remember('first', { terminal: 'accepted-1' });
  replay.remember('second', { terminal: 'accepted-2' });
  replay.touch('first');
  replay.remember('third', { terminal: 'accepted-3' });

  assert.equal(replay.get('second'), undefined);
  assert.deepEqual(
    [...replay].map(([key, value]) => [key, value.terminal]),
    [['first', 'accepted-1'], ['third', 'accepted-3']]
  );

  replay.remember('first', { terminal: 'accepted-1-replayed' });
  replay.touch('missing');
  assert.deepEqual(
    [...replay].map(([key, value]) => [key, value.terminal]),
    [['third', 'accepted-3'], ['first', 'accepted-1-replayed']]
  );
  assert.equal(replay.delete('missing'), false);
  assert.equal(replay.delete('third'), true);
  replay.clear();
  assert.deepEqual([...replay], []);
});

test('framework and NestJS builders register separate location and relocation stores', async () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const relocationStore = {};

  const options = framework.createFrameworkOptions((builder) => {
    builder.addLocationStore(store);
    builder.addRelocationStore(relocationStore);
    const locations = builder.configureLocations();
    locations
      .ownerLeaseRenewIntervalMs(123)
      .sessionRelocationSealTimeoutMs(1_234);
    assert.equal(typeof locations.maxActiveOutboundRelocations, 'undefined');
    assert.equal(typeof locations.maxActiveInboundRelocations, 'undefined');
    assert.equal(typeof locations.maxConcurrentRelocationCaptures, 'undefined');
    assert.equal(typeof locations.maxConcurrentRelocationRestores, 'undefined');
    assert.equal(typeof locations.maxRelocationPayloadInFlightBytes, 'undefined');
  });
  const registration = framework.createFrameworkRegistration(options);
  assert.equal(registration.locations.storeInstance, store);
  assert.equal(registration.locations.relocationStoreInstance, relocationStore);
  assert.equal(registration.locations.options.ownerLeaseRenewIntervalMs, 123);
  assert.equal(registration.locations.options.sessionRelocationSealTimeoutMs, 1_234);

  const nestBuilder = nestjs.zlinkFramework()
    .addLocationStore(store)
    .addRelocationStore(relocationStore);
  nestBuilder.configureLocations()
    .ownerLeaseRenewIntervalMs(100)
    .ownerLeaseRenewTimeoutMs(30)
    .ownerLeaseFencingMarginMs(50)
    .ownerLeaseTtlMs(456)
    .sessionRelocationSealTimeoutMs(2_345);
  const nestModule = nestjs.ZLinkModule.forRoot(nestBuilder.build());
  const nestRegistration = await resolveFrameworkRegistration(nestModule);
  assert.equal(nestRegistration.locations.storeInstance, store);
  assert.equal(nestRegistration.locations.relocationStoreInstance, relocationStore);
  assert.equal(nestRegistration.locations.options.ownerLeaseTtlMs, 456);
  assert.equal(nestRegistration.locations.options.sessionRelocationSealTimeoutMs, 2_345);

});

test('owner lease startup relation includes two timeouts when renewal is slower than its interval', () => {
  const defaults = framework.createFrameworkOptions(() => {});
  assert.doesNotThrow(() => framework.createFrameworkRegistration(defaults));
  const invalid = framework.createFrameworkOptions((builder) => {
    builder
      .configureLocations()
      .ownerLeaseRenewIntervalMs(1000)
      .ownerLeaseRenewTimeoutMs(8000)
      .ownerLeaseTtlMs(15_000);
  });
  assert.throws(() => framework.createFrameworkRegistration(invalid), /ownerLease/u);
});

test('Session relocation seal timeout is a positive finite Location option', () => {
  for (const value of [0, -1, Number.POSITIVE_INFINITY, Number.NaN]) {
    const options = framework.createFrameworkOptions((builder) => {
      builder.configureLocations().sessionRelocationSealTimeoutMs(value);
    });
    assert.throws(
      () => framework.createFrameworkRegistration(options),
      /sessionRelocationSealTimeoutMs must be a finite positive number/u
    );
  }
});

test('framework runtime host uses the explicit location store for Actor lifecycle and authority routing', () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: store
      }
    })
  });

  const actorOptions = runtime.createActorManagerOptions();
  const spotOptions = runtime.createSpotManagerOptions();

  assert.equal(typeof actorOptions.locationLifecycle, 'object');
  assert.equal(spotOptions.locationLifecycle, undefined);
  assert.equal(typeof spotOptions.spotRouteResolver?.resolve, 'function');
});

test('host shutdown cancels startup and releases a provider claim committed after cancellation', async () => {
  const calls = [];
  const inner = new framework.ZLinkInMemoryProviderLocationStore();
  let resolveStarted;
  const claimStarted = new Promise(resolve => { resolveStarted = resolve; });
  let claimSignal;
  let claimKey;
  let claimStartedOnce = false;
  const provider = {
    read: inner.read.bind(inner),
    scan: inner.scan.bind(inner),
    async write(request, signal) {
      if (claimStartedOnce) return await inner.write(request, signal);
      claimStartedOnce = true;
      claimKey = request.mutations[0].key;
      claimSignal = signal;
      resolveStarted();
      return await new Promise((_resolve, reject) => {
        signal.addEventListener('abort', async () => {
          await inner.write(request);
          reject(signal.reason);
        }, { once: true });
      });
    }
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: { storeInstance: provider }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, rid('startup-shutdown'))
  });
  const starting = host.start();
  const observedStart = starting.then(() => 'started', error => error);
  await claimStarted;
  const termination = await host.shutdown({ deadlineMs: 1000 });
  assert.equal(claimSignal.aborted, true, 'host stop cancels the active claim');
  const startResult = await observedStart;
  assert.equal(startResult?.name, 'AbortError');
  assert.equal(termination.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal(host.isStarted, false);
  assert.equal((await inner.read(claimKey)).kind, 'missing');
});

for (const failedPhase of ['confirmation', 'release']) {
  test(`host shutdown reports startup ${failedPhase} cleanup failure without retry`, async () => {
    const inner = new framework.ZLinkInMemoryProviderLocationStore();
    let claimKey;
    let claimSignal;
    let committed = false;
    let claimStartedOnce = false;
    let cleanupReads = 0;
    let cleanupReleases = 0;
    const providerEvents = [];
    let resolveStarted;
    const started = new Promise(resolve => { resolveStarted = resolve; });
    const provider = {
      async read(key, signal) {
        providerEvents.push({ operation: 'read', key: key.value, committed, cancelled: signal?.aborted });
        if (committed && key.value === claimKey.value) {
          cleanupReads += 1;
          if (failedPhase === 'confirmation') throw new Error('confirmation unavailable');
        }
        return await inner.read(key, signal);
      },
      scan: inner.scan.bind(inner),
      async write(request, signal) {
        providerEvents.push({ operation: 'write', mutations: request.mutations.map(mutation => ({ kind: mutation.kind, key: mutation.key.value })), committed });
        if (!claimStartedOnce) {
          claimStartedOnce = true;
          claimKey = request.mutations[0].key;
          claimSignal = signal;
          resolveStarted();
          return await new Promise((_resolve, reject) => {
            signal.addEventListener('abort', async () => {
              await inner.write(request);
              committed = true;
              reject(signal.reason);
            }, { once: true });
          });
        }
        if (request.mutations.some(mutation => mutation.kind === 'delete' && mutation.key.value === claimKey.value)) {
          cleanupReleases += 1;
          throw new Error('release unavailable');
        }
        return await inner.write(request, signal);
      }
    };
    const host = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration({ locations: { storeInstance: provider } })
    }, { backendAdapterFactory: fakeBackendAdapterFactory([], rid(`failed-${failedPhase}`)) });
    const starting = host.start();
    const observedStart = starting.then(() => 'started', error => error);
    await started;
    const result = await host.shutdown({ deadlineMs: 1000 });
    assert.equal(claimSignal.aborted, true);
    assert.equal((await observedStart)?.name, 'AbortError');
    assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped, JSON.stringify(providerEvents));
    assert.equal(result.reason, framework.ZLinkFrameworkTerminationReason.TeardownFailed);
    assert.equal(cleanupReads, failedPhase === 'release' ? 2 : 1, JSON.stringify(providerEvents));
    assert.equal(cleanupReleases, failedPhase === 'release' ? 1 : 0);
    assert.equal((await inner.read(claimKey)).kind, 'found');
  });
}

test('host shutdown deadline bounds a pending opaque write reconciliation', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const inner = new framework.ZLinkInMemoryProviderLocationStore();
  let claimKey;
  let committed = false;
  let resolveStarted;
  let resolveReconciling;
  let completeRead;
  const started = new Promise(resolve => { resolveStarted = resolve; });
  const reconciling = new Promise(resolve => { resolveReconciling = resolve; });
  const provider = {
    async read(key, signal) {
      if (committed && key.value === claimKey.value) {
        resolveReconciling();
        return await new Promise(resolve => { completeRead = () => resolve(inner.read(key, signal)); });
      }
      return await inner.read(key, signal);
    },
    scan: inner.scan.bind(inner),
    async write(request, signal) {
      claimKey = request.mutations[0].key;
      resolveStarted();
      return await new Promise((_resolve, reject) => {
        signal.addEventListener('abort', async () => {
          await inner.write(request);
          committed = true;
          reject(signal.reason);
        }, { once: true });
      });
    }
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({ locations: { storeInstance: provider } })
  }, { backendAdapterFactory: fakeBackendAdapterFactory([], rid('opaque-reconcile-deadline')) });
  const observedStart = host.start().then(() => 'started', error => error);
  await started;
  let terminated = false;
  const shuttingDown = host.shutdown({ deadlineMs: 1000 }).then(result => {
    terminated = true;
    return result;
  });
  await reconciling;
  t.mock.timers.tick(999);
  await Promise.resolve();
  assert.equal(terminated, false);
  t.mock.timers.tick(1);
  const result = await shuttingDown;
  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
  assert.equal((await observedStart)?.name, 'AbortError');
  assert.equal(host.isStarted, false);
  completeRead();
});

test('direct host stop uses one existing default deadline for a pending provider', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const inner = new framework.ZLinkInMemoryProviderLocationStore();
  let blockScan = false;
  let cleanupSignal;
  let finishScan;
  let resolveScanning;
  const scanning = new Promise(resolve => { resolveScanning = resolve; });
  const provider = {
    read: inner.read.bind(inner),
    write: inner.write.bind(inner),
    async scan(request, signal) {
      if (!blockScan) return await inner.scan(request, signal);
      cleanupSignal = signal;
      resolveScanning();
      return await new Promise((_resolve, reject) => { finishScan = () => reject(signal.reason); });
    }
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({ locations: { storeInstance: provider } })
  }, { backendAdapterFactory: fakeBackendAdapterFactory([], rid('direct-stop-deadline')) });
  await host.start();
  blockScan = true;
  const deadlines = [];
  const setTimer = global.setTimeout;
  t.mock.method(global, 'setTimeout', (callback, delay, ...args) => {
    deadlines.push(delay);
    return setTimer(callback, delay, ...args);
  });
  const stopping = host.stop();
  const observedStop = stopping.then(() => undefined, error => error);
  await scanning;
  assert.deepEqual(deadlines, [30_000]);
  t.mock.timers.tick(29_999);
  assert.equal(cleanupSignal.aborted, false);
  t.mock.timers.tick(1);
  let settled = false;
  observedStop.then(() => { settled = true; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(settled, false);
  finishScan();
  const error = await observedStop;
  assert(error instanceof framework.ZLinkOwnerCleanupError);
  assert.equal(error.cause?.name, 'DeadlineExceededError');
  assert.equal(cleanupSignal.aborted, true);
  assert.equal(host.isStarted, false);
  assert.equal(host.status.deadline, undefined);
});

test('actor handoff source fence uses the Actor mesh node and rejects a cross-mesh RID', () => {
  const { host, meshRuntime: hostMesh } = createMeshHost({
    registration: framework.createFrameworkRegistration()
  });
  const primaryStatus = {
    routingId: rid('primary-node'),
    lifecycleGeneration: 11n
  };
  const actorStatus = {
    routingId: rid('mesh-b-node'),
    lifecycleGeneration: 22n
  };
  const states = new Map([
    ['actor-b', {
      meshName: 'mesh-b',
      nativeActorRef: {
        actorId: 'actor-b',
        nodeRid: rid('mesh-b-node'),
        generation: 7n
      },
      locationGeneration: 13n,
      ownerLeaseGeneration: 5n
    }],
    ['actor-mismatch', {
      meshName: 'mesh-b',
      nativeActorRef: {
        actorId: 'actor-mismatch',
        nodeRid: rid('primary-node'),
        generation: 8n
      },
      locationGeneration: 14n,
      ownerLeaseGeneration: 5n
    }]
  ]);
  const handoff = host.createSpotManagerOptions().actorHandoffRuntime;
  host.locationOwner = {
    currentRuntime: {
      currentOwnerToken: { ownerId: 'owner-b', leaseGeneration: 5n }
    }
  };
  Object.assign(hostMesh, {
    primaryMeshNode: { status: () => primaryStatus },
    meshNode(meshName) {
      return meshName === 'mesh-b' ? { status: () => actorStatus } : undefined;
    }
  });
  host.setActorManager({ getState: actorId => states.get(actorId) });

  handoff.begin('actor-b', 7n);
  assert.deepEqual(handoff.activeSourceOwner('actor-b'), {
    ownerId: 'owner-b',
    ownerLeaseGeneration: '5',
    nodeRid: 'mesh-b-node',
    nodeRidHex: Buffer.from('mesh-b-node').toString('hex'),
    nodeGeneration: '22',
    authorityOwnerGeneration: '13'
  });
  handoff.cancel('actor-b');

  assert.throws(
    () => handoff.begin('actor-mismatch', 8n),
    /committed source owner fence/u
  );
  assert.equal(handoff.isActive('actor-mismatch'), false);
});

test('framework host republishes the current lifecycle state after owner lease recovery', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const firstToken = { ownerId: 'owner-a', leaseGeneration: 1n };
  const secondToken = { ownerId: 'owner-a', leaseGeneration: 2n };
  let currentToken = firstToken;
  let recoveryHandler;
  const runtime = {
    get currentOwnerToken() {
      return currentToken;
    },
    addOwnerLeaseRenewedHandler(handler) {
      recoveryHandler = handler;
    },
    removeOwnerLeaseRenewedHandler() {}
  };
  const publishedStates = [];
  const spotNodeRuntime = {
    async publishMeshNodeState(state) {
      publishedStates.push(state);
    },
    async startLocationAutoConnect() {}
  };
  const channelRuntime = {
    async startLocationAutoConnect() {},
    async reclaimLocationOwnerRows() {}
  };

  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Draining;
  host.installOwnerLeaseRecoveryPublication(runtime, spotNodeRuntime, channelRuntime);
  currentToken = secondToken;
  recoveryHandler();
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(publishedStates, [
    framework.ZLinkFrameworkRuntimeState.Preparing,
    framework.ZLinkFrameworkRuntimeState.Draining
  ]);
  host.removeOwnerLeaseRecoveryPublication();
});

test('framework runtime host starts location runtime and injects lifecycle into managers', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000
        }
      },
      spotNodes: {
        play: {
          router: { bind: 'tcp://127.0.0.1:9101', routingId: 'node-a' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  const preStartActorOptions = runtime.createActorManagerOptions();
  const preStartSpotOptions = runtime.createSpotManagerOptions();
  const ownerId = runtime[inspectRuntime]().ownerId;
  assert.equal(typeof preStartActorOptions.locationLifecycle, 'object');
  assert.equal(preStartSpotOptions.locationLifecycle, undefined);
  assert.equal((await store.readOwnerLease(ownerId)).kind, 'missing');

  await runtime.start();

  const lease = await store.readOwnerLease(ownerId);
  assert.equal(lease.kind, 'found');

  const actorOptions = runtime.createActorManagerOptions();
  const spotOptions = runtime.createSpotManagerOptions();
  assert.equal(actorOptions.locationLifecycle, preStartActorOptions.locationLifecycle);
  assert.equal(spotOptions.locationLifecycle, undefined);
  assert.equal(String(spotOptions.nodeRidProvider('play')), 'node-a');
  assert.equal(typeof spotOptions.nodeGenerationProvider('play'), 'bigint');
  assert.equal(typeof spotOptions.spotRouteResolver?.resolve, 'function');

  await actorOptions.locationLifecycle.claimActor('player', 'actor-1', nodeRid);
  assert.notEqual(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
  await actorOptions.locationLifecycle.releaseActor('player', 'actor-1');

  await runtime.stop();

  assert.equal((await store.readOwnerLease(ownerId)).kind, 'missing');
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
  assert.deepEqual(calls, [
    'spot:dispose',
    'context:dispose'
  ]);
});

test('framework host publishes every local descriptor after degraded startup recovers its owner lease', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(inner, now);
  let unavailable = true;
  const provider = {
    read(key, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.read(key, signal);
    },
    write(request, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.write(request, signal);
    },
    scan(request, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.scan(request, signal);
    }
  };
  const calls = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 20,
          ownerLeaseRenewTimeoutMs: 5,
          ownerLeaseFencingMarginMs: 1,
          pollingIntervalMs: 20
        }
      },
      channels: {
        api: {
          routingId: 'api-node',
          server: { bind: 'tcp://local-api' },
          sendHandlers: [{ packetName: 'Notice', handler: { handle() {} } }]
        },
        events: {
          routingId: 'events-node',
          publisher: { bind: 'tcp://local-events' }
        }
      },
      spotNodes: {
        play: {
          router: { bind: 'tcp://local-play', routingId: 'play-node' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, rid('play-node'))
  });

  try {
    await runtime.start();
    assert.equal((await store.listMeshNodes('play')).items.length, 0);
    assert.equal((await store.listClientServers('api')).items.length, 0);
    assert.equal((await store.listFanoutPublishers('events')).items.length, 0);

    unavailable = false;
    await waitForCondition(async () => {
      const [meshNodes, servers, publishers] = await Promise.all([
        store.listMeshNodes('play'),
        store.listClientServers('api'),
        store.listFanoutPublishers('events')
      ]);
      return meshNodes.items.length === 1
        && servers.items.length === 1
        && publishers.items.length === 1;
    });
  } finally {
    unavailable = false;
    await runtime.stop();
  }
});

test('framework host without an owner lease stays Preparing until it publishes descriptors', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(inner, now);
  let unavailable = true;
  const guard = (operation) => {
    if (unavailable) throw new Error('location store unavailable');
    return operation();
  };
  const provider = {
    read: (key, signal) => guard(() => inner.read(key, signal)),
    write: (request, signal) => guard(() => inner.write(request, signal)),
    scan: (request, signal) => guard(() => inner.scan(request, signal))
  };
  const calls = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 20,
          ownerLeaseRenewTimeoutMs: 5,
          ownerLeaseFencingMarginMs: 1,
          pollingIntervalMs: 20
        }
      },
      spotNodes: {
        play: {
          router: { bind: 'tcp://local-play', routingId: 'play-node' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, rid('play-node'))
  });

  const observation = new AbortController();
  try {
    await runtime.start();
    // Location runtime §5: no new application work before the lease.
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.equal(runtime.status.isReady, false);
    assert.equal((await store.listMeshNodes('play')).items.length, 0);

    const observed = runtime.observe(observation.signal);
    unavailable = false;
    for await (const event of observed) {
      if (event.status.state === framework.ZLinkFrameworkRuntimeState.Serving) break;
    }

    assert.equal(runtime.status.isReady, true);
    const [descriptor] = (await store.listMeshNodes('play')).items;
    assert.equal(descriptor.state, framework.ZLinkFrameworkRuntimeState.Serving);
  } finally {
    observation.abort();
    unavailable = false;
    await runtime.stop();
  }
});

test('an automatic RID host serves immediately when its descriptor is Missing', async t => {
  t.mock.method(performance, 'now', () => 0);
  const calls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        useInMemoryStores: true,
        options: { ownerLeaseFencingMarginMs: 5 }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://127.0.0.1:0' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls)
  });
  try {
    await host.start();
    assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    assert.equal(
      host.locationOwner.currentStores.locationStore.ownerLeaseReleaseDelayForTesting
        .observationCountForTesting,
      0
    );
    assert.equal((await host.locationOwner.locationStore().listMeshNodes('play')).items.length, 1);
  } finally {
    await host.stop();
  }
});

test('a Fixed RID host stays Preparing while its previous owner release delay is pending', async t => {
  let storeNow = Date.UTC(2026, 6, 3, 0, 0, 0);
  let monotonicMs = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const inner = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  const seedStore = new framework.ZLinkLocationStoreRepository(
    inner,
    () => new Date(storeNow),
    undefined,
    () => monotonicMs,
    5
  );
  const previousOwner = await seedStore.claimOwnerLease('previous-fixed-owner', 100);
  assert.equal(previousOwner.kind, 'claimed');
  if (previousOwner.kind !== 'claimed') throw new Error('previous owner lease claim failed');
  const previousDescriptor = {
    meshName: 'play',
    rid: rid('fixed-host-node'),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://fixed-host-node',
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 1 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'fixed-host-node',
    ownerId: previousOwner.token.ownerId,
    leaseGeneration: previousOwner.token.leaseGeneration,
    updatedAt: new Date(storeNow)
  };
  await assert.rejects(
    seedStore.updateMeshNode(
      previousDescriptor,
      framework.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
    ),
    { kind: framework.ZLinkFrameworkErrorKind.Unavailable }
  );
  monotonicMs = 15;
  assert.equal(
    (await seedStore.updateMeshNode(
      previousDescriptor,
      framework.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
    )).status,
    framework.ZLinkLocationWriteStatus.Stored
  );
  storeNow += 100;

  let meshNodeWrites = 0;
  const provider = {
    read: inner.read.bind(inner),
    scan: inner.scan.bind(inner),
    write(request, signal) {
      if (request.mutations.some(mutation => mutation.key.value.startsWith('mesh-node\0'))) {
        meshNodeWrites += 1;
      }
      return inner.write(request, signal);
    }
  };
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 20_000,
          ownerLeaseTtlMs: 60_000,
          ownerLeaseFencingMarginMs: 5,
          ownerLeaseRenewTimeoutMs: 3_000,
          pollingIntervalMs: 60_000
        }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://local-play', routingId: 'fixed-host-node' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory([], rid('fixed-host-node'))
  });
  const observation = new AbortController();
  try {
    await runtime.start();
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.equal(runtime.status.isReady, false);
    assert.equal(runtime[inspectRuntime]().hasOwnerToken, true);
    assert.equal(meshNodeWrites, 0);
    const row = await inner.read({
      value: `mesh-node\0play\0${Buffer.from('fixed-host-node').toString('hex')}`
    });
    assert.equal(row.kind, 'found');
    if (row.kind !== 'found') throw new Error('previous Fixed RID descriptor missing');
    const stored = JSON.parse(Buffer.from(row.value.bytes).toString('utf8'));
    assert.equal(stored.descriptor.ownerId, previousOwner.token.ownerId);
    assert.equal(stored.descriptor.lifecycleGeneration, '1');

    const serving = (async () => {
      for await (const event of runtime.observe(observation.signal)) {
        if (event.status.state === framework.ZLinkFrameworkRuntimeState.Serving) return;
      }
      throw new Error('host stopped before Fixed RID claim completed');
    })();
    monotonicMs = 30;
    assert.equal(await runtime.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
    await serving;
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    assert.ok(meshNodeWrites > 0);
    const reclaimed = await inner.read({
      value: `mesh-node\0play\0${Buffer.from('fixed-host-node').toString('hex')}`
    });
    assert.equal(reclaimed.kind, 'found');
    if (reclaimed.kind !== 'found') throw new Error('reclaimed Fixed RID descriptor missing');
    const reclaimedDescriptor = JSON.parse(Buffer.from(reclaimed.value.bytes).toString('utf8'));
    assert.equal(reclaimedDescriptor.descriptor.ownerId, runtime[inspectRuntime]().ownerId);
    assert.notEqual(reclaimedDescriptor.descriptor.ownerId, previousOwner.token.ownerId);
  } finally {
    observation.abort();
    await runtime.stop();
  }
});

test('a Serving Fixed RID host republishes its descriptor after its own lease generation changes', async t => {
  let storeNow = Date.UTC(2026, 6, 3, 0, 0, 0);
  const clock = fixedRidHostClock(t);
  const inner = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: inner,
        options: {
          ownerLeaseRenewIntervalMs: 20_000,
          ownerLeaseTtlMs: 60_000,
          ownerLeaseFencingMarginMs: 5,
          ownerLeaseRenewTimeoutMs: 3_000,
          pollingIntervalMs: 60_000
        }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://local-play', routingId: 'fixed-recovery-node' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory([], rid('fixed-recovery-node'))
  });
  try {
    await runtime.start();
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    await finishFixedRidHostStartup(
      runtime,
      clock,
      framework.ZLinkFrameworkRuntimeState.Preparing,
      15,
      1
    );
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    const before = (await runtime.locationOwner.locationStore().listMeshNodes('play')).items[0];
    assert.ok(before);
    const oldOwner = runtime.locationOwner.currentRuntime.currentOwnerToken;
    assert.ok(oldOwner);
    assert.equal(before.ownerId, oldOwner.ownerId);
    assert.equal(before.leaseGeneration, oldOwner.leaseGeneration);
    assert.equal(
      await runtime.locationOwner.locationStore().removeMeshNode(
        { meshName: 'play', rid: rid('fixed-recovery-node') },
        oldOwner
      ),
      framework.ZLinkLocationWriteStatus.Stored
    );
    assert.equal((await runtime.locationOwner.locationStore().listMeshNodes('play')).items.length, 0);

    storeNow += 60_001;
    clock.advance(60_001);
    const recovery = waitForNextHostOwnerRecovery(runtime);
    assert.equal(await runtime.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
    await recovery;

    const newOwner = runtime.locationOwner.currentRuntime.currentOwnerToken;
    const after = (await runtime.locationOwner.locationStore().listMeshNodes('play')).items[0];
    assert.ok(newOwner);
    assert.ok(after);
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    assert.equal(newOwner.ownerId, oldOwner.ownerId);
    assert.notEqual(newOwner.leaseGeneration, oldOwner.leaseGeneration);
    assert.equal(after.ownerId, newOwner.ownerId);
    assert.equal(after.leaseGeneration, newOwner.leaseGeneration);
  } finally {
    await runtime.stop();
  }
});

for (const storage of ['provider', 'in-memory']) {
  test(`a Fixed RID publication conflict moves the losing host to Error (${storage})`, async t => {
    let storeNow = Date.UTC(2026, 6, 3, 0, 0, 0);
    const clock = fixedRidHostClock(t);
    const provider =
      storage === 'provider'
        ? new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow))
        : undefined;
    const sharedStore =
      storage === 'provider'
        ? new framework.ZLinkLocationStoreRepository(
            provider,
            () => new Date(storeNow),
            undefined,
            () => performance.now(),
            5
          )
        : new framework.ZLinkInMemoryLocationStore(
            () => new Date(storeNow),
            () => performance.now(),
            5
          );
    await seedFixedRidDescriptor(
      sharedStore,
      'play',
      'fixed-race-node',
      'expired-fixed-owner',
      storeNow
    );
    storeNow += 101;

    const locations = {
      ...(storage === 'provider' ? { storeInstance: provider } : { useInMemoryStores: true }),
      options: {
        ownerLeaseRenewIntervalMs: 20_000,
        ownerLeaseTtlMs: 60_000,
        ownerLeaseFencingMarginMs: 5,
        ownerLeaseRenewTimeoutMs: 3_000,
        pollingIntervalMs: 60_000
      }
    };
    const makeHost = () => {
      const host = new framework.ZLinkFrameworkRuntimeHost({
        registration: framework.createFrameworkRegistration({
          locations,
          spotNodes: {
            play: { router: { bind: 'tcp://local-play', routingId: 'fixed-race-node' } }
          }
        })
      }, { backendAdapterFactory: fakeBackendAdapterFactory([], rid('fixed-race-node')) });
      if (storage === 'in-memory') {
        host.locationOwner.stores = {
          locationStore: sharedStore,
          authorityStore: sharedStore,
          clientServerStore: sharedStore,
          fanoutStore: sharedStore,
          peerStore: sharedStore,
          spotStore: sharedStore,
          actorStore: sharedStore,
          routeStore: sharedStore,
          ownerLeaseStore: sharedStore
        };
      }
      return host;
    };
    const first = makeHost();
    const second = makeHost();
    let notifyCleanup;
    const cleanupCompleted = new Promise(resolve => { notifyCleanup = resolve; });
    const shutdown = second.shutdown.bind(second);
    second.shutdown = async (...args) => {
      try {
        return await shutdown(...args);
      } finally {
        notifyCleanup();
      }
    };
    try {
      await first.start();
      await second.start();
      assert.equal(first.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);

      const firstRecovery = waitForNextHostOwnerRecovery(first);
      clock.advance(15);
      assert.equal(await first.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
      await firstRecovery;
      assert.equal(first.status.state, framework.ZLinkFrameworkRuntimeState.Serving);

      const errorObserved = observeRuntimeState(second, framework.ZLinkFrameworkRuntimeState.Error);
      const secondRecovery = waitForNextHostOwnerRecovery(second);
      assert.equal(await second.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
      await secondRecovery;
      assert.equal(await errorObserved.reached, true);
      errorObserved.controller.abort();
      await cleanupCompleted;
      assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
      assert.equal(second.executionState, undefined);
      assert.equal(first.status.state, framework.ZLinkFrameworkRuntimeState.Serving);

      const [descriptor] = (await first.locationOwner.locationStore().listMeshNodes('play')).items;
      assert.ok(descriptor);
      assert.equal(
        descriptor.ownerId,
        first.locationOwner.currentRuntime.currentOwnerToken.ownerId
      );
    } finally {
      await second.stop();
      await first.stop();
    }
  });
}

for (const storage of ['provider', 'in-memory']) {
  test(`P1 partial mesh publication preserves the Fixed RID boundary (${storage})`, async (t) => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const { host } = scenario.makeHost([], {
      automatic: { router: { bind: 'tcp://automatic' } },
      play: { router: { bind: 'tcp://fixed', routingId: 'fixed-p1-node' } }
    });
    try {
      await host.start();
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      await publishRegressionWeight(host, 'automatic', 42);
      assert.equal((await scenario.store.listMeshNodes('automatic')).items[0].placementWeight, 42);

      scenario.advance(14);
      await renewFixedRidRegressionHost(host);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      assert.equal(
        (await scenario.store.listMeshNodes('play')).items[0].ownerId,
        'expired-p1-owner'
      );
      scenario.advance(1);
      await renewFixedRidRegressionHost(host);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      assert.equal(
        (await scenario.store.listMeshNodes('play')).items[0].ownerId,
        host.locationOwner.currentRuntime.currentOwnerToken.ownerId
      );
    } finally {
      await host.stop();
    }
  });
}

for (const storage of ['provider', 'in-memory']) {
  test(`P1 Serving recovery reclaims its previous owner row (${storage})`, async t => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const calls = [];
    const { host } = scenario.makeHost(calls);
    const states = [];
    const observation = new AbortController();
    const shutdown = host.shutdown.bind(host);
    let shutdownCalls = 0;
    let observed;
    host.shutdown = (...args) => {
      shutdownCalls++;
      return shutdown(...args);
    };
    try {
      await host.start();
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      const runtime = host.locationOwner.currentRuntime;
      const previousOwner = runtime.currentOwnerToken;
      const [previousDescriptor] = (await scenario.store.listMeshNodes('play')).items;
      assert.ok(previousOwner);
      assert.ok(previousDescriptor);
      assert.equal(previousDescriptor.ownerId, previousOwner.ownerId);
      assert.equal(previousDescriptor.leaseGeneration, previousOwner.leaseGeneration);

      observed = (async () => {
        for await (const { status } of host.observe(observation.signal)) {
          if (states.at(-1) !== status.state) states.push(status.state);
        }
      })();
      scenario.advance(60_001);
      scenario.advanceStore(60_001);
      await renewFixedRidRegressionHost(host);
      observation.abort();
      await observed;

      const currentOwner = runtime.currentOwnerToken;
      const [descriptor] = (await scenario.store.listMeshNodes('play')).items;
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      assert.ok(currentOwner);
      assert.equal(currentOwner.ownerId, previousOwner.ownerId);
      assert.notEqual(currentOwner.leaseGeneration, previousOwner.leaseGeneration);
      assert.ok(descriptor);
      assert.equal(descriptor.ownerId, currentOwner.ownerId);
      assert.equal(descriptor.leaseGeneration, currentOwner.leaseGeneration);
      assert.ok(states.includes(framework.ZLinkFrameworkRuntimeState.Serving));
      assert.equal(states.includes(framework.ZLinkFrameworkRuntimeState.Error), false);
      assert.equal(shutdownCalls, 0);
    } finally {
      observation.abort();
      await observed;
      await host.stop();
    }
  });
}

for (const storage of ['provider', 'in-memory']) {
  test(`P1 recovery restores Missing immediately and rejects an invalid foreign row (${storage})`, async t => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const { host } = scenario.makeHost();
    try {
      await host.start();
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      const owner = host.locationOwner.currentRuntime.currentOwnerToken;
      const key = { meshName: 'play', rid: rid('fixed-p1-node') };
      assert.equal(await scenario.store.removeMeshNode(key, owner), framework.ZLinkLocationWriteStatus.Stored);
      await publishRegressionWeight(host, 'play', 41);
      const [restored] = (await scenario.store.listMeshNodes('play')).items;
      assert.equal(restored.ownerId, owner.ownerId);
      assert.equal(restored.placementWeight, 41);

      assert.equal(await scenario.store.removeMeshNode(key, owner), framework.ZLinkLocationWriteStatus.Stored);
      const foreign = await scenario.store.claimOwnerLease('foreign-recovery-owner', 60_000);
      assert.equal(foreign.kind, 'claimed');
      assert.equal((await scenario.store.updateMeshNode({
        ...restored, ownerId: foreign.token.ownerId, leaseGeneration: foreign.token.leaseGeneration
      }, framework.ZLinkLocationWriteIntent.NewClaim)).status, framework.ZLinkLocationWriteStatus.Stored);
      assert.equal(await scenario.store.releaseOwnerLease(foreign.token), 'released');
      const before = (await scenario.store.listMeshNodes('play')).items;
      await assert.rejects(() => publishRegressionWeight(host, 'play', 42), /ignoredStale/u);
      assert.deepEqual((await scenario.store.listMeshNodes('play')).items, before);
    } finally {
      await host.stop();
    }
  });

  for (const retry of ['none', 'stop', 'shutdown']) {
    const fails = retry !== 'none';
    test(`P1 stop before shutdown classifies ${fails ? `failed teardown with ${retry} after shutdown retry` : 'successful teardown'} (${storage})`, async t => {
      const scenario = await fixedRidRegressionScenario(t, storage);
      const calls = [];
      const { host } = scenario.makeHost(calls);
      const entered = deferredRegressionStep();
      const release = deferredRegressionStep();
      let attempts = 0;
      let storeDisposes = 0;
      if (scenario.provider) scenario.provider.dispose = async () => { storeDisposes++; };
      let stopping;
      let shuttingDown;
      try {
        await host.start();
        scenario.advance(15);
        await renewFixedRidRegressionHost(host);
        const dispose = host.context.dispose.bind(host.context);
        host.context.dispose = async () => {
          attempts++;
          entered.resolve();
          await release.promise;
          if (fails && attempts === 1) throw new Error('context dispose failed once');
          await dispose();
        };
        stopping = host.stop();
        const stopResult = Promise.allSettled([stopping]);
        await entered.promise;
        shuttingDown = host.shutdown();
        const shutdownResult = Promise.allSettled([shuttingDown]);
        release.resolve();
        const [result] = await shutdownResult;
        assert.equal(result.status, 'fulfilled', result.reason?.stack);
        assert.deepEqual(result.value, {
          outcome: fails ? framework.ZLinkFrameworkTerminationOutcome.ForceStopped : framework.ZLinkFrameworkTerminationOutcome.Stopped,
          reason: fails ? framework.ZLinkFrameworkTerminationReason.TeardownFailed : framework.ZLinkFrameworkTerminationReason.None
        });
        assert.deepEqual(host.status.terminationResult, result.value);
        assert.equal((await stopResult)[0].status, fails ? 'rejected' : 'fulfilled');
        assert.equal(attempts, fails ? 2 : 1);
        if (fails) {
          if (retry === 'stop') await host.stop();
          if (retry === 'shutdown') assert.deepEqual(await host.shutdown(), result.value);
        }
        assert.equal(calls.filter(call => call === 'spot:dispose').length, 1);
        assert.equal(calls.filter(call => call === 'context:dispose').length, 1);
        assert.equal(
          storeDisposes,
          scenario.provider ? (fails ? 2 : 1) : 0
        ); // #3923: shutdown retries teardown and disposes the owned store again
        assert.equal(host.executionState, undefined);
        if (fails) {
          await host.start();
          assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
          scenario.advance(15);
          await renewFixedRidRegressionHost(host);
          assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
        }
      } finally {
        release.resolve();
        await Promise.allSettled([stopping, shuttingDown]);
        await host.stop();
      }
    });
  }

  test(`P1 stop-owned teardown retry uses the absolute shutdown deadline (${storage})`, async t => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const { host } = scenario.makeHost();
    const firstDisposeEntered = deferredRegressionStep();
    const releaseFirstDispose = deferredRegressionStep();
    const retryDisposeEntered = deferredRegressionStep();
    const releaseRetryDispose = deferredRegressionStep();
    let contextDisposes = 0;
    let stopping;
    let shuttingDown;
    try {
      await host.start();
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      const disposeContext = host.context.dispose.bind(host.context);
      host.context.dispose = async () => {
        contextDisposes++;
        if (contextDisposes === 1) {
          firstDisposeEntered.resolve();
          await releaseFirstDispose.promise;
          throw new Error('context dispose failed once');
        }
        if (contextDisposes === 2) {
          retryDisposeEntered.resolve();
          await releaseRetryDispose.promise;
        }
        await disposeContext();
      };

      stopping = host.stop();
      const stopResult = Promise.allSettled([stopping]);
      await firstDisposeEntered.promise;
      shuttingDown = host.shutdown({ deadlineMs: 50 });
      const shutdownResult = Promise.allSettled([shuttingDown]);
      releaseFirstDispose.resolve();
      await retryDisposeEntered.promise;
      scenario.advance(51);
      releaseRetryDispose.resolve();

      const [result] = await shutdownResult;
      assert.equal(result.status, 'fulfilled', result.reason?.stack);
      assert.deepEqual(result.value, {
        outcome: framework.ZLinkFrameworkTerminationOutcome.ForceStopped,
        reason: framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
      });
      assert.deepEqual(host.status.terminationResult, result.value);
      assert.equal((await stopResult)[0].status, 'rejected');
      assert.equal(contextDisposes, 2);
      assert.equal(host.executionState, undefined);
    } finally {
      releaseFirstDispose.resolve();
      releaseRetryDispose.resolve();
      await Promise.allSettled([stopping, shuttingDown]);
      await host.stop();
    }
  });

  test(`P1 shutdown retry failure can be completed by a later shutdown (${storage})`, async t => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const calls = [];
    const { host } = scenario.makeHost(calls);
    let contextDisposes = 0;
    let storeDisposes = 0;
    if (scenario.provider) scenario.provider.dispose = async () => { storeDisposes++; };
    try {
      await host.start();
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      const disposeContext = host.context.dispose.bind(host.context);
      host.context.dispose = async () => {
        contextDisposes++;
        if (contextDisposes <= 2) throw new Error('context dispose failed twice');
        await disposeContext();
      };

      const first = await host.shutdown();
      assert.deepEqual(first, {
        outcome: framework.ZLinkFrameworkTerminationOutcome.ForceStopped,
        reason: framework.ZLinkFrameworkTerminationReason.TeardownFailed
      });
      assert.deepEqual(host.status.terminationResult, first);
      assert.equal(contextDisposes, 2);
      assert.equal(storeDisposes, scenario.provider ? 2 : 0); // #3923: both teardown attempts dispose the owned store
      assert.notEqual(host.executionState, undefined);
      await assert.rejects(() => host.start(), /cleanup must complete/u);

      assert.deepEqual(await host.shutdown(), {
        outcome: framework.ZLinkFrameworkTerminationOutcome.Stopped,
        reason: framework.ZLinkFrameworkTerminationReason.None
      });
      assert.equal(contextDisposes, 3);
      assert.equal(storeDisposes, scenario.provider ? 3 : 0); // #3923: explicit retry disposes the owned store again
      assert.equal(host.executionState, undefined);
      await host.start();
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    } finally {
      await host.stop();
    }
  });

  test(`P1 Serving recovery rejection observes Error Draining Stopped (${storage})`, async t => {
    const scenario = await fixedRidRegressionScenario(t, storage);
    const calls = [];
    const { host } = scenario.makeHost(calls);
    const states = [];
    const cleanupStarted = deferredRegressionStep();
    const shutdown = host.shutdown.bind(host);
    host.shutdown = (...args) => {
      const operation = shutdown(...args);
      cleanupStarted.resolve(operation);
      return operation;
    };
    const observation = new AbortController();
    let observed;
    try {
      await host.start();
      scenario.advance(15);
      await renewFixedRidRegressionHost(host);
      const runtime = host.locationOwner.currentRuntime;
      const oldOwner = runtime.currentOwnerToken;
      const [descriptor] = (await scenario.store.listMeshNodes('play')).items;
      assert.equal(await scenario.store.removeMeshNode({ meshName: 'play', rid: descriptor.rid }, oldOwner), framework.ZLinkLocationWriteStatus.Stored);
      const foreign = await scenario.store.claimOwnerLease('foreign-serving-owner', 60_000);
      assert.equal(foreign.kind, 'claimed');
      assert.equal((await scenario.store.updateMeshNode({
        ...descriptor, ownerId: foreign.token.ownerId, leaseGeneration: foreign.token.leaseGeneration
      }, framework.ZLinkLocationWriteIntent.NewClaim)).status, framework.ZLinkLocationWriteStatus.Stored);
      assert.equal(await scenario.store.releaseOwnerLease(foreign.token), 'released');
      const before = (await scenario.store.listMeshNodes('play')).items;
      assert.equal(await scenario.store.releaseOwnerLease(oldOwner), 'released');
      const publications = [];
      const domainStore = host.locationOwner.locationStore();
      const updateMeshNode = domainStore.updateMeshNode.bind(domainStore);
      domainStore.updateMeshNode = async (next, ...args) => {
        publications.push(next);
        return await updateMeshNode(next, ...args);
      };
      observed = (async () => {
        for await (const { status } of host.observe(observation.signal)) {
          if (states.at(-1) !== status.state) states.push(status.state);
        }
      })();
      await renewFixedRidRegressionHost(host);
      const termination = await cleanupStarted.promise;
      observation.abort();
      await observed;
      assert.deepEqual(states, [
        framework.ZLinkFrameworkRuntimeState.Serving,
        framework.ZLinkFrameworkRuntimeState.Error,
        framework.ZLinkFrameworkRuntimeState.Draining,
        framework.ZLinkFrameworkRuntimeState.Stopped
      ]);
      assert.deepEqual(termination, {
        outcome: framework.ZLinkFrameworkTerminationOutcome.ForceStopped,
        reason: framework.ZLinkFrameworkTerminationReason.TeardownFailed
      });
      assert.deepEqual(host.status.terminationResult, termination);
      assert.ok(publications.length > 0);
      assert.equal(publications[0].ownerId, oldOwner.ownerId);
      assert.notEqual(publications[0].leaseGeneration, oldOwner.leaseGeneration);
      assert.equal(publications[0].lifecycleGeneration, descriptor.lifecycleGeneration);
      assert.deepEqual((await scenario.store.listMeshNodes('play')).items, before);
      assert.equal(calls.filter(call => call === 'context:dispose').length, 1);
      const writesAtStop = (await scenario.store.listMeshNodes('play')).items;
      const publicationsAtStop = publications.length;
      scenario.advance(60_001);
      assert.equal(await runtime.renewOwnerLeaseOnce(), false);
      assert.equal(publications.length, publicationsAtStop);
      assert.deepEqual((await scenario.store.listMeshNodes('play')).items, writesAtStop);
    } finally {
      observation.abort();
      await observed;
      await host.stop();
    }
  });
}

test('P1 Missing recovery CAS never overwrites a concurrently created invalid owner row', async t => {
  const scenario = await fixedRidRegressionScenario(t, 'provider');
  const { host } = scenario.makeHost();
  try {
    await host.start();
    scenario.advance(15);
    await renewFixedRidRegressionHost(host);
    const owner = host.locationOwner.currentRuntime.currentOwnerToken;
    const [descriptor] = (await scenario.store.listMeshNodes('play')).items;
    assert.equal(await scenario.store.removeMeshNode({ meshName: 'play', rid: descriptor.rid }, owner),
      framework.ZLinkLocationWriteStatus.Stored);
    const foreign = await scenario.store.claimOwnerLease('concurrent-recovery-owner', 60_000);
    assert.equal(foreign.kind, 'claimed');
    const write = scenario.provider.write.bind(scenario.provider);
    let inserted = false;
    scenario.provider.write = async (request, signal) => {
      if (!inserted && request.conditions.some(condition =>
        condition.kind === 'missing' && condition.key.value.startsWith('mesh-node\0'))) {
        inserted = true;
        assert.equal((await scenario.store.updateMeshNode({
          ...descriptor, ownerId: foreign.token.ownerId, leaseGeneration: foreign.token.leaseGeneration
        }, framework.ZLinkLocationWriteIntent.NewClaim)).status, framework.ZLinkLocationWriteStatus.Stored);
        assert.equal(await scenario.store.releaseOwnerLease(foreign.token), 'released');
      }
      return await write(request, signal);
    };
    await assert.rejects(() => publishRegressionWeight(host, 'play', 42), /ignoredStale/u);
    assert.equal(inserted, true);
    assert.equal((await scenario.store.listMeshNodes('play')).items[0].ownerId, foreign.token.ownerId);
  } finally {
    await host.stop();
  }
});

test('P1 unconfirmed first Fixed RID publication renews its own committed row', async t => {
  const scenario = await fixedRidRegressionScenario(t, 'provider');
  const { host } = scenario.makeHost();
  try {
    await host.start();
    const write = scenario.provider.write.bind(scenario.provider);
    const read = scenario.provider.read.bind(scenario.provider);
    let committed = false;
    let uncertain = true;
    scenario.provider.write = async (request, signal) => {
      const result = await write(request, signal);
      if (uncertain && request.mutations.some(mutation =>
        mutation.kind === 'put' && mutation.key.value.startsWith('mesh-node\0'))) {
        committed = true;
        throw new Error('first descriptor response lost after commit');
      }
      return result;
    };
    scenario.provider.read = async (key, signal) => {
      if (uncertain && committed && key.value.startsWith('mesh-node\0')) {
        throw new Error('descriptor confirmation unavailable');
      }
      return await read(key, signal);
    };
    scenario.advance(15);
    await renewFixedRidRegressionHost(host);
    assert.equal(committed, true);
    assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    uncertain = false;
    await renewFixedRidRegressionHost(host);
    assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    const [descriptor] = (await scenario.store.listMeshNodes('play')).items;
    assert.equal(descriptor.ownerId, host.locationOwner.currentRuntime.currentOwnerToken.ownerId);
  } finally {
    await host.stop();
  }
});

test('P1 conflict cleanup joins shutdown and concurrent public stop', async (t) => {
  const scenario = await fixedRidRegressionScenario(t, 'provider');
  const { host: winner } = scenario.makeHost();
  const calls = [];
  const { host: loser, coordinator } = scenario.makeHost(calls);
  const drainEntered = deferredRegressionStep();
  const allowDrain = deferredRegressionStep();
  const disposeEntered = deferredRegressionStep();
  const allowDispose = deferredRegressionStep();
  let shutdown;
  let stopping;
  let ownedDisposes = 0;
  scenario.provider.dispose = async () => {
    ownedDisposes += 1;
  };
  try {
    await winner.start();
    await loser.start();
    scenario.advance(15);
    await renewFixedRidRegressionHost(winner);
    const shutdownHost = coordinator.shutdownHost.bind(coordinator);
    coordinator.shutdownHost = async (...args) => {
      drainEntered.resolve();
      await allowDrain.promise;
      return await shutdownHost(...args);
    };
    const disposeContext = loser.context.dispose.bind(loser.context);
    loser.context.dispose = async () => {
      await disposeContext();
      disposeEntered.resolve();
      await allowDispose.promise;
    };
    loser.errorSink.onRuntimeTaskException(({ taskName }) => {
      if (taskName === 'owner lease recovery') shutdown = loser.shutdown();
    });
    await renewFixedRidRegressionHost(loser);
    await drainEntered.promise;
    assert.equal(
      calls.filter((call) => call === 'spot:dispose').length,
      0,
      'transport cleanup must wait for the shutdown owner to finish draining'
    );
    stopping = loser.stop();
    let stopCompleted = false;
    stopping.then(() => {
      stopCompleted = true;
    });
    allowDrain.resolve();
    await disposeEntered.promise;
    assert.equal(stopCompleted, false, 'public stop shares the pending teardown');
    assert.equal(calls.filter((call) => call === 'context:dispose').length, 1);
    allowDispose.resolve();
    await Promise.all([shutdown, stopping]);
    assert.equal(calls.filter((call) => call === 'spot:dispose').length, 1);
    assert.equal(calls.filter((call) => call === 'context:dispose').length, 1);
    assert.equal(ownedDisposes, 1);
    assert.equal(loser.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
  } finally {
    allowDrain.resolve();
    allowDispose.resolve();
    await shutdown;
    await stopping;
    await loser.stop();
    await winner.stop();
  }
});

test('P1 predecessor CAS loser enters Error and cannot republish after winner stop', async (t) => {
  const scenario = await fixedRidRegressionScenario(t, 'provider');
  const hosts = [scenario.makeHost().host, scenario.makeHost().host];
  const bothSubmitted = deferredRegressionStep();
  const allowWrites = deferredRegressionStep();
  const write = scenario.provider.write.bind(scenario.provider);
  const submissions = [];
  const outcomes = new Map();
  scenario.provider.write = async (request, signal) => {
    if (
      request.mutations.some(
        (mutation) => mutation.kind === 'put' && mutation.key.value.startsWith('mesh-node\0')
      )
    ) {
      submissions.push(request);
      if (submissions.length === 2) bothSubmitted.resolve();
      await allowWrites.promise;
    }
    return await write(request, signal);
  };
  try {
    await Promise.all(hosts.map((host) => host.start()));
    for (const host of hosts) {
      const store = host.locationOwner.locationStore();
      const update = store.updateMeshNode.bind(store);
      store.updateMeshNode = async (...args) => {
        const result = await update(...args);
        if (args[1] === ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim && !outcomes.has(host)) outcomes.set(host, result.status);
        return result;
      };
    }
    const runtimes = new Map(hosts.map(host => [host, host.locationOwner.currentRuntime]));
    const cleanups = new Map(hosts.map(host => [host, deferredRegressionStep()]));
    for (const host of hosts) {
      const shutdown = host.shutdown.bind(host);
      host.shutdown = async () => {
        try { return await shutdown(); }
        finally { cleanups.get(host).resolve(); }
      };
    }
    const errorObservations = hosts.map(host => observeRuntimeState(host, framework.ZLinkFrameworkRuntimeState.Error));
    scenario.advance(15);
    const recoveries = hosts.map((host) => renewFixedRidRegressionHost(host));
    await bothSubmitted.promise;
    const predecessor = submissions.map((request) =>
      request.conditions.find(
        (condition) => condition.kind === 'version' && condition.key.value.startsWith('mesh-node\0')
      )
    );
    assert.ok(predecessor.every(Boolean));
    assert.equal(predecessor[0].expected.value, predecessor[1].expected.value);
    allowWrites.resolve();
    await Promise.all(recoveries);
    const loser = hosts.find(
      (host) => outcomes.get(host) === framework.ZLinkLocationWriteStatus.IgnoredStale
    );
    assert.ok(loser, 'one host must lose the CAS against the shared predecessor');
    const winner = hosts.find((host) => host !== loser);
    assert.equal(await errorObservations[hosts.indexOf(loser)].reached, true);
    for (const observation of errorObservations) observation.controller.abort();
    const loserRuntime = runtimes.get(loser);
    const winnerOwner = winner.locationOwner.currentRuntime.currentOwnerToken;
    await cleanups.get(loser).promise;
    assert.equal(loser.executionState, undefined, 'CAS failure completes automatic cleanup');
    await winner.stop();
    assert.equal(
      await scenario.store.removeMeshNode({ meshName: 'play', rid: rid('fixed-p1-node') }, winnerOwner),
      framework.ZLinkLocationWriteStatus.Stored
    );
    const submissionsAtStop = submissions.length;
    scenario.advance(60_001);
    assert.equal(await loserRuntime.renewOwnerLeaseOnce(), false);
    assert.equal(loser.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
    assert.equal(submissions.length, submissionsAtStop);
    assert.equal((await scenario.store.listMeshNodes('play')).items.length, 0);
  } finally {
    allowWrites.resolve();
    await Promise.all(hosts.map((host) => host.stop()));
  }
});

function deferredRegressionStep() {
  let resolve, reject;
  const promise = new Promise((done, failed) => {
    resolve = done;
    reject = failed;
  });
  return { promise, resolve, reject };
}

async function publishRegressionWeight(host, meshName, weight) {
  const runner = host.taskRunner;
  const runDetached = runner.runDetached.bind(runner);
  const published = deferredRegressionStep();
  runner.runDetached = (taskName, callback) => {
    runDetached(taskName, async signal => {
      try {
        await callback(signal);
        if (taskName === `RouteMesh '${meshName}' runtime weight publication`) published.resolve();
      } catch (error) {
        if (taskName === `RouteMesh '${meshName}' runtime weight publication`) published.reject(error);
        throw error;
      }
    });
  };
  try {
    host.routeMeshRuntimeOptions.mesh(meshName).placementWeight = weight;
    await published.promise;
  } finally {
    runner.runDetached = runDetached;
  }
}

test('P1 shutdown disposes an owned store when context disposal fails', async t => {
  const scenario = await fixedRidRegressionScenario(t, 'provider');
  const { host } = scenario.makeHost();
  let contextDisposes = 0;
  let storeDisposes = 0;
  scenario.provider.dispose = async () => {
    storeDisposes++;
  };
  try {
    await host.start();
    scenario.advance(15);
    await renewFixedRidRegressionHost(host);
    const disposeContext = host.context.dispose.bind(host.context);
    host.context.dispose = async () => {
      contextDisposes++;
      if (contextDisposes === 1) throw new Error('context dispose failed once');
      await disposeContext();
    };

    assert.deepEqual(await host.shutdown(), {
      outcome: framework.ZLinkFrameworkTerminationOutcome.ForceStopped,
      reason: framework.ZLinkFrameworkTerminationReason.TeardownFailed
    });
    assert.equal(
      contextDisposes,
      2,
      `context dispose attempts=${contextDisposes}; executionState=${host.executionState === undefined ? 'released' : 'retained'}`
    );
    assert.equal(storeDisposes, 2); // #3923: same shutdown retries owned-store disposal
    assert.equal(host.executionState, undefined);
  } finally {
    await host.stop();
  }
});

async function renewFixedRidRegressionHost(host) {
  const recovery = waitForNextHostOwnerRecovery(host);
  assert.equal(await host.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
  await recovery;
}

async function fixedRidRegressionScenario(t, storage) {
  let monotonicMs = 0;
  let storeNow = Date.UTC(2026, 6, 3);
  t.mock.method(performance, 'now', () => monotonicMs);
  const provider =
    storage === 'provider'
      ? new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow))
      : undefined;
  const store =
    provider === undefined
      ? new framework.ZLinkInMemoryLocationStore(
          () => new Date(storeNow),
          () => monotonicMs,
          5
        )
      : new framework.ZLinkLocationStoreRepository(
          provider,
          () => new Date(storeNow),
          undefined,
          () => monotonicMs,
          5
        );
  await seedFixedRidDescriptor(store, 'play', 'fixed-p1-node', 'expired-p1-owner', storeNow);
  storeNow += 101;
  return {
    provider,
    store,
    advance(milliseconds) {
      monotonicMs += milliseconds;
    },
    advanceStore(milliseconds) {
      storeNow += milliseconds;
    },
    makeHost(
      calls = [],
      spotNodes = {
        play: { router: { bind: 'tcp://fixed', routingId: 'fixed-p1-node' } }
      }
    ) {
      const backendAdapterFactory = fakeBackendAdapterFactory(calls, rid('automatic-p1-node'));
      const createMeshAdapter = backendAdapterFactory.createMeshAdapter;
      backendAdapterFactory.createMeshAdapter = () => {
        const adapter = createMeshAdapter();
        const createMeshNode = adapter.createMeshNode;
        adapter.createMeshNode = (...args) => {
          const node = createMeshNode(...args);
          const status = node.status;
          node.status = () => {
            const snapshot = status();
            return { ...snapshot, routingId: rid(snapshot.routingId) };
          };
          return node;
        };
        return adapter;
      };
      const { host, coordinator } = createRelayHost(
        {
          registration: framework.createFrameworkRegistration({
            locations: {
              ...(provider === undefined
                ? { useInMemoryStores: true }
                : { storeInstance: provider }),
              options: {
                ownerLeaseRenewIntervalMs: 20_000,
                ownerLeaseTtlMs: 60_000,
                ownerLeaseFencingMarginMs: 5,
                ownerLeaseRenewTimeoutMs: 3_000,
                pollingIntervalMs: 60_000
              }
            },
            spotNodes
          })
        },
        { backendAdapterFactory }
      );
      if (provider === undefined) {
        host.locationOwner.stores = Object.fromEntries(
          [
            'locationStore',
            'authorityStore',
            'clientServerStore',
            'fanoutStore',
            'peerStore',
            'spotStore',
            'actorStore',
            'routeStore',
            'ownerLeaseStore'
          ].map((name) => [name, store])
        );
      }
      return { host, coordinator };
    }
  };
}

test('Fixed RID startup observes every mesh before the shared release-delay boundary', async t => {
  let storeNow = Date.UTC(2026, 6, 3, 0, 0, 0);
  const clock = fixedRidHostClock(t);
  const inner = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  const seedStore = new framework.ZLinkLocationStoreRepository(
    inner,
    () => new Date(storeNow),
    undefined,
    () => performance.now(),
    5
  );
  await seedFixedRidDescriptor(seedStore, 'play', 'fixed-mesh-a', 'old-owner-a', storeNow);
  await seedFixedRidDescriptor(seedStore, 'control', 'fixed-mesh-b', 'old-owner-b', storeNow);
  storeNow += 101;
  assert.equal((await seedStore.listMeshNodes('play')).items[0].ownerId, 'old-owner-a');
  assert.equal((await seedStore.listMeshNodes('control')).items[0].ownerId, 'old-owner-b');
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: inner,
        options: {
          ownerLeaseRenewIntervalMs: 20_000,
          ownerLeaseTtlMs: 60_000,
          ownerLeaseFencingMarginMs: 5,
          ownerLeaseRenewTimeoutMs: 3_000,
          pollingIntervalMs: 60_000
        }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://local-play', routingId: 'fixed-mesh-a' } },
        control: { router: { bind: 'tcp://local-control', routingId: 'fixed-mesh-b' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory([], rid('fixed-mesh-a'))
  });
  try {
    await runtime.start();
    assert.equal(runtime.options.registration.spotNodes.size, 2);
    assert.equal(runtime.spotNodeRuntime.meshNodesByName.size, 2);
    assert.deepEqual(
      [...runtime.spotNodeRuntime.options.registration.spotNodes.keys()],
      ['play', 'control']
    );
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.deepEqual(
      [...runtime.locationOwner.currentStores.locationStore.ownerLeaseReleaseDelayForTesting.observations.keys()],
      ['mesh-node\0play\0fixed-mesh-a', 'mesh-node\0control\0fixed-mesh-b']
    );
    await finishFixedRidHostStartup(
      runtime,
      clock,
      framework.ZLinkFrameworkRuntimeState.Preparing,
      15,
      1
    );
    assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    const ownerToken = runtime.locationOwner.currentRuntime.currentOwnerToken;
    assert.ok(ownerToken);
    for (const meshName of ['play', 'control']) {
      const descriptor = (await runtime.locationOwner.locationStore().listMeshNodes(meshName)).items[0];
      assert.ok(descriptor);
      assert.equal(descriptor.ownerId, ownerToken.ownerId);
      assert.equal(descriptor.leaseGeneration, ownerToken.leaseGeneration);
      assert.equal(descriptor.state, framework.ZLinkFrameworkRuntimeState.Serving);
    }
  } finally {
    await runtime.stop();
  }
});

test('a Fixed RID host waits after normal stop when its descriptor is Missing', async t => {
  const storeNow = Date.UTC(2026, 6, 3, 0, 0, 0);
  let monotonicMs = 0;
  let meshNodeMutations = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const inner = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  const provider = {
    read: inner.read.bind(inner),
    scan: inner.scan.bind(inner),
    write(request, signal) {
      if (request.mutations.some(mutation => mutation.key.value.startsWith('mesh-node\0'))) {
        meshNodeMutations += 1;
      }
      return inner.write(request, signal);
    }
  };
  const store = new framework.ZLinkLocationStoreRepository(
    provider,
    () => new Date(storeNow),
    undefined,
    () => monotonicMs,
    5
  );
  const locations = {
    storeInstance: provider,
    options: {
      ownerLeaseRenewIntervalMs: 1000,
      ownerLeaseRenewTimeoutMs: 100,
      ownerLeaseTtlMs: 60_000,
      ownerLeaseFencingMarginMs: 5
    }
  };

  const makeHost = () => new framework.ZLinkFrameworkRuntimeHost(
    {
      registration: framework.createFrameworkRegistration({
        locations,
        spotNodes: {
          play: { router: { bind: 'tcp://local-play', routingId: 'fixed-restart-node' } }
        }
      })
    },
    { backendAdapterFactory: fakeBackendAdapterFactory([], rid('fixed-restart-node')) }
  );
  const renew = async host => {
    const renewed = await host.locationOwner.currentRuntime.renewOwnerLeaseOnce();
    return renewed;
  };
  let first;
  let second;
  try {
    first = makeHost();
    await first.start();
    const releaseDelay = first.locationOwner.currentStores.locationStore.ownerLeaseReleaseDelayForTesting;
    assert.equal(releaseDelay.delayMs, 15);
    assert.equal(releaseDelay.observationCountForTesting, 1);
    assert.equal([...releaseDelay.observations.values()][0].t0, 0);
    if (first.status.state === framework.ZLinkFrameworkRuntimeState.Preparing) {
      const recovery = waitForNextHostOwnerRecovery(first);
      monotonicMs += 15;
      assert.equal(await renew(first), true);
      await recovery;
      assert.equal(releaseDelay.observationCountForTesting, 0);
      assert.ok(meshNodeMutations > 0);
      assert.equal((await store.listMeshNodes('play')).items.length, 1);
    }
    assert.equal(first.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    await first.stop();
    const [stoppedDescriptor] = (await store.listMeshNodes('play')).items;
    assert.ok(stoppedDescriptor);
    assert.equal(
      await store.removeMeshNode(
        { meshName: 'play', rid: rid('fixed-restart-node') },
        {
          ownerId: stoppedDescriptor.ownerId,
          leaseGeneration: stoppedDescriptor.leaseGeneration
        }
      ),
      framework.ZLinkLocationWriteStatus.Stored
    );
    assert.equal((await store.listMeshNodes('play')).items.length, 0);
    const mutationsAfterNormalStop = meshNodeMutations;

    second = makeHost();
    await second.start();
    assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.equal(meshNodeMutations, mutationsAfterNormalStop);
    const t0 = monotonicMs;

    const firstRetry = waitForNextHostOwnerRecovery(second);
    assert.equal(await renew(second), true);
    await firstRetry;
    assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.equal(meshNodeMutations, mutationsAfterNormalStop);

    monotonicMs = t0 + 14;
    const earlyRetry = waitForNextHostOwnerRecovery(second);
    assert.equal(await renew(second), true);
    await earlyRetry;
    assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
    assert.equal(meshNodeMutations, mutationsAfterNormalStop);

    monotonicMs = t0 + 15;
    const boundaryRetry = waitForNextHostOwnerRecovery(second);
    assert.equal(await renew(second), true);
    await boundaryRetry;
    assert.equal(second.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
    assert.ok(meshNodeMutations > mutationsAfterNormalStop);
  } finally {
    if (second !== undefined) await second.stop();
    if (first !== undefined) await first.stop();
  }
});

// Location runtime §5: the lease is held but the Store does not confirm the
// descriptor write (provider error, or no answer within the renew timeout).
// Startup ends in Preparing and the next renew publishes the descriptor.
for (const fault of ['error', 'noAnswer']) {
  test(`framework host with an unconfirmed descriptor write (${fault}) starts Preparing and serves after the next renew`, async () => {
    const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
    const inner = new framework.ZLinkInMemoryProviderLocationStore(now);
    const store = new framework.ZLinkLocationStoreRepository(inner, now);
    let descriptorFault = true;
    let faultedWrites = 0;
    let notifyFirstFault;
    let notifySecondFault;
    const firstFault = new Promise(resolve => { notifyFirstFault = resolve; });
    const secondFault = new Promise(resolve => { notifySecondFault = resolve; });
    const provider = {
      read: (key, signal) => inner.read(key, signal),
      scan: (request, signal) => inner.scan(request, signal),
      write(request, signal) {
        const meshNodeWrite = request.mutations.some(
          mutation => mutation.kind === 'put' && mutation.key.value.startsWith('mesh-node')
        );
        if (!descriptorFault || !meshNodeWrite) return inner.write(request, signal);
        faultedWrites += 1;
        if (faultedWrites === 1) notifyFirstFault();
        if (faultedWrites === 2) notifySecondFault();
        if (fault === 'error') return Promise.reject(new Error('descriptor write failed'));
        // No answer: pending until the fault clears, then the write applies.
        return new Promise(resolve => {
          held.push(() => resolve(inner.write(request, signal)));
        });
      }
    };
    const held = [];
    const clearFault = () => {
      descriptorFault = false;
      for (const release of held.splice(0)) release();
    };
    const calls = [];
    const runtime = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration({
        locations: {
          storeInstance: provider,
          options: {
            ownerLeaseRenewIntervalMs: 20,
            ownerLeaseRenewTimeoutMs: 50,
            ownerLeaseFencingMarginMs: 1,
            pollingIntervalMs: 20
          }
        },
        spotNodes: {
          play: {
            router: { bind: 'tcp://local-play', routingId: 'play-node' }
          }
        }
      })
    }, {
      backendAdapterFactory: fakeBackendAdapterFactory(calls, rid('play-node'))
    });

    const locationStoreErrorOperations = [];
    runtime.createSpotManagerOptions().metrics.recordLocationStoreError = operation =>
      locationStoreErrorOperations.push(operation);
    const runtimeTaskFailures = [];
    let notifyRuntimeTaskFailure;
    const runtimeTaskFailure = new Promise(resolve => { notifyRuntimeTaskFailure = resolve; });
    let removeRuntimeTaskFailureHandler;

    const observation = new AbortController();
    try {
      await runtime.start();
      removeRuntimeTaskFailureHandler = runtime.errorSink.onRuntimeTaskException(({ taskName }) =>
        {
          runtimeTaskFailures.push(taskName);
          notifyRuntimeTaskFailure(taskName);
        }
      );
      await firstFault;
      assert.ok(faultedWrites > 0);
      assert.equal(runtime[inspectRuntime]().hasOwnerToken, true);
      assert.deepEqual(
        {
          ownerLeaseUsable: runtime[inspectRuntime]().ownerLeaseUsable,
          ownerAdmissionOpen: runtime[inspectRuntime]().ownerAdmissionOpen,
          ownerLeaseRecoveryRecorded: locationStoreErrorOperations.includes(
            'owner_lease_recovery'
          )
        },
        {
          ownerLeaseUsable: true,
          ownerAdmissionOpen: true,
          ownerLeaseRecoveryRecorded: false
        }
      );
      assert.equal(runtime.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      assert.equal(runtime.status.isReady, false);
      assert.equal((await store.listMeshNodes('play')).items.length, 0);

      const [secondTaskFailure] = await Promise.all([runtimeTaskFailure, secondFault]);
      assert.equal(secondTaskFailure, 'startup descriptor publication');
      assert.deepEqual(
        {
          retryFailureTask: runtimeTaskFailures.at(-1),
          ownerLeaseUsable: runtime[inspectRuntime]().ownerLeaseUsable,
          ownerAdmissionOpen: runtime[inspectRuntime]().ownerAdmissionOpen,
          ownerLeaseRecoveryRecorded: locationStoreErrorOperations.includes(
            'owner_lease_recovery'
          ),
          state: runtime.status.state
        },
        {
          retryFailureTask: 'startup descriptor publication',
          ownerLeaseUsable: true,
          ownerAdmissionOpen: true,
          ownerLeaseRecoveryRecorded: false,
          state: framework.ZLinkFrameworkRuntimeState.Preparing
        }
      );

      const observed = runtime.observe(observation.signal);
      clearFault();
      for await (const event of observed) {
        if (event.status.state === framework.ZLinkFrameworkRuntimeState.Serving) break;
      }

      assert.equal(runtime.status.isReady, true);
      const [descriptor] = (await store.listMeshNodes('play')).items;
      assert.equal(descriptor.state, framework.ZLinkFrameworkRuntimeState.Serving);
    } finally {
      observation.abort();
      removeRuntimeTaskFailureHandler?.();
      clearFault();
      await runtime.stop();
    }
  });
}

test('framework host does not overlap owner lease recovery attempts', async t => {
  let monotonicMs = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const scenario = recoverableHostScenario(1_000, 1);
  const runtime = scenario.runtime;
  const recoveryCounts = {
    startStatefulAuthority: 0,
    spotAutoConnect: 0,
    channelAutoConnect: 0,
    enterServing: 0
  };
  let preparingPublicationAttempts = 0;
  let activeRecoveryAttempts = 0;
  let maximumActiveRecoveryAttempts = 0;
  let unblockRecoveryPublication;
  let notifyPreparingPublication;
  let notifyRecoveryComplete;
  const preparingPublicationStarted = new Promise(resolve => {
    notifyPreparingPublication = resolve;
  });
  const recoveryPublicationGate = new Promise(resolve => {
    unblockRecoveryPublication = resolve;
  });
  const armRecoveryCompletion = () => new Promise(resolve => {
    notifyRecoveryComplete = resolve;
  });
  let recoveryComplete = armRecoveryCompletion();
  let removeRenewalObserver;

  try {
    await runtime.start();
    const locationRuntime = runtime.locationOwner.currentRuntime;
    const spotNodeRuntime = runtime.spotNodeRuntime;
    const channelRuntime = runtime.channelRuntime;

    const publishMeshNodeState = spotNodeRuntime.publishMeshNodeState.bind(spotNodeRuntime);
    spotNodeRuntime.publishMeshNodeState = async (state, ...args) => {
      if (state === framework.ZLinkFrameworkRuntimeState.Preparing) {
        preparingPublicationAttempts += 1;
        notifyPreparingPublication();
        await recoveryPublicationGate;
      }
      return await publishMeshNodeState(state, ...args);
    };

    const resumeOwnerLeaseOwnedWork = runtime.resumeOwnerLeaseOwnedWork.bind(runtime);
    runtime.resumeOwnerLeaseOwnedWork = async (...args) => {
      activeRecoveryAttempts += 1;
      maximumActiveRecoveryAttempts = Math.max(
        maximumActiveRecoveryAttempts,
        activeRecoveryAttempts
      );
      try {
        await resumeOwnerLeaseOwnedWork(...args);
      } finally {
        activeRecoveryAttempts -= 1;
        notifyRecoveryComplete();
      }
    };

    const startStatefulAuthority = runtime.startStatefulAuthority.bind(runtime);
    runtime.startStatefulAuthority = async (...args) => {
      recoveryCounts.startStatefulAuthority += 1;
      return await startStatefulAuthority(...args);
    };
    const startSpotAutoConnect = spotNodeRuntime.startLocationAutoConnect.bind(spotNodeRuntime);
    spotNodeRuntime.startLocationAutoConnect = async (...args) => {
      recoveryCounts.spotAutoConnect += 1;
      return await startSpotAutoConnect(...args);
    };
    const startChannelAutoConnect = channelRuntime.startLocationAutoConnect.bind(channelRuntime);
    channelRuntime.startLocationAutoConnect = async (...args) => {
      recoveryCounts.channelAutoConnect += 1;
      return await startChannelAutoConnect(...args);
    };
    const enterServing = runtime.enterServing.bind(runtime);
    runtime.enterServing = async (...args) => {
      recoveryCounts.enterServing += 1;
      return await enterServing(...args);
    };

    let renewals = 0;
    const renewalObserver = () => {
      renewals += 1;
    };
    locationRuntime.addOwnerLeaseRenewedHandler(renewalObserver);
    removeRenewalObserver = () => locationRuntime.removeOwnerLeaseRenewedHandler(renewalObserver);

    scenario.recover();
    await locationRuntime.renewOwnerLeaseOnce();
    await preparingPublicationStarted;
    await locationRuntime.renewOwnerLeaseOnce();
    assert.ok(renewals >= 2);
    assert.equal(maximumActiveRecoveryAttempts, 1);
    assert.equal(preparingPublicationAttempts, 1);

    const serving = observeRuntimeState(runtime, framework.ZLinkFrameworkRuntimeState.Serving);
    unblockRecoveryPublication();
    await recoveryComplete;
    recoveryComplete = armRecoveryCompletion();
    monotonicMs = 3;
    await locationRuntime.renewOwnerLeaseOnce();
    assert.equal(await serving.reached, true);
    serving.controller.abort();
    await recoveryComplete;
    assert.equal(preparingPublicationAttempts, 2);
    assert.deepEqual(recoveryCounts, {
      startStatefulAuthority: 1,
      spotAutoConnect: 1,
      channelAutoConnect: 1,
      enterServing: 1
    });
  } finally {
    unblockRecoveryPublication();
    removeRenewalObserver?.();
    scenario.recover();
    try {
      if (activeRecoveryAttempts > 0) await recoveryComplete;
    } finally {
      await runtime.stop();
    }
  }
});

test('framework host whose descriptor is claimed by another owner fails startup', async t => {
  let monotonicMs = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new framework.ZLinkInMemoryProviderLocationStore(now);
  const createHost = () => new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: inner,
        options: {
          ownerLeaseRenewIntervalMs: 20,
          ownerLeaseRenewTimeoutMs: 50,
          ownerLeaseFencingMarginMs: 1,
          pollingIntervalMs: 20
        }
      },
      spotNodes: {
        play: {
          router: { bind: 'tcp://local-play', routingId: 'play-node' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory([], rid('play-node'))
  });
  const first = createHost();
  const second = createHost();
  try {
    await first.start();
    if (first.status.state === framework.ZLinkFrameworkRuntimeState.Preparing) {
      const serving = observeRuntimeState(first, framework.ZLinkFrameworkRuntimeState.Serving);
      monotonicMs = 3;
      assert.equal(await first.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
      assert.equal(await serving.reached, true);
      serving.controller.abort();
    }
    await assert.rejects(second.start());
  } finally {
    await second.stop().catch(() => {});
    await first.stop();
  }
});

test('framework host retries recovery publication after the first owner lease publication fails', async () => {
  const scenario = recoverableHostScenario();
  let publicationAttempts = 0;
  try {
    await scenario.runtime.start();
    const publish = scenario.runtime.spotNodeRuntime.publishMeshNodeState
      .bind(scenario.runtime.spotNodeRuntime);
    scenario.runtime.spotNodeRuntime.publishMeshNodeState = async (...args) => {
      publicationAttempts += 1;
      if (publicationAttempts === 1) {
        throw new Error('first recovery publication failed');
      }
      return await publish(...args);
    };

    scenario.recover();
    await waitForCondition(() => publicationAttempts >= 1
      && !scenario.runtime[inspectRuntime]().ownerLeaseUsable);
    await waitForCondition(async () =>
      (await scenario.store.listMeshNodes('play')).items.length === 1
    );

    assert.ok(publicationAttempts >= 3);
    assert.equal(scenario.runtime[inspectRuntime]().ownerLeaseUsable, true);
  } finally {
    scenario.recover();
    await scenario.runtime.stop();
  }
});

test('degraded host rejects object messages and timers until owner lease recovery', async () => {
  const scenario = recoverableHostScenario();
  let actorMessages = 0;
  let timerTicks = 0;
  let registry;
  try {
    await scenario.runtime.start();
    scenario.spotManager.dispatchMeshActor = (_origin, _mesh, _owner, _record, admit) =>
      admit(async () => { actorMessages += 1; });
    registry = new framework.ZLinkSpotTimerRegistry(
      undefined,
      undefined,
      undefined,
      scenario.runtime.createSpotManagerOptions().statefulExecution.admissionOpen
    );
    class LeaseTimerHandler {
      async handle() { timerTicks += 1; }
    }
    await registry.add(
      'owner-lease',
      2,
      undefined,
      LeaseTimerHandler,
      new framework.ZLinkSpotSerialTurnExecutor(true, 'owner-lease-timer'),
      {}
    );

    await assert.rejects(
      scenario.runtime.dispatchMeshRecord(() => 'remote',
        'play',
        { ownerKind: framework.ReadyOwnerKind.Node },
        { kind: framework.ReceiveKind.ActorSend, parts: [] }
      ),
      /admission is closed/u
    );
    await new Promise(resolve => setTimeout(resolve, 10));
    assert.equal(actorMessages, 0);
    assert.equal(timerTicks, 0);

    scenario.recover();
    await waitForCondition(() => scenario.runtime[inspectRuntime]().ownerLeaseUsable);
    await scenario.runtime.dispatchMeshRecord(() => 'remote',
      'play',
      { ownerKind: framework.ReadyOwnerKind.Node },
      { kind: framework.ReceiveKind.ActorSend, parts: [] }
    );
    await waitForCondition(() => timerTicks > 0);
    assert.equal(actorMessages, 1);
  } finally {
    await registry?.dispose();
    scenario.recover();
    await scenario.runtime.stop();
  }
});

test('degraded host rejects factory and restore confirmation until owner lease recovery', async () => {
  class LeaseOwnedSpot {
    async onCreate() { return { accepted: true }; }
  }
  const scenario = recoverableHostScenario();
  scenario.spotManager = new framework.DefaultZLinkSpotManager({
    detachedTaskRunner: detachedTaskRunner,
    ...scenario.runtime.createSpotManagerOptions(),
    spotFactories: [LeaseOwnedSpot]
  });
  scenario.runtime.setSpotManager(scenario.spotManager);
  try {
    await scenario.runtime.start();
    installFakeNativeSpotFactory(scenario.runtime);
    await assert.rejects(
      scenario.spotManager.create('play', LeaseOwnedSpot),
      /admission is closed/u
    );
    await assert.rejects(
      scenario.spotManager.getOrCreateWithAuthority(
        'play',
        LeaseOwnedSpot,
        rid('restore-before-lease'),
        undefined,
        {
          objectKind: 'user_spot',
          stableType: 'lease-owned',
          objectGeneration: 1n,
          authorityOwnerGeneration: 1n
        }
      ),
      /admission is closed/u
    );

    scenario.recover();
    await waitForCondition(() => scenario.runtime[inspectRuntime]().ownerLeaseUsable);
    const created = await scenario.spotManager.create('play', LeaseOwnedSpot);
    const restored = await scenario.spotManager.getOrCreateWithAuthority(
      'play',
      LeaseOwnedSpot,
      rid('restore-after-lease'),
      undefined,
      {
        objectKind: 'user_spot',
        stableType: 'lease-owned',
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n
      }
    );
    assert.equal(created.state, framework.ZLinkSpotCreateState.Created);
    assert.equal(restored.state, framework.ZLinkSpotCreateState.Created);
  } finally {
    scenario.recover();
    await scenario.runtime.stop();
  }
});

test('degraded host rejects stateful target, relocation and peer capacity admission until owner lease recovery', async t => {
  let monotonicMs = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const scenario = recoverableHostScenario();
  let targetCallbacks = 0;
  try {
    await scenario.runtime.start();
    scenario.spotManager.dispatchMeshInstance = (_mesh, _owner, _record, admit) =>
      admit(async () => {
        targetCallbacks += 1;
        return true;
      });
    assert.equal(scenario.runtime.status.acceptingWork, false);
    assert.equal(scenario.runtime.spotNodeRuntime.options.peerAdmissionSealed('play'), true);
    await assert.rejects(
      scenario.runtime.dispatchMeshRecord(() => 'remote',
        'play',
        { ownerKind: framework.ReadyOwnerKind.Spot },
        { kind: framework.ReceiveKind.InstanceSpotActivation, parts: [] }
      ),
      /admission is closed/u
    );
    assert.equal(targetCallbacks, 0);
    const blocked = await scenario.runtime.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 50
    });
    assert.equal(blocked.outcome, framework.ZLinkFrameworkRelocationOutcome.Blocked);
    // A lease-less host is Preparing (location runtime §5), and Relocate from
    // Preparing is Blocked/RuntimeNotReady (host relocation §3).
    assert.equal(blocked.reason, framework.ZLinkFrameworkRelocationReason.RuntimeNotReady);

    const serving = observeRuntimeState(
      scenario.runtime,
      framework.ZLinkFrameworkRuntimeState.Serving
    );
    scenario.recover();
    const initialRecovery = waitForNextHostOwnerRecovery(scenario.runtime);
    assert.equal(await scenario.runtime.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
    await initialRecovery;
    monotonicMs = 3;
    const retryRecovery = waitForNextHostOwnerRecovery(scenario.runtime);
    assert.equal(await scenario.runtime.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
    await retryRecovery;
    assert.equal(await serving.reached, true);
    serving.controller.abort();
    assert.equal(scenario.runtime.status.acceptingWork, true);
    assert.equal(scenario.runtime.spotNodeRuntime.options.peerAdmissionSealed('play'), false);
    await scenario.runtime.dispatchMeshRecord(() => 'remote',
      'play',
      { ownerKind: framework.ReadyOwnerKind.Spot },
      { kind: framework.ReceiveKind.InstanceSpotActivation, parts: [] }
    );
    assert.equal(targetCallbacks, 1);
    const admitted = await scenario.runtime.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 50
    });
    assert.notEqual(admitted.reason, framework.ZLinkFrameworkRelocationReason.StoreUnavailable);
  } finally {
    scenario.recover();
    await scenario.runtime.stop();
  }
});

test('framework host startup begins a lifecycle flow', async () => {
  const calls = [];
  const backendAdapterFactory = fakeBackendAdapterFactory(calls, rid('lifecycle-node'));
  const createChannelAdapter = backendAdapterFactory.createChannelAdapter;
  let startupFlow;
  backendAdapterFactory.createChannelAdapter = () => {
    startupFlow = flowContext.currentOrCreateFlow('Lifecycle', false);
    return createChannelAdapter();
  };
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      dispatch: {
        diagnostics: {
          messageFlow: 'normal',
          sampleRate: 1,
          includeMessageSizes: false
        }
      }
    })
  }, { backendAdapterFactory });

  await runtime.start();
  await runtime.stop();

  assert.equal(startupFlow?.flowOrigin, 'Lifecycle');
  assert.match(startupFlow?.flowId ?? '', /^[0-9a-f-]{36}$/);
});

test('framework runtime host starts channel auto-connect loops from location peers', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');

  const remoteOwner = await store.claimOwnerLease('owner-remote', 30000);
  assert.equal(remoteOwner.kind, 'claimed');
  await store.updateClientServer(
    clientServerDescriptor(remoteOwner.token, 'api', 'node-b', 'tcp://remote-api'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    routePeer('manual-mesh', 'owner-remote', 'node-b', 'tcp://remote-manual-route'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-remote', 'node-b', 'tcp://remote-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updateFanoutPublisher(
    fanoutDescriptor(remoteOwner.token, 'events', 'node-b', 'tcp://remote-events'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );

  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000,
          pollingIntervalMs: 1000
        }
      },
      channels: {
        api: {
          client: {}
        },
        manual: {
          client: {
            manualConnections: ['tcp://manual']
          }
        },
        events: {
          subscriber: {},
          publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
        },
        'local-events': {
          routingId: 'node-a',
          publisher: { bind: 'tcp://local-events' }
        },
        'manual-events': {
          subscriber: { manualConnections: ['tcp://manual-events'] },
          publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
        }
      },
      routeChannels: [{
        routerChannelId: 'mesh',
        bind: 'tcp://local-route',
        routingId: 'node-a'
      }, {
        routerChannelId: 'manual-mesh',
        bind: 'tcp://local-manual-route',
        routingId: 'node-a',
        manualConnections: ['tcp://manual-route']
      }],
      spotNodes: {
        play: {
          router: { bind: 'tcp://127.0.0.1:9101', routingId: 'node-a' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  try {
    await runtime.start();
    assert.deepEqual(
      calls.filter((call) =>
        (call.startsWith('dealer:') || call.startsWith('router:') || call.startsWith('subscriber:'))
        && call.includes(':connect:')).sort(),
      [
        'dealer:api:connect:tcp://remote-api',
        'dealer:manual:connect:tcp://manual',
        'router:manual-mesh:connect:tcp://manual-route',
        'subscriber:manual-events:connect:tcp://manual-events',
        'subscriber:events:connect:tcp://remote-events'
      ].sort()
    );
    const publisherRows = await store.listFanoutPublishers('local-events');
    assert.equal(publisherRows.items.length, 1);
    assert.equal(String(publisherRows.items[0].publisherRid), 'node-a');
  } finally {
    await runtime.stop();
  }
  assert.ok(calls.includes('dealer:api:disconnect:tcp://remote-api'));
  assert.ok(calls.includes('subscriber:events:disconnect:tcp://remote-events'));
  assert.equal(calls.some((call) => call.includes('tcp://remote-manual')), false);
  assert.equal(
    calls.filter((call) => call === 'dealer:manual:disconnect:tcp://manual').length,
    0
  );
});

test('manual Mesh router connection suppresses only the matching store-driven route', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');

  await store.claimOwnerLease('owner-remote', 30000);
  await store.updatePeer(
    spotPeer('owner-remote', 'node-b', 'tcp://remote-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-lower', 'node-0', 'tcp://lower-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-remote', 'node-c', 'tcp://manual-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );

  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000,
          pollingIntervalMs: 1000
        }
      },
      spotNodes: {
        play: {
          router: {
            bind: 'tcp://local-spot',
            routingId: 'node-a',
            manualConnections: ['tcp://manual-spot']
          },
          pubSub: {
            bind: 'tcp://local-pub',
            routingId: 'node-a'
          }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  try {
    await runtime.start();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(calls.some((call) => call.includes('tcp://remote-spot')), false);
    assert.equal(calls.some((call) => call.includes('tcp://remote-pub')), false);
    assert.equal(calls.some((call) => call.includes('tcp://lower-spot')), false);
    assert.equal(calls.some((call) => call.includes('tcp://lower-pub')), false);
    assert.ok(calls.includes('spot:connectPeer:tcp://manual-spot'));
  } finally {
    await runtime.stop();
  }

  assert.equal(calls.filter((call) => call === 'spot:disconnectPeer:tcp://manual-spot').length, 0);
});

async function resolveFrameworkRegistration(module) {
  const provider = module.providers.find((entry) => entry.provide === nestjs.ZLINK_FRAMEWORK_REGISTRATION);
  if ('useValue' in provider) {
    return provider.useValue;
  }
  const container = await resolveModuleProviders(module, [nestjs.ZLINK_FRAMEWORK_REGISTRATION]);
  return container.get(nestjs.ZLINK_FRAMEWORK_REGISTRATION);
}

test('concurrent relocation with a different deadline joins the running operation', async t => {
  let monotonicMs = 0;
  t.mock.method(performance, 'now', () => monotonicMs);
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const calls = [];
  const nodeRid = rid('node-join-deadline');
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000,
          ownerLeaseFencingMarginMs: 1
        }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://127.0.0.1:9137', routingId: 'node-join-deadline' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  try {
    await runtime.start();
    if (runtime.status.state === framework.ZLinkFrameworkRuntimeState.Preparing) {
      const serving = observeRuntimeState(runtime, framework.ZLinkFrameworkRuntimeState.Serving);
      monotonicMs = 3;
      assert.equal(await runtime.locationOwner.currentRuntime.renewOwnerLeaseOnce(), true);
      assert.equal(await serving.reached, true);
      serving.controller.abort();
    }
    // Two concurrent relocations with the same mode and effective target
    // version but different deadlines. The join key is (mode, effective target
    // version) and excludes the deadline, so the second call joins the running
    // operation and shares its terminal result rather than being rejected as a
    // concurrent operation. The parity would break if the join keyed on the
    // deadline.
    const first = runtime.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 200
    });
    const second = runtime.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 800
    });
    const [firstResult, secondResult] = await Promise.all([first, second]);

    // The host is Serving so the operation actually started (not an early
    // RuntimeNotReady), which is what makes the join meaningful.
    assert.notEqual(
      firstResult.reason,
      framework.ZLinkFrameworkRelocationReason.RuntimeNotReady
    );
    assert.deepEqual(secondResult, firstResult);
    assert.notEqual(
      secondResult.reason,
      framework.ZLinkFrameworkRelocationReason.OperationInProgress
    );
  } finally {
    await runtime.stop();
  }
});

function recoverableHostScenario(ownerLeaseRenewIntervalMs = 20, ownerLeaseFencingMarginMs = 1) {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new framework.ZLinkInMemoryProviderLocationStore(now);
  let unavailable = true;
  const provider = {
    read(key, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.read(key, signal);
    },
    write(request, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.write(request, signal);
    },
    scan(request, signal) {
      if (unavailable) throw new Error('location store unavailable');
      return inner.scan(request, signal);
    }
  };
  const calls = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs,
          ownerLeaseRenewTimeoutMs: 5,
          ownerLeaseFencingMarginMs,
          pollingIntervalMs: 20
        }
      },
      spotNodes: {
        play: { router: { bind: 'tcp://local-play', routingId: 'play-node' } }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, rid('play-node'))
  });
  const spotManager = new framework.DefaultZLinkSpotManager(runtime.createSpotManagerOptions());
  runtime.setSpotManager(spotManager);
  return {
    runtime,
    spotManager,
    store: new framework.ZLinkLocationStoreRepository(inner, now),
    recover() { unavailable = false; }
  };
}

function installFakeNativeSpotFactory(runtime) {
  const node = runtime.spotNodeRuntime.meshNode('play');
  let generation = 0n;
  const nativeSpot = () => {
    const lifecycleGeneration = ++generation;
    return {
      status: () => ({ lifecycleGeneration }),
      setSubscription() {},
      close() {}
    };
  };
  node.getOrCreateSpot = () => ({ spot: nativeSpot(), created: true });
  node.restoreSpotAuthority = () => nativeSpot();
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

async function waitForCondition(predicate) {
  for (let attempt = 0; attempt < 100; attempt++) {
    if (await predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 5));
  }
  throw new Error('Timed out waiting for the expected host state.');
}

function observeRuntimeState(runtime, state) {
  const controller = new AbortController();
  const reached = (async () => {
    for await (const event of runtime.observe(controller.signal)) {
      if (event.status.state === state) return true;
    }
    return false;
  })();
  return { controller, reached };
}

function waitForNextHostOwnerRecovery(host) {
  const original = host.resumeOwnerLeaseOwnedWork.bind(host);
  let resolve;
  const completed = new Promise(done => { resolve = done; });
  host.resumeOwnerLeaseOwnedWork = async (...args) => {
    try {
      return await original(...args);
    } finally {
      host.resumeOwnerLeaseOwnedWork = original;
      resolve();
    }
  };
  return completed;
}

async function seedFixedRidDescriptor(store, meshName, nodeRid, ownerId, storeNow) {
  const owner = await store.claimOwnerLease(ownerId, 100);
  assert.equal(owner.kind, 'claimed');
  if (owner.kind !== 'claimed') throw new Error('previous owner lease claim failed');
  const descriptor = {
    meshName,
    rid: rid(nodeRid),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: `tcp://${nodeRid}`,
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 1 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: nodeRid,
    ownerId: owner.token.ownerId,
    leaseGeneration: owner.token.leaseGeneration,
    updatedAt: new Date(storeNow)
  };
  assert.equal(
    (await store.updateMeshNode(descriptor, framework.ZLinkLocationWriteIntent.NewClaim)).status,
    framework.ZLinkLocationWriteStatus.Stored
  );
}

function fakeBackendAdapterFactory(calls, nodeRid) {
  return {
    createChannelAdapter() {
      return {
        createContext() {
          return {
            nativeInstance: {},
            getCoreHwmBudgetSnapshot() { return undefined; },
            resetCoreHwmBudgetMetrics() {},
            shutdown() {},
            async dispose() {
              calls.push('context:dispose');
            }
          };
        },
        createDealerSocket() {
          return fakeConnectableSocket(calls, 'dealer');
        },
        createRouterSocket() {
          const socket = fakeConnectableSocket(calls, 'router');
          return Object.assign(socket, {
            setRoutingId() {},
            recv() { return null; },
            reply() { return { message() { return this; }, submit() {} }; }
          });
        },
        createPublisherSocket() {
          return {
            nativeInstance: {},
            setChannelName(channelName) { this.channelName = channelName; },
            bind(endpoint) { calls.push(`publisher:${this.channelName ?? ''}:bind:${endpoint}`); },
            publish() { return true; },
            async dispose() {}
          };
        },
        createSubscriberSocket() {
          return {
            ...fakeConnectableSocket(calls, 'subscriber'),
            setSubscription() {},
            subscribe() { return false; }
          };
        },
        createReadablePoller() {
          return {
            wait() { return false; },
            waitForReadable(signal) {
              return new Promise((resolve) => {
                if (signal?.aborted === true) resolve(false);
                else signal?.addEventListener('abort', () => resolve(false), { once: true });
              });
            },
            markDrained() {},
            dispose() {}
          };
        },
        createTopicMessage() {
          return { parts: [] };
        }
      };
    },
    createMeshAdapter() {
      return {
        createMeshNode(_context, options) {
          let routingId = options.routingId === undefined ? String(nodeRid) : String(options.routingId);
          let nextConnectionIntent = 1n;
          return {
            nativeInstance: {},
            setRoutingId(value) { routingId = String(value); },
            setBind() {},
            addChannelName() {},
            setChannelWeight() {},
            setPlacementWeight() {},
            configureObjectPlacement() {},
            createPublisher() { return { close() {} }; },
            start() {},
            setReadyHandler() {},
            setCompletionHandler() {},
            createReadyBatch() { return fakeReadyBatch(); },
            createReceiveBatch() { return fakeReceiveBatch(); },
            drainReady() { return { ok: false, hasResidue: false, records: [] }; },
            connectPeer({ endpoint, expectedRid }) {
              calls.push(`spot:connectPeer:${endpoint}`);
              if (expectedRid !== undefined) {
                calls.push(`spot:connectPeerRid:${expectedRid.toHex()}:${endpoint}`);
              }
              return nextConnectionIntent++;
            },
            removePeerConnection(connectionIntent) {
              calls.push(`spot:removePeerConnection:${connectionIntent}`);
            },
            disconnectPeer(peerRid) { calls.push(`spot:disconnectPeerRid:${peerRid.toHex()}`); },
            status() {
              return {
                routingId,
                lifecycleGeneration: 3n,
                descriptorRevision: 1n,
                localEndpoint: 'tcp://127.0.0.1:9101',
                state: 3,
                lastChangedMs: 1n,
                pendingApplicationMessages: 0n,
                pendingInfrastructureMessages: 0n
              };
            },
            peers() { return []; },
            async shutdown() {},
            async close() {
              calls.push('spot:dispose');
            }
          };
        }
      };
    },
    createStreamAdapter() {
      return {
        createStreamSocket() {
          throw new Error('not used');
        }
      };
    },
    createMonitoringAdapter() {
      return {
        openSocketMonitor() {
          return {
            nativeInstance: {},
            onEvent() {},
            recv() { return null; },
            drain() { return 0; },
            status() { return {}; },
            async dispose() {}
          };
        }
      };
    }
  };
}

function fakeReadyBatch() {
  return {
    reset() {},
    takeClaim() { throw new Error('no ready records'); },
    close() {}
  };
}

function fakeReceiveBatch() {
  return {
    reset() {},
    close() {}
  };
}

function fakeConnectableSocket(calls, kind) {
  const socket = {
    nativeInstance: {},
    channelName: '',
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: 0,
    setReceiveFlowState() {},
    setChannelName(channelName) {
      this.channelName = channelName;
    },
    setRoutingId() {},
    bind(endpoint) {
      calls.push(`${kind}:${this.channelName}:bind:${endpoint}`);
    },
    connect(endpoint) {
      calls.push(`${kind}:${this.channelName}:connect:${endpoint}`);
    },
    disconnect(endpoint) {
      calls.push(`${kind}:${this.channelName}:disconnect:${endpoint}`);
    },
    attachDiscovery() {},
    send() { return true; },
    request() { return true; },
    recv() { return null; },
    async dispose() {}
  };
  if (kind !== 'router') {
    return socket;
  }
  socket.options = {
    set probe(value) {
      calls.push(`${kind}:${socket.channelName}:probe:${value}`);
    },
    setConnectRoutingId(routingId) {
      calls.push(`${kind}:${socket.channelName}:connectRoutingId:${routingId.toHex()}`);
    }
  };
  return socket;
}

function clientServerDescriptor(owner, channelName, nodeRid, endpoint) {
  return {
    channelName,
    serverRid: nodeRid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint,
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'default',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function routePeer(meshName, ownerId, nodeRid, endpoint) {
  return {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName,
    nodeRid,
    role: framework.ZLinkLocationRole.Router,
    endpoint,
    weight: 100,
    value: 0n,
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}

function fanoutDescriptor(owner, channelName, nodeRid, endpoint) {
  return {
    channelName,
    publisherRid: nodeRid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'default',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function spotPeer(ownerId, nodeRid, endpoint) {
  return {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'play',
    nodeRid,
    role: framework.ZLinkLocationRole.Router,
    endpoint,
    weight: 100,
    value: 0n,
    metadata: {},
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}

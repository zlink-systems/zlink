const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

test('RuntimeEventQueue retains the newest terminal status and reports the discarded oldest one', async () => {
  const queue = new internal.RuntimeEventQueue(1);
  queue.seal({ sequence: 1 });
  queue.complete({ sequence: 2 });

  const terminal = await queue.next();
  assert.equal(terminal.done, false);
  assert.equal(terminal.value.status.sequence, 2);
  assert.equal(terminal.value.loss.discardedTerminalCount, 1n);
  assert.equal((await queue.next()).done, true);
});

test('RuntimeEventQueue coalesces its initial status and preserves an initial terminal status', async () => {
  const queue = new internal.RuntimeEventQueue(1);
  queue.push({ sequence: 1 }, 'mesh');
  queue.push({ sequence: 2 }, 'mesh');
  const latest = await queue.next();
  assert.equal(latest.value.status.sequence, 2);
  assert.deepEqual(latest.value.loss, { coalescedCount: 1n, discardedTerminalCount: 0n });
  await queue.return();

  const terminal = new internal.RuntimeEventQueue(1);
  terminal.pushTerminal({ sequence: 3 }, 'mesh');
  terminal.push({ sequence: 4 }, 'mesh');
  const retained = await terminal.next();
  assert.equal(retained.value.status.sequence, 3);
  assert.deepEqual(retained.value.loss, { coalescedCount: 1n, discardedTerminalCount: 0n });
  await terminal.return();
});

test('ClientServer runtime projects minimal status and emits complete status changes', async () => {
  let changed;
  let weight = 100;
  const manager = {
    clientServerTopology() {
      return {
        localRole: 'clientAndServer',
        pendingRequestCount: 2,
        descriptors: [
          {
            channelName: 'orders',
            serverRoutingId: 'server-a',
            lifecycleGeneration: 3n,
            descriptorRevision: 5n,
            weight,
            state: 'serving',
            securityIdentity: 'default',
            effectiveMaxMessageBytes: 1024,
            advertisedEndpoint: 'tcp://127.0.0.1:10000'
          }
        ]
      };
    },
    observeClientServerTopology(_channelName, callback) {
      changed = callback;
      return () => {
        changed = undefined;
      };
    }
  };
  const runtime = new internal.ZLinkClientServerRuntimeProjection(() => manager);
  const snapshot = runtime.snapshot('orders');

  assert.equal(snapshot.localRole, 'clientAndServer');
  assert.equal(snapshot.isReady, true);
  assert.equal(snapshot.readyTargetCount, 1);
  assert.equal(snapshot.targets[0].nodeRid, 'server-a');
  assert.equal('lifecycleGeneration' in snapshot.targets[0], false);
  assert.equal('descriptorSource' in snapshot.targets[0], false);

  const events = runtime.observe('orders')[Symbol.asyncIterator]();
  const initial = await Promise.race([events.next(), Promise.resolve(undefined)]);
  assert.notEqual(initial, undefined, 'Observe must retain current status before topology changes');
  assert.equal(initial.value.status.channelName, 'orders');
  assert.equal(initial.value.status.targets[0].weight, 100);
  weight = 200;
  changed();
  const status = await events.next();
  assert.equal(status.value.status.channelName, 'orders');
  assert.equal(status.value.status.isReady, true);
  assert.equal(status.value.status.targets[0].weight, 200);
  assert.deepEqual(status.value.loss, { coalescedCount: 0n, discardedTerminalCount: 0n });
  await events.return();
  assert.equal(typeof changed, 'function');
  runtime.stopObservers();
  assert.equal(changed, undefined);
});

test('Fanout runtime projects minimal publisher status and emits complete status changes', async () => {
  let changed;
  let publisherRoutingId = 'publisher-a';
  const manager = {
    fanoutTopology() {
      return {
        descriptors: [
          {
            channelName: 'events',
            publisherRoutingId,
            lifecycleGeneration: 7n,
            descriptorRevision: 9n,
            advertisedEndpoint: 'tcp://127.0.0.1:10001',
            state: 'serving'
          }
        ]
      };
    },
    observeFanoutTopology(_channelName, callback) {
      changed = callback;
      return () => {
        changed = undefined;
      };
    }
  };
  const runtime = new internal.ZLinkFanoutRuntimeProjection(() => manager);
  const snapshot = runtime.snapshot('events');

  assert.equal(snapshot.readyPublisherCount, 1);
  assert.equal(snapshot.publishers[0].state, framework.ZLinkPeerState.Ready);
  assert.equal('lifecycleGeneration' in snapshot.publishers[0], false);
  assert.equal('descriptorRevision' in snapshot.publishers[0], false);
  assert.equal('endpoint' in snapshot.publishers[0], false);

  const events = runtime.observe('events')[Symbol.asyncIterator]();
  const initial = await Promise.race([events.next(), Promise.resolve(undefined)]);
  assert.notEqual(initial, undefined, 'Observe must retain current status before topology changes');
  assert.equal(initial.value.status.channelName, 'events');
  assert.equal(initial.value.status.publishers[0].nodeRid, 'publisher-a');
  publisherRoutingId = 'publisher-b';
  changed();
  const status = await events.next();
  assert.equal(status.value.status.channelName, 'events');
  assert.equal(status.value.status.publishers[0].nodeRid, 'publisher-b');
  assert.equal(status.value.status.sequence, initial.value.status.sequence + 1n);
  await events.return();
});

for (const kind of ['ClientServer', 'Fanout']) test(`${kind} status publishes one sequence per channel to every observer`, async t => {
  const callbacks = new Map();
  const states = new Map([['events', 'serving'], ['other', 'serving']]);
  const manager = {
    fanoutTopology(channelName) {
      return { descriptors: [{ publisherRoutingId: channelName, state: states.get(channelName) }] };
    },
    observeFanoutTopology(channelName, callback) {
      const listeners = callbacks.get(channelName) ?? new Set();
      callbacks.set(channelName, listeners);
      listeners.add(callback);
      return () => listeners.delete(callback);
    }
  };
  manager.clientServerTopology = channelName => ({
    localRole: 'client',
    descriptors: [{ serverRoutingId: channelName, weight: 1, state: states.get(channelName) }]
  });
  manager.observeClientServerTopology = manager.observeFanoutTopology;
  const runtime = new internal[`ZLink${kind}RuntimeProjection`](() => manager);
  const first = runtime.observe('events')[Symbol.asyncIterator]();
  const second = runtime.observe('events')[Symbol.asyncIterator]();
  const other = runtime.observe('other')[Symbol.asyncIterator]();
  t.after(async () => {
    await first.return();
    await second.return();
    await other.return();
  });
  await first.next();
  await second.next();
  await other.next();
  states.set('events', 'retiring');
  for (const callback of callbacks.get('events')) callback();
  assert.equal(runtime.snapshot('events').sequence, 2n);
  assert.equal(runtime.snapshot('other').sequence, 1n);
  assert.equal((await first.next()).value.status.sequence, 2n);
  assert.equal((await second.next()).value.status.sequence, 2n);
  for (const callback of callbacks.get('events')) callback();
  assert.equal(runtime.snapshot('events').sequence, 2n);
  states.set('other', 'retiring');
  for (const callback of callbacks.get('other')) callback();
  assert.equal((await other.next()).value.status.sequence, 2n);
  assert.equal(runtime.snapshot('events').sequence, 2n);
  const late = runtime.observe('events')[Symbol.asyncIterator]();
  assert.equal((await late.next()).value.status.sequence, 2n);
  await late.return();
  await first.return();
  await second.return();
  assert.equal(callbacks.get('events').size, 1);
  states.set('events', 'stopped');
  assert.equal(runtime.snapshot('events').sequence, 3n);
  const resumed = runtime.observe('events')[Symbol.asyncIterator]();
  assert.equal((await resumed.next()).value.status.sequence, 3n);
  await resumed.return();
  manager.clientServerTopology = manager.fanoutTopology = () => {
    throw new Error('Native runtime has been disposed');
  };
  runtime.stopObservers();
  assert.equal(runtime.snapshot('events').sequence, 4n);
  runtime.stopObservers();
  assert.equal(runtime.snapshot('events').sequence, 4n);
  const terminal = runtime.observe('events')[Symbol.asyncIterator]();
  assert.equal((await terminal.next()).value.status.sequence, 4n);
  await terminal.return();
});

for (const kind of ['ClientServer', 'Fanout']) test(`${kind} failed status seals its source and remains available after disposal`, async () => {
  let hostState = framework.ZLinkFrameworkRuntimeState.Serving;
  const manager = {
    clientServerTopology: () => ({ localRole: 'client', descriptors: [] }),
    fanoutTopology: () => ({ descriptors: [] }),
    observeClientServerTopology: () => () => {},
    observeFanoutTopology: () => () => {}
  };
  const runtime = new internal[`ZLink${kind}RuntimeProjection`](() => manager, () => hostState);
  const events = runtime.observe('events')[Symbol.asyncIterator]();
  await events.next();
  hostState = framework.ZLinkFrameworkRuntimeState.Error;
  const publicationStarted = Date.now();
  runtime.hostStateChanged();
  const failed = (await events.next()).value.status;
  assert.equal(failed.state, framework.ZLinkTopologyState.Failed);
  assert.equal(failed.sequence, 2n);
  assert.ok(failed.observedAt.getTime() >= publicationStarted);
  assert.equal((await events.return()).done, true);
  manager.clientServerTopology = manager.fanoutTopology = () => {
    throw new Error('Native runtime has been disposed');
  };
  runtime.stopObservers();
  runtime.stopObservers();
  assert.equal(runtime.snapshot('events').sequence, 2n);
  assert.equal(runtime.snapshot('events').state, framework.ZLinkTopologyState.Failed);
  const late = runtime.observe('events')[Symbol.asyncIterator]();
  assert.equal((await late.next()).value.status.sequence, 2n);
  assert.equal((await late.return()).done, true);
});

test('ClientServer and Fanout peer reasons follow monitoring priority and ignore target weight', () => {
  const cases = [
    ['serving', framework.ZLinkPeerState.Ready, undefined, undefined],
    ['retiring', framework.ZLinkPeerState.Draining,
      framework.ZLinkTopologyReason.Draining, framework.ZLinkTopologyReason.Draining],
    ['preparing', framework.ZLinkPeerState.Connecting,
      framework.ZLinkTopologyReason.NoReadyTarget, framework.ZLinkTopologyReason.NoReadyPeer],
    ['stopped', framework.ZLinkPeerState.NotConnected,
      framework.ZLinkTopologyReason.NoReadyTarget, framework.ZLinkTopologyReason.NoReadyPeer],
    ['error', framework.ZLinkPeerState.NotConnected,
      framework.ZLinkTopologyReason.NoReadyTarget, framework.ZLinkTopologyReason.NoReadyPeer]
  ];
  for (const [state, expectedState, targetReason, publisherReason] of cases) {
    for (const weight of [0, 100]) {
      const manager = {
        clientServerTopology: () => ({
          localRole: 'client',
          descriptors: [{ serverRoutingId: 'server-a', weight, state }]
        }),
        fanoutTopology: () => ({
          descriptors: [{ publisherRoutingId: 'publisher-a', state }]
        }),
        observeClientServerTopology: () => () => {},
        observeFanoutTopology: () => () => {}
      };
      const target = new internal.ZLinkClientServerRuntimeProjection(() => manager)
        .snapshot('orders').targets[0];
      assert.equal(target.state, expectedState);
      assert.equal(target.weight, weight);
      assert.equal(target.unavailableReason, targetReason);
      const publisher = new internal.ZLinkFanoutRuntimeProjection(() => manager)
        .snapshot('events').publishers[0];
      assert.equal(publisher.state, expectedState);
      assert.equal(publisher.unavailableReason, publisherReason);
    }
  }
});

test('ClientServer and Fanout topology disable readiness during host relocation without hiding physical counts', async () => {
  let hostState = framework.ZLinkFrameworkRuntimeState.Serving;
  const manager = {
    clientServerTopology() {
      return {
        localRole: 'client',
        pendingRequestCount: 0,
        descriptors: [
          {
            serverRoutingId: 'server-a',
            weight: 100,
            state: 'serving'
          }
        ]
      };
    },
    observeClientServerTopology() {
      return () => {};
    },
    fanoutTopology() {
      return {
        descriptors: [
          {
            publisherRoutingId: 'publisher-a',
            state: 'serving'
          }
        ]
      };
    },
    observeFanoutTopology() {
      return () => {};
    }
  };
  const clientServer = new internal.ZLinkClientServerRuntimeProjection(
    () => manager,
    () => hostState
  );
  const fanout = new internal.ZLinkFanoutRuntimeProjection(
    () => manager,
    () => hostState
  );

  assert.equal(clientServer.snapshot('orders').isReady, true);
  assert.equal(fanout.snapshot('events').isReady, true);
  const clientServerEvents = clientServer.observe('orders')[Symbol.asyncIterator]();
  const fanoutEvents = fanout.observe('events')[Symbol.asyncIterator]();

  hostState = framework.ZLinkFrameworkRuntimeState.Relocating;
  clientServer.hostStateChanged();
  fanout.hostStateChanged();
  assert.equal((await clientServerEvents.next()).value.status.isReady, false);
  assert.equal((await fanoutEvents.next()).value.status.isReady, false);
  assert.equal(clientServer.snapshot('orders').isReady, false);
  assert.equal(clientServer.snapshot('orders').readyTargetCount, 1);
  assert.equal(clientServer.snapshot('orders').state, framework.ZLinkTopologyState.Stopping);
  assert.equal(fanout.snapshot('events').isReady, false);
  assert.equal(fanout.snapshot('events').readyPublisherCount, 1);

  hostState = framework.ZLinkFrameworkRuntimeState.Relocated;
  assert.equal(clientServer.snapshot('orders').state, framework.ZLinkTopologyState.Stopping);
  assert.equal(fanout.snapshot('events').state, framework.ZLinkTopologyState.Stopping);
  await clientServerEvents.return();
  await fanoutEvents.return();
});

test('Stopped host observation delivers terminal status and completes', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  await host.shutdown({ deadlineMs: 1000 });
  const events = host.observe()[Symbol.asyncIterator]();
  assert.equal(
    (await events.next()).value.status.state,
    framework.ZLinkFrameworkRuntimeState.Stopped
  );
  assert.equal((await events.next()).done, true);
});

test('Framework runtime shutdown surface emits status and Nest exports topology tokens', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  const events = host.observe()[Symbol.asyncIterator]();
  const initial = await events.next();
  assert.equal(initial.value.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
  const result = await host.shutdown({ deadlineMs: 1000 });
  const event = await events.next();

  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal(
    host.status.terminationResult.outcome,
    framework.ZLinkFrameworkTerminationOutcome.Stopped
  );
  assert.equal(event.value.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
  assert.deepEqual(event.value.loss, {
    coalescedCount: 1n,
    discardedTerminalCount: 0n
  });
  assert.equal(typeof nestjs.ZLINK_CLIENT_SERVER_RUNTIME, 'symbol');
  assert.equal(typeof nestjs.ZLINK_FANOUT_RUNTIME, 'symbol');
});

test('Manual RouteMesh without a Location Store reports ready when the host serves', async () => {
  const meshName = `manual-ready.${process.pid}`;
  const registration = internal.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder
      .addRouteMesh(meshName)
      .listen(`inproc://${meshName}`)
      .routingId(`manual-ready-node-${process.pid}`);
    mesh.channel(meshName).server();
  });
  const host = new internal.ZLinkFrameworkRuntimeHost({ registration });

  try {
    await host.start();
    const status = host.routeMeshRuntime.snapshot(meshName);
    assert.equal(status.state, framework.ZLinkTopologyState.Ready);
    assert.equal(status.isReady, true);
    assert.equal(status.channels[0].isReady, true);
  } finally {
    await host.stop();
  }
});

test(
  'RouteMesh placement status uses current local object counts for availability',
  { timeout: 1000 },
  async (t) => {
    const gate = new internal.ZLinkRuntimeAdmissionGate();
    let nodeState = internal.MeshNodeRuntimeState.Serving;
    const node = {
      status() {
        return {
          routingId: 'node-a',
          lifecycleGeneration: 1n,
          descriptorRevision: 1n,
          state: nodeState,
          lastChangedMs: 1n
        };
      },
      peers() {
        return [];
      },
      peerChannels() {
        return { names: [], weights: [] };
      }
    };
    const descriptor = {
      objectRole: framework.ZLinkObjectRole.Server,
      placementWeight: 100,
      populationCapacity: {
        actors: { active: 0, reserved: 0, limit: 2 },
        spots: { active: 0, reserved: 0, limit: 2 },
        spotTypes: []
      },
      activationConcurrency: { active: 0, limit: 8 },
      channelWeights: {},
      applicationVersion: 1n,
      objectCapabilities: []
    };
    let counts = { activeActorCount: 1, activeSpotCount: 1 };
    let hostState = framework.ZLinkFrameworkRuntimeState.Serving;
    let locationStoreHealthy = true;
    const createCoordinator = () => {
      const coordinator = new internal.ZLinkRouteMeshRuntimeCoordinator({
      meshNames: ['game'],
      meshOptions: new Map([['game', { meshChannels: {} }]]),
      meshNode: () => node,
      meshNodeDescriptor: () => descriptor,
      localPlacementCounts: () => counts,
      hostState: () => hostState,
      isLocationStoreHealthy: () => locationStoreHealthy,
      admission: gate,
      publishRetiring: async () => {},
      rollbackRetiring: async () => {},
      publishDraining: async () => {},
      publishHostDraining: async () => {},
      drainResources: async () => {},
      cleanupHostResources: async () => {},
      forceStopResources: async () => {}
      });
      coordinator.markServing();
      return coordinator;
    };

    const runtime = createCoordinator();
    for (const [nativeState, expectedTopology] of [
      [internal.MeshNodeRuntimeState.Preparing, framework.ZLinkTopologyState.Starting],
      [internal.MeshNodeRuntimeState.Serving, framework.ZLinkTopologyState.Ready],
      [internal.MeshNodeRuntimeState.Retiring, framework.ZLinkTopologyState.Stopping],
      [internal.MeshNodeRuntimeState.Draining, framework.ZLinkTopologyState.Stopping],
      [internal.MeshNodeRuntimeState.Stopped, framework.ZLinkTopologyState.Stopped],
      [internal.MeshNodeRuntimeState.Error, framework.ZLinkTopologyState.Failed]
    ]) {
      nodeState = nativeState;
      await t.test(`native descriptor state ${nativeState} preserves its lifecycle meaning`, () => {
        assert.equal(createCoordinator().snapshot('game').state, expectedTopology);
      });
    }
    nodeState = internal.MeshNodeRuntimeState.Serving;
    const serving = runtime.snapshot('game');
    assert.equal(serving.placement.activeActorCount, 1);
    assert.equal(serving.placement.activeSpotCount, 1);
    assert.equal(serving.placement.isAvailable, true);

    counts = { activeActorCount: 2, activeSpotCount: 2 };
    const exhausted = runtime.snapshot('game');
    assert.equal(exhausted.placement.activeActorCount, 2);
    assert.equal(exhausted.placement.activeSpotCount, 2);
    assert.equal(exhausted.placement.isAvailable, false);
    assert.equal(
      exhausted.placement.unavailableReason,
      framework.ZLinkTopologyReason.CapacityExceeded
    );

    counts = { activeActorCount: 1, activeSpotCount: 1 };
    assert.equal(runtime.snapshot('game').placement.isAvailable, true);
    descriptor.placementWeight = 0;
    for (const [state, expectedReason] of [
      [framework.ZLinkFrameworkRuntimeState.Draining, framework.ZLinkTopologyReason.Draining],
      [framework.ZLinkFrameworkRuntimeState.Relocating, framework.ZLinkTopologyReason.Draining],
      [framework.ZLinkFrameworkRuntimeState.Relocated, framework.ZLinkTopologyReason.Draining],
      [framework.ZLinkFrameworkRuntimeState.Preparing, framework.ZLinkTopologyReason.RuntimeNotReady],
      [framework.ZLinkFrameworkRuntimeState.Stopped, framework.ZLinkTopologyReason.RuntimeNotReady],
      [framework.ZLinkFrameworkRuntimeState.Error, framework.ZLinkTopologyReason.RuntimeNotReady],
      [framework.ZLinkFrameworkRuntimeState.Serving, framework.ZLinkTopologyReason.LocationUnavailable]
    ]) {
      hostState = state;
      locationStoreHealthy = false;
      assert.equal(createCoordinator().snapshot('game').placement.unavailableReason, expectedReason);
    }
    hostState = framework.ZLinkFrameworkRuntimeState.Serving;
    locationStoreHealthy = true;
    nodeState = internal.MeshNodeRuntimeState.Draining;
    assert.equal(runtime.snapshot('game').placement.unavailableReason,
      framework.ZLinkTopologyReason.CapacityExceeded);
    nodeState = internal.MeshNodeRuntimeState.Serving;
    const events = runtime.observe('game')[Symbol.asyncIterator]();
    try {
      const first = await events.next();
      assert.equal(first.value.status.placement.isAvailable, false);
      assert.equal(
        first.value.status.placement.unavailableReason,
        framework.ZLinkTopologyReason.CapacityExceeded
      );
      assert.deepEqual(first.value.loss, { coalescedCount: 0n, discardedTerminalCount: 0n });
    } finally {
      await events.return();
    }
  }
);

test(
  'Host, ClientServer and fanout observation starts with the current complete status',
  { timeout: 1000 },
  async () => {
    const host = new internal.ZLinkFrameworkRuntimeHost({
      registration: internal.createFrameworkRegistration()
    });
    const manager = {
      clientServerTopology: () => ({
        localRole: 'client',
        descriptors: [{ serverRoutingId: 'server-a', weight: 0, state: 'serving' }]
      }),
      observeClientServerTopology: () => () => {},
      fanoutTopology: () => ({
        descriptors: [{ publisherRoutingId: 'publisher-a', state: 'serving' }]
      }),
      observeFanoutTopology: () => () => {}
    };
    const clientServer = new internal.ZLinkClientServerRuntimeProjection(() => manager);
    const fanout = new internal.ZLinkFanoutRuntimeProjection(() => manager);
    const cases = [
      [host.observe(), (status) => assert.equal(status.state, host.status.state)],
      [clientServer.observe('orders'), (status) => assert.equal(status.targets[0].weight, 0)],
      [fanout.observe('events'), (status) => assert.equal(status.readyPublisherCount, 1)]
    ];
    for (const [observations, verify] of cases) {
      const events = observations[Symbol.asyncIterator]();
      try {
        const first = await events.next();
        verify(first.value.status);
        assert.deepEqual(first.value.loss, { coalescedCount: 0n, discardedTerminalCount: 0n });
      } finally {
        await events.return();
      }
    }
  }
);

test('Framework shutdown disposes the registered Location Store after runtime cleanup', async () => {
  let disposed = 0;
  const store = {
    async read() {
      return { kind: 'missing', storeNow: new Date(0) };
    },
    async write() {
      return { kind: 'conflict', storeNow: new Date(0) };
    },
    async scan() {
      return { kind: 'expired' };
    },
    dispose() {
      disposed += 1;
    }
  };
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({
      locations: { storeInstance: store }
    })
  });
  host.executionState = {
    abortController: new AbortController(),
    listenerTasks: [],
    async dispose() {}
  };

  const result = await host.shutdown({ deadlineMs: 1000 });

  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal(disposed, 1);
});

test('Shutdown seals active RouteMesh ClientServer and Fanout observers with terminal status', async () => {
  let nativeSnapshotsAvailable = true;
  const manager = {
    clientServerTopology() {
      if (!nativeSnapshotsAvailable) {
        throw new Error('native runtime unavailable');
      }
      return {
        localRole: 'client',
        pendingRequestCount: 0,
        descriptors: [{ serverRoutingId: 'server-a', weight: 100, state: 'serving' }]
      };
    },
    observeClientServerTopology() {
      return () => {};
    },
    fanoutTopology() {
      if (!nativeSnapshotsAvailable) {
        throw new Error('native runtime unavailable');
      }
      return {
        descriptors: [{ publisherRoutingId: 'publisher-a', state: 'serving' }]
      };
    },
    observeFanoutTopology() {
      return () => {};
    }
  };
  const clientServer = new internal.ZLinkClientServerRuntimeProjection(() => manager);
  const fanout = new internal.ZLinkFanoutRuntimeProjection(() => manager);
  const routeMesh = new internal.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game'],
    meshOptions: new Map([['game', { meshChannels: {} }]]),
    meshNode: () =>
      nativeSnapshotsAvailable
        ? {
            status: () => ({
              routingId: 'node-a',
              lifecycleGeneration: 1n,
              descriptorRevision: 1n,
              state: internal.MeshNodeRuntimeState.Serving,
              lastChangedMs: 1n
            }),
            peers: () => [],
            peerChannels: () => ({ names: [], weights: [] })
          }
        : undefined,
    hostState: () => host.runtimeState,
    admission: new internal.ZLinkRuntimeAdmissionGate(),
    publishRetiring: async () => {},
    rollbackRetiring: async () => {},
    publishDraining: async () => {},
    publishHostDraining: async () => {},
    drainResources: async () => {},
    cleanupHostResources: async () => {},
    forceStopResources: async () => {}
  });
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  routeMesh.markServing();
  host.channelRuntime = manager;
  host.routeMeshCoordinator = routeMesh;
  host.clientServerRuntime = clientServer;
  host.fanoutRuntime = fanout;
  host.stop = async () => {
    host.stopTopologyObservers();
    host.executionState = undefined;
  };

  const routeEvents = routeMesh.observe('game')[Symbol.asyncIterator]();
  const clientEvents = clientServer.observe('orders')[Symbol.asyncIterator]();
  const fanoutEvents = fanout.observe('events')[Symbol.asyncIterator]();
  assert.equal(routeMesh.snapshot('game').state, framework.ZLinkTopologyState.Ready);
  nativeSnapshotsAvailable = false;
  const result = await host.shutdown({ deadlineMs: 1000 });

  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  for (const [name, events] of [['RouteMesh', routeEvents], ['ClientServer', clientEvents], ['Fanout', fanoutEvents]]) {
    const terminal = await events.next();
    assert.equal(terminal.done, false, name);
    assert.equal(terminal.value.status.state, framework.ZLinkTopologyState.Stopped);
    assert.equal(terminal.value.status.isReady, false);
    let settled = false;
    const pending = events.next().then((value) => {
      settled = true;
      return value;
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(settled, false);
    assert.equal((await events.return()).done, true);
    assert.equal((await pending).done, true);
  }
});

test('Topology observer callback failures do not change host lifecycle transitions', () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.routeMeshCoordinator = {
    hostStateChanged() {
      throw new Error('route observer failed');
    }
  };
  host.clientServerRuntime = {
    hostStateChanged() {
      throw new Error('client observer failed');
    }
  };
  host.fanoutRuntime = {
    hostStateChanged() {
      throw new Error('fanout observer failed');
    }
  };

  assert.doesNotThrow(() => host.setRuntimeState(framework.ZLinkFrameworkRuntimeState.Draining));
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Draining);
});

test('Relocate rejects local manual topology before changing host state and Shutdown remains available', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: { client: { manualConnections: ['tcp://127.0.0.1:19001'] } }
    }
  });
  const host = new internal.ZLinkFrameworkRuntimeHost({ registration });

  // The focused contract test enters the observable Serving state without
  // starting transport resources; the blocker must run before touching them.
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  assert.deepEqual(
    await host.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
    }),
    {
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      effectiveTargetApplicationVersion: 0n,
      outcome: framework.ZLinkFrameworkRelocationOutcome.Blocked,
      reason: framework.ZLinkFrameworkRelocationReason.ManualTopologyUnsupported
    }
  );
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
  assert.equal(host.status.acceptingWork, true);
  assert.equal(host.status.relocationResult, undefined);

  const shutdown = await host.shutdown({ deadlineMs: 1000 });
  assert.equal(shutdown.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
});

test('Concurrent relocation with a different deadline joins the running operation', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;

  const first = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    deadlineMs: 150
  });
  // Same mode and effective target application version, different deadline: the
  // concurrent call joins the running operation and shares its terminal result,
  // rather than being rejected with OperationInProgress. The join key excludes
  // the deadline.
  const second = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    deadlineMs: 300
  });
  const [firstResult, secondResult] = await Promise.all([first, second]);
  assert.deepEqual(secondResult, firstResult);
  assert.notEqual(
    secondResult.reason,
    framework.ZLinkFrameworkRelocationReason.OperationInProgress
  );
});

test('Rejected concurrent relocation reports the requested target version', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;

  const running = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    deadlineMs: 300
  });
  // Different mode and effective target: rejected with OperationInProgress, and
  // the result reflects the valid option the rejected call requested (rolling
  // update to version 7), not the running operation's target.
  const rejected = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.RollingUpdate,
    targetApplicationVersion: 7n,
    deadlineMs: 300
  });
  assert.equal(rejected.outcome, framework.ZLinkFrameworkRelocationOutcome.Blocked);
  assert.equal(rejected.reason, framework.ZLinkFrameworkRelocationReason.OperationInProgress);
  assert.equal(rejected.mode, framework.ZLinkFrameworkRelocationMode.RollingUpdate);
  assert.equal(rejected.effectiveTargetApplicationVersion, 7n);
  await running;
});

test('Serving readiness stays public while a sealed admission gate stops new work', () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.admission.register('mesh-a');
  assert.equal(host.status.isReady, true);
  assert.equal(host.status.acceptingWork, true);

  host.admission.seal('mesh-a');
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
  assert.equal(host.status.isReady, true);
  assert.equal(host.status.acceptingWork, false);
});

test('Relocate keeps Serving when descriptor publication is reversibly rolled back', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      return 'store_unavailable';
    }
  };

  assert.deepEqual(
    await host.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
    }),
    {
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      effectiveTargetApplicationVersion: 0n,
      outcome: framework.ZLinkFrameworkRelocationOutcome.Blocked,
      reason: framework.ZLinkFrameworkRelocationReason.StoreUnavailable
    }
  );
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
  assert.equal(host.status.relocationResult, undefined);
});

test('Relocate reports an irreversible descriptor rollback failure without claiming success', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      throw new internal.ZLinkRetiringRollbackError();
    }
  };
  host.stop = async () => {};

  const result = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Blocked);
  assert.equal(result.reason, framework.ZLinkFrameworkRelocationReason.RelocationFailed);
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Error);
});

test('Relocate preserves an incompatible participant state as StateIncompatible', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      return 'prepared';
    },
    async relocateHost() {
      return {
        kind: 'forceStopped',
        reason: 'teardown_failed',
        error: new internal.ZLinkRelocationStateIncompatibleError(
          'Relocation application state exceeds the 64 MiB participant limit.'
        )
      };
    },
    async restoreHostServing() {}
  };

  const result = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Blocked);
  assert.equal(result.reason, framework.ZLinkFrameworkRelocationReason.StateIncompatible);
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
});

test('Relocate spends one absolute deadline across preflight publication and resource movement', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  let preflightDeadlineAt;
  let publicationBudget;
  let movementBudget;
  host.preflightAutomaticPeerReadiness = async (deadlineAtMs) => {
    preflightDeadlineAt = deadlineAtMs;
    await new Promise((resolve) => setTimeout(resolve, 15));
    return undefined;
  };
  host.routeMeshCoordinator = {
    async prepareHostRetire(deadlineMs) {
      publicationBudget = deadlineMs;
      await new Promise((resolve) => setTimeout(resolve, 15));
      return 'prepared';
    },
    async relocateHost(deadlineMs) {
      movementBudget = deadlineMs;
      return { kind: 'drained' };
    }
  };
  host.publishHostRelocated = async () => {};

  const startedAt = performance.now();
  const result = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    deadlineMs: 200
  });

  assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
  assert.ok(preflightDeadlineAt >= startedAt + 190);
  assert.ok(publicationBudget > 0 && publicationBudget < 200);
  assert.ok(movementBudget > 0 && movementBudget < publicationBudget);
});

test('Relocation manual topology classification covers every local service registration', () => {
  const manualRegistrations = [
    {
      routeChannels: [
        {
          routerChannelId: 'route-a',
          bind: 'tcp://127.0.0.1:19101',
          manualConnections: ['tcp://127.0.0.1:19001']
        }
      ]
    },
    {
      spotNodes: {
        play: {
          router: { bind: 'tcp://127.0.0.1:19102', manualConnections: ['tcp://127.0.0.1:19002'] }
        }
      }
    },
    {
      spotNodes: {
        play: {
          router: {
            bind: 'tcp://127.0.0.1:19103',
            manualPeerConnections: [{ peerRid: 'peer-a', endpoint: 'tcp://127.0.0.1:19003' }]
          }
        }
      }
    },
    { channels: { orders: { client: { manualConnections: ['tcp://127.0.0.1:19004'] } } } },
    {
      channels: {
        events: {
          subscriber: { manualConnections: ['tcp://127.0.0.1:19005'] },
          publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
        }
      }
    },
    {
      channels: { events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:19006' } } }
    }
  ];

  for (const options of manualRegistrations) {
    const registration = internal.createFrameworkRegistration(options);
    assert.equal(internal.hasUnsupportedManualTopology(registration), true);
  }

  const automaticPublisher = internal.createFrameworkRegistration({
    locations: { useInMemoryStores: true },
    channels: { events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:19007' } } }
  });
  assert.equal(internal.hasUnsupportedManualTopology(automaticPublisher), false);
});

test('Relocation requires explicit valid mode and rolling update target version', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({ applicationVersion: 3n })
  });
  assert.throws(() => host.relocate({}), /mode is required/);
  assert.throws(
    () =>
      host.relocate({
        mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
        targetApplicationVersion: 4n
      }),
    /cannot define targetApplicationVersion/
  );
  assert.throws(
    () =>
      host.relocate({
        mode: framework.ZLinkFrameworkRelocationMode.RollingUpdate,
        targetApplicationVersion: 3n
      }),
    /greater than the source version/
  );
});

test('Successful relocation leaves infrastructure started until explicit shutdown', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({ applicationVersion: 3n })
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      return 'prepared';
    },
    async relocateHost() {
      return { kind: 'drained' };
    }
  };

  const result = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.RollingUpdate,
    targetApplicationVersion: 4n
  });
  assert.deepEqual(result, {
    mode: framework.ZLinkFrameworkRelocationMode.RollingUpdate,
    effectiveTargetApplicationVersion: 4n,
    outcome: framework.ZLinkFrameworkRelocationOutcome.Relocated,
    reason: framework.ZLinkFrameworkRelocationReason.None
  });
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Relocated);
  assert.equal(host.isStarted, true);
});

test('Shutdown stops new relocation units and waits only for admitted work to commit', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  let relocationStarted;
  let releaseAdmitted;
  const started = new Promise((resolve) => {
    relocationStarted = resolve;
  });
  const admitted = new Promise((resolve) => {
    releaseAdmitted = resolve;
  });
  const events = [];
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      return 'prepared';
    },
    async relocateHost(_deadlineMs, stopStartingSignal) {
      events.push('relocation:start');
      relocationStarted();
      await new Promise((resolve) =>
        stopStartingSignal.addEventListener('abort', resolve, { once: true })
      );
      events.push('relocation:shutdown-observed');
      await admitted;
      events.push('relocation:admitted-committed');
      return { kind: 'forceStopped', reason: 'teardown_failed' };
    },
    async shutdownHost() {
      events.push('shutdown:drain');
      return { kind: 'drained' };
    }
  };
  host.stop = async () => {
    events.push('shutdown:stop');
  };

  const relocation = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  await started;
  const shutdown = host.shutdown({ deadlineMs: 1_000 });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Draining);
  assert.equal(host.status.acceptingWork, false);
  assert.deepEqual(events, ['relocation:start', 'relocation:shutdown-observed']);

  releaseAdmitted();
  assert.deepEqual(await relocation, {
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    effectiveTargetApplicationVersion: 0n,
    outcome: framework.ZLinkFrameworkRelocationOutcome.Blocked,
    reason: framework.ZLinkFrameworkRelocationReason.ShutdownRequested
  });
  assert.equal((await shutdown).outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.deepEqual(events, [
    'relocation:start',
    'relocation:shutdown-observed',
    'relocation:admitted-committed',
    'shutdown:drain',
    'shutdown:stop'
  ]);
});

test('concurrent Relocate shares identical options and rejects a different operation', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({ applicationVersion: 3n })
  });
  host.executionState = { abortController: new AbortController() };
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  let release;
  let prepares = 0;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      prepares++;
      await new Promise((resolve) => {
        release = resolve;
      });
      return 'store_unavailable';
    }
  };

  const first = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  await new Promise((resolve) => setImmediate(resolve));
  const same = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  const different = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.RollingUpdate,
    targetApplicationVersion: 4n
  });
  assert.equal(different.reason, framework.ZLinkFrameworkRelocationReason.OperationInProgress);
  assert.equal(prepares, 1);
  release();
  assert.deepEqual(await same, await first);
});

test('application shutdown hook tears down without implicitly relocating', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  let stopped = 0;
  host.routeMeshCoordinator = {
    async drainHost() {
      throw new Error('shutdown hook must not relocate');
    }
  };
  host.stop = async () => {
    stopped++;
  };

  await host.onApplicationShutdown();
  assert.equal(stopped, 1);
});

test('Shared shutdown keeps the first absolute deadline', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  let finishShutdown;
  let observedDeadline;
  host.routeMeshCoordinator.shutdownHost = async (deadline) => {
    observedDeadline = deadline;
    await new Promise((resolve) => {
      finishShutdown = resolve;
    });
    return { kind: 'drained' };
  };
  const first = host.shutdown({ deadlineMs: 50 });
  const joined = host.shutdown({ deadlineMs: 30_000 });
  assert.equal(host.runtimeDeadline, observedDeadline);
  finishShutdown();
  assert.equal(await first, await joined);
});

test('Host Draining publishes weights only for locally registered server channels', async () => {
  const meshName = `shutdown-channels.${process.pid}`;
  const registration = internal.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder
      .addRouteMesh(meshName)
      .listen(`inproc://${meshName}`)
      .routingId(`shutdown-channel-node-${process.pid}`);
    mesh.channel('server-channel').server();
    mesh.channel('client-channel').client();
  });
  const host = new internal.ZLinkFrameworkRuntimeHost({ registration });
  await host.start();
  const result = await host.shutdown({ deadlineMs: 1000 });
  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal(result.reason, framework.ZLinkFrameworkTerminationReason.None);
});

test('Shutdown deadline includes final owned resource cleanup', async (t) => {
  let nowMs = 0;
  t.mock.method(performance, 'now', () => nowMs);
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.stop = async () => {
    nowMs += 51;
  };
  const result = await host.shutdown({ deadlineMs: 50 });
  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
  assert.equal(result.reason, framework.ZLinkFrameworkTerminationReason.DeadlineExceeded);
});

test('Host Observe retains current status before lifecycle changes', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  const expected = host.status;
  const events = host.observe()[Symbol.asyncIterator]();
  try {
    const initial = await Promise.race([events.next(), Promise.resolve(undefined)]);
    assert.notEqual(initial, undefined, 'Host Observe must retain current status before lifecycle changes');
    assert.equal(initial.value.status.state, expected.state);
    assert.equal(initial.value.status.sequence, expected.sequence);
    assert.equal(initial.value.status.isReady, expected.isReady);
  } finally {
    await events.return();
  }
});

test('Host Observe coalesces an unread initial status with lifecycle changes', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  const events = host.observe()[Symbol.asyncIterator]();
  try {
    await host.shutdown({ deadlineMs: 1000 });
    const terminal = await events.next();
    assert.equal(terminal.value.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
    assert.deepEqual(terminal.value.loss, {
      coalescedCount: 2n,
      discardedTerminalCount: 0n
    });
  } finally {
    await events.return();
  }
});

test('RouteMesh Observe retains current status before topology changes', async () => {
  const runtime = new internal.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game'],
    meshOptions: new Map([['game', { meshChannels: {} }]]),
    meshNode: () => ({
      status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n,
        descriptorRevision: 1n, state: 3, lastChangedMs: 1n }),
      peers: () => [],
      peerChannels: () => ({ names: [], weights: [] })
    }),
    admission: new internal.ZLinkRuntimeAdmissionGate(),
    publishRetiring: async () => {},
    rollbackRetiring: async () => {},
    publishDraining: async () => {},
    publishHostDraining: async () => {},
    drainResources: async () => {},
    cleanupHostResources: async () => {},
    forceStopResources: async () => {}
  });
  runtime.markServing();
  const expected = runtime.snapshot('game');
  const events = runtime.observe('game')[Symbol.asyncIterator]();
  try {
    const initial = await Promise.race([events.next(), Promise.resolve(undefined)]);
    assert.notEqual(initial, undefined, 'RouteMesh Observe must retain current status before topology changes');
    assert.equal(initial.value.status.meshName, 'game');
    assert.equal(initial.value.status.sequence, expected.sequence);
    assert.equal(initial.value.status.state, expected.state);
  } finally {
    await events.return();
  }
});

for (const kind of ['ClientServer', 'Fanout']) test(`${kind} query publishes changed payload without observers`, async () => {
  let targetState = 'serving';
  let changed;
  const manager = {
    clientServerTopology: () => ({ localRole: 'client', descriptors: [{ serverRoutingId: 'server', weight: 1, state: targetState }] }),
    fanoutTopology: () => ({ descriptors: [{ publisherRoutingId: 'publisher', state: targetState }] }),
    observeClientServerTopology: (_name, callback) => { changed = callback; return () => {}; },
    observeFanoutTopology: (_name, callback) => { changed = callback; return () => {}; }
  };
  const runtime = new internal[`ZLink${kind}RuntimeProjection`](() => manager);
  const initial = runtime.snapshot('events');
  assert.equal(initial.sequence, 1n);
  assert.equal(runtime.snapshot('events').sequence, 1n);
  targetState = 'retiring';
  assert.equal(runtime.snapshot('events').sequence, 2n);
  assert.equal(runtime.snapshot('events').sequence, 2n);
  targetState = 'stopped';
  changed();
  targetState = 'preparing';
  assert.equal(runtime.snapshot('events').sequence, 4n);
  const events = runtime.observe('events')[Symbol.asyncIterator]();
  assert.equal((await events.next()).value.status.sequence, 4n);
  await events.return();
  targetState = 'serving';
  changed();
  assert.equal(runtime.snapshot('events').sequence, 5n);
  runtime.stopObservers();
  const terminal = runtime.snapshot('events');
  manager.clientServerTopology = manager.fanoutTopology = () => { throw new Error('disposed'); };
  assert.equal(runtime.snapshot('events'), terminal);
  runtime.stopObservers();
  assert.equal(runtime.snapshot('events'), terminal);
});
for (const kind of ['ClientServer', 'Fanout']) test(`${kind} failed first read does not retain an unpublished source`, () => {
  let failRead = true;
  let readCount = 0;
  const read = () => {
    readCount += 1;
    if (failRead) throw new Error('invalid channel or unavailable native runtime');
    return kind === 'ClientServer' ? { localRole: 'client', descriptors: [] } : { descriptors: [] };
  };
  const manager = {
    clientServerTopology: read, fanoutTopology: read,
    observeClientServerTopology: () => () => {}, observeFanoutTopology: () => () => {}
  };
  const runtime = new internal[`ZLink${kind}RuntimeProjection`](() => manager);
  assert.throws(() => runtime.snapshot('events'), /invalid channel/);
  assert.throws(() => runtime.snapshot('events'), /invalid channel/);
  failRead = false;
  runtime.hostStateChanged();
  assert.equal(readCount, 2);
  runtime.stopObservers();
  const first = runtime.snapshot('events');
  assert.equal(first.sequence, 1n);
  assert.notEqual(first.state, framework.ZLinkTopologyState.Stopped);
});

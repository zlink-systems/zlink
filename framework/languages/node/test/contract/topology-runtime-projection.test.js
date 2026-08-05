const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

test('ClientServer runtime projects minimal status and emits complete status changes', async () => {
  let changed;
  const manager = {
    clientServerTopology() {
      return {
        localRole: 'clientAndServer',
        pendingRequestCount: 2,
        descriptors: [{
          channelName: 'orders',
          serverRoutingId: 'server-a',
          lifecycleGeneration: 3n,
          descriptorRevision: 5n,
          weight: 100,
          state: 'serving',
          securityIdentity: 'default',
          effectiveMaxMessageBytes: 1024,
          advertisedEndpoint: 'tcp://127.0.0.1:10000'
        }]
      };
    },
    observeClientServerTopology(_channelName, callback) {
      changed = callback;
      return () => { changed = undefined; };
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
  changed();
  const status = await events.next();
  assert.equal(status.value.status.channelName, 'orders');
  assert.equal(status.value.status.isReady, true);
  assert.deepEqual(status.value.loss, { coalescedCount: 0n, discardedTerminalCount: 0n });
  await events.return();
  assert.equal(changed, undefined);
});

test('Fanout runtime projects minimal publisher status and emits complete status changes', async () => {
  let changed;
  const manager = {
    fanoutTopology() {
      return {
        descriptors: [{
          channelName: 'events',
          publisherRoutingId: 'publisher-a',
          lifecycleGeneration: 7n,
          descriptorRevision: 9n,
          advertisedEndpoint: 'tcp://127.0.0.1:10001',
          state: 'serving'
        }]
      };
    },
    observeFanoutTopology(_channelName, callback) {
      changed = callback;
      return () => { changed = undefined; };
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
  changed();
  const status = await events.next();
  assert.equal(status.value.status.channelName, 'events');
  assert.equal(status.value.status.publishers[0].nodeRid, 'publisher-a');
  await events.return();
});

test('ClientServer and Fanout topology disable readiness during host relocation without hiding physical counts', async () => {
  let hostState = framework.ZLinkFrameworkRuntimeState.Serving;
  const manager = {
    clientServerTopology() {
      return {
        localRole: 'client',
        pendingRequestCount: 0,
        descriptors: [{
          serverRoutingId: 'server-a',
          weight: 100,
          state: 'serving'
        }]
      };
    },
    observeClientServerTopology() { return () => {}; },
    fanoutTopology() {
      return {
        descriptors: [{
          publisherRoutingId: 'publisher-a',
          state: 'serving'
        }]
      };
    },
    observeFanoutTopology() { return () => {}; }
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

test('Framework runtime shutdown surface emits status and Nest exports topology tokens', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  const events = host.observe()[Symbol.asyncIterator]();
  const result = await host.shutdown({ deadlineMs: 1000 });
  const event = await events.next();

  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  assert.equal(
    host.status.terminationResult.outcome,
    framework.ZLinkFrameworkTerminationOutcome.Stopped
  );
  assert.equal(event.value.status.state, framework.ZLinkFrameworkRuntimeState.Draining);
  assert.equal(typeof nestjs.ZLINK_CLIENT_SERVER_RUNTIME, 'symbol');
  assert.equal(typeof nestjs.ZLINK_FANOUT_RUNTIME, 'symbol');
});

test('Manual RouteMesh without a Location Store reports ready when the host serves', async () => {
  const meshName = `manual-ready.${process.pid}`;
  const registration = internal.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder.addRouteMesh(meshName)
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

test('RouteMesh placement status uses current local object counts for availability', () => {
  const gate = new internal.ZLinkRuntimeAdmissionGate();
  const node = {
    status() {
      return {
        routingId: 'node-a',
        lifecycleGeneration: 1n,
        descriptorRevision: 1n,
        state: 3,
        lastChangedMs: 1n
      };
    },
    peers() { return []; },
    peerChannels() { return { names: [], weights: [] }; }
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
  const runtime = new internal.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game'],
    meshOptions: new Map([['game', { meshChannels: {} }]]),
    meshNode: () => node,
    meshNodeDescriptor: () => descriptor,
    localPlacementCounts: () => counts,
    admission: gate,
    publishRetiring: async () => {},
    rollbackRetiring: async () => {},
    publishDraining: async () => {},
    publishHostDraining: async () => {},
    drainResources: async () => {},
    cleanupHostResources: async () => {},
    forceStopResources: async () => {}
  });

  runtime.markServing();
  const serving = runtime.snapshot('game');
  assert.equal(serving.placement.activeActorCount, 1);
  assert.equal(serving.placement.activeSpotCount, 1);
  assert.equal(serving.placement.isAvailable, true);

  counts = { activeActorCount: 2, activeSpotCount: 2 };
  const exhausted = runtime.snapshot('game');
  assert.equal(exhausted.placement.activeActorCount, 2);
  assert.equal(exhausted.placement.activeSpotCount, 2);
  assert.equal(exhausted.placement.isAvailable, false);
  assert.equal(exhausted.placement.unavailableReason, framework.ZLinkTopologyReason.CapacityExceeded);
});

test('Framework shutdown disposes the registered Location Store after runtime cleanup', async () => {
  let disposed = 0;
  const store = {
    async read() { return { kind: 'missing', storeNow: new Date(0) }; },
    async write() { return { kind: 'conflict', storeNow: new Date(0) }; },
    async scan() { return { kind: 'expired' }; },
    dispose() { disposed += 1; }
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
    observeClientServerTopology() { return () => {}; },
    fanoutTopology() {
      if (!nativeSnapshotsAvailable) {
        throw new Error('native runtime unavailable');
      }
      return {
        descriptors: [{ publisherRoutingId: 'publisher-a', state: 'serving' }]
      };
    },
    observeFanoutTopology() { return () => {}; }
  };
  const clientServer = new internal.ZLinkClientServerRuntimeProjection(() => manager);
  const fanout = new internal.ZLinkFanoutRuntimeProjection(() => manager);
  const routeMesh = new internal.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game'],
    meshOptions: new Map([['game', { meshChannels: {} }]]),
    meshNode: () => nativeSnapshotsAvailable ? ({
      status: () => ({
        routingId: 'node-a',
        lifecycleGeneration: 1n,
        descriptorRevision: 1n,
        state: 3,
        lastChangedMs: 1n
      }),
      peers: () => [],
      peerChannels: () => ({ names: [], weights: [] })
    }) : undefined,
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
  routeMesh.markServing();
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
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
  nativeSnapshotsAvailable = false;
  const result = await host.shutdown({ deadlineMs: 1000 });

  assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
  for (const events of [routeEvents, clientEvents, fanoutEvents]) {
    const terminal = await events.next();
    assert.equal(terminal.done, false);
    assert.equal(terminal.value.status.state, framework.ZLinkTopologyState.Stopped);
    assert.equal(terminal.value.status.isReady, false);
    let settled = false;
    const pending = events.next().then(value => {
      settled = true;
      return value;
    });
    await new Promise(resolve => setImmediate(resolve));
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
    hostStateChanged() { throw new Error('route observer failed'); }
  };
  host.clientServerRuntime = {
    hostStateChanged() { throw new Error('client observer failed'); }
  };
  host.fanoutRuntime = {
    hostStateChanged() { throw new Error('fanout observer failed'); }
  };

  assert.doesNotThrow(() =>
    host.setRuntimeState(framework.ZLinkFrameworkRuntimeState.Draining));
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
  assert.deepEqual(await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  }), {
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    effectiveTargetApplicationVersion: 0n,
    outcome: framework.ZLinkFrameworkRelocationOutcome.Blocked,
    reason: framework.ZLinkFrameworkRelocationReason.ManualTopologyUnsupported
  });
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
  assert.equal(host.status.acceptingWork, true);
  assert.equal(host.status.relocationResult, undefined);

  const shutdown = await host.shutdown({ deadlineMs: 1000 });
  assert.equal(shutdown.outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
});

test('Relocate keeps Serving when descriptor publication is reversibly rolled back', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() { return 'store_unavailable'; }
  };

  assert.deepEqual(await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  }), {
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
    effectiveTargetApplicationVersion: 0n,
    outcome: framework.ZLinkFrameworkRelocationOutcome.Blocked,
    reason: framework.ZLinkFrameworkRelocationReason.StoreUnavailable
  });
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
  assert.equal(host.status.relocationResult, undefined);
});

test('Relocate reports an irreversible descriptor rollback failure without claiming success', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() { throw new internal.ZLinkRetiringRollbackError(); }
  };
  host.stop = async () => {};

  const result = await host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Blocked);
  assert.equal(result.reason, framework.ZLinkFrameworkRelocationReason.RelocationFailed);
  assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Error);
});

test('Relocate spends one absolute deadline across preflight publication and resource movement', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration()
  });
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  let preflightDeadlineAt;
  let publicationBudget;
  let movementBudget;
  host.preflightAutomaticPeerReadiness = async (deadlineAtMs) => {
    preflightDeadlineAt = deadlineAtMs;
    await new Promise(resolve => setTimeout(resolve, 15));
    return undefined;
  };
  host.routeMeshCoordinator = {
    async prepareHostRetire(deadlineMs) {
      publicationBudget = deadlineMs;
      await new Promise(resolve => setTimeout(resolve, 15));
      return 'prepared';
    },
    async relocateHost(deadlineMs) {
      movementBudget = deadlineMs;
      return { kind: 'drained' };
    }
  };
  host.publishHostRelocated = async () => {};

  const startedAt = Date.now();
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
    { routeChannels: [{ routerChannelId: 'route-a', bind: 'tcp://127.0.0.1:19101', manualConnections: ['tcp://127.0.0.1:19001'] }] },
    { spotNodes: { play: { router: { bind: 'tcp://127.0.0.1:19102', manualConnections: ['tcp://127.0.0.1:19002'] } } } },
    { spotNodes: { play: { router: { bind: 'tcp://127.0.0.1:19103', manualPeerConnections: [{ peerRid: 'peer-a', endpoint: 'tcp://127.0.0.1:19003' }] } } } },
    { channels: { orders: { client: { manualConnections: ['tcp://127.0.0.1:19004'] } } } },
    { channels: { events: {
      subscriber: { manualConnections: ['tcp://127.0.0.1:19005'] },
      publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
    } } },
    { channels: { events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:19006' } } } }
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
  assert.throws(
    () => host.relocate({}),
    /mode is required/
  );
  assert.throws(
    () => host.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      targetApplicationVersion: 4n
    }),
    /cannot define targetApplicationVersion/
  );
  assert.throws(
    () => host.relocate({
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
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  host.routeMeshCoordinator = {
    async prepareHostRetire() { return 'prepared'; },
    async relocateHost() { return { kind: 'drained' }; }
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

test('concurrent Relocate shares identical options and rejects a different operation', async () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({ applicationVersion: 3n })
  });
  host.executionState = {};
  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Serving;
  let release;
  let prepares = 0;
  host.routeMeshCoordinator = {
    async prepareHostRetire() {
      prepares++;
      await new Promise(resolve => { release = resolve; });
      return 'store_unavailable';
    }
  };

  const first = host.relocate({
    mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance
  });
  await new Promise(resolve => setImmediate(resolve));
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
  host.stop = async () => { stopped++; };

  await host.onApplicationShutdown();
  assert.equal(stopped, 1);
});

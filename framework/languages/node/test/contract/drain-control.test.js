'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

test('deployment identity builder publishes validated host-wide version and maintenance wave', () => {
  const registration = framework.createFrameworkRegistrationWithBuilder((options) => {
    options.setApplicationVersion(42n).setMaintenanceWave('blue');
  });
  assert.equal(registration.applicationVersion, 42n);
  assert.equal(registration.maintenanceWave, 'blue');
  assert.equal(framework.createFrameworkRegistration().applicationVersion, 0n);
  assert.throws(
    () => framework.createFrameworkRegistration({ applicationVersion: -1n }),
    /signed 64-bit non-negative range/
  );
  assert.throws(
    () => framework.createFrameworkRegistration({ applicationVersion: 1n << 63n }),
    /signed 64-bit non-negative range/
  );
  assert.throws(
    () => framework.createFrameworkRegistration({ maintenanceWave: '' }),
    /1\.\.255 byte UTF-8/
  );
  assert.throws(
    () => framework.createFrameworkRegistration({ maintenanceWave: 'x'.repeat(256) }),
    /1\.\.255 byte UTF-8/
  );
  assert.throws(
    () => framework.createFrameworkRegistration({ maintenanceWave: 'blue\0wave' }),
    /without NUL/
  );
});

test('Message Follow duration defaults to the common 30 second contract', () => {
  assert.equal(framework.createFrameworkRegistration().messageFollowDurationMs, 30_000);
});

test('fixed drain seals admission, publishes draining, waits accepted work, then drains resources', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const order = [];
  const release = deferred();
  const runtime = createRuntime(gate, {
    async publishDraining() { order.push('published'); },
    async drainResources() { order.push('resources'); }
  });
  runtime.markServing();
  const accepted = gate.run('game', 'accepted request', async () => {
    order.push('handler');
    await release.promise;
    order.push('reply_closed');
  });

  const draining = runtime.drain('game', 1000);
  await tick();
  assert.deepEqual(order, ['handler', 'published']);
  assert.equal(runtime.isReady('game'), false);
  assert.throws(
    () => gate.claim('game', 'late request'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected
  );

  release.resolve();
  await accepted;
  assert.deepEqual(await draining, { kind: 'drained' });
  assert.deepEqual(order, ['handler', 'published', 'reply_closed', 'resources']);
});

test('drain and awaitDrained share one mesh-keyed operation', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let calls = 0;
  const runtime = createRuntime(gate, {
    async drainResources() { calls += 1; }
  });
  runtime.markServing();
  const waiting = runtime.awaitDrained('game');
  const first = runtime.drain('game');
  const second = runtime.drain('game', 1);
  assert.deepEqual(await first, { kind: 'drained' });
  assert.deepEqual(await second, { kind: 'drained' });
  assert.deepEqual(await waiting, { kind: 'drained' });
  assert.equal(calls, 1);
});

test('deadline uses the closed snake_case force reason and terminal event exactly once', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const runtime = createRuntime(gate, {
    async drainResources(_meshName, signal) {
      await new Promise((_, reject) => signal.addEventListener('abort', () => reject(signal.reason), { once: true }));
    }
  });
  runtime.markServing();
  const observed = runtime.observe('game', 4)[Symbol.asyncIterator]();
  assert.deepEqual(await runtime.drain('game', 10), {
    kind: 'forceStopped',
    reason: 'deadline_exceeded'
  });
  const first = await observed.next();
  const second = await observed.next();
  assert.equal(first.done, false);
  assert.equal(second.done, false);
  await observed.return();
  const events = [first.value.status, second.value.status];
  assert.deepEqual(events.map((event) => event.state), [
    framework.ZLinkTopologyState.Stopping,
    framework.ZLinkTopologyState.Failed
  ]);
  assert.equal(events.filter((event) => event.state === framework.ZLinkTopologyState.Failed).length, 1);
});

test('drain classifies publish, owner cleanup, and teardown failures with closed snake_case reasons', async () => {
  const cases = [
    ['ZLinkDrainingStatePublishError', 'drain_state_publish_failed', 'publishDraining'],
    ['ZLinkOwnerCleanupError', 'owner_cleanup_failed', 'drainResources'],
    ['Error', 'teardown_failed', 'drainResources']
  ];
  for (const [errorName, reason, phase] of cases) {
    const gate = new framework.ZLinkRuntimeAdmissionGate();
    const failure = new Error(reason);
    failure.name = errorName;
    const runtime = createRuntime(gate, {
      async publishDraining() {
        if (phase === 'publishDraining') throw failure;
      },
      async drainResources() {
        if (phase === 'drainResources') throw failure;
      }
    });
    assert.deepEqual(await runtime.drain('game'), { kind: 'forceStopped', reason });
  }
});

test('stale or unknown mesh handles fail with a typed route error and do not create state', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const runtime = createRuntime(gate);
  assert.throws(
    () => runtime.snapshot('missing'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.throws(
    () => runtime.isReady('missing'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
});

test('RouteMesh snapshot projects typed population and activation capacity from the current descriptor', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const runtime = createRuntime(gate, {
    meshNodeDescriptor: () => ({
      objectRole: framework.ZLinkObjectRole.Server,
      placementWeight: 275,
      populationCapacity: {
        actors: { active: 7, reserved: 2, limit: 100 },
        spots: { active: 3, reserved: 1, limit: 20 },
        spotTypes: [{
          objectKind: 'user_spot',
          stableType: 'room',
          active: 2,
          reserved: 1,
          limit: 10
        }]
      },
      activationConcurrency: { active: 4, limit: 64 },
      applicationVersion: 9n,
      objectCapabilities: [{
        objectKind: 'user_spot',
        stableType: 'room',
        policy: 'snapshot',
        hasSnapshotAdapter: true,
        limit: 10
      }]
    })
  });
  runtime.markServing();

  const snapshot = runtime.snapshot('game');
  assert.equal(snapshot.meshName, 'game');
  assert.equal(snapshot.state, framework.ZLinkTopologyState.Ready);
  assert.equal(snapshot.isReady, true);
  assert.equal(snapshot.placement.isAvailable, true);
  assert.equal(snapshot.placement.activeActorCount, 7);
  assert.equal(snapshot.placement.activeSpotCount, 3);
});

test('RouteMesh observer reports a complete status after placement capacity changes', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let descriptor = {
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 1, reserved: 0, limit: 100 },
      spots: { active: 1, reserved: 0, limit: 20 },
      spotTypes: []
    },
    activationConcurrency: { active: 1, limit: 64 },
    channelWeights: {},
    applicationVersion: 1n,
    objectCapabilities: []
  };
  const runtime = createRuntime(gate, {
    meshNodeDescriptor: () => descriptor
  });
  runtime.markServing();
  const events = runtime.observe('game', 4)[Symbol.asyncIterator]();

  descriptor = {
    ...descriptor,
    populationCapacity: {
      actors: { active: 2, reserved: 1, limit: 100 },
      spots: { active: 1, reserved: 0, limit: 20 },
      spotTypes: [{
        objectKind: 'user_spot',
        stableType: 'room',
        active: 1,
        reserved: 0,
        limit: 10
      }]
    },
    activationConcurrency: { active: 2, limit: 64 }
  };

  const observed = await Promise.race([
    events.next(),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error('placement change event was not observed')),
      1000
    ))
  ]);
  await events.return();

  assert.equal(observed.done, false);
  assert.equal(observed.value.status.meshName, 'game');
  assert.equal(observed.value.status.state, framework.ZLinkTopologyState.Ready);
  assert.equal(observed.value.status.isReady, true);
  assert.equal(observed.value.status.placement.isAvailable, true);
  assert.equal(observed.value.status.placement.activeActorCount, 2);
  assert.equal(observed.value.status.placement.activeSpotCount, 1);
  assert.ok(observed.value.status.observedAt instanceof Date);
});

test('RouteMesh snapshot keeps NotRequired distinct from NotConnected', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const node = {
    ...fakeMeshNode(),
    peers() {
      return [{
        routingId: 'client-b',
        lifecycleGeneration: 2n,
        descriptorRevision: 3n,
        endpoint: 'tcp://client-b',
        state: 6,
        lastError: 0
      }];
    }
  };
  const snapshot = createRuntime(gate, { meshNode: node }).snapshot('game');

  assert.equal(snapshot.peers.length, 1);
  assert.equal(snapshot.peers[0].state, framework.ZLinkPeerState.NotRequired);
  assert.equal(snapshot.peers[0].unavailableReason, undefined);
  assert.notEqual(
    snapshot.peers[0].state,
    framework.ZLinkPeerState.NotConnected
  );
});

test('RouteMesh readiness degrades for every required peer that is not ready', () => {
  for (const [backendPeerState, expectedPeerState] of [
    [1, framework.ZLinkPeerState.Connecting],
    [5, framework.ZLinkPeerState.NotConnected]
  ]) {
    const gate = new framework.ZLinkRuntimeAdmissionGate();
    const node = {
      ...fakeMeshNode(),
      peers() {
        return [{
          routingId: 'required-peer',
          lifecycleGeneration: 2n,
          descriptorRevision: 3n,
          endpoint: 'tcp://required-peer',
          state: backendPeerState,
          lastError: 0
        }];
      }
    };
    const runtime = createRuntime(gate, { meshNode: node });
    runtime.markServing();

    const snapshot = runtime.snapshot('game');
    assert.equal(snapshot.peers[0].state, expectedPeerState);
    assert.equal(
      snapshot.peers[0].unavailableReason,
      framework.ZLinkTopologyReason.NoReadyPeer
    );
    assert.equal(snapshot.state, framework.ZLinkTopologyState.Degraded);
    assert.equal(snapshot.isReady, false);
    assert.equal(runtime.isReady('game'), false);
  }
});

test('NotRequired peers do not degrade RouteMesh readiness', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const node = {
    ...fakeMeshNode(),
    peers() {
      return [{
        routingId: 'client-b',
        lifecycleGeneration: 2n,
        descriptorRevision: 3n,
        endpoint: 'tcp://client-b',
        state: 6,
        lastError: 0
      }];
    }
  };
  const runtime = createRuntime(gate, { meshNode: node });
  runtime.markServing();

  const snapshot = runtime.snapshot('game');
  assert.equal(snapshot.state, framework.ZLinkTopologyState.Ready);
  assert.equal(snapshot.isReady, true);
  assert.equal(runtime.isReady('game'), true);
});

test('client-only RouteMesh Channel reports remote ready targets', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const node = {
    ...fakeMeshNode(),
    peers() {
      return [{
        routingId: 'channel-server',
        lifecycleGeneration: 2n,
        descriptorRevision: 3n,
        endpoint: 'tcp://channel-server',
        state: 3,
        lastError: 0
      }];
    },
    peerChannels() {
      return { names: ['orders'], weights: [50] };
    }
  };
  const runtime = createRuntime(gate, {
    meshNode: node,
    meshOptions: { meshChannels: { orders: { client: true } } }
  });
  runtime.markServing();

  assert.deepEqual(runtime.snapshot('game').channels, [{
    channelName: 'orders',
    isReady: true,
    readyTargetCount: 1
  }]);
});

test('RouteMesh status follows local channel and placement weight overrides', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let descriptor = runtimeDescriptor({
    placementWeight: 100,
    channelWeights: { orders: 100 }
  });
  const runtime = createRuntime(gate, {
    meshOptions: { meshChannels: { orders: { server: true, weight: 100 } } },
    meshNodeDescriptor: () => descriptor
  });
  runtime.markServing();

  const initial = runtime.snapshot('game');
  assert.deepEqual(initial.channels, [{
    channelName: 'orders',
    isReady: true,
    readyTargetCount: 1
  }]);
  assert.equal(initial.placement.isAvailable, true);

  const events = runtime.observe('game', 4)[Symbol.asyncIterator]();
  descriptor = {
    ...descriptor,
    placementWeight: 0,
    channelWeights: { orders: 0 }
  };
  const observed = await Promise.race([
    events.next(),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error('local weight change was not observed')),
      1000
    ))
  ]);
  await events.return();

  assert.equal(observed.done, false);
  assert.deepEqual(observed.value.status.channels, [{
    channelName: 'orders',
    isReady: false,
    readyTargetCount: 0
  }]);
  assert.equal(observed.value.status.placement.isAvailable, false);
  assert.equal(
    observed.value.status.placement.unavailableReason,
    framework.ZLinkTopologyReason.NoReadyTarget
  );
});

test('RouteMesh placement is unavailable when all population capacity is exhausted', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const descriptor = runtimeDescriptor({
    populationCapacity: {
      actors: { active: 10, reserved: 0, limit: 10 },
      spots: { active: 4, reserved: 1, limit: 5 },
      spotTypes: []
    }
  });
  const runtime = createRuntime(gate, {
    meshNodeDescriptor: () => descriptor
  });
  runtime.markServing();

  const placement = runtime.snapshot('game').placement;
  assert.equal(placement.isAvailable, false);
  assert.equal(
    placement.unavailableReason,
    framework.ZLinkTopologyReason.CapacityExceeded
  );
});

test('RouteMesh status degrades while the Location Store is unhealthy', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let storeHealthy = true;
  const runtime = createRuntime(gate, {
    meshNodeDescriptor: () => runtimeDescriptor(),
    isLocationStoreHealthy: () => storeHealthy
  });
  runtime.markServing();

  assert.equal(runtime.snapshot('game').state, framework.ZLinkTopologyState.Ready);
  storeHealthy = false;

  const degraded = runtime.snapshot('game');
  assert.equal(degraded.state, framework.ZLinkTopologyState.Degraded);
  assert.equal(degraded.isReady, false);
  assert.equal(degraded.placement.isAvailable, false);
  assert.equal(
    degraded.placement.unavailableReason,
    framework.ZLinkTopologyReason.LocationUnavailable
  );
});

test('RouteMesh observer reports Location Store degradation and recovery', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let storeHealthy = true;
  const runtime = createRuntime(gate, {
    meshNodeDescriptor: () => runtimeDescriptor(),
    isLocationStoreHealthy: () => storeHealthy
  });
  runtime.markServing();
  const events = runtime.observe('game', 4)[Symbol.asyncIterator]();

  storeHealthy = false;
  const degraded = await nextObserved(
    events,
    'Location Store degradation was not observed'
  );
  assert.equal(degraded.state, framework.ZLinkTopologyState.Degraded);
  assert.equal(degraded.isReady, false);

  storeHealthy = true;
  const recovered = await nextObserved(
    events,
    'Location Store recovery was not observed'
  );
  await events.return();
  assert.equal(recovered.state, framework.ZLinkTopologyState.Ready);
  assert.equal(recovered.isReady, true);
  assert.equal(recovered.placement.isAvailable, true);
  assert.ok(recovered.sequence > degraded.sequence);
});

test('multi-mesh drain fails before global owner cleanup can mutate another mesh', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let published = 0;
  let cleaned = 0;
  const node = fakeMeshNode();
  const runtime = new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game-a', 'game-b'],
    meshOptions: new Map([['game-a', {}], ['game-b', {}]]),
    meshNode: () => node,
    admission: gate,
    publishRetiring: async () => {},
    rollbackRetiring: async () => {},
    publishDraining: async () => { published += 1; },
    publishHostDraining: async () => {},
    drainResources: async () => { cleaned += 1; },
    cleanupHostResources: async () => {},
    forceStopResources: async () => {}
  });

  await assert.rejects(
    () => runtime.drain('game-a'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected
  );
  await assert.rejects(
    () => runtime.awaitDrained('game-b'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected
  );
  assert.equal(published, 0);
  assert.equal(cleaned, 0);
  assert.equal(gate.accepts('game-a'), true);
  assert.equal(gate.accepts('game-b'), true);
});

test('RouteMesh disables public readiness during relocation while preserving physical counts', () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  let hostState = framework.ZLinkFrameworkRuntimeState.Serving;
  const runtime = createRuntime(gate, {
    hostState: () => hostState,
    meshNodeDescriptor: () => runtimeDescriptor()
  });
  runtime.markServing();
  const serving = runtime.snapshot('game');
  assert.equal(serving.isReady, true);

  hostState = framework.ZLinkFrameworkRuntimeState.Relocating;
  runtime.hostStateChanged();
  const relocating = runtime.snapshot('game');
  assert.equal(relocating.isReady, false);
  assert.equal(relocating.state, framework.ZLinkTopologyState.Stopping);
  assert.equal(relocating.readyPeerCount, serving.readyPeerCount);
  assert.equal(relocating.placement.isAvailable, false);

  hostState = framework.ZLinkFrameworkRuntimeState.Relocated;
  runtime.hostStateChanged();
  assert.equal(runtime.snapshot('game').state, framework.ZLinkTopologyState.Stopping);
});

test('host drain seals every mesh, drains each resource set, and cleans the shared owner once', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const order = [];
  const node = fakeMeshNode();
  const runtime = new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game-a', 'game-b'],
    meshOptions: new Map([['game-a', {}], ['game-b', {}]]),
    meshNode: () => node,
    admission: gate,
    publishRetiring: async (meshName) => { order.push(`retiring:${meshName}`); },
    rollbackRetiring: async (meshName) => { order.push(`rollback:${meshName}`); },
    publishDraining: async (meshName) => { order.push(`publish:${meshName}`); },
    publishHostDraining: async () => { order.push('publish:host'); },
    drainResources: async (meshName) => { order.push(`drain:${meshName}`); },
    cleanupHostResources: async () => { order.push('cleanup'); },
    forceStopResources: async () => {}
  });
  runtime.markServing();

  assert.deepEqual(await runtime.drainHost(), { kind: 'drained' });
  assert.equal(gate.accepts('game-a'), false);
  assert.equal(gate.accepts('game-b'), false);
  assert.equal(runtime.snapshot('game-a').state, framework.ZLinkTopologyState.Stopped);
  assert.equal(runtime.snapshot('game-b').state, framework.ZLinkTopologyState.Stopped);
  assert.equal(order.filter((entry) => entry === 'cleanup').length, 1);
  assert.equal(order.filter((entry) => entry === 'publish:host').length, 1);
  assert.deepEqual(
    new Set(order.filter((entry) => entry === 'publish:game-a' || entry === 'publish:game-b')),
    new Set(['publish:game-a', 'publish:game-b'])
  );
  assert.deepEqual(
    new Set(order.filter((entry) => entry.startsWith('drain:'))),
    new Set(['drain:game-a', 'drain:game-b'])
  );
  assert.ok(order.indexOf('retiring:game-a') < order.indexOf('drain:game-a'));
  assert.ok(order.indexOf('drain:game-a') < order.indexOf('publish:game-a'));
  assert.ok(order.indexOf('publish:host') < order.indexOf('cleanup'));
});

test('host relocation moves stateful resources without sealing or tearing down infrastructure', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const order = [];
  const runtime = new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game-a', 'game-b'],
    meshOptions: new Map([['game-a', {}], ['game-b', {}]]),
    meshNode: () => fakeMeshNode(),
    admission: gate,
    publishRetiring: async (meshName) => { order.push(`retiring:${meshName}`); },
    rollbackRetiring: async (meshName) => { order.push(`rollback:${meshName}`); },
    publishDraining: async () => { throw new Error('relocation must not publish draining'); },
    publishHostDraining: async () => { throw new Error('relocation must not publish host draining'); },
    drainResources: async (meshName) => { order.push(`relocate:${meshName}`); },
    cleanupHostResources: async () => { throw new Error('relocation must not clean host owner'); },
    forceStopResources: async () => { throw new Error('relocation must not force-stop resources'); }
  });
  runtime.markServing();
  assert.equal(await runtime.prepareHostRetire(1000), 'prepared');

  assert.deepEqual(await runtime.relocateHost(1000), { kind: 'drained' });
  assert.equal(gate.accepts('game-a'), true);
  assert.equal(gate.accepts('game-b'), true);
  assert.equal(runtime.snapshot('game-a').state, framework.ZLinkTopologyState.Ready);
  assert.equal(runtime.snapshot('game-b').state, framework.ZLinkTopologyState.Ready);
  assert.deepEqual(
    new Set(order.filter(entry => entry.startsWith('relocate:'))),
    new Set(['relocate:game-a', 'relocate:game-b'])
  );
});

test('Retire descriptor publication rolls back before host drain state changes', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const order = [];
  const node = fakeMeshNode();
  const runtime = new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game-a', 'game-b'],
    meshOptions: new Map([['game-a', {}], ['game-b', {}]]),
    meshNode: () => node,
    admission: gate,
    publishRetiring: async (meshName) => {
      order.push(`retiring:${meshName}`);
      if (meshName === 'game-b') throw new Error('store unavailable');
    },
    rollbackRetiring: async (meshName) => { order.push(`serving:${meshName}`); },
    publishDraining: async () => {},
    publishHostDraining: async () => {},
    drainResources: async () => {},
    cleanupHostResources: async () => {},
    forceStopResources: async () => {}
  });
  runtime.markServing();

  assert.equal(await runtime.prepareHostRetire(1000), 'store_unavailable');
  assert.deepEqual(order, [
    'retiring:game-a',
    'retiring:game-b',
    'serving:game-b',
    'serving:game-a'
  ]);
  assert.equal(runtime.isReady('game-a'), true);
  assert.equal(runtime.isReady('game-b'), true);
  assert.equal(gate.accepts('game-a'), true);
  assert.equal(gate.accepts('game-b'), true);
});

test('Retire descriptor publication does not report a reversible block when rollback is unconfirmed', async () => {
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  const runtime = createRuntime(gate, {
    publishRetiring: async () => { throw new Error('ambiguous store response'); },
    rollbackRetiring: async () => { throw new Error('store unavailable'); }
  });

  await assert.rejects(
    () => runtime.prepareHostRetire(1000),
    (error) => error.name === 'ZLinkRetiringRollbackError'
  );
  assert.equal(runtime.isReady('game'), false);
  assert.equal(gate.accepts('game'), true);
});

test('Retire rollback reconciles a committed descriptor when only the Store response was lost', async () => {
  const manager = Object.create(framework.ZLinkSpotNodeRuntimeManager.prototype);
  const committed = {
    rid: 'node-old',
    lifecycleGeneration: 3n,
    descriptorRevision: 8n,
    ownerId: 'owner-old',
    leaseGeneration: 5n
  };
  manager.locationAutoConnect = {
    runtime: {
      currentOwnerToken: { ownerId: 'owner-old', leaseGeneration: 5n },
      async listLiveMeshNodes() { return [committed]; }
    }
  };
  manager.meshNodes = new Map([['game', {
    status() { return { routingId: 'node-old', lifecycleGeneration: 3n }; }
  }]]);
  manager.publishedMeshNodeDescriptors = new Map([['game', {
    ...committed,
    descriptorRevision: 7n
  }]]);
  manager.descriptorRevisionByMesh = new Map([['game', 7n]]);
  let republished;
  manager.publishMeshNodeState = async (state, _signal, meshName) => {
    republished = {
      state,
      meshName,
      revision: manager.publishedMeshNodeDescriptors.get(meshName).descriptorRevision
    };
  };

  await manager.reconcileAndPublishMeshNodeState(
    framework.ZLinkFrameworkRuntimeState.Serving,
    'game'
  );

  assert.deepEqual(republished, {
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    meshName: 'game',
    revision: 8n
  });
});

test('Retire readiness requires exact RID and lifecycle generation in the admitted Core peer table', () => {
  const local = {
    routingId: 'node-old',
    lifecycleGeneration: 1n,
    applicationVersion: 1n
  };
  const descriptors = [
    meshDescriptor('node-old', 1n, framework.ZLinkFrameworkRuntimeState.Serving),
    meshDescriptor('node-green', 7n, framework.ZLinkFrameworkRuntimeState.Serving)
  ];

  assert.equal(framework.hasExactPeerReadiness(descriptors, local, [
    { routingId: 'node-green', lifecycleGeneration: 6n, state: 3 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness(descriptors, local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 2 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness(descriptors, local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), true);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    { ...descriptors[1], applicationVersion: 2n }
  ], local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    { ...descriptors[1], applicationVersion: 0n }
  ], { ...local, applicationVersion: 0n }, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), true);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    { ...descriptors[1], applicationVersion: 2n }
  ], { ...local, applicationVersion: 2n }, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), true);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    { ...descriptors[1], applicationVersion: 0n }
  ], local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    { ...descriptors[1], maintenanceWave: 'wave-a' }
  ], { ...local, maintenanceWave: 'wave-a' }, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness([
    ...descriptors,
    meshDescriptor('node-green-2', 3n, framework.ZLinkFrameworkRuntimeState.Serving)
  ], local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), true);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    {
      ...descriptors[1],
      objectRole: framework.ZLinkObjectRole.Client
    }
  ], local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 }
  ]), false);
  assert.equal(framework.hasExactPeerReadiness([
    descriptors[0],
    {
      ...descriptors[1],
      objectRole: framework.ZLinkObjectRole.Client
    },
    meshDescriptor('node-server', 4n, framework.ZLinkFrameworkRuntimeState.Serving)
  ], local, [
    { routingId: 'node-green', lifecycleGeneration: 7n, state: 3 },
    { routingId: 'node-server', lifecycleGeneration: 4n, state: 3 }
  ]), true);
  const draining = [
    descriptors[0],
    meshDescriptor('node-green', 7n, framework.ZLinkFrameworkRuntimeState.Draining)
  ];
  assert.equal(framework.hasExactPeerReadiness(draining, local, []), false);
  assert.equal(framework.hasExactPeerReadiness([descriptors[0]], local, []), false);
});

function createRuntime(gate, overrides = {}) {
  const node = fakeMeshNode();
  return new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['game'],
    meshOptions: new Map([['game', overrides.meshOptions ?? { meshChannels: {} }]]),
    meshNode: (meshName) =>
      meshName === 'game' ? (overrides.meshNode ?? node) : undefined,
    meshNodeDescriptor: overrides.meshNodeDescriptor,
    isLocationStoreHealthy: overrides.isLocationStoreHealthy,
    hostState: overrides.hostState,
    admission: gate,
    publishRetiring: overrides.publishRetiring ?? (async () => {}),
    rollbackRetiring: overrides.rollbackRetiring ?? (async () => {}),
    publishDraining: overrides.publishDraining ?? (async () => {}),
    publishHostDraining: overrides.publishHostDraining ?? (async () => {}),
    drainResources: overrides.drainResources ?? (async () => {}),
    cleanupHostResources: overrides.cleanupHostResources ?? (async () => {}),
    forceStopResources: overrides.forceStopResources ?? (async () => {})
  });
}

function runtimeDescriptor(overrides = {}) {
  return {
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 100 },
      spots: { active: 0, reserved: 0, limit: 100 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 64 },
    channelWeights: {},
    applicationVersion: 1n,
    objectCapabilities: [],
    ...overrides
  };
}

function fakeMeshNode() {
  return {
    status() {
      return {
        meshName: 'game', routingId: 'node-a', lifecycleGeneration: 1n,
        descriptorRevision: 1n, localEndpoint: 'tcp://127.0.0.1:1', state: 3,
        lastChangedMs: 1n,
        pendingApplicationMessages: 0n, pendingInfrastructureMessages: 0n
      };
    },
    peers() { return []; },
    peerChannels() { return { names: [], weights: [] }; }
  };
}

function meshDescriptor(rid, lifecycleGeneration, state) {
  return {
    rid,
    lifecycleGeneration,
    state,
    applicationVersion: 1n,
    objectRole: framework.ZLinkObjectRole.Server
  };
}

function deferred() {
  let resolve;
  const promise = new Promise((completed) => { resolve = completed; });
  return { promise, resolve };
}

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

async function nextObserved(events, timeoutMessage) {
  const observed = await Promise.race([
    events.next(),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error(timeoutMessage)),
      1000
    ))
  ]);
  assert.equal(observed.done, false);
  return observed.value.status;
}

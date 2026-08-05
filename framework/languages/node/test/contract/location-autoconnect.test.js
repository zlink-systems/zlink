const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const routingIdRuntime = require('../../packages/framework/dist/runtime/routing-id');
const spotNodeAutoConnect = require('../../packages/framework/dist/runtime/spots/spot-node-autoconnect');

test('backend routing-id conversion preserves opaque ids from another package instance', () => {
  const expected = zlink.RoutingId.from('node-remote');
  const foreignRoutingId = {
    toBytes() {
      return expected.toBytes();
    },
    toHex() {
      return expected.toHex();
    },
    toString() {
      return 'node-remote';
    }
  };

  assert.equal(
    routingIdRuntime.toBackendRoutingId(foreignRoutingId).toHex(),
    expected.toHex()
  );
});

test('spot auto-connect carries the expected lifecycle and removes an unresolved pending intent', () => {
  const calls = [];
  const node = {
    status() {
      return {
        routingId: zlink.RoutingId.from('node-local'),
        localEndpoint: 'tcp://local'
      };
    },
    connectPeer(options) {
      calls.push({
        endpoint: options.endpoint,
        expectedRid: String(options.expectedRid),
        expectedSecurityIdentity: options.expectedSecurityIdentity,
        expectedLifecycleGeneration: options.expectedLifecycleGeneration
      });
      return 7n;
    },
    removePeerConnection(intentId) {
      calls.push(`remove:${intentId}`);
    },
    peers() {
      return [{
        endpoint: 'tcp://remote',
        routingId: null,
        lifecycleGeneration: 0n,
        state: 2
      }];
    },
    disconnectPeer() {
      calls.push('disconnect');
    }
  };
  const capability = spotNodeAutoConnect.spotNodeAutoConnectCapability(
    'zoneworld.zones',
    { router: { bind: 'tcp://local' } },
    node
  );
  const target = {
    targetKey: 'remote',
    nodeRid: zlink.RoutingId.from('node-remote'),
    lifecycleGeneration: 17n,
    endpoint: 'tcp://remote',
    role: internal.ZLinkLocationRole.Router,
    connectionKind: 'route-mesh',
    metadata: { securityIdentity: 'remote-security' }
  };

  assert.equal(capability.executor.connect(target), true);
  capability.executor.disconnect(target);

  assert.deepEqual(calls, [{
    endpoint: 'tcp://remote',
    expectedRid: 'node-remote',
    expectedSecurityIdentity: 'remote-security',
    expectedLifecycleGeneration: 17n
  }, 'remove:7']);
  assert.equal(capability.executor.isDisconnected(target), true);
});

test('spot auto-connect treats a synchronous unavailable endpoint as a retryable target', () => {
  let attempts = 0;
  const node = {
    status() {
      return {
        routingId: zlink.RoutingId.from('node-local'),
        localEndpoint: 'tcp://local'
      };
    },
    connectPeer() {
      attempts += 1;
      throw new Error('connect ECONNREFUSED');
    },
    peers() {
      return [];
    }
  };
  const capability = spotNodeAutoConnect.spotNodeAutoConnectCapability(
    'zoneworld.zones',
    { router: { bind: 'tcp://local' } },
    node
  );
  const target = {
    targetKey: 'remote',
    nodeRid: zlink.RoutingId.from('node-remote'),
    lifecycleGeneration: 17n,
    endpoint: 'tcp://unavailable',
    role: internal.ZLinkLocationRole.Router
  };

  assert.equal(capability.executor.connect(target), false);
  assert.equal(capability.executor.connect(target), false);
  assert.equal(attempts, 2);
});

test('spot auto-connect removes an admitted passive peer after its descriptor disappears', () => {
  const calls = [];
  const remoteRid = zlink.RoutingId.from('node-remote');
  const node = {
    status() {
      return {
        routingId: zlink.RoutingId.from('node-local'),
        localEndpoint: 'tcp://local'
      };
    },
    peers() {
      return [{
        endpoint: 'tcp://remote',
        routingId: remoteRid,
        lifecycleGeneration: 17n,
        state: 3
      }];
    },
    disconnectPeer(peerRid, lifecycleGeneration) {
      calls.push(`disconnect:${peerRid}:${lifecycleGeneration}`);
    }
  };
  const capability = spotNodeAutoConnect.spotNodeAutoConnectCapability(
    'zoneworld.zones',
    { router: { bind: 'tcp://local' } },
    node
  );

  capability.executor.disconnectStalePeers([]);

  assert.deepEqual(calls, ['disconnect:node-remote:17']);
});

test('spot auto-connect preserves a peer when only its opaque lifecycle generation differs', () => {
  const calls = [];
  const remoteRid = zlink.RoutingId.from('node-remote');
  const node = {
    status() {
      return {
        routingId: zlink.RoutingId.from('node-local'),
        localEndpoint: 'tcp://local'
      };
    },
    peers() {
      return [{
        endpoint: 'tcp://remote',
        routingId: remoteRid,
        lifecycleGeneration: 17n,
        state: 3
      }];
    },
    disconnectPeer(peerRid, lifecycleGeneration) {
      calls.push(`disconnect:${peerRid}:${lifecycleGeneration}`);
    }
  };
  const capability = spotNodeAutoConnect.spotNodeAutoConnectCapability(
    'zoneworld.zones',
    { router: { bind: 'tcp://local' } },
    node
  );

  capability.executor.disconnectStalePeers([{
    targetKey: 'remote',
    nodeRid: remoteRid,
    lifecycleGeneration: 18n,
    endpoint: 'tcp://remote',
    role: internal.ZLinkLocationRole.Router
  }]);

  assert.deepEqual(calls, []);
});

test('spot auto-connect uses the concrete endpoint resolved by the started MeshNode', () => {
  const node = {
    status() {
      return {
        routingId: zlink.RoutingId.from('node-local'),
        localEndpoint: 'tcp://127.0.0.1:43127'
      };
    }
  };

  const capability = spotNodeAutoConnect.spotNodeAutoConnectCapability(
    'zoneworld.actors',
    { router: { bind: 'tcp://127.0.0.1:0' } },
    node
  );

  assert.equal(capability.local.endpoint, 'tcp://127.0.0.1:43127');
  assert.equal(capability.localRow, undefined);
});

test('spot auto-connect derives RouteMesh candidates from live MeshNode descriptors', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(
    () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0))
  );
  const remoteOwner = await store.claimOwnerLease('owner-b', 30_000);
  assert.equal(remoteOwner.kind, 'claimed');
  await store.updateMeshNode(
    meshDescriptor(remoteOwner.token, 'node-b', 'tcp://b', 17n),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  const runtime = runtimeFor(store, 'owner-a');
  const context = spotNodeAutoConnect.createSpotNodeLocationAutoConnectContext(
    runtime,
    stores(store),
    {}
  );
  assert.equal(context.changeStampStore, undefined);

  const peers = await context.resolver.listLivePeers({
    autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'play',
    role: internal.ZLinkLocationRole.Router
  });

  assert.equal(peers.length, 1);
  assert.equal(String(peers[0].nodeRid), 'node-b');
  assert.equal(peers[0].endpoint, 'tcp://b');
  assert.equal(peers[0].generation, 17n);
  assert.equal(peers[0].draining, false);
});

test('auto-connect planner keys one intent per peer lifecycle generation', () => {
  const routeLocal = local(internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router, 'node-a', 'tcp://a');
  const lifecycleOne = {
    ...peer('owner-b', internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router, 'node-b', 'tcp://b'),
    generation: 1n
  };
  const lifecycleTwo = { ...lifecycleOne, generation: 2n };
  assert.notEqual(
    internal.ZLinkAutoConnectPlanner.targetKeyOf(lifecycleOne),
    internal.ZLinkAutoConnectPlanner.targetKeyOf(lifecycleTwo)
  );
  assert.equal(
    [...internal.ZLinkAutoConnectPlanner.computeDesired(
      routeLocal, [lifecycleTwo]
    ).values()][0].lifecycleGeneration,
    2n
  );
});

test('auto-connect planner applies role policy pairwise initiator and dial-only exception', () => {
  const routeLocal = local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://a');
  const routeDesired = internal.ZLinkAutoConnectPlanner.computeDesired(routeLocal, [
    peer('owner-b', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-b', 'tcp://b'),
    peer('owner-0', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-0', 'tcp://0'),
    peer('owner-dealer', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Dealer, 'node-c', 'tcp://c'),
    peer('owner-self', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://a')
  ]);

  assert.deepEqual([...routeDesired.values()].map((target) => target.endpoint), ['tcp://b']);

  const dialOnly = local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-z', '');
  const dialOnlyDesired = internal.ZLinkAutoConnectPlanner.computeDesired(dialOnly, [
    peer('owner-a', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://a')
  ]);
  assert.deepEqual([...dialOnlyDesired.values()].map((target) => target.endpoint), ['tcp://a']);

  const subscriber = local(internal.ZLinkLocationAutoConnectType.Fanout, internal.ZLinkLocationRole.Sub, 'node-d', 'tcp://subscriber');
  const fanoutDesired = internal.ZLinkAutoConnectPlanner.computeDesired(subscriber, [
    peer('owner-publisher', internal.ZLinkLocationAutoConnectType.Fanout, internal.ZLinkLocationRole.Pub, 'node-r', 'tcp://publisher'),
    peer('owner-subscriber', internal.ZLinkLocationAutoConnectType.Fanout, internal.ZLinkLocationRole.Sub, 'node-x', 'tcp://peer-subscriber')
  ]);
  assert.deepEqual([...fanoutDesired.values()].map((target) => target.endpoint), ['tcp://publisher']);
});

test('RouteMesh auto-connect skips only Object Client pairs without server memberships', () => {
  const routeLocal = {
    ...local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-a',
      'tcp://a'
    ),
    objectRole: 'client',
    hasRouteMeshServerChannel: false
  };
  const remoteClient = {
    ...peer(
      'owner-b',
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-b',
      'tcp://b'
    ),
    generation: 3n,
    metadata: {
      objectRole: 'client',
      hasRouteMeshServerChannel: 'false',
      descriptorRevision: '7'
    }
  };

  assert.equal(
    internal.ZLinkAutoConnectPlanner.computeDesired(
      routeLocal,
      [remoteClient]
    ).size,
    0
  );
  assert.deepEqual(
    internal.ZLinkAutoConnectPlanner.computeNotRequired(
      routeLocal,
      [remoteClient]
    ).map((target) => ({
      endpoint: target.endpoint,
      generation: target.lifecycleGeneration,
      revision: target.descriptorRevision
    })),
    [{ endpoint: 'tcp://b', generation: 3n, revision: 7n }]
  );

  const weightZeroServerMembership = {
    ...remoteClient,
    metadata: {
      ...remoteClient.metadata,
      hasRouteMeshServerChannel: 'true'
    }
  };
  assert.equal(
    internal.ZLinkAutoConnectPlanner.computeDesired(
      routeLocal,
      [weightZeroServerMembership]
    ).size,
    1
  );
  assert.equal(
    internal.ZLinkAutoConnectPlanner.computeNotRequired(
      routeLocal,
      [weightZeroServerMembership]
    ).length,
    0
  );
});

test('RouteMesh automatic discovery keeps the lower RID as the sole initiator', () => {
  const serviceClient = {
    ...local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-session',
      'tcp://session'
    ),
    objectRole: 'client',
    hasRouteMeshServerChannel: true
  };
  const objectServer = {
    ...peer(
      'owner-actor',
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-actor',
      'tcp://actor'
    ),
    metadata: {
      objectRole: 'server',
      hasRouteMeshServerChannel: 'true',
      descriptorRevision: '1'
    }
  };

  assert.deepEqual(
    [...internal.ZLinkAutoConnectPlanner.computeDesired(serviceClient, [objectServer]).values()]
      .map((target) => target.endpoint),
    []
  );

  const objectServerLocal = {
    ...local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-actor',
      'tcp://actor'
    ),
    objectRole: 'server',
    hasRouteMeshServerChannel: true
  };
  const serviceClientPeer = {
    ...peer(
      'owner-session',
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-session',
      'tcp://session'
    ),
    metadata: {
      objectRole: 'client',
      hasRouteMeshServerChannel: 'true',
      descriptorRevision: '1'
    }
  };
  assert.deepEqual(
    [...internal.ZLinkAutoConnectPlanner.computeDesired(objectServerLocal, [serviceClientPeer]).values()]
      .map((target) => target.endpoint),
    ['tcp://session']
  );
});

test('store peer resolver reads store each time and joins owner liveness', async () => {
  let nowMs = Date.UTC(2026, 6, 3, 0, 0, 0);
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  await store.claimOwnerLease('owner-live', 1000);
  await store.updatePeer(
    peer('owner-live', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-live', 'tcp://live'),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    peer('owner-missing', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-missing', 'tcp://missing'),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  const tracker = new internal.ZLinkOwnerLeaseTracker({
    store,
    options: { pollingIntervalMs: 0 },
    monotonicNowMs: () => 0
  });
  const resolver = new internal.ZLinkStoreLocationResolvers({
    stores: stores(store),
    leaseTracker: tracker
  });

  assert.deepEqual(
    (await resolver.listLivePeers({ meshName: 'play' })).map((row) => row.endpoint),
    ['tcp://live']
  );

  nowMs += 1001;
  assert.deepEqual(await resolver.listLivePeers({ meshName: 'play' }), []);
});

test('store resolver refreshes a cached owner miss when a newly observed row names that owner', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(
    () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0))
  );
  const tracker = new internal.ZLinkOwnerLeaseTracker({
    store,
    options: { pollingIntervalMs: 10_000 },
    monotonicNowMs: () => 0
  });
  const resolver = new internal.ZLinkStoreLocationResolvers({
    stores: stores(store),
    leaseTracker: tracker
  });

  assert.equal(await tracker.getLiveOwnerSetVersion(), 1);
  assert.deepEqual(await resolver.listLivePeers({ meshName: 'play' }), []);

  await store.claimOwnerLease('owner-new', 30_000);
  await store.updatePeer(
    peer(
      'owner-new',
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-new',
      'tcp://new'
    ),
    internal.ZLinkLocationWriteIntent.NewClaim
  );

  assert.deepEqual(
    (await resolver.listLivePeers({ meshName: 'play' })).map((row) => row.endpoint),
    ['tcp://new']
  );
});

test('store resolver refreshes a cached expired lease when the same owner renews before a row read', async () => {
  let nowMs = Date.UTC(2026, 6, 3, 0, 0, 0);
  let monotonicMs = 0;
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  const ownerRenewed = await store.claimOwnerLease('owner-renewed', 100);
  const tracker = new internal.ZLinkOwnerLeaseTracker({
    store,
    options: { pollingIntervalMs: 10_000 },
    monotonicNowMs: () => monotonicMs
  });
  const resolver = new internal.ZLinkStoreLocationResolvers({
    stores: stores(store),
    leaseTracker: tracker
  });

  assert.equal(await tracker.isOwnerLive('owner-renewed'), true);
  // The store still accepts the renewal before its lease deadline, while the
  // local monotonic projection has already exhausted the cached snapshot.
  nowMs += 50;
  monotonicMs += 101;
  assert.equal(ownerRenewed.kind, 'claimed');
  await store.renewOwnerLease(ownerRenewed.token, 30_000);
  await store.updatePeer(
    peer(
      'owner-renewed',
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-renewed',
      'tcp://renewed'
    ),
    internal.ZLinkLocationWriteIntent.NewClaim
  );

  assert.deepEqual(
    (await resolver.listLivePeers({ meshName: 'play' })).map((row) => row.endpoint),
    ['tcp://renewed']
  );
});

test('auto-connect reconciler publishes local row diffs handover and stays fail-static on store failure', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, 'owner-local');
  await runtime.start(rid('node-local'));

  await store.claimOwnerLease('owner-remote', 30000);
  const remote = await store.updatePeer(
    peer('owner-remote', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-remote', 'tcp://remote'),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(remote.status, internal.ZLinkLocationWriteStatus.Stored);

  const resolver = new SwitchablePeerResolver(new internal.ZLinkStoreLocationResolvers({
    stores: stores(store),
    leaseTracker: new internal.ZLinkOwnerLeaseTracker({
      store,
      options: { pollingIntervalMs: 0 },
      monotonicNowMs: () => 0
    })
  }));
  const calls = [];
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    localRow: peer('ignored', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    runtime,
    peerResolver: resolver,
    executor: executor(calls),
    options: { ownerLeaseRenewIntervalMs: 1000 },
    monotonicNowMs: () => 0
  });

  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote:owner-remote']);
  assert.equal((await store.listPeers({ endpoint: 'tcp://local' })).length, 1);
  assert.equal(reconciler.knowsPeer(rid('node-remote')), true);

  await store.claimOwnerLease('owner-restarted', 30000);
  await store.updatePeer(
    peer('owner-restarted', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-remote', 'tcp://remote'),
    internal.ZLinkLocationWriteIntent.Takeover
  );
  await reconciler.tick();
  assert.deepEqual(calls, [
    'connect:tcp://remote:owner-remote',
    'connect:tcp://remote:owner-restarted',
    'disconnect:tcp://remote:owner-remote'
  ]);

  resolver.fail = true;
  await reconciler.tick();
  assert.equal(reconciler.storeFailed, true);
  assert.equal(reconciler.activeTargets.length, 1);
  assert.equal(calls.length, 3);

  await reconciler.shutdown();
  assert.deepEqual(calls.slice(-1), ['disconnect:tcp://remote:owner-restarted']);
  assert.equal((await store.listPeers({ endpoint: 'tcp://local' })).length, 1);
  await runtime.stop();
  assert.equal((await store.listPeers({ endpoint: 'tcp://local' })).length, 0);
});

test('auto-connect reconciler does not mark a target active when executor skips dial', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, 'owner-local');
  await runtime.start(rid('node-local'));
  await store.claimOwnerLease('owner-remote', 30000);
  await store.updatePeer(
    peer('owner-remote', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-remote', 'tcp://manual'),
    internal.ZLinkLocationWriteIntent.NewClaim
  );

  const calls = [];
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    localRow: peer('ignored', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    runtime,
    peerResolver: new internal.ZLinkStoreLocationResolvers({
      stores: stores(store),
      leaseTracker: new internal.ZLinkOwnerLeaseTracker({
        store,
        options: { pollingIntervalMs: 0 },
        monotonicNowMs: () => 0
      })
    }),
    executor: {
      connect(target) {
        calls.push(`skip:${target.endpoint}:${target.ownerId}`);
        return false;
      },
      disconnect(target) {
        calls.push(`disconnect:${target.endpoint}:${target.ownerId}`);
      }
    },
    options: { ownerLeaseRenewIntervalMs: 1000 },
    monotonicNowMs: () => 0
  });

  await reconciler.tick();
  assert.deepEqual(calls, ['skip:tcp://manual:owner-remote']);
  assert.deepEqual(reconciler.activeTargets, []);

  await reconciler.shutdown();
  assert.deepEqual(calls, ['skip:tcp://manual:owner-remote']);
  await runtime.stop();
});

test('auto-connect reconciler waits for an old peer disconnect before reusing its route key', async () => {
  let disconnected = false;
  let rows = [peer(
    'owner-old',
    internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router,
    'node-z',
    'tcp://old'
  )];
  const calls = [];
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-a',
      'tcp://local'
    ),
    runtime: {},
    peerResolver: { async listLivePeers() { return rows; } },
    executor: {
      connect(target) {
        calls.push(`connect:${target.endpoint}`);
        return true;
      },
      disconnect(target) {
        calls.push(`disconnect:${target.endpoint}`);
      },
      isDisconnected() {
        return disconnected;
      }
    }
  });

  await reconciler.tick();
  rows = [peer(
    'owner-new',
    internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router,
    'node-z',
    'tcp://new'
  )];
  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://old', 'disconnect:tcp://old']);

  disconnected = true;
  await reconciler.tick();
  assert.deepEqual(calls, [
    'connect:tcp://old',
    'disconnect:tcp://old',
    'connect:tcp://new'
  ]);
});

test('auto-connect reconciler removes a disconnected endpoint until a fresh store read', async () => {
  let storeFailed = false;
  let disconnected;
  const calls = [];
  const remote = peer(
    'owner-remote',
    internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router,
    'node-remote',
    'tcp://remote'
  );
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-local',
      'tcp://dealer'
    ),
    runtime: {},
    peerResolver: {
      async listLivePeers() {
        if (storeFailed) throw new Error('store unavailable');
        return [remote];
      }
    },
    executor: {
      connect(target) {
        calls.push(`connect:${target.endpoint}`);
        return true;
      },
      disconnect() {},
      onDisconnected(handler) {
        disconnected = handler;
      }
    },
    options: { storeFailureGraceMs: 3000 },
    monotonicNowMs: () => 0
  });

  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote']);
  storeFailed = true;
  disconnected('tcp://remote');
  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote']);
  assert.deepEqual(reconciler.activeTargets, []);

  storeFailed = false;
  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote', 'connect:tcp://remote']);
  assert.equal(reconciler.activeTargets.length, 1);
});

test('auto-connect reconciler defers stale-peer pruning after an empty candidate scan', async () => {
  let nowMs = 0;
  let rows = [peer(
    'owner-remote',
    internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router,
    'node-remote',
    'tcp://remote'
  )];
  const calls = [];
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(
      internal.ZLinkLocationAutoConnectType.RouteMesh,
      internal.ZLinkLocationRole.Router,
      'node-local',
      'tcp://dealer'
    ),
    runtime: {},
    peerResolver: {
      async listLivePeers() {
        return rows;
      }
    },
    executor: {
      connect(target) {
        calls.push(`connect:${target.endpoint}`);
        return true;
      },
      disconnect(target) {
        calls.push(`disconnect:${target.endpoint}`);
      },
      disconnectStalePeers(targets) {
        calls.push(`stale:${targets.length}`);
      }
    },
    options: { ownerLeaseRenewIntervalMs: 1000 },
    monotonicNowMs: () => nowMs
  });

  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote', 'stale:1']);

  rows = [];
  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://remote', 'stale:1']);
  assert.equal(reconciler.activeTargets.length, 1);

  nowMs = 1000;
  await reconciler.tick();
  assert.deepEqual(calls, [
    'connect:tcp://remote',
    'stale:1',
    'stale:0',
    'disconnect:tcp://remote'
  ]);
  assert.equal(reconciler.activeTargets.length, 0);
});

test('auto-connect reconciler retries the last desired target only within store failure grace', async () => {
  let nowMs = 0;
  let storeFailed = false;
  let connectAttempts = 0;
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(
    internal.ZLinkLocationAutoConnectType.RouteMesh,
    internal.ZLinkLocationRole.Router,
      'node-local',
      'tcp://dealer'
    ),
    runtime: {},
    peerResolver: {
      async listLivePeers() {
        if (storeFailed) throw new Error('store unavailable');
        return [peer(
          'owner-remote',
          internal.ZLinkLocationAutoConnectType.RouteMesh,
          internal.ZLinkLocationRole.Router,
          'node-remote',
          'tcp://remote'
        )];
      }
    },
    executor: {
      connect() {
        connectAttempts += 1;
        return false;
      },
      disconnect() {}
    },
    options: { storeFailureGraceMs: 3000 },
    monotonicNowMs: () => nowMs
  });

  await reconciler.tick();
  assert.equal(connectAttempts, 1);
  storeFailed = true;
  await reconciler.tick();
  assert.equal(connectAttempts, 2);

  nowMs = 4000;
  await reconciler.tick();
  assert.equal(connectAttempts, 2);
});

test('publish-only auto-connect capability does not query or reconcile peers', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, 'owner-local');
  await runtime.start(rid('node-local'));
  let peerQueries = 0;
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    localRow: peer('ignored', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-local', 'tcp://local'),
    runtime,
    peerResolver: {
      async listLivePeers() {
        peerQueries += 1;
        return [];
      }
    },
    executor: {
      connect() {
        assert.fail('publish-only capability must not connect peers');
      },
      disconnect() {
        assert.fail('publish-only capability must not disconnect peers');
      }
    },
    reconcilePeers: false
  });

  await reconciler.tick();
  assert.equal(peerQueries, 0);
  assert.equal((await store.listPeers({ endpoint: 'tcp://local' })).length, 1);

  await reconciler.shutdown();
  await runtime.stop();
});

test('auto-connect reconciler retains an existing draining peer without dialing a new draining peer', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, 'owner-local');
  await runtime.start(rid('node-local'));
  await store.claimOwnerLease('owner-existing', 30000);
  const existing = await store.updatePeer(
    peer('owner-existing', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-existing', 'tcp://existing'),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  const calls = [];
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local: local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://local'),
    localRow: peer('ignored', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://local'),
    runtime,
    peerResolver: new internal.ZLinkStoreLocationResolvers({
      stores: stores(store),
      leaseTracker: new internal.ZLinkOwnerLeaseTracker({ store, options: { pollingIntervalMs: 0 }, monotonicNowMs: () => 0 })
    }),
    executor: executor(calls),
    options: { ownerLeaseRenewIntervalMs: 1000 },
    monotonicNowMs: () => 0
  });
  await reconciler.tick();
  await store.updatePeer({
    ...peer('owner-existing', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-existing', 'tcp://existing'),
    draining: true,
    generation: existing.generation
  }, internal.ZLinkLocationWriteIntent.Renew);
  await store.claimOwnerLease('owner-new', 30000);
  await store.updatePeer({
    ...peer('owner-new', internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-new', 'tcp://new'),
    draining: true
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  await reconciler.tick();
  assert.deepEqual(calls, ['connect:tcp://existing:owner-existing']);
  assert.deepEqual(reconciler.activeTargets.map((target) => target.endpoint), ['tcp://existing']);
  await reconciler.shutdown();
  await runtime.stop();
});

test('auto-connect loop skips unchanged change stamp until live owner set changes', async () => {
  const reconciler = {
    storeFailed: false,
    ticks: 0,
    async tick() {
      this.ticks++;
    },
    async shutdown() {}
  };
  const stampStore = {
    stamp: 1n,
    async getMeshNodeChangeStamp() {
      return this.stamp;
    }
  };
  const leaseTracker = {
    version: 1,
    async getLiveOwnerSetVersion() {
      return this.version;
    }
  };
  const loop = new internal.ZLinkAutoConnectLoop({
    reconciler,
    local: local(internal.ZLinkLocationAutoConnectType.RouteMesh, internal.ZLinkLocationRole.Router, 'node-a', 'tcp://a'),
    changeStampStore: stampStore,
    leaseTracker
  });

  await loop.tick();
  await loop.tick();
  assert.equal(reconciler.ticks, 1);

  leaseTracker.version = 2;
  await loop.tick();
  assert.equal(reconciler.ticks, 2);

  stampStore.stamp = 2n;
  await loop.tick();
  assert.equal(reconciler.ticks, 3);
});

class SwitchablePeerResolver {
  constructor(inner) {
    this.inner = inner;
    this.fail = false;
  }

  async listLivePeers(filter, signal) {
    if (this.fail) {
      throw new Error('store unavailable');
    }
    return this.inner.listLivePeers(filter, signal);
  }
}

function runtimeFor(store, ownerId) {
  return new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId,
    now: () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)),
    setTimer() {
      return 0;
    },
    clearTimer() {}
  });
}

function stores(store) {
  return {
    locationStore: store,
    peerStore: store,
    spotStore: store,
    actorStore: store,
    routeStore: store,
    ownerLeaseStore: store
  };
}

function executor(calls) {
  return {
    connect(target) {
      calls.push(`connect:${target.endpoint}:${target.ownerId}`);
      return true;
    },
    disconnect(target) {
      calls.push(`disconnect:${target.endpoint}:${target.ownerId}`);
    }
  };
}

function local(autoConnectType, role, nodeRid, endpoint) {
  return {
    autoConnectType,
    meshName: 'play',
    role,
    nodeRid: rid(nodeRid),
    endpoint
  };
}

function peer(ownerId, autoConnectType, role, nodeRid, endpoint) {
  return {
    autoConnectType,
    meshName: 'play',
    nodeRid: rid(nodeRid),
    role,
    endpoint,
    weight: 100,
    value: 0n,
    metadata: { endpoint },
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}

function meshDescriptor(owner, nodeRid, endpoint, lifecycleGeneration) {
  return {
    meshName: 'play',
    rid: rid(nodeRid),
    lifecycleGeneration,
    descriptorRevision: 1n,
    endpoint,
    objectRole: internal.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 100 },
      spots: { active: 0, reserved: 0, limit: 100 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 16 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [],
    state: internal.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: '',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

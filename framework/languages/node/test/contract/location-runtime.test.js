const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');

test('location runtime renews owner lease, records store failure, and recovers', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const leaseStore = new FlakyOwnerLeaseStore(store);
  const runtime = runtimeFor(store, { ownerLeaseStore: leaseStore });

  await runtime.start(rid('node-a'));
  assert.equal(runtime.ownerLeaseHealthy, true);
  assert.equal(runtime.lastError, undefined);

  leaseStore.fail = true;
  assert.equal(await runtime.renewOwnerLeaseOnce(), false);
  assert.equal(runtime.ownerLeaseHealthy, false);
  assert.match(runtime.lastError, /store unreachable/);

  leaseStore.fail = false;
  assert.equal(await runtime.renewOwnerLeaseOnce(), true);
  assert.equal(runtime.ownerLeaseHealthy, true);
  assert.equal(runtime.lastError, undefined);
});

test('location runtime reclaims immediately after the Store rejects a stale owner token', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  let stale = true;
  const leaseStore = {
    claimOwnerLease: store.claimOwnerLease.bind(store),
    readOwnerLease: store.readOwnerLease.bind(store),
    async renewOwnerLease(token, leaseTtlMs, signal) {
      if (stale) {
        stale = false;
        await store.releaseOwnerLease(token, signal);
        return { kind: 'stale' };
      }
      return await store.renewOwnerLease(token, leaseTtlMs, signal);
    },
    releaseOwnerLease: store.releaseOwnerLease.bind(store)
  };
  const runtime = runtimeFor(store, { ownerLeaseStore: leaseStore });

  await runtime.start(rid('node-a'));
  const previous = runtime.currentOwnerToken;
  assert.equal(await runtime.renewOwnerLeaseOnce(), true);
  assert.notDeepEqual(runtime.currentOwnerToken, previous);
  assert.equal(runtime.ownerLeaseHealthy, true);
  assert.equal(runtime.lastError, undefined);
});

test('location lifecycle reclaims tracked Spot and Actor rows after an owner lease generation change', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = runtimeFor(store, { ownerId: 'owner-a' });
  const lifecycle = new internal.ZLinkLocationLifecycle(runtime, store, 'play', store);

  await runtime.start(rid('node-a'));
  const actorClaim = await lifecycle.claimActor('player', 'actor-1', rid('node-a'));
  assert.equal(actorClaim.status, internal.ZLinkActorClaimStatus.Claimed);
  assert.equal(
    await lifecycle.claimSpot(
      'play',
      'spot-1',
      'player',
      rid('node-a'),
      framework.ZLinkSpotKind.Entry,
      1n,
      1n
    ),
    internal.ZLinkLocationWriteStatus.Stored
  );

  const oldOwner = runtime.currentOwnerToken;
  await store.releaseOwnerLease(oldOwner);
  assert.equal(await runtime.renewOwnerLeaseOnce(), true);
  const newOwner = runtime.currentOwnerToken;
  assert.notDeepEqual(newOwner, oldOwner);

  await lifecycle.reclaimOwnerRows();
  assert.equal(
    (await store.resolveActor({ meshName: 'play', actorId: 'actor-1' })).leaseGeneration,
    newOwner.leaseGeneration
  );
  assert.equal(
    (await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' })).leaseGeneration,
    newOwner.leaseGeneration
  );

  await lifecycle.releaseActor('player', 'actor-1');
  await lifecycle.releaseSpot('play', 'spot-1');
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
  assert.equal(await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' }), undefined);
  await runtime.stop();
});

test('location runtime bounds fixed routing-id owner lease renewal by the configured timeout', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const leaseStore = {
    claimOwnerLease: store.claimOwnerLease.bind(store),
    readOwnerLease: store.readOwnerLease.bind(store),
    async renewOwnerLease(token, leaseTtlMs, signal) {
      return await new Promise((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(signal.reason), { once: true });
      });
    },
    releaseOwnerLease: store.releaseOwnerLease.bind(store)
  };
  const runtime = runtimeFor(store, {
    ownerLeaseStore: leaseStore,
    locationOptions: { ownerLeaseRenewTimeoutMs: 10 }
  });

  await runtime.start(rid('node-a'));
  const keepAlive = setTimeout(() => {}, 100);
  assert.equal(await runtime.renewOwnerLeaseOnce(), false);
  clearTimeout(keepAlive);
  assert.match(runtime.lastError, /exceeded 10ms/);
  await runtime.stop();
});

test('location runtime schedules heartbeats from a monotonic fixed cadence after a late renewal', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const timers = [];
  let wallClockMs = 0;
  let monotonicMs = 0;
  const runtime = new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: store
    },
    options: {
      ownerLeaseRenewIntervalMs: 100,
      ownerLeaseRenewTimeoutMs: 50
    },
    now: () => new Date(wallClockMs),
    monotonicNowMs: () => monotonicMs,
    setTimer(callback, delayMs) {
      timers.push({ callback, delayMs });
      return timers.length - 1;
    },
    clearTimer() {}
  });

  await runtime.start(rid('node-a'));
  assert.equal(timers[0].delayMs, 100);

  wallClockMs = -6_000;
  monotonicMs = 150;
  timers.shift().callback();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(timers[0].delayMs, 50);

  await runtime.stop();
});

test('location runtime waits for an in-flight heartbeat before removing the owner lease', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const timers = [];
  let completeRenewal;
  const leaseStore = {
    claimOwnerLease: store.claimOwnerLease.bind(store),
    readOwnerLease: store.readOwnerLease.bind(store),
    async renewOwnerLease(token, leaseTtlMs, signal) {
      return await new Promise((resolve) => { completeRenewal = resolve; });
    },
    releaseOwnerLease: store.releaseOwnerLease.bind(store)
  };
  const runtime = new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: leaseStore
    },
    ownerId: 'owner-a',
    options: {
      ownerLeaseRenewIntervalMs: 100,
      ownerLeaseRenewTimeoutMs: 1_000
    },
    now: () => new Date(0),
    setTimer(callback, delayMs) {
      timers.push({ callback, delayMs });
      return timers.length - 1;
    },
    clearTimer() {}
  });

  await runtime.start(rid('node-a'));
  timers.shift().callback();
  await new Promise((resolve) => setImmediate(resolve));

  let stopped = false;
  const stopping = runtime.stop().then(() => { stopped = true; });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(stopped, false);

  completeRenewal({
    kind: 'renewed',
    leaseExpiresAt: new Date(30_000),
    storeNow: new Date(0)
  });
  await stopping;
  assert.deepEqual(await store.readOwnerLease('owner-a'), { kind: 'missing' });
});

test('location runtime does not install a reclaimed lease after stop races with the claim', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const timers = [];
  let claimCount = 0;
  let completeFreshClaim;
  const released = [];
  const leaseStore = {
    async claimOwnerLease(ownerId, leaseTtlMs, signal) {
      claimCount += 1;
      if (claimCount === 1) {
        return await store.claimOwnerLease(ownerId, leaseTtlMs, signal);
      }
      return await new Promise((resolve) => { completeFreshClaim = resolve; });
    },
    readOwnerLease: store.readOwnerLease.bind(store),
    async renewOwnerLease(token, leaseTtlMs, signal) {
      await store.releaseOwnerLease(token, signal);
      return { kind: 'stale' };
    },
    async releaseOwnerLease(token, signal) {
      released.push(token);
      return await store.releaseOwnerLease(token, signal);
    }
  };
  const runtime = new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: leaseStore
    },
    ownerId: 'owner-a',
    options: {
      ownerLeaseRenewIntervalMs: 100,
      ownerLeaseRenewTimeoutMs: 1000
    },
    now: () => new Date(0),
    setTimer(callback, delayMs) {
      timers.push({ callback, delayMs });
      return timers.length - 1;
    },
    clearTimer() {}
  });

  await runtime.start(rid('node-a'));
  timers.shift().callback();
  await new Promise((resolve) => setImmediate(resolve));

  const stopping = runtime.stop();
  await new Promise((resolve) => setImmediate(resolve));
  completeFreshClaim({
    kind: 'claimed',
    token: { ownerId: 'owner-a', leaseGeneration: 2n },
    leaseExpiresAt: new Date(30_000),
    storeNow: new Date(0)
  });
  await stopping;

  assert.equal(runtime.isStarted, false);
  assert.equal(runtime.currentOwnerToken, undefined);
  assert.deepEqual(await store.readOwnerLease('owner-a'), { kind: 'missing' });
  assert.deepEqual(released, [{ ownerId: 'owner-a', leaseGeneration: 2n }]);
});

test('location runtime stamps owner id, reports stale ownership, and removes rows on stop', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const oldOwner = runtimeFor(store, { ownerId: 'owner-a' });
  const newOwner = runtimeFor(store, { ownerId: 'owner-b' });
  const lost = [];
  oldOwner.addOwnershipLostHandler((event) => lost.push(event));

  await oldOwner.start(rid('node-a'));
  await newOwner.start(rid('node-b'));

  const claimed = await oldOwner.writeActor(actor('ignored', 0n), internal.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(claimed.status, internal.ZLinkLocationWriteStatus.Stored);
  assert.equal((await store.resolveActor({ meshName: 'play', actorId: 'actor-1' })).ownerId, 'owner-a');

  const takeover = await newOwner.writeActor(actor('ignored', 0n), internal.ZLinkLocationWriteIntent.Takeover);
  assert.equal(takeover.status, internal.ZLinkLocationWriteStatus.Stored);

  const stale = await oldOwner.writeActor(
    actor('ignored', claimed.generation),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(stale.status, internal.ZLinkLocationWriteStatus.IgnoredStale);
  assert.equal(lost.length, 1);
  assert.equal(lost[0].kind, internal.ZLinkLocationKind.Actor);

  await oldOwner.writeSpot(spot('ignored', 'spot-1'), internal.ZLinkLocationWriteIntent.NewClaim);
  await oldOwner.writeMeshNode({
    ...placementDescriptor('node-a', 'Player', 100, 0, 0),
    ownerId: 'ignored',
    leaseGeneration: 0n
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  await oldOwner.stop();

  assert.deepEqual(await store.readOwnerLease('owner-a'), { kind: 'missing' });
  assert.equal((await store.listMeshNodes('play')).items.length, 0);
  assert.equal(await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' }), undefined);
});

test('location runtime exposes owner cleanup failure instead of completing stop successfully', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const failingLocationStore = {
    async removeAllByOwner() { throw new Error('owner rows unavailable'); }
  };
  const runtime = runtimeFor(store, { locationStore: failingLocationStore });
  await runtime.start(rid('node-a'));

  await assert.rejects(() => runtime.stop(), /owner rows unavailable/);
  assert.match(runtime.lastError, /owner rows unavailable/);
});

test('location runtime emits row events and resolvers emit resolve misses', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const events = [];
  const sink = {
    peerRowUpdated(key, row) { events.push(['peer-updated', key, row]); },
    peerRowRemoved(key) { events.push(['peer-removed', key]); },
    desiredSetChanged(change) { events.push(['desired', change]); },
    spotRowUpdated(key, row) { events.push(['spot-updated', key, row]); },
    spotRowRemoved(key) { events.push(['spot-removed', key]); },
    spotResolveMiss(key) { events.push(['spot-miss', key]); },
    actorRowUpdated(key, row) { events.push(['actor-updated', key, row]); },
    actorRowRemoved(key) { events.push(['actor-removed', key]); },
    actorResolveMiss(key) { events.push(['actor-miss', key]); },
    routeRowUpdated(key, row) { events.push(['route-updated', key, row]); },
    routeRowRemoved(key) { events.push(['route-removed', key]); },
    routeResolveMiss(key) { events.push(['route-miss', key]); }
  };
  const runtime = runtimeFor(store, { ownerId: 'owner-a', events: sink });

  await runtime.start(rid('node-a'));
  const claimed = await runtime.writeActor(actor('', 0n), internal.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(events[0][0], 'actor-updated');
  assert.equal(events[0][2].ownerId, 'owner-a');
  assert.equal('generation' in events[0][2], false);

  await runtime.removeActor({ meshName: 'play', actorId: 'actor-1' }, claimed.generation);
  assert.equal(events[1][0], 'actor-removed');

  const resolvers = resolversFor(store, sink);
  assert.equal(await resolvers.resolveActorSpotHandle('play', 'missing'), undefined);
  assert.equal(events[2][0], 'actor-miss');
  assert.equal(events[2][1].actorId, 'missing');
});

test('location runtime applies listPageSize when callers omit a page size', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const pages = [];
  for (const methodName of ['listSpots', 'listActors', 'listRoutes']) {
    const original = store[methodName].bind(store);
    store[methodName] = (filter, page, signal) => {
      pages.push({ methodName, page });
      return original(filter, page, signal);
    };
  }
  const runtime = runtimeFor(store, { locationOptions: { listPageSize: 37 } });

  await runtime.listSpotLocations({});
  await runtime.listActorLocations({}, { continuationToken: 'next' });
  await runtime.listRouteLocations({}, { pageSize: 5 });

  assert.deepEqual(pages, [
    { methodName: 'listSpots', page: { pageSize: 37 } },
    { methodName: 'listActors', page: { continuationToken: 'next', pageSize: 37 } },
    { methodName: 'listRoutes', page: { pageSize: 5 } }
  ]);
});

test('location resolver filters exact actor capacity before weighted placement', async () => {
  const resolver = resolversFor(new internal.ZLinkInMemoryLocationStore());
  resolver.liveRows.filter = async rows => rows;
  resolver.options.stores.locationStore = {
    async listMeshNodes() {
      return { items: [
        placementDescriptor('node-z', 'Player', 1, 0, 0),
        placementDescriptor('node-a', 'Player', 1, 0, 0),
        placementDescriptor('node-full', 'Player', 10_000, 1, 0),
        placementDescriptor('node-other', 'Other', 10_000, 0, 0)
      ] };
    }
  };

  assert.equal(String(await resolver.selectActorPlacement('play', 'Player', 'node-source')), 'node-z');
  assert.equal(String(await resolver.selectActorPlacement('play', 'Player', 'node-source')), 'node-a');
  assert.equal(String(await resolver.selectActorPlacement('play', 'Player', 'node-z')), 'node-a');
});

function placementDescriptor(nodeRid, stableType, placementWeight, active, reserved) {
  return {
    meshName: 'play',
    rid: rid(nodeRid),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: `tcp://${nodeRid}`,
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight,
    populationCapacity: {
      actors: { active, reserved, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 1 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [{
      objectKind: 'actor',
      stableType,
      policy: 'disabled',
      hasSnapshotAdapter: false,
      limit: 0
    }],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'test',
    ownerId: nodeRid,
    leaseGeneration: 1n,
    updatedAt: new Date()
  };
}

test('location readiness returns false when ready state is missing or query fails', async () => {
  const ready = new internal.DefaultZLinkLocationReadiness({
    async listTopology(filter) {
      assert.equal(filter.meshName, 'play');
      assert.equal(filter.role, undefined);
      assert.equal(filter.kind, undefined);
      assert.equal(filter.state, framework.ZLinkLocationTopologyState.Ready);
      return { items: [{ nodeRid: rid('node-a') }] };
    }
  });
  assert.equal(await ready.isPeerReady('play', framework.ZLinkLocationRole.Router, rid('node-a')), true);

  const notReady = new internal.DefaultZLinkLocationReadiness({
    async listTopology() {
      return { items: [] };
    }
  });
  assert.equal(await notReady.isPeerReady('play', framework.ZLinkLocationRole.Router), false);

  const unknown = new internal.DefaultZLinkLocationReadiness({
    async listTopology() {
      throw new Error('store unavailable');
    }
  });
  assert.equal(await unknown.isPeerReady('play', framework.ZLinkLocationRole.Router), false);
});

test('location lifecycle claims before activation and loser never activates', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const nodeA = await lifecycleNode(store, 'owner-a', 'node-a');
  const nodeB = await lifecycleNode(store, 'owner-b', 'node-b');
  let activatedB = 0;

  const winner = await nodeA.lifecycle.executeActorClaimThenActivate(
    'player',
    'actor-1',
    rid('node-a'),
    undefined,
    async () => {
      const row = await store.resolveActor({ meshName: 'play', actorId: 'actor-1' });
      assert.equal(row.ownerId, 'owner-a');
      return 'instance-a';
    }
  );
  assert.equal(winner.activated, 'instance-a');
  assert.equal(winner.existingLocation, undefined);

  const loser = await nodeB.lifecycle.executeActorClaimThenActivate(
    'player',
    'actor-1',
    rid('node-b'),
    undefined,
    async () => {
      activatedB++;
      return 'instance-b';
    }
  );
  assert.equal(loser.activated, undefined);
  assert.equal(activatedB, 0);
  assert.equal(loser.existingLocation.ownerNodeRid.toHex(), rid('node-a').toHex());
  assert.equal(loser.existingLocation.ownerId, 'owner-a');
});

test('location lifecycle already-owned claim does not activate a second instance', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await lifecycleNode(store, 'owner-a', 'node-a');

  const first = await node.lifecycle.executeActorClaimThenActivate(
    'player',
    'actor-1',
    rid('node-a'),
    undefined,
    async () => 'instance-a'
  );
  assert.equal(first.activated, 'instance-a');

  let secondActivated = 0;
  const second = await node.lifecycle.executeActorClaimThenActivate(
    'player',
    'actor-1',
    rid('node-a'),
    undefined,
    async () => {
      secondActivated += 1;
      throw new Error('duplicate activation');
    }
  );

  assert.equal(second.activated, undefined);
  assert.equal(second.existingLocation, undefined);
  assert.equal(secondActivated, 0);
  assert.equal(node.lifecycle.ownsActor('player', 'actor-1'), true);
  assert.notEqual(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
});

test('location lifecycle rolls failed activation back and renews actor spot state', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await lifecycleNode(store, 'owner-a', 'node-a');

  await assert.rejects(
    () => node.lifecycle.executeActorClaimThenActivate(
      'player',
      'actor-1',
      rid('node-a'),
      undefined,
      async () => {
        throw new Error('factory failed');
      }
    ),
    /factory failed/
  );
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);

  const claim = await node.lifecycle.claimActor('player', 'actor-1', rid('node-a'));
  assert.equal(claim.status, internal.ZLinkActorClaimStatus.Claimed);
  const claimed = await store.resolveActor({ meshName: 'play', actorId: 'actor-1' });
  assert.ok(claimed);
  assert.equal('generation' in claimed, false);
  assert.ok(claim.generation > 0n);

  await node.lifecycle.setActorRef(
    'player',
    'actor-1',
    {
      nodeRid: rid('node-a'),
      actorId: 'actor-1',
      objectGeneration: 1n,
      meshName: 'play'
    },
    3n
  );
  await node.lifecycle.notifyActorJoinedSpot('player', 'actor-1', 'play', 'spot-1', 7n, 11n, 3n);
  const joined = await store.resolveActor({ meshName: 'play', actorId: 'actor-1' });
  assert.deepEqual(joined.actorRef, {
    nodeRid: rid('node-a'),
    actorId: 'actor-1',
    objectGeneration: 1n,
    meshName: 'play'
  });
  assert.equal(joined.spotKind, framework.ZLinkSpotKind.User);
  assert.equal(joined.meshName, 'play');
  assert.equal(joined.spotId, 'spot-1');
  assert.equal(joined.spotGeneration, 7n);
  assert.equal(joined.membershipEpoch, 11n);

  await node.lifecycle.notifyActorLeftSpot(
    'player',
    'actor-1',
    'play-entry-00000000-0000-4000-8000-000000000001',
    13n,
    17n,
    3n
  );
  const left = await store.resolveActor({ meshName: 'play', actorId: 'actor-1' });
  assert.equal(left.spotKind, framework.ZLinkSpotKind.Entry);
  assert.equal(left.spotId, 'play-entry-00000000-0000-4000-8000-000000000001');
  assert.equal(left.spotGeneration, 13n);
  assert.equal(left.membershipEpoch, 17n);
  assert.equal(left.ownerNodeGeneration, 3n);
  assert.equal('generation' in left, false);
});

test('location lifecycle deactivates stale hosted actor and protects new owner row', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const nodeA = await lifecycleNode(store, 'owner-a', 'node-a');
  const nodeB = await lifecycleNode(store, 'owner-b', 'node-b');
  let deactivated = 0;

  const claim = await nodeA.lifecycle.claimActor('player', 'actor-1', rid('node-a'), async () => {
    deactivated++;
  });
  assert.equal(claim.status, internal.ZLinkActorClaimStatus.Claimed);

  const takeover = await nodeB.runtime.writeActor(actor('ignored', 0n), internal.ZLinkLocationWriteIntent.Takeover);
  assert.equal(takeover.status, internal.ZLinkLocationWriteStatus.Stored);

  await assert.rejects(
    () => nodeA.lifecycle.notifyActorJoinedSpot('player', 'actor-1', 'play', 'spot-1', 7n, 11n, 3n),
    /renewal.*rejected/i
  );
  await Promise.resolve();
  assert.equal(deactivated, 1);
  assert.equal(nodeA.lifecycle.ownsActor('player', 'actor-1'), false);

  await nodeA.lifecycle.releaseActor('player', 'actor-1');
  const row = await store.resolveActor({ meshName: 'play', actorId: 'actor-1' });
  assert.equal(row.ownerId, 'owner-b');
});

test('location lifecycle retries source actor cleanup without losing the tracked generation', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await lifecycleNode(store, 'owner-a', 'node-a');
  await node.lifecycle.claimActor('player', 'actor-retry', rid('node-a'));
  const removeActor = node.runtime.removeActor.bind(node.runtime);
  let attempts = 0;
  node.runtime.removeActor = async (...args) => {
    attempts += 1;
    if (attempts === 1) throw new Error('temporary store failure');
    return await removeActor(...args);
  };

  await node.lifecycle.releaseActorEventually('player', 'actor-retry');

  assert.equal(attempts, 2);
  assert.equal(node.lifecycle.ownsActor('player', 'actor-retry'), false);
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'actor-retry' }), undefined);
});

test('location lifecycle releases placement Actor authority for the exact native ref', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, { ownerId: 'owner-a' });
  await runtime.start(rid('node-a'));
  const ownerToken = runtime.currentOwnerToken;
  assert.ok(ownerToken);
  let current = {
    kind: 'snapshot',
    storeVersion: { value: 'authority-v1' },
    payload: Buffer.alloc(0),
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: ownerToken.leaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'play', rid: rid('node-a') },
      descriptorLifecycleGeneration: 1n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(0)
  };
  let deletes = 0;
  const authorityStore = {
    async readAuthority() {
      return current;
    },
    async compareExchangeAuthority(_key, expectedVersion, mutation) {
      assert.equal(expectedVersion.value, 'authority-v1');
      assert.deepEqual(mutation, { kind: 'delete' });
      deletes++;
      current = { kind: 'missing', storeNow: new Date(0) };
      return { kind: 'deleted', storeVersion: { value: 'authority-v2' }, storeNow: new Date(0) };
    }
  };
  const lifecycle = new internal.ZLinkLocationLifecycle(runtime, store, 'play', authorityStore);
  await lifecycle.claimActor('player', 'actor-1', rid('node-a'));
  const actorRef = {
    actorId: 'actor-1',
    objectGeneration: 7n,
    meshName: 'play',
    nodeRid: rid('node-a')
  };
  await lifecycle.setActorRef('player', 'actor-1', actorRef, 1n);

  await lifecycle.releaseActor('player', 'actor-1', actorRef);

  assert.equal(deletes, 1);
  assert.equal(current.kind, 'missing');

  current = {
    kind: 'snapshot',
    storeVersion: { value: 'authority-v3' },
    payload: Buffer.alloc(0),
    objectGeneration: actorRef.objectGeneration,
    authorityOwnerGeneration: 12n,
    ownerId: ownerToken.ownerId,
    ownerLeaseGeneration: ownerToken.leaseGeneration + 1n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'play', rid: rid('node-a') },
      descriptorLifecycleGeneration: 1n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(0)
  };
  await lifecycle.releaseActor('player', 'actor-1', actorRef);
  assert.equal(deletes, 1);
  await runtime.stop();
});

test('location lifecycle durably closes an exact Ready Instance before deleting its authority', async () => {
  const locationStore = new internal.ZLinkInMemoryLocationStore();
  const runtime = runtimeFor(locationStore, { ownerId: 'owner-a' });
  const ownerLeaseGeneration = 5n;
  const payload = internal.encodeServiceInstanceAuthorityPayload({
    state: 'ready',
    stableType: 'Room',
    spotId: 'room-1',
    ownerId: 'owner-a',
    ownerLeaseGeneration,
    ownerMeshName: 'play',
    ownerNodeRid: 'node-a',
    ownerNodeGeneration: 3n
  });
  let current = {
    kind: 'snapshot',
    storeVersion: { value: 'authority-v1' },
    payload,
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    ownerId: 'owner-a',
    ownerLeaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'instance_spot',
      stableType: 'Room',
      descriptor: { meshName: 'play', rid: rid('node-a') },
      descriptorLifecycleGeneration: 3n,
      capacity: { actors: 0, spots: 1 }
    },
    storeNow: new Date(0)
  };
  const mutations = [];
  const authorityStore = {
    async readAuthority() {
      return current;
    },
    async compareExchangeAuthority(_key, expectedVersion, mutation) {
      mutations.push({ expectedVersion: expectedVersion.value, mutation });
      if (mutation.kind === 'put') {
        current = {
          ...current,
          storeVersion: { value: 'authority-v2' },
          payload: mutation.payload
        };
        return {
          kind: 'stored',
          storeVersion: current.storeVersion,
          storeNow: new Date(0)
        };
      }
      current = { kind: 'missing', storeNow: new Date(0) };
      return {
        kind: 'deleted',
        storeVersion: { value: 'authority-v3' },
        storeNow: new Date(0)
      };
    }
  };
  const invalidated = [];
  const lifecycle = new internal.ZLinkLocationLifecycle(
    runtime,
    locationStore,
    'play',
    authorityStore,
    undefined,
    (spotId) => invalidated.push(String(spotId))
  );
  lifecycle.trackInstanceSpot({
    meshName: 'play',
    spotId: 'room-1',
    stableType: 'Room',
    nodeRid: rid('node-a'),
    nodeGeneration: 3n,
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    ownerId: 'owner-a',
    ownerLeaseGeneration,
    storeVersion: 'authority-v1'
  });

  assert.equal(await lifecycle.beginInstanceSpotClosing('play', 'room-1'), true);
  assert.equal(internal.decodeServiceInstanceAuthorityPayload(current.payload).state, 'closing');
  assert.deepEqual(invalidated, ['room-1']);
  assert.equal(mutations[0].expectedVersion, 'authority-v1');
  assert.equal(mutations[0].mutation.generationTransition, 'preserve');

  await lifecycle.releaseSpot('play', 'room-1');
  assert.equal(mutations[1].expectedVersion, 'authority-v2');
  assert.deepEqual(mutations[1].mutation, { kind: 'delete' });
  assert.equal(current.kind, 'missing');
  assert.deepEqual(invalidated, ['room-1', 'room-1']);
});

test('location lifecycle ignores an old Instance generation release after authority replacement', async () => {
  const locationStore = new internal.ZLinkInMemoryLocationStore();
  const runtime = runtimeFor(locationStore, { ownerId: 'owner-a' });
  let deletes = 0;
  const authorityStore = {
    async readAuthority() {
      return {
        kind: 'snapshot',
        storeVersion: { value: 'authority-v2' },
        payload: Buffer.alloc(0),
        objectGeneration: 8n,
        authorityOwnerGeneration: 12n,
        ownerId: 'owner-b',
        ownerLeaseGeneration: 6n,
        allocation: {
          state: 'active',
          objectKind: 'instance_spot',
          stableType: 'Room',
          descriptor: { meshName: 'play', rid: rid('node-b') },
          descriptorLifecycleGeneration: 4n,
          capacity: { actors: 0, spots: 1 }
        },
        storeNow: new Date(0)
      };
    },
    async compareExchangeAuthority(_key, _expectedVersion, mutation) {
      if (mutation.kind === 'delete') {
        return {
          kind: 'conflict',
          current: await this.readAuthority(),
          storeNow: new Date(0)
        };
      }
      return { kind: 'stored', storeVersion: { value: 'authority-v3' }, storeNow: new Date(0) };
    }
  };
  const lifecycle = new internal.ZLinkLocationLifecycle(
    runtime,
    locationStore,
    'play',
    authorityStore
  );
  lifecycle.trackInstanceSpot({
    meshName: 'play',
    spotId: 'room-1',
    stableType: 'Room',
    nodeRid: rid('node-a'),
    nodeGeneration: 3n,
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    storeVersion: 'authority-v1'
  });

  await lifecycle.releaseSpot('play', 'room-1', 7n);
  assert.equal(deletes, 0);
});

test('location lifecycle claims spots and binds actor session routes with takeover', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const nodeA = await lifecycleNode(store, 'owner-a', 'node-a');
  const nodeB = await lifecycleNode(store, 'owner-b', 'node-b');

  const spotStatus = await nodeA.lifecycle.claimSpot(
    'play',
    'spot-1',
    'game',
    rid('node-a'),
    framework.ZLinkSpotKind.User,
    5n,
    2n
  );
  assert.equal(spotStatus, internal.ZLinkLocationWriteStatus.Stored);
  assert.equal((await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' })).ownerId, 'owner-a');

  await nodeA.lifecycle.releaseSpot('play', 'spot-1');
  assert.equal(await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' }), undefined);

  const sessionRid = rid('session-1');
  const routeKey = sessionRid.toHex();
  await nodeA.lifecycle.bindActorSessionRoute(sessionRid, 'actor-1', rid('node-a'));
  assert.equal(
    (await store.resolveRoute({ routeKind: internal.ZLinkRouteKind.ActorSession, routeKey })).ownerId,
    'owner-a'
  );

  await nodeB.lifecycle.bindActorSessionRoute(sessionRid, 'actor-1', rid('node-b'));
  const rebound = await store.resolveRoute({ routeKind: internal.ZLinkRouteKind.ActorSession, routeKey });
  assert.equal(rebound.ownerId, 'owner-b');
  assert.equal(rebound.ownerNodeRid.toHex(), rid('node-b').toHex());

  await nodeB.lifecycle.removeActorSessionRoute(sessionRid);
  assert.equal(await store.resolveRoute({ routeKind: internal.ZLinkRouteKind.ActorSession, routeKey }), undefined);
});

test('store location resolvers seed reusable SpotHandle snapshots from live rows', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await lifecycleNode(store, 'owner-a', 'node-a', 'play');
  const resolvers = resolversFor(store);
  let spotResolveCount = 0;
  const resolveSpot = store.resolveSpot.bind(store);
  store.resolveSpot = (key, signal) => {
    spotResolveCount++;
    return resolveSpot(key, signal);
  };

  assert.equal(
    await node.lifecycle.claimSpot(
      'play',
      'spot-1',
      'game',
      rid('node-a'),
      framework.ZLinkSpotKind.User,
      7n,
      3n
    ),
    internal.ZLinkLocationWriteStatus.Stored
  );
  await node.lifecycle.claimActor('player', 'actor-1', rid('node-a'));
  let actorAddress = await resolvers.resolveActorSpotHandle('play', 'actor-1');
  assert.equal(actorAddress, undefined);

  await node.lifecycle.setActorRef(
    'player',
    'actor-1',
    {
      nodeRid: rid('node-a'),
      actorId: 'actor-1',
      objectGeneration: 1n,
      meshName: 'play'
    },
    3n
  );
  await node.lifecycle.notifyActorJoinedSpot('player', 'actor-1', 'play', 'spot-1', 7n, 11n, 3n);
  actorAddress = await resolvers.resolveActorSpotHandle('play', 'actor-1');
  assert.equal(actorAddress.spotId, 'spot-1');
  const actorTarget = await internal.resolveSpotHandle(actorAddress);
  assert.equal(actorTarget.meshName, 'play');
  assert.equal(actorTarget.spotKind, framework.ZLinkSpotKind.User);

  const firstSpotRef = await resolvers.resolveSpotHandle('play', 'spot-1');
  const secondSpotRef = await resolvers.resolveSpotHandle('play', 'spot-1');
  const firstTarget = await internal.resolveSpotHandle(firstSpotRef);
  assert.equal(firstTarget.meshName, 'play');
  assert.equal(firstTarget.nodeRid, String(rid('node-a')));
  assert.equal(firstTarget.spotKind, framework.ZLinkSpotKind.User);
  assert.equal(secondSpotRef.spotId, 'spot-1');
  assert.equal(spotResolveCount, 1);

  await node.runtime.stop();
  resolvers.invalidateSpotRoute('spot-1', 'play');
  resolvers.invalidateActorRoute('actor-1', 'play');
  assert.equal(await resolvers.resolveSpotHandle('play', 'spot-1'), undefined);
  assert.equal(await resolvers.resolveActorSpotHandle('play', 'actor-1'), undefined);
});

test('actor resolver rejects a stale membership when the same SPOT RID is recreated', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const firstOwner = await lifecycleNode(store, 'owner-a', 'node-a', 'play');
  const successor = await lifecycleNode(store, 'owner-b', 'node-b', 'play');
  const spotId = 'spot-1';
  const actorId = 'actor-1';
  const resolvers = resolversFor(store);

  await firstOwner.lifecycle.claimSpot(
    'play',
    spotId,
    'game',
    rid('node-a'),
    framework.ZLinkSpotKind.User,
    7n,
    3n
  );
  await firstOwner.lifecycle.claimActor('player', actorId, rid('node-a'));
  await firstOwner.lifecycle.setActorRef('player', actorId, {
    nodeRid: rid('node-a'),
    actorId,
    objectGeneration: 5n,
    meshName: 'play'
  });
  await firstOwner.lifecycle.notifyActorJoinedSpot(
    'player',
    actorId,
    'play',
    spotId,
    7n,
    11n,
    3n
  );
  assert.notEqual(await resolvers.resolveActorRef(actorId), undefined);

  const previousSpot = await store.resolveSpot({ meshName: 'play', spotId });
  await successor.runtime.writeSpot({
    ...previousSpot,
    spotGeneration: 8n,
    ownerNodeRid: rid('node-b'),
    ownerNodeGeneration: 4n
  }, internal.ZLinkLocationWriteIntent.Takeover);
  resolvers.invalidateSpotRoute(spotId, 'play');

  assert.equal(await resolvers.resolveActorRef(actorId), undefined);
  assert.equal(await resolvers.resolveActorSpotHandle('play', actorId), undefined);

  await successor.lifecycle.takeoverActorJoinedSpot(
    'player',
    actorId,
    {
      nodeRid: rid('node-b'),
      actorId,
      objectGeneration: 6n,
      meshName: 'play'
    },
    'play',
    spotId,
    8n,
    12n,
    4n
  );
  const current = await resolvers.resolveActorRef(actorId);
  assert.equal(String(current.nodeRid), 'node-b');
  assert.equal(current.objectGeneration, 6n);
});

test('store location resolver returns a live remote ActorRef', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = runtimeFor(store, { ownerId: 'owner-a' });
  await runtime.start(rid('node-a'));
  const actorRef = {
    nodeRid: rid('node-b'),
    actorId: 'alice',
    objectGeneration: 9n,
    meshName: 'play'
  };
  await runtime.writeActor({
    ...actor('owner-a', 0n),
    actorId: 'alice',
    actorRef,
    ownerNodeRid: rid('node-b'),
    ownerNodeGeneration: 1n,
    spotId: 'play-entry-node-b',
    spotGeneration: 1n,
    membershipEpoch: 1n
  }, internal.ZLinkLocationWriteIntent.NewClaim);

  assert.deepEqual(await resolversFor(store).resolveActorRef('alice'), actorRef);
  await runtime.stop();
});

test('Actor Message Follow invalidation preserves a newer cached owner fence', () => {
  const resolver = resolversFor(new internal.ZLinkInMemoryLocationStore());
  const route = {
    meshName: 'play',
    actorRef: {
      nodeRid: rid('node-new'),
      actorId: 'alice',
      objectGeneration: 9n,
      meshName: 'play'
    },
    actorType: 'player',
    ownerNodeGeneration: 11n,
    ownerId: 'owner-new',
    ownerLeaseGeneration: 13n,
    authorityOwnerGeneration: 17n,
    authorityStoreVersion: 'v2'
  };
  resolver.directActorRoutes.set('alice', {
    row: route,
    expiresAtMs: Number.MAX_SAFE_INTEGER,
    storeVersion: 'v2'
  });

  assert.equal(resolver.invalidateActorRouteIfMatches({
    actorId: 'alice',
    objectGeneration: 8n,
    targetNodeRid: 'node-old',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 3n
  }), false);
  assert.equal(resolver.directActorRoutes.has('alice'), true);

  assert.equal(resolver.invalidateActorRouteIfMatches({
    actorId: 'alice',
    objectGeneration: 9n,
    targetNodeRid: 'node-new',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 17n,
    ownerLeaseGeneration: 13n
  }), true);
  assert.equal(resolver.directActorRoutes.has('alice'), false);
});

test('location spot route resolver bridges internal routed transport', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await lifecycleNode(store, 'owner-a', 'node-a', 'play');
  const resolver = new internal.ZLinkLocationSpotRouteResolver(resolversFor(store), ['play']);

  await node.lifecycle.claimSpot(
    'play',
    'spot-1',
    'game',
    rid('node-a'),
    framework.ZLinkSpotKind.User,
    7n,
    3n
  );
  const address = await resolver.resolve('spot-1');
  assert.equal(address.routerChannelId, 'play');
  assert.equal(address.targetNodeRid.toHex(), rid('node-a').toHex());
  assert.equal(address.spotId, 'spot-1');
  assert.equal(address.spotKind, framework.ZLinkSpotKind.User);

  await node.runtime.stop();
  resolver.invalidate('spot-1');
  await assert.rejects(
    () => resolver.resolve('spot-1'),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});

test('location spot route resolver reads a local activation when the store row is not yet visible', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const local = {
    routerChannelId: 'play',
    targetNodeRid: rid('node-local'),
    spotId: 'spot-local',
    spotKind: framework.ZLinkSpotKind.User,
    targetSpotGeneration: 8n
  };
  const resolver = new internal.ZLinkLocationSpotRouteResolver(
    resolversFor(store),
    ['play'],
    (meshName) => meshName,
    (spotId) => spotId === local.spotId ? local : undefined
  );

  assert.deepEqual(await resolver.resolve(local.spotId), local);
});

test('location resolvers resolve Entry Spots from live MeshNode descriptors', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const runtime = runtimeFor(store, { ownerId: 'owner-a' });
  await runtime.start(rid('node-a'));
  const entrySpotId = 'play-entry-00000000-0000-4000-8000-000000000001';
  await runtime.writeMeshNode({
    ...placementDescriptor('node-a', 'Player', 100, 0, 0),
    entrySpotId
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  const resolvers = resolversFor(store);
  const handle = await resolvers.resolveSpotHandle('play', entrySpotId);
  assert.notEqual(handle, undefined);
  const handleTarget = await internal.resolveSpotHandle(handle);
  assert.equal(handleTarget.meshName, 'play');
  assert.equal(handleTarget.nodeRid, String(rid('node-a')));
  assert.equal(handleTarget.spotId, entrySpotId);
  assert.equal(handleTarget.spotKind, framework.ZLinkSpotKind.Entry);

  const resolver = new internal.ZLinkLocationSpotRouteResolver(resolvers, ['play']);

  const address = await resolver.resolve(entrySpotId);
  assert.equal(address.routerChannelId, 'play');
  assert.equal(address.targetNodeRid.toHex(), rid('node-a').toHex());
  assert.equal(address.spotId, entrySpotId);
  assert.equal(address.spotKind, framework.ZLinkSpotKind.Entry);

  await runtime.stop();
});

test('Entry Spot route resolution excludes non-serving nodes during replacement', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const entrySpotId = 'play-entry-00000000-0000-4000-8000-000000000002';

  const draining = {
    ...placementDescriptor('node-draining', 'Player', 100, 0, 0),
    entrySpotId,
    state: framework.ZLinkFrameworkRuntimeState.Draining
  };
  const serving = {
    ...placementDescriptor('node-serving', 'Player', 100, 0, 0),
    entrySpotId
  };
  const resolvers = resolversFor(store);
  // Keep the two visible descriptors in the same order as a replacement scan.
  // The resolver must apply the Serving-state fence before selecting a target.
  resolvers.liveRows.filter = async rows => rows;
  resolvers.options.stores.locationStore = {
    async listMeshNodes() {
      return { items: [draining, serving] };
    }
  };

  const route = await resolvers.resolveEntrySpotNode(entrySpotId, ['play']);
  assert.equal(String(route.nodeRid), 'node-serving');
  assert.equal(route.targetOwnerId, 'node-serving');
});

test('production repository persists owner lease and MeshNode records through only opaque Store primitives', async () => {
  let nowMs = Date.UTC(2026, 6, 3, 0, 0, 0);
  const provider = new internal.ZLinkInMemoryProviderLocationStore(() => new Date(nowMs));
  assert.deepEqual(
    ['read', 'scan', 'write'].filter(method => typeof provider[method] === 'function'),
    ['read', 'scan', 'write']
  );
  assert.equal(provider.claimOwnerLease, undefined);
  assert.equal(provider.updateMeshNode, undefined);

  const writer = new internal.ZLinkLocationStoreRepository(provider, () => new Date(nowMs));
  const reader = new internal.ZLinkLocationStoreRepository(provider, () => new Date(nowMs));
  const claimed = await writer.claimOwnerLease('owner-a', 30_000);
  assert.equal(claimed.kind, 'claimed');
  const descriptor = {
    ...placementDescriptor('node-a', 'Player', 100, 0, 0),
    ownerId: 'owner-a',
    leaseGeneration: claimed.token.leaseGeneration,
    entrySpotId: 'play-entry-00000000-0000-4000-8000-000000000001'
  };
  const stored = await writer.updateMeshNode(
    descriptor,
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(stored.status, internal.ZLinkLocationWriteStatus.Stored);

  const observed = await reader.listMeshNodes('play');
  assert.equal(observed.items.length, 1);
  assert.equal(observed.items[0].entrySpotId, descriptor.entrySpotId);
  assert.equal(observed.items[0].leaseGeneration, claimed.token.leaseGeneration);

  nowMs += 1_000;
  const renewed = await reader.renewOwnerLease(claimed.token, 30_000);
  assert.equal(renewed.kind, 'renewed');
  assert.equal(
    await reader.removeMeshNode(
      { meshName: 'play', rid: 'node-a' },
      claimed.token
    ),
    internal.ZLinkLocationWriteStatus.Stored
  );
  assert.equal((await writer.listMeshNodes('play')).items.length, 0);
});

test('production repository removes provider-owned records in one fenced conditional batch', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  const writes = [];
  const provider = {
    read: inner.read.bind(inner),
    scan: inner.scan.bind(inner),
    async write(request, signal) {
      writes.push(request);
      return await inner.write(request, signal);
    }
  };
  const repository = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await repository.claimOwnerLease('owner-batch', 30_000);
  assert.equal(claimed.kind, 'claimed');

  await repository.updateMeshNode({
    ...placementDescriptor('node-batch', 'Player', 100, 0, 0),
    ownerId: 'owner-batch',
    leaseGeneration: claimed.token.leaseGeneration
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  await repository.updateClientServer({
    channelName: 'orders',
    serverRid: rid('server-batch'),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://127.0.0.1:43001',
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'server-batch',
    ownerId: 'owner-batch',
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: now
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  await repository.updateFanoutPublisher({
    channelName: 'events',
    publisherRid: rid('publisher-batch'),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://127.0.0.1:43002',
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'publisher-batch',
    ownerId: 'owner-batch',
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: now
  }, internal.ZLinkLocationWriteIntent.NewClaim);
  writes.length = 0;
  assert.equal(await repository.removeAllByOwner(claimed.token), 3n);
  assert.equal(writes.length, 1);
  assert.equal(writes[0].mutations.length, 3);
  assert.equal(writes[0].conditions.length, 4);
  assert.equal(writes[0].mutations.every(mutation => mutation.kind === 'delete'), true);
});

test('production repository restarts an expired owner cleanup scan from its first page', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  let expireNextScan = true;
  let scanCalls = 0;
  const provider = {
    read: inner.read.bind(inner),
    async scan(request, signal) {
      scanCalls += 1;
      if (expireNextScan && request.cursor === undefined) {
        expireNextScan = false;
        return { kind: 'expired' };
      }
      return await inner.scan(request, signal);
    },
    write: inner.write.bind(inner)
  };
  const repository = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await repository.claimOwnerLease('owner-expired-scan', 30_000);
  assert.equal(claimed.kind, 'claimed');
  await repository.updateMeshNode({
    ...placementDescriptor('node-expired-scan', 'Player', 100, 0, 0),
    ownerId: 'owner-expired-scan',
    leaseGeneration: claimed.token.leaseGeneration
  }, internal.ZLinkLocationWriteIntent.NewClaim);

  assert.equal(await repository.removeAllByOwner(claimed.token), 1n);
  assert.equal(scanCalls > 6, true);
  assert.equal((await repository.listMeshNodes('play')).items.length, 0);
});

test('production repository does not delete a row after its owner lease is replaced', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  let ownerToken;
  let replaced = false;
  const provider = {
    read: inner.read.bind(inner),
    scan: inner.scan.bind(inner),
    async write(request, signal) {
      if (!replaced && request.mutations.some(mutation => mutation.kind === 'delete')) {
        replaced = true;
        assert.equal(await inner.releaseOwnerLease(ownerToken), 'released');
        assert.equal((await inner.claimOwnerLease('owner-fenced', 30_000)).kind, 'claimed');
      }
      return await inner.write(request, signal);
    }
  };
  const repository = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await repository.claimOwnerLease('owner-fenced', 30_000);
  assert.equal(claimed.kind, 'claimed');
  ownerToken = claimed.token;
  await repository.updateMeshNode({
    ...placementDescriptor('node-fenced', 'Player', 100, 0, 0),
    ownerId: 'owner-fenced',
    leaseGeneration: ownerToken.leaseGeneration
  }, internal.ZLinkLocationWriteIntent.NewClaim);

  assert.equal(await repository.removeAllByOwner(ownerToken), 0n);
  assert.equal((await repository.listMeshNodes('play')).items.length, 1);
});

test('production repository reconciles an opaque write applied before its response is lost', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  const provider = new ReplyLossLocationStore(inner);
  const repository = new internal.ZLinkLocationStoreRepository(provider, () => now);

  provider.loseNextResponse = true;
  const claimed = await repository.claimOwnerLease('owner-ambiguous', 30_000);

  assert.equal(claimed.kind, 'claimed');
  assert.equal(provider.observedWrites, 1);
  assert.deepEqual(
    await repository.readOwnerLease('owner-ambiguous'),
    {
      kind: 'found',
      token: claimed.token,
      leaseExpiresAt: new Date(now.getTime() + 30_000),
      storeNow: now
    }
  );
});

test('production repository classifies a changed ambiguous write as conflict', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const inner = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  const provider = new ReplyLossLocationStore(inner);
  const repository = new internal.ZLinkLocationStoreRepository(provider, () => now);

  provider.loseNextResponse = true;
  provider.replaceFirstMutationBeforeFailure = true;
  const claimed = await repository.claimOwnerLease('owner-conflict', 30_000);

  assert.deepEqual(claimed, { kind: 'conflict' });
  assert.equal(provider.observedWrites, 2);
});

test('production repository shares ClientServer and fanout discovery through only opaque Store primitives', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  const writer = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const reader = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await writer.claimOwnerLease('owner-a', 30_000);
  assert.equal(claimed.kind, 'claimed');

  const server = {
    channelName: 'orders',
    serverRid: rid('server-a'),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://127.0.0.1:43001',
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'server-a',
    ownerId: 'owner-a',
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: now
  };
  const publisher = {
    channelName: 'events',
    publisherRid: rid('publisher-a'),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://127.0.0.1:43002',
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'publisher-a',
    ownerId: 'owner-a',
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: now
  };

  assert.equal(
    (await writer.updateClientServer(
      server,
      internal.ZLinkLocationWriteIntent.NewClaim
    )).status,
    internal.ZLinkLocationWriteStatus.Stored
  );
  assert.equal(
    (await writer.updateFanoutPublisher(
      publisher,
      internal.ZLinkLocationWriteIntent.NewClaim
    )).status,
    internal.ZLinkLocationWriteStatus.Stored
  );

  const observedServers = await reader.listClientServers('orders');
  const observedPublishers = await reader.listFanoutPublishers('events');
  assert.equal(observedServers.items.length, 1);
  assert.equal(String(observedServers.items[0].serverRid), String(server.serverRid));
  assert.equal(observedPublishers.items.length, 1);
  assert.equal(String(observedPublishers.items[0].publisherRid), String(publisher.publisherRid));

  const renewed = await reader.updateClientServer(
    { ...server, descriptorRevision: 2n, weight: 250 },
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(renewed.status, internal.ZLinkLocationWriteStatus.Stored);
  assert.equal((await writer.listClientServers('orders')).items[0].weight, 250);

  assert.equal(await reader.removeAllByOwner(claimed.token), 2n);
  assert.equal((await writer.listClientServers('orders')).items.length, 0);
  assert.equal((await writer.listFanoutPublishers('events')).items.length, 0);
});

class ReplyLossLocationStore {
  loseNextResponse = false;
  replaceFirstMutationBeforeFailure = false;
  observedWrites = 0;

  constructor(inner) {
    this.inner = inner;
  }

  read(key, signal) {
    return this.inner.read(key, signal);
  }

  scan(request, signal) {
    return this.inner.scan(request, signal);
  }

  async write(request, signal) {
    this.observedWrites += 1;
    const result = await this.inner.write(request, signal);
    if (!this.loseNextResponse || result.kind !== 'applied') return result;

    this.loseNextResponse = false;
    if (this.replaceFirstMutationBeforeFailure) {
      const mutation = request.mutations[0];
      assert.equal(mutation.kind, 'put');
      const version = result.putVersions.find(
        candidate => candidate.key.value === mutation.key.value
      )?.version;
      assert.notEqual(version, undefined);
      await this.inner.write({
        conditions: [{ kind: 'version', key: mutation.key, expected: version }],
        mutations: [{ kind: 'put', key: mutation.key, bytes: Buffer.from('changed') }]
      });
      this.observedWrites += 1;
    }
    throw new DOMException(
      'provider applied the write but its response timed out',
      'TimeoutError'
    );
  }
}

test('production repository shares Spot Actor and route ownership through only opaque Store primitives', async () => {
  const now = new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new internal.ZLinkInMemoryProviderLocationStore(() => now);
  const writer = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const reader = new internal.ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await writer.claimOwnerLease('owner-a', 30_000);
  assert.equal(claimed.kind, 'claimed');

  const spotRow = {
    ...spot('owner-a', 'room-1'),
    leaseGeneration: claimed.token.leaseGeneration
  };
  const actorRow = {
    ...actor('owner-a', 1n),
    spotId: 'room-1',
    spotKind: framework.ZLinkSpotKind.User,
    leaseGeneration: claimed.token.leaseGeneration
  };
  const routeRow = {
    routeKind: internal.ZLinkRouteKind.ActorSession,
    routeKey: 'actor-1',
    ownerNodeRid: rid('node-1'),
    ownerId: 'owner-a',
    generation: claimed.token.leaseGeneration,
    value: Buffer.from([0, 1, 127, 255]),
    updatedAt: now
  };

  assert.equal(
    (await writer.updateSpot(
      spotRow,
      internal.ZLinkLocationWriteIntent.NewClaim
    )).status,
    internal.ZLinkLocationWriteStatus.Stored
  );
  assert.equal(
    (await writer.updateActor(
      actorRow,
      internal.ZLinkLocationWriteIntent.NewClaim
    )).status,
    internal.ZLinkLocationWriteStatus.Stored
  );
  const routeStored = await writer.updateRoute(
    routeRow,
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(routeStored.status, internal.ZLinkLocationWriteStatus.Stored);

  assert.equal((await reader.resolveSpot({ meshName: 'play', spotId: 'room-1' })).spotType, 'game');
  assert.equal((await reader.resolveActor({ meshName: 'play', actorId: 'actor-1' })).actorType, 'player');
  const observedRoute = await reader.resolveRoute({
    routeKind: internal.ZLinkRouteKind.ActorSession,
    routeKey: 'actor-1'
  });
  assert.deepEqual([...observedRoute.value], [0, 1, 127, 255]);
  assert.equal((await reader.listSpots({ meshName: 'play' })).items.length, 1);
  assert.equal((await reader.listActors({ spotId: 'room-1' })).items.length, 1);
  assert.equal((await reader.listRoutes({ ownerId: 'owner-a' })).items.length, 1);

  assert.equal(await reader.removeAllByOwner(claimed.token), 3n);
  assert.equal(await writer.resolveSpot({ meshName: 'play', spotId: 'room-1' }), undefined);
  assert.equal(await writer.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
  assert.equal(await writer.resolveRoute({
    routeKind: internal.ZLinkRouteKind.ActorSession,
    routeKey: 'actor-1'
  }), undefined);
});

async function lifecycleNode(store, ownerId, nodeRid, entryMeshName = 'play') {
  const runtime = runtimeFor(store, { ownerId });
  await runtime.start(rid(nodeRid));
  return {
    runtime,
    lifecycle: new internal.ZLinkLocationLifecycle(runtime, store, entryMeshName)
  };
}

function resolversFor(store, events) {
  return new internal.ZLinkStoreLocationResolvers({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: store
    },
    leaseTracker: new internal.ZLinkOwnerLeaseTracker({ store }),
    events,
    spotMeshNames: ['play']
  });
}

function runtimeFor(store, options = {}) {
  const timers = [];
  return new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: options.locationStore ?? store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: options.ownerLeaseStore ?? store
    },
    ownerId: options.ownerId,
    events: options.events,
    options: options.locationOptions,
    now: () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)),
    setTimer(callback, delayMs) {
      timers.push({ callback, delayMs });
      return timers.length - 1;
    },
    clearTimer() {}
  });
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

function actor(ownerId, _generation) {
  return {
    meshName: 'play',
    actorType: 'player',
    actorId: 'actor-1',
    actorRef: {
      nodeRid: rid('node-1'),
      actorId: 'actor-1',
      objectGeneration: 1n,
      meshName: 'play'
    },
    ownerNodeRid: rid('node-1'),
    ownerNodeGeneration: 1n,
    spotId: 'play-entry-node-1',
    spotGeneration: 1n,
    spotKind: framework.ZLinkSpotKind.Entry,
    membershipEpoch: 1n,
    ownerId,
    updatedAt: new Date(0)
  };
}

function spot(ownerId, spotId) {
  return {
    meshName: 'play',
    spotId,
    spotType: 'game',
    spotGeneration: 1n,
    ownerNodeRid: rid('node-a'),
    ownerNodeGeneration: 1n,
    spotKind: framework.ZLinkSpotKind.User,
    ownerId,
    updatedAt: new Date(0)
  };
}

class FlakyOwnerLeaseStore {
  constructor(inner) {
    this.inner = inner;
    this.fail = false;
  }

  claimOwnerLease(ownerId, leaseTtlMs, signal) {
    return this.inner.claimOwnerLease(ownerId, leaseTtlMs, signal);
  }

  readOwnerLease(ownerId, signal) {
    return this.inner.readOwnerLease(ownerId, signal);
  }

  renewOwnerLease(token, leaseTtlMs, signal) {
    if (this.fail) {
      throw new Error('store unreachable');
    }
    return this.inner.renewOwnerLease(token, leaseTtlMs, signal);
  }

  releaseOwnerLease(token, signal) {
    return this.inner.releaseOwnerLease(token, signal);
  }
}

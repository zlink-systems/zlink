const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const locationWrites = internal;

test('in-memory location store issues generations and fences owner writes', async () => {
  let nowMs = Date.UTC(2026, 6, 3, 0, 0, 0);
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(nowMs));

  let result = await store.claimOwnerLease('owner-a', 30000);
  assert.equal(result.kind, 'claimed');
  assert.equal(result.storeNow.toISOString(), new Date(nowMs).toISOString());
  assert.equal(result.leaseExpiresAt.toISOString(), new Date(nowMs + 30000).toISOString());
  result = await store.claimOwnerLease('owner-b', 30000);
  assert.equal(result.kind, 'claimed');
  assert.equal(result.storeNow.toISOString(), new Date(nowMs).toISOString());

  const claim = await store.updateActor(actor('owner-a', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(claim.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.equal(claim.generation, 1n);

  const conflict = await store.updateActor(actor('owner-b', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(conflict.status, locationWrites.ZLinkLocationWriteStatus.RejectedConflict);

  const renew = await store.updateActor(actor('owner-a', claim.generation), locationWrites.ZLinkLocationWriteIntent.Renew);
  assert.equal(renew.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.equal(renew.generation, claim.generation);

  const takeover = await store.updateActor(actor('owner-b', 0n), locationWrites.ZLinkLocationWriteIntent.Takeover);
  assert.equal(takeover.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.equal(takeover.generation, 2n);

  const stale = await store.updateActor(actor('owner-a', claim.generation), locationWrites.ZLinkLocationWriteIntent.Renew);
  assert.equal(stale.status, locationWrites.ZLinkLocationWriteStatus.IgnoredStale);

  const staleRemove = await store.removeActor(
    { meshName: 'play', actorId: 'actor-1' },
    { ownerId: 'owner-a', leaseGeneration: claim.generation }
  );
  assert.equal(staleRemove, locationWrites.ZLinkLocationWriteStatus.IgnoredStale);

  const removed = await store.removeActor(
    { meshName: 'play', actorId: 'actor-1' },
    { ownerId: 'owner-b', leaseGeneration: takeover.generation }
  );
  assert.equal(removed, locationWrites.ZLinkLocationWriteStatus.Stored);

  const reclaim = await store.updateActor(actor('owner-a', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(reclaim.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.equal(reclaim.generation, 3n);

  nowMs += 31000;
  const expiredOwnerClaim = await store.updateActor(actor('owner-b', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(expiredOwnerClaim.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.equal(expiredOwnerClaim.generation, 4n);

  assert.equal((await store.readOwnerLease('owner-a')).kind, 'missing');
  assert.equal((await store.readOwnerLease('owner-b')).kind, 'missing');
});

test('in-memory location store lists filters pages and bumps change stamps', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));

  const ownerA = await store.claimOwnerLease('owner-a', 30000);
  assert.equal(ownerA.kind, 'claimed');
  await store.updatePeer(peer('owner-a', 'node-1'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  await store.updatePeer(peer('owner-a', 'node-2'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  await store.updateSpot(spot('owner-a', 'spot-1'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  await store.updateSpot(spot('owner-a', 'spot-2'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  await store.updateRoute(route('owner-a', 'route-1'), locationWrites.ZLinkLocationWriteIntent.NewClaim);

  const peers = await store.listPeers({ meshName: 'play', role: framework.ZLinkLocationRole.Router });
  assert.equal(peers.length, 2);
  assert.equal((await store.getChangeStamp({ kind: internal.ZLinkLocationKind.Peer, meshName: 'play' })), 2n);
  assert.equal((await store.getChangeStamp({ kind: internal.ZLinkLocationKind.Peer })), 2n);

  const firstPage = await store.listSpots({ meshName: 'play' }, { pageSize: 1 });
  assert.equal(firstPage.items.length, 1);
  assert.equal(firstPage.continuationToken, '1');
  const secondPage = await store.listSpots(
    { meshName: 'play' },
    { pageSize: 1, continuationToken: firstPage.continuationToken }
  );
  assert.equal(secondPage.items.length, 1);
  assert.equal(secondPage.continuationToken, undefined);

  const resolvedSpot = await store.resolveSpot({ meshName: 'play', spotId: 'spot-1' });
  assert.equal(resolvedSpot.spotType, 'game');

  const resolvedRoute = await store.resolveRoute({
    routeKind: internal.ZLinkRouteKind.ActorSession,
    routeKey: 'route-1'
  });
  assert.deepEqual([...resolvedRoute.value], [1, 2, 3, 4]);

  assert.equal(await store.removeAllByOwner(ownerA.token), 5n);
  assert.equal((await store.listSpots({ meshName: 'play' })).items.length, 0);
});

test('in-memory location store matches the formal operation trace for core write statuses', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const trace = [];
  const record = (step, result) => trace.push(`${step}=${statusName(result.status)}:${result.generation}`);
  const recordStatus = (step, status) => trace.push(`${step}=${statusName(status)}:0`);
  const recordLease = (step, result) => trace.push(`${step}=expires:${result.leaseExpiresAt.toISOString()}`);

  const leaseA = await store.claimOwnerLease('owner-a', 30000);
  const leaseB = await store.claimOwnerLease('owner-b', 30000);
  assert.equal(leaseA.kind, 'claimed');
  assert.equal(leaseB.kind, 'claimed');
  recordLease('lease-a', leaseA);
  recordLease('lease-b', leaseB);

  const claim = await store.updateActor(actor('owner-a', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  record('actor-claim', claim);
  record('actor-claim-conflict',
    await store.updateActor(actor('owner-b', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim));
  record('actor-renew',
    await store.updateActor(actor('owner-a', claim.generation), locationWrites.ZLinkLocationWriteIntent.Renew));
  record('actor-renew-same-owner',
    await store.updateActor(actor('owner-a', claim.generation + 9n), locationWrites.ZLinkLocationWriteIntent.Renew));
  const takeover = await store.updateActor(actor('owner-b', 0n), locationWrites.ZLinkLocationWriteIntent.Takeover);
  record('actor-takeover', takeover);
  record('actor-old-owner-renew',
    await store.updateActor(actor('owner-a', claim.generation), locationWrites.ZLinkLocationWriteIntent.Renew));
  recordStatus('actor-old-owner-remove', await store.removeActor(
    { meshName: 'play', actorId: 'actor-1' },
    { ownerId: 'owner-a', leaseGeneration: claim.generation }
  ));
  recordStatus('actor-remove', await store.removeActor(
    { meshName: 'play', actorId: 'actor-1' },
    { ownerId: 'owner-b', leaseGeneration: takeover.generation }
  ));
  record('actor-reclaim',
    await store.updateActor(actor('owner-a', 0n), locationWrites.ZLinkLocationWriteIntent.NewClaim));

  record('spot-claim-1',
    await store.updateSpot(spot('owner-a', 'spot-1'), locationWrites.ZLinkLocationWriteIntent.NewClaim));
  record('spot-claim-2',
    await store.updateSpot(spot('owner-a', 'spot-2'), locationWrites.ZLinkLocationWriteIntent.NewClaim));
  recordStatus('spot-remove-wrong-owner', await store.removeSpot(
    { meshName: 'play', spotId: 'spot-1' },
    { ownerId: 'owner-b', leaseGeneration: 1n }
  ));
  const routeClaim = await store.updateRoute(route('owner-a'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  record('route-claim', routeClaim);
  record('route-remove', await store.removeRoute(
    { routeKind: internal.ZLinkRouteKind.ActorSession, routeKey: 'route-1' },
    { ownerId: 'owner-a', leaseGeneration: routeClaim.generation }
  ));
  record('route-reclaim',
    await store.updateRoute(route('owner-b'), locationWrites.ZLinkLocationWriteIntent.NewClaim));

  const peerClaim = await store.updatePeer(peer('owner-a'), locationWrites.ZLinkLocationWriteIntent.NewClaim);
  record('peer-claim', peerClaim);
  trace.push(`remove-all-by-owner=${await store.removeAllByOwner(leaseA.token)}`);
  trace.push(`lease-a-remove=${await store.releaseOwnerLease(leaseA.token)}`);
  record('peer-claim-after-lease-removed',
    await store.updatePeer(peer('owner-b'), locationWrites.ZLinkLocationWriteIntent.NewClaim));

  assert.deepEqual(trace, [
    'lease-a=expires:2026-07-03T00:00:30.000Z',
    'lease-b=expires:2026-07-03T00:00:30.000Z',
    'actor-claim=Stored:1',
    'actor-claim-conflict=RejectedConflict:0',
    'actor-renew=Stored:1',
    'actor-renew-same-owner=Stored:1',
    'actor-takeover=Stored:2',
    'actor-old-owner-renew=IgnoredStale:0',
    'actor-old-owner-remove=IgnoredStale:0',
    'actor-remove=Stored:0',
    'actor-reclaim=Stored:3',
    'spot-claim-1=Stored:1',
    'spot-claim-2=Stored:1',
    'spot-remove-wrong-owner=IgnoredStale:0',
    'route-claim=Stored:1',
    'route-remove=Stored:1',
    'route-reclaim=Stored:2',
    'peer-claim=Stored:1',
    'remove-all-by-owner=4',
    'lease-a-remove=released',
    'peer-claim-after-lease-removed=Stored:2'
  ]);
});

test('in-memory exact MeshNode descriptor and Actor transfer stores enforce their fences', async () => {
  let nowMs = Date.UTC(2026, 6, 3, 0, 0, 0);
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  const meshLease = await store.claimOwnerLease('mesh-owner-a', 30_000);
  const descriptor = {
    meshName: 'game',
    rid: rid('game-a'),
    lifecycleGeneration: 7n,
    descriptorRevision: 3n,
    endpoint: 'tcp://10.0.0.1:7300',
    objectRole: framework.ZLinkObjectRole.Server,
    entrySpotId: 'game-entry-123e4567-e89b-42d3-a456-426614174000',
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 100 },
      spots: { active: 0, reserved: 0, limit: 100 },
      spotTypes: [{
        objectKind: 'instance_spot', stableType: 'room', active: 0, reserved: 0, limit: 100
      }]
    },
    activationConcurrency: { active: 0, limit: 100 },
    channelWeights: { orders: 100, world: 50 },
    applicationVersion: 1n,
    spotTypes: ['room'],
    objectCapabilities: [{
      objectKind: 'instance_spot',
      stableType: 'room',
      policy: 'recreate',
      hasSnapshotAdapter: false,
      limit: 100
    }],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'cluster-a',
    ownerId: 'mesh-owner-a',
    leaseGeneration: meshLease.token.leaseGeneration,
    updatedAt: new Date(0)
  };
  const descriptorClaim = await store.updateMeshNode(
    descriptor,
    locationWrites.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(descriptorClaim.status, locationWrites.ZLinkLocationWriteStatus.Stored);
  assert.deepEqual((await store.listMeshNodes('game')).items, [{
    ...descriptor,
    updatedAt: new Date(nowMs)
  }]);
  assert.equal(
    await store.removeMeshNode(
      { meshName: 'game', rid: rid('game-a') },
      { ownerId: 'mesh-owner-a', leaseGeneration: descriptorClaim.generation + 1n }
    ),
    locationWrites.ZLinkLocationWriteStatus.IgnoredStale
  );

  const otherLease = await store.claimOwnerLease('mesh-owner-b', 30_000);
  assert.equal(otherLease.kind, 'claimed');
  const conflictingEntryIdentity = await store.updateMeshNode({
    ...descriptor,
    rid: rid('game-b'),
    lifecycleGeneration: 8n,
    descriptorRevision: 1n,
    endpoint: 'tcp://10.0.0.2:7300',
    ownerId: 'mesh-owner-b',
    leaseGeneration: otherLease.token.leaseGeneration
  }, locationWrites.ZLinkLocationWriteIntent.NewClaim);
  assert.equal(
    conflictingEntryIdentity.status,
    locationWrites.ZLinkLocationWriteStatus.RejectedConflict
  );
  assert.equal((await store.listMeshNodes('game')).items.length, 1);
  assert.equal(
    await store.removeMeshNode(
      { meshName: 'game', rid: rid('game-a') },
      meshLease.token
    ),
    locationWrites.ZLinkLocationWriteStatus.Stored
  );
  assert.equal(
    (await store.updateMeshNode({
      ...descriptor,
      rid: rid('game-b'),
      lifecycleGeneration: 8n,
      descriptorRevision: 1n,
      endpoint: 'tcp://10.0.0.2:7300',
      ownerId: 'mesh-owner-b',
      leaseGeneration: otherLease.token.leaseGeneration
    }, locationWrites.ZLinkLocationWriteIntent.NewClaim)).status,
    locationWrites.ZLinkLocationWriteStatus.Stored
  );

  await store.releaseOwnerLease(otherLease.token);
  const reclaimedMeshLease = await store.claimOwnerLease('mesh-owner-b', 30_000);
  assert.equal(reclaimedMeshLease.kind, 'claimed');
  const reclaimedDescriptor = await store.updateMeshNode({
    ...descriptor,
    rid: rid('game-b'),
    lifecycleGeneration: 8n,
    descriptorRevision: 2n,
    endpoint: 'tcp://10.0.0.2:7300',
    ownerId: 'mesh-owner-b',
    leaseGeneration: reclaimedMeshLease.token.leaseGeneration
  }, locationWrites.ZLinkLocationWriteIntent.Takeover);
  assert.equal(reclaimedDescriptor.status, locationWrites.ZLinkLocationWriteStatus.Stored);

  const request = actorTransferRequest();
  const prepared = await store.prepareActorTransfer(request);
  assert.equal(prepared.status, 'stored');
  assert.equal((await store.prepareActorTransfer(request)).status, 'stored');
  assert.equal((await store.prepareActorTransfer({
    ...request,
    transferId: '11111111-1111-1111-1111-111111111111'
  })).status, 'rejectedConflict');
  assert.equal((await store.commitActorTransfer(
    request.meshName, request.actorId, request.transferId, 'wrong-owner'
  )).status, 'rejectedConflict');
  assert.equal((await store.commitActorTransfer(
    request.meshName, request.actorId, request.transferId, request.recoveryOwnerId
  )).record.state, 'committed');
  nowMs += 30_001;
  assert.equal((await store.takeOverActorTransfer(
    request.meshName,
    request.actorId,
    request.transferId,
    'recovery-b',
    30_000
  )).record.recoveryOwnerId, 'recovery-b');
  assert.equal((await store.activateActorTransfer(
    request.meshName, request.actorId, request.transferId, 'recovery-b'
  )).record.state, 'activated');
  assert.equal(await store.resolveActorTransfer(request.meshName, request.actorId), undefined);
});

test('Framework-issued Entry Spot IDs are lowercase UUID v4 identities', () => {
  const ids = new Set();
  for (let index = 0; index < 128; index++) {
    const spotId = internal.createFrameworkEntrySpotId('game.node');
    assert.match(
      spotId,
      /^game\.node-entry-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
    );
    ids.add(spotId);
  }
  assert.equal(ids.size, 128);
  assert.throws(() => internal.createFrameworkEntrySpotId('bad prefix'), /diagnostic prefix/);
});

function rid(value) {
  return zlink.RoutingId.from(value);
}

function actor(ownerId, _generation) {
  return {
    meshName: 'play',
    actorType: 'player',
    actorId: 'actor-1',
    actorRef: { nodeRid: rid('node-1'), actorId: 'actor-1', generation: 1n },
    ownerNodeRid: rid('node-1'),
    ownerNodeGeneration: 1n,
    spotId: rid('node-1'),
    spotGeneration: 1n,
    spotKind: framework.ZLinkSpotKind.Entry,
    membershipEpoch: 1n,
    ownerId,
    updatedAt: new Date(0)
  };
}

function actorTransferRequest() {
  return {
    meshName: 'game',
    actorId: 'actor-1',
    transferId: '01234567-89ab-cdef-0123-456789abcdef',
    source: { nodeRid: rid('game-a'), actorId: 'actor-1', generation: 11n },
    target: { nodeRid: rid('game-b'), actorId: 'actor-1', generation: 11n },
    expectedActorGeneration: 11n,
    expectedMembershipEpoch: 4n,
    participants: new Set([rid('game-a'), rid('game-b')]),
    recoveryOwnerId: 'recovery-a',
    recoveryLeaseTtlMs: 30_000
  };
}

function peer(ownerId, nodeRid = 'node-1') {
  return {
    autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'play',
    nodeRid: rid(nodeRid),
    role: framework.ZLinkLocationRole.Router,
    endpoint: 'tcp://127.0.0.1:5001',
    weight: 100,
    value: 7n,
    metadata: { 'route-endpoint': 'tcp://127.0.0.1:6001' },
    capabilities: ['router', 'route-bridge'],
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}

function spot(ownerId, spotId) {
  return {
    meshName: 'play',
    spotId,
    spotType: 'game',
    spotGeneration: 1n,
    ownerNodeRid: rid('node-1'),
    ownerNodeGeneration: 1n,
    spotKind: framework.ZLinkSpotKind.User,
    ownerId,
    updatedAt: new Date(0)
  };
}

function route(ownerId, routeKey = 'route-1') {
  return {
    routeKind: internal.ZLinkRouteKind.ActorSession,
    routeKey,
    ownerNodeRid: rid('node-1'),
    ownerId,
    generation: 0n,
    value: Uint8Array.from([1, 2, 3, 4]),
    updatedAt: new Date(0)
  };
}

function statusName(status) {
  return Object.entries(locationWrites.ZLinkLocationWriteStatus)
    .find(([, value]) => value === status)[0];
}

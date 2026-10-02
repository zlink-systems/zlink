const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkRemoteActorPacketTargetStore } = require('../../packages/framework/dist/runtime/host/remote-actor-packet-target-store');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');

test('backend converts Entry membership at the shared native authority boundary', () => {
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, setReadableHandler() {}, bind() {},
    localEndpoint: () => 'inproc://node-samples-entry-authority',
    routesSnapshot: () => [], receive: () => undefined, close() {}
  };
  const backend = new ZLinkNodeRawMeshBackend('play', 'node-owner', {
    createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} })
  }, new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()));
  backend.setBind('inproc://node-samples-entry-authority');
  backend.start();
  try {
    const entry = backend.entrySpot();
    const entryGeneration = entry.status().lifecycleGeneration;
    backend.restoreActorAuthority('alice', 'Player', 3n, 5n, undefined, 0n, 7n);
    const restored = backend.actorLookup('alice');
    assert.equal(String(restored.spotId), String(entry.routingId));
    assert.equal(restored.spotGeneration, entryGeneration);
    assert.equal(restored.membershipEpoch, 7n);
    const room = backend.getOrCreateSpot('room').spot;
    backend.restoreActorAuthority('alice', 'Player', 3n, 5n, 'room', room.status().lifecycleGeneration, 8n);
    assert.equal(String(backend.actorLookup('alice').spotId), 'room');
  } finally {
    backend.close();
  }
});

test('joined Actor membership alone does not publish a direct Spot authority route', () => {
  const state = {
    spotId: 'room',
    spotGeneration: 7n,
    nativeActorRef: { actorId: 'alice', nodeRid: 'node-owner', generation: 3n }
  };
  const targets = new ZLinkRemoteActorPacketTargetStore({
    actorManager: () => ({ getState: () => state }),
    primaryNodeRid: () => 'node-owner',
    meshRouters: {
      defaultSpotRouterChannelId: () => 'play',
      defaultRouterChannelId: () => 'play'
    },
    spotRouterChannelIdForMesh: () => 'play'
  });
  assert.equal(targets.targetForState('alice'), undefined);
  const ready = {
    routerChannelId: 'play', targetNodeRid: 'node-owner', spotId: 'room',
    spotKind: framework.ZLinkSpotKind.User, targetSpotGeneration: 7n,
    targetNodeGeneration: 11n, authorityOwnerGeneration: 13n,
    targetOwnerId: 'owner', ownerLeaseGeneration: 17n, authorityStoreVersion: '19'
  };
  state.remoteActorPacketTarget = ready;
  assert.equal(targets.targetForState('alice'), ready);
  const actor = {
    actorId: 'alice',
    ref: { actorId: 'alice', nodeRid: 'node-remote', generation: 3n, meshName: 'play' }
  };
  const fallback = targets.cachedTargetForActor(actor);
  assert.equal(fallback.targetNodeRid, actor.ref.nodeRid);
  assert.equal(fallback.spotKind, framework.ZLinkSpotKind.Entry);
});

test('Entry Spot route carries the owning descriptor lifecycle into local Join', async () => {
  const resolver = new framework.ZLinkLocationSpotRouteResolver({
    resolveSpotRowInMeshes: async () => undefined,
    resolveEntrySpotNode: async () => ({
      meshName: 'play', nodeRid: 'node-owner', spotId: 'entry',
      targetNodeGeneration: 23n, targetOwnerId: 'owner', ownerLeaseGeneration: 17n
    })
  }, ['play']);
  const route = await resolver.resolve('entry');
  assert.equal(route.spotKind, framework.ZLinkSpotKind.Entry);
  assert.equal(route.targetSpotGeneration, 23n);
  assert.equal(route.targetNodeGeneration, 23n);
});

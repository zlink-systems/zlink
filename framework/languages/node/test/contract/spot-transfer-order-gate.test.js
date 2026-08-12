const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkActorTransferRuntime } = require(
  '../../packages/framework/dist/runtime/host/actor-transfer-runtime'
);

const nodeRoot = path.resolve(__dirname, '../..');

test('ST-F1 and ST-F3 compare packet values and require source cleanup evidence', () => {
  const f1 = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/SpotActorTransfer/Client/Scenarios/st-f1-packet-order-scenario.ts'
  ), 'utf8');
  const f3 = fs.readFileSync(path.join(
    nodeRoot,
    'e2e/SpotActorTransfer/Client/Scenarios/st-f3-request-order-scenario.ts'
  ), 'utf8');
  assert.match(f1, /assertValuesInOrder\([^;]+\['P1', 'P2', 'P3'\]/s);
  assert.match(f3, /assertValuesInOrder\([^;]+\['S1', 'S2', 'S3', 'S4'\]/s);
  assert.match(f1, /source_cleanup/);
});

test('committed Actor transfer reports source cleanup after source membership removal', async () => {
  const events = [];
  let finishCleanup;
  const cleanup = new Promise((resolve) => { finishCleanup = resolve; });
  const actor = { context: { actorId: 'actor-source-cleanup' } };
  const state = {
    actorType: 'player',
    spotId: 'source-spot',
    nativeActorRef: {
      actorId: actor.context.actorId,
      nodeRid: 'source-node',
      generation: 9n
    },
    locationGeneration: 3n,
    ownerLeaseGeneration: 5n,
    beginMove() { events.push('move-began'); },
    endMove() { events.push('move-ended'); }
  };
  const sourceAuthority = {
    kind: 'snapshot',
    storeVersion: { value: 'source-version' },
    payload: framework.encodeActorAuthorityIdentity({
      actorType: 'player',
      actor: {
        actorId: actor.context.actorId,
        objectGeneration: 9n,
        meshName: 'mesh',
        nodeRid: 'source-node'
      },
      meshName: 'mesh',
      ownerNodeGeneration: 3n,
      owner: { ownerId: 'source-owner', leaseGeneration: 5n },
      spotId: 'source-spot',
      spotGeneration: 1n
    }),
    objectGeneration: 9n,
    authorityOwnerGeneration: 3n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 5n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'mesh', rid: 'source-node' },
      descriptorLifecycleGeneration: 3n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(0)
  };
  const authorityStore = {
    async readAuthority() {
      return sourceAuthority;
    },
    async reserveRelocationCapacity(request) {
      events.push('authority-reserved');
      assert.equal(request.expectedStoreVersion.value, 'source-version');
      assert.equal(request.targetDescriptor.rid, 'target-node');
      return { kind: 'reserved', fence: { value: 'target-reservation' } };
    },
    async compareExchangeAuthority(_key, expectedStoreVersion, mutation) {
      events.push('authority-committed');
      assert.equal(expectedStoreVersion.value, 'source-version');
      assert.equal(mutation.generationTransition, 'newOwner');
      assert.equal(mutation.relocationCapacityFence.value, 'target-reservation');
      return {
        kind: 'stored',
        storeVersion: { value: 'target-version' },
        payload: mutation.payload,
        objectGeneration: 9n,
        authorityOwnerGeneration: 11n,
        ownerId: 'target-owner',
        ownerLeaseGeneration: 13n,
        allocation: {
          state: 'active',
          objectKind: 'actor',
          stableType: 'player',
          descriptor: { meshName: 'mesh', rid: 'target-node' },
          descriptorLifecycleGeneration: 7n,
          capacity: { actors: 1, spots: 0 }
        },
        storeNow: new Date(0)
      };
    }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () => ({
      async beginActorTransfer() { events.push('source-sealed'); },
      async prepareActorLeaveForTransfer() { events.push('source-leave-prepared'); },
      async commitActorLeaveAfterTransfer() { events.push('source-membership-removed'); }
    }),
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({}),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      begin() { events.push('handoff-began'); },
      isActive() { return false; },
      snapshot() { return []; },
      complete() { events.push('handoff-committed'); }
    },
    actorTransferRegistry: {
      async transferOut() {
        return {
          state: framework.ZLinkMessage.fromEncoded(zlink.Message.from('state'))
        };
      }
    },
    authorityStore: () => authorityStore,
    relocationStore: () => undefined,
    clearRemoteActorPacketTarget() {},
    onSourceDepartureCompleted(actorId) {
      events.push(`source-cleanup:${actorId}`);
      finishCleanup();
    }
  });

  const prepared = await runtime.prepareSource(actor, state);
  const target = {
    routerChannelId: 'mesh',
    targetNodeRid: 'target-node',
    spotId: 'target-spot',
    spotKind: framework.ZLinkSpotKind.User,
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 11n,
    targetOwnerId: 'target-owner',
    ownerLeaseGeneration: 13n
  };
  const targetActorRef = {
    actorId: actor.context.actorId,
    nodeRid: 'target-node',
    objectGeneration: 9n,
    meshName: 'mesh'
  };
  await prepared.reserveTarget(target);
  await prepared.commitAuthority(target, targetActorRef);
  prepared.commit(target, targetActorRef, []);
  await cleanup;

  assert.ok(
    events.indexOf('source-membership-removed') < events.indexOf(
      'source-cleanup:actor-source-cleanup'
    )
  );
  assert.equal(events.at(-1), 'source-cleanup:actor-source-cleanup');
});

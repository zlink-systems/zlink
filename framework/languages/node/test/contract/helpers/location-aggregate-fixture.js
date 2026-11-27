const assert = require('node:assert/strict');
const framework = require('../../../packages/framework/dist');
const {
  encodeAuthorityKey
} = require('../../../packages/framework/dist/runtime/locations/authority-key-codec');

function aggregateDescriptor(target, limit) {
  return {
    meshName: target.meshName,
    rid: target.nodeRid,
    lifecycleGeneration: target.nodeLifecycleGeneration,
    descriptorRevision: 1n,
    endpoint: `tcp://${target.nodeRid}`,
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 0 },
      spots: { active: 0, reserved: 0, limit },
      spotTypes: [
        {
          objectKind: 'user_spot',
          stableType: 'lobby',
          active: 0,
          reserved: 0,
          limit
        }
      ]
    },
    activationConcurrency: { active: 0, limit },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: ['lobby'],
    objectCapabilities: [
      {
        objectKind: 'user_spot',
        stableType: 'lobby',
        policy: 'snapshot',
        hasSnapshotAdapter: true,
        limit
      }
    ],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: String(target.nodeRid),
    ownerId: target.owner.ownerId,
    leaseGeneration: target.owner.leaseGeneration,
    updatedAt: new Date()
  };
}

async function createReadyUserSpot(repository, spotId, target) {
  const capacity = userSpotCapacity(1);
  const reserved = await repository.reserve({
    key: { kind: 'user_spot', globalId: spotId },
    intent: {
      stableType: 'lobby',
      requestContentReference: `request:${spotId}`,
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 16n
    },
    target,
    creatingPayload: Buffer.from(`creating-${spotId}`),
    capacity
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('reservation failed');
  const committed = await repository.commit({
    key: { kind: 'user_spot', globalId: spotId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target,
    readyPayload: Buffer.from(`ready-${spotId}`)
  });
  assert.equal(committed.kind, 'committed');
  if (committed.kind !== 'committed') throw new Error('commit failed');
  return committed.ready;
}

function aggregateRequest(
  aggregateId,
  generation,
  snapshots,
  target,
  spotIds = snapshots.map((_, index) =>
    snapshots.length === 1 ? 'room-abort' : `room-${index.toString().padStart(2, '0')}`
  )
) {
  const participants = snapshots
    .map((snapshot, index) => {
      const spotId = spotIds[index];
      return {
        authorityKey: encodeAuthorityKey('user_spot', spotId),
        expectedStoreVersion: snapshot.storeVersion,
        ownerTransition: 'newOwner',
        authorityPayload: Buffer.from(`relocated-${spotId}`),
        membershipMutation: Buffer.from(`membership-${spotId}`)
      };
    })
    .sort((left, right) => left.authorityKey.value.localeCompare(right.authorityKey.value));
  return {
    aggregateId,
    aggregateGeneration: generation,
    participants,
    targetDescriptor: {
      meshName: target.meshName,
      rid: target.nodeRid
    },
    targetDescriptorLifecycleGeneration: target.nodeLifecycleGeneration,
    capacity: userSpotCapacity(snapshots.length),
    targetOwner: target.owner
  };
}

function userSpotCapacity(count) {
  return {
    actors: 0,
    spots: count,
    spotType: {
      objectKind: 'user_spot',
      stableType: 'lobby',
      count
    }
  };
}

module.exports = {
  aggregateDescriptor,
  aggregateRequest,
  createReadyUserSpot,
  userSpotCapacity
};

import assert from 'node:assert/strict';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole
} from '../../packages/framework/src/contracts';
import {
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} from '../../packages/framework/src/contracts/Locations/Writes';
import { encodeServiceUserSpotAuthorityPayload } from '../../packages/framework/src/runtime/foundation/service-authority-payload-codec';
import type { ZLinkInMemoryAuthorityStore } from '../../packages/framework/src/runtime/locations/in-memory-authority-store';
import type { ZLinkInMemoryLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-location-store';
import type { ZLinkObjectCreationTarget } from '../../packages/framework/src/runtime/locations/internal-location-contracts';

export async function writeTargetDescriptor(
  store: ZLinkInMemoryLocationStore,
  ownerId: string,
  nodeRid: string,
  leaseTtlMs: number,
  limit = 1
) {
  const claimed = await store.claimOwnerLease(ownerId, leaseTtlMs);
  assert.equal(claimed.kind, 'claimed');
  if (claimed.kind !== 'claimed') throw new Error('owner lease claim failed');
  const descriptor = {
    meshName: 'mesh',
    rid: nodeRid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: `tcp://${nodeRid}`,
    objectRole: ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit },
      spotTypes: [
        { objectKind: 'user_spot' as const, stableType: 'room', active: 0, reserved: 0, limit }
      ]
    },
    activationConcurrency: { active: 0, limit: 1 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [
      {
        objectKind: 'user_spot' as const,
        stableType: 'room',
        policy: 'disabled' as const,
        hasSnapshotAdapter: false,
        entryAccepted: false,
        limit: 0
      }
    ],
    state: ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: nodeRid,
    ownerId,
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: new Date(100)
  };
  const stored = await store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim);
  assert.equal(stored.status, ZLinkLocationWriteStatus.Stored);
  return descriptor;
}

export function reserveRequest(globalId: string, placement: ZLinkObjectCreationTarget) {
  return {
    key: { kind: 'user_spot' as const, globalId },
    intent: {
      stableType: 'room',
      requestContentReference: `request:${globalId}`,
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 10n
    },
    target: placement,
    creatingPayload: Buffer.from('creating'),
    capacity: userSpotCapacity('room')
  };
}

export function userSpotCapacity(stableType: string) {
  return {
    actors: 0,
    spots: 1,
    spotType: {
      objectKind: 'user_spot' as const,
      stableType,
      count: 1
    }
  };
}

export async function createActive(
  store: Pick<ZLinkInMemoryAuthorityStore, 'reserve' | 'commit'>,
  globalId: string,
  placement: ZLinkObjectCreationTarget
) {
  const reserved = await store.reserve(reserveRequest(globalId, placement));
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('reservation failed');
  const committed = await store.commit({
    key: { kind: 'user_spot', globalId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: placement,
    readyPayload: encodeServiceUserSpotAuthorityPayload({
      state: 'ready',
      stableType: 'room',
      spotId: globalId,
      ownerId: placement.owner.ownerId,
      ownerLeaseGeneration: placement.owner.leaseGeneration,
      ownerMeshName: placement.meshName,
      ownerNodeRid: String(placement.nodeRid),
      ownerNodeGeneration: placement.nodeLifecycleGeneration
    })
  });
  assert.equal(committed.kind, 'committed');
  if (committed.kind !== 'committed') throw new Error('commit failed');
  return committed.ready;
}

import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole
} from '../../packages/framework/src/contracts';
import {
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} from '../../packages/framework/src/contracts/Locations/Writes';
import { ZLinkInMemoryProviderLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-provider-location-store';
import { ZLinkLocationStoreRepository } from '../../packages/framework/src/runtime/locations/location-store-repository';

test('capacity uses canonical v12 counter keys for each Spot type', async () => {
  const now = new Date('2026-08-23T00:00:00.000Z');
  const provider = new ZLinkInMemoryProviderLocationStore(() => now);
  const repository = new ZLinkLocationStoreRepository(provider, () => now);
  const claimed = await repository.claimOwnerLease('owner-a', 60_000);
  assert.equal(claimed.kind, 'claimed');
  if (claimed.kind !== 'claimed') return;

  const target = {
    meshName: 'play',
    nodeRid: 'node-a',
    nodeLifecycleGeneration: 1n,
    owner: claimed.token
  };
  const stored = await repository.updateMeshNode(
    {
      meshName: target.meshName,
      rid: target.nodeRid,
      lifecycleGeneration: target.nodeLifecycleGeneration,
      descriptorRevision: 1n,
      endpoint: 'tcp://node-a',
      objectRole: ZLinkObjectRole.Server,
      placementWeight: 100,
      populationCapacity: {
        actors: { active: 0, reserved: 0, limit: 0 },
        spots: { active: 0, reserved: 0, limit: 2 },
        spotTypes: ['Zulu', 'Alpha'].map((stableType) => ({
          objectKind: 'user_spot' as const,
          stableType,
          active: 0,
          reserved: 0,
          limit: 1
        }))
      },
      activationConcurrency: { active: 0, limit: 2 },
      channelWeights: {},
      applicationVersion: 1n,
      spotTypes: ['Zulu', 'Alpha'],
      objectCapabilities: ['Zulu', 'Alpha'].map((stableType) => ({
        objectKind: 'user_spot' as const,
        stableType,
        policy: 'snapshot' as const,
        hasSnapshotAdapter: true,
        entryAccepted: false,
        limit: 1
      })),
      state: ZLinkFrameworkRuntimeState.Serving,
      securityIdentity: 'node-a',
      ownerId: claimed.token.ownerId,
      leaseGeneration: claimed.token.leaseGeneration,
      updatedAt: now
    },
    ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(stored.status, ZLinkLocationWriteStatus.Stored);

  for (const stableType of ['Zulu', 'Alpha']) {
    const reserved = await repository.reserve({
      key: { kind: 'user_spot', globalId: stableType.toLowerCase() },
      intent: {
        stableType,
        requestContentReference: `request:${stableType}`,
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      creatingPayload: Buffer.from(stableType),
      capacity: {
        actors: 0,
        spots: 1,
        spotType: { objectKind: 'user_spot', stableType, count: 1 }
      }
    });
    assert.equal(reserved.kind, 'reserved');
  }

  const prefix = `zlink:v12:capacity\0play\0${Buffer.from('node-a').toString('hex')}\0spot-type\0userSpot\0`;
  const capacity = await provider.scan({ prefix, limit: 10 });
  assert.equal(capacity.kind, 'page');
  if (capacity.kind !== 'page') return;
  assert.deepEqual(
    capacity.value.items.map(({ key, value }) => [key.value, Buffer.from(value.bytes).toString()]),
    [
      [`${prefix}Alpha\0pending`, '1'],
      [`${prefix}Zulu\0pending`, '1']
    ]
  );
});

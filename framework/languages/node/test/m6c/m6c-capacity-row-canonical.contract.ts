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
import {
  ZLinkInMemoryProviderLocationStore,
  storeKey
} from '../../packages/framework/src/runtime/locations/in-memory-provider-location-store';
import {
  ZLinkLocationStoreRepository
} from '../../packages/framework/src/runtime/locations/location-store-repository';

test('capacity rows serialize spotTypes in UTF-16 ordinal key order', async () => {
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
  const stored = await repository.updateMeshNode({
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
      spotTypes: ['Zulu', 'Alpha'].map(stableType => ({
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
    objectCapabilities: ['Zulu', 'Alpha'].map(stableType => ({
      objectKind: 'user_spot' as const,
      stableType,
      policy: 'snapshot' as const,
      hasSnapshotAdapter: true,
      limit: 1
    })),
    state: ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'node-a',
    ownerId: claimed.token.ownerId,
    leaseGeneration: claimed.token.leaseGeneration,
    updatedAt: now
  }, ZLinkLocationWriteIntent.NewClaim);
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

  const capacity = await provider.read(storeKey('zlink:v11:capacity:play:node-a'));
  assert.equal(capacity.kind, 'found');
  if (capacity.kind !== 'found') return;
  const json = Buffer.from(capacity.value.bytes).toString('utf8');
  const alpha = json.indexOf('"user_spot\\u0000Alpha"');
  const zulu = json.indexOf('"user_spot\\u0000Zulu"');
  assert.ok(alpha >= 0 && zulu >= 0 && alpha < zulu, json);
});

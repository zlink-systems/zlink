const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const {
  ZLinkInMemoryLocationStore,
  ZLinkLocationStoreRepository,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} = require('../../packages/framework/dist/internal');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { aggregateDescriptor } = require('./helpers/location-aggregate-fixture');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');

for (const kind of ['user_spot', 'instance_spot']) {
  test(`Entry Spot descriptor publication and ${kind} reserve agree across stores`, async (t) => {
    const client = await connectTestRedis(t);
    if (client === undefined) return;

    const keyPrefix = `zlink:entry-spot-parity:${randomUUID()}`;
    const provider = new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix });
    t.after(async () => {
      try {
        await provider.dispose();
        const keys = await client.keys(`${keyPrefix}:*`);
        if (keys.length > 0) await client.del(keys);
      } finally {
        await client.quit();
      }
    });

    const redis = await writeSequence(new ZLinkLocationStoreRepository(provider), kind);
    const memory = await writeSequence(new ZLinkInMemoryLocationStore(), kind);
    assert.deepEqual(redis, {
      first: ZLinkLocationWriteStatus.Stored,
      second: ZLinkLocationWriteStatus.Stored,
      reserve: 'reserved',
      third: ZLinkLocationWriteStatus.Stored,
      competingReserve: 'typeMismatch'
    });
    assert.deepEqual(memory, redis);
  });
}

async function writeSequence(store, kind) {
  const entrySpotId = 'play-entry-00000000-0000-4000-8000-000000000001';
  const target = async (nodeRid) => {
    const owner = await store.claimOwnerLease(nodeRid, 60_000);
    assert.equal(owner.kind, 'claimed');
    return { meshName: 'play', nodeRid, nodeLifecycleGeneration: 1n, owner: owner.token };
  };
  const publish = async (placement) => {
    const descriptor = aggregateDescriptor(placement, 8);
    descriptor.entrySpotId = entrySpotId;
    descriptor.objectCapabilities.push({
      ...descriptor.objectCapabilities[0],
      objectKind: 'instance_spot'
    });
    descriptor.populationCapacity.spotTypes.push({
      ...descriptor.populationCapacity.spotTypes[0],
      objectKind: 'instance_spot'
    });
    return (await store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim)).status;
  };
  const reserve = async (placement, spotKind) =>
    (
      await store.reserve({
        key: { kind: spotKind, globalId: entrySpotId },
        intent: {
          stableType: 'lobby',
          requestContentReference: 'request:entry-spot-parity',
          requestSha256: Buffer.alloc(32),
          requestEncodedSize: 1n
        },
        target: placement,
        creatingPayload: Buffer.from([1]),
        capacity: {
          actors: 0,
          spots: 1,
          spotType: { objectKind: spotKind, stableType: 'lobby', count: 1 }
        }
      })
    ).kind;

  const firstTarget = await target('node-a');
  const first = await publish(firstTarget);
  const second = await publish(await target('node-b'));
  const reserved = await reserve(firstTarget, kind);
  const third = await publish(await target('node-c'));
  const competingKind = kind === 'user_spot' ? 'instance_spot' : 'user_spot';
  const competingReserve = await reserve(firstTarget, competingKind);
  return { first, second, reserve: reserved, third, competingReserve };
}

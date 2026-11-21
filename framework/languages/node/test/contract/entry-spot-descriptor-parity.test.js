const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { RoutingId } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');

const entryA = 'mesh-entry-00000000-0000-4000-8000-000000000001';
const entryB = 'mesh-entry-00000000-0000-4000-8000-000000000002';
const { ZLinkLocationWriteIntent: Intent, ZLinkLocationWriteStatus: Status } = internal;

for (const [id, expected] of [
  [entryA, Status.RejectedConflict],
  [entryB, Status.Stored]
]) {
  test(`Entry Spot NewClaim parity for second ID ${id}`, async (t) => {
    const memory = new internal.ZLinkInMemoryLocationStore();
    assert.equal(await publishSecond(memory, id), expected, 'in-memory NewClaim status');

    const redis = (await redisFixture(t))?.repository;
    if (redis === undefined) return;
    assert.equal(await publishSecond(redis, id), expected, 'Redis NewClaim status');
  });
}

for (const [backend, create] of [
  ['in-memory', () => new internal.ZLinkInMemoryLocationStore()],
  ['Redis', async (t) => (await redisFixture(t))?.repository]
]) {
  test(`${backend} fences Entry claim removal and keeps the ID immutable on Renew`, async (t) => {
    const store = await create(t);
    if (store === undefined) return;
    const ownerA = await claimOwner(store, 'entry-owner-a');
    const ownerB = await claimOwner(store, 'entry-owner-b');
    const first = descriptor(ownerA, 1, entryA);
    const second = descriptor(ownerB, 2, entryA);
    assert.equal((await store.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
    assert.equal(
      (await store.updateMeshNode({ ...first, descriptorRevision: 2n }, Intent.Renew)).status,
      Status.Stored
    );
    assert.equal(
      (
        await store.updateMeshNode(
          { ...first, descriptorRevision: 3n, entrySpotId: entryB },
          Intent.Renew
        )
      ).status,
      Status.IgnoredStale
    );
    assert.equal(await store.removeMeshNode(first, ownerB), Status.IgnoredStale);
    assert.equal(
      (await store.updateMeshNode(second, Intent.NewClaim)).status,
      Status.RejectedConflict
    );
    assert.equal(await store.removeMeshNode(first, ownerA), Status.Stored);
    assert.equal((await store.updateMeshNode(second, Intent.NewClaim)).status, Status.Stored);
  });

  test(`${backend} fences owner cleanup against the replacement Entry claim`, async (t) => {
    const store = await create(t);
    if (store === undefined) return;
    const ownerA = await claimOwner(store, 'entry-owner-a');
    const ownerB = await claimOwner(store, 'entry-owner-b');
    const first = descriptor(ownerA, 1, entryA);
    const second = descriptor(ownerB, 2, entryA);
    assert.equal((await store.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
    assert.equal(await store.removeAllByOwner(ownerA), 0n);
    assert.equal((await store.listMeshNodes('mesh')).items.length, 1);
    assert.equal((await store.updateMeshNode(first, Intent.Renew)).status, Status.Stored);
    assert.equal((await store.updateMeshNode(second, Intent.NewClaim)).status, Status.Stored);
    assert.equal(await store.removeAllByOwner(ownerA), 0n);
    assert.equal(await store.removeMeshNode(first, ownerA), Status.Stored);
    assert.equal(
      (await store.updateMeshNode(descriptor(ownerA, 3, entryA), Intent.NewClaim)).status,
      Status.RejectedConflict
    );
  });

  test(`${backend} reserves Entry, User and Instance Spot IDs in one namespace`, async (t) => {
    const store = await create(t);
    if (store === undefined) return;
    const ownerA = await claimOwner(store, 'entry-owner-a');
    const ownerB = await claimOwner(store, 'entry-owner-b');
    const first = descriptor(ownerA, 1, entryA);
    first.populationCapacity.spots = { active: 0, reserved: 0, limit: 2 };
    assert.equal((await store.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
    for (const kind of ['user_spot', 'instance_spot']) {
      const reserve = (spotId) =>
        store.reserve({
          key: { kind, globalId: spotId },
          intent: {
            stableType: 'room',
            requestContentReference: 'entry-parity-request',
            requestSha256: Buffer.alloc(32, 1),
            requestEncodedSize: 1n
          },
          target: {
            meshName: first.meshName,
            nodeRid: first.rid,
            nodeLifecycleGeneration: first.lifecycleGeneration,
            owner: ownerA
          },
          creatingPayload: Buffer.of(1),
          capacity: { actors: 0, spots: 1 }
        });
      assert.equal((await reserve(entryA)).kind, 'conflict');
      const genericId = `generic-${kind}`;
      assert.equal((await reserve(genericId)).kind, 'reserved');
      assert.equal(
        (await store.updateMeshNode(descriptor(ownerB, 2, genericId), Intent.NewClaim)).status,
        Status.RejectedConflict
      );
    }
  });
}

test('Redis NewClaim replay repairs a pre-existing descriptor without rewriting its row', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const { repository, provider } = fixture;
  const owner = await claimOwner(repository, 'entry-owner-a');
  const first = descriptor(owner, 1, entryA);
  const descriptorKey = { value: ['mesh-node', first.meshName, first.rid.toHex()].join('\0') };
  const claimKey = { value: `entry-spot-id\0${entryA}` };
  assert.equal((await repository.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
  const original = await provider.read(descriptorKey);
  assert.equal(original.kind, 'found');
  const deleted = await provider.write({
    conditions: [],
    mutations: [{ kind: 'delete', key: claimKey }]
  });
  assert.equal(deleted.kind, 'applied');

  assert.equal((await repository.updateMeshNode(first, Intent.Renew)).status, Status.Stored);
  assert.equal((await provider.read(claimKey)).kind, 'missing');
  assert.equal((await repository.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
  const repaired = await provider.read(claimKey);
  assert.equal(repaired.kind, 'found');
  assert.equal(
    (await provider.read(descriptorKey)).value.version.value,
    original.value.version.value
  );

  assert.equal((await repository.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
  assert.equal((await provider.read(claimKey)).value.version.value, repaired.value.version.value);
});

test('Redis descriptor remove fences concurrent NewClaim repair', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const { repository, provider } = fixture;
  const owner = await claimOwner(repository, 'entry-owner-a');
  const first = descriptor(owner, 1, entryA);
  const claimKey = { value: `entry-spot-id\0${entryA}` };
  assert.equal((await repository.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
  assert.equal(
    (await provider.write({ conditions: [], mutations: [{ kind: 'delete', key: claimKey }] })).kind,
    'applied'
  );

  const removing = new internal.ZLinkLocationStoreRepository({
    read: (key, signal) => provider.read(key, signal),
    scan: (request, signal) => provider.scan(request, signal),
    async write(request, signal) {
      assert.equal((await repository.updateMeshNode(first, Intent.NewClaim)).status, Status.Stored);
      return provider.write(request, signal);
    }
  });
  assert.equal(await removing.removeMeshNode(first, owner), Status.IgnoredStale);
  assert.equal((await provider.read(claimKey)).kind, 'found');
  assert.equal(await repository.removeMeshNode(first, owner), Status.Stored);
  assert.equal((await provider.read(claimKey)).kind, 'missing');
});

async function redisFixture(t) {
  const client = await connectTestRedis(t);
  if (client === undefined) return undefined;
  const prefix = `zlink:entry-descriptor-parity:${randomUUID()}`;
  const provider = new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix: prefix });
  t.after(async () => {
    await provider.dispose();
    try {
      for await (const keys of client.scanIterator({ MATCH: `${prefix}:*` })) {
        if (keys.length > 0) await client.del(keys);
      }
    } finally {
      await client.quit();
    }
  });
  return { repository: new internal.ZLinkLocationStoreRepository(provider), provider };
}

async function publishSecond(store, id) {
  const ownerA = await claimOwner(store, 'entry-owner-a');
  const ownerB = await claimOwner(store, 'entry-owner-b');
  const first = await store.updateMeshNode(descriptor(ownerA, 1, entryA), Intent.NewClaim);
  assert.equal(first.status, Status.Stored);
  return (await store.updateMeshNode(descriptor(ownerB, 2, id), Intent.NewClaim)).status;
}

async function claimOwner(store, ownerId) {
  const claim = await store.claimOwnerLease(ownerId, 30_000);
  assert.equal(claim.kind, 'claimed');
  return claim.token;
}

function descriptor(owner, node, entrySpotId) {
  const emptyUsage = { active: 0, reserved: 0, limit: 0 };
  return {
    meshName: 'mesh',
    rid: RoutingId.from(Buffer.of(node)),
    lifecycleGeneration: BigInt(node),
    descriptorRevision: 1n,
    endpoint: `tcp://127.0.0.1:${7000 + node}`,
    channelWeights: { game: 100 },
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [],
    objectRole: framework.ZLinkObjectRole.Server,
    entrySpotId,
    placementWeight: 100,
    populationCapacity: { actors: emptyUsage, spots: emptyUsage, spotTypes: [] },
    activationConcurrency: { active: 0, limit: 128 },
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'security',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date()
  };
}

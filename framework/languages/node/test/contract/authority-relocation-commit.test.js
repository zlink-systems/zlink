const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkInMemoryAuthorityStore,
  ZLinkInMemoryProviderLocationStore,
  ZLinkLocationStoreRepository,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} = require('../../packages/framework/dist/internal');
const {
  aggregateDescriptor,
  aggregateRequest,
  createReadyUserSpot
} = require('./helpers/location-aggregate-fixture');

async function prepareRelocation(store, sourceOwner, targetOwner, requestCount = 2) {
  const source = {
    meshName: 'play',
    nodeRid: 'source-node',
    nodeLifecycleGeneration: 1n,
    owner: sourceOwner
  };
  const target = {
    meshName: source.meshName,
    nodeRid: 'target-node',
    nodeLifecycleGeneration: 2n,
    owner: targetOwner
  };
  if (!(store instanceof ZLinkInMemoryAuthorityStore)) {
    for (const node of [source, target]) {
      const result = await store.updateMeshNode(
        aggregateDescriptor(node, 2),
        ZLinkLocationWriteIntent.NewClaim
      );
      assert.equal(result.status, ZLinkLocationWriteStatus.Stored);
    }
  }
  const spotIds = ['alpha', 'beta'];
  const snapshots = [];
  for (const id of spotIds) snapshots.push(await createReadyUserSpot(store, id, source));
  const id = { value: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa' };
  const request = aggregateRequest(id, 1n, snapshots, target, spotIds);
  const nodeFence = (node) => ({
    nodeRid: Buffer.from(node.nodeRid).toString('hex'),
    nodeGeneration: node.nodeLifecycleGeneration,
    ownerId: node.owner.ownerId,
    ownerLeaseGeneration: node.owner.leaseGeneration
  });
  for (const participant of request.participants) {
    participant.relocation = {
      relocationId: id.value,
      phase: 'prepared',
      source: nodeFence(source),
      target: { ...nodeFence(target), attemptGeneration: 3n },
      coordinator: { ...nodeFence(source), expectedStoreVersion: snapshots[0].storeVersion.value },
      sourceInventorySha256: 'ab'.repeat(32),
      applicationVersion: 1n
    };
  }
  const prepared = await store.prepareAggregate(request);
  assert.equal(prepared.kind, 'prepared');
  const [first] = request.participants;
  const before = await store.readAuthority(first.authorityKey);
  assert.equal(before.ownerId, sourceOwner.ownerId);
  assert.equal(before.relocation.phase, 'prepared');
  return {
    participants: request.participants,
    targetOwner,
    fence: {
      ...prepared.fence,
      capturedRequests: Array.from({ length: requestCount }, (_, index) => ({
        relocationId: id.value,
        objectKey: request.participants[index % request.participants.length].authorityKey.value,
        source: {
          ownerId: 'requester',
          leaseGeneration: '8',
          nodeRid: '41',
          nodeGeneration: '11'
        },
        operationId: (index + 1).toString(16).padStart(32, '0'),
        replyRouteId: String(index + 1)
      }))
    }
  };
}

function assertCommitted(authority, participant, targetOwner) {
  assert.equal(authority.ownerId, targetOwner.ownerId);
  assert.equal(authority.ownerLeaseGeneration, targetOwner.leaseGeneration);
  assert.equal(authority.relocation.phase, 'committed');
  assert.deepEqual(authority.relocation, {
    ...participant.relocation,
    phase: 'committed'
  });
  assert.deepEqual(Buffer.from(authority.payload), participant.authorityPayload);
}

async function assertCommittedParticipants(store, prepared) {
  for (const participant of prepared.participants) {
    assertCommitted(
      await store.readAuthority(participant.authorityKey),
      participant,
      prepared.targetOwner
    );
  }
}

test('in-memory relocation publishes the new owner and committed record together', async () => {
  const store = new ZLinkInMemoryAuthorityStore({
    isOwnerLive: () => true,
    isTargetLive: () => true
  });
  const prepared = await prepareRelocation(
    store,
    { ownerId: 'source-owner', leaseGeneration: 1n },
    { ownerId: 'target-owner', leaseGeneration: 2n }
  );
  assert.equal((await store.commitAggregate(prepared.fence)).kind, 'committed');
  await assertCommittedParticipants(store, prepared);
});

test('provider relocation publishes the committed record before materialization', async () => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const write = provider.write.bind(provider);
  const store = new ZLinkLocationStoreRepository(provider);
  const source = await store.claimOwnerLease('source-owner', 60_000);
  const target = await store.claimOwnerLease('target-owner', 60_000);
  const prepared = await prepareRelocation(store, source.token, target.token);
  const read = provider.read.bind(provider);
  let pauseReader;
  let resumeReader;
  const paused = new Promise((resolve) => {
    pauseReader = resolve;
  });
  const published = new Promise((resolve) => {
    resumeReader = resolve;
  });
  provider.read = async (key, signal) => {
    if (key.value.startsWith('zlink:v11:aggregate:')) {
      provider.read = read;
      pauseReader();
      await published;
    }
    return read(key, signal);
  };
  const overlapping = store.readAuthority(prepared.participants[0].authorityKey);
  await paused;
  const atPublish = [];
  provider.write = async (request, signal) => {
    const result = await write(request, signal);
    if (
      result.kind === 'applied' &&
      request.mutations.some(
        (mutation) =>
          mutation.kind === 'put' &&
          mutation.key.value.startsWith('zlink:v11:aggregate:') &&
          JSON.parse(Buffer.from(mutation.bytes)).state === 'committed'
      )
    ) {
      resumeReader();
      for (const participant of prepared.participants) {
        atPublish.push(await store.readAuthority(participant.authorityKey));
      }
    }
    return result;
  };
  assert.equal((await store.commitAggregate(prepared.fence)).kind, 'committed');
  assert.equal(atPublish.length, prepared.participants.length);
  for (const [index, participant] of prepared.participants.entries()) {
    assertCommitted(atPublish[index], participant, prepared.targetOwner);
  }
  const beforePublish = await overlapping;
  assert.equal(beforePublish.ownerId, source.token.ownerId);
  assert.equal(beforePublish.relocation.phase, 'prepared');
  assert.equal(
    (await store.commitAggregate({ ...prepared.fence, capturedRequests: [] })).kind,
    'alreadyCommitted'
  );
  await assertCommittedParticipants(store, prepared);
});

for (const count of [0, 1, 9]) {
  test(`target commit writes ${count} accepted reply rows in the owner batch`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const store = new ZLinkLocationStoreRepository(provider);
    const source = await store.claimOwnerLease('source-owner', 60_000);
    const target = await store.claimOwnerLease('target-owner', 60_000);
    const prepared = await prepareRelocation(store, source.token, target.token, count);
    const relocationId = prepared.fence.aggregateId.value;
    const prefix = `$zlink/relocation-reply/${relocationId}/`;
    const original = provider.write.bind(provider);
    let committedBatches = 0;
    provider.write = async (batch, signal) => {
      if (
        batch.mutations.some(
          ({ key, bytes }) =>
            key.value.startsWith('zlink:v11:aggregate:') &&
            JSON.parse(Buffer.from(bytes)).state === 'committed'
        )
      ) {
        committedBatches++;
        const rows = batch.mutations.filter(({ key }) => key.value.startsWith(prefix));
        assert.equal(rows.length, count);
        rows.forEach(({ key, bytes, retentionMs }, index) => {
          assert.ok(
            batch.conditions.some((c) => c.kind === 'missing' && c.key.value === key.value)
          );
          assert.equal(retentionMs, 86_400_000);
          assert.deepEqual(JSON.parse(Buffer.from(bytes)), {
            ...prepared.fence.capturedRequests[index],
            formatVersion: 1,
            state: 'accepted',
            writer: {
              ownerId: target.token.ownerId,
              leaseGeneration: target.token.leaseGeneration.toString()
            }
          });
        });
      }
      return original(batch, signal);
    };
    assert.equal((await provider.scan({ prefix, limit: 100 })).value.items.length, 0);
    assert.equal((await store.commitAggregate(prepared.fence)).kind, 'committed');
    assert.equal(committedBatches, 1);
    assert.equal((await provider.scan({ prefix, limit: 100 })).value.items.length, count);
    await assertCommittedParticipants(store, prepared);
    assert.equal(
      (await store.commitAggregate({ ...prepared.fence, capturedRequests: [] })).kind,
      'alreadyCommitted'
    );
    assert.equal((await provider.scan({ prefix, limit: 100 })).value.items.length, count);
  });
}

for (const bound of ['keys', 'bytes']) {
  test(`reply rows that exceed the commit batch ${bound} leave owner and rows unchanged`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const store = new ZLinkLocationStoreRepository(provider);
    const source = await store.claimOwnerLease('source-owner', 60_000);
    const target = await store.claimOwnerLease('target-owner', 60_000);
    const prepared = await prepareRelocation(
      store,
      source.token,
      target.token,
      bound === 'keys' ? 2048 : 1700
    );
    if (bound === 'bytes') {
      for (const request of prepared.fence.capturedRequests) {
        request.source.ownerId = 'A'.repeat(250);
        request.source.nodeRid = '43'.repeat(250);
      }
    }
    const prefix = `$zlink/relocation-reply/${prepared.fence.aggregateId.value}/`;
    await assert.rejects(store.commitAggregate(prepared.fence), (error) => {
      assert.match(error.message, /Location Store batch limits/);
      assert.match(error.cause.message, bound === 'keys' ? /2,048/ : /4 MiB/);
      return true;
    });
    assert.equal((await provider.scan({ prefix, limit: 100 })).value.items.length, 0);
    for (const participant of prepared.participants) {
      const authority = await store.readAuthority(participant.authorityKey);
      assert.equal(authority.ownerId, source.token.ownerId);
      assert.equal(authority.relocation.phase, 'prepared');
    }
    assert.equal((await store.abortAggregate(prepared.fence)).kind, 'aborted');
  });
}

test('Redis exposes committed reply rows to a second repository and versions the terminal CAS', async (t) => {
  const { randomUUID } = require('node:crypto');
  const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');
  const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
  const client = await connectTestRedis(t);
  if (client === undefined) return;
  const keyPrefix = `relocation-reply-contract-${randomUUID()}`;
  const firstProvider = new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix });
  const secondProvider = new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix });
  try {
    const first = new ZLinkLocationStoreRepository(firstProvider);
    const second = new ZLinkLocationStoreRepository(secondProvider);
    const source = await first.claimOwnerLease('source-owner', 60_000);
    const writer = (await first.claimOwnerLease('target-owner', 60_000)).token;
    const prepared = await prepareRelocation(first, source.token, writer, 1);
    const [identity] = prepared.fence.capturedRequests;
    const row = async () => {
      const values = [];
      for await (const value of second.relocationReplies.scan(identity.relocationId)) {
        values.push(value);
      }
      assert.ok(values.length <= 1);
      return values[0];
    };
    assert.equal(await row(), undefined);
    assert.equal((await first.commitAggregate(prepared.fence)).kind, 'committed');
    assert.equal((await row()).state, 'accepted');
    const result = await second.relocationReplies.complete(identity, writer, 'YW5zd2Vy', () =>
      assert.fail()
    );
    assert.equal((await row()).reply, result.reply);
    await first.relocationReplies.acknowledge(identity, writer, 'alreadyTerminal');
    assert.deepEqual(await row(), { ...result, state: 'alreadyTerminal' });
  } finally {
    await firstProvider.dispose();
    await secondProvider.dispose();
    for await (const keys of client.scanIterator({ MATCH: `${keyPrefix}:*` })) {
      if (keys.length > 0) await client.del(keys);
    }
    await client.quit();
  }
});

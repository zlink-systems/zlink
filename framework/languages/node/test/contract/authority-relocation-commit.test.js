const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkInMemoryAuthorityStore,
  ZLinkInMemoryLocationStore,
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

async function prepareRelocation(store, sourceOwner, targetOwner, acceptedRequests = 2n) {
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
  const relocation = {
    relocationId: id.value,
    phase: 'prepared',
    source: nodeFence(source),
    target: { ...nodeFence(target), attemptGeneration: 3n },
    coordinator: { ...nodeFence(source), expectedStoreVersion: snapshots[0].storeVersion.value },
    sourceInventorySha256: 'ab'.repeat(32),
    applicationVersion: 1n
  };
  const [first, member] = request.participants;
  first.relocation = relocation;
  const prepared = await store.prepareAggregate(request);
  assert.equal(prepared.kind, 'prepared');
  assert.equal((await store.readAuthority(member.authorityKey)).relocation, undefined);
  const before = await store.readAuthority(first.authorityKey);
  assert.equal(before.ownerId, sourceOwner.ownerId);
  assert.equal(before.relocation.phase, 'prepared');
  assert.equal(before.relocation.completion, undefined);
  return {
    participants: request.participants,
    relocation,
    targetOwner,
    fence: {
      ...prepared.fence,
      acceptedRequests: new Map([[first.authorityKey.value, acceptedRequests]])
    }
  };
}

function assertCommitted(authority, participant, relocation, acceptedRequests, targetOwner) {
  assert.equal(authority.ownerId, targetOwner.ownerId);
  assert.equal(authority.ownerLeaseGeneration, targetOwner.leaseGeneration);
  assert.equal(authority.relocation.phase, 'committed');
  assert.deepEqual(authority.relocation, {
    ...relocation,
    phase: 'committed',
    completion: {
      acceptedRequests,
      completedResults: 0n,
      pendingDeliveries: 0n,
      reference: null,
      sha256: null
    }
  });
  assert.deepEqual(Buffer.from(authority.payload), participant.authorityPayload);
}

async function assertCommittedParticipants(store, prepared) {
  for (const participant of prepared.participants) {
    assertCommitted(
      await store.readAuthority(participant.authorityKey),
      participant,
      prepared.relocation,
      prepared.fence.acceptedRequests.get(participant.authorityKey.value) ?? 0n,
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
    assertCommitted(
      atPublish[index],
      participant,
      prepared.relocation,
      prepared.fence.acceptedRequests.get(participant.authorityKey.value) ?? 0n,
      prepared.targetOwner
    );
  }
  const beforePublish = await overlapping;
  assert.equal(beforePublish.ownerId, source.token.ownerId);
  assert.equal(beforePublish.relocation.phase, 'prepared');
  assert.equal(beforePublish.relocation.completion, undefined);
  assert.equal(
    (await store.commitAggregate({ ...prepared.fence, acceptedRequests: new Map() })).kind,
    'alreadyCommitted'
  );
  await assertCommittedParticipants(store, prepared);
});

for (const backing of ['repository', 'in-memory']) {
  test(`completion accepted count is installed atomically at target commit/${backing}`, async () => {
    const store =
      backing === 'repository'
        ? new ZLinkLocationStoreRepository(new ZLinkInMemoryProviderLocationStore())
        : new ZLinkInMemoryLocationStore();
    const sourceOwner = await store.claimOwnerLease(`completion-source-${backing}`, 60000);
    const targetOwner = await store.claimOwnerLease(`completion-target-${backing}`, 60000);
    assert.equal(sourceOwner.kind, 'claimed');
    assert.equal(targetOwner.kind, 'claimed');
    const prepared = await prepareRelocation(store, sourceOwner.token, targetOwner.token, 9n);
    const [participant] = prepared.participants;
    const key = participant.authorityKey;
    assert.equal((await store.commitAggregate(prepared.fence)).kind, 'committed');
    assertCommitted(
      await store.readAuthority(key),
      participant,
      prepared.relocation,
      9n,
      targetOwner.token
    );
    assert.equal(
      (
        await store.commitAggregate({
          ...prepared.fence,
          acceptedRequests: new Map([[key.value, 123n]])
        })
      ).kind,
      'alreadyCommitted'
    );
    assert.equal((await store.readAuthority(key)).relocation.completion.acceptedRequests, 9n);
  });
}

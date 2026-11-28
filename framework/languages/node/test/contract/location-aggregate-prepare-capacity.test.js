const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkFrameworkErrorKind,
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

for (const targetLimit of [2, 1]) {
  test(`aggregate prepare re-evaluates capacity after another reservation (limit ${targetLimit})`, async () => {
    const inner = new ZLinkInMemoryProviderLocationStore();
    let competingPrepare;
    let competingResult;
    let firstWriteResult;
    const repository = new ZLinkLocationStoreRepository({
      read: (key, signal) => inner.read(key, signal),
      scan: (request, signal) => inner.scan(request, signal),
      async write(request, signal) {
        const publishingPrepare = request.mutations.some(
          (mutation) =>
            mutation.kind === 'put' &&
            mutation.key.value.startsWith('zlink:v11:aggregate:') &&
            JSON.parse(Buffer.from(mutation.bytes).toString()).state === 'prepared'
        );
        if (competingPrepare !== undefined && publishingPrepare) {
          const second = competingPrepare;
          competingPrepare = undefined;
          competingResult = await second();
          firstWriteResult = await inner.write(request, signal);
          return firstWriteResult;
        }
        return inner.write(request, signal);
      }
    });
    const sourceOwner = await repository.claimOwnerLease('source-owner', 60_000);
    const targetOwner = await repository.claimOwnerLease('target-owner', 60_000);
    assert.equal(sourceOwner.kind, 'claimed');
    assert.equal(targetOwner.kind, 'claimed');
    const source = {
      meshName: 'play',
      nodeRid: 'source-node',
      nodeLifecycleGeneration: 1n,
      owner: sourceOwner.token
    };
    const target = {
      meshName: 'play',
      nodeRid: 'target-node',
      nodeLifecycleGeneration: 1n,
      owner: targetOwner.token
    };
    for (const [descriptor, limit] of [
      [source, 2],
      [target, targetLimit]
    ]) {
      assert.equal(
        (
          await repository.updateMeshNode(
            aggregateDescriptor(descriptor, limit),
            ZLinkLocationWriteIntent.NewClaim
          )
        ).status,
        ZLinkLocationWriteStatus.Stored
      );
    }
    const firstSpot = await createReadyUserSpot(repository, 'room-first', source);
    const secondSpot = await createReadyUserSpot(repository, 'room-second', source);
    const first = aggregateRequest(
      { value: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa' },
      1n,
      [firstSpot],
      target,
      ['room-first']
    );
    const second = aggregateRequest(
      { value: 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb' },
      1n,
      [secondSpot],
      target,
      ['room-second']
    );
    competingPrepare = () => repository.prepareAggregate(second);

    const result = await repository.prepareAggregate(first);

    assert.equal(firstWriteResult.kind, targetLimit === 2 ? 'applied' : 'conflict');
    assert.equal(competingResult.kind, 'prepared');
    assert.equal(result.kind, targetLimit === 2 ? 'prepared' : 'conflict');
    assert.equal((await repository.commitAggregate(competingResult.fence)).kind, 'committed');
    if (result.kind === 'prepared') {
      assert.equal((await repository.commitAggregate(result.fence)).kind, 'committed');
    }
    const firstAuthority = await repository.readAuthority(first.participants[0].authorityKey);
    assert.equal(
      firstAuthority.ownerId,
      targetLimit === 2 ? target.owner.ownerId : source.owner.ownerId
    );
  });
}

test('aggregate prepare reuses the persisted ID generation after a marker reply is lost', async () => {
  const store = new ZLinkInMemoryProviderLocationStore();
  let loseMarkerReply = false;
  const repository = new ZLinkLocationStoreRepository({
    read: store.read.bind(store),
    scan: store.scan.bind(store),
    async write(request, signal) {
      const result = await store.write(request, signal);
      if (
        loseMarkerReply &&
        request.mutations.some(
          (mutation) =>
            mutation.key.value === 'zlink:v12:authority-owner-counter\0spot\0room-lost-reply'
        )
      ) {
        assert.equal(result.kind, 'applied');
        loseMarkerReply = false;
        throw new Error('applied participant marker reply lost');
      }
      return result;
    }
  });
  async function target(nodeRid) {
    const lease = await repository.claimOwnerLease(nodeRid, 60_000);
    assert.equal(lease.kind, 'claimed');
    const value = {
      meshName: 'play',
      nodeRid,
      nodeLifecycleGeneration: 1n,
      owner: lease.token
    };
    const descriptor = await repository.updateMeshNode(
      aggregateDescriptor(value, 1),
      ZLinkLocationWriteIntent.NewClaim
    );
    assert.equal(descriptor.status, ZLinkLocationWriteStatus.Stored);
    return value;
  }
  const source = await target('source-node');
  const destination = await target('target-node');
  const spot = await createReadyUserSpot(repository, 'room-lost-reply', source);
  const request = aggregateRequest(
    { value: 'cccccccc-cccc-4ccc-8ccc-cccccccccccc' },
    1n,
    [spot],
    destination,
    ['room-lost-reply']
  );
  loseMarkerReply = true;
  await assert.rejects(
    repository.prepareAggregate(request),
    (error) =>
      error.kind === ZLinkFrameworkErrorKind.Unavailable &&
      error.cause?.message === 'applied participant marker reply lost'
  );
  const prepared = await repository.prepareAggregate(request);
  assert.equal(prepared.kind, 'prepared');
  assert.equal((await repository.commitAggregate(prepared.fence)).kind, 'committed');
  const moved = await repository.readAuthority(request.participants[0].authorityKey);
  assert.equal(moved.ownerId, destination.owner.ownerId);
  assert.equal(moved.objectGeneration, spot.objectGeneration);
  assert.equal(moved.authorityOwnerGeneration, spot.authorityOwnerGeneration + 1n);
  const counter = await store.read({
    value: 'zlink:v12:authority-owner-counter\0spot\0room-lost-reply'
  });
  assert.equal(counter.kind, 'found');
  assert.equal(Buffer.from(counter.value.bytes).toString(), '3');
});

test('source Preserve observes NewOwner if target commits before the pending capacity release', async (t) => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository(provider);
  const [sourceOwner, targetOwner] = await Promise.all([
    repository.claimOwnerLease('source-owner', 60_000),
    repository.claimOwnerLease('target-owner', 60_000)
  ]);
  assert.equal(sourceOwner.kind, 'claimed');
  assert.equal(targetOwner.kind, 'claimed');
  const source = {
    meshName: 'play',
    nodeRid: 'source',
    nodeLifecycleGeneration: 1n,
    owner: sourceOwner.token
  };
  const target = { ...source, nodeRid: 'target', owner: targetOwner.token };
  for (const descriptor of [source, target]) {
    const updated = await repository.updateMeshNode(
      aggregateDescriptor(descriptor, 1),
      ZLinkLocationWriteIntent.NewClaim
    );
    assert.equal(updated.status, ZLinkLocationWriteStatus.Stored);
  }
  const spot = await createReadyUserSpot(repository, 'room-abort', source);
  const request = aggregateRequest(
    { value: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa' },
    1n,
    [spot],
    target
  );
  const locationFence = (node) => ({
    nodeRid: Buffer.from(node.nodeRid).toString('hex'),
    nodeGeneration: node.nodeLifecycleGeneration,
    ownerId: node.owner.ownerId,
    ownerLeaseGeneration: node.owner.leaseGeneration
  });
  const participant = request.participants[0];
  participant.relocation = {
    relocationId: request.aggregateId.value,
    phase: 'prepared',
    source: locationFence(source),
    target: { ...locationFence(target), attemptGeneration: 1n },
    coordinator: { ...locationFence(source), expectedStoreVersion: spot.storeVersion.value },
    sourceInventorySha256: 'a'.repeat(64),
    applicationVersion: 1n
  };
  const prepared = await repository.prepareAggregate(request);
  assert.equal(prepared.kind, 'prepared');
  const current = await repository.readAuthority(participant.authorityKey);
  const preserve = {
    kind: 'put',
    generationTransition: 'preserve',
    payload: current.payload
  };
  const unrelated = await repository.compareExchangeAuthority(
    participant.authorityKey,
    current.storeVersion,
    { ...preserve, payload: Buffer.from('changed source payload') }
  );
  assert.equal(unrelated.kind, 'conflict');

  const rootKey = { value: `zlink:v11:aggregate:${request.aggregateId.value}:1` };
  const root = await provider.read(rootKey);
  assert.equal(root.kind, 'found');
  const rootJson = Buffer.from(root.value.bytes).toString();
  const rootRecord = JSON.parse(rootJson);
  assert.match(rootRecord.inventoryDigestSha256, /^[0-9a-f]{64}$/);
  const expectedRoot = {
    aggregateGeneration: '1',
    aggregateId: request.aggregateId.value,
    capacity: {
      actors: 0,
      spotType: { count: 1, objectKind: 'userSpot', stableType: 'lobby' },
      spots: 1
    },
    inventoryDigestSha256: rootRecord.inventoryDigestSha256,
    participantCount: 1,
    state: 'prepared',
    targetDescriptor: { meshName: 'play', routingIdHex: '746172676574' },
    targetDescriptorLifecycleGeneration: '1',
    targetOwner: {
      ownerId: target.owner.ownerId,
      ownerLeaseGeneration: String(target.owner.leaseGeneration)
    }
  };
  assert.equal(rootJson, JSON.stringify(expectedRoot));

  const write = provider.write.bind(provider);
  let targetCommit;
  t.mock.method(provider, 'write', async (batch, signal) => {
    if (
      targetCommit === undefined &&
      batch.mutations.some(
        (mutation) =>
          mutation.kind === 'add' &&
          mutation.key.value === 'zlink:v12:capacity\0play\x00746172676574\0spots\0pending'
      )
    ) {
      targetCommit = repository.commitAggregate(prepared.fence);
      assert.equal((await targetCommit).kind, 'committed');
    }
    return write(batch, signal);
  });
  const result = await repository.compareExchangeAuthority(
    participant.authorityKey,
    current.storeVersion,
    preserve
  );
  assert.ok(targetCommit, 'the target commit must race the source capacity release');
  assert.equal(result.kind, 'conflict');
  assert.equal(result.current.relocation.phase, 'committed');
  assert.equal(result.current.ownerId, target.owner.ownerId);
});

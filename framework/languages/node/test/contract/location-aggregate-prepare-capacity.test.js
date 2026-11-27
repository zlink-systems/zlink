const assert = require('node:assert/strict');
const test = require('node:test');
const {
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

    assert.equal(firstWriteResult.kind, 'conflict');
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

test('source Preserve observes NewOwner if target commits before the pending capacity read', async (t) => {
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

  const read = provider.read.bind(provider);
  let targetCommit;
  t.mock.method(provider, 'read', async (key, signal) => {
    if (targetCommit === undefined && key.value === 'zlink:v11:capacity:play:target') {
      targetCommit = repository.commitAggregate(prepared.fence);
      assert.equal((await targetCommit).kind, 'committed');
    }
    return read(key, signal);
  });
  const result = await repository.compareExchangeAuthority(
    participant.authorityKey,
    current.storeVersion,
    preserve
  );
  assert.ok(targetCommit, 'the target commit must race the source capacity read');
  assert.equal(result.kind, 'conflict');
  assert.equal(result.current.relocation.phase, 'committed');
  assert.equal(result.current.ownerId, target.owner.ownerId);
});

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ZLinkLocationStoreRepository,
  ZLinkLocationWriteIntent
} = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  aggregateKey
} = require('../../packages/framework/dist/runtime/locations/aggregate-inventory-store');
const {
  PROVIDER_STORAGE_NAMESPACE_PREFIX,
  ZLinkInMemoryProviderLocationStore,
  storeKey
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  aggregateDescriptor,
  aggregateRequest,
  createReadyUserSpot
} = require('./helpers/location-aggregate-fixture');

const aggregateId = '00000000-0000-4000-8000-000000005354';
const fence = { aggregateId: { value: aggregateId }, aggregateGeneration: 1n };
const prefix = `${PROVIDER_STORAGE_NAMESPACE_PREFIX}aggregate-inventory:${aggregateId}:1:`;
const rootKey = storeKey(`${prefix}root`);
const pageKey = storeKey(`${prefix}page:0:0`);
const rowKey = aggregateKey(fence);

async function fixture() {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository(provider);
  const placements = {};
  for (const nodeRid of ['source', 'target', 'other']) {
    const owner = await repository.claimOwnerLease(`${nodeRid}-owner`, 60_000);
    assert.equal(owner.kind, 'claimed');
    const placement = {
      meshName: 'mesh',
      nodeRid,
      nodeLifecycleGeneration: 1n,
      owner: owner.token
    };
    await repository.updateMeshNode(
      aggregateDescriptor(placement, 4),
      ZLinkLocationWriteIntent.NewClaim
    );
    placements[nodeRid] = placement;
  }
  const spotId = 'room-abort';
  const primary = encodeAuthorityKey('user_spot', spotId);
  const snapshot = await createReadyUserSpot(repository, spotId, placements.source);
  const request = aggregateRequest(fence.aggregateId, 1n, [snapshot], placements.target);
  const digest = await repository.storeAggregateInventory(request);
  const source = {
    nodeRid: Buffer.from('source').toString('hex'),
    nodeGeneration: 1n,
    ownerId: snapshot.ownerId,
    ownerLeaseGeneration: snapshot.ownerLeaseGeneration
  };
  const relocation = {
    relocationId: aggregateId,
    phase: 'captured',
    source,
    target: null,
    coordinator: { ...source, expectedStoreVersion: snapshot.storeVersion.value },
    sourceInventorySha256: digest,
    applicationVersion: 1n
  };
  const captured = await repository.compareExchangeAuthority(primary, snapshot.storeVersion, {
    kind: 'put',
    generationTransition: 'preserve',
    payload: snapshot.payload,
    relocation
  });
  assert.equal(captured.kind, 'stored');
  const target = (placement) => ({
    nodeRid: placement.nodeRid,
    nodeGeneration: placement.nodeLifecycleGeneration,
    ownerId: placement.owner.ownerId,
    ownerLeaseGeneration: placement.owner.leaseGeneration
  });
  const restore = {
    relocation: { high: 0x4000n, low: 0x8000000000005354n },
    object: { kind: 'userSpot', spotId },
    coordinator: {
      nodeRid: 'source',
      nodeGeneration: 1n,
      ownerId: snapshot.ownerId,
      leaseGeneration: snapshot.ownerLeaseGeneration,
      expectedAuthorityStoreVersion: snapshot.storeVersion.value
    },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 1n,
    applicationVersion: 1n,
    target: target(placements.target),
    targetAttemptGeneration: 1n,
    deadlineUnixMs: BigInt(Date.now() + 10_000)
  };
  const envelope = {
    aggregateId,
    aggregateGeneration: 1n,
    participants: [
      {
        participantId: 1n,
        rootSpotId: spotId,
        rootObjectKind: 'user_spot',
        rootSpotGeneration: snapshot.objectGeneration,
        rootOwnerGeneration: snapshot.authorityOwnerGeneration
      }
    ]
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    spotManager: () => ({}),
    locationStore: () => repository
  });
  async function prepare(command) {
    const resolved = await runtime.resolveTargetParticipantInventory('mesh', command, envelope);
    const { prepared } = await runtime.reserveTargetForPrepare(
      'mesh',
      command,
      resolved.envelope,
      resolved.entries,
      resolved.digest
    );
    return prepared.fence;
  }
  return {
    provider,
    repository,
    prepare,
    restore,
    primary,
    otherTarget: target(placements.other),
    relocation
  };
}

test('Prepared accepts a new attempt with one representative CAS and fences the old attempt', async () => {
  const { provider, repository, prepare, restore, primary } = await fixture();
  const first = await prepare(restore);
  const previous = await repository.readAuthority(primary);
  const frozen = await Promise.all([
    provider.read(rowKey),
    provider.read(rootKey),
    provider.read(pageKey)
  ]);
  const writes = [];
  const write = provider.write.bind(provider);
  provider.write = (request, signal) => {
    writes.push(request);
    return write(request, signal);
  };
  const second = await prepare({ ...restore, targetAttemptGeneration: 2n });
  assert.equal(writes.length, 1);
  assert.equal(writes[0].mutations.length, 1);
  assert.equal(writes[0].conditions[0].expected.value, previous.storeVersion.value);
  const retained = await Promise.all([
    provider.read(rowKey),
    provider.read(rootKey),
    provider.read(pageKey)
  ]);
  assert.deepEqual(
    retained.map(({ value }) => value.version),
    frozen.map(({ value }) => value.version)
  );
  assert.deepEqual(
    retained.map(({ value }) => value.bytes),
    frozen.map(({ value }) => value.bytes)
  );
  assert.equal((await repository.readAuthority(primary)).relocation.target.attemptGeneration, 2n);
  assert.equal((await repository.commitAggregate(first)).kind, 'stale');
  assert.equal((await repository.abortAggregate(first)).kind, 'stale');
  assert.equal((await repository.commitAggregate(second)).kind, 'committed');
  assert.equal((await repository.commitAggregate(first)).kind, 'stale');
  assert.equal((await repository.readAuthority(primary)).relocation.target.attemptGeneration, 2n);
});

test('An old commit and an authority read can overlap terminal inventory cleanup', async () => {
  const { provider, repository, prepare, restore, primary } = await fixture();
  const first = await prepare(restore);
  const second = await prepare({ ...restore, targetAttemptGeneration: 2n });
  const read = provider.read.bind(provider);
  let interruptPage = true;
  let heldRead;
  provider.read = async (key, signal) => {
    if (interruptPage && key.value === pageKey.value) {
      interruptPage = false;
      let release;
      const held = new Promise((resolve) => {
        release = resolve;
      });
      const withHeldAuthorityProjection = async () => {
        provider.read = async (key, signal) => {
          if (key.value === rowKey.value) {
            provider.read = read;
            release();
            await cleanup;
          }
          return read(key, signal);
        };
        let finishCleanup;
        const cleanup = new Promise((resolve) => {
          finishCleanup = resolve;
        });
        heldRead = repository.readAuthority(primary);
        try {
          await held;
          assert.equal((await repository.commitAggregate(second)).kind, 'committed');
          await repository.cleanupAggregateInventory(second);
        } finally {
          finishCleanup();
        }
      };
      await withHeldAuthorityProjection();
    }
    return read(key, signal);
  };
  assert.equal((await repository.commitAggregate(first)).kind, 'stale');
  const observed = await heldRead;
  assert.equal(observed.ownerId, restore.target.ownerId);
  assert.equal(observed.relocation.target.attemptGeneration, 2n);
});

test('Prepared rejects Restore from a different target with Conflict', async () => {
  const { prepare, restore, otherTarget } = await fixture();
  await prepare(restore);
  await assert.rejects(prepare({ ...restore, target: otherTarget, targetAttemptGeneration: 2n }), {
    kind: ZLinkFrameworkErrorKind.AlreadyExists,
    message: /prepared for a different target/
  });
});

for (const [record, key] of [
  ['root', rootKey],
  ['page', pageKey]
]) {
  test(`Prepared with a missing ${record} reports damaged inventory`, async () => {
    const { provider, repository, prepare, restore } = await fixture();
    const prepared = await prepare(restore);
    await provider.write({ conditions: [], mutations: [{ kind: 'delete', key }] });
    await assert.rejects(repository.commitAggregate(prepared), new RegExp(`missing.*${record}`));
    await assert.rejects(
      repository.cleanupAggregateInventory(prepared),
      new RegExp(`missing.*${record}`)
    );
    assert.equal((await provider.read(rowKey)).kind, 'found');
  });
}

for (const phase of ['committed', 'aborted']) {
  test(`Terminal ${phase} finishes after interruption between cleanup batches`, async () => {
    const { provider, repository, prepare, restore } = await fixture();
    const prepared = await prepare(restore);
    const settle = phase === 'committed' ? 'commitAggregate' : 'abortAggregate';
    assert.equal((await repository[settle](prepared)).kind, phase);
    const deleted = [];
    let failAtRoot = true;
    const write = provider.write.bind(provider);
    provider.write = async (request, signal) => {
      const keys = request.mutations
        .filter(({ kind }) => kind === 'delete')
        .map(({ key }) => key.value);
      if (failAtRoot && keys.includes(rootKey.value)) throw new Error('cleanup interrupted');
      const result = await write(request, signal);
      if (result.kind === 'applied') deleted.push(...keys);
      return result;
    };
    await assert.rejects(
      async () => repository.cleanupAggregateInventory(prepared),
      (error) => error.cause?.message === 'cleanup interrupted'
    );
    assert.equal((await provider.read(pageKey)).kind, 'missing');
    assert.equal((await provider.read(rootKey)).kind, 'found');
    assert.equal((await provider.read(rowKey)).kind, 'found');
    failAtRoot = false;
    await repository.cleanupAggregateInventory(prepared);
    assert(deleted.indexOf(pageKey.value) < deleted.indexOf(rootKey.value));
    for (const kind of ['authorityPayload', 'membershipMutation']) {
      const index = deleted.indexOf(`${prefix}participant:0:${kind}`);
      assert(index >= 0 && index < deleted.indexOf(rootKey.value));
    }
    assert(deleted.indexOf(rootKey.value) < deleted.indexOf(rowKey.value));
    assert.equal((await provider.read(rowKey)).kind, 'missing');
  });

  test(`Terminal ${phase} with missing root recognizes completed inventory cleanup`, async () => {
    const { provider, repository, prepare, restore } = await fixture();
    const prepared = await prepare(restore);
    const settle = phase === 'committed' ? 'commitAggregate' : 'abortAggregate';
    assert.equal((await repository[settle](prepared)).kind, phase);
    await provider.write({ conditions: [], mutations: [{ kind: 'delete', key: rootKey }] });
    const terminal = phase === 'committed' ? 'alreadyCommitted' : 'alreadyAborted';
    assert.equal((await repository[settle](prepared)).kind, terminal);
    await repository.cleanupAggregateInventory(prepared);
    assert.equal((await provider.read(rowKey)).kind, 'missing');
  });
}

test('Automatic Spot target selection observes an already sealed inventory', async () => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository(provider);
  const owner = await repository.claimOwnerLease('source-owner', 60_000);
  const source = {
    meshName: 'mesh',
    nodeRid: 'source',
    nodeLifecycleGeneration: 1n,
    owner: owner.token
  };
  await repository.updateMeshNode(
    aggregateDescriptor(source, 4),
    ZLinkLocationWriteIntent.NewClaim
  );
  const snapshot = await createReadyUserSpot(repository, 'room', source);
  const seal = repository.storeAggregateInventory.bind(repository);
  let selectedFence;
  repository.storeAggregateInventory = async (request, signal) => {
    const digest = await seal(request, signal);
    selectedFence = {
      aggregateId: request.aggregateId,
      aggregateGeneration: request.aggregateGeneration
    };
    return digest;
  };
  const stop = new Error('target descriptor lookup stopped');
  let sealedRootKey;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([
        ['mesh', { spotFactoryRegistrations: { lobby: { relocation: { kind: 'disabled' } } } }]
      ])
    },
    meshNode: () => ({ status: () => ({ routingId: 'source' }) }),
    locationStore: () => repository,
    async liveDescriptors() {
      assert.ok(selectedFence, 'inventory must be sealed before target descriptor lookup');
      sealedRootKey = storeKey(
        `${PROVIDER_STORAGE_NAMESPACE_PREFIX}aggregate-inventory:${selectedFence.aggregateId.value}:1:root`
      );
      const root = await provider.read(sealedRootKey);
      assert.equal(root.kind, 'found');
      const digest = JSON.parse(Buffer.from(root.value.bytes)).digestSha256;
      const inventory = await repository.readAggregateInventory(selectedFence, digest);
      assert.deepEqual(Buffer.from(inventory[0].authorityPayload), Buffer.from(snapshot.payload));
      assert.deepEqual(Buffer.from(inventory[0].membershipMutation), Buffer.from('[]'));
      throw stop;
    }
  });
  await assert.rejects(
    runtime.relocatePerActorSpotShell('mesh', {
      spotId: 'room',
      serial: { executeControlLifecycleOperation: (action) => action() }
    }),
    (error) => error === stop
  );
  assert.equal((await provider.read(sealedRootKey)).kind, 'missing');
});

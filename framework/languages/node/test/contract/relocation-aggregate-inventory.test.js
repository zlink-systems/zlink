const assert = require('node:assert/strict');
const { test } = require('node:test');
const golden = require('../../../../runtime/protocol/golden/aggregate-storage-v1.json');
const schema = require('../../../../runtime/protocol/service-wire-v1.schema.json');
const {
  ZLinkAggregateInventoryStore
} = require('../../packages/framework/dist/runtime/locations/aggregate-inventory-store');
const {
  PROVIDER_STORAGE_NAMESPACE_PREFIX,
  ZLinkInMemoryProviderLocationStore,
  storeKey
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');

const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

const {
  ZLinkHostServiceRelocationRuntime,
  relocationFailedFailureCode
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeServiceRelocationEnvelope
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

function providerKey(goldenKey) {
  assert(goldenKey.startsWith('golden:'));
  return PROVIDER_STORAGE_NAMESPACE_PREFIX + goldenKey.slice('golden:'.length);
}

function fixtureInventory(vector) {
  const { aggregateId, aggregateGeneration } = vector.aggregate?.decoded ?? {
    aggregateId: vector.relocationId,
    aggregateGeneration: vector.aggregateGeneration
  };
  const fence = {
    aggregateId: { value: aggregateId },
    aggregateGeneration: BigInt(aggregateGeneration)
  };
  const records = new Map(
    [vector.root, ...vector.pages].map(({ key, jcsHex }) => [
      providerKey(key),
      Buffer.from(jcsHex, 'hex')
    ])
  );
  const { authorityPayloadHex, membershipMutationHex } = vector.participantHashInputs;
  const authorityPayload = Buffer.from(authorityPayloadHex, 'hex');
  const membershipMutation = Buffer.from(membershipMutationHex, 'hex');
  const entries = vector.pages.flatMap(({ decoded }) => decoded.entries);
  const participants = entries.map((entry) => {
    for (const [kind, bytes] of Object.entries({ authorityPayload, membershipMutation })) {
      const key = schema.jsonStorageFormats.keys[kind].replace(
        /\{([^}]+)\}/g,
        (_, name) =>
          ({ prefix: 'golden:', aggregateId, aggregateGeneration, index: entry.index })[name]
      );
      records.set(providerKey(key), bytes);
    }
    return {
      authorityKey: { value: entry.authorityKey },
      expectedStoreVersion: { value: entry.expectedStoreVersion },
      ownerTransition: entry.ownerTransition,
      authorityPayload,
      membershipMutation
    };
  });
  return { fence, records, entries, participants, digest: vector.root.decoded.digestSha256 };
}

async function loadRecords(provider, records) {
  for (const [key, bytes] of records) {
    await provider.write({
      conditions: [],
      mutations: [{ kind: 'put', key: storeKey(key), bytes }]
    });
  }
}

for (const vector of golden.cases) {
  test(`§8.3 inventory round trip: ${vector.name}`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const source = new ZLinkAggregateInventoryStore(provider);
    const { fence, records, entries, participants, digest } = fixtureInventory(vector);
    if (vector === golden.directJoin) {
      assert.equal(fence.aggregateId.value, vector.relocationId);
      assert.equal(fence.aggregateGeneration, 1n);
    }
    const storedDigest = await source.store({
      ...fence,
      participants,
      inventoryDigest: Buffer.from(digest, 'hex')
    });
    for (const [key, bytes] of records) {
      const record = await provider.read(storeKey(key));
      assert.equal(record.kind, 'found', key);
      assert.deepEqual(Buffer.from(record.value.bytes), bytes, key);
    }
    assert.equal(storedDigest, digest);
    const target = new ZLinkAggregateInventoryStore(provider);
    assert.deepEqual(
      await target.read(fence, Buffer.from(digest, 'hex')),
      entries.map((entry, index) => ({
        ...entry,
        authorityPayload: participants[index].authorityPayload,
        membershipMutation: participants[index].membershipMutation
      }))
    );
  });
}

test('§8.3 membership mutation size limits apply to the stored body', async () => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const inventory = new ZLinkAggregateInventoryStore(provider);
  const { fence, participants } = fixtureInventory(golden.cases[0]);
  const maximum = schema.bounds.find(({ name }) => name === 'maintenanceAggregateBytes').value;
  for (const size of [0, maximum + 1]) {
    const participant = { ...participants[0], membershipMutation: Buffer.alloc(size) };
    await assert.rejects(
      inventory.store({ ...fence, participants: [participant] }),
      /membership mutation length is invalid/
    );
  }

  const boundaryParticipants = [1, maximum].map((size, index) => ({
    ...participants[index],
    membershipMutation: Buffer.alloc(size, 0xa5)
  }));
  const digest = await inventory.store({ ...fence, participants: boundaryParticipants });
  const restored = await inventory.read(fence, Buffer.from(digest, 'hex'));
  assert.deepEqual(
    restored.map(({ membershipMutation }) => membershipMutation),
    boundaryParticipants.map(({ membershipMutation }) => membershipMutation)
  );
});

for (const damaged of [false, true]) {
  test(`Node target Restore attempt 2 ${damaged ? 'rejects damaged' : 'restores'} Node source inventory`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const store = new ZLinkAggregateInventoryStore(provider);
    const key = encodeAuthorityKey('user_spot', 'room');
    const memberKey = encodeAuthorityKey('actor', 'player');
    const actorState = { actorId: 'player', actorType: 'Player', actor: {}, meshName: 'mesh' };
    const target = {
      rid: 'target',
      lifecycleGeneration: 6n,
      ownerId: 'target-owner',
      leaseGeneration: 4n,
      applicationVersion: 1n
    };
    const primary = {
      kind: 'snapshot',
      allocation: { objectKind: 'user_spot', stableType: 'Room' },
      objectGeneration: 5n,
      authorityOwnerGeneration: 11n,
      storeVersion: { value: 'v1' },
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n,
      payload: Buffer.from([0, 255])
    };
    const member = {
      ...primary,
      allocation: { objectKind: 'actor', stableType: 'Player' },
      objectGeneration: 7n,
      authorityOwnerGeneration: 13n,
      storeVersion: { value: 'sealed-member' },
      payload: Buffer.from([42])
    };
    const phases = [];
    const rows = new Map([
      [key.value, primary],
      [memberKey.value, member]
    ]);
    const source = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          [
            'mesh',
            {
              spotFactoryRegistrations: { Room: { relocation: { kind: 'recreate' } } },
              actorFactoryRegistrations: { Player: { relocation: { kind: 'recreate' } } }
            }
          ]
        ])
      },
      spotManager: () => ({
        pendingSpotCloseDecision: () => undefined,
        isSpotClosing: () => false
      }),
      meshNode: () => ({ status: () => ({ routingId: 'source', lifecycleGeneration: 2n }) }),
      actorTransfer: {
        async prepareMaintenanceSession() {
          return { handoffBacklog: [] };
        }
      },
      locationStore: () => ({
        async readAuthority(authorityKey) {
          return rows.get(authorityKey.value);
        },
        async compareExchangeAuthority(authorityKey, expected, mutation) {
          const current = rows.get(authorityKey.value);
          assert.equal(expected.value, current.storeVersion.value);
          phases.push({ key: authorityKey.value, relocation: mutation.relocation });
          rows.set(authorityKey.value, {
            ...current,
            storeVersion: { value: `v${phases.length + 1}` },
            relocation: mutation.relocation
          });
          return { kind: 'stored' };
        },
        storeAggregateInventory: (request, signal) => store.store(request, signal)
      })
    });
    let captured;
    source.waitForRelocationBudgetHeadroom = async () => {};
    source.runCoordinator = async (_mesh, _target, _authority, sealed) => {
      captured = sealed;
    };
    await source.relocateSpotAggregate(
      'mesh',
      {
        spotId: 'room',
        spot: {},
        serial: { executeControlLifecycleOperation: (operation) => operation() },
        async captureRelocation() {
          return { timers: [] };
        }
      },
      'user_spot',
      [actorState],
      target
    );
    const id = captured.envelope.aggregateId;
    const hexId = id.replaceAll('-', '');
    const expected = phases[0].relocation.sourceInventorySha256;
    assert.deepEqual(
      phases.map(({ key, relocation }) => [key, relocation.phase]),
      [
        [key.value, 'preparing'],
        [key.value, 'captured']
      ]
    );
    assert.equal(phases[1].relocation.sourceInventorySha256, expected);
    assert.deepEqual(rows.get(memberKey.value).storeVersion, member.storeVersion);
    const inventoryFence = { aggregateId: { value: id }, aggregateGeneration: 1n };
    const rootKey = storeKey(`zlink:v11:aggregate-inventory:${id}:1:root`);
    const root = await provider.read(rootKey);
    assert.equal(root.kind, 'found');
    assert.equal(JSON.parse(Buffer.from(root.value.bytes)).digestSha256, expected);
    if (damaged) {
      await loadRecords(
        provider,
        new Map([
          [
            `zlink:v11:aggregate-inventory:${id}:1:participant:0:authorityPayload`,
            Buffer.from([43])
          ]
        ])
      );
    }

    const targetStore = new ZLinkAggregateInventoryStore(provider);
    const receiver = new ZLinkHostServiceRelocationRuntime({
      meshNode: () => ({ status: () => ({ routingId: 'target', lifecycleGeneration: 6n }) }),
      currentOwner: () => ({ ownerId: 'target-owner', leaseGeneration: 4n }),
      spotManager: () => undefined,
      locationStore: () => ({
        async readAuthority(authorityKey) {
          return rows.get(authorityKey.value);
        },
        readAggregateInventory: (requestedFence, digest, signal) => {
          assert.deepEqual(requestedFence, inventoryFence);
          assert.equal(digest, expected);
          return targetStore.read(requestedFence, Buffer.from(digest, 'hex'), signal);
        }
      })
    });
    receiver.targetAssemblies.set('attempt', {
      assembly: { payload: async () => encodeServiceRelocationEnvelope(captured.envelope, 1n) }
    });
    const prepare = {
      kind: 'prepare',
      relocation: {
        high: BigInt(`0x${hexId.slice(0, 16)}`),
        low: BigInt(`0x${hexId.slice(16)}`)
      },
      targetAttemptGeneration: 2n,
      coordinator: {
        ownerId: 'source-owner',
        leaseGeneration: 3n,
        nodeRid: 'source',
        nodeGeneration: 2n,
        expectedAuthorityStoreVersion: 'v1'
      },
      target: {
        ownerId: 'target-owner',
        ownerLeaseGeneration: 4n,
        nodeRid: 'target',
        nodeGeneration: 6n
      },
      object: {
        kind: 'userSpot',
        spotId: 'room',
        objectGeneration: 5n,
        expectedAuthorityOwnerGeneration: 11n
      },
      initiatorRole: 'source',
      sourceNodeRid: 'source',
      sourceNodeGeneration: 2n,
      applicationVersion: 1n,
      deadlineUnixMs: BigInt(Date.now() + 10_000)
    };
    const events = [];
    receiver.materializeTargetEnvelope = async (_mesh, envelope) => {
      events.push('materialized');
      assert.deepEqual(
        envelope.participants.map(({ key }) => key),
        [memberKey.value, key.value]
      );
      return {
        owner: {
          async restoreSavedWork() {
            events.push('restored');
          }
        },
        staging: {},
        target: {}
      };
    };
    receiver.reserveTargetForPrepare = async (_mesh, _request, _envelope, entries, digest) => {
      assert.equal(digest, expected);
      const spotEntry = entries.find((entry) => entry.authorityKey === key.value);
      const memberEntry = entries.find((entry) => entry.authorityKey === memberKey.value);
      assert.equal(spotEntry.expectedStoreVersion, 'v1');
      assert.deepEqual(Buffer.from(spotEntry.authorityPayload), primary.payload);
      assert.equal(memberEntry.expectedStoreVersion, member.storeVersion.value);
      events.push('prepared');
      return { prepared: {} };
    };
    const result = receiver.handlePrepareControl('mesh', 'attempt', prepare, 'source');
    if (damaged) {
      await assert.rejects(result, (error) => {
        assert.equal(relocationFailedFailureCode(error), 35);
        return /inventory participant checksum/.test(error.message);
      });
      assert.deepEqual(events, []);
    } else {
      assert.equal((await result).kind, 'ready');
      assert.deepEqual(events, ['materialized', 'restored', 'prepared']);
    }
  });
}

for (const staleMember of [false, true]) {
  test(`SpotWide sealed inventory ${staleMember ? 'rejects a changed member' : 'accepts a representative-only capture'}`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    await spotWideMatrix(provider, staleMember, true);
    await spotWideMatrix(provider, staleMember, false);
  });
}

async function spotWideMatrix(provider, staleMember, write) {
  const framework = require('../../packages/framework/dist');
  const {
    ZLinkLocationStoreRepository,
    ZLinkLocationWriteIntent
  } = require('../../packages/framework/dist/internal');
  const {
    encodeAuthorityKey
  } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
  const {
    encodeActorAuthorityIdentity
  } = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
  const {
    ZLinkHostServiceRelocationRuntime,
    ServiceRelocationDataLostError
  } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
  const {
    aggregateDescriptor,
    createReadyUserSpot
  } = require('./helpers/location-aggregate-fixture');
  const repository = new ZLinkLocationStoreRepository(provider);
  const aggregateId = '00000000-0000-4000-8000-000000005354';
  const spotId = 'matrix-room';
  const actorId = 'matrix-actor';
  const spotKey = encodeAuthorityKey('user_spot', spotId);
  const actorKey = encodeAuthorityKey('actor', actorId);
  const source = { meshName: 'matrix', nodeRid: 'source', nodeLifecycleGeneration: 1n };
  if (write) {
    const claimed = await repository.claimOwnerLease('source-owner', 60_000);
    assert.equal(claimed.kind, 'claimed');
    const target = { ...source, owner: claimed.token };
    const descriptor = aggregateDescriptor(target, 4);
    descriptor.populationCapacity.actors.limit = 4;
    descriptor.objectCapabilities.push({
      objectKind: 'actor',
      stableType: 'player',
      policy: 'recreate',
      hasSnapshotAdapter: false,
      limit: 4
    });
    await repository.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim);
    const spot = await createReadyUserSpot(repository, spotId, target);
    const key = { kind: 'actor', globalId: actorId };
    const reserved = await repository.reserve({
      key,
      intent: {
        stableType: 'player',
        requestContentReference: 'matrix',
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      creatingPayload: Buffer.from('creating'),
      capacity: { actors: 1, spots: 0 }
    });
    assert.equal(reserved.kind, 'reserved');
    const readyPayload = encodeActorAuthorityIdentity({
      actor: { actorId, nodeRid: source.nodeRid },
      actorType: 'player',
      spotId,
      spotKind: framework.ZLinkSpotKind.User,
      spotGeneration: spot.objectGeneration,
      owner: target.owner,
      meshName: source.meshName,
      ownerNodeGeneration: 1n
    });
    const committed = await repository.commit({
      key,
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      readyPayload
    });
    assert.equal(committed.kind, 'committed');
    const actor = committed.ready;
    const digest = await repository.storeAggregateInventory({
      aggregateId: { value: aggregateId },
      aggregateGeneration: 1n,
      participants: [
        [actorKey, actor],
        [spotKey, spot]
      ].map(([authorityKey, snapshot]) => ({
        authorityKey,
        expectedStoreVersion: snapshot.storeVersion,
        ownerTransition: 'newOwner',
        authorityPayload: snapshot.payload,
        membershipMutation: Buffer.from('[]')
      }))
    });
    const sourceFence = {
      nodeRid: Buffer.from(source.nodeRid).toString('hex'),
      nodeGeneration: 1n,
      ownerId: spot.ownerId,
      ownerLeaseGeneration: spot.ownerLeaseGeneration
    };
    const captured = await repository.compareExchangeAuthority(spotKey, spot.storeVersion, {
      kind: 'put',
      generationTransition: 'preserve',
      payload: spot.payload,
      relocation: {
        relocationId: aggregateId,
        phase: 'captured',
        source: sourceFence,
        target: null,
        coordinator: { ...sourceFence, expectedStoreVersion: spot.storeVersion.value },
        sourceInventorySha256: digest,
        applicationVersion: 1n
      }
    });
    assert.equal(captured.kind, 'stored');
    assert.equal(
      (await repository.readAuthority(actorKey)).storeVersion.value,
      actor.storeVersion.value
    );
    if (staleMember) {
      const changed = await repository.compareExchangeAuthority(actorKey, actor.storeVersion, {
        kind: 'put',
        generationTransition: 'preserve',
        payload: actor.payload
      });
      assert.equal(changed.kind, 'stored');
      assert.notEqual(changed.storeVersion.value, actor.storeVersion.value);
    }
    return;
  }
  const spot = await repository.readAuthority(spotKey);
  assert.equal(spot.kind, 'snapshot');
  const runtime = new ZLinkHostServiceRelocationRuntime({
    spotManager: () => ({}),
    locationStore: () => repository
  });
  const resolved = runtime.resolveTargetParticipantInventory(
    source.meshName,
    {
      relocation: { high: 0x4000n, low: 0x8000000000005354n },
      object: { kind: 'userSpot', spotId }
    },
    {
      aggregateId,
      aggregateGeneration: 1n,
      participants: [
        { participantId: 1n },
        {
          participantId: 2n,
          rootSpotId: spotId,
          rootObjectKind: 'user_spot',
          rootSpotGeneration: spot.objectGeneration,
          rootOwnerGeneration: spot.authorityOwnerGeneration
        }
      ]
    }
  );
  if (staleMember) {
    await assert.rejects(resolved, ServiceRelocationDataLostError);
  } else {
    const { envelope } = await resolved;
    assert.deepEqual(
      envelope.participants.map(({ key }) => key),
      [actorKey.value, spotKey.value]
    );
  }
}

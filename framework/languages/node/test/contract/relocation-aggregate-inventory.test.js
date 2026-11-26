const assert = require('node:assert/strict');
const { test } = require('node:test');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const golden = require('../../../../runtime/protocol/golden/aggregate-storage-v1.json');
const schema = require('../../../../runtime/protocol/service-wire-v1.schema.json');
const {
  aggregateKey,
  ZLinkAggregateInventoryStore
} = require('../../packages/framework/dist/runtime/locations/aggregate-inventory-store');
const {
  PROVIDER_STORAGE_NAMESPACE_PREFIX,
  ZLinkInMemoryProviderLocationStore,
  storeKey
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');

const {
  decodeAggregateRecord,
  encodeAggregateRecord
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');

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

for (const vector of [...golden.cases, golden.directJoin]) {
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
    if (vector.aggregate) {
      const expected = Buffer.from(vector.aggregate.jcsHex, 'hex');
      const record = decodeAggregateRecord(fence, expected);
      assert.equal(aggregateKey(fence).value, providerKey(vector.aggregate.key));
      assert.deepEqual(encodeAggregateRecord(fence, record), expected);
      assert.equal(record.inventoryDigestSha256, storedDigest);
    }
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

test('§8.3 aggregate records reject noncanonical bytes and different identities', () => {
  const vector = golden.cases[0];
  const { fence } = fixtureInventory(vector);
  const record = vector.aggregate.decoded;
  const bytes = Buffer.from(vector.aggregate.jcsHex, 'hex');
  assert.throws(() => decodeAggregateRecord({ ...fence, aggregateGeneration: 1n }, bytes), /§8.3/);
  assert.throws(
    () => decodeAggregateRecord(fence, Buffer.concat([bytes, Buffer.from(' ')])),
    /§8.3/
  );
  assert.throws(
    () => decodeAggregateRecord(fence, Buffer.from(JSON.stringify({ ...record, version: 1 }))),
    /§8.3/
  );
});

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

const rejectedInventoryErrors = {
  'missing-page': /inventory.*missing.*:page:/i,
  'missing-participant': /inventory.*missing.*:membershipMutation/i,
  'damaged-participant': /inventory participant checksum/i,
  'damaged-page': /inventory page checksum/i,
  'reordered-leaf-entries': /inventory.*order/i,
  'reordered-page-index': /inventory.*order/i
};

for (const vector of golden.rejected) {
  test(`§8.3 rejects ${vector.name}`, async () => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const base = golden.cases.find(({ name }) => name === vector.base);
    const { fence, records, digest } = fixtureInventory(base);
    for (const key of vector.removeKeys ?? []) assert(records.delete(providerKey(key)));
    for (const { key, hex } of vector.replaceRecords ?? [])
      records.set(providerKey(key), Buffer.from(hex, 'hex'));
    await loadRecords(provider, records);
    const target = new ZLinkAggregateInventoryStore(provider);
    await assert.rejects(
      target.read(fence, Buffer.from(vector.rootDigestSha256 ?? digest, 'hex')),
      rejectedInventoryErrors[vector.name]
    );
  });
}

const {
  ZLinkHostServiceRelocationRuntime,
  relocationFailedFailureCode
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  encodeServiceRelocationEnvelope
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

for (const phase of ['captured', 'damaged', 'prepared']) {
  const damaged = phase === 'damaged';
  const description = {
    captured: 'restores Node source inventory',
    damaged: 'rejects damaged Node source inventory',
    prepared: 'keeps replacement staging after the old Ready expires'
  }[phase];
  test(`Node target Restore attempt 2 ${description}`, async (t) => {
    const provider = new ZLinkInMemoryProviderLocationStore();
    const store = new ZLinkAggregateInventoryStore(provider);
    const key = encodeAuthorityKey('user_spot', 'room');
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
    const phases = [];
    let current = primary;
    const source = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          ['mesh', { spotFactoryRegistrations: { Room: { relocation: { kind: 'recreate' } } } }]
        ])
      },
      spotManager: () => ({
        pendingSpotCloseDecision: () => undefined,
        isSpotClosing: () => false
      }),
      meshNode: () => ({ status: () => ({ routingId: 'source', lifecycleGeneration: 2n }) }),
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          phases.push(mutation.relocation);
          current = {
            ...primary,
            storeVersion: { value: `v${phases.length + 1}` },
            relocation: mutation.relocation
          };
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
      [],
      target
    );
    const id = captured.envelope.aggregateId;
    const hexId = id.replaceAll('-', '');
    const expected = phases[0].sourceInventorySha256;
    assert.deepEqual(
      phases.map(({ phase }) => phase),
      ['preparing', 'captured']
    );
    assert.equal(phases[1].sourceInventorySha256, expected);
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
            Buffer.from([0, 254])
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
        async readAuthority() {
          return current;
        },
        readAggregateInventory: (requestedFence, digest, signal) => {
          assert.deepEqual(requestedFence, inventoryFence);
          assert.equal(digest, expected);
          return targetStore.read(requestedFence, Buffer.from(digest, 'hex'), signal);
        }
      })
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
    const attemptId = `${prepare.relocation.high}:${prepare.relocation.low}:2`;
    const oldId = `${prepare.relocation.high}:${prepare.relocation.low}:1`;
    receiver.targetAssemblies.set(attemptId, {
      assembly: { payload: async () => encodeServiceRelocationEnvelope(captured.envelope, 1n) }
    });
    const handler = {};
    const queue = [Buffer.from([5])];
    const staging = { handler, queue };
    let expireOldReady;
    if (phase === 'prepared') {
      current = {
        ...current,
        storeVersion: { value: 'v4' },
        relocation: {
          ...current.relocation,
          phase: 'prepared',
          target: {
            ...prepare.target,
            nodeRid: Buffer.from(prepare.target.nodeRid).toString('hex'),
            attemptGeneration: 1n
          }
        }
      };
      receiver.targetStages.set(oldId, {
        phase: 'ready',
        offer: { prepareFingerprint: 'previous request' },
        lane: Promise.resolve(),
        releaseActivation() {},
        releaseJoinAdmission() {},
        owner: {
          async abort() {
            events.push('discarded-previous-stage');
          }
        },
        staging: { primaryAuthorityKey: key }
      });
      const timer = t.mock.method(globalThis, 'setTimeout', (callback) => {
        expireOldReady = callback;
        return { unref() {} };
      });
      try {
        const first = { ...prepare, targetAttemptGeneration: 1n };
        await receiver.deliverTargetReady(
          'mesh',
          oldId,
          first,
          {
            reply() {
              return SubmitResult.NotConnected;
            }
          },
          Promise.resolve({ ...first, kind: 'ready', senderRole: 'target' })
        );
      } finally {
        timer.mock.restore();
      }
    }
    receiver.materializeTargetEnvelope = async (_mesh, envelope) => {
      events.push('materialized');
      assert.equal(envelope.participants[0].key, key.value);
      return {
        owner: {
          async restoreSavedWork() {
            events.push('restored');
          },
          async abort() {
            assert.fail('the old Ready expiry must not abort the replacement');
          }
        },
        staging,
        target: {}
      };
    };
    receiver.reserveTargetForPrepare = async (
      _mesh,
      _request,
      _envelope,
      entries,
      digest,
      _signal,
      priorVersion
    ) => {
      assert.equal(priorVersion?.value, phase === 'prepared' ? 'v4' : undefined);
      assert.equal(digest, expected);
      assert.equal(entries[0].expectedStoreVersion, 'v1');
      assert.deepEqual(Buffer.from(entries[0].authorityPayload), primary.payload);
      events.push('prepared');
      return { prepared: {} };
    };
    const result = receiver.handlePrepareControl('mesh', attemptId, prepare, 'source');
    if (damaged) {
      await assert.rejects(result, (error) => {
        assert.equal(relocationFailedFailureCode(error), 35);
        return /inventory participant checksum/.test(error.message);
      });
      assert.deepEqual(events, []);
    } else {
      assert.equal((await result).kind, 'ready');
      assert.deepEqual(events, [
        ...(phase === 'prepared' ? ['discarded-previous-stage'] : []),
        'materialized',
        'restored',
        'prepared'
      ]);
      if (phase === 'prepared') {
        expireOldReady();
        assert.equal(receiver.targetStages.has(oldId), false);
        const replacement = receiver.targetStages.get(attemptId);
        assert.equal(replacement.phase, 'ready');
        assert.equal(replacement.staging, staging);
        assert.equal(replacement.staging.handler, handler);
        assert.deepEqual(replacement.staging.queue, [Buffer.from([5])]);
      }
    }
  });
}

const matrixMode = process.env.ZLINK_INVENTORY_MATRIX_MODE;
if (
  matrixMode === 'spotwide-write' ||
  matrixMode === 'spotwide-read' ||
  (!matrixMode && process.env.ZLINK_REDIS_TEST_ENDPOINT)
) {
  const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
  const { testRedisUrl } = require('./helpers/test-redis');
  for (const staleMember of [false, true]) {
    test(`Redis inventory matrix: SpotWide ${staleMember ? 'stale member' : 'root phase only'}`, async () => {
      const prefix =
        process.env.ZLINK_INVENTORY_MATRIX_PREFIX ??
        `node-aggregate:${require('node:crypto').randomUUID()}`;
      const provider = new ZLinkRedisLocationStore({
        url: testRedisUrl,
        keyPrefix: `${prefix}:spotwide-${staleMember}`
      });
      try {
        if (matrixMode !== 'spotwide-read') await spotWideMatrix(provider, staleMember, 'write');
        if (matrixMode !== 'spotwide-write') await spotWideMatrix(provider, staleMember, 'read');
      } finally {
        await provider.dispose();
      }
    });
  }
} else if (matrixMode) {
  const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
  const { testRedisUrl } = require('./helpers/test-redis');
  const accepted = [
    golden.cases.find(({ name }) => name === 'small-3'),
    golden.cases.find(({ name }) => name === 'two-level-1025'),
    golden.directJoin
  ];
  const vectors = matrixMode === 'mutate' ? golden.rejected : [...accepted, ...golden.rejected];
  for (const vector of vectors) {
    test(`Redis inventory matrix: ${vector.name}`, async () => {
      const prefix = process.env.ZLINK_INVENTORY_MATRIX_PREFIX;
      assert(prefix);
      assert(['write', 'mutate', 'read'].includes(matrixMode));
      const provider = new ZLinkRedisLocationStore({
        url: testRedisUrl,
        keyPrefix: `${prefix}:${vector.name}`
      });
      try {
        const base = vector.base ? accepted.find(({ name }) => name === vector.base) : vector;
        const { fence, records, entries, participants, digest } = fixtureInventory(base);
        const inventory = new ZLinkAggregateInventoryStore(provider);
        if (matrixMode === 'write') {
          assert.equal(await inventory.store({ ...fence, participants }), digest);
          for (const [key, bytes] of records) {
            const record = await provider.read(storeKey(key));
            assert.equal(record.kind, 'found', key);
            assert.deepEqual(Buffer.from(record.value.bytes), bytes, key);
          }
        } else if (matrixMode === 'mutate') {
          const keys = [
            ...[...records.keys()].filter((key) => key.endsWith(':root')),
            ...(vector.removeKeys ?? []).map(providerKey),
            ...(vector.replaceRecords ?? []).map(({ key }) => providerKey(key))
          ];
          for (const key of keys) {
            const current = await provider.read(storeKey(key));
            assert.equal(current.kind, 'found', key);
            assert.deepEqual(Buffer.from(current.value.bytes), records.get(key), key);
          }
          const reply = await provider.write({
            conditions: [],
            mutations: [
              ...(vector.removeKeys ?? []).map((key) => ({
                kind: 'delete',
                key: storeKey(providerKey(key))
              })),
              ...(vector.replaceRecords ?? []).map(({ key, hex }) => ({
                kind: 'put',
                key: storeKey(providerKey(key)),
                bytes: Buffer.from(hex, 'hex')
              }))
            ]
          });
          assert.equal(reply.kind, 'applied');
        } else {
          const read = inventory.read(fence, Buffer.from(vector.rootDigestSha256 ?? digest, 'hex'));
          if (vector.base) {
            await assert.rejects(read, rejectedInventoryErrors[vector.name]);
          } else {
            const restored = await read;
            assert.equal(restored.length, entries.length);
            for (const [
              index,
              { authorityPayload, membershipMutation, ...entry }
            ] of restored.entries()) {
              assert.deepEqual(entry, entries[index]);
              assert.deepEqual(Buffer.from(authorityPayload), participants[index].authorityPayload);
              assert.deepEqual(
                Buffer.from(membershipMutation),
                participants[index].membershipMutation
              );
            }
          }
        }
      } finally {
        await provider.dispose();
      }
    });
  }
}

async function spotWideMatrix(provider, staleMember, mode) {
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
  async function publishPlacement(nodeRid) {
    const claimed = await repository.claimOwnerLease(`${nodeRid}-owner`, 60_000);
    assert.equal(claimed.kind, 'claimed');
    const placement = { ...source, nodeRid, owner: claimed.token };
    const descriptor = aggregateDescriptor(placement, 4);
    descriptor.populationCapacity.actors.limit = 4;
    descriptor.objectCapabilities.push({
      objectKind: 'actor',
      stableType: 'player',
      policy: 'recreate',
      hasSnapshotAdapter: false,
      limit: 4
    });
    await repository.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim);
    return placement;
  }
  if (mode === 'write') {
    const target = await publishPlacement(source.nodeRid);
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
    if (!matrixMode) await publishPlacement('target');
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
    const { envelope, entries, digest } = await resolved;
    const actor = await repository.readAuthority(actorKey);
    assert.equal(spot.relocation.phase, 'captured');
    assert.equal(spot.relocation.sourceInventorySha256, digest);
    assert.equal(actor.relocation, undefined);
    assert.equal(entries[0].expectedStoreVersion, actor.storeVersion.value);
    assert.deepEqual(Buffer.from(entries[0].authorityPayload), Buffer.from(actor.payload));
    assert.equal(Buffer.from(entries[0].authorityPayload).includes('target-owner'), false);
    assert.equal(entries[1].expectedStoreVersion, spot.relocation.coordinator.expectedStoreVersion);
    assert.deepEqual(
      envelope.participants.map(({ key }) => key),
      [actorKey.value, spotKey.value]
    );
    if (!matrixMode) {
      const { items } = await repository.listMeshNodes(source.meshName);
      const descriptor = items.find(({ rid }) => rid === 'target');
      assert.ok(descriptor);
      const relocation = spot.relocation;
      const prepare = {
        sourceNodeRid: source.nodeRid,
        sourceNodeGeneration: source.nodeLifecycleGeneration,
        coordinator: {
          nodeRid: source.nodeRid,
          nodeGeneration: source.nodeLifecycleGeneration,
          ownerId: relocation.source.ownerId,
          leaseGeneration: relocation.source.ownerLeaseGeneration,
          expectedAuthorityStoreVersion: relocation.coordinator.expectedStoreVersion
        },
        target: {
          nodeRid: descriptor.rid,
          nodeGeneration: descriptor.lifecycleGeneration,
          ownerId: descriptor.ownerId,
          ownerLeaseGeneration: descriptor.leaseGeneration
        },
        targetAttemptGeneration: 1n,
        applicationVersion: 1n,
        deadlineUnixMs: BigInt(Date.now() + 10_000)
      };
      const { prepared } = await runtime.reserveTargetForPrepare(
        source.meshName,
        prepare,
        envelope,
        entries,
        digest
      );
      assert.equal(
        prepared.plan.participants[0].expected.storeVersion.value,
        entries[0].expectedStoreVersion
      );
      const memberBefore = await repository.readAuthority(actorKey);
      assert.equal(memberBefore.relocation, undefined);
      const replacement = await runtime.reserveTargetForPrepare(
        source.meshName,
        { ...prepare, targetAttemptGeneration: 2n },
        envelope,
        entries,
        digest
      );
      assert.equal(
        (await repository.readAuthority(actorKey)).storeVersion.value,
        memberBefore.storeVersion.value
      );
      assert.equal((await repository.commitAggregate(prepared.fence)).kind, 'stale');
      assert.equal((await repository.abortAggregate(prepared.fence)).kind, 'stale');
      const committed = await repository.commitAggregate(replacement.prepared.fence);
      assert.equal(committed.kind, 'committed');
      const {
        decodeActorAuthorityIdentity
      } = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
      const committedActor = await repository.readAuthority(actorKey);
      const route = decodeActorAuthorityIdentity(
        committedActor.payload,
        committedActor.objectGeneration
      );
      assert.equal(route.actor.nodeRid, descriptor.rid);
      assert.equal(route.owner.ownerId, descriptor.ownerId);
      assert.equal(route.spotId, spotId);
      for (const key of [actorKey, spotKey]) {
        const authority = await repository.readAuthority(key);
        assert.equal(authority.ownerId, descriptor.ownerId);
        assert.equal(authority.relocation.phase, 'committed');
        assert.equal(authority.relocation.target.attemptGeneration, 2n);
      }
    }
  }
}

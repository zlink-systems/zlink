const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { RoutingId } = require('@zlink-systems/zlink');
const {
  ZLinkInMemoryProviderLocationStore,
  ZLinkLocationStoreRepository,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} = require('../../packages/framework/dist/internal');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { aggregateDescriptor } = require('./helpers/location-aggregate-fixture');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');
const {
  distinctActorCreations: fixture
} = require('../../../../runtime/conformance/location-contention-v1.json');

async function freshStore(t, backend) {
  if (backend === 'memory') return { store: new ZLinkInMemoryProviderLocationStore() };
  const client = await connectTestRedis(t);
  if (client === undefined) return undefined;
  const keyPrefix = `zlink:contention:${randomUUID()}`;
  let evals = 0;
  const store = new ZLinkRedisLocationStore({
    url: testRedisUrl,
    keyPrefix,
    client: {
      get isOpen() {
        return client.isOpen;
      },
      get isReady() {
        return client.isReady;
      },
      async sendCommand(args, options) {
        const result = await client.sendCommand(args, options);
        // Count successful script executions, including the EVAL after a cache miss.
        if (args[0] === 'EVAL' || args[0] === 'EVALSHA') evals += 1;
        return result;
      }
    }
  });
  t.after(async () => {
    try {
      await store.dispose();
      const keys = await client.keys(`${keyPrefix}:*`);
      if (keys.length) await client.del(keys);
    } finally {
      await client.disconnect();
    }
  });
  return { store, evals: () => evals };
}

async function setup(repository, actorLimit) {
  const lease = await repository.claimOwnerLease('contention-owner', 60_000);
  assert.equal(lease.kind, 'claimed');
  const target = {
    meshName: fixture.mesh,
    nodeRid: RoutingId.from('contention-node'),
    nodeLifecycleGeneration: 1n,
    owner: lease.token
  };
  const descriptor = aggregateDescriptor(target, 0);
  descriptor.populationCapacity.actors.limit = actorLimit;
  descriptor.objectCapabilities = [
    {
      objectKind: 'actor',
      stableType: fixture.actorType,
      policy: 'disabled',
      hasSnapshotAdapter: false,
      limit: actorLimit
    }
  ];
  assert.equal(
    (await repository.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim)).status,
    ZLinkLocationWriteStatus.Stored
  );
  return target;
}

function actorRequest(target, index) {
  const globalId = fixture.actorIdPrefix + index;
  return {
    key: { kind: 'actor', globalId },
    intent: {
      stableType: fixture.actorType,
      requestContentReference: `request:${globalId}`,
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 1n
    },
    target,
    capacity: { actors: 1, spots: 0 },
    creatingPayload: Buffer.from(globalId)
  };
}

function transition(request, reservation) {
  return {
    key: request.key,
    target: request.target,
    reservationId: reservation.reservationId,
    expectedStoreVersion: reservation.creating.storeVersion.value
  };
}

function capacityKey(state) {
  return {
    value: `zlink:v12:capacity\0${fixture.mesh}\0${Buffer.from('contention-node').toString('hex')}\0actors\0${state}`
  };
}

async function counter(store, key) {
  const read = await store.read(key);
  return read.kind === 'missing' ? '0' : Buffer.from(read.value.bytes).toString();
}

for (const backend of ['memory', 'redis']) {
  for (const [scenario, { actorLimit, expectedReservationConditions }] of Object.entries(
    fixture.cases
  )) {
    test(`fresh ${backend} Store creates ${fixture.actorCount} distinct Actors (${scenario})`, async (t) => {
      const fresh = await freshStore(t, backend);
      if (fresh === undefined) return;
      const { store, evals } = fresh;
      const counts = { reads: 0, floorReads: 0, writes: 0, conflicts: 0, sumAtMost: 0 };
      const firstReserves = new Set();
      let releaseFirstReserves;
      const barrier = new Promise((resolve) => {
        releaseFirstReserves = resolve;
      });
      let measuring = false;
      const repository = new ZLinkLocationStoreRepository({
        async read(key, signal) {
          if (measuring) {
            if (key.value === 'zlink:v12:generation-floor') counts.floorReads += 1;
            else counts.reads += 1;
          }
          return store.read(key, signal);
        },
        scan: (request, signal) => store.scan(request, signal),
        async write(request, signal) {
          const authority = request.mutations.find((m) => m.key.value.startsWith('authority\0'));
          if (measuring && authority) {
            counts.writes += 1;
            counts.sumAtMost += request.conditions.filter((c) => c.kind === 'sumAtMost').length;
            if (
              backend === 'memory' &&
              firstReserves.size < 2 &&
              request.conditions.some(
                (c) => c.kind === 'missing' && c.key.value === authority.key.value
              )
            ) {
              firstReserves.add(authority.key.value);
              if (firstReserves.size === 2) releaseFirstReserves();
              await barrier;
            }
          }
          const result = await store.write(request, signal);
          if (measuring && authority && result.kind === 'conflict') counts.conflicts += 1;
          return result;
        }
      });
      const target = await setup(repository, actorLimit);
      const initialEvals = evals?.();
      const start = Promise.resolve();
      const deadline = BigInt(Date.now() + 30_000);
      measuring = true;
      const jobs = Array.from({ length: fixture.actorCount }, async (_, index) => {
        await start;
        const request = actorRequest(target, index);
        const reserved = await repository.reserve(request, undefined, deadline);
        assert.equal(reserved.kind, 'reserved');
        const completed = await repository.completeCreation({
          ...transition(request, reserved),
          completion: {
            kind: 'created',
            readyPayload: Buffer.from('ready'),
            terminal: {
              operation: {
                sourceNodeRid: target.nodeRid,
                sourceNodeGeneration: target.nodeLifecycleGeneration,
                operationId: { high: BigInt(index + 1), low: 1n }
              },
              terminalEnvelope: Buffer.from('terminal'),
              operationDeadline: new Date(Number(deadline))
            }
          }
        });
        assert.equal(completed.kind, 'created');
        return reserved.creating.objectGeneration;
      });
      const generations = await Promise.all(jobs);
      measuring = false;
      const redisEvals = initialEvals === undefined ? undefined : evals() - initialEvals;
      t.diagnostic(JSON.stringify({ created: generations.length, ...counts, redisEvals }));
      if (redisEvals !== undefined)
        assert.equal(redisEvals, counts.reads + counts.floorReads + counts.writes);
      assert.equal(generations.length, fixture.expected.createdActors);
      assert.equal(counts.conflicts, fixture.expected.writeConflicts);
      assert.equal(counts.writes, fixture.actorCount * fixture.expected.authorityWritesPerActor);
      assert.equal(counts.sumAtMost, expectedReservationConditions);
      assert.equal(counts.floorReads, 1);
      assert.ok(counts.reads <= 1800, `Store reads: ${counts.reads}`);
      for (const generation of generations)
        assert.equal(generation, BigInt(fixture.expected.objectGeneration));
      assert.equal(
        await counter(store, capacityKey('active')),
        String(fixture.expected.activeActorSlots)
      );
      assert.equal(
        await counter(store, capacityKey('pending')),
        String(fixture.expected.pendingActorSlots)
      );
      if (actorLimit !== 0) {
        const overflow = await repository.reserve(
          actorRequest(target, fixture.actorCount),
          undefined,
          deadline
        );
        assert.equal(overflow.kind, 'placementCapacityExhausted');
        const overflowId = fixture.actorIdPrefix + fixture.actorCount;
        for (const value of [
          `authority\0actor\0${overflowId}`,
          `zlink:v12:object-counter\0actor\0${overflowId}`,
          `zlink:v12:authority-owner-counter\0actor\0${overflowId}`
        ])
          assert.equal((await store.read({ value })).kind, 'missing');
        assert.equal(
          await counter(store, capacityKey('active')),
          String(fixture.expected.activeActorSlots)
        );
        assert.equal(await counter(store, capacityKey('pending')), '0');
      }
    });
  }
}

for (const backend of ['memory', 'redis']) {
  test(`fresh ${backend} Store keeps per-ID generations and fences duplicate or corrupt returns`, async (t) => {
    const fresh = await freshStore(t, backend);
    if (fresh === undefined) return;
    const { store } = fresh;
    const {
      encodeAuthorityKey
    } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
    const floor = { value: 'zlink:v12:generation-floor' };
    assert.equal(
      (
        await store.write({
          conditions: [{ kind: 'missing', key: floor }],
          mutations: [{ kind: 'put', key: floor, bytes: Buffer.from('17') }]
        })
      ).kind,
      'applied'
    );
    const repository = new ZLinkLocationStoreRepository(store);
    const target = await setup(repository, 1);
    const request = actorRequest(target, 0);
    const key = encodeAuthorityKey(request.key.kind, request.key.globalId);
    const reservation = await repository.reserve(request);
    assert.equal(reservation.kind, 'reserved');
    assert.equal(reservation.creating.objectGeneration, 17n);
    assert.equal(reservation.creating.authorityOwnerGeneration, 17n);
    const abort = transition(request, reservation);
    const aborts = await Promise.all([repository.abort(abort), repository.abort(abort)]);
    assert.deepEqual(aborts.map((result) => result.kind).sort(), ['aborted', 'stale']);
    assert.equal(await counter(store, capacityKey('pending')), '0');

    const next = await repository.reserve(request);
    assert.equal(next.kind, 'reserved');
    assert.equal(next.creating.objectGeneration, 18n);
    assert.equal(next.creating.authorityOwnerGeneration, 18n);
    const ready = await repository.commit({
      ...transition(request, next),
      readyPayload: Buffer.from('ready')
    });
    assert.equal(ready.kind, 'committed');
    assert.equal(
      (await repository.compareExchangeAuthority(key, ready.ready.storeVersion, { kind: 'delete' }))
        .kind,
      'deleted'
    );
    const recreated = await repository.reserve(request);
    assert.equal(recreated.kind, 'reserved');
    assert.equal(recreated.creating.objectGeneration, 19n);
    assert.equal(recreated.creating.authorityOwnerGeneration, 19n);

    const pending = capacityKey('pending');
    await store.write({
      conditions: [],
      mutations: [{ kind: 'put', key: pending, bytes: Buffer.from('0') }]
    });
    const operation = {
      sourceNodeRid: target.nodeRid,
      sourceNodeGeneration: target.nodeLifecycleGeneration,
      operationId: { high: 1n, low: 1n }
    };
    let providerResult;
    const observed = new ZLinkLocationStoreRepository({
      read: store.read.bind(store),
      scan: store.scan.bind(store),
      async write(batch, signal) {
        providerResult = await store.write(batch, signal);
        return providerResult;
      }
    });
    await assert.rejects(
      observed.completeCreation({
        ...transition(request, recreated),
        completion: {
          kind: 'created',
          readyPayload: Buffer.from('ready'),
          terminal: {
            operation,
            terminalEnvelope: Buffer.from('terminal'),
            operationDeadline: new Date(Date.now() + 30_000)
          }
        }
      }),
      { kind: require('../../packages/framework/dist').ZLinkFrameworkErrorKind.DataLost }
    );
    assert.equal(providerResult.kind, 'corrupt');
    assert.deepEqual(providerResult.putVersions, []);
    assert.equal(
      (await observed.readAuthority(key)).storeVersion.value,
      recreated.creating.storeVersion.value
    );
    assert.equal((await observed.readCreationTerminal(operation)).kind, 'missing');
    assert.equal(await counter(store, pending), '0');
    assert.equal(await counter(store, capacityKey('active')), '0');
  });
}

test('a lower descriptor limit takes effect on new reservations and leaves existing reservations committable', async () => {
  const store = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository(store);
  const target = await setup(repository, 2);
  const requests = [actorRequest(target, 0), actorRequest(target, 1)];
  const reservations = await Promise.all(requests.map((request) => repository.reserve(request)));
  for (const reservation of reservations) assert.equal(reservation.kind, 'reserved');

  const [descriptor] = (await repository.listMeshNodes(target.meshName)).items;
  const result = await repository.updateMeshNode(
    {
      ...descriptor,
      descriptorRevision: descriptor.descriptorRevision + 1n,
      populationCapacity: {
        ...descriptor.populationCapacity,
        actors: { ...descriptor.populationCapacity.actors, limit: 1 }
      }
    },
    ZLinkLocationWriteIntent.Renew
  );
  assert.equal(result.status, ZLinkLocationWriteStatus.Stored);
  for (const [index, reservation] of reservations.entries()) {
    const completion = await repository.commit({
      ...transition(requests[index], reservation),
      readyPayload: Buffer.from('ready')
    });
    assert.equal(completion.kind, 'committed');
  }
  assert.equal(
    (await repository.reserve(actorRequest(target, 2))).kind,
    'placementCapacityExhausted'
  );
  assert.equal(await counter(store, capacityKey('active')), '2');
  assert.equal(await counter(store, capacityKey('pending')), '0');
});

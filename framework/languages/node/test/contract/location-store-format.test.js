const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { RoutingId } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { createMeshHost } = require('./fixtures/mesh-host');
const {
  aggregateDescriptor,
  createReadyUserSpot
} = require('./helpers/location-aggregate-fixture');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');
const { ownerLeaseClaimMutation } = require('./helpers/owner-lease-claim-mutation');
const {
  concurrentStartupConversion: fixture,
  v11ConversionCases
} = require('../../../../runtime/conformance/location-contention-v1.json');

const FORMAT = 'zlink:v12:format';
const FLOOR = 'zlink:v12:generation-floor';
const LEGACY_CAPACITY = 'zlink:v11:capacity:';
const legacyKey = (rid) => LEGACY_CAPACITY + encodeURIComponent(fixture.mesh) + ':' + rid;
const key = (value) => ({ value });

function usage(actors = 0) {
  return { actors, spots: 0, spotTypes: {} };
}

function legacyRow(active, pending) {
  return JSON.stringify({ active: usage(active), pending: usage(pending) });
}

async function put(store, value, content) {
  const result = await store.write({
    conditions: [],
    mutations: [{ kind: 'put', key: key(value), bytes: Buffer.from(content) }]
  });
  assert.equal(result.kind, 'applied');
}

async function read(store, value) {
  const result = await store.read(key(value));
  return result.kind === 'found' ? Buffer.from(result.value.bytes).toString() : undefined;
}

function counter(ridHex, dimension, state) {
  return `zlink:v12:capacity\0${fixture.mesh}\0${ridHex}\0${dimension}\0${state}`;
}

function createRuntime(provider, mesh = fixture.mesh) {
  const { host } = createMeshHost({
    registration: framework.createFrameworkRegistration({ locations: { storeInstance: provider } })
  });
  return host.locationOwner.ensureRuntime(mesh);
}

function startup(t, provider, index, signal, mesh) {
  const runtime = createRuntime(provider, mesh);
  t.after(() => runtime.stop());
  return runtime.start(`conversion-${index}`, signal);
}

async function seedDescriptor(store, ridHex, mesh = fixture.mesh) {
  const repository = new framework.ZLinkLocationStoreRepository(store);
  const lease = await repository.claimOwnerLease(randomUUID(), 60_000);
  assert.equal(lease.kind, 'claimed');
  const target = {
    meshName: mesh,
    nodeRid: RoutingId.fromHex(ridHex),
    nodeLifecycleGeneration: 1n,
    owner: lease.token
  };
  assert.equal(
    (
      await repository.updateMeshNode(
        aggregateDescriptor(target, 0),
        framework.ZLinkLocationWriteIntent.NewClaim
      )
    ).status,
    framework.ZLinkLocationWriteStatus.Stored
  );
  return { repository, target };
}

function intercept(store, write, scan = store.scan.bind(store)) {
  return { read: store.read.bind(store), write, scan };
}

for (const [name, { descriptorRoutingIdHexes, rows, expected }] of Object.entries(
  v11ConversionCases.cases
)) {
  test(`shared v11 conversion fixture: ${name}`, async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    for (const ridHex of descriptorRoutingIdHexes) {
      await seedDescriptor(store, ridHex, v11ConversionCases.mesh);
    }
    for (const { key, value } of rows) await put(store, key, value);

    const starting = startup(t, store, 0, undefined, v11ConversionCases.mesh);
    if (expected.startup === 'configurationError') {
      await assert.rejects(starting, { name: 'ZLinkConfigurationException' });
      assert.equal(await read(store, FORMAT), undefined);
      for (const { key, value } of rows) assert.equal(await read(store, key), value, key);
      return;
    }

    assert.equal(expected.startup, 'converted');
    await starting;
    assert.equal(await read(store, FORMAT), 'converted');
    for (const { key } of rows) assert.equal(await read(store, key), undefined, key);
    for (const [key, value] of Object.entries(expected.counters)) {
      assert.equal(await read(store, key), value, key);
    }
    for (const key of expected.absentCounters) {
      assert.equal(await read(store, key), undefined, key);
    }
  });
}

for (const backend of ['memory', 'redis']) {
  test(`${backend} startup converts the shared v11 fixture before the first owner lease write`, async (t) => {
    let store;
    if (backend === 'redis') {
      const client = await connectTestRedis(t);
      if (!client) return;
      const keyPrefix = `zlink:format:${randomUUID()}`;
      store = new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix });
      t.after(async () => {
        try {
          await store.dispose();
          const keys = await client.keys(`${keyPrefix}:*`);
          if (keys.length) await client.del(keys);
        } finally {
          await client.disconnect();
        }
      });
    } else {
      store = new framework.ZLinkInMemoryProviderLocationStore();
    }

    await put(store, 'zlink:v11:object-counter', String(fixture.legacyNextObjectGeneration));
    await put(
      store,
      'zlink:v11:authority-owner-counter',
      String(fixture.legacyNextAuthorityOwnerGeneration)
    );
    for (const record of fixture.records) {
      if (record.descriptorRoutingIdHex !== null)
        await seedDescriptor(store, record.descriptorRoutingIdHex);
      await put(
        store,
        legacyKey(encodeURIComponent(record.legacyRid)),
        legacyRow(record.activeActorSlots, record.pendingActorSlots)
      );
    }

    let writesBeforeConversion = 0;
    const provider = intercept(store, async (request, signal) => {
      const lease = ownerLeaseClaimMutation(request);
      if (lease && (await read(store, FORMAT)) !== fixture.expected.format)
        writesBeforeConversion++;
      return store.write(request, signal);
    });
    const runtimes = Array.from({ length: fixture.startupCount }, () => createRuntime(provider));
    try {
      await Promise.all(runtimes.map((runtime, index) => runtime.start(`conversion-${index}`)));
      assert.equal(writesBeforeConversion, fixture.expected.writesBeforeConversion);
      assert.equal(await read(store, FLOOR), String(fixture.expected.generationFloor));
      assert.equal(await read(store, FORMAT), fixture.expected.format);
      const page = await store.scan({ prefix: LEGACY_CAPACITY, limit: 1000 });
      assert.equal(page.value.items.length, fixture.expected.remainingLegacyRecords);
      for (const record of fixture.records) {
        for (const [state, count] of [
          ['active', record.activeActorSlots],
          ['pending', record.pendingActorSlots]
        ]) {
          assert.equal(
            await read(store, counter(record.expectedRoutingIdHex, 'actors', state)),
            count === 0 ? undefined : String(count)
          );
        }
      }
    } finally {
      await Promise.all(runtimes.map((runtime) => runtime.stop()));
    }
  });
}

test('startup matches a raw binary RID and preserves a short hex display as literal', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  await seedDescriptor(store, '80fe');
  await put(store, legacyKey('%80%FE'), legacyRow(2, 0));
  await put(store, legacyKey('hex%3A81'), legacyRow(0, 3));
  await startup(t, store, 0);
  assert.equal(await read(store, counter('80fe', 'actors', 'active')), '2');
  assert.equal(await read(store, counter('6865783a3831', 'actors', 'pending')), '3');
});

test('startup rejects a RID matching distinct raw and display descriptors', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  await seedDescriptor(store, '31');
  await seedDescriptor(store, '00000001');
  const oldKey = legacyKey('1');
  await put(store, oldKey, legacyRow(2, 4));
  await assert.rejects(startup(t, store, 0), {
    name: 'ZLinkConfigurationException',
    message: /ambiguous/i
  });
  assert.equal(await read(store, FORMAT), undefined);
  assert.equal(await read(store, oldKey), legacyRow(2, 4));
});

test('startup converts flat usage and preserves signed-64-bit counts exactly', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  await put(
    store,
    legacyKey('flat'),
    '{"actorsActive":9007199254740993,"actorsPending":0,"spotsActive":3,"spotsPending":4,"spotTypes":{"2:lobby":{"active":2,"pending":1},"3:stage:arena":{"active":0,"pending":9223372036854775807}}}'
  );
  await startup(t, store, 0);
  const rid = Buffer.from('flat').toString('hex');
  assert.equal(await read(store, counter(rid, 'actors', 'active')), '9007199254740993');
  assert.equal(await read(store, counter(rid, 'spots', 'pending')), '4');
  assert.equal(await read(store, counter(rid, 'spot-type\0userSpot\0lobby', 'active')), '2');
  assert.equal(
    await read(store, counter(rid, 'spot-type\0instanceSpot\0stage:arena', 'pending')),
    '9223372036854775807'
  );
});

for (const [format, legacyType] of [
  ['flat', 'user_spot\0lobby'],
  ['nested', '2:lobby'],
  ['flat', '02:lobby'],
  ['flat', '1:lobby']
]) {
  test(`startup rejects a ${format} row with spot type key ${JSON.stringify(legacyType)}`, async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    const oldKey = legacyKey('invalid-type');
    const row =
      format === 'nested'
        ? { active: { ...usage(), spotTypes: { [legacyType]: 1 } }, pending: usage() }
        : {
            actorsActive: 0,
            actorsPending: 0,
            spotsActive: 1,
            spotsPending: 0,
            spotTypes: { [legacyType]: { active: 1, pending: 0 } }
          };
    const bytes = JSON.stringify(row);
    await put(store, oldKey, bytes);
    await assert.rejects(startup(t, store, 0), {
      name: 'ZLinkConfigurationException',
      message: /invalid capacity spot type/i
    });
    assert.equal(await read(store, FORMAT), undefined);
    assert.equal(await read(store, oldKey), bytes);
  });
}

test('malformed v11 aggregate blocks both floor and marker', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const aggregate = 'zlink:v11:aggregate:relocation:1';
  await put(store, aggregate, '{"state":"staging"}');
  await assert.rejects(startup(t, store, 0), {
    name: 'ZLinkConfigurationException',
    message: /aggregate/i
  });
  assert.equal(await read(store, FLOOR), undefined);
  assert.equal(await read(store, FORMAT), undefined);
  assert.equal(await read(store, 'zlink:v11:owner-counter'), undefined);

  assert.equal(await read(store, aggregate), '{"state":"staging"}');
});

for (const targets of [2047, 2048]) {
  test(`startup applies the 2,048-key bound to ${targets} counters plus the legacy row`, async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    const oldKey = legacyKey('bound');
    const spotTypes = Object.fromEntries(
      Array.from({ length: targets }, (_, index) => [`instance_spot\0type-${index}`, 1])
    );
    await put(
      store,
      oldKey,
      JSON.stringify({ active: { ...usage(), spotTypes }, pending: usage() })
    );
    if (targets === 2047) {
      await startup(t, store, 0);
      assert.equal(await read(store, oldKey), undefined);
      assert.equal(await read(store, FORMAT), 'converted');
      assert.equal(
        await read(store, counter('626f756e64', 'spot-type\0instanceSpot\0type-2046', 'active')),
        '1'
      );
    } else {
      await assert.rejects(startup(t, store, 0), {
        name: 'ZLinkConfigurationException',
        message: /bound|limit/i
      });
      assert.ok(await read(store, oldKey));
      assert.equal(await read(store, FORMAT), undefined);
      assert.equal(
        await read(store, counter('626f756e64', 'spot-type\0instanceSpot\0type-0', 'active')),
        undefined
      );
    }
  });
}

test('startup rejects a conversion request larger than 4 MiB below the key bound', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const oldKey = LEGACY_CAPACITY + 'm'.repeat(200) + ':' + 'b'.repeat(250);
  const spotTypes = Object.fromEntries(
    Array.from({ length: 1900 }, (_, index) => [`user_spot\0${'x'.repeat(250)}${index}`, 1])
  );
  await put(store, oldKey, JSON.stringify({ active: { ...usage(), spotTypes }, pending: usage() }));
  await assert.rejects(startup(t, store, 0), {
    name: 'ZLinkConfigurationException',
    message: /bound|limit/i
  });
  assert.ok(await read(store, oldKey));
  assert.equal(await read(store, FORMAT), undefined);
});

test('startup confirms applied floor, row and marker writes after their responses are lost', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const oldKey = legacyKey('lost');
  const emptyKey = legacyKey('empty');
  await put(store, oldKey, legacyRow(1, 2));
  await put(store, emptyKey, legacyRow(0, 0));
  const expected = new Set([FLOOR, oldKey, emptyKey, FORMAT]);
  const provider = intercept(store, async (request, signal) => {
    const result = await store.write(request, signal);
    for (const mutation of request.mutations) {
      if (expected.delete(mutation.key.value)) {
        assert.equal(result.kind, 'applied');
        throw new Error('injected lost write response');
      }
    }
    return result;
  });
  await startup(t, provider, 0);
  assert.equal(expected.size, 0);
  assert.equal(await read(store, oldKey), undefined);
  assert.equal(await read(store, emptyKey), undefined);
  assert.equal(await read(store, counter('6c6f7374', 'actors', 'pending')), '2');
  assert.equal(await read(store, FORMAT), 'converted');
});

test(
  'startup stays Preparing on an unapplied format write failure and converts before a later claim',
  { timeout: 3000 },
  async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    let available = false;
    let writes = 0;
    const provider = intercept(store, async (request, signal) => {
      writes++;
      if (!available) throw new Error('injected unavailable floor write');
      if (ownerLeaseClaimMutation(request)) assert.equal(await read(store, FORMAT), 'converted');
      return store.write(request, signal);
    });
    const runtime = createRuntime(provider);
    t.after(() => runtime.stop());
    await runtime.start('conversion-outage', t.signal);
    assert.equal(runtime.ownerLeaseUsable, false);
    assert.equal(writes, 1);
    assert.equal(await read(store, FLOOR), undefined);
    assert.equal(await read(store, FORMAT), undefined);

    available = true;
    assert.equal(await runtime.renewOwnerLeaseOnce(t.signal), true);
    assert.equal(await read(store, FORMAT), 'converted');
  }
);

test('startup resumes an interrupted migration and restarts an expired paged snapshot', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const firstKey = legacyKey('a');
  const secondKey = legacyKey('b');
  await put(store, firstKey, legacyRow(1, 0));
  await put(store, secondKey, legacyRow(0, 2));
  await put(store, 'zlink:v11:object-counter', '17');
  const interrupted = new AbortController();
  const provider = intercept(store, async (request, signal) => {
    const result = await store.write(request, signal);
    if (request.mutations.some((mutation) => mutation.key.value === firstKey)) {
      interrupted.abort(new Error('injected process stop after first row'));
      signal.throwIfAborted();
    }
    return result;
  });
  await assert.rejects(startup(t, provider, 0, interrupted.signal), /injected process stop/);
  assert.equal(await read(store, firstKey), undefined);
  assert.ok(await read(store, secondKey));
  assert.equal(await read(store, FORMAT), undefined);
  assert.equal(await read(store, FLOOR), '17');

  await put(store, legacyKey('c'), legacyRow(0, 0));
  let expirations = 0;
  const resumed = intercept(store, store.write.bind(store), (request, signal) => {
    if (request.prefix === LEGACY_CAPACITY && request.cursor && expirations === 0) {
      expirations++;
      return { kind: 'expired' };
    }
    return store.scan(
      request.prefix === LEGACY_CAPACITY ? { ...request, limit: 1 } : request,
      signal
    );
  });
  await startup(t, resumed, 1);
  assert.equal(expirations, 1);
  assert.equal(await read(store, FLOOR), '17');
  assert.equal(await read(store, FORMAT), 'converted');
  assert.equal(await read(store, counter('61', 'actors', 'active')), '1');
  assert.equal(await read(store, counter('62', 'actors', 'pending')), '2');
  assert.equal((await store.scan({ prefix: LEGACY_CAPACITY, limit: 1 })).value.items.length, 0);
});

for (const [name, value] of [
  ['malformed RID encoding', ['%XX', legacyRow(1, 0)]],
  ['malformed capacity JSON', ['invalid', '{"active":']],
  ['fractional count', ['invalid', legacyRow(1.5, 0)]],
  ['numeric string count', ['invalid', legacyRow('3', 0)]],
  [
    'overflowing count',
    ['invalid', legacyRow(1, 0).replace('"actors":1', '"actors":9223372036854775808')]
  ]
]) {
  test(`startup leaves the legacy row and marker intact on ${name}`, async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    const oldKey = legacyKey(value[0]);
    await put(store, oldKey, value[1]);
    await assert.rejects(startup(t, store, 0), { name: 'ZLinkConfigurationException' });
    assert.equal(await read(store, FORMAT), undefined);
    assert.equal(await read(store, oldKey), value[1]);
  });
}

test('startup rejects a destination already present while its v11 row remains', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const oldKey = legacyKey('conflict');
  const destination = counter('636f6e666c696374', 'actors', 'active');
  await put(store, oldKey, legacyRow(1, 2));
  await put(store, destination, '9');
  await assert.rejects(startup(t, store, 0), {
    name: 'ZLinkConfigurationException',
    message: /counter.*exists/i
  });
  assert.equal(await read(store, destination), '9');
  assert.ok(await read(store, oldKey));
  assert.equal(await read(store, FORMAT), undefined);
});

for (const [format, floor] of [
  ['converted', undefined],
  ['converted', '01'],
  ['unknown', '1']
]) {
  test(`startup rejects format ${format} with floor ${String(floor)}`, async (t) => {
    const store = new framework.ZLinkInMemoryProviderLocationStore();
    await put(store, FORMAT, format);
    if (floor !== undefined) await put(store, FLOOR, floor);
    await assert.rejects(startup(t, store, 0), { name: 'ZLinkConfigurationException' });
    assert.equal(await read(store, 'zlink:v11:owner-counter'), undefined);
  });
}

test('startup uses the generation floor for an ID issued before conversion', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const id = 'previously-issued';
  const previousObjectGeneration = 10n;
  const previousOwnerGeneration = 16n;
  await put(store, 'zlink:v11:object-counter', String(previousObjectGeneration + 1n));
  await put(store, 'zlink:v11:authority-owner-counter', String(previousOwnerGeneration + 1n));
  await startup(t, store, 0);
  const { repository, target } = await seedDescriptor(store, '73616d652d6964');
  const ready = await createReadyUserSpot(repository, id, target);
  assert.ok(ready.objectGeneration > previousObjectGeneration);
  assert.ok(ready.authorityOwnerGeneration > previousOwnerGeneration);
  assert.equal(ready.objectGeneration, 17n);
  assert.equal(ready.authorityOwnerGeneration, 17n);
});

test('a late converter accepts an aggregate created after another startup completes', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  let enterScan;
  const scanEntered = new Promise((resolve) => {
    enterScan = resolve;
  });
  let releaseScan;
  const scanReleased = new Promise((resolve) => {
    releaseScan = resolve;
  });
  const delayed = intercept(store, store.write.bind(store), async (request, signal) => {
    if (request.prefix === 'zlink:v11:aggregate:') {
      enterScan();
      await scanReleased;
    }
    return store.scan(request, signal);
  });

  const late = startup(t, delayed, 0);
  try {
    await scanEntered;
    await startup(t, store, 1);
    await put(store, 'zlink:v11:aggregate:created-in-v12:1', '{"state":"staging"}');
  } finally {
    releaseScan();
  }
  await late;
  assert.equal(await read(store, FORMAT), 'converted');
});

test('lease renewals after a completed conversion do not read the format marker again', async () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  let markerReads = 0;
  const counted = {
    read(key, signal) {
      if (key.value === FORMAT) markerReads++;
      return store.read(key, signal);
    },
    write: store.write.bind(store),
    scan: store.scan.bind(store)
  };
  const repository = new framework.ZLinkLocationStoreRepository(counted);
  await repository.ensureStoreFormat();
  assert.equal(await read(store, FORMAT), 'converted');
  const afterConversion = markerReads;
  await repository.ensureStoreFormat();
  await repository.ensureStoreFormat();
  assert.equal(markerReads, afterConversion);
});

const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { testRedisUrl, connectTestRedis } = require('./helpers/test-redis');
const {
  ZLINK_PROVIDER_MAX_WRITE_KEYS,
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/internal');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const { ZLinkEtcdLocationStore } = require('../../packages/framework-locations-etcd/dist');
const {
  EtcdAdminStore
} = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');
const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');

const memory = async () => {
  const store = new ZLinkInMemoryProviderLocationStore();
  return [store, store];
};

async function redis(t) {
  const client = await connectTestRedis(t);
  if (client === undefined) return;
  const keyPrefix = `counter-${randomUUID()}`;
  const stores = Array.from(
    { length: 2 },
    () => new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix })
  );
  t.after(async () => {
    await Promise.all(stores.map((store) => store.dispose()));
    try {
      const rows = await client.keys(`${keyPrefix}:*`);
      if (rows.length > 0) await client.del(rows);
    } finally {
      await client.quit();
    }
  });
  return stores;
}

async function etcd(t) {
  const endpoint = process.env.ZLINK_ETCD_TEST_ENDPOINT;
  if (endpoint === undefined) {
    t.skip('ZLINK_ETCD_TEST_ENDPOINT is not set');
    return;
  }
  const options = { endpoints: [endpoint], namespace: `counter-${randomUUID()}` };
  const client = new EtcdClient(options);
  try {
    await new EtcdAdminStore(client).init();
  } finally {
    client.close();
  }
  const stores = Array.from({ length: 2 }, () => new ZLinkEtcdLocationStore(options));
  t.after(() => Promise.all(stores.map((store) => store.dispose())));
  return stores;
}

const key = (value) => ({ value });
const put = (key, value) => ({ kind: 'put', key, bytes: Buffer.from(value) });
const add = (key, delta) => ({ kind: 'add', key, delta });
const sumAtMost = (keys, delta, limit) => ({ kind: 'sumAtMost', keys, delta, limit });
const max = (1n << 63n) - 1n;

async function value(store, key) {
  const read = await store.read(key);
  assert.equal(read.kind, 'found');
  return [Buffer.from(read.value.bytes).toString(), read.value.version];
}

for (const [name, fixture] of Object.entries({ memory, redis, etcd })) {
  test(`${name} counter condition uses an exact sum and observes missing keys as zero`, async (t) => {
    const stores = await fixture(t);
    if (stores === undefined) return;
    const [store] = stores;
    const active = key('active');
    const pending = key('pending');
    const opaque = key('opaque');
    const large = 9_007_199_254_740_993n;
    await store.write({
      conditions: [],
      mutations: [put(active, String(large)), put(opaque, 'binary\0data')]
    });

    const reserved = await store.write({
      conditions: [sumAtMost([active, pending], 1n, large + 1n)],
      mutations: [add(pending, 1n)]
    });
    assert.equal(reserved.kind, 'applied');
    const [pendingValue, version] = await value(store, pending);
    assert.equal(pendingValue, '1');
    assert.deepEqual(reserved.putVersions, [{ key: pending, version }]);
    const rejected = await store.write({
      conditions: [sumAtMost([active, pending], 1n, large + 1n)],
      mutations: [add(pending, 1n)]
    });
    assert.equal(rejected.kind, 'conflict');
    assert.deepEqual(await value(store, pending), ['1', version]);
    assert.equal((await value(store, opaque))[0], 'binary\0data');

    await store.write({
      conditions: [],
      mutations: [put(active, String(max)), put(pending, String(max))]
    });
    assert.equal(
      (
        await store.write({
          conditions: [sumAtMost([active, pending], -max, max)],
          mutations: [add(active, -max)]
        })
      ).kind,
      'applied'
    );
    assert.equal((await value(store, active))[0], '0');

    const marker = key('marker');
    const mixed = await store.write({
      conditions: [
        sumAtMost([active], max + 1n, max + 1n),
        { kind: 'value', key: opaque, expected: Buffer.from('binary\0data') }
      ],
      mutations: [put(marker, 'created'), add(active, max), put(opaque, 'updated')]
    });
    assert.equal(mixed.kind, 'applied');
    assert.deepEqual(
      mixed.putVersions.map(({ key }) => key),
      [marker, active, opaque]
    );
    assert.equal((await value(store, active))[0], String(max));
    assert.equal((await value(store, marker))[0], 'created');
    assert.equal((await value(store, opaque))[0], 'updated');
  });

  test(`${name} rejects invalid counters without applying any mutation`, async (t) => {
    const stores = await fixture(t);
    if (stores === undefined) return;
    const [store] = stores;
    const counter = key('counter');
    const marker = key('marker');
    const missing = key('missing');
    await store.write({
      conditions: [],
      mutations: [put(counter, String(max)), put(marker, 'original')]
    });
    const original = await value(store, marker);
    const originalCounter = await value(store, counter);
    for (const mutation of [add(missing, -1n), add(counter, 1n), add(missing, max + 1n)]) {
      const result = await store.write({
        conditions: [],
        mutations: [put(marker, 'changed'), mutation]
      });
      assert.equal(result.kind, 'corrupt');
      assert.deepEqual(result.putVersions, []);
      assert.deepEqual(await value(store, marker), original);
      assert.deepEqual(await value(store, counter), originalCounter);
      assert.equal((await store.read(missing)).kind, 'missing');
    }

    for (const invalid of [
      'x',
      '+1',
      '-1',
      '01',
      '',
      '1\n',
      String(max + 1n),
      Buffer.from([0xb1])
    ]) {
      await store.write({ conditions: [], mutations: [put(counter, invalid)] });
      const before = await value(store, counter);
      for (const batch of [
        { conditions: [sumAtMost([counter], 0n, max)], mutations: [put(marker, 'changed')] },
        { conditions: [], mutations: [put(marker, 'changed'), add(counter, 1n)] }
      ]) {
        assert.equal((await store.write(batch)).kind, 'corrupt');
        assert.deepEqual(await value(store, counter), before);
        assert.deepEqual(await value(store, marker), original);
      }
    }
  });

  test(`${name} applies concurrent unconditional adds without a conflict`, async (t) => {
    const stores = await fixture(t);
    if (stores === undefined) return;
    const counter = key('counter');
    const results = await Promise.all(
      stores.map((store) => store.write({ conditions: [], mutations: [add(counter, 1n)] }))
    );
    assert.deepEqual(
      results.map((result) => result.kind),
      ['applied', 'applied']
    );
    assert.equal((await value(stores[0], counter))[0], '2');
  });

  test(`${name} includes counter keys in the write bound and rejects duplicates`, async (t) => {
    const stores = await fixture(t);
    if (stores === undefined) return;
    const [store] = stores;
    const keys = Array.from({ length: ZLINK_PROVIDER_MAX_WRITE_KEYS }, (_, index) =>
      key(`bound-${index}`)
    );
    const request = { conditions: [sumAtMost(keys, 1n, 1n)], mutations: [add(keys[0], 1n)] };
    assert.equal((await store.write(request)).kind, 'applied');
    await assert.rejects(
      store.write({ ...request, mutations: [add(key('extra'), 1n)] }),
      RangeError
    );
    await assert.rejects(
      store.write({
        conditions: [sumAtMost([keys[0], keys[0]], 0n, 1n)],
        mutations: []
      }),
      RangeError
    );
    await assert.rejects(
      store.write({
        conditions: [sumAtMost([keys[0]], 0n, 1n), { kind: 'missing', key: keys[0] }],
        mutations: []
      }),
      RangeError
    );
    await assert.rejects(
      store.write({
        conditions: [],
        mutations: [add(keys[0], 1n), put(keys[0], '0')]
      }),
      RangeError
    );
  });
}

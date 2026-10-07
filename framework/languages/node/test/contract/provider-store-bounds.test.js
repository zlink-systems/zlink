const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');

test('in-memory Store counts key and expected version bytes in encoded batch bounds', async () => {
  const store = new internal.ZLinkInMemoryProviderLocationStore();
  const conditions = Array.from({ length: 1024 }, (_, index) => ({
    kind: 'version',
    key: { value: `metadata/${index}` },
    expected: { value: 'v'.repeat(4096) }
  }));
  await assert.rejects(store.write({ conditions, mutations: [] }), RangeError);
});

test('in-memory Store validates expected version bounds before checking conditions', async () => {
  const store = new internal.ZLinkInMemoryProviderLocationStore();
  for (const value of ['', 'v'.repeat(4097), '가'.repeat(1366)]) {
    await assert.rejects(
      store.write({
        conditions: [
          {
            kind: 'version',
            key: { value: 'version/subject' },
            expected: { value }
          }
        ],
        mutations: []
      }),
      RangeError
    );
  }
});

test('owner lease renewal preserves an unapplied provider error', async () => {
  const inner = new internal.ZLinkInMemoryProviderLocationStore();
  const failure = new Error('node-store provider write failed');
  let failWrites = false;
  const provider = {
    read: (key, signal) => inner.read(key, signal),
    scan: (request, signal) => inner.scan(request, signal),
    write: (request, signal) =>
      failWrites ? Promise.reject(failure) : inner.write(request, signal)
  };
  const repository = new internal.ZLinkLocationStoreRepository(provider);
  const claimed = await repository.claimOwnerLease('node-store-renew-error', 30000);
  assert.equal(claimed.kind, 'claimed');
  failWrites = true;
  await assert.rejects(
    repository.renewOwnerLease(claimed.token, 30000),
    (error) =>
      error instanceof ZLinkFrameworkException &&
      error.kind === ZLinkFrameworkErrorKind.Unavailable &&
      error.cause === failure
  );
});

test('owner lease claim preserves an error before a late provider commit', async () => {
  const inner = new internal.ZLinkInMemoryProviderLocationStore();
  const failure = new Error('node-store pending commit response failed');
  let pending;
  const provider = {
    read: (key, signal) => inner.read(key, signal),
    scan: (request, signal) => inner.scan(request, signal),
    async write(request) {
      pending = request;
      throw failure;
    }
  };
  const repository = new internal.ZLinkLocationStoreRepository(provider);
  await assert.rejects(
    repository.claimOwnerLease('node-store-late-claim', 30000),
    (error) =>
      error instanceof ZLinkFrameworkException &&
      error.kind === ZLinkFrameworkErrorKind.Unavailable &&
      error.cause === failure
  );
  assert.deepEqual(await repository.readOwnerLease('node-store-late-claim'), { kind: 'missing' });
  assert.equal((await inner.write(pending)).kind, 'applied');
  assert.equal((await repository.readOwnerLease('node-store-late-claim')).kind, 'found');
});

for (const invalid of [
  { bytes: Uint8Array.of(2), retentionMs: 0 },
  { bytes: new Uint8Array(1024 * 1024 + 1) }
]) {
  test(`invalid Store batch leaves all values and versions unchanged (${invalid.retentionMs ?? 'size'})`, async () => {
    const store = new internal.ZLinkInMemoryProviderLocationStore();
    const first = { value: 'batch/first' };
    await store.write({
      conditions: [],
      mutations: [{ kind: 'put', key: first, bytes: Uint8Array.of(1) }]
    });
    const before = (await store.read(first)).value;
    await assert.rejects(
      store.write({
        conditions: [],
        mutations: [
          { kind: 'put', key: first, bytes: Uint8Array.of(3) },
          { kind: 'put', key: { value: 'batch/invalid' }, ...invalid }
        ]
      }),
      RangeError
    );
    const after = (await store.read(first)).value;
    assert.deepEqual(after.bytes, before.bytes);
    assert.deepEqual(after.version, before.version);
    assert.equal((await store.read({ value: 'batch/invalid' })).kind, 'missing');
  });
}

for (const provider of ['memory', 'redis']) {
  test(`${provider} Store returns Expired for malformed bounded cursors`, async () => {
    const store =
      provider === 'memory'
        ? new internal.ZLinkInMemoryProviderLocationStore()
        : new ZLinkRedisLocationStore({
            keyPrefix: 'node-store-invalid-cursor',
            client: {
              isReady: true,
              async sendCommand() {
                assert.fail('Malformed cursor must not perform Redis I/O');
              }
            }
          });
    try {
      for (const value of ['invalid', '1:not-an-offset', '-1:0']) {
        assert.deepEqual(await store.scan({ prefix: '', limit: 1, cursor: { value } }), {
          kind: 'expired'
        });
      }
      for (const value of ['', 'x'.repeat(4097)]) {
        await assert.rejects(store.scan({ prefix: '', limit: 1, cursor: { value } }), RangeError);
      }
    } finally {
      if (provider === 'redis') await store.dispose();
    }
  });
}

test('in-memory Store expires malformed offsets for an active snapshot', async () => {
  const store = new internal.ZLinkInMemoryProviderLocationStore();
  await store.write({
    conditions: [],
    mutations: [
      { kind: 'put', key: { value: 'cursor/a' }, bytes: Uint8Array.of(1) },
      { kind: 'put', key: { value: 'cursor/b' }, bytes: Uint8Array.of(2) }
    ]
  });
  const first = await store.scan({ prefix: 'cursor/', limit: 1 });
  const id = first.value.nextCursor.value.split(':')[0];
  for (const offset of ['', '0x1', '1e0', '01', '3']) {
    assert.deepEqual(
      await store.scan({ prefix: 'cursor/', limit: 1, cursor: { value: `${id}:${offset}` } }),
      { kind: 'expired' }
    );
  }
  const second = await store.scan({ prefix: 'cursor/', limit: 1, cursor: first.value.nextCursor });
  assert.deepEqual(
    second.value.items.map((item) => item.key.value),
    ['cursor/b']
  );
});

test('in-memory Store limits each encoded scan page and preserves the snapshot', async () => {
  const store = new internal.ZLinkInMemoryProviderLocationStore();
  for (let index = 0; index < 5; index += 1) {
    await store.write({
      conditions: [],
      mutations: [
        { kind: 'put', key: { value: `page/${index}` }, bytes: new Uint8Array(1024 * 1024) }
      ]
    });
  }
  let cursor;
  const keys = [];
  do {
    const page = await store.scan({ prefix: 'page/', limit: 5, cursor });
    assert.equal(page.kind, 'page');
    const bytes = page.value.items.reduce(
      (sum, item) =>
        sum +
        Buffer.byteLength(item.key.value) +
        Buffer.byteLength(item.value.version.value) +
        item.value.bytes.byteLength,
      0
    );
    assert.ok(bytes <= 4 * 1024 * 1024, `encoded page was ${bytes} bytes`);
    keys.push(...page.value.items.map((item) => item.key.value));
    cursor = page.value.nextCursor;
  } while (cursor !== undefined);
  assert.deepEqual(keys, ['page/0', 'page/1', 'page/2', 'page/3', 'page/4']);
});

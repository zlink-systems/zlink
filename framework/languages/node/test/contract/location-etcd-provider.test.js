const assert = require('node:assert/strict');
const test = require('node:test');
const grpc = require('@grpc/grpc-js');
const { ZLinkEtcdLocationStore } = require('../../packages/framework-locations-etcd/dist');
const {
  EtcdClient,
  protoDefinition
} = require('../../packages/framework-locations-etcd/dist/internal/client');
const {
  encodeClock,
  encodeMetadata,
  MAX_TIME_MS
} = require('../../packages/framework-locations-etcd/dist/internal/encoding');
const {
  ScanCursor,
  monotonicMs
} = require('../../packages/framework-locations-etcd/dist/internal/cursor');

async function fixture(handler) {
  const server = new grpc.Server();
  const definitions = grpc.loadPackageDefinition(protoDefinition()).etcdserverpb;
  server.addService(definitions.KV.service, { txn: handler });
  const port = await new Promise((resolve, reject) =>
    server.bindAsync('127.0.0.1:0', grpc.ServerCredentials.createInsecure(), (error, port) =>
      error ? reject(error) : resolve(port)
    )
  );
  const store = new ZLinkEtcdLocationStore({
    endpoints: [`http://127.0.0.1:${port}`],
    namespace: 'unit',
    operationTimeoutMs: 500
  });
  return {
    store,
    async close() {
      await store.dispose();
      server.forceShutdown();
    }
  };
}
const kv = (value, revision = '7') => ({ value, mod_revision: revision, create_revision: '1' });
const header = { cluster_id: '18446744073709551615', revision: '7' };
const read = (...values) => ({
  header,
  responses: values.map((value) => ({
    response_range: { kvs: value === undefined ? [] : [value] }
  }))
});

test('etcd package root exposes the Store and the declared option types', () => {
  assert.deepEqual(Object.keys(require('../../packages/framework-locations-etcd/dist')), [
    'ZLinkEtcdLocationStore'
  ]);
  const declaration = require('node:fs').readFileSync(
    require('node:path').resolve(
      __dirname,
      '../../packages/framework-locations-etcd/dist/index.d.ts'
    ),
    'utf8'
  );
  for (const name of [
    'ZLinkEtcdLocationOptions',
    'ZLinkEtcdTlsOptions',
    'ZLinkEtcdClientIdentity',
    'ZLinkEtcdCredentials'
  ])
    assert.match(declaration, new RegExp(`\\b${name}\\b`));
});

test('etcd caller errors and a pre-aborted signal perform no I/O', async () => {
  const f = await fixture(() => assert.fail('invalid input reached I/O'));
  try {
    const key = { value: 'a' };
    for (const request of [
      {
        conditions: [
          { kind: 'missing', key },
          { kind: 'missing', key }
        ],
        mutations: []
      },
      {
        conditions: [],
        mutations: [
          { kind: 'delete', key },
          { kind: 'delete', key }
        ]
      },
      { conditions: [], mutations: [{ kind: 'put', key, bytes: new Uint8Array(1048577) }] },
      ...[0, NaN, Infinity, Number(MAX_TIME_MS) + 1].map((retentionMs) => ({
        conditions: [],
        mutations: [{ kind: 'put', key, bytes: Buffer.alloc(0), retentionMs }]
      })),
      {
        conditions: Array.from({ length: 1024 }, (_, i) => ({
          kind: 'version',
          key: { value: `a${i}` },
          expected: { value: 'v'.repeat(4096) }
        })),
        mutations: []
      },
      {
        conditions: Array.from({ length: 2049 }, (_, i) => ({
          kind: 'missing',
          key: { value: `a${i}` }
        })),
        mutations: []
      }
    ])
      await assert.rejects(f.store.write(request), RangeError);
    for (const value of ['', 'v'.repeat(4097)])
      await assert.rejects(
        f.store.write({
          conditions: [{ kind: 'version', key, expected: { value } }],
          mutations: []
        }),
        RangeError
      );
    for (const value of ['', 'x'.repeat(4097)])
      await assert.rejects(f.store.scan({ prefix: '', limit: 1, cursor: { value } }), RangeError);
    for (const value of ['invalid', '1:not-an-offset'])
      assert.deepEqual(await f.store.scan({ prefix: '', limit: 1, cursor: { value } }), {
        kind: 'expired'
      });
    const signal = AbortSignal.abort();
    for (const operation of [
      () => f.store.read(key, signal),
      () => f.store.write({ conditions: [], mutations: [] }, signal),
      () => f.store.scan({ prefix: '', limit: 1 }, signal)
    ])
      await assert.rejects(operation(), /stopped/);
  } finally {
    await f.close();
  }
});

test('etcd exact read rejects every corrupt pair, including expired records', async () => {
  const metadata = encodeMetadata({ hasExpiry: true, expiresAtMs: 1n, payloadLength: 1 });
  for (const [m, p] of [
    [undefined, kv(Buffer.from('a'))],
    [kv(metadata), undefined],
    [kv(metadata), kv(Buffer.from('a'), '8')],
    [kv(metadata), kv(Buffer.alloc(0))],
    [kv(Buffer.from([2, ...metadata.subarray(1)])), kv(Buffer.from('a'))]
  ]) {
    const f = await fixture((_call, callback) => callback(null, read(kv(encodeClock(100n)), m, p)));
    try {
      await assert.rejects(f.store.read({ value: 'subject' }), /encoding/);
    } finally {
      await f.close();
    }
  }
});

test('etcd write expiry arithmetic rejects overflow without submitting apply', async () => {
  let calls = 0;
  const f = await fixture((_call, callback) => {
    calls++;
    callback(null, read(kv(encodeClock(MAX_TIME_MS))));
  });
  try {
    await assert.rejects(
      f.store.write({
        conditions: [],
        mutations: [{ kind: 'put', key: { value: 'a' }, bytes: Buffer.alloc(0), retentionMs: 0.1 }]
      }),
      RangeError
    );
    assert.equal(calls, 1);
  } finally {
    await f.close();
  }
});

test('etcd Value payload compaction restarts the decision with the original operation deadline', async () => {
  const bytes = Buffer.from('value');
  const metadata = encodeMetadata({
    hasExpiry: true,
    expiresAtMs: 1000n,
    payloadLength: bytes.length
  });
  const deadlines = [];
  let decisions = 0;
  let payloads = 0;
  let applies = 0;
  const f = await fixture((call, callback) => {
    const request = call.request;
    if (request.compare.length > 0) {
      applies++;
      const clockCompare = request.compare.find((c) => c.target === 3);
      assert.equal(clockCompare.result, 2);
      assert.deepEqual(clockCompare.value, encodeClock(1000n));
      assert.equal(request.compare.find((c) => c.target === 2).mod_revision, '8');
      callback(null, { succeeded: true, header: { ...header, revision: '9' } });
    } else if (request.success.length === 2) {
      decisions++;
      callback(
        null,
        read(kv(encodeClock(BigInt(decisions))), kv(metadata, decisions === 1 ? '7' : '8'))
      );
    } else {
      payloads++;
      if (payloads === 1)
        callback({
          code: grpc.status.OUT_OF_RANGE,
          details: 'etcdserver: mvcc: required revision has been compacted'
        });
      else callback(null, read(kv(bytes, '8')));
    }
  });
  const rpc = EtcdClient.prototype.rpc;
  EtcdClient.prototype.rpc = function (...args) {
    deadlines.push(args[3].deadline.getTime());
    return rpc.apply(this, args);
  };
  try {
    const result = await f.store.write({
      conditions: [{ kind: 'value', key: { value: 'subject' }, expected: bytes }],
      mutations: [{ kind: 'put', key: { value: 'marker' }, bytes }]
    });
    assert.equal(result.kind, 'applied');
    assert.equal(result.storeNow.getTime(), 2);
    assert.equal(result.putVersions[0].version.value, 'e1:ffffffffffffffff:9');
    assert.equal(decisions, 2);
    assert.equal(payloads, 2);
    assert.equal(applies, 1);
    assert.equal(new Set(deadlines).size, 1, 'all RPCs retain the operation deadline');
  } finally {
    EtcdClient.prototype.rpc = rpc;
    await f.close();
  }
});

test('etcd cancellation cancels a pending RPC; dispose drains and rejects new operations', async () => {
  let entered;
  let cancelled;
  const arrived = new Promise((resolve) => {
    entered = resolve;
  });
  const cancellation = new Promise((resolve) => {
    cancelled = resolve;
  });
  const f = await fixture((call) => {
    call.on('cancelled', cancelled);
    entered();
  });
  try {
    const stop = new AbortController();
    const operation = f.store.read({ value: 'pending' }, stop.signal);
    await arrived;
    const rejection = assert.rejects(operation, /stopped/);
    const disposal = f.store.dispose();
    assert.equal(disposal, f.store.dispose());
    await assert.rejects(f.store.read({ value: 'new' }), /disposed/);
    stop.abort();
    await Promise.all([rejection, disposal, cancellation]);
  } finally {
    await f.close();
  }
});

test('etcd cursors enforce monotonic validity, instance and prefix with maximum UTF-8 fields', () => {
  const cursors = new ScanCursor('n'.repeat(64));
  const snapshot = {
    cluster: '18446744073709551615',
    revision: '9223372036854775807',
    clock: MAX_TIME_MS,
    started: monotonicMs(),
    last: Buffer.from('가'.repeat(341) + 'x')
  };
  const prefix = '가'.repeat(341);
  const encoded = cursors.encode(snapshot, prefix);
  assert.ok(Buffer.byteLength(encoded) <= 4096);
  assert.deepEqual(cursors.decode(encoded, prefix), snapshot);
  assert.equal(new ScanCursor('other').decode(encoded, prefix), undefined);
  assert.equal(cursors.decode(encoded, 'different'), undefined);
  assert.equal(
    cursors.decode(
      cursors.encode({ ...snapshot, started: monotonicMs() - 300001n }, prefix),
      prefix
    ),
    undefined
  );
});

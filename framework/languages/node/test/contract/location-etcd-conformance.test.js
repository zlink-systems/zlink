const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  Harness,
  command,
  child,
  receive,
  rpc,
  until,
  delay,
  key,
  b64,
  encoding,
  root,
  pkg,
  logRoot
} = require('./location-etcd-fixtures/provider-harness.cjs');
const inventory = require('../../../../runtime/conformance/location-store-provider-v1.json');
const golden = require('../../../../runtime/protocol/golden/location-store-etcd-v1.json');

const putRequest = (subject, bytes = Buffer.from('marker')) => ({
  conditions: [],
  mutations: [{ kind: 'put', key: key(subject), bytes }]
});
const applied = (value) => assert.equal(value.kind, 'applied');
const missing = async (store, subject) =>
  assert.equal((await store.read(key(subject))).kind, 'missing');
const found = async (store, subject) => {
  const read = await store.read(key(subject));
  assert.equal(read.kind, 'found');
  return read.value;
};
const cases = {};
const mutationResults = [];

async function checkMutation(id, action) {
  let failure;
  try {
    await action();
  } catch (error) {
    failure = error;
  }
  assert.ok(
    failure instanceof assert.AssertionError,
    `${id}: the bad implementation must fail the observation, got ${failure?.stack}`
  );
  mutationResults.push({ id, status: 'detected', failure: failure.message });
}
function interceptApply(h, store, hook) {
  let armed = true;
  h.wrapRpc(store, async ({ method, request, operation }, send) => {
    if (
      armed &&
      method === 'txn' &&
      request.success?.some((op) => op.request_put || op.request_delete_range)
    ) {
      armed = false;
      return hook(request, send, operation);
    }
    return send();
  });
}
async function heldApply(h, f, boundary, badClock = false) {
  const proxy = await h.proxy(h.cluster.endpoints[0]);
  const store = h.store(f.namespace, [proxy.endpoint]);
  interceptApply(h, store, async (request, send) => {
    if (badClock) request.compare = request.compare.filter((c) => c.target !== 'VALUE');
    await proxy.controlCall('hold-upstream');
    const pending = send();
    pending.catch(() => {});
    try {
      await until(
        async () => (await proxy.controlCall('status')).bufferedUpstream > 0,
        'held apply bytes'
      );
      await boundary();
    } finally {
      await proxy.controlCall('release');
    }
    return pending;
  });
  return store;
}
async function managers(h, f, count = 1) {
  const before = await h.clock(f);
  const running = Array.from({ length: count }, (_, i) => h.manager(f.file, `${f.namespace}-${i}`));
  await until(
    async () => (await h.clock(f)).revision !== before.revision,
    'actual management process clock update'
  );
  return running;
}
async function advanceWithManager(h, f, target) {
  const running = await managers(h, f);
  try {
    await until(async () => (await h.clock(f)).value >= target, 'Store clock expiry');
  } finally {
    for (const manager of running) await h.stopManager(manager);
  }
}
async function pages(store, prefix, limit = 1000) {
  const items = [];
  let cursor;
  let clock;
  do {
    const result = await store.scan({ prefix, limit, cursor });
    assert.equal(result.kind, 'page');
    clock ??= result.value.storeNow.getTime();
    assert.equal(result.value.storeNow.getTime(), clock);
    assert.ok(result.value.items.length <= limit);
    const size = result.value.items.reduce(
      (n, i) =>
        n +
        Buffer.byteLength(i.key.value) +
        Buffer.byteLength(i.value.version.value) +
        i.value.bytes.length,
      0
    );
    assert.ok(size <= 4194304);
    items.push(...result.value.items);
    cursor = result.value.nextCursor;
    if (cursor) assert.ok(Buffer.byteLength(cursor.value) <= 4096);
  } while (cursor);
  return items;
}

cases['etcd.corrupt-record'] = async (h, f) => {
  const bytes = Buffer.from([0, 255, 3]);
  const valid = encoding.encodeMetadata({
    hasExpiry: false,
    expiresAtMs: 0n,
    payloadLength: bytes.length
  });
  const fixtures = [
    { metadata: Buffer.from([99, ...valid.subarray(1)]), payload: bytes },
    { metadata: undefined, payload: bytes },
    { metadata: valid, payload: undefined },
    { metadata: valid, payload: bytes, split: true },
    { metadata: valid, payload: Buffer.from('longer') }
  ];
  for (const [i, fixture] of fixtures.entries()) {
    const subject = `corrupt/${i}`;
    const m = {
      request_put: {
        key: b64(f.keys.metadata(Buffer.from(subject))),
        value: b64(fixture.metadata || valid)
      }
    };
    const p = {
      request_put: {
        key: b64(f.keys.payload(Buffer.from(subject))),
        value: b64(fixture.payload || [])
      }
    };
    const success = [fixture.metadata && m, fixture.payload && p].filter(Boolean);
    if (fixture.split) {
      await h.raw('txn', { success: [m] });
      await h.raw('txn', { success: [p] });
    } else await h.raw('txn', { success });
    await assert.rejects(f.store.read(key(subject)), /encoding/);
    if (fixture.metadata)
      await assert.rejects(f.store.scan({ prefix: subject, limit: 10 }), /encoding/);
    if (i === 0)
      await assert.rejects(
        f.store.write({ conditions: [{ kind: 'missing', key: key(subject) }], mutations: [] }),
        /encoding/
      );
  }
  const clock = await h.clock(f);
  for (const fixture of golden.corruptRecords) {
    const subject = `golden/${fixture.name}`;
    if (fixture.clockHex !== undefined) {
      await h.raw('put', {
        key: b64(f.keys.clock),
        value: b64(Buffer.from(fixture.clockHex, 'hex'))
      });
      for (const action of [
        () => f.store.read(key(subject)),
        () => f.store.write(putRequest(subject)),
        () => f.store.scan({ prefix: subject, limit: 1 })
      ])
        await assert.rejects(action());
      await h.setClock(f, clock.value);
      continue;
    }
    const success = [];
    if (fixture.metadataHex !== null)
      success.push({
        request_put: {
          key: b64(f.keys.metadata(Buffer.from(subject))),
          value: b64(Buffer.from(fixture.metadataHex, 'hex'))
        }
      });
    if (fixture.payloadHex !== null)
      success.push({
        request_put: {
          key: b64(f.keys.payload(Buffer.from(subject))),
          value: b64(Buffer.from(fixture.payloadHex, 'hex'))
        }
      });
    if (fixture.metadataModRevision !== undefined)
      for (const op of success) await h.raw('txn', { success: [op] });
    else await h.raw('txn', { success });
    await assert.rejects(f.store.read(key(subject)), /encoding/);
    if (fixture.metadataHex !== null)
      await assert.rejects(f.store.scan({ prefix: subject, limit: 1 }), /encoding/);
  }
  await h.setClock(f, encoding.MAX_TIME_MS);
  await assert.rejects(h.put(f, 'overflow', bytes, 1), RangeError);
  await h.setClock(f, clock.value);
  // A deliberately split writer must fail the same public read observation.
  await checkMutation('split-record-transaction', async () => {
    await h.raw('put', { key: b64(f.keys.metadata(Buffer.from('bad-writer'))), value: b64(valid) });
    await h.raw('put', { key: b64(f.keys.payload(Buffer.from('bad-writer'))), value: b64(bytes) });
    let result;
    try {
      result = await f.store.read(key('bad-writer'));
    } catch {
      result = { kind: 'store-failure' };
    }
    assert.equal(result.kind, 'found', 'atomic writer must publish a readable pair');
  });
};

async function hostClock(h, f) {
  await h.put(f, 'clock/a', Buffer.from([0, 255, 1]), 60000);
  await h.put(f, 'clock/b', Buffer.from([0, 255, 1]), 60000);
  const results = [];
  for (const offset of [0, -86400000, 864000000]) {
    const task = child(
      process.execPath,
      [
        '--require',
        path.join(__dirname, 'location-etcd-fixtures/date-control.cjs'),
        path.join(__dirname, 'location-etcd-fixtures/provider-process.cjs')
      ],
      {
        env: { ...process.env, ZLINK_ETCD_WALL_OFFSET: String(offset) },
        stdio: ['ignore', 'pipe', 'pipe', 'ipc']
      }
    );
    const message = receive(task);
    task.running.send({ options: h.options(f.namespace), subject: 'clock/a', prefix: 'clock/' });
    const result = await message;
    const exit = await task.exited;
    assert.equal(exit.code, 0, JSON.stringify(result) + exit.output);
    assert.equal(result.error, undefined);
    results.push(result.result);
  }
  assert.deepEqual(results[1], results[0]);
  assert.deepEqual(results[2], results[0]);
  const expiry = BigInt(Date.parse(results[0].read.value.expiresAt));
  await h.setClock(f, expiry + 1n);
  const expiredResults = [];
  for (const offset of [0, -86400000, 864000000]) {
    const task = child(
      process.execPath,
      [
        '--require',
        path.join(__dirname, 'location-etcd-fixtures/date-control.cjs'),
        path.join(__dirname, 'location-etcd-fixtures/provider-process.cjs')
      ],
      {
        env: { ...process.env, ZLINK_ETCD_WALL_OFFSET: String(offset) },
        stdio: ['ignore', 'pipe', 'pipe', 'ipc']
      }
    );
    const message = receive(task);
    task.running.send({ options: h.options(f.namespace), subject: 'clock/a', prefix: 'clock/' });
    const result = await message;
    const exit = await task.exited;
    assert.equal(exit.code, 0, JSON.stringify(result) + exit.output);
    assert.equal(result.result.read.kind, 'missing');
    assert.equal(result.result.write.kind, 'conflict');
    assert.equal(result.result.scan.value.items.length, 0);
    expiredResults.push(result.result);
  }
  assert.deepEqual(expiredResults[1], expiredResults[0]);
  assert.deepEqual(expiredResults[2], expiredResults[0]);
}
cases['etcd.provider-clock-independent'] = hostClock;

cases['etcd.manager-stop-restart-bound'] = async (h, f) => {
  const running = await managers(h, f);
  for (const manager of running) await h.stopManager(manager);
  const before = await h.clock(f);
  await delay(60000); // Required outage duration; no inference about RPC progress.
  assert.deepEqual(await h.clock(f), before);
  // Measure from the observation made during the outage, immediately before restarting.
  const started = performance.now();
  const restarted = await managers(h, f);
  const after = await h.clock(f);
  const elapsed = performance.now() - started;
  assert.ok(Number(after.value - before.value) <= 1.01 * 1.0005 * elapsed + 1010);
  for (const manager of restarted) await h.stopManager(manager);
};
cases['etcd.manager-delayed-write-bound'] = async (h, f) => {
  const proxy = await h.proxy(h.cluster.endpoints[0]);
  const file = path.join(h.directory, `${f.namespace}-held.json`);
  fs.writeFileSync(file, JSON.stringify(h.options(f.namespace, [proxy.endpoint], 60000)));
  const before = await h.clock(f);
  const manager = h.manager(file, `${f.namespace}-delayed`);
  try {
    await until(
      () => manager.events.some((e) => e.kind === 'sleep' && e.ms === 1000),
      'manager sampled Store clock'
    );
    await proxy.controlCall('hold-upstream');
    await until(
      async () => (await proxy.controlCall('status')).bufferedUpstream > 0,
      'management read held'
    );
    await delay(20000); // Deliberately suspend RPC progress for the specified interval.
    await proxy.controlCall('release');
    const after = await until(async () => {
      const c = await h.clock(f);
      return c.revision !== before.revision && c;
    }, 'delayed clock write');
    assert.ok(after.value - before.value <= 1010n);
  } finally {
    await proxy.controlCall('release');
    await h.stopManager(manager);
  }
};
cases['etcd.manager-wall-clock-jumps'] = async (h, f) => {
  const manager = h.manager(f.file, `${f.namespace}-wall`);
  let before = await h.clock(f);
  try {
    for (const offset of [-864000000, 864000000, 0]) {
      const started = performance.now();
      manager.running.send({ kind: 'wall', offset });
      await until(
        () => manager.events.some((e) => e.kind === 'wall-set' && e.offset === offset),
        'wall adjustment acknowledged'
      );
      const after = await until(async () => {
        const c = await h.clock(f);
        return c.revision !== before.revision && c;
      }, 'clock after wall jump');
      assert.ok(after.value >= before.value);
      assert.ok(
        Number(after.value - before.value) <= 1.01 * 1.0005 * (performance.now() - started) + 1010
      );
      before = after;
    }
  } finally {
    await h.stopManager(manager);
  }
};
cases['etcd.manager-three-process-frequency'] = async (h, f) => {
  const running = await managers(h, f, 3);
  const started = performance.now();
  let last = await h.clock(f);
  const samples = [{ ...last, at: performance.now() }];
  try {
    while (samples.length < 5) {
      const next = await until(async () => {
        const c = await h.clock(f);
        return c.revision !== last.revision && c;
      }, 'three-manager update');
      samples.push({ ...next, at: performance.now() });
      last = next;
    }
    assert.ok(
      performance.now() - started >= 3800,
      'three managers must not triple write frequency'
    );
    for (let i = 1; i < samples.length; i++)
      assert.ok(samples[i].value - samples[i - 1].value <= 1010n);
    fs.writeFileSync(
      path.join(logRoot, 'three-manager-samples.json'),
      JSON.stringify(
        samples,
        (_, value) => (typeof value === 'bigint' ? value.toString() : value),
        2
      )
    );
  } finally {
    for (const manager of running) await h.stopManager(manager);
  }
};
cases['etcd.namespace-initialization'] = async (h, f) => {
  const before = await h.clock(f);
  const result = await command(
    [path.join(pkg, 'dist/admin.js'), 'init', '--config', f.file],
    `${f.namespace}-second-init`,
    true
  );
  assert.equal(result.code, 2);
  assert.deepEqual(await h.clock(f), before);
  const namespace = `${f.namespace}-occupied`;
  await h.raw('put', { key: b64(`${namespace}\0anything`), value: b64('occupied') });
  const file = path.join(h.directory, `${namespace}.json`);
  fs.writeFileSync(file, JSON.stringify(h.options(namespace)));
  assert.equal(
    (
      await command(
        [path.join(pkg, 'dist/admin.js'), 'init', '--config', file],
        `${namespace}-init`,
        true
      )
    ).code,
    2
  );
  assert.equal(await h.get(encoding.keys(namespace).clock), undefined);
};
cases['etcd.clock-key-loss'] = async (h, f) => {
  await h.raw('delete_range', { key: b64(f.keys.clock) });
  for (const action of [
    () => f.store.read(key('a')),
    () => f.store.write(putRequest('a')),
    () => f.store.scan({ prefix: '', limit: 1 })
  ])
    await assert.rejects(action());
  const manager = h.manager(f.file, `${f.namespace}-missing`);
  const result = await manager.exited;
  assert.equal(result.code, 3);
  assert.equal(await h.get(f.keys.clock), undefined);
  await h.stopManager(manager);
};
cases['etcd.value-condition-mismatch'] = async (h, f) => {
  await h.put(f, 'subject', Buffer.from([0, 255, 1]));
  const before = await found(f.store, 'subject');
  for (const expected of [Buffer.from([0, 255, 2]), Buffer.from([0])]) {
    assert.equal(
      (
        await f.store.write({
          conditions: [{ kind: 'value', key: key('subject'), expected }],
          mutations: putRequest('marker').mutations
        })
      ).kind,
      'conflict'
    );
    await missing(f.store, 'marker');
    assert.deepEqual((await found(f.store, 'subject')).version, before.version);
  }
};
async function expiredVersion(h, f) {
  await h.put(f, 'subject', Buffer.from('x'), 1);
  const before = await found(f.store, 'subject');
  await advanceWithManager(h, f, BigInt(before.expiresAt.getTime()));
  assert.equal(
    (
      await f.store.write({
        conditions: [{ kind: 'version', key: key('subject'), expected: before.version }],
        mutations: putRequest('marker').mutations
      })
    ).kind,
    'conflict'
  );
  await missing(f.store, 'marker');
}
cases['etcd.expired-version-condition'] = expiredVersion;
cases['spi.expired-version-conflict'] = expiredVersion;
cases['etcd.expiry-before-apply'] = async (h, f) => {
  async function observation(bad) {
    await h.put(f, 'subject', Buffer.from('x'), 100);
    const before = await found(f.store, 'subject');
    const store = await heldApply(
      h,
      f,
      () => advanceWithManager(h, f, BigInt(before.expiresAt.getTime())),
      bad
    );
    const result = await store.write({
      conditions: [{ kind: 'version', key: key('subject'), expected: before.version }],
      mutations: putRequest(bad ? 'bad-marker' : 'marker').mutations
    });
    assert.equal(result.kind, 'conflict', 'Version must be live at apply');
    await missing(store, bad ? 'bad-marker' : 'marker');
  }
  await observation(false);
  await checkMutation('remove-apply-clock-comparison', () => observation(true));
};
cases['etcd.condition-change-before-apply'] = async (h, f) => {
  async function observation(immediateConflict) {
    const subject = immediateConflict ? 'bad-subject' : 'subject';
    const marker = `${subject}/marker`;
    await h.put(f, subject, Buffer.from('same'));
    const store = await heldApply(h, f, () => h.put(f, subject, Buffer.from('same')));
    let write = store.write.bind(store);
    if (immediateConflict) {
      // Bad provider variant: report Conflict on its first failed apply comparison.
      const stop = new Error('bad immediate conflict');
      h.wrapRpc(store, async ({ request }, send) => {
        const r = await send();
        if (request.compare?.length > 0 && !r.succeeded) throw stop;
        return r;
      });
      const original = write;
      write = async (request) => {
        try {
          return await original(request);
        } catch (e) {
          if (e !== stop) throw e;
          return { kind: 'conflict' };
        }
      };
    }
    applied(
      await write({
        conditions: [{ kind: 'value', key: key(subject), expected: Buffer.from('same') }],
        mutations: putRequest(marker).mutations
      })
    );
    await found(store, marker);
  }
  await observation(false);
  await checkMutation('immediate-conflict-on-failed-comparison', () => observation(true));
  await h.put(f, 'version-subject');
  const before = await found(f.store, 'version-subject');
  const store = await heldApply(h, f, () => h.put(f, 'version-subject', Buffer.from('changed')));
  assert.equal(
    (
      await store.write({
        conditions: [{ kind: 'version', key: key('version-subject'), expected: before.version }],
        mutations: putRequest('version-marker').mutations
      })
    ).kind,
    'conflict'
  );
  await missing(store, 'version-marker');
};

async function maximum(h, f, renew) {
  const mutations = Array.from({ length: 2048 }, (_, i) => ({
    kind: 'put',
    key: key(`max/${String(i).padStart(4, '0')}`),
    bytes: Buffer.alloc(i < 2047 ? 2039 : 2039)
  }));
  const bytes = mutations.reduce((n, m) => n + Buffer.byteLength(m.key.value) + m.bytes.length, 0);
  mutations[2047].bytes = Buffer.alloc(mutations[2047].bytes.length + 4194304 - bytes);
  assert.equal(
    mutations.reduce((n, m) => n + Buffer.byteLength(m.key.value) + m.bytes.length, 0),
    4194304
  );
  let running = [];
  if (renew) running = await managers(h, f, 3);
  const owner = h.store(f.namespace);
  await h.put({ ...f, store: owner }, 'owner', Buffer.from('owner'), 60000);
  const before = await found(owner, 'owner');
  try {
    const started = performance.now();
    const [result, renewed] = await Promise.all([
      f.store.write({ conditions: [], mutations }),
      owner
        .write({
          conditions: [{ kind: 'version', key: key('owner'), expected: before.version }],
          mutations: [
            { kind: 'put', key: key('owner'), bytes: Buffer.from('renewed'), retentionMs: 60000 }
          ]
        })
        .then((value) => ({ value, duration: performance.now() - started }))
    ]);
    applied(result);
    applied(renewed.value);
    assert.ok(renewed.duration < 3000, `renewal took ${renewed.duration}ms`);
    assert.equal(result.putVersions.length, 2048);
    assert.equal(new Set(result.putVersions.map((p) => p.version.value)).size, 1);
    const all = await pages(f.store, 'max/');
    assert.equal(all.length, 2048);
    assert.deepEqual(
      all.map((i) => Buffer.from(i.value.bytes)),
      mutations.map((m) => m.bytes)
    );
    // Exercise the worst-case 2,048 metadata conditions plus 4,096 writes.
    const conditions = result.putVersions.map((p) => ({
      kind: 'version',
      key: p.key,
      expected: p.version
    }));
    const adjusted = mutations.map((m) => ({ ...m, bytes: Buffer.alloc(1900) }));
    applied(await f.store.write({ conditions, mutations: adjusted }));
  } finally {
    for (const manager of running) await h.stopManager(manager);
  }
}
cases['etcd.maximum-batch-renew-latency'] = (h, f) => maximum(h, f, true);
cases['spi.maximum-atomic-batch'] = (h, f) => maximum(h, f, false);
cases['etcd.clock-updates-during-commit'] = async (h, f) => {
  await h.put(f, 'subject', Buffer.from('same'), 60000);
  const before = await found(f.store, 'subject');
  const observed = await h.clock(f);
  const store = await heldApply(h, f, async () => {
    const running = await managers(h, f, 3);
    try {
      await until(
        async () => (await h.clock(f)).value >= observed.value + 2000n,
        'multiple clock updates'
      );
    } finally {
      for (const manager of running) await h.stopManager(manager);
    }
  });
  const result = await store.write({
    conditions: [{ kind: 'version', key: key('subject'), expected: before.version }],
    mutations: putRequest('marker').mutations
  });
  applied(result);
  assert.equal(result.storeNow.getTime(), Number(observed.value));
};

cases['etcd.leader-loss-after-applied'] = async (h, f) => {
  const leader = await h.leader();
  const write = await h.put(f, 'subject', Buffer.from('durable'));
  try {
    await h.clusterCommand('kill', '--member', String(leader.index));
    const next = await h.leader();
    assert.notEqual(next.index, leader.index);
    const store = h.store(f.namespace, [next.endpoint]);
    assert.deepEqual((await found(store, 'subject')).version, write.putVersions[0].version);
  } finally {
    await h.clusterCommand('resume', '--member', String(leader.index));
  }
};
async function lostResult(h, f, badReplay = false, killLeader = true) {
  const leader = await h.leader();
  const proxy = await h.proxy(leader.endpoint);
  const store = h.store(f.namespace, [proxy.endpoint], 1500);
  const subject = badReplay ? 'bad-result' : 'result';
  let sentVersion;
  let sends = 0;
  interceptApply(h, store, async (request, send, operation) => {
    sends++;
    await proxy.controlCall('hold-downstream');
    const pending = send();
    pending.catch(() => {});
    const persisted = await until(async () => {
      const r = await h.get(f.keys.metadata(Buffer.from(subject)), leader.endpoint);
      return r;
    }, 'apply persisted while result held');
    sentVersion = encoding.encodeVersion(h.cluster.clusterId, persisted.mod_revision);
    if (killLeader) await h.clusterCommand('kill', '--member', String(leader.index));
    await proxy.controlCall('disconnect');
    await proxy.controlCall('release');
    try {
      return await pending;
    } catch (error) {
      if (!badReplay) throw error;
      const next = await h.leader();
      const {
        EtcdClient
      } = require('../../packages/framework-locations-etcd/dist/internal/client');
      const other = new EtcdClient(h.options(f.namespace, [next.endpoint]));
      sends++;
      try {
        return await other.rpc('KV', 'txn', request, other.operation(operation.signal));
      } finally {
        other.close();
      }
    }
  });
  try {
    await assert.rejects(store.write(putRequest(subject)), 'lost result must remain unknown');
    assert.equal(sends, 1, 'unknown write must not be retransmitted');
    const next = await h.leader();
    const reader = h.store(f.namespace, [next.endpoint]);
    assert.equal((await found(reader, subject)).version.value, sentVersion);
    assert.equal(
      (
        await reader.write({
          conditions: [{ kind: 'missing', key: key(subject) }],
          mutations: putRequest('duplicate').mutations
        })
      ).kind,
      'conflict'
    );
    await missing(reader, 'duplicate');
  } finally {
    await proxy.controlCall('release');
    if (killLeader) await h.clusterCommand('resume', '--member', String(leader.index));
  }
}
cases['etcd.leader-loss-before-result'] = async (h, f) => {
  await lostResult(h, f);
  await checkMutation('retransmit-unknown-transaction', () => lostResult(h, f, true));
};
cases['spi.unknown-commit-reconstruction'] = async (h, f) => {
  await lostResult(h, f, false, false);
  const proxy = await h.proxy(h.cluster.endpoints[0]);
  const store = h.store(f.namespace, [proxy.endpoint]);
  const stop = new AbortController();
  interceptApply(h, store, async (_request, send) => {
    await proxy.controlCall('hold-downstream');
    const response = send();
    response.catch(() => {});
    await until(
      () => h.get(f.keys.metadata(Buffer.from('cancelled'))),
      'cancelled transaction persisted'
    );
    stop.abort();
    await proxy.controlCall('release');
    return response;
  });
  await assert.rejects(store.write(putRequest('cancelled'), stop.signal), /stopped/);
  await found(f.store, 'cancelled');
};
cases['etcd.minority-isolation'] = async (h, f) => {
  const leader = await h.leader();
  const isolated = h.cluster.members.find((m) => m.index !== leader.index);
  await h.put(f, 'subject', Buffer.from('old'));
  await until(async () => {
    const r = await h.get(f.keys.payload(Buffer.from('subject')), isolated.endpoint);
    return r && Buffer.from(r.value, 'base64').toString() === 'old';
  }, 'follower caught up');
  await h.clusterCommand('partition', '--member', String(isolated.index));
  try {
    const majority = h.store(f.namespace, [leader.endpoint]);
    await h.put({ ...f, store: majority }, 'subject', Buffer.from('new'));
    const store = h.store(f.namespace, [isolated.endpoint], 700);
    await assert.rejects(store.read(key('subject')));
    await assert.rejects(store.write(putRequest('minority-write')));
    await checkMutation('serializable-read', async () => {
      const bad = h.store(f.namespace, [isolated.endpoint], 700);
      h.wrapRpc(bad, ({ request }, send) => {
        for (const op of request.success || [])
          if (op.request_range) op.request_range.serializable = true;
        return send();
      });
      await assert.rejects(
        bad.read(key('subject')),
        'minority read must fail instead of returning old bytes'
      );
    });
  } finally {
    await h.clusterCommand('heal');
  }
};
cases['etcd.quota-exhaustion'] = async (h, f) => {
  const prefix = Buffer.from(`${f.namespace}/quota/`);
  const anchor = Buffer.from(`${f.namespace}/quota-anchor`);
  await h.raw('put', { key: b64(anchor), value: b64('anchor') });
  let failure;
  try {
    for (let i = 0; i < 256; i++) {
      try {
        await h.raw('put', {
          key: b64(Buffer.concat([prefix, Buffer.from(String(i))])),
          value: b64(Buffer.alloc(1048576, i))
        });
      } catch (error) {
        failure = error;
        break;
      }
    }
    assert.ok(failure, 'fill actual backend quota');
    await assert.rejects(h.put(f, 'quota-marker'));
  } finally {
    const deleted = await h.raw('delete_range', {
      key: b64(prefix),
      range_end: b64(encoding.prefixEnd(prefix))
    });
    assert.ok(Number(deleted.deleted) > 0);
    const remaining = await h.raw('range', {
      key: b64(prefix),
      range_end: b64(encoding.prefixEnd(prefix)),
      count_only: true
    });
    assert.equal(Number(remaining.count || 0), 0);
    // Compact beyond the deletion revision, so the tombstoned payload versions can be reclaimed.
    await h.raw('delete_range', { key: b64(anchor) });
    const revision = (await h.raw('range', { key: b64(f.keys.clock) })).header.revision;
    await rpc(h.cluster.endpoints[0], '/v3/kv/compaction', { revision, physical: true }, 30000);
    const statuses = [];
    for (const endpoint of h.cluster.endpoints) {
      await until(
        async () => {
          const hash = await rpc(endpoint, '/v3/maintenance/hashkv');
          return BigInt(hash.compact_revision) >= BigInt(revision);
        },
        'member physical compaction',
        30000
      );
      await rpc(endpoint, '/v3/maintenance/defragment', {}, 30000);
      const status = await rpc(endpoint, '/v3/maintenance/status');
      assert.ok(BigInt(status.dbSize) < BigInt(status.dbSizeQuota));
      statuses.push(status);
    }
    const alarms = await rpc(h.cluster.endpoints[0], '/v3/maintenance/alarm', { action: 'GET' });
    for (const alarm of alarms.alarms || [])
      await rpc(h.cluster.endpoints[0], '/v3/maintenance/alarm', {
        action: 'DEACTIVATE',
        memberID: alarm.memberID,
        alarm: alarm.alarm
      });
    const cleared = await rpc(h.cluster.endpoints[0], '/v3/maintenance/alarm', { action: 'GET' });
    assert.equal((cleared.alarms || []).length, 0);
    fs.writeFileSync(
      path.join(logRoot, `${f.namespace}-quota-recovery.json`),
      JSON.stringify({ deleted, remaining, statuses, alarms, cleared }, null, 2)
    );
    applied(await h.put(f, 'quota-recovered'));
  }
};

cases['etcd.snapshot-stability'] = async (h, f) => {
  for (const subject of ['snapshot/a', 'snapshot/b', 'snapshot/c'])
    await h.put(f, subject, Buffer.from(subject), 10000);
  async function observation(bad) {
    const leader = await h.leader();
    const survivor = h.cluster.members.find((member) => member.index !== leader.index);
    const store = h.store(f.namespace, [survivor.endpoint]);
    const first = await store.scan({ prefix: 'snapshot/', limit: 1 });
    assert.equal(first.kind, 'page');
    const expected = await found(store, 'snapshot/b');
    if (bad) {
      h.wrapRpc(store, ({ method, request }, send) => {
        if (method === 'range') delete request.revision;
        for (const op of request.success || [])
          if (op.request_range) delete op.request_range.revision;
        return send();
      });
    }
    await h.put(f, 'snapshot/b', Buffer.from('changed'));
    await f.store.write({
      conditions: [],
      mutations: [{ kind: 'delete', key: key('snapshot/c') }]
    });
    await h.setClock(f, BigInt(expected.expiresAt.getTime()) + 1n);
    try {
      await h.clusterCommand('kill', '--member', String(leader.index));
      const next = await h.leader();
      assert.notEqual(next.index, leader.index);
      const second = await store.scan({
        prefix: 'snapshot/',
        limit: 10,
        cursor: first.value.nextCursor
      });
      assert.equal(second.kind, 'page');
      assert.equal(second.value.storeNow.getTime(), first.value.storeNow.getTime());
      assert.deepEqual(
        second.value.items.map((i) => i.key.value),
        ['snapshot/b', 'snapshot/c']
      );
      assert.deepEqual(
        second.value.items[0].value.bytes,
        expected.bytes,
        'every page must read revision R'
      );
    } finally {
      await h.clusterCommand('resume', '--member', String(leader.index));
      // Docker restart may assign a different host port; setup writes need a fresh instance.
      f.store = h.store(f.namespace);
    }
  }
  await observation(false);
  // Restore a fresh live set for the intentionally latest-revision variant.
  for (const subject of ['snapshot/a', 'snapshot/b', 'snapshot/c'])
    await h.put(f, subject, Buffer.from(subject), 10000);
  await checkMutation('scan-latest-revision', () => observation(true));
};
cases['etcd.expired-prefix-pagination'] = async (h, f) => {
  const clock = (await h.clock(f)).value;
  const success = [];
  for (let i = 0; i < 1100; i++) {
    const subject = `prefix/${String(i).padStart(4, '0')}`;
    success.push(
      {
        request_put: {
          key: b64(f.keys.metadata(Buffer.from(subject))),
          value: b64(
            encoding.encodeMetadata({ hasExpiry: true, expiresAtMs: clock, payloadLength: 1 })
          )
        }
      },
      { request_put: { key: b64(f.keys.payload(Buffer.from(subject))), value: b64('x') } }
    );
  }
  await h.raw('txn', { success });
  await h.put(f, 'prefix/z', Buffer.from('live'));
  assert.deepEqual(
    (await pages(f.store, 'prefix/', 1)).map((i) => i.key.value),
    ['prefix/z']
  );
};
cases['etcd.maximum-cursor'] = async (h, f) => {
  const namespace = 'n'.repeat(64);
  const file = path.join(h.directory, 'maximum-namespace.json');
  // Reuse this fixed namespace only after deleting the previous repetition's records.
  const keys = encoding.keys(namespace);
  await h.raw('delete_range', {
    key: b64(keys.namespacePrefix),
    range_end: b64(encoding.prefixEnd(keys.namespacePrefix))
  });
  fs.writeFileSync(file, JSON.stringify(h.options(namespace)));
  await command(
    [path.join(pkg, 'dist/admin.js'), 'init', '--config', file],
    'maximum-namespace-init'
  );
  const store = h.store(namespace);
  const prefix = '가'.repeat(341);
  for (const suffix of ['a', 'b'])
    await h.put({ ...f, store }, prefix + suffix, Buffer.from(suffix));
  const first = await store.scan({ prefix, limit: 1 });
  assert.equal(first.kind, 'page');
  assert.ok(Buffer.byteLength(first.value.nextCursor.value) <= 4096);
  const second = await store.scan({ prefix, limit: 1, cursor: first.value.nextCursor });
  assert.equal(second.value.items[0].key.value, prefix + 'b');
};
cases['etcd.compacted-snapshot'] = async (h, f) => {
  for (const subject of ['a', 'b']) await h.put(f, subject);
  const first = await f.store.scan({ prefix: '', limit: 1 });
  await h.put(f, 'later');
  const revision = (await h.raw('range', { key: b64(f.keys.clock) })).header.revision;
  await h.clusterCommand('compact', '--revision', revision);
  assert.deepEqual(await f.store.scan({ prefix: '', limit: 1, cursor: first.value.nextCursor }), {
    kind: 'expired'
  });
  // Compaction between metadata and payload must discard that entire page.
  const store = h.store(f.namespace);
  let compact = true;
  h.wrapRpc(store, async ({ method }, send) => {
    const response = await send();
    if (compact && method === 'range') {
      compact = false;
      await h.put(f, 'advance');
      const r = (await h.raw('range', { key: b64(f.keys.clock) })).header.revision;
      await h.clusterCommand('compact', '--revision', r);
    }
    return response;
  });
  assert.deepEqual(await store.scan({ prefix: '', limit: 1 }), { kind: 'expired' });
};
cases['etcd.expiry-cleanup-race'] = async (h, f) => {
  const now = (await h.clock(f)).value;
  await h.pair(f, 'expired', Buffer.from('old'), now);
  await h.pair(f, 'rewritten', Buffer.from('old'), now);
  const proxy = await h.proxy(h.cluster.endpoints[0]);
  // The real cleanup CLI is stopped before its conditional deletion RPC by a test preloader.
  const preload = path.join(h.directory, 'cleanup-boundary.cjs');
  fs.writeFileSync(
    preload,
    `const {EtcdClient}=require(${JSON.stringify(path.join(pkg, 'dist/internal/client'))});const rpc=EtcdClient.prototype.rpc;EtcdClient.prototype.rpc=async function(service,method,request,operation){if(method==='txn'&&request.success?.some(x=>x.request_delete_range)&&!global.held){global.held=true;process.send({kind:'before-delete'});await new Promise(r=>process.once('message',r));}return rpc.call(this,service,method,request,operation);};`
  );
  const file = path.join(h.directory, `${f.namespace}-cleanup.json`);
  fs.writeFileSync(file, JSON.stringify(h.options(f.namespace, [proxy.endpoint])));
  const task = child(
    process.execPath,
    ['--require', preload, path.join(pkg, 'dist/admin.js'), 'cleanup', '--config', file],
    { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] }
  );
  await receive(task);
  await proxy.controlCall('hold-upstream');
  task.running.send({ release: true });
  await until(
    async () => (await proxy.controlCall('status')).bufferedUpstream > 0,
    'cleanup delete held'
  );
  await h.put(f, 'rewritten', Buffer.from('new'), 60000);
  await proxy.controlCall('release');
  const result = await task.exited;
  assert.equal(result.code, 0, result.output);
  assert.equal(await h.get(f.keys.metadata(Buffer.from('expired'))), undefined);
  assert.equal(await h.get(f.keys.payload(Buffer.from('expired'))), undefined);
  assert.equal(Buffer.from((await found(f.store, 'rewritten')).bytes).toString(), 'new');
};

cases['spi.exact-read-observation'] = async (h, f) => {
  const before = await h.clock(f);
  const bytes = Buffer.from([0, 255, 128]);
  const result = await h.put(f, 'subject', bytes, 1234.1);
  applied(result);
  const value = await found(f.store, 'subject');
  assert.deepEqual(Buffer.from(value.bytes), bytes);
  assert.deepEqual(value.version, result.putVersions[0].version);
  assert.equal(value.storeNow.getTime(), Number(before.value));
  assert.equal(value.expiresAt.getTime(), Number(before.value) + 1235);
  const m = await h.get(f.keys.metadata(Buffer.from('subject')));
  const p = await h.get(f.keys.payload(Buffer.from('subject')));
  assert.equal(m.mod_revision, p.mod_revision);
  assert.equal(value.version.value, encoding.encodeVersion(h.cluster.clusterId, m.mod_revision));
};
cases['spi.expired-and-durable'] = async (h, f) => {
  await h.put(f, 'expiring', Buffer.from('x'), 1);
  await h.put(f, 'durable', Buffer.from('durable'));
  const value = await found(f.store, 'expiring');
  await advanceWithManager(h, f, BigInt(value.expiresAt.getTime()));
  await missing(f.store, 'expiring');
  await found(f.store, 'durable');
  applied(
    await f.store.write({ conditions: [], mutations: [{ kind: 'delete', key: key('durable') }] })
  );
  await missing(f.store, 'durable');
};
cases['spi.atomic-conflict'] = async (h, f) => {
  await h.put(f, 'existing', Buffer.from('before'));
  const before = await found(f.store, 'existing');
  assert.equal(
    (
      await f.store.write({
        conditions: [
          { kind: 'missing', key: key('absent') },
          { kind: 'missing', key: key('existing') }
        ],
        mutations: [
          { kind: 'put', key: key('existing'), bytes: Buffer.from('after') },
          { kind: 'put', key: key('marker'), bytes: Buffer.from('marker') }
        ]
      })
    ).kind,
    'conflict'
  );
  assert.deepEqual(await found(f.store, 'existing'), before);
  await missing(f.store, 'marker');
};
cases['spi.value-condition-identity'] = async (h, f) => {
  const bytes = Buffer.from([0, 255, 17]);
  const condition = { kind: 'value', key: key('subject'), expected: bytes };
  const write = () =>
    f.store.write({ conditions: [condition], mutations: putRequest('marker').mutations });
  const first = await h.put(f, 'subject', bytes, 60000);
  const renewed = await h.put(f, 'subject', bytes, 120000);
  assert.notDeepEqual(first.putVersions, renewed.putVersions);
  applied(await write());
  for (const change of [
    () => h.put(f, 'subject', Buffer.from([0, 255, 18])),
    () => f.store.write({ conditions: [], mutations: [{ kind: 'delete', key: key('subject') }] }),
    async () => {
      await h.put(f, 'subject', bytes, 1);
      await advanceWithManager(h, f, BigInt((await found(f.store, 'subject')).expiresAt.getTime()));
    }
  ]) {
    await f.store.write({ conditions: [], mutations: [{ kind: 'delete', key: key('marker') }] });
    await change();
    assert.equal((await write()).kind, 'conflict');
    await missing(f.store, 'marker');
  }
};
cases['spi.snapshot-validity'] = async (h, f) => {
  await h.put(f, 'a');
  await h.put(f, 'b');
  const first = await f.store.scan({ prefix: '', limit: 1 });
  await h.put(f, 'b', Buffer.from('changed'));
  assert.equal(
    Buffer.from(
      (await f.store.scan({ prefix: '', limit: 1, cursor: first.value.nextCursor })).value.items[0]
        .value.bytes
    ).toString(),
    'x'
  );
  const other = await h.namespace('other-cursor');
  assert.deepEqual(
    await other.store.scan({ prefix: '', limit: 1, cursor: first.value.nextCursor }),
    { kind: 'expired' }
  );
  for (const value of ['invalid', 'x'.repeat(4096)])
    assert.deepEqual(await f.store.scan({ prefix: '', limit: 1, cursor: { value } }), {
      kind: 'expired'
    });
  // Move only the encoded monotonic start into the past; the snapshot must expire.
  const bytes = Buffer.from(first.value.nextCursor.value, 'base64url');
  const old = process.hrtime.bigint() / 1000000n - 300001n;
  bytes.writeBigUInt64BE(old, 41);
  assert.deepEqual(
    await f.store.scan({ prefix: '', limit: 1, cursor: { value: bytes.toString('base64url') } }),
    { kind: 'expired' }
  );
};
cases['spi.snapshot-clock'] = async (h, f) => {
  await hostClock(h, f);
  await h.put(f, 'clock/a', Buffer.from('a'), 60000);
  await h.put(f, 'clock/b', Buffer.from('b'), 60000);
  const first = await f.store.scan({ prefix: 'clock/', limit: 1 });
  const expected = await found(f.store, 'clock/b');
  await h.setClock(f, BigInt(expected.expiresAt.getTime()) + 1n);
  const second = await f.store.scan({ prefix: 'clock/', limit: 1, cursor: first.value.nextCursor });
  assert.equal(second.kind, 'page');
  assert.equal(second.value.items.length, 1);
  assert.equal(second.value.storeNow.getTime(), first.value.storeNow.getTime());
  await missing(f.store, 'clock/b');
};
cases['spi.cursor-page-bounds'] = async (h, f) => {
  for (let i = 0; i < 5; i++) await h.put(f, `page/${i}`, Buffer.alloc(1048576, i));
  assert.deepEqual(
    (await pages(f.store, 'page/', 1000)).map((i) => i.key.value),
    ['page/0', 'page/1', 'page/2', 'page/3', 'page/4']
  );
  for (const limit of [0, 1001])
    await assert.rejects(f.store.scan({ prefix: '', limit }), RangeError);
  for (const value of ['', 'x'.repeat(4097)])
    await assert.rejects(f.store.scan({ prefix: '', limit: 1, cursor: { value } }), RangeError);
};

// Explicit opt-in keeps Docker lifecycle and the required 60-second outage out of ordinary unit runs.
if (process.env.ZLINK_ETCD_CONFORMANCE === '1')
  test('Node etcd Location Store conformance', { timeout: 900000 }, async (t) => {
    const h = new Harness();
    const report = { language: 'node', cases: [] };
    const results = path.join(logRoot, 'node-conformance.json');
    let started = false;
    try {
      await h.start();
      started = true;
      const selected = process.env.ZLINK_ETCD_CASES?.split(',');
      if (selected)
        for (const id of selected)
          assert.ok(
            inventory.cases.some((c) => c.id === id && c.level !== 'cross-language'),
            `unknown executable case: ${id}`
          );
      for (const item of inventory.cases.filter(
        (c) => c.level !== 'cross-language' && (!selected || selected.includes(c.id))
      )) {
        const repetitions =
          item.level === 'fault-injection'
            ? Number(process.env.ZLINK_ETCD_FAULT_REPETITIONS || 3)
            : 1;
        assert.ok(
          Number.isSafeInteger(repetitions) && repetitions >= 1,
          'repetition count must be positive'
        );
        let passed = true;
        for (let repetition = 1; repetition <= repetitions; repetition++) {
          await t.test(
            `${item.id} (${repetition}/${repetitions})`,
            { timeout: 180000 },
            async () => {
              try {
                assert.equal(
                  typeof cases[item.id],
                  'function',
                  'every inventory ID needs an executable observation'
                );
                const f = await h.namespace(`${item.id.replaceAll('.', '-')}-${repetition}`);
                if (item.requiresStoreManager) {
                  const running = await managers(h, f);
                  for (const manager of running) await h.stopManager(manager);
                }
                await cases[item.id](h, f);
              } catch (error) {
                passed = false;
                throw error;
              } finally {
                // A test must not leave any management process running for the next test.
                for (const manager of [...h.managers]) await h.stopManager(manager);
              }
            }
          );
          if (!passed) break; // Preserve the first real failure; do not use a retry as evidence.
        }
        report.cases.push({ id: item.id, status: passed ? 'passed' : 'failed' });
        fs.writeFileSync(results, JSON.stringify(report, null, 2));
        fs.writeFileSync(
          path.join(logRoot, 'mutation-results.json'),
          JSON.stringify(mutationResults, null, 2)
        );
        if (!passed) break;
      }
    } finally {
      if (started) await h.close();
      else if (fs.existsSync(path.join(h.run, 'cluster.json'))) {
        h.refresh();
        await h.close();
      }
      const coverage = await command(
        [
          path.join(root, 'scripts/etcd/check-conformance-coverage.mjs'),
          '--language',
          'node',
          results
        ],
        'coverage',
        true
      );
      console.log(`coverage exit=${coverage.code}: ${coverage.output.trim()}`);
      console.log('#4082: etcd.cross-language-record, etcd.cross-language-version-condition');
      console.log(
        `conformance summary: ${report.cases.filter((c) => c.status === 'passed').length}/33 passed; ${mutationResults.length} mutant observations rejected`
      );
    }
  });

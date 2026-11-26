const assert = require('node:assert/strict');
const test = require('node:test');
const {
  RelocationCompletionStore
} = require('../../packages/framework/dist/runtime/locations/relocation-completion-store');
const {
  decodeRelocationManifestV1
} = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const {
  encodeAuthorityAggregate,
  decodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record');
const { completionFixture: fixture, example, entry } = require('./helpers/relocation-completion');

test('authority metadata preserves completion with decimal counters and rejects invalid counts', () => {
  const f = fixture();
  const relocation = f.current().relocation;
  assert.deepEqual(
    decodeAuthorityAggregate(encodeAuthorityAggregate(undefined, relocation)).relocation,
    relocation
  );
  for (const completion of [
    { ...relocation.completion, completedResults: 2n },
    { ...relocation.completion, pendingDeliveries: 1n },
    { ...relocation.completion, reference: 'unpaired' }
  ])
    assert.throws(
      () => encodeAuthorityAggregate(undefined, { ...relocation, completion }),
      /DataLost/
    );
});

test('Put and readback precede single reference/count CAS; terminal replaces immutable payload once', async () => {
  const f = fixture();
  const published = await f.owner.append(f.key, f.current(), entry);
  assert.deepEqual(f.events, ['put', 'read', 'put', 'read', 'cas']);
  assert.equal(published.relocation.completion.completedResults, 1n);
  assert.equal(published.relocation.completion.pendingDeliveries, 1n);
  assert.throws(() => f.owner.requireCompleted(published), /DataLost/);
  const root = published.relocation.completion.reference;
  const acknowledged = await f.owner.acknowledge(f.key, published, entry, 'terminalReceived');
  assert.notEqual(acknowledged.relocation.completion.reference, root);
  assert.equal(acknowledged.relocation.completion.pendingDeliveries, 0n);
  f.owner.requireCompleted(acknowledged);
  assert.equal(f.blobs.has(root), true, 'old immutable payload remains under retention');
  const puts = f.events.filter((x) => x === 'put').length;
  await f.owner.acknowledge(f.key, acknowledged, entry, 'alreadyTerminal');
  assert.equal(f.events.filter((x) => x === 'put').length, puts);
  assert.deepEqual(
    (await new RelocationCompletionStore(f.authorities, f.store).read(acknowledged)).entries,
    [{ ...entry, delivery: 'terminalReceived' }]
  );
});

test('Put failure and mismatched readback never CAS', async () => {
  for (const mode of ['put', 'read']) {
    const f = fixture();
    if (mode === 'put')
      f.store.put = async () => {
        throw new TypeError('injected Put rejection');
      };
    else
      f.store.read = async () => ({
        kind: 'found',
        bytes: Buffer.from('corrupt'),
        storeNow: new Date(0),
        expiresAt: new Date(1)
      });
    await assert.rejects(
      f.owner.append(f.key, f.current(), entry),
      mode === 'put' ? /injected/ : /DataLost/
    );
    assert.equal(f.events.includes('cas'), false);
    assert.equal(f.current().relocation.completion.reference, null);
  }
});

test('CAS Conflict leaves only unreachable payload and the previous authoritative root', async () => {
  const f = fixture();
  f.authorities.compareExchangeAuthority = async () => ({ kind: 'conflict' });
  await assert.rejects(f.owner.append(f.key, f.current(), entry), /CAS conflict/);
  assert.equal(f.current().relocation.completion.reference, null);
  assert.equal(f.blobs.size, 2);
});

test('reopening the completion reader after CAS preserves exact bytes without handlers', async () => {
  for (let iteration = 0; iteration < 40; iteration++) {
    const f = fixture();
    const current = await f.owner.append(f.key, f.current(), entry);
    const resumed = new RelocationCompletionStore(f.authorities, f.store);
    const record = await resumed.read(current);
    assert.deepEqual(record.entries, [entry]);
    await resumed.append(f.key, current, entry);
    assert.equal(f.current().relocation.completion.completedResults, 1n);
    for (const changed of [
      { ...entry, source: { ...entry.source, nodeGeneration: '99' } },
      { ...entry, replyRouteId: '6' }
    ])
      await assert.rejects(
        resumed.acknowledge(f.key, current, changed, 'terminalReceived'),
        /DataLost/
      );
    const terminal = await resumed.acknowledge(f.key, current, entry, 'sourceLeaseExpired');
    await resumed.acknowledge(f.key, terminal, entry, 'terminalReceived');
    assert.equal(f.current().relocation.completion.pendingDeliveries, 0n);
  }
});

test('published missing blob, checksum, identity and count mismatch are DataLost', async () => {
  for (const mode of ['missing', 'chunk', 'sha256', 'count', 'identity']) {
    const f = fixture();
    const current = await f.owner.append(f.key, f.current(), entry);
    if (mode === 'missing') f.blobs.delete(current.relocation.completion.reference);
    if (mode === 'chunk') {
      const manifest = decodeRelocationManifestV1(
        f.blobs.get(current.relocation.completion.reference)
      );
      f.blobs.set(manifest.chunks[0].reference, Buffer.from('corrupt'));
    }
    if (mode === 'sha256') current.relocation.completion.sha256 = '00'.repeat(32);
    if (mode === 'count') current.relocation.completion.pendingDeliveries = 0n;
    if (mode === 'identity')
      current.relocation.relocationId = '00000000-0000-0001-0000-000000000002';
    await assert.rejects(f.owner.read(current), /DataLost/);
  }
});

test('pending retention renews chunks before the manifest using provider clock', async () => {
  const f = fixture();
  const current = await f.owner.append(f.key, f.current(), entry);
  const root = current.relocation.completion.reference;
  f.advance();
  await f.owner.read(current);
  const renewals = f.events.filter((x) => x.startsWith('renew:'));
  assert.equal(renewals.length, 2);
  assert.equal(renewals[1], `renew:${root}`);
});

test('lost CAS response is reconciled by reading its exact published reference', async () => {
  const f = fixture();
  const original = f.authorities.compareExchangeAuthority;
  f.authorities.readAuthority = async () => f.current();
  f.authorities.compareExchangeAuthority = async (...args) => {
    await original(...args);
    throw new Error('injected lost CAS response');
  };
  const published = await f.owner.append(f.key, f.current(), entry);
  assert.equal(published.relocation.completion.completedResults, 1n);
  assert.deepEqual((await f.owner.read(published)).entries, [entry]);
});

test('target commit fixes capture plus pre-boundary request counts, excluding Send and timers', async () => {
  const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
  const {
    encodeAuthorityKey
  } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
  const key = encodeAuthorityKey('actor', 'actor');
  const stage = {
    phase: 'ready',
    cutoverReceived: true,
    offer: { prepare: { target: {} } },
    boundaryRelay: [1, 0].map((kind) => ({
      object: { actorId: 'actor' },
      frozenRecord: { canonicalBytes: Buffer.from([kind]) }
    })),
    owner: {
      async replayRelayedMessage(_staging, objectKey, message) {
        assert.equal(objectKey, key.value);
        stage.staging.hidden
          .get(key.value)
          .replayPackets.push({ returnResponse: message.payload[0] === 1 });
      }
    },
    staging: {
      envelope: { aggregateId: example.relocationId },
      primaryAuthorityKey: key,
      hidden: new Map([
        [key.value, { replayPackets: [{ returnResponse: true }, { returnResponse: false }] }]
      ])
    }
  };
  let calls = 0;
  const authority = { storeVersion: { value: 'committed' } };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      async commitAggregate(fence) {
        calls++;
        assert.equal(fence.acceptedRequests.get(key.value), 2n);
        return { kind: 'committed' };
      }
    })
  });
  runtime.readAggregateForCommitRetry = async () => ({
    kind: 'committed',
    authorities: new Map([[key.value, authority]])
  });
  assert.equal(
    await runtime.commitTargetReservation(stage, { prepared: { fence: {}, plan: {} } }),
    authority
  );
  assert.equal(calls, 1);
  assert.equal(stage.boundaryRelay.length, 0);
});

test('pending delivery blocks another Relocate and logs the durable reason', async () => {
  const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
  const f = fixture();
  const current = await f.owner.append(f.key, f.current(), entry);
  const runtime = new ZLinkHostServiceRelocationRuntime({
    currentOwner: () => ({ ownerId: 'target', leaseGeneration: 1n }),
    locationStore: () => ({
      async listAuthorities() {
        return { kind: 'page', items: [{ key: f.key, snapshot: current }] };
      }
    })
  });
  assert.equal(await runtime.hasPendingDeliveries(), true);
  current.relocation.completion.pendingDeliveries = 0n;
  assert.equal(await runtime.hasPendingDeliveries(), false);
});

test('pending delivery scan restarts from the first page when the scan cursor expires', async () => {
  const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
  const f = fixture();
  const current = await f.owner.append(f.key, f.current(), entry);
  let starts = 0,
    expired = false;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    currentOwner: () => ({ ownerId: 'target', leaseGeneration: 1n }),
    locationStore: () => ({
      async listAuthorities(_prefix, cursor) {
        if (cursor === undefined) {
          starts++;
          return { kind: 'page', items: [], nextCursor: 'second' };
        }
        if (!expired) {
          expired = true;
          return { kind: 'scanExpired' };
        }
        return { kind: 'page', items: [{ key: f.key, snapshot: current }] };
      }
    })
  });
  assert.equal(await runtime.hasPendingDeliveries(), true);
  assert.equal(expired, true);
  assert.equal(starts, 2);
  const aborted = new AbortController();
  aborted.abort();
  expired = false;
  await assert.rejects(runtime.hasPendingDeliveries(aborted.signal), { name: 'AbortError' });
});

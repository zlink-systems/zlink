const assert = require('node:assert/strict');
const test = require('node:test');
const { replyFixture, defaultIdentity } = require('./helpers/relocation-reply');
const {
  decodeRelocationReplyRecord,
  encodeRelocationReplyRecord,
  relocationReplyKey
} = require('../../packages/framework/dist/runtime/locations/relocation-reply-record');

const hour = 3_600_000;
const reply = Buffer.from('immutable terminal result').toString('base64');
const failure = Buffer.from('requestFailed').toString('base64');

async function seeded() {
  const fixture = replyFixture();
  await fixture.setWriter(fixture.writer);
  await fixture.seed();
  return fixture;
}

test('row codec uses canonical JSON and rejects corrupt logical identity', async () => {
  const f = await seeded();
  const [row] = await f.rows();
  const key = relocationReplyKey(row);
  assert.equal(
    key,
    '$zlink/relocation-reply/00000000-0000-0000-0000-000000000001/owner-A.9.41.7/00000000000000070000000000000001'
  );
  const bytes = encodeRelocationReplyRecord(row);
  assert.equal(
    bytes.toString(),
    '{"formatVersion":1,"objectKey":"zla1:a:5:actor","operationId":"00000000000000070000000000000001","relocationId":"00000000-0000-0000-0000-000000000001","replyRouteId":"1","source":{"leaseGeneration":"9","nodeGeneration":"7","nodeRid":"41","ownerId":"owner-A"},"state":"accepted","writer":{"leaseGeneration":"1","ownerId":"target"}}'
  );
  assert.deepEqual(decodeRelocationReplyRecord(bytes, key), row);
  assert.throws(
    () => decodeRelocationReplyRecord(bytes, key.replace('owner-A', 'owner-B')),
    /DataLost/
  );
  assert.throws(
    () => decodeRelocationReplyRecord(Buffer.concat([bytes, Buffer.from(' ')]), key),
    /DataLost/
  );
  assert.deepEqual(await f.rows('00000000-0000-0000-0000-000000000002'), []);
});

test('lost result CAS response rereads the same row; a later result cannot replace its bytes', async () => {
  const f = await seeded();
  const write = f.provider.write.bind(f.provider);
  let attempts = 0;
  f.provider.write = async (...args) => {
    attempts++;
    assert.equal((await write(...args)).kind, 'applied');
    throw new Error('result CAS response lost');
  };
  const observed = await f.store.complete(defaultIdentity, f.writer, reply, () => failure);
  assert.equal(observed.state, 'pending');
  assert.equal(observed.reply, reply);
  assert.equal(attempts, 1);
  const duplicate = await f.store.complete(defaultIdentity, f.writer, failure, () => failure);
  assert.equal(duplicate.reply, reply);
  assert.equal(attempts, 1);
});

for (const evidence of ['terminalReceived', 'alreadyTerminal', 'sourceLeaseExpired']) {
  test(`pending row takes ${evidence} once and expires after five minutes`, async () => {
    const f = await seeded();
    await f.store.complete(defaultIdentity, f.writer, reply, () => failure);
    await f.store.acknowledge(defaultIdentity, f.writer, evidence);
    await f.store.acknowledge(defaultIdentity, f.writer, evidence);
    await f.store.acknowledge(
      defaultIdentity,
      f.writer,
      evidence === 'alreadyTerminal' ? 'terminalReceived' : 'alreadyTerminal'
    );
    f.advance(299_999);
    const [row] = await f.rows();
    assert.equal(row.state, evidence);
    assert.equal(row.reply, reply);
    f.advance(1);
    assert.deepEqual(await f.rows(), []);
  });
}

test('writer lease is fenced in the CAS even if it expires after the read', async () => {
  const f = await seeded();
  const write = f.provider.write.bind(f.provider);
  f.provider.write = async (request, signal) => {
    await write({
      conditions: [],
      mutations: [{ kind: 'delete', key: { value: 'test-lease:target' } }]
    });
    return write(request, signal);
  };
  assert.equal(await f.store.complete(defaultIdentity, f.writer, reply, () => failure), undefined);
  assert.equal((await f.rows())[0].state, 'accepted');
  f.provider.write = write;
  const successor = { ...f.writer, leaseGeneration: 2n };
  await f.setWriter(successor);
  assert.equal(await f.store.complete(defaultIdentity, successor, reply, () => failure), undefined);
  f.advance(24 * hour);
  assert.deepEqual(await f.rows(), []);
});

test('pending renewal uses StoreNow and the remaining twelve hours', async () => {
  const f = await seeded();
  await f.store.complete(defaultIdentity, f.writer, reply, () => failure);
  const key = { value: relocationReplyKey(defaultIdentity) };
  f.advance(12 * hour - 1);
  assert.equal(await f.store.retain(defaultIdentity, f.writer), true);
  assert.equal((await f.provider.read(key)).value.expiresAt.getTime(), 24 * hour);
  f.advance(1);
  assert.equal(await f.store.retain(defaultIdentity, f.writer), true);
  assert.equal((await f.provider.read(key)).value.expiresAt.getTime(), 36 * hour);
});

test('an accepted row renews on the writer heartbeat before it expires', async () => {
  const f = await seeded();
  const key = { value: relocationReplyKey(defaultIdentity) };
  f.advance(12 * hour - 1);
  const schedule = (writer, from, to) =>
    f.store.acceptedRetentionAtHeartbeat(writer, new Date(from), new Date(to));
  assert.equal(schedule(f.writer, 11 * hour, 12 * hour - 1), undefined);
  f.advance(1);
  await schedule({ ...f.writer, leaseGeneration: 2n }, 12 * hour - 1, 12 * hour)();
  assert.equal((await f.provider.read(key)).value.expiresAt.getTime(), 24 * hour);
  await schedule(f.writer, 12 * hour - 1, 12 * hour)();
  assert.equal((await f.provider.read(key)).value.expiresAt.getTime(), 36 * hour);
  f.advance(12 * hour);
  assert.equal((await f.rows())[0].state, 'accepted');
});

test('oversize reply records the requestFailed reply in one result CAS', async () => {
  const f = await seeded();
  const large = Buffer.alloc(1_048_576).toString('base64');
  const row = await f.store.complete(defaultIdentity, f.writer, large, () => failure);
  assert.equal(row.state, 'pending');
  assert.equal(row.reply, failure);
});

test('source boundary snapshots replay request identities and excludes sends', async () => {
  const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
  const key = { value: defaultIdentity.objectKey };
  const makePacket = (index) => ({
    returnResponse: index !== 0,
    source: {
      ownerId: 'owner-A',
      ownerLeaseGeneration: '9',
      nodeRid: 'A',
      nodeGeneration: '7',
      replyRouteId: String(index)
    },
    messageFollowContext: { operationId: index.toString(16).padStart(32, '0') }
  });
  const stage = {
    phase: 'ready',
    cutoverReceived: true,
    offer: { prepare: { target: {} } },
    boundaryRelay: [2, 0].map((index) => ({
      object: { actorId: 'actor' },
      frozenRecord: { applicationPayload: { bytes: Buffer.from([index]) } }
    })),
    owner: {
      async replayRelayedMessage(_staging, objectKey, message) {
        assert.equal(objectKey, key.value);
        stage.staging.hidden.get(key.value).replayPackets.push(makePacket(message.payload[0]));
      }
    },
    staging: {
      envelope: { aggregateId: defaultIdentity.relocationId },
      primaryAuthorityKey: key,
      hidden: new Map([[key.value, { replayPackets: [makePacket(0), makePacket(1)] }]])
    }
  };
  let fenced;
  const authority = { storeVersion: { value: 'committed' } };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      async commitAggregate(fence) {
        fenced = fence;
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
  assert.deepEqual(
    fenced.capturedRequests.map((request) => request.replyRouteId),
    ['1', '2']
  );
  assert.deepEqual(fenced.capturedRequests[0], {
    ...defaultIdentity,
    operationId: '00000000000000000000000000000001'
  });
  assert.equal(stage.boundaryRelay.length, 0);
});

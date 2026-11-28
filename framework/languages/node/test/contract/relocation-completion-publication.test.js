const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const { completionFixture, entry: first, example } = require('./helpers/relocation-completion');
const deferred = require('./helpers/deferred.cjs');

const second = {
  ...first,
  operationId: '00000000000000010000000000000003',
  replyRouteId: '6',
  reply: 'BAUG'
};

test('completion publication verifies the blob before one CAS for all four fields', async () => {
  const f = completionFixture(2n);
  const assertPublication = async (publish, expectedEntries) => {
    const previous = f.current().relocation.completion;
    f.events.length = 0;
    await publish();

    const publication = f.events.slice(f.events.indexOf('put'));
    assert.deepEqual(publication, ['put', 'read', 'put', 'read', 'cas']);
    assert.equal(f.events.filter((event) => event === 'cas').length, 1);

    const current = f.current();
    const completion = current.relocation.completion;
    assert.notEqual(completion.reference, previous.reference);
    assert.match(completion.sha256, /^[a-f0-9]{64}$/);
    assert.notEqual(completion.sha256, previous.sha256);
    assert.equal(completion.acceptedRequests, 2n);
    assert.equal(completion.completedResults, 1n);
    assert.equal(completion.pendingDeliveries, expectedEntries[0].delivery === 'pending' ? 1n : 0n);
    assert.deepEqual((await f.owner.read(current)).entries, expectedEntries);
  };

  await assertPublication(() => f.owner.append(f.key, f.current(), first), [first]);
  const firstReference = f.current().relocation.completion.reference;
  const firstManifest = Buffer.from(f.blobs.get(firstReference));
  await assertPublication(
    () => f.owner.acknowledge(f.key, f.current(), first, 'terminalReceived'),
    [{ ...first, delivery: 'terminalReceived' }]
  );
  assert.deepEqual(f.blobs.get(firstReference), firstManifest);
});

for (const [name, completed, pending] of [
  ['an accepted result is missing', 1n, 0n],
  ['the last reply is awaiting acknowledgement', 2n, 1n]
]) {
  test(`Completed waits when ${name}`, { timeout: 5000 }, async () => {
    const f = completionFixture(2n);
    const acknowledge = (entry) =>
      f.owner.acknowledge(f.key, f.current(), entry, 'terminalReceived');
    await f.owner.append(f.key, f.current(), first);
    await acknowledge(first);
    if (completed === 2n) await f.owner.append(f.key, f.current(), second);

    const phases = [];
    const exchange = f.authorities.compareExchangeAuthority;
    f.authorities.compareExchangeAuthority = async (...args) => {
      const stored = await exchange(...args);
      phases.push(stored.relocation);
      return stored;
    };
    f.authorities.readAuthority = async () => f.current();
    const host = new ZLinkHostServiceRelocationRuntime({
      locationStore: () => f.authorities,
      relocationStore: f.store,
      spotManager: () => undefined
    });
    host.commitTargetReservation = async () => f.current();
    // Transport has no ACK to report until the test acknowledges the stored reply.
    host.redeliverPendingCompletions = async (_mesh, _key, current) => current;
    const opened = deferred();
    const replay = deferred();
    const stage = {
      phase: 'ready',
      offer: {
        prepareFingerprint: 'completion',
        prepare: { target: f.current().relocation.target }
      },
      staging: {
        primaryAuthorityKey: f.key,
        envelope: {
          aggregateId: example.relocationId,
          participants: [{ key: f.key.value }],
          memberships: []
        },
        hidden: new Map()
      },
      owner: {
        async publish() {},
        normalize: () => replay.promise,
        openAdmission: () => opened.resolve()
      },
      releaseActivation() {},
      releaseJoinAdmission() {}
    };

    const finalizing = host.finalizeTargetStage('mesh', 'completion', stage);
    await Promise.race([opened.promise, finalizing]);
    await turn();
    const progress = (completion) => [
      completion.acceptedRequests,
      completion.completedResults,
      completion.pendingDeliveries
    ];
    assert.deepEqual(
      phases.map((relocation) => relocation?.phase),
      ['activating', 'activated']
    );
    assert.deepEqual(progress(f.current().relocation.completion), [2n, completed, pending]);

    if (completed === 1n) await f.owner.append(f.key, f.current(), second);
    await acknowledge(second);
    const expected = f.current().relocation.completion;
    assert.deepEqual(progress(expected), [2n, 2n, 0n]);

    replay.resolve();
    await finalizing;
    assert.deepEqual(
      phases.slice(-3).map((relocation) => relocation?.phase),
      ['cleaning', 'completed', undefined]
    );
    assert.deepEqual(phases.at(-2).completion, expected);
  });
}

test('a failed Relocation Store put makes zero Location Store CAS attempts', async () => {
  const f = completionFixture(2n);
  const initial = f.current();
  const failure = new TypeError('injected completion Put failure');
  f.store.put = async () => {
    f.events.push('put');
    throw failure;
  };

  const result = await Promise.allSettled([f.owner.append(f.key, initial, first)]);
  assert.deepEqual(f.events, ['put']);
  assert.deepEqual(result, [{ status: 'rejected', reason: failure }]);
  assert.equal(f.current(), initial);
});

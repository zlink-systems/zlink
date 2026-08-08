const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkBoundSessionAcceptedJournal
} = require('../../packages/framework/dist/runtime/actors/bound-session-accepted-journal');

function memoryStore() {
  const values = new Map();
  return {
    values,
    async put(reference, payload) {
      values.set(reference.value, Buffer.from(payload));
      const storeNow = new Date();
      return {
        kind: 'stored',
        expiresAt: new Date(storeNow.getTime() + 60_000),
        storeNow
      };
    },
    async read(reference) {
      const payload = values.get(reference.value);
      const storeNow = new Date();
      return payload === undefined
        ? { kind: 'missing', storeNow }
        : {
            kind: 'found',
            bytes: payload,
            expiresAt: new Date(storeNow.getTime() + 60_000),
            storeNow
          };
    },
    async delete(reference) {
      values.delete(reference.value);
    }
  };
}

function requestEntry() {
  return {
    index: 0,
    header: Buffer.from('header').toString('base64'),
    payload: Buffer.from('payload').toString('base64'),
    returnResponse: true,
    source: {
      ownerId: 'owner-source',
      ownerLeaseGeneration: '7',
      nodeRid: 'node-source',
      nodeGeneration: '11',
      replyRouteId: '41'
    },
    messageFollowContext: {
      operationId: '11111111111111111111111111111111',
      objectGeneration: '9',
      request: true,
      hopCount: 0,
      visitedOwners: [],
      payloadChecksumSha256: 'checksum'
    }
  };
}

test('accepted-journal root records the source owner fence and the entry identities', async () => {
  const store = memoryStore();
  const journal = new ZLinkBoundSessionAcceptedJournal(store);
  const root = await journal.prepare(
    'actor-fence',
    9n,
    'seal-fence',
    41n,
    [requestEntry()],
    undefined,
    { ownerId: 'owner-source', ownerLeaseGeneration: 7n }
  );
  assert.equal(root.sourceOwnerId, 'owner-source');
  assert.equal(root.sourceOwnerLeaseGeneration, 7n);

  const decoded = JSON.parse(store.values.get(root.reference.value).toString('utf8'));
  assert.equal(decoded.sourceOwnerId, 'owner-source');
  assert.equal(decoded.sourceOwnerLeaseGeneration, '7');
  assert.equal(decoded.entries[0].source.ownerId, 'owner-source');
  assert.equal(decoded.entries[0].source.replyRouteId, '41');
  assert.equal(decoded.entries[0].messageFollowContext.operationId, '11111111111111111111111111111111');

  await journal.verify(root);
  await assert.rejects(
    journal.verify({ ...root, sourceOwnerLeaseGeneration: 8n }),
    /source owner fence does not match/
  );
  await assert.rejects(
    journal.verify({ ...root, sourceOwnerId: 'owner-other' }),
    /source owner fence does not match/
  );
});

test('accepted-journal verify keeps backward read tolerance for owner fence fields', async () => {
  const store = memoryStore();
  const journal = new ZLinkBoundSessionAcceptedJournal(store);
  const legacyRoot = await journal.prepare('actor-legacy', 9n, 'seal-legacy', 41n, []);
  assert.equal(legacyRoot.sourceOwnerId, undefined);
  // A verifier that already knows the fence accepts a root written before the
  // fence was recorded.
  await journal.verify({
    ...legacyRoot,
    sourceOwnerId: 'owner-source',
    sourceOwnerLeaseGeneration: 7n
  });

  const fencedRoot = await journal.prepare(
    'actor-fenced',
    9n,
    'seal-fenced',
    41n,
    [],
    undefined,
    { ownerId: 'owner-source', ownerLeaseGeneration: 7n }
  );
  // A wire target without the owner id still verifies the fields it carries.
  await journal.verify({
    ...fencedRoot,
    sourceOwnerId: undefined,
    sourceOwnerLeaseGeneration: 7n
  });
  await assert.rejects(
    journal.verify({
      ...fencedRoot,
      sourceOwnerId: undefined,
      sourceOwnerLeaseGeneration: 8n
    }),
    /source owner fence does not match/
  );
});

test('accepted-journal rejects an invalid source owner fence at prepare', async () => {
  const journal = new ZLinkBoundSessionAcceptedJournal(memoryStore());
  await assert.rejects(
    journal.prepare('actor-bad', 9n, 'seal-bad', 41n, [], undefined, {
      ownerId: '',
      ownerLeaseGeneration: 7n
    }),
    /source owner fence is invalid/
  );
  await assert.rejects(
    journal.prepare('actor-bad', 9n, 'seal-bad', 41n, [], undefined, {
      ownerId: 'owner-source',
      ownerLeaseGeneration: 0n
    }),
    /source owner fence is invalid/
  );
});

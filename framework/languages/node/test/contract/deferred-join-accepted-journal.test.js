const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkDeferredJoinAcceptedJournal
} = require('../../packages/framework/dist/runtime/actors/deferred-join-accepted-journal');
const {
  ZLinkActorDispatchMailbox
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');

function harness(options = {}) {
  const roots = new Map();
  const events = [];
  const references = [];
  let authorityVersion = 1;
  let compareExchangeCalls = 0;
  let authority = {
    kind: 'snapshot',
    storeVersion: { value: String(authorityVersion) },
    payload: encodeActorAuthorityIdentity({
      actorType: 'player',
      actor: {
        nodeRid: 'node-a',
        actorId: 'actor-a',
        objectGeneration: 17n
      },
      meshName: 'game',
      ownerNodeGeneration: 1n,
      owner: {
        ownerId: 'owner-a',
        leaseGeneration: 5n
      }
    }),
    objectGeneration: 17n,
    authorityOwnerGeneration: 3n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'game', rid: 'node-a' },
      descriptorLifecycleGeneration: 1n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(100)
  };
  const authorityStore = {
    async readAuthority() {
      return authority;
    },
    async compareExchangeAuthority(_key, expected, mutation) {
      events.push('authority-cas');
      compareExchangeCalls++;
      if (options.conflictOnCompareExchangeCalls?.includes(compareExchangeCalls)) {
        return { kind: 'conflict', current: authority };
      }
      if (expected.value !== authority.storeVersion.value) {
        return { kind: 'conflict', current: authority };
      }
      authorityVersion++;
      authority = {
        ...authority,
        storeVersion: { value: String(authorityVersion) },
        payload: Buffer.from(mutation.payload)
      };
      const { kind: _kind, ...stored } = authority;
      return { kind: 'stored', ...stored };
    }
  };
  const relocationStore = {
    async put(reference, payload) {
      references.push(reference.value);
      const stable = Buffer.from(payload);
      roots.set(reference.value, stable);
      events.push(`put:${reference.value}`);
      return {
        kind: 'stored',
        expiresAt: new Date(86_400_100),
        storeNow: new Date(100)
      };
    },
    async read(reference) {
      const payload = roots.get(reference.value);
      return payload === undefined
        ? { kind: 'missing', storeNow: new Date(100) }
        : {
            kind: 'found',
            bytes: Buffer.from(payload),
            expiresAt: new Date(86_400_100),
            storeNow: new Date(100)
          };
    },
    async delete(reference) {
      events.push(`delete:${reference.value}`);
      roots.delete(reference.value);
    }
  };
  return {
    events,
    references,
    roots,
    journal: new ZLinkDeferredJoinAcceptedJournal(authorityStore, relocationStore),
    restartJournal: () => new ZLinkDeferredJoinAcceptedJournal(authorityStore, relocationStore)
  };
}

test('cross-node Accepted root preserves identity, reply and cursor across mailbox retry', async () => {
  const { events, references, roots, journal } = harness();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const actorRef = {
    nodeRid: 'node-b',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const operationId = { high: 41n, low: 73n };
  let root = await journal.prepare(
    sourceActorRef.actorId,
    operationId,
    sourceActorRef,
    Buffer.from('"accepted"')
  );
  assert.equal(root.cursor, 'prepared');
  assert.equal(root.reference.value, references[0]);

  root = await journal.markCommitted(root, actorRef);
  assert.equal(root.cursor, 'committed');
  assert.equal(root.reference.value, references[1]);
  assert.equal(root.actor.nodeRid, 'node-b');
  assert.equal(roots.has(references[0]), false);

  const mailbox = new ZLinkActorDispatchMailbox();
  const callbackEvents = [];
  let attempts = 0;
  const actor = {
    actorId: actorRef.actorId,
    async onJoinCompleted(completion) {
      callbackEvents.push(
        `completion:${completion.operationId.high}:${completion.actor.nodeRid}:${completion.actor.objectGeneration}`
      );
      attempts++;
      if (attempts === 1) throw new Error('retry');
    }
  };
  const backlog = mailbox.submit(async () => {
    callbackEvents.push('backlog');
  });
  await assert.rejects(
    journal.deliver(root, actor, actorRef, operation => mailbox.submit(operation)),
    /retry/
  );
  await backlog;
  assert.deepEqual(callbackEvents, [
    'backlog',
    'completion:41:node-b:17'
  ]);
  assert.equal((await journal.recover(actorRef.actorId)).cursor, 'committed');

  root = await journal.deliver(
    root,
    actor,
    actorRef,
    operation => mailbox.submit(operation)
  );
  assert.equal(root.cursor, 'delivered');
  assert.equal(root.reference.value, references[2]);
  assert.equal(roots.has(references[1]), false);
  assert.equal(roots.has(references[2]), false);

  await journal.deliver(
    root,
    actor,
    actorRef,
    operation => mailbox.submit(operation)
  );
  assert.equal(attempts, 2);
  assert.deepEqual(events, [
    `put:${references[0]}`,
    'authority-cas',
    `put:${references[1]}`,
    'authority-cas',
    `delete:${references[0]}`,
    `put:${references[2]}`,
    'authority-cas',
    `delete:${references[1]}`,
    'authority-cas',
    `delete:${references[2]}`
  ]);
});

test('generation fence rejects a replacement Actor before mailbox admission', async () => {
  const { journal } = harness();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const actorRef = {
    nodeRid: 'node-b',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const root = await journal.markCommitted(await journal.prepare(
    actorRef.actorId,
    { high: 1n, low: 2n },
    sourceActorRef,
    Buffer.alloc(0)
  ), actorRef);
  let admitted = false;
  await assert.rejects(
    journal.deliver(
      root,
      { actorId: actorRef.actorId },
      { ...actorRef, objectGeneration: 18n },
      async operation => {
        admitted = true;
        return await operation();
      }
    ),
    /generation fence/
  );
  assert.equal(admitted, false);
});

test('a replacement runtime replays backlog before Accepted callback and does not redeliver after Delivered', async () => {
  const { journal, restartJournal } = harness();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const targetActorRef = {
    ...sourceActorRef,
    nodeRid: 'node-b'
  };
  const operationId = { high: 91n, low: 37n };
  let prepared = await journal.prepare(
    sourceActorRef.actorId,
    operationId,
    sourceActorRef,
    Buffer.from('"accepted"'),
    undefined,
    {
      targetMeshName: 'game',
      targetSpotId: 'room-a',
      targetSpotGeneration: 5n,
      membershipEpoch: 9n,
      request: Buffer.from('immutable-transfer-request')
    }
  );
  prepared = await journal.markRecoveryMessageReplayed(prepared, 1);
  const committed = await journal.markCommitted(prepared, targetActorRef);

  // Recreate every process-local coordinator while retaining only the two
  // provider-backed stores.
  const recoveredRuntime = restartJournal();
  const recovered = await recoveredRuntime.recover(targetActorRef.actorId);
  assert.equal(recovered.cursor, 'committed');
  assert.deepEqual(recovered.operationId, operationId);
  assert.equal(recovered.actor.objectGeneration, 17n);
  assert.equal(recovered.replayCursor, 1);
  assert.equal(recovered.recovery.targetSpotId, 'room-a');
  assert.equal(recovered.recovery.targetSpotGeneration, 5n);
  assert.equal(recovered.recovery.membershipEpoch, 9n);
  assert.equal(
    (await recoveredRuntime.readRecoveryPayload(recovered)).toString(),
    'immutable-transfer-request'
  );

  const mailbox = new ZLinkActorDispatchMailbox();
  const events = [];
  let callbacks = 0;
  const backlog = mailbox.submit(async () => {
    events.push('backlog');
  });
  const delivered = await recoveredRuntime.deliver(
    recovered,
    {
      actorId: targetActorRef.actorId,
      async onJoinCompleted(completion) {
        callbacks++;
        events.push(`callback:${completion.operationId.high}:${completion.actor.objectGeneration}`);
      }
    },
    targetActorRef,
    operation => mailbox.submit(operation)
  );
  await backlog;

  assert.equal(delivered.cursor, 'delivered');
  assert.deepEqual(events, ['backlog', 'callback:91:17']);
  assert.equal(callbacks, 1);

  const secondReplacement = restartJournal();
  const alreadyDelivered = await secondReplacement.recover(targetActorRef.actorId);
  assert.equal(alreadyDelivered, undefined);
  assert.equal(callbacks, 1);
});

test('discarding a failed prepared operation restores the Actor authority for the next Join', async () => {
  const { journal, roots } = harness();
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const failed = await journal.prepare(
    actorRef.actorId,
    { high: 10n, low: 20n },
    actorRef,
    Buffer.from('"failed"')
  );
  assert.equal(roots.size, 1);

  await journal.discardPrepared(failed);
  assert.equal(roots.size, 0);
  assert.equal(await journal.recover(actorRef.actorId), undefined);

  const replacement = await journal.prepare(
    actorRef.actorId,
    { high: 30n, low: 40n },
    actorRef,
    Buffer.from('"replacement"')
  );
  assert.deepEqual(replacement.operationId, { high: 30n, low: 40n });
  assert.equal(replacement.cursor, 'prepared');
});

test('Delivered cursor CAS conflict is reconciled without invoking the callback twice', async () => {
  const { journal } = harness({ conflictOnCompareExchangeCalls: [3] });
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const targetActorRef = {
    ...sourceActorRef,
    nodeRid: 'node-b'
  };
  const committed = await journal.markCommitted(await journal.prepare(
    sourceActorRef.actorId,
    { high: 50n, low: 60n },
    sourceActorRef,
    Buffer.alloc(0)
  ), targetActorRef);
  let callbacks = 0;

  const delivered = await journal.deliver(
    committed,
    {
      actorId: targetActorRef.actorId,
      async onJoinCompleted() {
        callbacks++;
      }
    },
    targetActorRef,
    operation => operation()
  );

  assert.equal(delivered.cursor, 'delivered');
  assert.equal(await journal.recover(targetActorRef.actorId), undefined);
  assert.equal(callbacks, 1);
});

test('Delivered reference release CAS conflict is retried without invoking the callback twice', async () => {
  const { journal } = harness({ conflictOnCompareExchangeCalls: [4] });
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const targetActorRef = {
    ...sourceActorRef,
    nodeRid: 'node-b'
  };
  const committed = await journal.markCommitted(await journal.prepare(
    sourceActorRef.actorId,
    { high: 70n, low: 80n },
    sourceActorRef,
    Buffer.alloc(0)
  ), targetActorRef);
  let callbacks = 0;

  await journal.deliver(
    committed,
    {
      actorId: targetActorRef.actorId,
      async onJoinCompleted() {
        callbacks++;
      }
    },
    targetActorRef,
    operation => operation()
  );

  assert.equal(await journal.recover(targetActorRef.actorId), undefined);
  assert.equal(callbacks, 1);
});

test('callback completion retains recovery payload until backlog replay and admission release', async () => {
  const { journal, restartJournal } = harness();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const targetActorRef = { ...sourceActorRef, nodeRid: 'node-b' };
  const operationId = { high: 101n, low: 202n };
  const prepared = await journal.prepare(
    sourceActorRef.actorId,
    operationId,
    sourceActorRef,
    Buffer.from('"accepted"'),
    undefined,
    {
      targetMeshName: 'game',
      targetSpotId: 'room-a',
      targetSpotGeneration: 5n,
      membershipEpoch: 9n,
      request: Buffer.from('state-and-backlog')
    }
  );
  const committed = await journal.markCommitted(prepared, targetActorRef);
  let callbacks = 0;
  const delivered = await journal.deliver(
    committed,
    {
      actorId: targetActorRef.actorId,
      async onJoinCompleted() {
        callbacks++;
      }
    },
    targetActorRef,
    operation => operation(),
    undefined,
    true
  );
  assert.equal(delivered.cursor, 'delivered');

  // Simulate a process crash after the callback but before backlog replay.
  const replacement = restartJournal();
  const recovered = await replacement.recover(targetActorRef.actorId);
  assert.equal(recovered.cursor, 'delivered');
  assert.equal(callbacks, 1);
  assert.equal(
    (await replacement.readRecoveryPayload(recovered)).toString(),
    'state-and-backlog'
  );

  const replayed = await replacement.markRecoveryMessageReplayed(recovered, 1);
  await replacement.releaseRecovery(replayed);
  assert.equal(await restartJournal().recover(targetActorRef.actorId), undefined);
  assert.equal(callbacks, 1);
});

test('a delivered Join completion can be replaced by the next Join for the same Actor generation', async () => {
  const { journal, roots } = harness();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const firstTargetRef = {
    ...sourceActorRef,
    nodeRid: 'node-b'
  };
  const secondTargetRef = {
    ...sourceActorRef,
    nodeRid: 'node-c'
  };
  let first = await journal.prepare(
    sourceActorRef.actorId,
    { high: 1n, low: 2n },
    sourceActorRef,
    Buffer.from('"first"')
  );
  first = await journal.markCommitted(first, firstTargetRef);
  first = await journal.deliver(
    first,
    { actorId: sourceActorRef.actorId },
    firstTargetRef,
    operation => operation()
  );

  const second = await journal.prepare(
    sourceActorRef.actorId,
    { high: 3n, low: 4n },
    firstTargetRef,
    Buffer.from('"second"')
  );
  assert.equal(second.cursor, 'prepared');
  assert.deepEqual(second.operationId, { high: 3n, low: 4n });
  assert.equal(second.actor.nodeRid, 'node-b');
  assert.equal(roots.has(first.reference.value), false);

  const recovered = await journal.recover(secondTargetRef.actorId);
  assert.deepEqual(recovered.operationId, second.operationId);
  assert.equal(recovered.cursor, 'prepared');
});

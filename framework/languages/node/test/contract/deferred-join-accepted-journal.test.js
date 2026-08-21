const assert = require('node:assert/strict');
const test = require('node:test');

const {
  isDeferredJoinAcceptedRootPublication,
  ZLinkDeferredJoinAcceptedJournal
} = require('../../packages/framework/dist/runtime/actors/deferred-join-accepted-journal');
const {
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkActorDispatchMailbox
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  decodeActorAuthorityIdentity,
  decodeRelocatingActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  crc32c,
  ServiceRelocationAuthorityPayloadCodec,
  serviceRelocationAuthorityApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

const relocationPublication = {
  reference: 'zlink-direct:11111111-1111-4111-8111-111111111111:2',
  checksumCrc32c: 7,
  aggregateId: '11111111-1111-4111-8111-111111111111',
  aggregateGeneration: 2n,
  inventoryDigest: 'a'.repeat(64),
  targetOwnerId: 'owner-a',
  targetOwnerLeaseGeneration: 13n
};

function harness(options = {}) {
  const roots = new Map();
  const events = [];
  const references = [];
  let authorityVersion = 1;
  let compareExchangeCalls = 0;
  const actorAuthorityPayload = encodeActorAuthorityIdentity({
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
    },
    spotId: 'entry-a',
    spotGeneration: 1n,
    spotKind: 'entry'
  });
  const storedActorAuthorityPayload = options.actorRelocationEnvelope === true
    ? encodeActorRelocationEnvelope(actorAuthorityPayload)
    : actorAuthorityPayload;
  let initialAuthorityPayload = options.relocationEnvelope === true
    ? new ServiceRelocationAuthorityPayloadCodec().publish(
        storedActorAuthorityPayload,
        relocationPublication
      )
    : storedActorAuthorityPayload;
  if (options.mutateAuthorityPayload !== undefined) {
    initialAuthorityPayload = options.mutateAuthorityPayload(initialAuthorityPayload);
  }
  let authority = {
    kind: 'snapshot',
    storeVersion: { value: String(authorityVersion) },
    payload: initialAuthorityPayload,
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
    relocationStore,
    journal: new ZLinkDeferredJoinAcceptedJournal(
      authorityStore,
      relocationStore,
      options.messageSerializers
    ),
    restartJournal: () => new ZLinkDeferredJoinAcceptedJournal(
      authorityStore,
      relocationStore,
      options.messageSerializers
    ),
    authorityPayload: () => Buffer.from(authority.payload),
    replaceAuthorityPayload(payload) {
      authority = { ...authority, payload: Buffer.from(payload) };
    }
  };
}

test('deferred Join cursor and release writes preserve the relocation authority envelope', async () => {
  const { journal, authorityPayload } = harness({
    relocationEnvelope: true,
    actorRelocationEnvelope: true
  });
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const sourceActorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const targetActorRef = { ...sourceActorRef, nodeRid: 'node-b' };

  const prepared = await journal.prepare(
    sourceActorRef.actorId,
    { high: 1n, low: 2n },
    sourceActorRef,
    Buffer.from('"accepted"')
  );
  assert.deepEqual(codec.read(authorityPayload()), relocationPublication);
  const preparedActorAuthority = deferredJoinApplicationPayload(authorityPayload());
  const preparedRelocationPrefix = actorRelocationPrefix(preparedActorAuthority);
  assert.equal(preparedRelocationPrefix.subarray(6, 22).toString('hex'),
    '00112233445566778899aabbccddeeff');
  assert.equal(preparedRelocationPrefix[22], 2, 'relocation phase');
  assert.equal(preparedRelocationPrefix[23], 1, 'bound-session route flag');
  assert.equal(preparedRelocationPrefix.includes(Buffer.from('binding-a')), true);
  assert.equal(
    decodeRelocatingActorAuthorityIdentity(
      preparedActorAuthority,
      17n
    ).actor.nodeRid,
    'node-a'
  );

  const committed = await journal.markCommitted(prepared, targetActorRef);
  assert.deepEqual(codec.read(authorityPayload()), relocationPublication);
  const committedActorAuthority = deferredJoinApplicationPayload(authorityPayload());
  assert.deepEqual(
    actorRelocationPrefix(committedActorAuthority),
    preparedRelocationPrefix,
    'markCommitted must preserve relocation id, phase, and bound-session route'
  );
  assert.equal(
    decodeRelocatingActorAuthorityIdentity(
      committedActorAuthority,
      17n
    ).actor.nodeRid,
    'node-b'
  );

  const delivered = await journal.deliver(
    committed,
    { actorId: targetActorRef.actorId, async onJoinCompleted() {} },
    targetActorRef,
    operation => operation()
  );
  assert.equal(delivered.cursor, 'delivered');
  assert.deepEqual(codec.read(authorityPayload()), relocationPublication);
  assert.notEqual(
    serviceRelocationAuthorityApplicationPayload(authorityPayload()).byteLength,
    0
  );
  assert.equal(
    decodeRelocatingActorAuthorityIdentity(
      serviceRelocationAuthorityApplicationPayload(authorityPayload()),
      17n
    ).actor.nodeRid,
    'node-b'
  );
  assert.equal(await journal.recover(targetActorRef.actorId), undefined);
});

function deferredJoinApplicationPayload(payload) {
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  assert.notEqual(
    new ServiceRelocationAuthorityPayloadCodec().read(outerApplication),
    undefined
  );
  const application = serviceRelocationAuthorityApplicationPayload(outerApplication);
  assert.equal(application.subarray(0, 4).toString('ascii'), 'ZLAP');
  return application;
}

function actorRelocationPrefix(payload) {
  const applicationOffset = payload.indexOf(Buffer.from('ZLAU'));
  assert.ok(applicationOffset > 4, 'ZLAP application payload must contain canonical ZLAU');
  return Buffer.from(payload.subarray(0, applicationOffset - 4));
}

function encodeActorRelocationEnvelope(authority) {
  const parts = [
    Buffer.from('ZLAP'),
    u16le(6),
    Buffer.from('00112233445566778899aabbccddeeff', 'hex'),
    Buffer.of(2, 1),
    bytes8('node-b'),
    bytes8('session-a'),
    text16('binding-a'),
    u64le(1n),
    u64le(17n),
    u64le(3n),
    text16('game'),
    u64le(4n),
    u64le(5n),
    u64le(6n),
    u64le(0n),
    text16('session-owner-a'),
    u64le(7n),
    i32le(authority.byteLength),
    authority
  ];
  const envelope = Buffer.concat(parts);
  return Buffer.concat([envelope, u32le(crc32c(envelope))]);
}

function bytes8(value) {
  const bytes = Buffer.from(value, 'utf8');
  return Buffer.concat([Buffer.of(bytes.byteLength), bytes]);
}

function text16(value) {
  const bytes = Buffer.from(value, 'utf8');
  return Buffer.concat([u16le(bytes.byteLength), bytes]);
}

function u16le(value) {
  const result = Buffer.alloc(2);
  result.writeUInt16LE(value);
  return result;
}

function u32le(value) {
  const result = Buffer.alloc(4);
  result.writeUInt32LE(value);
  return result;
}

function i32le(value) {
  const result = Buffer.alloc(4);
  result.writeInt32LE(value);
  return result;
}

function u64le(value) {
  const result = Buffer.alloc(8);
  result.writeBigUInt64LE(value);
  return result;
}

function withZeroCanonicalAggregateGeneration(payload) {
  const bytes = Buffer.from(payload);
  let offset = 11;
  offset += 2;
  offset += 2 + bytes.readUInt16BE(offset);
  for (const field of ['owner']) {
    void field;
    offset += 1 + bytes[offset];
  }
  offset += 8;
  offset += 1 + bytes[offset];
  offset += 1 + bytes[offset];
  offset += 8;
  assert.equal(bytes[offset], 1, 'canonical relocation slot presence');
  const slotLength = bytes.readUInt32BE(offset + 1);
  offset += 5;
  const slotEnd = offset + slotLength;
  offset += 24;
  const skipText8 = () => { offset += 1 + bytes[offset]; };
  skipText8(); offset += 8;
  skipText8(); offset += 8;
  skipText8(); offset += 8;
  skipText8(); offset += 8;
  skipText8(); offset += 8;
  skipText8(); offset += 8;
  offset += 10;
  assert.equal(bytes[offset], 1, 'canonical relocation extension presence');
  bytes.writeBigUInt64BE(0n, offset + 1);
  assert.ok(offset + 9 <= slotEnd);
  bytes.writeUInt32BE(crc32c(bytes.subarray(0, bytes.byteLength - 4)), bytes.byteLength - 4);
  return bytes;
}

test('canonical journal-root classification requires the exact participant and aggregate fence', async () => {
  const context = harness({ relocationEnvelope: true });
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const root = await context.journal.prepare(
    actorRef.actorId,
    { high: 11n, low: 12n },
    actorRef,
    Buffer.from('reply'),
    undefined,
    undefined,
    'b'.repeat(64)
  );
  const exact = {
    authorityKey: encodeAuthorityKey('actor', actorRef.actorId).value,
    objectKind: 'actor',
    objectGeneration: actorRef.objectGeneration,
    aggregateId: relocationPublication.aggregateId,
    aggregateGeneration: relocationPublication.aggregateGeneration
  };

  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    exact
  ), true);
  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    { ...exact, authorityKey: encodeAuthorityKey('actor', 'actor-other').value }
  ), false, 'another participant must not acquire journal-root replacement authority');
  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    { ...exact, objectGeneration: 18n }
  ), false);
  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    { ...exact, aggregateGeneration: 3n }
  ), false);
});

test('standalone journal-root classification requires the exact Actor and operation fence', async () => {
  const context = harness();
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const root = await context.journal.prepare(
    actorRef.actorId,
    { high: 11n, low: 12n },
    actorRef,
    Buffer.from('reply')
  );
  const exact = {
    authorityKey: encodeAuthorityKey('actor', actorRef.actorId).value,
    objectKind: 'actor',
    objectGeneration: actorRef.objectGeneration,
    aggregateId: '00000000-0000-000b-0000-00000000000c',
    aggregateGeneration: actorRef.objectGeneration
  };

  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    exact
  ), true);
  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    { ...exact, aggregateId: '00000000-0000-000b-0000-00000000000d' }
  ), false);
  assert.equal(await isDeferredJoinAcceptedRootPublication(
    context.relocationStore,
    root.reference.value,
    root.checksumCrc32c,
    { ...exact, authorityKey: encodeAuthorityKey('actor', 'actor-other').value }
  ), false);
});

test('canonical ZLJC round-trip preserves a custom reply content type', async () => {
  const contentType = 'application/x-zlink-custom-reply';
  const context = harness({
    relocationEnvelope: true,
    messageSerializers: new Map([[contentType, {
      serialize() { throw new Error('not used'); },
      deserialize(payload) { return Buffer.from(payload.data()).toString('hex'); }
    }]])
  });
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const root = await context.journal.prepare(
    actorRef.actorId,
    { high: 21n, low: 22n },
    actorRef,
    Buffer.from([0, 1, 2, 3]),
    contentType,
    undefined,
    'c'.repeat(64)
  );
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const current = codec.read(context.authorityPayload());
  context.replaceAuthorityPayload(codec.publish(
    codec.clear(context.authorityPayload(), current.reference),
    {
      ...current,
      reference: root.reference.value,
      checksumCrc32c: root.checksumCrc32c
    }
  ));

  const restarted = context.restartJournal();
  const recovered = await restarted.recover(actorRef.actorId);
  assert.equal(recovered.replyContentType, contentType);
  assert.deepEqual(recovered.rawReply, Buffer.from([0, 1, 2, 3]));
  const committed = await restarted.markCommitted(recovered, actorRef);
  let decodedReply;
  await restarted.deliver(
    committed,
    {
      actorId: actorRef.actorId,
      async onJoinCompleted(completion) {
        decodedReply = completion.reply.decode();
      }
    },
    actorRef,
    operation => operation()
  );
  assert.equal(decodedReply, '00010203');
});

test('a zero canonical aggregate generation fails as typed data loss instead of becoming one', async () => {
  const context = harness({
    relocationEnvelope: true,
    mutateAuthorityPayload: withZeroCanonicalAggregateGeneration
  });
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  await assert.rejects(
    () => context.journal.prepare(
      actorRef.actorId,
      { high: 31n, low: 32n },
      actorRef,
      Buffer.alloc(0)
    ),
    error => error.kind === ZLinkFrameworkErrorKind.DataLost
      && /no aggregate generation/.test(error.message)
  );
});

test('discarding a prepared Join clears the canonical embedded relocation slot', async () => {
  const { journal, authorityPayload } = harness({ relocationEnvelope: true });
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 17n,
    meshName: 'game'
  };
  const prepared = await journal.prepare(
    actorRef.actorId,
    { high: 3n, low: 4n },
    actorRef,
    Buffer.alloc(0)
  );

  await journal.discardPrepared(prepared);

  assert.equal(codec.read(authorityPayload()), undefined);
  assert.equal(authorityPayload().subarray(0, 4).toString('ascii'), 'ZLAU');
  assert.equal(await journal.recover(actorRef.actorId), undefined);
});

test('cross-node Accepted root preserves identity, reply and cursor across mailbox retry', async () => {
  const { events, references, roots, journal, authorityPayload } = harness();
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
  assert.equal(authorityPayload().subarray(0, 4).toString('ascii'), 'ZLAU');
  assert.equal(
    new ServiceRelocationAuthorityPayloadCodec().read(authorityPayload()).canonical,
    true,
    'Node journal publication must use the embedded canonical relocation slot'
  );

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
  assert.equal(new ServiceRelocationAuthorityPayloadCodec().read(authorityPayload()), undefined);

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

test('a replacement runtime preserves committed Accepted completion ordering without duplicating relocation backlog', async () => {
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
  const prepared = await journal.prepare(
    sourceActorRef.actorId,
    operationId,
    sourceActorRef,
    Buffer.from('"accepted"')
  );
  const committed = await journal.markCommitted(prepared, targetActorRef);

  // Recreate every process-local coordinator while retaining only the two
  // provider-backed stores.
  const recoveredRuntime = restartJournal();
  const recovered = await recoveredRuntime.recover(targetActorRef.actorId);
  assert.equal(recovered.cursor, 'committed');
  assert.deepEqual(recovered.operationId, operationId);
  assert.equal(recovered.actor.objectGeneration, 17n);

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

test('callback completion releases the Accepted journal independently of relocation durable backlog', async () => {
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
    Buffer.from('"accepted"')
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
    operation => operation()
  );
  assert.equal(delivered.cursor, 'delivered');
  assert.equal(callbacks, 1);
  // Relocation queue/state lives in the canonical relocation manifest, not in
  // this completion journal. Delivery therefore releases only this root.
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

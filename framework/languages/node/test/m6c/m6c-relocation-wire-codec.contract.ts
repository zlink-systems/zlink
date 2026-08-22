import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  decodeMaintenanceRelocationControl,
  decodeSessionRelocationRoute,
  decodeSessionRelocationSeal,
  decodeSessionRelocationSealed,
  decodeStatefulReply,
  encodeMaintenanceRelocationControl,
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  encodeSessionRelocationSealed,
  encodeStatefulReply,
  serviceSessionRelocationIdentityKey,
  type ServiceMaintenanceRelocationControl,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import {
  restoreRelocationAdapterState,
  type ZLinkRelocationStateAdapterLike
} from '../../packages/framework/src/runtime/host/relocation-state-adapter';
import { effectiveActorJoinChunkLimitBytes } from '../../packages/framework/src/runtime/host/relocation-direct-transfer';

interface GoldenEntry {
  readonly name: string;
  readonly command: number;
  readonly hex: string;
}

interface MalformedEntry extends GoldenEntry {
  readonly error: string;
}

const coordinator = {
  ownerId: 'coordinator',
  leaseGeneration: 7n,
  nodeRid: 'node-a',
  nodeGeneration: 11n,
  expectedAuthorityStoreVersion: 'store-3'
} as const;

const target = {
  nodeRid: 'node-b',
  nodeGeneration: 12n,
  ownerId: 'target-owner',
  ownerLeaseGeneration: 8n
} as const;

const relocationObject = {
  kind: 'userSpot',
  spotId: 'spot-1',
  objectGeneration: 9n,
  expectedAuthorityOwnerGeneration: 10n
} as const;

function relocationGolden(): {
  readonly canonical: readonly GoldenEntry[];
  readonly malformed: readonly MalformedEntry[];
} {
  return JSON.parse(readFileSync(
    '../../runtime/protocol/golden/relocation-control-v1.json',
    'utf8'
  )) as {
    readonly canonical: readonly GoldenEntry[];
    readonly malformed: readonly MalformedEntry[];
  };
}

function sessionGolden(): readonly GoldenEntry[] {
  return (JSON.parse(readFileSync(
    '../../runtime/protocol/golden/session-relocation-barrier-v1.json',
    'utf8'
  )) as { readonly canonical: readonly GoldenEntry[] }).canonical;
}

interface ActorJoinGoldenEntry {
  readonly name: string;
  readonly command: number;
  readonly correlation: string;
  readonly decoded: {
    readonly joinResult: 'accepted' | 'rejected';
    readonly spot?: { readonly spotId: string; readonly generation: string };
    readonly membershipEpoch?: string;
    readonly receiveChunkLimitBytes?: number;
  };
  readonly hex: string;
}

interface ActorJoinMalformedEntry {
  readonly name: string;
  readonly command: number;
  readonly correlation: string;
  readonly hex: string;
  readonly error: string;
}

function actorJoinReplyGolden(): {
  readonly canonical: readonly ActorJoinGoldenEntry[];
  readonly malformed: readonly ActorJoinMalformedEntry[];
} {
  return JSON.parse(readFileSync(
    '../../runtime/protocol/golden/actor-join-reply-v1.json',
    'utf8'
  )) as {
    readonly canonical: readonly ActorJoinGoldenEntry[];
    readonly malformed: readonly ActorJoinMalformedEntry[];
  };
}

function assertGoldenRoundTrip<T>(
  entry: GoldenEntry,
  expected: T,
  encode: (value: T) => Buffer,
  decode: (encoded: Uint8Array) => T
): void {
  const encoded = Buffer.from(entry.hex, 'hex');
  const decoded = decode(encoded);
  assert.deepEqual(decoded, expected, entry.name);
  assert.equal(encode(decoded).toString('hex'), entry.hex, entry.name);
  assert.throws(() => decode(encoded.subarray(0, -1)));

  const forbiddenFlag = Buffer.from(encoded);
  forbiddenFlag[4] = 1;
  assert.throws(() => decode(forbiddenFlag));
}

test('commands 30, 31, 34, 40, 52, and 53 match the shared golden vectors', () => {
  const golden = relocationGolden();
  const entries = golden.canonical;
  const dataEntry = entries.find(entry => entry.name === 'relocationDataPostCaptureIngress');
  assert.ok(dataEntry);
  const base = {
    relocation: { high: 4n, low: 5n },
    targetAttemptGeneration: 6n,
    coordinator
  } as const;
  const values: readonly ServiceMaintenanceRelocationControl[] = [
    { kind: 'ready', ...base, target, object: relocationObject, senderRole: 'target' },
    {
      kind: 'failed',
      ...base,
      target,
      object: relocationObject,
      senderRole: 'target',
      failureCode: 35
    },
    decodeMaintenanceRelocationControl(Buffer.from(dataEntry.hex, 'hex')),
    {
      kind: 'cutover',
      ...base,
      senderRole: 'source',
      object: relocationObject,
      boundaryRecordCount: 3n,
      boundaryChecksumCrc32c: 0xe3069283
    },
    {
      kind: 'prepare',
      ...base,
      target,
      initiatorRole: 'source',
      object: relocationObject,
      sourceNodeRid: 'node-a',
      sourceNodeGeneration: 11n,
      payloadTotalLength: 24n,
      payloadChunkCount: 2,
      payloadChecksumCrc32c: 0x29bc8795,
      applicationVersion: 1n
    },
    {
      kind: 'state',
      ...base,
      senderRole: 'source',
      object: relocationObject,
      chunkOrdinal: 0,
      chunkData: Buffer.from('000102030405060708090a0b0c0d0e0f', 'hex')
    },
    {
      kind: 'state',
      ...base,
      senderRole: 'source',
      object: relocationObject,
      chunkOrdinal: 1,
      chunkData: Buffer.from('1011121314151617', 'hex')
    }
  ];

  assert.deepEqual(entries.map(entry => entry.command), [30, 53, 31, 34, 40, 52, 52]);
  assert.deepEqual(entries.map(entry => entry.name), [
    'relocationReady',
    'relocationFailed',
    'relocationDataPostCaptureIngress',
    'relocationCutover',
    'relocationPrepareRestore',
    'relocationStateChunk',
    'relocationStateFinalChunk'
  ]);
  for (const [index, entry] of entries.entries()) {
    const encoded = Buffer.from(entry.hex, 'hex');
    const decoded = decodeMaintenanceRelocationControl(encoded);
    assert.deepEqual(decoded, values[index], entry.name);
    assert.equal(encodeMaintenanceRelocationControl(decoded).toString('hex'), entry.hex);
    assert.throws(() => decodeMaintenanceRelocationControl(encoded.subarray(0, -1)));

    const forbiddenFlag = Buffer.from(encoded);
    forbiddenFlag[4] = 1;
    assert.throws(() => decodeMaintenanceRelocationControl(forbiddenFlag));
  }

  const data = values[2];
  assert.equal(data?.kind, 'data');
  if (data?.kind === 'data') {
    assert.equal(data.senderRole, 'source');
    assert.deepEqual(data.object, relocationObject);
    assert.equal(data.frozenRecord.recordKind, 6);
  }

  for (const entry of golden.malformed) {
    assert.throws(
      () => decodeMaintenanceRelocationControl(Buffer.from(entry.hex, 'hex')),
      Error,
      entry.name
    );
  }

  for (const command of [32, 35, 41]) {
    const reserved = Buffer.from(entries[0]!.hex, 'hex');
    reserved[3] = command;
    assert.throws(() => decodeMaintenanceRelocationControl(reserved));
  }
});

test('commands 42, 43, and 44 match the shared Session golden vectors', () => {
  const actor = { nodeRid: '', actorId: 'actor-1', generation: 5n } as const;
  const session = {
    sessionOwnerNodeRid: 'source',
    sessionOwnerNodeGeneration: 2n,
    sessionOwnerId: 'session-owner',
    sessionOwnerLeaseGeneration: 8n,
    sessionRid: 'session',
    bindingGeneration: 6n
  } as const;
  const sessionCoordinator = {
    ownerId: 'coordinator',
    leaseGeneration: 3n,
    nodeRid: 'source',
    nodeGeneration: 2n,
    expectedAuthorityStoreVersion: 'store-v17'
  } as const;
  const sessionBase = {
    relocation: { high: 7n, low: 9n },
    coordinator: sessionCoordinator
  } as const;
  const seal: ServiceSessionRelocationSeal = {
    ...sessionBase,
    senderRole: 'source',
    actor: {
      actor: { ...actor, nodeRid: 'source' },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n
    },
    session
  };
  const sealed: ServiceSessionRelocationSealed = {
    ...sessionBase,
    actor: seal.actor,
    session
  };
  const commit: ServiceSessionRelocationRoute = {
    ...sessionBase,
    senderRole: 'target',
    actor,
    session,
    route: {
      action: 'commit',
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetNodeRid: 'target',
      targetNodeGeneration: 4n
    }
  };
  const abort: ServiceSessionRelocationRoute = {
    ...sessionBase,
    senderRole: 'source',
    actor,
    session,
    route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
  };
  const entries = sessionGolden();

  assert.deepEqual(entries.map(entry => entry.command), [42, 43, 44, 44]);
  assertGoldenRoundTrip(entries[0]!, seal,
    encodeSessionRelocationSeal, decodeSessionRelocationSeal);
  assertGoldenRoundTrip(entries[1]!, sealed,
    encodeSessionRelocationSealed, decodeSessionRelocationSealed);
  assertGoldenRoundTrip(entries[2]!, commit,
    encodeSessionRelocationRoute, decodeSessionRelocationRoute);
  assertGoldenRoundTrip(entries[3]!, abort,
    encodeSessionRelocationRoute, decodeSessionRelocationRoute);

  const reserved = Buffer.from(entries[2]!.hex, 'hex');
  reserved[3] = 45;
  assert.throws(() => decodeSessionRelocationRoute(reserved));
  assert.throws(() => encodeSessionRelocationSeal({
    ...seal,
    senderRole: 'target'
  } as never), /sender role/);
});

test('Session relocation retain identity includes every coordinator fence field', () => {
  const base: ServiceSessionRelocationSeal = {
    relocation: { high: 7n, low: 9n },
    coordinator: {
      ownerId: 'coordinator',
      leaseGeneration: 3n,
      nodeRid: 'source',
      nodeGeneration: 2n,
      expectedAuthorityStoreVersion: 'store-v17'
    },
    senderRole: 'source',
    actor: {
      actor: { nodeRid: 'source', actorId: 'actor-1', generation: 5n },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n
    },
    session: {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      sessionOwnerId: 'session-owner-id',
      sessionOwnerLeaseGeneration: 8n,
      sessionRid: 'session',
      bindingGeneration: 6n
    }
  };
  const key = serviceSessionRelocationIdentityKey(base);
  const changed = [
    { ...base.coordinator, ownerId: 'other-owner' },
    { ...base.coordinator, leaseGeneration: 4n },
    { ...base.coordinator, nodeRid: 'other-source' },
    { ...base.coordinator, nodeGeneration: 3n },
    { ...base.coordinator, expectedAuthorityStoreVersion: 'store-v18' }
  ];

  for (const coordinatorFence of changed) {
    assert.notEqual(
      serviceSessionRelocationIdentityKey({ ...base, coordinator: coordinatorFence }),
      key
    );
  }
});

test('actorJoin reply tail (command 20, actor-join-reply-tail) matches the shared golden vectors', () => {
  const golden = actorJoinReplyGolden();

  assert.deepEqual(golden.canonical.map(entry => entry.name), [
    'actorJoinAcceptedTypical',
    'actorJoinAcceptedAtRelocationChunkLimitBound',
    'actorJoinAcceptedNotAdvertised',
    'actorJoinRejectedWithSpot',
    'actorJoinRejectedWithoutSpot'
  ]);

  for (const entry of golden.canonical) {
    assert.equal(entry.command, 20, entry.name);
    const correlation = BigInt(entry.correlation);
    const encoded = Buffer.from(entry.hex, 'hex');
    const decoded = decodeStatefulReply(encoded, correlation, 'actorJoin');
    assert.equal(decoded.correlation, correlation, entry.name);
    assert.equal(decoded.terminalResult, 0, entry.name);
    assert.equal(decoded.failureCode, 0, entry.name);
    assert.ok(decoded.tail && decoded.tail.kind === 'actorJoin', entry.name);
    const tail = decoded.tail as {
      readonly kind: 'actorJoin';
      readonly joinResult: number;
      readonly spot?: { readonly spotId: string; readonly generation: bigint };
      readonly membershipEpoch?: bigint;
      readonly receiveChunkLimitBytes?: number;
    };
    const expectedJoinResult = entry.decoded.joinResult === 'accepted' ? 0 : 1;
    assert.equal(tail.joinResult, expectedJoinResult, entry.name);
    if (entry.decoded.spot === undefined) {
      assert.equal(tail.spot, undefined, entry.name);
    } else {
      assert.deepEqual(tail.spot, {
        spotId: entry.decoded.spot.spotId,
        generation: BigInt(entry.decoded.spot.generation)
      }, entry.name);
    }
    if (entry.decoded.membershipEpoch !== undefined) {
      assert.equal(tail.membershipEpoch, BigInt(entry.decoded.membershipEpoch), entry.name);
    }
    if (entry.decoded.receiveChunkLimitBytes !== undefined) {
      assert.equal(tail.receiveChunkLimitBytes, entry.decoded.receiveChunkLimitBytes, entry.name);
    }

    // Round-trip: re-encoding the decoded value must reproduce the exact golden bytes.
    const reencoded = encodeStatefulReply(
      decoded.correlation,
      decoded.terminalResult,
      decoded.failureCode,
      decoded.tail
    );
    assert.equal(reencoded.toString('hex'), entry.hex, entry.name);

    // Truncating the frame by one byte must fail to decode.
    assert.throws(
      () => decodeStatefulReply(encoded.subarray(0, -1), correlation, 'actorJoin'),
      Error,
      entry.name
    );

    // Setting a forbidden flag must fail to decode.
    const forbiddenFlag = Buffer.from(encoded);
    forbiddenFlag[4] = 1;
    assert.throws(
      () => decodeStatefulReply(forbiddenFlag, correlation, 'actorJoin'),
      Error,
      entry.name
    );
  }

  for (const entry of golden.malformed) {
    assert.equal(entry.command, 20, entry.name);
    const correlation = BigInt(entry.correlation);
    assert.throws(
      () => decodeStatefulReply(Buffer.from(entry.hex, 'hex'), correlation, 'actorJoin'),
      Error,
      entry.name
    );
  }
});

test('actorJoin accepted reply tail round-trips receiveChunkLimitBytes and ' +
  'decodes old-format frames as not-advertised', () => {
  const correlation = 42n;
  const spot = { spotId: 'spot-1', generation: 3n } as const;
  const encoded = encodeStatefulReply(correlation, 0, 0, {
    kind: 'actorJoin',
    joinResult: 0,
    spot,
    membershipEpoch: 5n,
    receiveChunkLimitBytes: 32768
  });
  const decoded = decodeStatefulReply(encoded, correlation, 'actorJoin');
  assert.deepEqual(decoded.tail, {
    kind: 'actorJoin',
    joinResult: 0,
    spot,
    membershipEpoch: 5n,
    receiveChunkLimitBytes: 32768
  });

  // 0 means "not advertised" and is the default when the field is omitted.
  const encodedDefault = encodeStatefulReply(correlation, 0, 0, {
    kind: 'actorJoin',
    joinResult: 0,
    spot,
    membershipEpoch: 5n
  });
  const decodedDefault = decodeStatefulReply(encodedDefault, correlation, 'actorJoin');
  assert.equal(
    (decodedDefault.tail as { readonly receiveChunkLimitBytes?: number }).receiveChunkLimitBytes,
    0
  );

  // An old-format frame that stops right after membershipEpoch (no trailing
  // u32) still decodes — tolerant of an unpatched encoder.
  const oldFormatBody = Buffer.concat([
    Buffer.from([0x5a, 0x4d, 1, 20, 0]),
    (() => {
      const buffer = Buffer.alloc(8);
      buffer.writeBigUInt64BE(correlation);
      return buffer;
    })(),
    Buffer.alloc(4), // terminalResult = 0
    Buffer.alloc(4), // failureCode = 0
    Buffer.from([0, 0, 0, 0]), // joinResult = 0 (u32)
    (() => {
      // joinBodyLength covers spotRef + membershipEpoch only, no cap field.
      const spotIdBytes = Buffer.from(spot.spotId, 'utf8');
      const spotRef = Buffer.concat([
        Buffer.from([spotIdBytes.byteLength]),
        spotIdBytes,
        (() => {
          const value = Buffer.alloc(8);
          value.writeBigUInt64BE(spot.generation);
          return value;
        })()
      ]);
      const epoch = Buffer.alloc(8);
      epoch.writeBigUInt64BE(5n);
      const body = Buffer.concat([spotRef, epoch]);
      const length = Buffer.alloc(2);
      length.writeUInt16BE(body.byteLength);
      return Buffer.concat([length, body]);
    })()
  ]);
  const oldFormatDecoded = decodeStatefulReply(oldFormatBody, correlation, 'actorJoin');
  assert.deepEqual(oldFormatDecoded.tail, {
    kind: 'actorJoin',
    joinResult: 0,
    spot,
    membershipEpoch: 5n,
    receiveChunkLimitBytes: 0
  });

  assert.throws(() => encodeStatefulReply(correlation, 0, 0, {
    kind: 'actorJoin',
    joinResult: 0,
    spot,
    membershipEpoch: 5n,
    receiveChunkLimitBytes: 67_108_865
  }));
});

test('effectiveActorJoinChunkLimitBytes takes the minimum of configured, advertised and the conservative floor', () => {
  // 0 from the target means "not advertised" and must not participate in the min.
  assert.equal(effectiveActorJoinChunkLimitBytes(1024 * 1024, 0), 32 * 1024);
  assert.equal(effectiveActorJoinChunkLimitBytes(1024 * 1024, undefined), 32 * 1024);
  assert.equal(effectiveActorJoinChunkLimitBytes(1024 * 1024, 4096), 4096);
  assert.equal(effectiveActorJoinChunkLimitBytes(1024, 4096), 1024);
});

test('restoreRelocationAdapterState restores through the adapter and returns the same instance', async () => {
  const events: string[] = [];
  const adapter: ZLinkRelocationStateAdapterLike<{ readonly id: number }> = {
    async capture() { return Buffer.alloc(0); },
    async restore(instance, payload) {
      events.push(`restore:${instance.id}:${Buffer.from(payload).toString()}`);
    }
  };
  const instance = { id: 7 };
  const resolved = await restoreRelocationAdapterState(
    adapter,
    instance,
    Buffer.from('payload-bytes'),
    new AbortController().signal
  );
  assert.equal(resolved, instance);
  assert.deepEqual(events, ['restore:7:payload-bytes']);
});

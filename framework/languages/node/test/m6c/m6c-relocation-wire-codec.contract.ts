import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  decodeMaintenanceRelocationControl,
  decodeSessionRelocationRoute,
  decodeSessionRelocationSeal,
  decodeSessionRelocationSealed,
  encodeMaintenanceRelocationControl,
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  encodeSessionRelocationSealed,
  type ServiceMaintenanceRelocationControl,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

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

test('commands 30, 31, 34, 40, and 52 match the shared golden vectors', () => {
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

  assert.deepEqual(entries.map(entry => entry.command), [30, 31, 34, 40, 52, 52]);
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

  const data = values[1];
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

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  decodeCanonicalActorJoinRecoverySavedWork,
  encodeCanonicalActorJoinRecoverySavedWork,
  type CanonicalActorJoinRecoveryRequest
} from '../../packages/framework/src/runtime/foundation/actor-join-recovery-codec';
import {
  decodeStatefulHeader,
  encodeActorCreateHeader,
  encodeUserSpotCloseHeader,
  encodeUserSpotCreateHeader,
  type ServiceActorCreateRecord,
  type ServiceStatefulWireRecord,
  type ServiceUserSpotCloseRecord,
  type ServiceUserSpotCreateRecord
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import {
  decodeActorCreate49,
  decodeUserSpotClose48,
  decodeUserSpotCreate47,
  decodeZljrRecordV1,
  encodeActorCreate49,
  encodeUserSpotClose48,
  encodeUserSpotCreate47,
  encodeZljrRecordV1
} from '../../packages/framework/src/runtime/protocol/service_wire_pilot_codec.generated';

interface GoldenEntry {
  readonly name: string;
  readonly hex: string;
}

interface CommandGolden {
  readonly format: string;
  readonly commandId: 47 | 48 | 49;
  readonly canonical: GoldenEntry;
  readonly malformed: readonly GoldenEntry[];
}

interface ZljrGolden {
  readonly canonical: GoldenEntry;
  readonly malformed: readonly GoldenEntry[];
}

function commandGolden(name: string): CommandGolden {
  return JSON.parse(readFileSync(
    `../../runtime/protocol/golden/${name}.json`,
    'utf8'
  )) as CommandGolden;
}

function zljrGolden(): ZljrGolden {
  return JSON.parse(readFileSync(
    '../../runtime/protocol/golden/zljr-v1.json',
    'utf8'
  )) as ZljrGolden;
}

function handCommandRoundTrip(encoded: Uint8Array): Buffer {
  const decoded = decodeStatefulHeader(encoded);
  switch (decoded.kind) {
    case 'userSpotCreate':
      return encodeUserSpotCreateHeader(decoded);
    case 'userSpotClose':
      return encodeUserSpotCloseHeader(decoded);
    case 'actorCreate':
      return encodeActorCreateHeader(decoded);
    default:
      throw new Error(`Unexpected stateful command kind '${decoded.kind}'.`);
  }
}

function generatedCommandRoundTrip(command: 47 | 48 | 49, encoded: Uint8Array): Buffer {
  switch (command) {
    case 47:
      return Buffer.from(encodeUserSpotCreate47(decodeUserSpotCreate47(encoded)));
    case 48:
      return Buffer.from(encodeUserSpotClose48(decodeUserSpotClose48(encoded)));
    case 49:
      return Buffer.from(encodeActorCreate49(decodeActorCreate49(encoded)));
  }
}

function generatedCommandDecode(command: 47 | 48 | 49, encoded: Uint8Array): void {
  switch (command) {
    case 47: decodeUserSpotCreate47(encoded); return;
    case 48: decodeUserSpotClose48(encoded); return;
    case 49: decodeActorCreate49(encoded); return;
  }
}

function assertCommandProjection(command: 47 | 48 | 49, decoded: ServiceStatefulWireRecord): void {
  if (command === 47) {
    assert.equal(decoded.kind, 'userSpotCreate');
    const record = decoded as ServiceUserSpotCreateRecord;
    const generated = decodeUserSpotCreate47(encodeUserSpotCreateHeader(record));
    assert.equal(Buffer.from(generated.sourceNodeRid).toString('utf8'), record.sourceNodeRid);
    assert.equal(Buffer.from(generated.reservation.targetNodeRid).toString('utf8'),
      record.reservation.targetNodeRid);
    assert.equal(generated.reservation.pendingCapacityDelta,
      record.reservation.pendingCapacityDelta);
    return;
  }
  if (command === 48) {
    assert.equal(decoded.kind, 'userSpotClose');
    const record = decoded as ServiceUserSpotCloseRecord;
    const generated = decodeUserSpotClose48(encodeUserSpotCloseHeader(record));
    assert.equal(Buffer.from(generated.sourceNodeRid).toString('utf8'), record.sourceNodeRid);
    assert.equal(generated.target.expectedAuthorityOwnerGeneration,
      record.target.authorityOwnerGeneration);
    return;
  }
  assert.equal(decoded.kind, 'actorCreate');
  const record = decoded as ServiceActorCreateRecord;
  const generated = decodeActorCreate49(encodeActorCreateHeader(record));
  assert.equal(Buffer.from(generated.sourceNodeRid).toString('utf8'), record.sourceNodeRid);
  assert.equal(generated.reservation.pendingCapacityDelta,
    record.reservation.pendingCapacityDelta);
}

test('batch-4 commands 47, 48, and 49 hand/generated codecs match the complete golden frames', () => {
  for (const name of ['user-spot-create-v1', 'user-spot-close-v1', 'actor-create-v1']) {
    const golden = commandGolden(name);
    const encoded = Buffer.from(golden.canonical.hex, 'hex');
    const decoded = decodeStatefulHeader(encoded);
    assertCommandProjection(golden.commandId, decoded);
    assert.deepEqual(handCommandRoundTrip(encoded), encoded, `hand:${golden.canonical.name}`);
    assert.deepEqual(generatedCommandRoundTrip(golden.commandId, encoded), encoded,
      `generated:${golden.canonical.name}`);
  }
});

test('batch-4 commands 47, 48, and 49 hand/generated codecs reject the same malformed goldens', () => {
  for (const name of ['user-spot-create-v1', 'user-spot-close-v1', 'actor-create-v1']) {
    const golden = commandGolden(name);
    for (const malformed of golden.malformed) {
      const encoded = Buffer.from(malformed.hex, 'hex');
      assert.throws(() => decodeStatefulHeader(encoded), Error, `hand:${malformed.name}`);
      assert.throws(() => generatedCommandDecode(golden.commandId, encoded), Error,
        `generated:${malformed.name}`);
    }
  }
});

function goldenRecoveryRequest(): CanonicalActorJoinRecoveryRequest {
  return {
    actorId: 'actor-a',
    actorType: 'sample.Actor',
    handoffId: '00112233445566778899aabbccddeeff',
    sourceSpotId: 'source-spot',
    sourceNodeRid: 'src',
    actorGeneration: 7n,
    actorAuthorityOwnerGeneration: 11n,
    actorNodeGeneration: 13n,
    expectedOwnerLeaseGeneration: 17n,
    relocationId: '00112233445566778899aabbccddeeff',
    relocationContentType: 'application/vnd.zlink.actor-relocation.snapshot',
    requestContentType: 'application/json',
    request: Buffer.from('{}'),
    targetSpotId: 'target-spot',
    targetNodeRid: 'dst',
    targetNodeGeneration: 19n,
    targetSpotGeneration: 23n,
    targetAuthorityOwnerGeneration: 12n,
    targetSpotAuthorityOwnerGeneration: 29n,
    coordinator: {
      ownerId: 'owner-a',
      leaseGeneration: 17n,
      nodeRid: 'src',
      nodeGeneration: 13n,
      expectedAuthorityStoreVersion: 'store-v1'
    },
    operationId: { high: 31n, low: 37n },
    replyContentType: 'application/json',
    reply: Buffer.from('[]')
  };
}

function handRejectsZljr(encoded: Uint8Array): boolean {
  try {
    return decodeCanonicalActorJoinRecoverySavedWork(encoded) === undefined;
  } catch {
    return true;
  }
}

test('batch-4 ZLJR hand/generated codecs match the fixed 1958-byte golden record', () => {
  const golden = zljrGolden().canonical;
  const encoded = Buffer.from(golden.hex, 'hex');
  assert.equal(encoded.byteLength, 1958);
  assert.deepEqual(encodeCanonicalActorJoinRecoverySavedWork(goldenRecoveryRequest()), encoded);

  const hand = decodeCanonicalActorJoinRecoverySavedWork(encoded);
  assert.ok(hand);
  assert.equal(hand.request.actorId, 'actor-a');
  assert.deepEqual(hand.request.request, Buffer.from('{}'));
  assert.deepEqual(hand.reply, Buffer.from('[]'));

  const generated = decodeZljrRecordV1(encoded);
  assert.equal(Buffer.from(generated.source.nodeRid).toString('utf8'), '737263');
  assert.equal(JSON.parse(Buffer.from(generated.metadata).toString('utf8')).Request.ActorId,
    hand.request.actorId);
  assert.deepEqual(Buffer.from(encodeZljrRecordV1(generated)), encoded);
});

test('batch-4 ZLJR hand/generated codecs reject every adjudicated malformed record', () => {
  for (const malformed of zljrGolden().malformed) {
    const encoded = Buffer.from(malformed.hex, 'hex');
    assert.equal(handRejectsZljr(encoded), true, `hand:${malformed.name}`);
    assert.throws(() => decodeZljrRecordV1(encoded), Error, `generated:${malformed.name}`);
  }
});

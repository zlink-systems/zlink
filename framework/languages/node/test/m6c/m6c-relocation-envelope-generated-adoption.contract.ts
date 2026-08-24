import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  decodeServiceRelocationEnvelope,
  encodeServiceRelocationEnvelope
} from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import {
  decodeRelocationEnvelopeV1,
  encodeRelocationEnvelopeV1
} from '../../packages/framework/src/runtime/protocol/service_wire_pilot_codec.generated';

function goldenEnvelope(): Buffer {
  const fixture = JSON.parse(readFileSync(
    '../../runtime/protocol/golden/relocation-envelope-v1.json',
    'utf8'
  )) as { readonly logicalHex: string };
  const projection = JSON.parse(readFileSync(
    '../../runtime/protocol/generated/fixtures/relocation-envelope-v1-pilot.json',
    'utf8'
  )) as { readonly hex: string };
  assert.equal(projection.hex, fixture.logicalHex);
  return Buffer.from(fixture.logicalHex, 'hex');
}

function assertRejectedByBoth(encoded: Uint8Array): void {
  assert.throws(() => decodeServiceRelocationEnvelope(encoded, 1n), Error);
  assert.throws(() => decodeRelocationEnvelopeV1([encoded]), Error);
}

test('relocation envelope runtime and generated codecs are byte-equal on the shared golden', () => {
  const golden = goldenEnvelope();
  const hand = decodeServiceRelocationEnvelope(golden, 7n);
  const generated = decodeRelocationEnvelopeV1([golden]);

  assert.deepEqual(
    encodeServiceRelocationEnvelope(hand, hand.applicationVersion!),
    golden
  );
  assert.deepEqual(Buffer.from(encodeRelocationEnvelopeV1(generated)), golden);
});

test('relocation envelope runtime and generated codecs reject all adjudicated cross-section mutations', () => {
  const membershipMutation = goldenEnvelope();
  membershipMutation[132] = 4;
  assertRejectedByBoth(membershipMutation);

  const timerReferenceMutation = goldenEnvelope();
  timerReferenceMutation[402] = 'x'.charCodeAt(0);
  assertRejectedByBoth(timerReferenceMutation);

  const golden = goldenEnvelope();
  const emptyApplicationStates = Buffer.concat([
    golden.subarray(0, 48),
    Buffer.alloc(16)
  ]);
  assert.equal(emptyApplicationStates.byteLength, 64);
  assertRejectedByBoth(emptyApplicationStates);
});

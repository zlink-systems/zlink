import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  decodeActorRequestCommand,
  decodeActorSendCommand,
  encodeMetadataFrame,
  decodeSpotRequestCommand,
  decodeSpotSendCommand,
  encodeActorRequestCommand,
  encodeActorSendCommand,
  encodeSpotRequestCommand,
  encodeSpotSendCommand
} from '../../packages/framework/src/runtime/protocol/service_wire_codec.generated';
import {
  encodeActorHeader,
  encodeSpotHeader
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import { encodeApplicationPayload } from '../../packages/framework/src/runtime/foundation/service-wire-m6a-codec';

const context = {
  effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 0xffff_fdf6,
  effectiveCompleteMessageBytes: 0xffff_ffff
};
const payload = encodeApplicationPayload({
  packetName: 'request',
  contentType: 'application/json',
  payload: Buffer.from('{}')
});
const spot = {
  spot: { spotId: 'spot-a', generation: 7n },
  targetNodeRid: 'node-b',
  targetNodeGeneration: 3n,
  authorityOwnerGeneration: 9n,
  ownerLeaseGeneration: 4n,
  storeVersion: 'local-only'
};
const actor = {
  actor: { nodeRid: 'node-b', actorId: 'actor-a', generation: 5n },
  targetNodeGeneration: 3n,
  authorityOwnerGeneration: 9n,
  ownerLeaseGeneration: 4n
};

test('stateful commands 21, 22, 24, and 25 use generated wire bytes', () => {
  const cases = [
    [encodeSpotHeader('spotSend', 'source', spot), decodeSpotSendCommand, encodeSpotSendCommand],
    [
      encodeSpotHeader('spotRequest', 'source', spot, 11n),
      decodeSpotRequestCommand,
      encodeSpotRequestCommand
    ],
    [encodeActorHeader('actorSend', actor), decodeActorSendCommand, encodeActorSendCommand],
    [
      encodeActorHeader('actorRequest', actor, 12n),
      decodeActorRequestCommand,
      encodeActorRequestCommand
    ]
  ] as const;
  for (const [header, decode, encode] of cases) {
    const frames = [header, payload];
    const decoded = decode(frames, context) as never;
    assert.deepEqual(
      encode(decoded, context).map((frame) => Buffer.from(frame)),
      frames
    );
  }
});

test('Spot request metadata, operation, deadline and Follow hop use generated framing', () => {
  const metadata = encodeMetadataFrame({ entries: [{ key: 'trace', value: 'java' }] }, context);
  const header = encodeSpotHeader(
    'spotRequest',
    'source',
    spot,
    13n,
    { high: 3n, low: 8n },
    5000n,
    2,
    metadata
  );
  const frames = [header, metadata, payload];
  const decoded = decodeSpotRequestCommand(frames, context);
  assert.equal(decoded.flags, 1);
  assert.deepEqual(decoded.operation, { high: 3n, low: 8n });
  assert.equal(decoded.remainingDeadlineMs, 5000n);
  assert.equal(decoded.messageFollowHopCount, 2);
  assert.deepEqual(
    encodeSpotRequestCommand(decoded, context).map((frame) => Buffer.from(frame)),
    frames.map((frame) => Buffer.from(frame))
  );
});

const assert = require('node:assert/strict');
const test = require('node:test');
const fixture = require('../../../../runtime/protocol/golden/actor-accepted-record-v1.json');
const {
  decodeQueuedHandoffPacket,
  encodeActorAcceptedRecord
} = require('../../packages/framework/dist/runtime/actors/actor-accepted-record');
const { decodeRoutingId } = require('../../packages/framework/dist/runtime/routing-id');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { decodeStreamHeader } = require('../../packages/framework/dist/runtime/streams/protocol');

for (const request of [true, false]) {
  test(`restores a Java frozen Actor ${request ? 'request' : 'send'}`, () => {
    const payload = Buffer.from(request ? fixture.requestHex : fixture.sendHex, 'hex');
    const packet = decodeQueuedHandoffPacket({ sequence: 9n, payload });

    assert.equal(packet.index, 8);
    assert.equal(packet.returnResponse, request);
    assert.equal(decodeStreamHeader(Buffer.from(packet.header, 'base64')).name, 'accepted-packet');
    assert.deepEqual(Buffer.from(packet.payload, 'base64'), Buffer.from([0, 255, 3]));
    assert.equal(
      packet.messageFollowContext.operationId,
      request ? '000000000000001f0000000000000029' : '000000000000001f000000000000002a'
    );
    assert.equal(packet.messageFollowContext.objectGeneration, '17');
    assert.equal(packet.source?.nodeRidHex, request ? '00ff01' : undefined);
    assert.equal(packet.source?.replyRouteId, request ? '37' : undefined);

    const encoded = encodeActorAcceptedRecord(
      {
        objectKind: 'actor',
        key: encodeAuthorityKey('actor', 'accepted-actor').value,
        objectGeneration: 17n
      },
      packet,
      () => ({
        ownerId: 'source-owner',
        leaseGeneration: 13n,
        nodeRid: decodeRoutingId('', '00ff01'),
        nodeGeneration: 11n
      })
    );

    assert.deepEqual(encoded.canonicalBytes, payload);
  });
}

test('rejects a malformed frozen multipart record and a different Actor identity', () => {
  const payload = Buffer.from(fixture.requestHex, 'hex');
  assert.throws(() =>
    decodeQueuedHandoffPacket({ sequence: 9n, payload }, 'target-owner', 'other-actor')
  );
  assert.throws(() =>
    decodeQueuedHandoffPacket({ sequence: 9n, payload: payload.subarray(0, -1) })
  );
});

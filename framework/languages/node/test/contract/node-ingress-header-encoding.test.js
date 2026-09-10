'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

const correlation = '00112233445566778899aabbccddeeff';
const deadline = Date.parse('2026-09-10T00:00:00.000Z');

function bytes(part) {
  return Buffer.isBuffer(part) ? part : part.data();
}

test('outbound binary channel header writes escaped UTF-8 without a whole JSON string', t => {
  let stringifyCalls = 0;
  t.mock.method(JSON, 'stringify', () => {
    stringifyCalls += 1;
    throw new Error('channel header must not stringify a temporary object');
  });

  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
    1,
    'ch"\n\\\ud800',
    'M😀\udc00',
    Buffer.from('00ff80', 'hex'),
    deadline,
    undefined,
    undefined,
    correlation,
    false
  );
  try {
    assert.equal(stringifyCalls, 0);
    assert.deepEqual(
      bytes(parts[0]),
      Buffer.from('{"formatMarker":242,"kind":1,"channelName":"ch\\"\\n\\\\\\ud800","messageName":"M😀\\udc00","contentType":"application/octet-stream","correlationId":"00112233445566778899aabbccddeeff","deadline":"2026-09-10T00:00:00.000Z","topic":null,"errorCode":null,"errorMessage":null,"metadata":{}}')
    );
  } finally {
    envelope.closeMessages(parts);
  }
});

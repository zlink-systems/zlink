'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const json = require('../../packages/framework/dist/runtime/messaging/framework-json-v1');

function parts(changes = {}) {
  const bytes = Buffer.from(JSON.stringify({
    formatMarker: 242, kind: 1, channelName: 'channel', messageName: 'Packet',
    contentType: 'application/json', correlationId: 'request-1', deadline: null,
    topic: null, metadata: {}, ...changes
  }));
  return [{ data() { return bytes; } }];
}

test('canonical header fast path preserves the legacy decoded result', t => {
  const input = parts();
  const legacy = json.parseFrameworkJsonV1(input[0].data().toString(), {
    rejectPropertyName: property => ['__proto__', 'constructor', 'prototype'].includes(property)
  });
  legacy.errorCode = null;
  legacy.errorMessage = null;
  legacy.source = undefined;
  legacy.flowId = undefined;
  legacy.flowOrigin = undefined;
  Object.freeze(legacy.metadata);
  t.mock.method(json, 'parseFrameworkJsonV1', () => assert.fail('canonical header used the legacy parser'));

  const header = envelope.decodeChannelHeader(input);

  assert.deepEqual(header, legacy);
  assert.ok(Object.isFrozen(header.metadata));
});

test('header decoding preserves omitted optional fields and unknown fields', () => {
  const header = envelope.decodeChannelHeader(rawParts(
    '{"formatMarker":242,"kind":1,"channelName":"channel","messageName":"Packet",'
      + '"contentType":"application/json","correlationId":"request-1","deadline":null,'
      + '"topic":null,"metadata":{},"unknown":{"nested":[true,3.5]}}'
  ));
  assert.equal(header.errorCode, null);
  assert.equal(header.errorMessage, null);
  assert.equal(header.source, undefined);
  assert.deepEqual(header.unknown, { nested: [true, 3.5] });
});

test('header decoding preserves legacy malformed JSON errors and prototype-key rejection', () => {
  assert.throws(
    () => envelope.decodeChannelHeader(rawParts('{"formatMarker":242,"kind":1')),
    error => {
      assert.equal(error.constructor, SyntaxError);
      return true;
    }
  );
  for (const property of ['__proto__', 'constructor', 'prototype']) {
    assert.throws(
      () => envelope.decodeChannelHeader(rawParts(
        '{"formatMarker":242,"kind":1,"channelName":"channel","messageName":"Packet",'
          + '"contentType":"application/json","correlationId":"request-1","deadline":null,'
          + '"topic":null,"metadata":{},"' + property + '":{}}'
      )),
      /is not allowed/
    );
  }
});

test('header normalization preserves validation and Off ignores observation fields', () => {
  assert.throws(() => envelope.decodeChannelHeader(parts({ channelName: 1 })), /channelName/);
  assert.throws(() => envelope.decodeChannelHeader(parts({ correlationId: null })), /correlationId/);
  assert.throws(() => envelope.decodeChannelHeader(parts({ flowId: 'invalid', flowOrigin: 99 })), /flowId/);
  assert.throws(() => envelope.decodeChannelHeader(parts({ metadata: { key: 1 } })), /metadata/);
  const off = envelope.decodeChannelHeader(parts({ flowId: 'invalid', flowOrigin: 99 }), false);
  assert.equal(off.flowId, undefined);
  assert.equal(off.flowOrigin, undefined);
  assert.equal(off.correlationId, 'request-1');
});

function rawParts(value) {
  const bytes = Buffer.from(value);
  return [{ data() { return bytes; } }];
}

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

test('header validation retains the parsed header and normalizes optional fields', t => {
  const parse = json.parseFrameworkJsonV1;
  let parsed;
  t.mock.method(json, 'parseFrameworkJsonV1', (...args) => (parsed = parse(...args)));
  const header = envelope.decodeChannelHeader(parts({
    flowId: '01992176-0000-7000-8000-000000000001', flowOrigin: 3
  }));
  assert.equal(header, parsed);
  assert.equal(header.flowOrigin, 'Application');
  assert.equal(header.errorCode, null);
  assert.equal(header.errorMessage, null);
  assert.equal(header.source, undefined);
  assert.ok(Object.isFrozen(header.metadata));
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

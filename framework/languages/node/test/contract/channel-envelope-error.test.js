'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const payloadCodec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
const { Message } = require('@zlink-systems/zlink');

function readable(parts) {
  return parts.map((part) => typeof part.data === 'function'
    ? part
    : { data: () => Buffer.from(part) });
}

test('selective application serializer leaves framework payload encoding on JSON', () => {
  const serializer = {
    canSerialize(value) {
      return value?.kind === 'application';
    },
    serialize() {
      throw new Error('serializer must not encode framework-owned payloads');
    },
    deserialize() {
      return { decoded: true };
    }
  };
  const registry = new Map([['application/x-test', serializer]]);

  assert.equal(payloadCodec.selectSerializer({ kind: 'framework' }, registry), undefined);
  assert.equal(payloadCodec.selectSerializer({ kind: 'application' }, registry), serializer);
  assert.equal(payloadCodec.selectDefaultSerializer(registry), serializer);
});

test('selective application serializer decodes its wire payload and JSON fallback separately', () => {
  let deserializeCalls = 0;
  const serializer = {
    canSerialize(value) {
      return value?.kind === 'application';
    },
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(`wire:${value.kind}`));
    },
    deserialize(payload) {
      deserializeCalls += 1;
      assert.equal(payload.getString('utf8'), 'wire:application');
      return { kind: 'application', decoded: true };
    }
  };
  const registry = new Map([['application/x-test', serializer]]);
  const jsonMessage = Message.from(Buffer.from('{"kind":"framework"}'));
  const applicationMessage = Message.from(Buffer.from('wire:application'));
  try {
    assert.deepEqual(
      payloadCodec.decodeFrameworkPayloadMessage(jsonMessage, registry),
      { kind: 'framework' }
    );
    assert.deepEqual(
      payloadCodec.decodeFrameworkPayloadMessage(applicationMessage, registry),
      { kind: 'application', decoded: true }
    );
    assert.equal(deserializeCalls, 1);
  } finally {
    jsonMessage.close();
    applicationMessage.close();
  }
});

test('channel correlation follows the request versus one-way contract', () => {
  const sendParts = envelope.encodeChannelEnvelopeParts(3, 'api', 'Notice', { value: 'one-way' });
  const requestParts = envelope.encodeChannelEnvelopeParts(1, 'api', 'Lookup', { id: 'a' });
  const publishParts = envelope.encodeChannelPublishEnvelopeParts(
    'events',
    'updates',
    'Changed',
    { value: 'published' }
  );
  try {
    assert.equal(envelope.decodeChannelHeader(readable(sendParts)).correlationId, null);
    const requestCorrelation = envelope.decodeChannelHeader(readable(requestParts)).correlationId;
    assert.match(requestCorrelation, /^[\x00-\x7f]{1,64}$/);
    assert.equal(envelope.decodeChannelHeader(readable(publishParts)).correlationId, null);
    assert.throws(
      () => envelope.encodeChannelEnvelopeParts(3, 'api', 'Notice', { value: 'bad' }, undefined, undefined, undefined, 'corr'),
      /must not contain correlationId/
    );
  } finally {
    envelope.closeMessages(sendParts);
    envelope.closeMessages(requestParts);
    envelope.closeMessages(publishParts);
  }
});

test('channel envelope keeps an absolute deadline established before payload encoding', () => {
  const deadlineUnixMs = Date.now() + 5_000;
  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
    1,
    'api',
    'Lookup',
    { id: 'a' },
    deadlineUnixMs
  );
  try {
    assert.equal(
      Date.parse(envelope.decodeChannelHeader(readable(parts)).deadline),
      deadlineUnixMs
    );
  } finally {
    envelope.closeMessages(parts);
  }
});

test('channel header rejects correlation values that do not match the message kind', () => {
  const commandWithCorrelation = readable([
    Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 3,
      channelName: 'api',
      messageName: 'Notice',
      contentType: 'application/json',
      correlationId: 'unexpected',
      deadline: null,
      topic: null,
      metadata: {}
    })),
    Buffer.from('{}')
  ]);
  const requestWithoutCorrelation = readable([
    Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 1,
      channelName: 'api',
      messageName: 'Lookup',
      contentType: 'application/json',
      correlationId: null,
      deadline: null,
      topic: null,
      metadata: {}
    })),
    Buffer.from('{}')
  ]);
  assert.throws(
    () => envelope.decodeChannelHeader(commandWithCorrelation),
    /must not contain correlationId/
  );
  assert.throws(
    () => envelope.decodeChannelHeader(requestWithoutCorrelation),
    /requires correlationId/
  );
});

test('channel header treats nullable flow fields as absent', () => {
  const header = readable([
    Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 1,
      channelName: 'api',
      messageName: 'Lookup',
      contentType: 'application/json',
      correlationId: 'cross-language-request',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null,
      source: null,
      metadata: {},
      flowId: null,
      flowOrigin: null
    }))
  ]);

  assert.equal(envelope.decodeChannelHeader(header).flowId, undefined);
  assert.equal(envelope.decodeChannelHeader(header).flowOrigin, undefined);
});

test('channel envelope borrows the received payload until dispatch closes it', () => {
  const payload = Buffer.from('{"value":"borrowed"}');
  const parts = [
    { data: () => Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 3,
      channelName: 'api',
      messageName: 'Notice',
      contentType: 'application/json',
      correlationId: null,
      deadline: null,
      topic: null,
      metadata: {}
    })) },
    { data: () => payload }
  ];

  const decoded = envelope.decodeChannelEnvelope(parts);

  assert.strictEqual(decoded.payload, payload);
  assert.deepEqual(JSON.parse(decoded.payload.toString()), { value: 'borrowed' });
});

test('IMP-ND-03 channel Error preserves the public framework kind without retry hints', () => {
  const requestParts = envelope.encodeChannelEnvelopeParts(1, 'api', 'Lookup', { id: 'a' });
  const request = envelope.decodeChannelEnvelope(readable(requestParts));
  const replyParts = envelope.encodeChannelErrorReplyParts(
    request.header,
    new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.Unavailable,
      'route is converging'
    )
  );
  try {
    assert.throws(
      () => envelope.decodeChannelReply(readable(replyParts)),
      (error) => error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
        && !('isRetriable' in error)
        && error.message === 'route is converging'
    );
  } finally {
    envelope.closeMessages(requestParts);
    envelope.closeMessages(replyParts);
  }
});

test('IMP-ND-03 channel Error maps a non-framework error to InternalFailure', () => {
  const requestParts = envelope.encodeChannelEnvelopeParts(1, 'api', 'Lookup', { id: 'a' });
  const request = envelope.decodeChannelEnvelope(readable(requestParts));
  const failure = new TypeError('bad handler input');
  const replyParts = envelope.encodeChannelErrorReplyParts(request.header, failure);
  try {
    assert.throws(
      () => envelope.decodeChannelReply(readable(replyParts)),
      (error) => error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.InternalFailure
        && error.message === failure.message
    );
  } finally {
    envelope.closeMessages(requestParts);
    envelope.closeMessages(replyParts);
  }
});

test('IMP-ND-03 unknown channel content type fails before handler payload delivery', () => {
  const envelopeValue = {
    header: {
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 1,
      channelName: 'api',
      messageName: 'Lookup',
      contentType: 'application/x-unknown',
      correlationId: 'unknown-content-type',
      deadline: null,
      topic: null,
      metadata: {}
    },
    payload: Buffer.from([0x01, 0x02, 0x03])
  };

  assert.throws(
      () => envelope.decodeChannelPayload(envelopeValue),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
      && /unsupported channel content type/.test(error.message)
  );
});

test('IMP-ND-03 unknown channel content type in a reply does not fall back to JSON', () => {
  const replyParts = readable([
    Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 2,
      channelName: 'api',
      messageName: 'Lookup',
      contentType: 'application/x-unknown',
      correlationId: 'unknown-reply-content-type',
      deadline: null,
      topic: null,
      metadata: {}
    })),
    Buffer.from('{"id":"a"}')
  ]);

  assert.throws(
    () => envelope.decodeChannelReply(replyParts),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
      && /unsupported channel content type/.test(error.message)
  );
});

test('IMP-ND-03 unknown channel content type in an empty reply is still a ProtocolError', () => {
  const replyParts = readable([
    Buffer.from(JSON.stringify({
      formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
      kind: 2,
      channelName: 'api',
      messageName: 'Lookup',
      contentType: 'application/x-unknown-empty',
      correlationId: 'unknown-empty-reply-content-type',
      deadline: null,
      topic: null,
      metadata: {}
    })),
    Buffer.alloc(0)
  ]);

  assert.throws(
    () => envelope.decodeChannelReply(replyParts),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
      && /unsupported channel content type/.test(error.message)
  );
});

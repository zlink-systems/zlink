'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const payloadCodec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
const creationPayloadCodec = require('../../packages/framework/dist/runtime/messaging/creation-payload-codec');
const frameworkJson = require('../../packages/framework/dist/runtime/messaging/framework-json-v1');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkStreamFrameMessageFactory } = require('../../packages/framework/dist/runtime/streams/stream-frame-factory');
const encodedPayloadStorage = require('../../packages/framework/dist/contracts/Common/encoded-payload-storage');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { Message } = require('@zlink-systems/zlink');

test('default payload codec enforces schema-independent framework-json-v1 rules', () => {
  assert.equal(frameworkJson.FRAMEWORK_JSON_V1_PROFILE, 'framework-json-v1');

  const encoded = payloadCodec.encodeFrameworkPayload({
    signed64: -9223372036854775808n,
    unsigned64: 18446744073709551615n,
    bytes: Uint8Array.from([1, 2]),
    ratio: 1.25
  });
  try {
    assert.equal(
      encoded.message.getString('utf8'),
      '{"signed64":"-9223372036854775808","unsigned64":"18446744073709551615","bytes":"AQI=","ratio":1.25}'
    );
  } finally {
    encoded.message.close();
  }

  assert.throws(
    () => payloadCodec.encodeFrameworkPayload({ value: -9223372036854775809n }),
    /outside the supported range/
  );
  assert.throws(
    () => payloadCodec.encodeFrameworkPayload({ value: 18446744073709551616n }),
    /outside the supported range/
  );
  assert.throws(
    () => payloadCodec.encodeFrameworkPayload({ value: Number.NaN }),
    /only accepts finite JSON numbers/
  );
  assert.throws(
    () => payloadCodec.encodeFrameworkPayload({ value: new Date(0) }),
    /does not implicitly encode Date/
  );
  assert.throws(
    () => payloadCodec.encodeFrameworkPayload({ value: { toJSON: () => 'hidden' } }),
    /does not implicitly invoke custom toJSON/
  );
});

test('default payload decode rejects BOM and duplicate properties at every depth', () => {
  for (const json of [
    '\ufeff{"value":1}',
    '{"value":1,"value":2}',
    '{"nested":{"value":1,"value":2}}'
  ]) {
    const message = Message.from(Buffer.from(json));
    try {
      assert.throws(
        () => payloadCodec.decodeFrameworkPayloadMessage(message),
        /PayloadDecodeFailed: framework payload is not valid JSON/
      );
    } finally {
      message.close();
    }
  }
});

test('STREAM default JSON encoding uses the same framework-json-v1 profile', () => {
  let wire;
  const factory = new ZLinkStreamFrameMessageFactory({
    flowCreationEnabled: () => false,
    messageFactory: {
      createTextMessage() { throw new Error('binary frame expected'); },
      createBinaryMessage(payload) {
        wire = payload;
        return {};
      }
    }
  });

  factory.createJsonFrameMessage(
    streamProtocol.ZLinkStreamMessageKind.Send,
    'Profile',
    new Map(),
    false,
    undefined,
    { generation: 18446744073709551615n, bytes: Uint8Array.from([1, 2]) }
  );
  assert.equal(
    Buffer.from(streamProtocol.decodeStreamFrame(wire).payload).toString('utf8'),
    '{"generation":"18446744073709551615","bytes":"AQI="}'
  );
  assert.throws(
    () => factory.createJsonFrameMessage(
      streamProtocol.ZLinkStreamMessageKind.Send,
      'Profile',
      new Map(),
      false,
      undefined,
      { ratio: Number.POSITIVE_INFINITY }
    ),
    /only accepts finite JSON numbers/
  );
});

function readable(parts) {
  return parts.map((part) => typeof part.data === 'function'
    ? part
    : { data: () => Buffer.from(part) });
}

test('lazy framework payload adopts runtime-owned bytes and JSON decode avoids data copies', () => {
  const bytes = Buffer.from('{"value":1}');
  const message = ZLinkBufferMessage.fromOwned(bytes);
  const wrapped = payloadCodec.wrapFrameworkPayloadMessage(message);
  const encoded = wrapped.toEncodedPayload();

  assert.equal(encodedPayloadStorage.borrowEncodedPayload(encoded), bytes);
  encoded.data = () => { throw new Error('JSON decode must not request a defensive byte copy'); };
  assert.deepEqual(wrapped.decode(), { value: 1 });
});

test('public encoded payload keeps defensive input and output copies', () => {
  const source = Buffer.from('stable');
  const payload = framework.ZLinkEncodedPayload.from(source);
  source.fill(0);

  const exposed = payload.data();
  exposed.fill(0);

  assert.equal(payload.getString(), 'stable');
  assert.equal(Buffer.from(payload.toBytes()).toString(), 'stable');
});

test('selective application serializer leaves framework payload encoding on JSON', () => {
  class FrameworkPayload {}
  class ApplicationPayload {}
  let selectionCalls = 0;
  const serializer = {
    canSerialize(value) {
      selectionCalls += 1;
      return value instanceof ApplicationPayload;
    },
    serialize() {
      throw new Error('serializer must not encode framework-owned payloads');
    },
    deserialize() {
      return { decoded: true };
    }
  };
  const registry = new Map([['application/x-test', serializer]]);

  assert.equal(payloadCodec.selectSerializer(new FrameworkPayload(), registry), undefined);
  assert.equal(payloadCodec.selectSerializer(new FrameworkPayload(), registry), undefined);
  assert.equal(payloadCodec.selectSerializer(new ApplicationPayload(), registry), serializer);
  assert.equal(payloadCodec.selectSerializer(new ApplicationPayload(), registry), serializer);
  assert.equal(selectionCalls, 2);
  assert.equal(payloadCodec.selectDefaultSerializer(registry), serializer);
});

test('wire content type selects the exact serializer without parsing payload bytes', () => {
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
      payloadCodec.decodeFrameworkPayloadMessage(
        applicationMessage,
        registry,
        undefined,
        'application/x-test'
      ),
      { kind: 'application', decoded: true }
    );
    assert.equal(deserializeCalls, 1);
    assert.throws(
      () => payloadCodec.decodeFrameworkPayloadMessage(
        applicationMessage,
        registry,
        undefined,
        'application/x-unknown'
      ),
      /PayloadDecodeFailed: unsupported framework content type/
    );
  } finally {
    jsonMessage.close();
    applicationMessage.close();
  }
});

test('creation payload preserves the selected serializer across the durable envelope', () => {
  const serializer = {
    canSerialize(value) {
      return value?.kind === 'application';
    },
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(`wire:${value.kind}`));
    },
    deserialize(payload) {
      assert.equal(payload.getString('utf8'), 'wire:application');
      return { kind: 'application', decoded: true };
    }
  };
  const registry = new Map([['application/x-test', serializer]]);
  const encoded = creationPayloadCodec.encodeFrameworkCreationPayload(
    { kind: 'application' },
    registry
  );

  assert.deepEqual(
    creationPayloadCodec.decodeFrameworkCreationPayload(encoded, registry),
    { kind: 'application', decoded: true }
  );
  assert.throws(
    () => creationPayloadCodec.decodeFrameworkCreationPayload(Buffer.from('invalid'), registry),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
  );
});

test('creation payload represents an omitted request as JSON null', () => {
  const encoded = creationPayloadCodec.encodeFrameworkCreationPayload(undefined);

  assert.equal(creationPayloadCodec.decodeFrameworkCreationPayload(encoded), null);
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

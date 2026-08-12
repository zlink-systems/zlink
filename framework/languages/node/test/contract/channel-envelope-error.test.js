'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');

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

const codecSelectionFixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/codec-selection-v1.json'
), 'utf8'));

function fixtureSerializer(name) {
  return {
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify({ name, value })));
    },
    deserialize() {
      return name;
    }
  };
}

test('codec registry normalization consumes the shared conformance fixture', () => {
  assert.equal(codecSelectionFixture.fixture, 'zlink.framework.codec-selection');
  assert.equal(codecSelectionFixture.version, 1);

  for (const scenario of codecSelectionFixture.normalizationScenarios) {
    const builder = new framework.DefaultZLinkCodecRegistryBuilder();
    if (scenario.expectedError !== undefined) {
      assert.throws(
        () => builder.addSerializer(scenario.input, fixtureSerializer(scenario.name)),
        (error) => error instanceof framework.ZLinkConfigurationException
      );
      continue;
    }
    builder.addSerializer(scenario.input, fixtureSerializer(scenario.name));
    assert.deepEqual([...builder.registeredSerializers.keys()], [scenario.expected]);
  }

  const duplicate = codecSelectionFixture.normalizedDuplicateScenario;
  const selected = fixtureSerializer('selected');
  const builder = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer(duplicate.registrationInputs[0], fixtureSerializer('replaced'))
    .addSerializer(duplicate.registrationInputs[1], selected);
  assert.equal(builder.registeredSerializers.size, duplicate.finalEntryCount);
  assert.equal(builder.registeredSerializers.get('application/x-base'), selected);

  let capturedCodecs;
  const registrationOptions = framework.createFrameworkOptions((options) => {
    capturedCodecs = options.codecs();
    capturedCodecs.addSerializer('application/x-startup', fixtureSerializer('startup'));
  });
  const registration = framework.createFrameworkRegistration(registrationOptions);
  capturedCodecs.addSerializer('application/x-late', fixtureSerializer('late'));
  assert.deepEqual([...registration.messageSerializers.keys()], ['application/x-startup']);
});

test('codec selection consumes declared-type, priority, fallback, and cache fixture rules', () => {
  class BaseMessage {}
  class DerivedMessage extends BaseMessage {}
  class OtherMessage {}
  class UnregisteredMessage {}
  const messageTypes = { BaseMessage, DerivedMessage, OtherMessage, UnregisteredMessage };
  const extensions = new Map(codecSelectionFixture.extensions.map((extension) => [
    extension.name,
    {
      ...extension,
      serializer: fixtureSerializer(extension.name)
    }
  ]));

  for (const scenario of codecSelectionFixture.sendScenarios) {
    const builder = new framework.DefaultZLinkCodecRegistryBuilder();
    for (const extensionName of scenario.registrationOrder) {
      const extension = extensions.get(extensionName);
      builder.addSerializer(
        extension.contentType,
        extension.serializer,
        (declaredType) => extension.matchesDeclaredTypes.includes(declaredType.name)
      );
    }
    const declaredType = messageTypes[scenario.declaredType];
    const runtimeType = messageTypes[scenario.runtimeType];
    const encoded = payloadCodec.encodeFrameworkPayload(
      framework.ZLinkMessage.from(new runtimeType(), declaredType),
      builder.registeredSerializers
    );
    try {
      assert.equal(encoded.contentType, scenario.expectedContentType);
    } finally {
      encoded.message.close();
    }
  }

  const cache = codecSelectionFixture.cacheScenarios[0];
  const selectorCalls = new Map();
  const cacheBuilder = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer('application/x-cache', fixtureSerializer('cache'), (declaredType) => {
      selectorCalls.set(declaredType, (selectorCalls.get(declaredType) ?? 0) + 1);
      return false;
    });
  const types = Array.from(
    { length: cache.distinctDeclaredTypes },
    (_, index) => ({ [`CacheType${index}`]: class {} })[`CacheType${index}`]
  );
  for (const type of types) {
    assert.equal(payloadCodec.selectSerializer(undefined, cacheBuilder.registeredSerializers, type), undefined);
  }
  assert.equal(payloadCodec.selectSerializer(
    undefined,
    cacheBuilder.registeredSerializers,
    types[0]
  ), undefined);
  assert.equal(payloadCodec.selectSerializer(
    undefined,
    cacheBuilder.registeredSerializers,
    types.at(-1)
  ), undefined);
  assert.equal(selectorCalls.get(types[0]), 1);
  assert.equal(selectorCalls.get(types.at(-1)), 2);
});

test('codec selection keeps the content type attached to the matching registration', () => {
  class FirstMessage {}
  class SecondMessage {}
  const sharedSerializer = fixtureSerializer('shared');
  const builder = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer(
      'application/x-first',
      sharedSerializer,
      (declaredType) => declaredType === FirstMessage
    )
    .addSerializer(
      'application/x-second',
      sharedSerializer,
      (declaredType) => declaredType === SecondMessage
    );

  const encoded = payloadCodec.encodeFrameworkPayload(
    framework.ZLinkMessage.from(new FirstMessage(), FirstMessage),
    builder.registeredSerializers
  );
  try {
    assert.equal(encoded.contentType, 'application/x-first');
  } finally {
    encoded.message.close();
  }
});

test('channel receive accepts only canonical registered content types from the shared fixture', () => {
  const baseSerializer = fixtureSerializer('baseCodec');
  const codecs = {
    serializers: new Map([['application/x-base', baseSerializer]])
  };
  for (const scenario of codecSelectionFixture.receiveScenarios) {
    const incoming = {
      header: {
        formatMarker: envelope.ZLINK_CHANNEL_FORMAT_MARKER,
        kind: 1,
        channelName: 'codec',
        messageName: scenario.name,
        contentType: scenario.wireContentType,
        correlationId: null,
        deadline: null,
        topic: null,
        metadata: {}
      },
      payload: Buffer.from([1])
    };
    if (scenario.expectedTerminal === 'success') {
      assert.equal(envelope.decodeChannelPayload(incoming, codecs), scenario.expectedCodec);
    } else {
      assert.throws(
        () => envelope.decodeChannelPayload(incoming, codecs),
        (error) => error instanceof framework.ZLinkFrameworkException
          && error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
      );
    }
  }
});

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

test('typed decode reattaches a DTO prototype without invoking its constructor', () => {
  class RequiredDto {
    constructor(required) {
      if (required === undefined) throw new Error('constructor must not run during decode');
      this.required = required;
    }

    marker() {
      return 'dto';
    }
  }

  const encoded = framework.ZLinkEncodedPayload.from(Buffer.from('{"required":7,"__proto__":{"polluted":true}}'));
  const message = framework.ZLinkMessage.fromEncoded(encoded);
  const decoded = message.decode(RequiredDto);

  assert.ok(decoded instanceof RequiredDto);
  assert.equal(decoded.required, 7);
  assert.equal(decoded.marker(), 'dto');
  assert.equal(Object.prototype.hasOwnProperty.call(decoded, '__proto__'), true);
  assert.equal(({}).polluted, undefined);
});

test('typed framework payload decode returns the requested DTO type after schema validation', () => {
  class ContractDto {
    constructor(required) {
      if (required === undefined) throw new Error('constructor must not run during decode');
      this.required = required;
    }
  }
  framework.ZLinkPacket('ContractDto', {
    payload: {
      type: 'object',
      required: ['required'],
      properties: { required: { type: 'int32' } }
    }
  })(ContractDto);

  const message = Message.from(Buffer.from('{"required":7}'));
  try {
    const decoded = payloadCodec.decodeFrameworkTypedPayloadMessage(
      message,
      undefined,
      ContractDto,
      'application/json',
      'ContractDto'
    );
    assert.ok(decoded instanceof ContractDto);
    assert.equal(decoded.required, 7);
  } finally {
    message.close();
  }
});

test('typed framework-json-v1 schema consumes every shared golden vector', () => {
  const fixture = JSON.parse(fs.readFileSync(path.resolve(
    __dirname,
    '../../../../runtime/protocol/golden/framework-json-v1.json'
  ), 'utf8'));
  const schema = {
    type: 'object',
    required: fixture.contract.requiredProperties,
    properties: {
      signed64: { type: 'int64' },
      unsigned64: { type: 'uint64' },
      int32: { type: 'int32' },
      ratio: { type: 'number' },
      state: { type: 'enum', names: fixture.contract.enumNames },
      bytes: { type: 'bytes' },
      nullable: { type: 'nullable', value: { type: 'string' } },
      labels: { type: 'record', values: { type: 'int32' } }
    }
  };

  class GoldenPayload {}
  framework.ZLinkPacket('GoldenPayload', { payload: schema })(GoldenPayload);

  for (const vector of fixture.valid) {
    const message = Message.from(Buffer.from(vector.jsonUtf8));
    try {
      const decoded = payloadCodec.decodeFrameworkTypedPayloadMessage(
        message,
        undefined,
        GoldenPayload
      );
      assert.equal(typeof decoded.signed64, 'bigint', vector.name);
      assert.equal(typeof decoded.unsigned64, 'bigint', vector.name);
      assert.deepEqual([...decoded.bytes], [1, 2], vector.name);
      assert.equal(Object.hasOwn(decoded, 'unknownFuture'), false, vector.name);
    } finally {
      message.close();
    }
  }

  for (const vector of fixture.invalid) {
    const message = Message.from(Buffer.from(vector.jsonUtf8));
    try {
      assert.throws(
        () => payloadCodec.decodeFrameworkTypedPayloadMessage(message, undefined, GoldenPayload),
        /PayloadDecodeFailed/,
        vector.name
      );
    } finally {
      message.close();
    }
  }
});

test('Channel request and reply apply the packet JSON contract without a codec registration', () => {
  const contract = {
    payload: {
      type: 'object',
      required: ['count'],
      properties: { count: { type: 'int32' } }
    },
    reply: {
      type: 'object',
      required: ['generation', 'bytes'],
      properties: {
        generation: { type: 'uint64' },
        bytes: { type: 'bytes' }
      }
    }
  };
  class ContractRequest {
    constructor(count) { this.count = count; }
  }
  framework.ZLinkPacket('ContractRequest', contract)(ContractRequest);

  const requestParts = envelope.encodeChannelEnvelopeParts(
    1,
    'contract',
    undefined,
    new ContractRequest(7)
  );
  try {
    assert.deepEqual(
      envelope.decodeChannelPayload(envelope.decodeChannelEnvelope(readable(requestParts))),
      { count: 7 }
    );
  } finally {
    envelope.closeMessages(requestParts);
  }

  const headerParts = envelope.encodeChannelEnvelopeParts(
      1,
      'contract',
      undefined,
      new ContractRequest(7)
    );
  const requestHeader = envelope.decodeChannelHeader(readable(headerParts));
  envelope.closeMessages(headerParts);
  const replyParts = envelope.encodeChannelReplyParts(requestHeader, {
    generation: 9n,
    bytes: Uint8Array.from([1, 2])
  });
  try {
    const reply = envelope.decodeChannelReply(readable(replyParts));
    assert.equal(reply.generation, 9n);
    assert.deepEqual([...reply.bytes], [1, 2]);
  } finally {
    envelope.closeMessages(replyParts);
  }

  const invalidParts = [
    Buffer.from(JSON.stringify({
      formatMarker: 0xf2,
      kind: 1,
      channelName: 'contract',
      messageName: 'ContractRequest',
      contentType: 'application/json',
      correlationId: 'c',
      deadline: null,
      topic: null,
      metadata: {}
    })),
    Buffer.from('{}')
  ];
  assert.throws(
    () => envelope.decodeChannelPayload(envelope.decodeChannelEnvelope(readable(invalidParts))),
    /PayloadDecodeFailed/
  );
});

test('packet JSON contracts are validated and frozen when the packet class is defined', () => {
  class InvalidPacket {}
  assert.throws(
    () => framework.ZLinkPacket('InvalidPacket', {
      payload: {
        type: 'object',
        required: ['missing'],
        properties: {}
      }
    })(InvalidPacket),
    /required property 'missing' has no property schema/
  );
  class InvalidAdditionalProperties {}
  assert.throws(
    () => framework.ZLinkPacket('InvalidAdditionalProperties', {
      payload: {
        type: 'object',
        required: [],
        properties: {},
        additionalProperties: 'no'
      }
    })(InvalidAdditionalProperties),
    /additionalProperties must be boolean/
  );

  const mutableNames = ['Open'];
  class FrozenPacket {}
  framework.ZLinkPacket('FrozenPacket', {
    payload: { type: 'enum', names: mutableNames }
  })(FrozenPacket);
  mutableNames.push('Closed');
  const message = Message.from(Buffer.from('"Closed"'));
  try {
    assert.throws(
      () => payloadCodec.decodeFrameworkTypedPayloadMessage(message, undefined, FrozenPacket),
      /PayloadDecodeFailed/
    );
  } finally {
    message.close();
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
    serialize() {
      throw new Error('serializer must not encode framework-owned payloads');
    },
    deserialize() {
      return { decoded: true };
    }
  };
  const registry = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer('application/x-test', serializer, (declaredType) => {
      selectionCalls += 1;
      return declaredType === ApplicationPayload;
    })
    .registeredSerializers;

  assert.equal(payloadCodec.selectSerializer(new FrameworkPayload(), registry), undefined);
  assert.equal(payloadCodec.selectSerializer(new FrameworkPayload(), registry), undefined);
  assert.equal(payloadCodec.selectSerializer(new ApplicationPayload(), registry), serializer);
  assert.equal(payloadCodec.selectSerializer(new ApplicationPayload(), registry), serializer);
  assert.equal(selectionCalls, 2);
  assert.equal(payloadCodec.selectDefaultSerializer(registry), undefined);
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
  const registry = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer('application/x-test', serializer)
    .registeredSerializers;
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

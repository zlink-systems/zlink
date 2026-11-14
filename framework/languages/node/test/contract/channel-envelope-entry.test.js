'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const {
  ZLinkEncodedPayload
} = require('../../packages/framework/dist/contracts/Common/ZLinkEncodedPayload');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  DefaultZLinkCodecRegistryBuilder,
  ZLinkMessage
} = require('../../packages/framework/dist/internal');

const correlation = '00112233445566778899aabbccddeeff';
const deadline = 2000000000000;
const payload = { value: '한글\n"\\\ud800' };
const customType = 'application/x-entry-test';
const body = Buffer.from(JSON.stringify(payload));
const serializer = {
  serialize: () => ZLinkEncodedPayload.fromOwned(body),
  deserialize: (encoded) => JSON.parse(encoded.getString())
};
const codecs = { serializers: new Map([[customType, serializer]]) };

function bytes(part) {
  if (typeof part === 'string') return Buffer.from(part);
  if (typeof part.data === 'function') return part.data();
  return Buffer.from(part.buffer, part.byteOffset, part.byteLength);
}

function messages(parts) {
  return parts.map((part) => ZLinkBufferMessage.fromOwned(bytes(part)));
}

// Reference the previous canonical wire and standalone allocation policy.
// This encoder does not call the production header writer or its allocator.
function legacyParts(kind, contentType, data, topic = null, metadata = {}, timestamp = deadline) {
  const header = {
    formatMarker: 242,
    kind,
    channelName: 'entry',
    messageName: 'Packet',
    contentType,
    correlationId: kind === 3 ? null : correlation,
    deadline: kind === 1 ? new Date(timestamp).toISOString() : null,
    topic
  };
  if (kind !== 2) {
    header.errorCode = null;
    header.errorMessage = null;
  }
  header.metadata = metadata;
  const text = JSON.stringify(header);
  const buffer = Buffer.alloc(Buffer.byteLength(text));
  buffer.write(text);
  return [buffer, data];
}

for (const kind of [3, 1, 2]) {
  for (const prepared of [false, true]) {
    test(`entry ${kind} ${prepared ? 'prepared' : 'generic'} matches legacy header and payload bytes`, () => {
      const registry = prepared ? codecs : undefined;
      const contentType = prepared ? customType : 'application/json';
      let parts;
      if (kind === 2) {
        const request = envelope.decodeChannelHeader(
          messages(legacyParts(1, contentType, body)),
          false
        );
        parts = envelope.encodeChannelReplyParts(request, payload, registry);
      } else {
        parts = envelope.encodeChannelEnvelopePartsAtDeadline(
          kind,
          'entry',
          'Packet',
          payload,
          kind === 1 ? deadline : undefined,
          undefined,
          registry,
          kind === 1 ? correlation : undefined,
          false
        );
      }
      const legacy = legacyParts(kind, contentType, body);
      assert.deepEqual(parts.map(bytes), legacy.map(bytes));
      const received = messages(parts);
      try {
        const decoded =
          kind === 2
            ? envelope.decodeChannelReply(received, registry)
            : envelope.decodeChannelPayload(
                envelope.decodeChannelEnvelope(received, undefined, false),
                registry
              );
        assert.deepEqual(decoded, payload);
      } finally {
        envelope.closeMessages(received);
      }
    });
  }
}

test('generic entry preserves escaped topic and application metadata bytes', () => {
  const metadata = new Map([['tenant', '서울\n"\\']]);
  const topic = '주제\n"\\\ud800';
  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
    1,
    'entry',
    'Packet',
    payload,
    deadline,
    topic,
    codecs,
    correlation,
    false,
    metadata
  );
  assert.deepEqual(
    parts.map(bytes),
    legacyParts(1, customType, body, topic, Object.fromEntries(metadata)).map(bytes)
  );
  const received = messages(parts);
  try {
    const decoded = envelope.decodeChannelEnvelope(received, undefined, false);
    assert.equal(decoded.header.topic, topic);
    assert.deepEqual(decoded.header.metadata, Object.fromEntries(metadata));
    assert.deepEqual(envelope.decodeChannelPayload(decoded, codecs), payload);
  } finally {
    envelope.closeMessages(received);
  }
});

test('raw entry keeps Buffer, Uint8Array and Message identity and bypasses serializer encoding', () => {
  const registry = {
    serializers: new Map([
      [
        customType,
        {
          serialize: () => assert.fail('raw payload must not be serialized')
        }
      ]
    ])
  };
  for (const value of [body, new Uint8Array(body), ZLinkBufferMessage.fromOwned(body)]) {
    const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
      3,
      'entry',
      'Packet',
      value,
      undefined,
      undefined,
      registry,
      undefined,
      false
    );
    assert.equal(parts[1], value);
    assert.deepEqual(parts.map(bytes), legacyParts(3, 'application/octet-stream', body).map(bytes));
    const received = messages(parts);
    try {
      assert.deepEqual(
        envelope.decodeChannelPayload(envelope.decodeChannelEnvelope(received, undefined, false)),
        body
      );
    } finally {
      envelope.closeMessages(received);
    }
  }
});

function assertBackingBudget(headers) {
  assert.ok(
    new Set(headers.map((header) => header.buffer)).size <= 8,
    '1000 headers must use at most eight backing stores'
  );
}

test('request headers share allocation storage without reusing live byte regions', () => {
  const retained = [];
  for (let index = 0; index < 1000; index++) {
    const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
      1,
      'entry',
      'Packet',
      payload,
      deadline + index,
      undefined,
      codecs,
      correlation,
      false
    );
    retained.push(parts[0]);
  }
  assertBackingBudget(retained);
  // Standalone legacy frames must violate the same allocation assertion.
  const legacy = Array.from({ length: 1000 }, () => legacyParts(1, customType, body)[0]);
  assert.throws(() => assertBackingBudget(legacy), assert.AssertionError);
  for (let index = 0; index < retained.length; index++) {
    const header = envelope.decodeChannelHeader(
      [ZLinkBufferMessage.fromOwned(retained[index])],
      false
    );
    assert.equal(header.deadline, new Date(deadline + index).toISOString());
  }
});

test('generic headers and large headers write every byte of their final frame', () => {
  for (const topic of ['x'.repeat(1024), 'x'.repeat(17000)]) {
    const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
      1,
      'entry',
      'Packet',
      payload,
      deadline,
      topic,
      undefined,
      correlation,
      false
    );
    assert.deepEqual(parts.map(bytes), legacyParts(1, 'application/json', body, topic).map(bytes));
  }
});

test('entry preserves codec selection failures and deadline validation', () => {
  const failure = new Error('selection failed');
  const serializers = new DefaultZLinkCodecRegistryBuilder().addSerializer(
    customType,
    serializer,
    () => {
      throw failure;
    }
  ).registeredSerializers;
  assert.throws(
    () =>
      envelope.encodeChannelEnvelopePartsAtDeadline(
        1,
        'entry',
        'Packet',
        payload,
        deadline,
        undefined,
        { serializers },
        correlation,
        false
      ),
    (error) => error === failure
  );
  for (const invalid of [0, -1, NaN, Infinity, 1.5]) {
    assert.throws(
      () =>
        envelope.encodeChannelEnvelopePartsAtDeadline(
          1,
          'entry',
          'Packet',
          payload,
          invalid,
          undefined,
          codecs,
          correlation,
          false
        ),
      { name: 'RangeError', message: 'deadlineUnixMs must be a positive safe integer.' }
    );
  }
});

test('request deadline bytes match ISO across calendar and extended-year boundaries', () => {
  const timestamps = [
    1,
    9,
    10,
    99,
    100,
    999,
    1000,
    ...[
      '1999-12-31T23:59:59.999Z',
      '2000-01-01T00:00:00.000Z',
      '2000-02-28T23:59:59.999Z',
      '2000-02-29T00:00:00.000Z',
      '2000-03-01T00:00:00.001Z',
      '2100-02-28T23:59:59.999Z',
      '2100-03-01T00:00:00.000Z',
      '2400-02-29T12:34:56.789Z',
      '9999-12-31T23:59:59.999Z',
      '+010000-01-01T00:00:00.000Z'
    ].map(Date.parse),
    8640000000000000
  ];
  for (const timestamp of timestamps) {
    for (const registry of [undefined, codecs]) {
      const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
        1,
        'entry',
        'Packet',
        payload,
        timestamp,
        undefined,
        registry,
        correlation,
        false
      );
      assert.deepEqual(
        parts.map(bytes),
        legacyParts(1, registry ? customType : 'application/json', body, null, {}, timestamp).map(
          bytes
        )
      );
    }
  }
  for (const timestamp of [8640000000000001, Number.MAX_SAFE_INTEGER]) {
    for (const registry of [undefined, codecs]) {
      assert.throws(
        () =>
          envelope.encodeChannelEnvelopePartsAtDeadline(
            1,
            'entry',
            'Packet',
            payload,
            timestamp,
            undefined,
            registry,
            correlation,
            false
          ),
        { name: 'RangeError', message: 'Invalid time value' }
      );
    }
  }
});

test('request deadline bytes preserve flow and application metadata on both header paths', () => {
  const flow = { flowId: '01900000-1234-7000-8000-000000000001', flowOrigin: 'Application' };
  const timestamp = Date.parse('2400-02-29T12:34:56.789Z');
  for (const registry of [undefined, codecs]) {
    for (const metadata of [new Map(), new Map([['tenant', '서울']])]) {
      const parts = runWithFlow(flow, () =>
        envelope.encodeChannelEnvelopePartsAtDeadline(
          1,
          'entry',
          'Packet',
          payload,
          timestamp,
          undefined,
          registry,
          correlation,
          true,
          metadata
        )
      );
      const expected = legacyParts(
        1,
        registry ? customType : 'application/json',
        body,
        null,
        Object.fromEntries(metadata),
        timestamp
      );
      const header = JSON.parse(expected[0]);
      header.flowId = flow.flowId;
      header.flowOrigin = 3;
      expected[0] = Buffer.from(JSON.stringify(header));
      assert.deepEqual(parts.map(bytes), expected.map(bytes));
      const corrupted = Buffer.from(parts[0]);
      const milliseconds = corrupted.indexOf('.789Z');
      assert.ok(milliseconds >= 0);
      corrupted[milliseconds + 1] = 0x30;
      assert.throws(() => assert.deepEqual(corrupted, expected[0]), assert.AssertionError);
    }
  }
});

test('prepared deadlines write digits without formatting an ISO string per request', (t) => {
  const encode = (timestamp, metadata) =>
    envelope.encodeChannelEnvelopePartsAtDeadline(
      1,
      'entry',
      'Packet',
      payload,
      timestamp,
      undefined,
      codecs,
      correlation,
      false,
      metadata
    );
  // Initialize the existing prepared template before observing per-request work.
  encode(deadline);
  const original = Date.prototype.toISOString;
  let calls = 0;
  t.mock.method(Date.prototype, 'toISOString', function () {
    calls += 1;
    return original.call(this);
  });
  for (let index = 0; index < 200; index++) {
    const header = JSON.parse(encode(deadline + index)[0]);
    assert.equal(header.deadline, original.call(new Date(deadline + index)));
  }
  assert.equal(calls, 0);
  encode(deadline, new Map([['tenant', 'entry']]));
  assert.equal(calls, 1);
  encode(Date.parse('+010000-01-01T00:00:00.000Z'));
  assert.equal(calls, 2);
});

test('raw and default JSON entry classify the payload once', (t) => {
  const value = { value: 'default JSON' };
  const original = Buffer.isBuffer;
  const calls = [];
  t.mock.method(Buffer, 'isBuffer', (candidate) => {
    calls.push(candidate);
    return original(candidate);
  });
  for (const data of [body, value]) {
    calls.length = 0;
    envelope.encodeChannelEnvelopePartsAtDeadline(
      3,
      'entry',
      'Packet',
      data,
      undefined,
      undefined,
      undefined,
      undefined,
      false
    );
    assert.equal(calls.filter((candidate) => candidate === data).length, 1);
  }
});

test('typed and explicitly encoded message wrappers preserve their wire and payload storage', () => {
  const typed = envelope.encodeChannelEnvelopePartsAtDeadline(
    3,
    'entry',
    'Packet',
    ZLinkMessage.from(payload, Object),
    undefined,
    undefined,
    codecs,
    undefined,
    false
  );
  assert.deepEqual(typed.map(bytes), legacyParts(3, customType, body).map(bytes));
  const encoded = envelope.encodeChannelEnvelopePartsAtDeadline(
    3,
    'entry',
    'Packet',
    ZLinkMessage.fromEncoded(ZLinkEncodedPayload.fromOwned(body)),
    undefined,
    undefined,
    codecs,
    undefined,
    false
  );
  assert.equal(encoded[1], body);
  assert.deepEqual(encoded.map(bytes), legacyParts(3, 'application/octet-stream', body).map(bytes));
});

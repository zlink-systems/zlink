const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const json = require('../../packages/framework/dist/runtime/messaging/framework-json-v1');
const portableJson = require('../../packages/stream-wire/dist/internal/framework-json-v1');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');

const bytesSchema = {
  type: 'object',
  properties: { body: { type: 'bytes' } },
  required: ['body']
};

function messages(parts) {
  return parts.map((part) =>
    ZLinkBufferMessage.fromOwned(typeof part === 'string' ? Buffer.from(part, 'utf8') : part)
  );
}

function assertDecoded(value, expected, typed) {
  if (typed) {
    assert.ok(value.body instanceof Uint8Array);
    assert.deepEqual(Buffer.from(value.body), expected);
  } else {
    assert.equal(value.body, expected.toString('base64'));
    assert.deepEqual(Buffer.from(value.body, 'base64'), expected);
  }
}

for (const byteType of ['Buffer', 'Uint8Array']) {
  for (const size of [0, 1, 2, 3, 127, 128, 1024, 4096]) {
    test(`default channel JSON preserves ${byteType} fields at ${size} bytes`, () => {
      const storage = Buffer.alloc(size + 19, 0xa5);
      const view = storage.subarray(7, size + 7);
      for (let index = 0; index < size; index++) {
        view[index] = (31 * index + 17 * Math.floor(index / 251) + 29) % 256;
      }
      const original = Buffer.from(storage);
      const body =
        byteType === 'Buffer' ? view : new Uint8Array(storage.buffer, view.byteOffset, view.length);
      const payload = Object.freeze({ body });
      const expectedJson = `{"body":"${view.toString('base64')}"}`;

      for (const typed of [false, true]) {
        const packetName = `native-json.${byteType}.${size}.${typed}`;
        if (typed) {
          framework.ZLinkPacket(packetName, { payload: bytesSchema, reply: bytesSchema })(
            class BytesPayload {}
          );
        }
        assert.equal(
          portableJson.stringifyFrameworkJsonV1(payload, typed ? bytesSchema : undefined),
          expectedJson
        );
        const encoded = envelope.encodeChannelEnvelopeParts(
          1,
          'bench',
          packetName,
          payload,
          undefined,
          undefined,
          undefined,
          '0123456789abcdef0123456789abcdef',
          false
        );
        assert.equal(typeof encoded[1], 'string');
        assert.equal(encoded[1], expectedJson);
        const request = messages(encoded);
        let reply;
        try {
          const header = envelope.decodeChannelHeader(request, false);
          assert.equal(header.contentType, 'application/json');
          assertDecoded(
            envelope.decodeChannelPayload(envelope.decodeChannelEnvelope(request, header, false)),
            view,
            typed
          );
          const encodedReply = envelope.encodeChannelReplyParts(header, payload);
          assert.equal(encodedReply[1], expectedJson);
          reply = messages(encodedReply);
          assertDecoded(envelope.decodeChannelReply(reply), view, typed);
        } finally {
          for (const message of [...request, ...(reply ?? [])]) message.close();
        }
      }
      assert.deepEqual(storage, original);
    });
  }
}

test('Node default channel JSON encodes bytes without a portable binary string', (t) => {
  t.mock.method(globalThis, 'btoa', () => {
    throw new Error('portable Base64 conversion reached the Node typed JSON path');
  });
  for (const body of [Buffer.from([1, 2, 3]), Uint8Array.from([1, 2, 3])]) {
    const encoded = envelope.encodeChannelEnvelopeParts(
      1,
      'bench',
      'native-json.no-binary-string',
      { body },
      undefined,
      undefined,
      undefined,
      undefined,
      false
    );
    assert.equal(encoded[1], '{"body":"AQID"}');
    const request = messages(encoded);
    try {
      const header = envelope.decodeChannelHeader(request, false);
      assert.equal(envelope.encodeChannelReplyParts(header, { body })[1], '{"body":"AQID"}');
    } finally {
      for (const message of request) message.close();
    }
  }
});

test('Node byte conversion retains the shared JSON policy and replacer semantics', () => {
  for (const value of [
    { value: NaN },
    { value: Infinity },
    { value: -9223372036854775809n },
    { value: 18446744073709551616n },
    { value: new Date(0) },
    { value: { toJSON: () => 'hidden' } }
  ]) {
    assert.throws(() => json.stringifyFrameworkJsonV1(value));
    assert.throws(() => portableJson.stringifyFrameworkJsonV1(value));
  }
  const value = {
    min: -9223372036854775808n,
    max: 18446744073709551615n,
    nested: [null, { bytes: Uint8Array.from([0, 255]) }],
    text: 'é\n"'
  };
  assert.equal(json.stringifyFrameworkJsonV1(value), portableJson.stringifyFrameworkJsonV1(value));
  function withGetter() {
    let reads = 0;
    return Object.defineProperty({}, 'value', {
      enumerable: true,
      get: () => (++reads === 1 ? 'first' : 'second')
    });
  }
  assert.equal(json.stringifyFrameworkJsonV1(withGetter()), '{"value":"first"}');
  assert.equal(
    json.stringifyFrameworkJsonV1(withGetter()),
    portableJson.stringifyFrameworkJsonV1(withGetter())
  );
  const replacer = function (key, item) {
    return key === 'replace' ? Uint8Array.from([1, 2]) : item;
  };
  assert.equal(
    json.stringifyFrameworkJsonV1({ replace: 1 }, undefined, replacer),
    portableJson.stringifyFrameworkJsonV1({ replace: 1 }, undefined, replacer)
  );
});

test('bytes schema continues to reject missing padding and noncanonical padding bits', () => {
  for (const body of ['AQI', 'AQJ=', 'AR==']) {
    assert.throws(
      () => json.parseFrameworkJsonV1(JSON.stringify({ body }), {}, bytesSchema),
      /padded RFC 4648 base64/
    );
  }
});

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

for (const size of [0, 1, 2, 3, 127, 128, 1024, 4096]) {
  test(`nested byte objects and arrays preserve exact JSON at ${size} bytes`, () => {
    const storage = Buffer.alloc(size + 17, 0xc3);
    for (let index = 0; index < size; index++) storage[index + 5] = (index * 31 + 73) % 256;
    const buffer = storage.subarray(5, size + 5);
    const view = new Uint8Array(storage.buffer, buffer.byteOffset, size);
    const value = Object.freeze({
      nested: Object.freeze({ body: buffer }),
      items: Object.freeze([buffer, Uint8Array.from(buffer), view])
    });
    const base64 = buffer.toString('base64');
    const expected = JSON.stringify({ nested: { body: base64 }, items: [base64, base64, base64] });
    const schema = {
      type: 'object',
      required: ['nested', 'items'],
      properties: {
        nested: bytesSchema,
        items: { type: 'array', items: { type: 'bytes' } }
      }
    };
    const before = Buffer.from(storage);
    for (const contract of [undefined, schema]) {
      assert.equal(json.stringifyFrameworkJsonV1(value, contract), expected);
      assert.equal(portableJson.stringifyFrameworkJsonV1(value, contract), expected);
    }
    const decoded = json.parseFrameworkJsonV1(expected, {}, schema);
    for (const bytes of [decoded.nested.body, ...decoded.items]) {
      assert.equal(Object.getPrototypeOf(bytes), Uint8Array.prototype);
      assert.equal(bytes.byteLength, size);
      assert.deepEqual(Buffer.from(bytes), buffer);
    }
    if (size > 0) {
      decoded.items[0][0] ^= 255;
      assert.deepEqual(Buffer.from(decoded.items[1]), buffer);
    }
    assert.deepEqual(storage, before);
  });
}

test('Node bytes schema uses a native decoder after canonical validation', (t) => {
  t.mock.method(globalThis, 'atob', () => {
    throw new Error('portable decoder used');
  });
  const decoded = json.parseFrameworkJsonV1('{"body":"AP8="}', {}, bytesSchema);
  assert.deepEqual(decoded.body, Uint8Array.from([0, 255]));
  const nestedSchema = { type: 'record', values: { type: 'nullable', value: { type: 'bytes' } } };
  assert.deepEqual(json.parseFrameworkJsonV1('{"a":"AP8=","b":null}', {}, nestedSchema), {
    a: Uint8Array.from([0, 255]),
    b: null
  });
});

test('native bytes decode owns exactly its storage, never a shared Buffer pool slice', () => {
  for (const size of [0, 1, 2, 3, 127, 128, 1024, 4096]) {
    const source = Buffer.alloc(size, 0xa5);
    const text = JSON.stringify({ body: source.toString('base64') });
    const decoded = json.parseFrameworkJsonV1(text, {}, bytesSchema).body;
    assert.equal(decoded.byteOffset, 0, `size ${size} offset`);
    assert.equal(decoded.buffer.byteLength, size, `size ${size} backing storage`);
    assert.deepEqual(Buffer.from(decoded), source);
  }
});

test('native decode and portable decode accept exactly canonical padded base64', () => {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const candidates = [
    ['', true],
    ['AQID', true],
    ['AQI=', true],
    ['AQ==', true],
    ...[
      'A',
      'AA',
      'AAA',
      'AQI',
      'AQ',
      'AQ===',
      '=AQI',
      'AA=A',
      '====',
      ' AQI=',
      'AQI= ',
      'AQ I=',
      'AQI=\n',
      'AQI=\r\n',
      'AQI=\t',
      '-w==',
      '_w==',
      'AA-_',
      'é==='
    ].map((text) => [text, false]),
    ...[...alphabet].map((symbol, index) => [`A${symbol}==`, (index & 15) === 0]),
    ...[...alphabet].map((symbol, index) => [`AA${symbol}=`, (index & 3) === 0])
  ];
  for (const [body, accepted] of candidates) {
    const text = JSON.stringify({ body });
    if (accepted) {
      const native = json.parseFrameworkJsonV1(text, {}, bytesSchema);
      const portable = portableJson.parseFrameworkJsonV1(text, {}, bytesSchema);
      assert.deepEqual(native, portable, body);
      assert.equal(Buffer.from(native.body).toString('base64'), body);
    } else {
      for (const codec of [json, portableJson]) {
        assert.throws(
          () => codec.parseFrameworkJsonV1(text, {}, bytesSchema),
          /padded RFC 4648 base64/,
          body
        );
      }
    }
  }
});

test('byte projection preserves getter, toJSON, cycle and custom replacer effects', () => {
  function compare(make, replacer) {
    const observations = [];
    for (const codec of [portableJson, json]) {
      const events = [];
      const value = make(events);
      try {
        observations.push({
          result: codec.stringifyFrameworkJsonV1(value, undefined, replacer),
          events
        });
      } catch (error) {
        observations.push({ error: error.constructor.name, events });
      }
    }
    assert.deepEqual(observations[1], observations[0]);
  }
  compare((events) => ({
    body: Buffer.from([1, 2]),
    get value() {
      events.push('get');
      return events.length === 1 ? 'first' : 'second';
    }
  }));
  compare((events) => {
    const value = { body: Buffer.from([1]), later: 'before' };
    Object.defineProperty(value, 'body', {
      enumerable: true,
      get() {
        events.push('body');
        value.later = 'after';
        return Buffer.from([events.length]);
      }
    });
    return value;
  });
  compare((events) => ({
    body: Buffer.from([1]),
    value: {
      toJSON() {
        events.push('toJSON');
        return 1;
      }
    }
  }));
  compare((events) => {
    const value = { body: Buffer.from([1]), later: 'before' };
    value.fn = Object.assign(() => {}, {
      toJSON() {
        events.push('function toJSON');
        value.later = 'after';
        return 1;
      }
    });
    return value;
  });
  compare(() => {
    const value = { body: Buffer.from([1]) };
    value.self = value;
    return value;
  });
  compare(
    (events) =>
      new Proxy(
        { body: Buffer.from([1]) },
        {
          get(target, key, receiver) {
            events.push(`get:${String(key)}`);
            return Reflect.get(target, key, receiver);
          },
          ownKeys(target) {
            events.push('ownKeys');
            return Reflect.ownKeys(target);
          },
          getOwnPropertyDescriptor(target, key) {
            events.push(`descriptor:${String(key)}`);
            return Reflect.getOwnPropertyDescriptor(target, key);
          }
        }
      )
  );
  compare((events) => {
    const body = Buffer.from([1, 2]);
    body.toJSON = () => {
      events.push('byte toJSON');
      return 'ignored';
    };
    return { body };
  });
  compare(
    () => ({ body: Buffer.from([1, 2]) }),
    function (key, item) {
      if (key === 'body') assert.equal(item.type, 'Buffer');
      return item;
    }
  );
  compare(
    () =>
      new (class Payload {
        constructor() {
          this.body = Buffer.from([1, 2]);
        }
      })()
  );
  compare(() => ({ body: Buffer.from([1]), boxed: new Number(3) }));
  compare(() => ({ body: Buffer.from([1]), sparse: [1, , 3] }));
  compare(() =>
    Object.defineProperty({ body: Buffer.from([1]) }, '__proto__', {
      value: { body: Uint8Array.from([2]) },
      enumerable: true
    })
  );
});

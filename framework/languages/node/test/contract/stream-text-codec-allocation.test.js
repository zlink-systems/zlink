const assert = require('node:assert/strict');
const test = require('node:test');

test('STREAM text codecs reuse their processors across packets', () => {
  const NativeEncoder = globalThis.TextEncoder;
  const NativeDecoder = globalThis.TextDecoder;
  let encoders = 0;
  let decoders = 0;
  globalThis.TextEncoder = class extends NativeEncoder {
    constructor(...args) {
      super(...args);
      encoders++;
    }
  };
  globalThis.TextDecoder = class extends NativeDecoder {
    constructor(...args) {
      super(...args);
      decoders++;
    }
  };
  try {
    const wire = require('../../packages/stream-wire/dist');
    // Exclude module initialization; count processor construction on the packet path.
    encoders = 0;
    decoders = 0;
    for (let index = 0; index < 100; index++) {
      const header = {
        kind: wire.ZlinkStreamMessageKind.Request,
        codec: wire.ZlinkStreamCodec.Json,
        flags: wire.ZlinkStreamHeaderFlags.None,
        requestSeq: BigInt(index + 1),
        name: 'Echo한글😀',
        metadata: new Map([['key', '값']]),
        correlationId: 'correlation'
      };
      const decoded = wire.decodeStreamWireHeader(wire.encodeStreamWireHeader(header));
      assert.equal(decoded.name, header.name);
      assert.deepEqual(decoded.metadata, header.metadata);
      assert.equal(decoded.correlationId, header.correlationId);
      const bound = wire.encodeStreamWireActorBoundPayload(1, 'actor한글');
      assert.equal(wire.decodeStreamWireActorBoundPayload(bound).actorId, 'actor한글');
    }
    console.log(
      `100 packets: TextEncoder constructions=${encoders}, TextDecoder constructions=${decoders}`
    );
    assert.deepEqual({ encoders, decoders }, { encoders: 0, decoders: 0 });
  } finally {
    globalThis.TextEncoder = NativeEncoder;
    globalThis.TextDecoder = NativeDecoder;
  }
});

test('STREAM text decoding resets between inputs, including malformed UTF-8', () => {
  const wire = require('../../packages/stream-wire/dist');
  for (const bytes of [
    new Uint8Array([0xe2, 0x82]),
    new Uint8Array([0xac]),
    new Uint8Array([0xef, 0xbb, 0xbf, 0x61]),
    new Uint8Array(),
    new Uint8Array([0xff]),
    new Uint8Array([0x62])
  ]) {
    assert.equal(wire.utf8Decode(bytes), new TextDecoder().decode(bytes));
  }
  for (const text of ['', '한글😀', '\ud800', 'next']) {
    assert.deepEqual(wire.utf8Encode(text), new TextEncoder().encode(text));
  }
  const invalidBound = wire.encodeStreamWireActorBoundPayload(1, 'a');
  invalidBound[invalidBound.length - 1] = 0xff;
  assert.throws(
    () => wire.decodeStreamWireActorBoundPayload(invalidBound),
    /Actor id is not valid UTF-8/
  );
  const validBound = wire.encodeStreamWireActorBoundPayload(1, '정상');
  assert.equal(wire.decodeStreamWireActorBoundPayload(validBound).actorId, '정상');
});

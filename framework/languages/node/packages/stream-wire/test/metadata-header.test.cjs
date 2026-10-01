const assert = require('node:assert/strict');
const test = require('node:test');
const wire = require('../dist');
const { namedReplyHeader } = require('../../../test/contract/helpers/named-reply-header');

test('wire metadata size includes the standalone count byte and matches the shared encoder', () => {
  assert.equal(wire.streamWireMetadataSize(new Map()), 1);
  assert.deepEqual([...wire.encodeStreamWireMetadata(new Map())], [0]);
  for (const text of [
    '',
    'ascii',
    '지역',
    '😀',
    '\ud800',
    '\udfff',
    '\ud800x\udfff',
    '\ud800\ud800\udc00'
  ]) {
    const metadata = new Map([[text || 'empty-value', text]]);
    assert.equal(
      wire.streamWireMetadataSize(metadata),
      wire.encodeStreamWireMetadata(metadata).length
    );
  }
});

test('wire size and encoding share metadata field validation', () => {
  for (const invalid of [
    new Map([['', 'x']]),
    new Map([['x'.repeat(256), '']]),
    new Map([['k', 'x'.repeat(65536)]]),
    new Map(Array.from({ length: 256 }, (_, i) => [`k${i}`, '']))
  ]) {
    let encodingError;
    try {
      wire.encodeStreamWireMetadata(invalid);
    } catch (error) {
      encodingError = error;
    }
    assert.ok(encodingError);
    assert.throws(() => wire.streamWireMetadataSize(invalid), { message: encodingError.message });
  }
  const boundary = new Map([['k'.repeat(255), 'x'.repeat(65535)]]);
  assert.equal(
    wire.streamWireMetadataSize(boundary),
    wire.encodeStreamWireMetadata(boundary).length
  );
});

test('F20 empty reply names decode while nonempty Response and Error names fail', () => {
  for (const kind of [1, 2]) {
    const header = wire.encodeStreamWireHeader({
      kind,
      codec: wire.ZlinkStreamCodec.Raw,
      flags: 0,
      requestSeq: kind === 2 ? 1n : undefined,
      name: 'Push',
      metadata: new Map()
    });
    assert.equal(wire.decodeStreamWireHeader(header).name, 'Push');
  }
  for (const kind of [3, 4]) {
    const header = wire.encodeStreamWireHeader({
      kind,
      codec: wire.ZlinkStreamCodec.Json,
      flags: 0,
      requestSeq: 1n,
      name: '',
      metadata: new Map()
    });
    assert.equal(wire.decodeStreamWireHeader(header).name, '');
    const named = namedReplyHeader(kind);
    assert.throws(() => wire.decodeStreamWireHeader(named), /Stream packet name is invalid/);
  }
});

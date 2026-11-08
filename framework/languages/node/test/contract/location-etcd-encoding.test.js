const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const codec = require('../../packages/framework-locations-etcd/dist/internal/encoding');
const golden = require(
  path.resolve(__dirname, '../../../../runtime/protocol/golden/location-store-etcd-v1.json')
);
const bytes = (hex) => (hex === null ? null : Buffer.from(hex, 'hex'));

test('etcd encoding consumes every golden key, metadata, clock and version vector', () => {
  for (const vector of golden.keyVectors) {
    const key = codec.keys(vector.namespace);
    assert.equal(
      key.metadata(bytes(vector.frameworkKeyHex)).toString('hex'),
      vector.metadataKeyHex,
      vector.name
    );
    assert.equal(
      key.payload(bytes(vector.frameworkKeyHex)).toString('hex'),
      vector.payloadKeyHex,
      vector.name
    );
    assert.equal(key.clock.toString('hex'), vector.clockKeyHex, vector.name);
  }
  for (const vector of golden.metadataVectors) {
    const metadata = {
      hasExpiry: vector.hasExpiry,
      expiresAtMs: BigInt(vector.expiresAtMs),
      payloadLength: vector.payloadLength
    };
    assert.equal(codec.encodeMetadata(metadata).toString('hex'), vector.metadataHex, vector.name);
    assert.deepEqual(codec.decodeMetadata(bytes(vector.metadataHex)), metadata, vector.name);
  }
  for (const vector of golden.clockVectors) {
    assert.equal(codec.encodeClock(BigInt(vector.value)).toString('hex'), vector.hex);
    assert.equal(codec.decodeClock(bytes(vector.hex)), BigInt(vector.value));
  }
  for (const vector of golden.storeVersions.accepted) {
    assert.equal(codec.encodeVersion(vector.clusterIdDecimal, vector.modRevision), vector.value);
    assert.deepEqual(codec.decodeVersion(vector.value), {
      clusterId: vector.clusterIdDecimal,
      revision: vector.modRevision
    });
  }
  for (const value of golden.storeVersions.rejected)
    assert.equal(codec.decodeVersion(value), null, value);
});

test('etcd encoding rejects every corrupt golden record as storeFailure', () => {
  for (const vector of golden.corruptRecords) {
    assert.equal(vector.expected, 'storeFailure');
    const decode =
      vector.clockHex !== undefined
        ? () => codec.decodeClock(bytes(vector.clockHex))
        : () =>
            codec.validateRecord(
              bytes(vector.metadataHex),
              bytes(vector.payloadHex),
              vector.metadataModRevision ?? '1',
              vector.payloadModRevision ?? '1'
            );
    assert.throws(decode, codec.StoreFormatError, vector.name);
  }
  assert.equal(codec.validateRecord(null, null, '0', '0'), null);
  assert.throws(() => codec.encodeClock(codec.MAX_TIME_MS + 1n), RangeError);
  assert.throws(() => codec.encodeClock(-1n), RangeError);
  assert.throws(
    () =>
      codec.encodeMetadata({
        hasExpiry: true,
        expiresAtMs: codec.MAX_TIME_MS + 1n,
        payloadLength: 0
      }),
    RangeError
  );
  assert.throws(() => codec.decodeMetadata(Buffer.alloc(13)), codec.StoreFormatError);
  assert.throws(
    () => codec.decodeMetadata(Buffer.from('0100000000000000000100000000', 'hex')),
    codec.StoreFormatError
  );
  assert.throws(
    () => codec.decodeMetadata(Buffer.from('0100000000000000000000100001', 'hex')),
    codec.StoreFormatError
  );
  assert.equal(codec.decodeVersion('e1:0000000000000000:9223372036854775808'), null);
  assert.throws(() => codec.encodeVersion('0', '1\n'), RangeError);
  assert.deepEqual(Object.keys(require('../../packages/framework-locations-etcd/dist')), []);
});

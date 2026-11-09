// etcd Store wire oracle: common spec §3, checked against the shared golden vectors.
const assert = require("node:assert/strict");
const golden = require("../../framework/runtime/protocol/golden/location-store-etcd-v1.json");
const MAX_TIME_MS = 9223372036854n;
function keys(namespace) {
  const namespacePrefix = Buffer.from(`${namespace}\0`);
  const metadataPrefix = Buffer.from(`${namespace}\0m\0`);
  const payloadPrefix = Buffer.from(`${namespace}\0p\0`);
  return {
    namespacePrefix,
    metadataPrefix,
    payloadPrefix,
    clock: Buffer.from(`${namespace}\0c`),
    metadata: (key) => Buffer.concat([metadataPrefix, key]),
    payload: (key) => Buffer.concat([payloadPrefix, key]),
  };
}
function encodeClock(value) {
  const b = Buffer.alloc(8);
  b.writeBigUInt64BE(BigInt(value));
  return b;
}
function decodeClock(bytes) {
  assert.equal(bytes.length, 8);
  return bytes.readBigUInt64BE();
}
function encodeMetadata({ hasExpiry, expiresAtMs, payloadLength }) {
  const b = Buffer.alloc(14);
  b[0] = 1;
  b[1] = Number(hasExpiry);
  b.writeBigUInt64BE(BigInt(expiresAtMs), 2);
  b.writeUInt32BE(payloadLength, 10);
  return b;
}
function encodeVersion(cluster, revision) {
  return `e1:${BigInt(cluster).toString(16).padStart(16, "0")}:${revision}`;
}
function prefixEnd(prefix) {
  const end = Buffer.from(prefix);
  for (let i = end.length - 1; i >= 0; i--)
    if (end[i] < 255) {
      end[i]++;
      return end.subarray(0, i + 1);
    }
  return Buffer.from([0]);
}
function verifyGolden() {
  for (const v of golden.keyVectors) {
    const k = keys(v.namespace),
      logical = Buffer.from(v.frameworkKeyHex, "hex");
    assert.equal(k.metadata(logical).toString("hex"), v.metadataKeyHex);
    assert.equal(k.payload(logical).toString("hex"), v.payloadKeyHex);
    assert.equal(k.clock.toString("hex"), v.clockKeyHex);
  }
  for (const v of golden.metadataVectors)
    assert.equal(encodeMetadata(v).toString("hex"), v.metadataHex);
  for (const v of golden.clockVectors)
    assert.equal(encodeClock(v.value).toString("hex"), v.hex);
  for (const v of golden.storeVersions.accepted)
    assert.equal(encodeVersion(v.clusterIdDecimal, v.modRevision), v.value);
}
module.exports = {
  keys,
  encodeClock,
  decodeClock,
  encodeMetadata,
  encodeVersion,
  prefixEnd,
  MAX_TIME_MS,
  verifyGolden,
};

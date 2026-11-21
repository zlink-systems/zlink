const {
  ServiceRelocationAuthorityPayloadCodec
} = require('../../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

// Generates only the retired v1 envelope as compatibility reader input.
class LegacyRelocationAuthorityFixture extends ServiceRelocationAuthorityPayloadCodec {
  publish(base, publication) {
    const u16 = (n) => {
      const bytes = Buffer.alloc(2);
      bytes.writeUInt16LE(n);
      return bytes;
    };
    const u32 = (n) => {
      const bytes = Buffer.alloc(4);
      bytes.writeUInt32LE(n);
      return bytes;
    };
    const u64 = (n) => {
      const bytes = Buffer.alloc(8);
      bytes.writeBigUInt64LE(n);
      return bytes;
    };
    const text = (s) => {
      const bytes = Buffer.from(s, 'utf8');
      return Buffer.concat([u16(bytes.length), bytes]);
    };
    const block = (bytes) => Buffer.concat([u32(bytes.length), bytes]);
    const uuid = Buffer.from(publication.aggregateId.replaceAll('-', ''), 'hex');
    const guid = Buffer.from([
      uuid[3],
      uuid[2],
      uuid[1],
      uuid[0],
      uuid[5],
      uuid[4],
      uuid[7],
      uuid[6],
      ...uuid.subarray(8)
    ]);
    return Buffer.concat([
      Buffer.from('RALZ'),
      u16(1),
      text(publication.reference),
      u32(publication.checksumCrc32c),
      guid,
      u64(publication.aggregateGeneration),
      block(Buffer.from(publication.inventoryDigest, 'hex')),
      text(publication.targetOwnerId),
      u64(publication.targetOwnerLeaseGeneration),
      block(base)
    ]);
  }
}
module.exports = { LegacyRelocationAuthorityFixture };

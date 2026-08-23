const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const {
  decodeCanonicalAuthorityPayload,
  decodeActorAuthorityPayload,
  encodeActorAuthorityPayload,
  encodeCanonicalAuthorityPayload
} = require('../../packages/framework/dist/runtime/actors/actor-authority-payload-codec');
const {
  decodeActorAuthorityIdentity,
  decodeRelocatingActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  crc32c
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

test('authority-payload-v1 golden fixture decodes and re-encodes byte-exactly', () => {
  const fixture = JSON.parse(readFileSync(path.resolve(
    __dirname,
    '../../../../runtime/protocol/golden/durable-authority-v1.json'
  ), 'utf8'));
  assert(fixture.consumers.includes('node'));
  const encoded = Buffer.from(fixture.encodedHex, 'hex');
  const decoded = decodeCanonicalAuthorityPayload(encoded);
  assert.notEqual(decoded, undefined);
  assert.equal(
    encodeCanonicalAuthorityPayload(decoded).toString('hex'),
    fixture.encodedHex
  );
});

test('Actor authority identity uses direct canonical ZLAU and record generation', () => {
  const encoded = encodeActorAuthorityIdentity({
    actorType: 'Player',
    actor: {
      actorId: 'actor-a',
      objectGeneration: 17n,
      meshName: 'game',
      nodeRid: 'node-a'
    },
    meshName: 'game',
    ownerNodeGeneration: 23n,
    owner: { ownerId: 'owner-a', leaseGeneration: 29n },
    spotId: 'room-a',
    spotGeneration: 31n,
    spotKind: 'user'
  });
  assert.equal(encoded.subarray(0, 4).toString('ascii'), 'ZLAU');
  assert.equal(encoded.includes(Buffer.from('actorGeneration')), false);
  assert.deepEqual(decodeActorAuthorityIdentity(encoded, 17n), {
    actorType: 'Player',
    actor: {
      actorId: 'actor-a',
      objectGeneration: 17n,
      meshName: 'game',
      nodeRid: 'node-a'
    },
    meshName: 'game',
    ownerNodeGeneration: 23n,
    owner: { ownerId: 'owner-a', leaseGeneration: 29n },
    state: 'ready',
    spotId: 'room-a',
    spotGeneration: 31n,
    spotKind: 'user'
  });
  assert.equal(decodeActorAuthorityIdentity(
    Buffer.from('{"version":1,"actorId":"actor-a"}'),
    17n
  ), undefined);
});

test('Actor authority payload matches the .NET byte layout and decodes byte-exact fields', () => {
  const value = {
    state: 'ready',
    stableType: 'A',
    actorId: 'B',
    currentSpotId: 'C',
    currentSpotGeneration: 2n,
    currentSpotKind: 'entry',
    ownerId: 'D',
    ownerLeaseGeneration: 3n,
    meshName: 'E',
    nodeRid: 'F',
    nodeGeneration: 4n
  };
  const expectedHex = '5a4c4155010000000000340001001001410142010143'
    + '0000000000000002010144000000000000000301450146'
    + '000000000000000400000000000000000000b2374797';
  const encoded = encodeActorAuthorityPayload(value);
  assert.equal(encoded.toString('hex'), expectedHex);
  assert.deepEqual(decodeActorAuthorityPayload(encoded), value);
});

test('ZLAP v5 and v6 unwrap only at the authority phase allowed by the decoder', () => {
  const authority = encodeActorAuthorityIdentity(actorIdentity());
  const v5 = encodeZlap({ version: 5, phase: 4, authority });
  const v6 = encodeZlap({ version: 6, phase: 4, authority, bound: true });

  assert.deepEqual(decodeActorAuthorityIdentity(v5, 17n), actorIdentity());
  assert.deepEqual(decodeActorAuthorityIdentity(v6, 17n), actorIdentity());

  for (const phase of [1, 2, 3]) {
    const relocating = encodeZlap({ version: 6, phase, authority, bound: true });
    assert.equal(decodeActorAuthorityIdentity(relocating, 17n), undefined);
    assert.deepEqual(
      decodeRelocatingActorAuthorityIdentity(relocating, 17n),
      actorIdentity()
    );
  }
});

test('ZLAP rejects zero bound-route generations and checksum mismatches', () => {
  const authority = encodeActorAuthorityIdentity(actorIdentity());
  for (const zeroGeneration of [
    'binding',
    'object',
    'authority-owner',
    'target-node',
    'owner-lease',
    'session-owner-node'
  ]) {
    const invalid = encodeZlap({
      version: 6,
      phase: 4,
      authority,
      bound: true,
      zeroGeneration
    });
    assert.equal(decodeActorAuthorityIdentity(invalid, 17n), undefined, zeroGeneration);
    assert.equal(
      decodeRelocatingActorAuthorityIdentity(invalid, 17n),
      undefined,
      zeroGeneration
    );
  }

  const corrupt = encodeZlap({ version: 5, phase: 4, authority });
  corrupt[corrupt.length - 1] ^= 0x01;
  assert.equal(decodeActorAuthorityIdentity(corrupt, 17n), undefined);
  assert.equal(decodeRelocatingActorAuthorityIdentity(corrupt, 17n), undefined);
});

function actorIdentity() {
  return {
    actorType: 'Player',
    actor: {
      actorId: 'actor-a',
      objectGeneration: 17n,
      meshName: 'game',
      nodeRid: 'node-a'
    },
    meshName: 'game',
    ownerNodeGeneration: 23n,
    owner: { ownerId: 'owner-a', leaseGeneration: 29n },
    state: 'ready',
    spotId: 'room-a',
    spotGeneration: 31n,
    spotKind: 'user'
  };
}

function encodeZlap({
  version,
  phase,
  authority,
  bound = false,
  zeroGeneration
}) {
  const parts = [
    Buffer.from('ZLAP'),
    u16le(version),
    Buffer.from('00112233445566778899aabbccddeeff', 'hex'),
    Buffer.of(phase, bound ? 1 : 0)
  ];
  if (bound) {
    parts.push(
      bytes8('node-b'),
      bytes8('session-a'),
      text16('binding-a'),
      u64le(zeroGeneration === 'binding' ? 0n : 1n),
      u64le(zeroGeneration === 'object' ? 0n : 2n),
      u64le(zeroGeneration === 'authority-owner' ? 0n : 3n),
      text16('game'),
      u64le(zeroGeneration === 'target-node' ? 0n : 4n),
      u64le(zeroGeneration === 'owner-lease' ? 0n : 5n),
      u64le(zeroGeneration === 'session-owner-node' ? 0n : 6n),
      u64le(0n)
    );
    if (version === 6) parts.push(text16('session-owner-a'), u64le(7n));
  }
  parts.push(i32le(authority.byteLength), authority);
  const envelope = Buffer.concat(parts);
  return Buffer.concat([envelope, u32le(crc32c(envelope))]);
}

function bytes8(value) {
  const bytes = Buffer.from(value, 'utf8');
  return Buffer.concat([Buffer.of(bytes.byteLength), bytes]);
}

function text16(value) {
  const bytes = Buffer.from(value, 'utf8');
  return Buffer.concat([u16le(bytes.byteLength), bytes]);
}

function u16le(value) {
  const result = Buffer.alloc(2);
  result.writeUInt16LE(value);
  return result;
}

function u32le(value) {
  const result = Buffer.alloc(4);
  result.writeUInt32LE(value);
  return result;
}

function i32le(value) {
  const result = Buffer.alloc(4);
  result.writeInt32LE(value);
  return result;
}

function u64le(value) {
  const result = Buffer.alloc(8);
  result.writeBigUInt64LE(value);
  return result;
}

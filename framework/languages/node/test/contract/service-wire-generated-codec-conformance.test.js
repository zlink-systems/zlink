const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { after, test } = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const protocolRoot = path.resolve(nodeRoot, '../../runtime/protocol');
const generatedSource = path.join(protocolRoot, 'generated/node/service_wire_codec.generated.ts');
const compiler = process.env.ZLINK_TYPESCRIPT_COMPILER
  ?? path.join(nodeRoot, 'node_modules/typescript/bin/tsc');
const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-service-wire-ts-'));

execFileSync(process.execPath, [
  compiler,
  generatedSource,
  '--target', 'ES2022',
  '--module', 'commonjs',
  '--moduleResolution', 'node',
  '--skipLibCheck',
  '--outDir', outputDirectory
], { cwd: nodeRoot, stdio: 'inherit' });

const codec = require(path.join(outputDirectory, 'service_wire_codec.generated.js'));
const pilot = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const {
  decodeCanonicalAuthorityPayload,
  encodeCanonicalAuthorityPayload
} = require('../../packages/framework/dist/runtime/actors/actor-authority-payload-codec');
const {
  decodeInstanceActivationRecoveryEnvelope,
  encodeInstanceActivationRecoveryEnvelope
} = require('../../packages/framework/dist/runtime/foundation/service-instance-activation-recovery-codec');
const catalog = JSON.parse(fs.readFileSync(
  path.join(protocolRoot, 'generated/fixtures/index.json'),
  'utf8'
));

after(() => fs.rmSync(outputDirectory, { recursive: true, force: true }));

function pointer(document, value) {
  return value.split('/').slice(1).reduce((current, segment) =>
    current[segment.replaceAll('~1', '/').replaceAll('~0', '~')], document);
}

function frames(entry, fixture) {
  const pointers = entry.pointers;
  const hex = pointer(fixture, pointers.framesHex ?? pointers.hex);
  const values = Array.isArray(hex) ? hex : [hex];
  return values.map((value) => Buffer.from(value, 'hex'));
}

const commandOracles = new Map([
  [28, [pilot.decodeActorJoin28, pilot.encodeActorJoin28]],
  [47, [(value) => pilot.decodeUserSpotCreate47(value[0]),
    (value) => [pilot.encodeUserSpotCreate47(value)]]],
  [48, [(value) => pilot.decodeUserSpotClose48(value[0]),
    (value) => [pilot.encodeUserSpotClose48(value)]]],
  [49, [(value) => pilot.decodeActorCreate49(value[0]),
    (value) => [pilot.encodeActorCreate49(value)]]]
]);

function oracleRoundTrip(indexed, bytes) {
  switch (indexed.surface.format) {
    case 'authority-payload-v1':
      return encodeCanonicalAuthorityPayload(decodeCanonicalAuthorityPayload(bytes));
    case 'instance-activation-recovery-v1':
      return encodeInstanceActivationRecoveryEnvelope(
        decodeInstanceActivationRecoveryEnvelope(bytes)
      );
    case 'relocation-data-chunk-v1':
      return pilot.encodeRelocationDataChunkV1(pilot.decodeRelocationDataChunkV1(bytes));
    case 'relocation-manifest-v1':
      return pilot.encodeRelocationManifestV1(pilot.decodeRelocationManifestV1(bytes));
    case 'relocation-envelope-v1':
      return pilot.encodeRelocationEnvelopeV1(pilot.decodeRelocationEnvelopeV1([bytes]));
    default:
      throw new RangeError(`missing hand-codec oracle for ${indexed.surface.format}`);
  }
}

test('generated TypeScript codec round-trips every indexed canonical fixture byte-exactly', () => {
  assert.equal(catalog.fixtures.length, 9);

  for (const indexed of catalog.fixtures) {
    const fixture = JSON.parse(fs.readFileSync(
      path.join(protocolRoot, indexed.goldenFixture),
      'utf8'
    ));

    for (const canonical of indexed.canonical) {
      const label = `${indexed.surface.format}:${canonical.name}`;
      if (indexed.kind === 'durable') {
        const bytes = Buffer.from(pointer(fixture, canonical.pointers.encodedHex), 'hex');
        const decoded = codec.decodeServiceWireDurableFormat(indexed.surface.format, bytes);
        assert.deepEqual(Buffer.from(
          codec.encodeServiceWireDurableFormat(indexed.surface.format, decoded)
        ), bytes, label);
        assert.deepEqual(Buffer.from(oracleRoundTrip(indexed, bytes)), bytes, `${label}:oracle`);
      } else if (indexed.kind === 'logical') {
        const bytes = Buffer.from(pointer(fixture, canonical.pointers.logicalHex), 'hex');
        const decoded = codec.decodeRelocationLogicalStream(bytes);
        assert.deepEqual(Buffer.from(codec.encodeRelocationLogicalStream(decoded)), bytes, label);
        assert.deepEqual(Buffer.from(oracleRoundTrip(indexed, bytes)), bytes, `${label}:oracle`);
      } else {
        const bytes = frames(canonical, fixture);
        const decoded = codec.decodeServiceWireCommand(bytes);
        assert.deepEqual(
          codec.encodeServiceWireCommand(decoded).map((frame) => Buffer.from(frame)),
          bytes,
          label
        );
        const [decode, encode] = commandOracles.get(indexed.surface.commandId);
        assert.deepEqual(
          encode(decode(bytes)).map((frame) => Buffer.from(frame)),
          bytes,
          `${label}:oracle`
        );
      }
    }
  }
});

test('generated TypeScript codec rejects every indexed malformed fixture', () => {
  let malformedCount = 0;
  for (const indexed of catalog.fixtures) {
    const fixture = JSON.parse(fs.readFileSync(
      path.join(protocolRoot, indexed.goldenFixture),
      'utf8'
    ));

    for (const malformed of indexed.malformed) {
      malformedCount += 1;
      assert.throws(
        () => codec.decodeServiceWireCommand(frames(malformed, fixture)),
        undefined,
        `${indexed.surface.format}:${malformed.name}`
      );
      const [decode] = commandOracles.get(indexed.surface.commandId);
      assert.throws(
        () => decode(frames(malformed, fixture)),
        undefined,
        `${indexed.surface.format}:${malformed.name}:oracle`
      );
    }
  }
  assert.equal(malformedCount, 12);
});

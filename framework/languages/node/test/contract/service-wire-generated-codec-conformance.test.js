const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { after, test } = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const protocolRoot = path.resolve(nodeRoot, '../../runtime/protocol');
const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-service-wire-ts-'));
const compiler = path.join(nodeRoot, 'node_modules/typescript/bin/tsc');
execFileSync(process.execPath, [
  compiler,
  path.join(protocolRoot, 'generated/node/service_wire_codec.generated.ts'),
  '--target', 'ES2022',
  '--module', 'commonjs',
  '--moduleResolution', 'node',
  '--skipLibCheck',
  '--outDir', outputDirectory
], { cwd: nodeRoot, stdio: 'inherit' });

const codec = require(path.join(outputDirectory, 'service_wire_codec.generated.js'));
const constants = require('../../../../runtime/protocol/generated/node/service_wire_constants.js');
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
const context = {
  effectiveCompleteMessageBytes: 4294967295,
  effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 4294966774,
  runtimePredicates: {
    'service-wire-constants.valid-terminal-failure': constants.isValidServiceWireTerminalFailure
  }
};

after(() => fs.rmSync(outputDirectory, { recursive: true, force: true }));

function pointer(document, value) {
  return value.split('/').slice(1).reduce((current, segment) =>
    current[segment.replaceAll('~1', '/').replaceAll('~0', '~')], document);
}

function frameBytes(entry, fixture) {
  const hex = pointer(fixture, entry.pointers.framesHex ?? entry.pointers.hex);
  return (Array.isArray(hex) ? hex : [hex]).map((value) => Buffer.from(value, 'hex'));
}

const commands = new Map([
  [16, [codec.decodeNodeSendCommand, codec.encodeNodeSendCommand]],
  [20, [codec.decodeReplyCommand, codec.encodeReplyCommand]],
  [24, [codec.decodeActorSendCommand, codec.encodeActorSendCommand]],
  [28, [codec.decodeActorJoinCommand, codec.encodeActorJoinCommand]],
  [47, [codec.decodeUserSpotCreateCommand, codec.encodeUserSpotCreateCommand]],
  [48, [codec.decodeUserSpotCloseCommand, codec.encodeUserSpotCloseCommand]],
  [49, [codec.decodeActorCreateCommand, codec.encodeActorCreateCommand]]
]);
const commandOracles = new Map([
  [28, [pilot.decodeActorJoin28, pilot.encodeActorJoin28]],
  [47, [(frames) => pilot.decodeUserSpotCreate47(frames[0]),
    (value) => [pilot.encodeUserSpotCreate47(value)]]],
  [48, [(frames) => pilot.decodeUserSpotClose48(frames[0]),
    (value) => [pilot.encodeUserSpotClose48(value)]]],
  [49, [(frames) => pilot.decodeActorCreate49(frames[0]),
    (value) => [pilot.encodeActorCreate49(value)]]]
]);
const durable = new Map([
  ['authority-payload-v1', [codec.decodeAuthorityPayloadV1DurableFormat,
    codec.encodeAuthorityPayloadV1DurableFormat]],
  ['instance-activation-recovery-v1', [codec.decodeInstanceActivationRecoveryV1DurableFormat,
    codec.encodeInstanceActivationRecoveryV1DurableFormat]],
  ['relocation-data-chunk-v1', [codec.decodeRelocationDataChunkV1DurableFormat,
    codec.encodeRelocationDataChunkV1DurableFormat]],
  ['relocation-manifest-v1', [codec.decodeRelocationManifestV1DurableFormat,
    codec.encodeRelocationManifestV1DurableFormat]]
]);
const typeCodecs = new Map([
  ['application-payload-bytes', [codec.decodeApplicationPayloadBytes,
    codec.encodeApplicationPayloadBytes]],
  ['application-payload-envelope-v1', [codec.decodeApplicationPayloadEnvelopeV1,
    codec.encodeApplicationPayloadEnvelopeV1]],
  ['descriptor-extension', [codec.decodeDescriptorExtension, codec.encodeDescriptorExtension]],
  ['text8', [codec.decodeText8, codec.encodeText8]]
]);

function oracleRoundTrip(format, bytes) {
  switch (format) {
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
      throw new RangeError(`missing hand-codec oracle for ${format}`);
  }
}

function indexedCases() {
  const cases = [];
  for (const indexed of catalog.fixtures) {
    const fixture = JSON.parse(fs.readFileSync(
      path.join(protocolRoot, indexed.goldenFixture),
      'utf8'
    ));
    for (const canonical of indexed.canonical) {
      cases.push({ kind: 'canonical', indexed, fixture, entry: canonical });
    }
    for (const malformed of indexed.malformed) {
      cases.push({ kind: 'malformed', indexed, fixture, entry: malformed });
    }
  }
  for (const entry of catalog.operationCases) {
    cases.push({ kind: 'operation', entry });
  }
  return cases;
}

test('generated TypeScript codec consumes every indexed conformance case', () => {
  assert.equal(catalog.version, 2);
  assert.equal(catalog.fixtures.length, 9);
  assert.equal(catalog.fixtures.reduce((count, fixture) => count + fixture.canonical.length, 0), 11);
  assert.equal(catalog.fixtures.reduce((count, fixture) => count + fixture.malformed.length, 0), 12);
  assert.equal(catalog.operationCases.length, 19);

  for (const item of indexedCases()) {
    const operation = item.kind === 'operation' ? item.entry.operation : item.kind;
    const label = `${operation}:${item.entry.name}`;
    if (item.kind === 'canonical') {
      const { indexed, fixture, entry } = item;
      if (indexed.kind === 'command') {
        const bytes = frameBytes(entry, fixture);
        const [decode, encode] = commands.get(indexed.surface.commandId);
        assert.deepEqual(encode(decode(bytes, context), context)
          .map((frame) => Buffer.from(frame)), bytes, label);
        const [oracleDecode, oracleEncode] = commandOracles.get(indexed.surface.commandId);
        assert.deepEqual(oracleEncode(oracleDecode(bytes))
          .map((frame) => Buffer.from(frame)), bytes, `${label}:oracle`);
      } else {
        const hex = pointer(fixture, entry.pointers.encodedHex ?? entry.pointers.logicalHex);
        const bytes = Buffer.from(hex, 'hex');
        if (indexed.kind === 'durable') {
          const [decode, encode] = durable.get(indexed.surface.format);
          assert.deepEqual(Buffer.from(encode(decode(bytes, context), context)), bytes, label);
        } else {
          const decoded = codec.decodeRelocationEnvelopeV1LogicalStream(bytes, context);
          assert.deepEqual(Buffer.from(
            codec.encodeRelocationEnvelopeV1LogicalStream(decoded, context)
          ), bytes, label);
        }
        assert.deepEqual(Buffer.from(oracleRoundTrip(indexed.surface.format, bytes)), bytes,
          `${label}:oracle`);
      }
      continue;
    }

    if (item.kind === 'malformed') {
      const bytes = frameBytes(item.entry, item.fixture);
      const [decode] = commands.get(item.indexed.surface.commandId);
      assert.throws(() => decode(bytes, context), undefined, label);
      const [oracleDecode] = commandOracles.get(item.indexed.surface.commandId);
      assert.throws(() => oracleDecode(bytes), undefined, `${label}:oracle`);
      continue;
    }

    const entry = item.entry;
    if (entry.operation === 'negotiated-bound') {
      const bytes = Buffer.from(entry.hex, 'hex');
      const [decode, encode] = typeCodecs.get(entry.surface.type);
      const value = decode(bytes, context);
      const negotiatedContext = {
        runtimePredicates: context.runtimePredicates,
        ...entry.context
      };
      for (const direction of entry.directions) {
        const action = direction === 'decode'
          ? () => decode(bytes, negotiatedContext)
          : () => encode(value, negotiatedContext);
        const directionLabel = `${label}:${direction}`;
        if (entry.expect === 'reject') assert.throws(action, undefined, directionLabel);
        else assert.doesNotThrow(action, directionLabel);
      }
      continue;
    }
    const decodeContext = { ...context, ...entry.context };
    const action = () => {
      if (entry.surface.format === 'type') {
        const [decode] = typeCodecs.get(entry.surface.type);
        return decode(Buffer.from(entry.hex, 'hex'), decodeContext);
      }
      if (entry.surface.format === 'semantic') {
        return codec.validateReplyCommandRuntimePredicates(entry.input, decodeContext);
      }
      if (entry.surface.format === 'command') {
        const [decode] = commands.get(entry.surface.commandId);
        return decode(entry.framesHex.map((hex) => Buffer.from(hex, 'hex')), decodeContext);
      }
      if (entry.surface.format === 'relocation-envelope-v1') {
        return codec.decodeRelocationEnvelopeV1LogicalStream(Buffer.from(entry.hex, 'hex'), decodeContext);
      }
      const [decode] = durable.get(entry.surface.format);
      return decode(Buffer.from(entry.hex, 'hex'), decodeContext);
    };
    if (entry.expect === 'reject') assert.throws(action, undefined, label);
    else assert.doesNotThrow(action, label);
  }
});

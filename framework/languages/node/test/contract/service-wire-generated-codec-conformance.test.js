const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { after, test } = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const protocolRoot = path.resolve(nodeRoot, '../../runtime/protocol');
const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-service-wire-ts-'));
const compiler = path.join(nodeRoot, 'node_modules/typescript/bin/tsc');
execFileSync(
  process.execPath,
  [
    compiler,
    path.join(protocolRoot, 'generated/node/service_wire_codec.generated.ts'),
    '--target',
    'ES2022',
    '--module',
    'commonjs',
    '--moduleResolution',
    'node',
    '--skipLibCheck',
    '--outDir',
    outputDirectory
  ],
  { cwd: nodeRoot, stdio: 'inherit' }
);

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
const catalog = JSON.parse(
  fs.readFileSync(path.join(protocolRoot, 'generated/fixtures/index.json'), 'utf8')
);
const context = {
  effectiveCompleteMessageBytes: 4294967295,
  effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 4294966774
};

after(() => fs.rmSync(outputDirectory, { recursive: true, force: true }));

function pointer(document, value) {
  return value
    .split('/')
    .slice(1)
    .reduce(
      (current, segment) => current[segment.replaceAll('~1', '/').replaceAll('~0', '~')],
      document
    );
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
  [
    47,
    [
      (frames) => pilot.decodeUserSpotCreate47(frames[0]),
      (value) => [pilot.encodeUserSpotCreate47(value)]
    ]
  ],
  [
    48,
    [
      (frames) => pilot.decodeUserSpotClose48(frames[0]),
      (value) => [pilot.encodeUserSpotClose48(value)]
    ]
  ],
  [
    49,
    [
      (frames) => pilot.decodeActorCreate49(frames[0]),
      (value) => [pilot.encodeActorCreate49(value)]
    ]
  ]
]);
const durable = new Map([
  [
    'authority-payload-v1',
    [codec.decodeAuthorityPayloadV1DurableFormat, codec.encodeAuthorityPayloadV1DurableFormat]
  ],
  [
    'instance-activation-recovery-v1',
    [
      codec.decodeInstanceActivationRecoveryV1DurableFormat,
      codec.encodeInstanceActivationRecoveryV1DurableFormat
    ]
  ],
  [
    'relocation-data-chunk-v1',
    [codec.decodeRelocationDataChunkV1DurableFormat, codec.encodeRelocationDataChunkV1DurableFormat]
  ],
  [
    'relocation-manifest-v1',
    [codec.decodeRelocationManifestV1DurableFormat, codec.encodeRelocationManifestV1DurableFormat]
  ]
]);
const typeCodecs = new Map([
  [
    'application-payload-bytes',
    [codec.decodeApplicationPayloadBytes, codec.encodeApplicationPayloadBytes]
  ],
  [
    'application-payload-envelope-v1',
    [codec.decodeApplicationPayloadEnvelopeV1, codec.encodeApplicationPayloadEnvelopeV1]
  ],
  ['descriptor-extension', [codec.decodeDescriptorExtension, codec.encodeDescriptorExtension]],
  ['text8', [codec.decodeText8, codec.encodeText8]]
]);

function generatedName(name) {
  return name
    .split(/[^A-Za-z0-9]+/)
    .filter(Boolean)
    .map((part) => part[0].toUpperCase() + part.slice(1))
    .join('');
}

function codecForType(name) {
  const generated = generatedName(name);
  return typeCodecs.get(name) ?? [codec[`decode${generated}`], codec[`encode${generated}`]];
}

function recipeBytes(recipe) {
  const bytes = Buffer.alloc(recipe.encodedBytes);
  let offset = 0;
  for (const segment of recipe.segments) {
    if (segment.hex !== undefined) {
      const value = Buffer.from(segment.hex, 'hex');
      value.copy(bytes, offset);
      offset += value.length;
    } else {
      bytes.fill(segment.repeatByte, offset, offset + segment.count);
      offset += segment.count;
    }
  }
  assert.equal(offset, recipe.encodedBytes);
  assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), recipe.sha256);
  return bytes;
}

function operationBytes(entry) {
  if (entry.hex !== undefined) return Buffer.from(entry.hex, 'hex');
  if (entry.byteRecipe !== undefined) return recipeBytes(entry.byteRecipe);
  return undefined;
}

function exerciseLogicalStream(entry) {
  const decoder = new codec.RelocationEnvelopeV1LogicalStreamDecoder(context);
  if (entry.syntheticPriorInputByteCount !== undefined) {
    decoder.setSyntheticPriorInputByteCountForTest(entry.syntheticPriorInputByteCount);
  }
  let complete;
  let supplied = 0;
  for (let index = 0; index < entry.chunksHex.length; ++index) {
    const chunk = Buffer.from(entry.chunksHex[index], 'hex');
    supplied += chunk.length;
    const expected = entry.chunkOutcomes[index];
    let step;
    try {
      step = decoder.push(chunk, index === entry.finalChunkIndex);
    } catch (error) {
      assert.equal(entry.expect, 'reject');
      assert.equal(index, entry.failureChunkIndex, entry.name);
      assert.equal(expected, entry.failureKind);
      assert.ok(error instanceof codec.LogicalDecodeError);
      assert.equal(error.kind, entry.failureKind);
      assert.equal(error.offset, entry.failureOffset);
      const failureKind = error.kind;
      const failureOffset = error.offset;
      chunk.fill(0);
      assert.equal(error.kind, failureKind);
      assert.equal(error.offset, failureOffset);
      if (entry.assertChunkReleasedAfterFailure) {
        assert.throws(() => decoder.push(new Uint8Array(), false));
        assert.equal(error.kind, failureKind);
        assert.equal(error.offset, failureOffset);
      }
      return;
    }
    assert.notEqual(index, entry.failureChunkIndex, 'expected logical failure was not raised');
    chunk.fill(0);
    if (step.state === 'needMore') {
      assert.ok(step.bufferedInputByteCount <= entry.maximumBufferedInputBytes);
      const expectedBuffered = entry.expectedBufferedInputByteCounts?.[index];
      if (expectedBuffered !== null && expectedBuffered !== undefined) {
        assert.equal(step.bufferedInputByteCount, expectedBuffered, entry.name);
      }
    }
    assert.equal(step.consumedByteCount, supplied);
    assert.ok(
      step.continuationDepth <=
        codec.RelocationEnvelopeV1LogicalStreamDecoder.maximumContinuationDepth
    );
    assert.equal(step.state, expected === 'need-more' ? 'needMore' : expected);
    if (step.state === 'complete') complete = step.value;
  }
  assert.equal(entry.expect, 'accept');
  assert.ok(complete !== undefined);
  assert.throws(() => decoder.push(new Uint8Array(), true));
  const canonical = Buffer.concat(entry.chunksHex.map((hex) => Buffer.from(hex, 'hex')));
  assert.deepEqual(
    Buffer.from(codec.encodeRelocationEnvelopeV1LogicalStream(complete, context)),
    canonical
  );
}

function semanticInput(value) {
  if (Array.isArray(value)) return value.map(semanticInput);
  if (value && typeof value === 'object') {
    if (Number.isInteger(value.repeatByte) && Number.isInteger(value.count)) {
      return Uint8Array.from({ length: value.count }, () => value.repeatByte);
    }
    return Object.fromEntries(
      Object.entries(value).map(([name, entry]) =>
        name.endsWith('Hex')
          ? [name.slice(0, -3), Buffer.from(entry, 'hex')]
          : [name, semanticInput(entry)]
      )
    );
  }
  return value;
}

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
    const fixture = JSON.parse(
      fs.readFileSync(path.join(protocolRoot, indexed.goldenFixture), 'utf8')
    );
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

function operationCodec(entry) {
  if (entry.surface.format === 'type') return codecForType(entry.surface.type);
  if (entry.surface.format === 'command') return commands.get(entry.surface.commandId);
  if (entry.surface.format === 'relocation-envelope-v1') {
    return [
      codec.decodeRelocationEnvelopeV1LogicalStream,
      codec.encodeRelocationEnvelopeV1LogicalStream
    ];
  }
  if (entry.surface.format === 'semantic') {
    const validate = (value, operationContext) => {
      codec.validateReplyCommandRuntimePredicates(value, operationContext);
      return value;
    };
    return [validate, validate];
  }
  return durable.get(entry.surface.format);
}

function operationWire(entry) {
  if (entry.surface.format === 'command') {
    return entry.framesHex.map((hex) => Buffer.from(hex, 'hex'));
  }
  return operationBytes(entry);
}

function assertWireEqual(actual, expected, label) {
  if (Array.isArray(expected)) {
    assert.deepEqual(
      actual.map((frame) => Buffer.from(frame)),
      expected,
      label
    );
  } else {
    assert.deepEqual(Buffer.from(actual), expected, label);
  }
}

test('generated TypeScript codec consumes every indexed conformance case', () => {
  assert.equal(catalog.version, 4);
  assert.equal(catalog.fixtures.length, 9);
  assert.equal(
    catalog.fixtures.reduce((count, fixture) => count + fixture.canonical.length, 0),
    11
  );
  assert.equal(
    catalog.fixtures.reduce((count, fixture) => count + fixture.malformed.length, 0),
    12
  );
  assert.equal(catalog.operationCases.filter((entry) => entry.expect === 'accept').length, 33);
  assert.equal(catalog.operationCases.filter((entry) => entry.expect === 'reject').length, 60);
  const boundaryPairs = new Map();
  for (const entry of catalog.operationCases.filter(
    (candidate) => candidate.boundaryPair !== undefined
  )) {
    const entries = boundaryPairs.get(entry.boundaryPair) ?? [];
    entries.push(entry);
    boundaryPairs.set(entry.boundaryPair, entries);
  }
  assert.equal(boundaryPairs.size, 25);
  for (const [operation, entries] of boundaryPairs) {
    assert.deepEqual(
      new Set(entries.map((entry) => entry.expect)),
      new Set(['accept', 'reject']),
      operation
    );
  }

  for (const item of indexedCases()) {
    const operation = item.kind === 'operation' ? item.entry.operation : item.kind;
    const label = `${operation}:${item.entry.name}`;
    if (item.kind === 'canonical') {
      const { indexed, fixture, entry } = item;
      if (indexed.kind === 'command') {
        const bytes = frameBytes(entry, fixture);
        const [decode, encode] = commands.get(indexed.surface.commandId);
        assert.deepEqual(
          encode(decode(bytes, context), context).map((frame) => Buffer.from(frame)),
          bytes,
          label
        );
        const [oracleDecode, oracleEncode] = commandOracles.get(indexed.surface.commandId);
        assert.deepEqual(
          oracleEncode(oracleDecode(bytes)).map((frame) => Buffer.from(frame)),
          bytes,
          `${label}:oracle`
        );
      } else {
        const hex = pointer(fixture, entry.pointers.encodedHex ?? entry.pointers.logicalHex);
        const bytes = Buffer.from(hex, 'hex');
        if (indexed.kind === 'durable') {
          const [decode, encode] = durable.get(indexed.surface.format);
          assert.deepEqual(Buffer.from(encode(decode(bytes, context), context)), bytes, label);
        } else {
          const decoded = codec.decodeRelocationEnvelopeV1LogicalStream(bytes, context);
          assert.deepEqual(
            Buffer.from(codec.encodeRelocationEnvelopeV1LogicalStream(decoded, context)),
            bytes,
            label
          );
        }
        assert.deepEqual(
          Buffer.from(oracleRoundTrip(indexed.surface.format, bytes)),
          bytes,
          `${label}:oracle`
        );
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
    if (entry.operation === 'logical-stream') {
      exerciseLogicalStream(entry);
      continue;
    }
    const pair = operationCodec(entry);
    assert.ok(pair, `${label}:codec`);
    const [decode, encode] = pair;
    const wire = operationWire(entry);
    const operationContext =
      entry.operation === 'negotiated-bound'
        ? { runtimePredicates: context.runtimePredicates, ...entry.context }
        : { ...context, ...entry.context };
    let encodeValue;
    if (entry.input !== undefined) encodeValue = semanticInput(entry.input);
    else if (entry.encodeInputHex !== undefined) {
      const seed = decode(Buffer.from(entry.encodeInputHex, 'hex'), context);
      if (entry.encodeMutation !== 'duplicate-first-item') {
        throw new RangeError(`${label}:encode mutation`);
      }
      encodeValue = [seed[0], seed[0]];
    } else if ((entry.directions ?? []).includes('encode')) encodeValue = decode(wire, context);
    for (const direction of entry.directions ?? ['decode']) {
      const directionLabel = `${label}:${direction}`;
      const action =
        direction === 'decode'
          ? () => decode(entry.surface.format === 'semantic' ? encodeValue : wire, operationContext)
          : () => encode(encodeValue, operationContext);
      if (entry.expect === 'reject') {
        const expectedMessage = entry.expectedFailure?.nodeMessage;
        assert.throws(
          action,
          expectedMessage === undefined
            ? undefined
            : (error) => error instanceof Error && error.message.includes(expectedMessage),
          directionLabel
        );
      } else {
        const result = action();
        if (direction === 'encode' && entry.surface.format !== 'semantic') {
          assertWireEqual(result, wire, directionLabel);
        }
        if (direction === 'decode' && entry.decoded !== undefined) {
          assert.deepEqual(result, semanticInput(entry.decoded), directionLabel);
        }
      }
    }
  }
});

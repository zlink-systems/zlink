#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { OPERATION_KINDS } from "./service-wire-lowering.mjs";
import {
  encodeGoldenBody,
  encodeGoldenEnvelope,
  validateGoldenFixtures,
  validateSchema,
  validateServiceWireFixtureOracles,
} from "./validate-service-wire-schema.mjs";

const scriptPath = fileURLToPath(import.meta.url);
const protocolDirectory = path.dirname(scriptPath);

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function sha256(filePath) {
  return crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex");
}

function resolveRepositoryPath(relativePath, label) {
  if (typeof relativePath !== "string" || path.isAbsolute(relativePath)) {
    throw new Error(`${label} must be a repository-relative path`);
  }
  const resolved = path.resolve(protocolDirectory, relativePath);
  const relativeToRoot = path.relative(protocolDirectory, resolved);
  if (relativeToRoot.startsWith("..") || path.isAbsolute(relativeToRoot)) {
    throw new Error(`${label} escapes the protocol directory: ${relativePath}`);
  }
  return resolved;
}

function fixturePath(relativePath) {
  return resolveRepositoryPath(relativePath, "goldenFixture");
}

function assertEncodedBody(formatName, fixture) {
  if (!fixture || typeof fixture.decoded !== "object" || fixture.decoded === null) {
    throw new Error(`fixture:${formatName}: decoded case is required`);
  }
  const body = encodeGoldenBody(formatName, fixture.decoded);
  if (!Buffer.isBuffer(body)) {
    throw new Error(`fixture:${formatName}: encoder did not return bytes`);
  }
}

function pointerCase(name, pointers) {
  return { name, pointers };
}

function canonicalCases(fixture) {
  if (typeof fixture.encodedHex === "string") {
    return [pointerCase("canonical", {
      encodedHex: "/encodedHex",
      decoded: "/decoded",
    })];
  }
  if (typeof fixture.logicalHex === "string") {
    return [pointerCase("canonical", {
      logicalHex: "/logicalHex",
      decoded: "/decoded",
    })];
  }
  if (fixture.canonical !== undefined) {
    return [pointerCase(fixture.canonical.name ?? "canonical", {
      hex: "/canonical/hex",
      decoded: "/canonical/decoded",
    })];
  }
  if (Array.isArray(fixture.valid)) {
    return fixture.valid.map((entry, index) => pointerCase(entry.name, {
      input: `/valid/${index}/input`,
      framesHex: `/valid/${index}/framesHex`,
    }));
  }
  throw new Error(`fixture:${fixture.format}: canonical cases are missing`);
}

function malformedCases(fixture) {
  if (Array.isArray(fixture.malformed)) {
    return fixture.malformed.map((entry, index) => ({
      name: entry.name,
      error: entry.error,
      pointers: {
        hex: `/malformed/${index}/hex`,
      },
    }));
  }
  if (Array.isArray(fixture.invalid)) {
    return fixture.invalid.map((entry, index) => ({
      name: entry.name,
      error: entry.error,
      pointers: {
        framesHex: `/invalid/${index}/framesHex`,
      },
    }));
  }
  return [];
}

function surface(format, type, command, commandId) {
  return {
    format,
    type: type ?? null,
    command: command ?? null,
    commandId: commandId ?? null,
  };
}

function typeByName(schema, name) {
  const type = schema.types.find((entry) => entry.name === name);
  if (!type) throw new Error(`operation fixture type is missing: ${name}`);
  return type;
}

function unsignedBytes(schema, typeName, value) {
  const type = typeByName(schema, typeName);
  const width = { u8: 1, u16: 2, u32: 4, u64: 8 }[type.encoding];
  if (!width) throw new Error(`operation fixture integer is unsupported: ${typeName}`);
  let remaining = BigInt(value);
  const bytes = Buffer.alloc(width);
  for (let index = width - 1; index >= 0; index -= 1) {
    bytes[index] = Number(remaining & 0xffn);
    remaining >>= 8n;
  }
  return bytes;
}

function commandHeader(schema, commandName, flags = 0) {
  const command = schema.commands.find((entry) => entry.name === commandName);
  if (!command) throw new Error(`operation fixture command is missing: ${commandName}`);
  return Buffer.from([
    ...schema.protocol.magic,
    schema.protocol.wireMajor,
    command.id,
    flags,
  ]);
}

function prefixedText(schema, typeName, value) {
  const type = typeByName(schema, typeName);
  const bytes = Buffer.from(value, "utf8");
  return Buffer.concat([unsignedBytes(schema, type.lengthType.$ref, bytes.length), bytes]);
}

function applicationPayloadBytes(schema) {
  const body = Buffer.concat([
    prefixedText(schema, "text8", "fixture.packet"),
    prefixedText(schema, "text8", "application/octet-stream"),
    unsignedBytes(schema, "u32", 1),
    Buffer.from([0x7f]),
  ]);
  return Buffer.concat([Buffer.from([1]), unsignedBytes(schema, "u32", body.length), body]);
}

function operationCase(name, operation, rule, expect, caseSurface, bytes) {
  return {
    name,
    operation,
    rule,
    expect,
    surface: caseSurface,
    ...(Array.isArray(bytes)
      ? { framesHex: bytes.map((entry) => entry.toString("hex")) }
      : { hex: bytes.toString("hex") }),
  };
}

function buildOperationCases(schema) {
  const logical = schema.relocationLogicalStreamFormat;
  const logicalFixture = readJson(fixturePath(logical.goldenFixture));
  const unordered = structuredClone(logicalFixture.decoded);
  unordered.applicationStates.reverse();
  const tlv = typeByName(schema, "descriptor-extension");
  const unknownId = Math.max(...tlv.fields.map((field) => field.id)) + 1;
  const unknownBody = Buffer.from([0x7f]);
  const unknownItem = Buffer.concat([
    unsignedBytes(schema, tlv.fieldIdType.$ref, unknownId),
    unsignedBytes(schema, tlv.fieldLengthType.$ref, unknownBody.length),
    unknownBody,
  ]);
  const unknownTlv = Buffer.concat([
    unsignedBytes(schema, tlv.totalLengthType.$ref, unknownItem.length),
    unknownItem,
  ]);
  const missingRequiredTlv = unsignedBytes(schema, tlv.totalLengthType.$ref, 0);
  const text = typeByName(schema, "text8");
  const invalidUtf8Body = Buffer.from([0xc3, 0x28]);
  const invalidUtf8 = Buffer.concat([
    unsignedBytes(schema, text.lengthType.$ref, invalidUtf8Body.length),
    invalidUtf8Body,
  ]);
  const nulText = Buffer.concat([
    unsignedBytes(schema, text.lengthType.$ref, 1),
    Buffer.from([0]),
  ]);
  const flags = new Map(schema.flags.map((flag) => [flag.name, flag.bit]));
  const successTerminal = typeByName(schema, "request-terminal-result").values
    .find((entry) => entry.value === 0).name;
  const nonzeroFailure = typeByName(schema, "framework-error-code").values
    .find((entry) => entry.value !== 0).name;
  const durable = schema.durableFormats[0];
  const durableFixture = readJson(fixturePath(durable.goldenFixture));
  const envelope = encodeGoldenEnvelope(durable, durableFixture.decoded);
  const invalidFlags = Buffer.from(envelope);
  const flagsOffset = durable.magic.length + 1;
  const declaredFlags = Buffer.from(unsignedBytes(schema, durable.flagsType.$ref, durable.flags));
  declaredFlags[declaredFlags.length - 1] ^= 1;
  declaredFlags.copy(invalidFlags, flagsOffset);
  const invalidChecksum = Buffer.from(envelope);
  invalidChecksum[invalidChecksum.length - 1] ^= 1;
  const trailing = Buffer.concat([envelope, Buffer.from([0])]);
  return [
    operationCase(
      "vector-ordering",
      "constraint",
      "sorted",
      "reject",
      surface(logical.name, logical.body.$ref),
      encodeGoldenBody(logical.name, unordered),
    ),
    operationCase(
      "tlv-unknown-non-empty-skip",
      "tlv32",
      tlv.unknownField,
      "accept",
      surface("type", tlv.name),
      unknownTlv,
    ),
    operationCase(
      "tlv-required-field-presence",
      "tlv32",
      "required-field",
      "reject",
      surface("type", tlv.name),
      missingRequiredTlv,
    ),
    operationCase("invalid-utf8", "text-validation", "strict-utf-8", "reject",
      surface("type", text.name), invalidUtf8),
    operationCase("nul-text", "text-validation", "nul-forbidden", "reject",
      surface("type", text.name), nulText),
    operationCase(
      "flag-implication",
      "flag-constraint",
      "all-or-none",
      "reject",
      surface("command", null, "actorSend", 24),
      [commandHeader(schema, "actorSend", flags.get("boundSession"))],
    ),
    operationCase(
      "metadata-frame-required",
      "metadata-flag-frame",
      "frame-required",
      "reject",
      surface("command", null, "nodeSend", 16),
      [commandHeader(schema, "nodeSend", flags.get("metadata")), applicationPayloadBytes(schema)],
    ),
    {
      name: "terminal-predicate",
      operation: "runtime-predicate",
      rule: "terminal-failure-integrity",
      expect: "reject",
      surface: surface("semantic", null, "reply", 20),
      input: { terminalResult: successTerminal, failureCode: nonzeroFailure },
    },
    operationCase("durable-flags", "durable-header", "flags-exact", "reject",
      surface(durable.name, durable.body.$ref), invalidFlags),
    operationCase("durable-checksum", "checksum", durable.checksum.algorithm, "reject",
      surface(durable.name, durable.body.$ref), invalidChecksum),
    operationCase("durable-trailing", "bounded-reader", "trailing-forbidden", "reject",
      surface(durable.name, durable.body.$ref), trailing),
  ];
}

function makeEntry(kind, goldenFixture, schemaSurface, fixture) {
  if (kind !== "command") {
    assertEncodedBody(schemaSurface.format, fixture);
  }
  const sourcePath = fixturePath(goldenFixture);
  return {
    kind,
    goldenFixture,
    goldenSha256: sha256(sourcePath),
    surface: schemaSurface,
    canonical: canonicalCases(fixture),
    malformed: malformedCases(fixture),
  };
}

function buildIndex(schema, schemaPath) {
  validateSchema(schema);
  validateGoldenFixtures(schema, schemaPath);
  const oracles = validateServiceWireFixtureOracles(schema, schemaPath);
  const fixtures = [];

  for (const format of schema.durableFormats) {
    const fixture = readJson(fixturePath(format.goldenFixture));
    fixtures.push(makeEntry(
      "durable",
      format.goldenFixture,
      surface(format.name, format.body?.$ref),
      fixture,
    ));
  }

  const logical = oracles.logical;
  const logicalFixture = readJson(fixturePath(logical.goldenFixture));
  fixtures.push(makeEntry(
    "logical",
    logical.goldenFixture,
    surface(logical.format, logical.type),
    logicalFixture,
  ));

  for (const commandFixture of [...oracles.commands].sort((left, right) => (
    left.commandId - right.commandId
  ))) {
    const fixture = readJson(fixturePath(commandFixture.goldenFixture));
    fixtures.push(makeEntry(
      "command",
      commandFixture.goldenFixture,
      surface(
        commandFixture.format,
        null,
        commandFixture.command,
        commandFixture.commandId,
      ),
      fixture,
    ));
  }

  return {
    schema: "service-wire-v1",
    version: 2,
    fixtures,
    operationCases: buildOperationCases(schema),
  };
}

function renderIndex(index) {
  return `${JSON.stringify(index, null, 2)}\n`;
}

function validateIndexReferences(index) {
  if (index === null || typeof index !== "object" || !Array.isArray(index.fixtures)) {
    throw new Error("fixture index must contain a fixtures array");
  }
  for (const [indexNumber, entry] of index.fixtures.entries()) {
    const location = `fixture index fixtures[${indexNumber}]`;
    const sourcePath = fixturePath(entry.goldenFixture);
    if (!fs.existsSync(sourcePath)) {
      throw new Error(`${location}: golden file does not exist: ${entry.goldenFixture}`);
    }
    if (typeof entry.goldenSha256 !== "string") {
      throw new Error(`${location}: goldenSha256 is required`);
    }
    const actualHash = sha256(sourcePath);
    if (actualHash !== entry.goldenSha256) {
      throw new Error(
        `${location}: golden hash mismatch for ${entry.goldenFixture}`
        + ` (index ${entry.goldenSha256}, actual ${actualHash})`,
      );
    }
  }
  if (!Array.isArray(index.operationCases) || index.operationCases.length === 0) {
    throw new Error("fixture index must contain operationCases");
  }
  const operationKinds = new Set(OPERATION_KINDS);
  const caseNames = new Set();
  for (const [caseIndex, entry] of index.operationCases.entries()) {
    if (!operationKinds.has(entry.operation) || !["accept", "reject"].includes(entry.expect)) {
      throw new Error(`fixture index operationCases[${caseIndex}] is invalid`);
    }
    if (caseNames.has(entry.name)) {
      throw new Error(`fixture index operation case is duplicated: ${entry.name}`);
    }
    caseNames.add(entry.name);
  }
}

function run(mode, schemaArgument, outputArgument) {
  const schemaPath = path.resolve(schemaArgument);
  const indexPath = path.resolve(outputArgument);
  const schema = readJson(schemaPath);
  if (mode === "write") {
    const index = buildIndex(schema, schemaPath);
    fs.writeFileSync(indexPath, renderIndex(index), "utf8");
    const rejected = index.operationCases.filter((entry) => entry.expect === "reject").length;
    console.log(`service wire fixture catalog written: ${index.fixtures.length} fixtures, ${rejected} malformed operation vectors`);
    return;
  }

  if (!fs.existsSync(indexPath)) {
    throw new Error(`fixture index does not exist: ${path.relative(protocolDirectory, indexPath)}`);
  }
  const actualText = fs.readFileSync(indexPath, "utf8");
  const actualIndex = JSON.parse(actualText);
  validateIndexReferences(actualIndex);
  const expectedText = renderIndex(buildIndex(schema, schemaPath));
  if (actualText !== expectedText) {
    throw new Error(`service wire fixture catalog drift detected: ${indexPath}`);
  }
  const rejected = actualIndex.operationCases.filter((entry) => entry.expect === "reject").length;
  console.log(`service wire fixture catalog valid: ${actualIndex.fixtures.length} fixtures, ${rejected} malformed operation vectors`);
}

if (process.argv[1] && scriptPath === path.resolve(process.argv[1])) {
  const [modeArgument, schemaArgument, outputArgument, ...extraArguments] = process.argv.slice(2);
  if (!["--write", "--check"].includes(modeArgument) || !schemaArgument || !outputArgument
      || extraArguments.length > 0) {
    console.error("usage: generate-service-wire-fixtures.mjs --write|--check <schema-path> <output-path>");
    process.exit(2);
  }
  try {
    run(modeArgument === "--write" ? "write" : "check", schemaArgument, outputArgument);
  } catch (error) {
    console.error(error.stack ?? String(error));
    process.exit(1);
  }
}

export {
  buildIndex,
  renderIndex,
  validateIndexReferences,
};

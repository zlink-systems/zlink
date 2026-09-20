#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { OPERATION_KINDS } from "./service-wire-lowering.mjs";
import {
  crc32c,
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
  const width = { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[type.encoding];
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

function encodeSchemaValue(schema, typeName, value) {
  const type = typeByName(schema, typeName);
  if (type.kind === "integer") {
    return unsignedBytes(schema, typeName, value);
  }
  if (type.kind === "enum") {
    const selected = type.values.find((entry) => entry.name === value);
    if (!selected) throw new Error(`operation fixture enum value is missing: ${typeName}.${value}`);
    return unsignedBytes(schema, typeName, selected.value);
  }
  if (type.kind === "length-prefixed-text") {
    if (value === null && type.zeroLengthMeaning === "absent") {
      return unsignedBytes(schema, type.lengthType.$ref, 0);
    }
    return prefixedText(schema, typeName, value);
  }
  if (type.kind === "length-prefixed-bytes") {
    const bytes = Buffer.from(value);
    return Buffer.concat([
      unsignedBytes(schema, type.lengthType.$ref, bytes.length),
      bytes,
    ]);
  }
  if (type.kind === "struct") {
    return encodeSchemaFields(schema, type.fields, value);
  }
  if (type.kind === "vector") {
    return Buffer.concat([
      unsignedBytes(schema, type.countType.$ref, value.length),
      ...value.map((entry) => encodeSchemaValue(schema, type.item.$ref, entry)),
    ]);
  }
  if (type.kind === "versioned-vector") {
    return Buffer.concat(type.layout.flatMap((field) => {
      if (field.kind === "repeat") {
        return value.map((entry) => encodeSchemaValue(schema, field.item.$ref, entry));
      }
      const fieldValue = Object.hasOwn(field, "constant") ? field.constant : value.length;
      return [encodeSchemaValue(schema, field.$ref, fieldValue)];
    }));
  }
  if (type.kind === "versioned-length-delimited") {
    const body = encodeSchemaFields(schema, type.body, value);
    return Buffer.concat([
      encodeSchemaValue(schema, type.version.$ref, type.version.constant),
      encodeSchemaValue(schema, type.length.$ref, body.length),
      body,
    ]);
  }
  if (type.kind === "conditional-union") {
    const selected = type.cases.find((entry) => Object.entries(entry.when)
      .every(([name, expected]) => value[name] === expected));
    if (!selected) throw new Error(`operation fixture union case is missing: ${typeName}`);
    const discriminators = type.discriminators.flatMap((discriminator) => (
      discriminator.source === "wire"
        ? [encodeSchemaValue(schema, discriminator.$ref, value[discriminator.name])]
        : []
    ));
    const body = encodeSchemaFields(schema, selected.fields, value);
    return Buffer.concat([
      ...discriminators,
      ...(type.bodyLengthType
        ? [encodeSchemaValue(schema, type.bodyLengthType.$ref, body.length)]
        : []),
      body,
    ]);
  }
  throw new Error(`operation fixture schema encoder does not support ${typeName}:${type.kind}`);
}

function encodeSchemaFields(schema, fields, value) {
  return Buffer.concat(fields.flatMap((field) => {
    if (field.when?.fieldPresent !== undefined && value[field.when.fieldPresent] === null) {
      return [];
    }
    if (field.when?.fieldEquals !== undefined
        && value[field.when.fieldEquals.name] !== field.when.fieldEquals.value) {
      return [];
    }
    return [encodeSchemaValue(schema, field.$ref, value[field.name])];
  }));
}

function encodeCommandFrame(schema, commandName, flags, value) {
  const command = schema.commands.find((entry) => entry.name === commandName);
  if (!command) throw new Error(`operation fixture command is missing: ${commandName}`);
  const body = Buffer.concat(command.body.flatMap((field) => {
    if (field.when?.allFlagsSet !== undefined) {
      const bits = new Map(schema.flags.map((entry) => [entry.name, entry.bit]));
      const present = field.when.allFlagsSet.every((name) => (flags & bits.get(name)) !== 0);
      if (!present) return [];
    }
    return [encodeSchemaValue(schema, field.$ref, value[field.name])];
  }));
  return Buffer.concat([commandHeader(schema, commandName, flags), body]);
}

function byteRecipe(parts) {
  const hash = crypto.createHash("sha256");
  let encodedBytes = 0;
  const segments = parts.map((part) => {
    if (Buffer.isBuffer(part)) {
      hash.update(part);
      encodedBytes += part.length;
      return { hex: part.toString("hex") };
    }
    const block = Buffer.alloc(Math.min(part.count, 65536), part.byte);
    let remaining = part.count;
    while (remaining > 0) {
      const length = Math.min(remaining, block.length);
      hash.update(block.subarray(0, length));
      remaining -= length;
    }
    encodedBytes += part.count;
    return { repeatByte: part.byte, count: part.count };
  });
  return { segments, encodedBytes, sha256: hash.digest("hex") };
}

function authorityKey(schema, identity) {
  const format = schema.authorityKeyFormat;
  const variant = identity.objectKind === "actor"
    ? { kind: "actor", component: identity.actor.actorId }
    : { kind: "spot", component: identity.spot?.spotId ?? identity.spotId };
  const discriminator = format.kindDiscriminators.find(
    (entry) => entry.objectKind === variant.kind,
  );
  const bytes = Buffer.from(variant.component, "utf8");
  const escaped = [...bytes].map((byte) => (
    (byte >= 0x41 && byte <= 0x5a)
      || (byte >= 0x61 && byte <= 0x7a)
      || (byte >= 0x30 && byte <= 0x39)
      || [0x2d, 0x2e, 0x5f, 0x7e].includes(byte)
      ? String.fromCharCode(byte)
      : `%${byte.toString(16).toUpperCase().padStart(2, "0")}`
  )).join("");
  return [format.prefix, discriminator.wire, String(bytes.length), escaped]
    .join(format.separator);
}

function tlvItem(schema, type, field, value) {
  const body = encodeSchemaValue(schema, field.$ref, value);
  return Buffer.concat([
    unsignedBytes(schema, type.fieldIdType.$ref, field.id),
    unsignedBytes(schema, type.fieldLengthType.$ref, body.length),
    body,
  ]);
}

function tlvEnvelope(schema, type, items) {
  const body = Buffer.concat(items);
  return Buffer.concat([
    unsignedBytes(schema, type.totalLengthType.$ref, body.length),
    body,
  ]);
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

function operationCase(name, operation, rule, expect, caseSurface, bytes, details = {}) {
  return {
    name,
    operation,
    rule,
    expect,
    surface: caseSurface,
    ...(Array.isArray(bytes)
      ? { framesHex: bytes.map((entry) => entry.toString("hex")) }
      : { hex: bytes.toString("hex") }),
    ...details,
  };
}

function boundaryCase(operation, expect, rule, caseSurface, encoded, details = {}) {
  const name = `boundary-${operation}-${expect}`;
  const base = encoded?.chunks !== undefined
    ? {
      name,
      operation,
      rule,
      expect,
      surface: caseSurface,
      chunksHex: encoded.chunks.map((chunk) => chunk.toString("hex")),
      ...details,
    }
    : Buffer.isBuffer(encoded) || Array.isArray(encoded)
    ? operationCase(name, operation, rule, expect, caseSurface, encoded, details)
    : {
      name,
      operation,
      rule,
      expect,
      surface: caseSurface,
      byteRecipe: encoded,
      ...details,
    };
  return { ...base, boundaryPair: operation };
}

function buildOperationCases(schema) {
  const logical = schema.relocationLogicalStreamFormat;
  const logicalFixture = readJson(fixturePath(logical.goldenFixture));
  const unordered = structuredClone(logicalFixture.decoded);
  unordered.applicationStates.reverse();
  const tlv = typeByName(schema, "descriptor-extension");
  const requiredValues = new Map([
    ["runtimeState", "serving"],
    ["applicationVersion", 0],
    ["protocolCapabilities", [schema.protocol.requiredCapability]],
    ["objectRole", "none"],
    ["placementWeight", 1],
    ["activeCapacityLimit", 1],
    ["pendingCapacityLimit", 0],
    ["activeCapacityUsed", 0],
    ["pendingCapacityUsed", 0],
  ]);
  const requiredFields = tlv.fields.filter((field) => field.required);
  const requiredItems = requiredFields.map((field) => tlvItem(
    schema,
    tlv,
    field,
    requiredValues.get(field.name),
  ));
  const unknownId = Math.max(...tlv.fields.map((field) => field.id)) + 1;
  const unknownBody = Buffer.from([0x7f]);
  const unknownItem = Buffer.concat([
    unsignedBytes(schema, tlv.fieldIdType.$ref, unknownId),
    unsignedBytes(schema, tlv.fieldLengthType.$ref, unknownBody.length),
    unknownBody,
  ]);
  const unknownTlv = tlvEnvelope(schema, tlv, [...requiredItems, unknownItem]);
  const omittedRequiredField = requiredFields.at(-1);
  const unknownWithMissingRequired = tlvEnvelope(schema, tlv, [
    ...requiredItems.slice(0, -1),
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
  invalidFlags.writeUInt32BE(
    crc32c(invalidFlags.subarray(0, invalidFlags.length - 4)),
    invalidFlags.length - 4,
  );
  const invalidChecksum = Buffer.from(envelope);
  invalidChecksum[invalidChecksum.length - 1] ^= 1;
  const trailing = Buffer.concat([envelope, Buffer.from([0])]);
  const invalidReplayCursor = structuredClone(durableFixture.decoded);
  invalidReplayCursor.activationRecoveryState.replayCursor = (
    BigInt(invalidReplayCursor.activationRecoveryState.inboxSequence) + 1n
  ).toString();
  const invalidActivationRecovery = encodeGoldenEnvelope(durable, invalidReplayCursor);
  const duplicateMetadataKeys = encodeSchemaValue(schema, "metadata-frame", [
    { key: "A", value: "first" },
    { key: "B", value: "second" },
    { key: "A", value: "third" },
  ]);
  const negotiatedPayload = encodeSchemaValue(
    schema,
    "application-payload-bytes",
    Buffer.from([0x01, 0x02, 0x03, 0x04]),
  );
  const negotiatedEnvelope = applicationPayloadBytes(schema);
  const boolAccept = encodeSchemaValue(schema, "bool8", "true");
  const boolReject = unsignedBytes(schema, "bool8", 2);
  const integerAccept = encodeSchemaValue(
    schema,
    "application-version",
    9223372036854775807n,
  );
  const integerReject = unsignedBytes(schema, "application-version", -1n);
  const ridAccept = encodeSchemaValue(schema, "rid", Buffer.from("r"));
  const ridReject = unsignedBytes(schema, "u8", 0);
  const bomText = Buffer.concat([
    unsignedBytes(schema, "u8", 4),
    Buffer.from([0xef, 0xbb, 0xbf, 0x61]),
  ]);
  const overlongText = Buffer.concat([
    unsignedBytes(schema, "u8", 2),
    Buffer.from([0xc0, 0xaf]),
  ]);
  const absentActor = encodeSchemaValue(schema, "optional-actor-ref", { actorId: null });
  const absentActorWithGeneration = Buffer.concat([
    absentActor,
    encodeSchemaValue(schema, "nonzero-u64", 1),
  ]);
  const actorRef = encodeSchemaValue(schema, "actor-ref", {
    actorId: "a",
    objectGeneration: 1,
  });
  const sortedText = encodeSchemaValue(schema, "sorted-text8-vector", ["aa", "z"]);
  const truncatedVector = Buffer.concat([
    encodeSchemaValue(schema, "u16", 2),
    encodeSchemaValue(schema, "text8", "aa"),
  ]);
  const metadataVector = encodeSchemaValue(schema, "metadata-frame", [
    { key: "A", value: "first" },
    { key: "B", value: "second" },
  ]);
  const invalidMetadataVersion = Buffer.from(metadataVector);
  invalidMetadataVersion[0] = 2;
  const actorIdentity = {
    objectKind: "actor",
    actor: { actorId: "a", objectGeneration: 1 },
    expectedAuthorityOwnerGeneration: 1,
  };
  const encodedActorIdentity = encodeSchemaValue(
    schema,
    "relocation-object-identity",
    actorIdentity,
  );
  const invalidUnionBody = Buffer.concat([
    encodedActorIdentity.subarray(0, 1),
    unsignedBytes(
      schema,
      "u16",
      encodedActorIdentity.readUInt16BE(1) + 1,
    ),
    encodedActorIdentity.subarray(3),
    Buffer.from([0]),
  ]);
  const canonicalIdentityLong = {
    objectKind: "actor",
    actor: { actorId: "aaaaaaaaaa", objectGeneration: 1 },
    expectedAuthorityOwnerGeneration: 1,
  };
  const canonicalIdentityShort = {
    objectKind: "actor",
    actor: { actorId: "zz", objectGeneration: 1 },
    expectedAuthorityOwnerGeneration: 1,
  };
  const participant = (object) => ({
    object,
    expectedStoreVersion: "v",
    mutation: Buffer.from([1]),
  });
  const canonicalParticipants = [
    participant(canonicalIdentityLong),
    participant(canonicalIdentityShort),
  ];
  const canonicalKeys = canonicalParticipants.map((entry) => authorityKey(schema, entry.object));
  const canonicalParticipantInput = canonicalParticipants.map((entry) => ({
    object: entry.object,
    expectedStoreVersion: entry.expectedStoreVersion,
    mutationHex: entry.mutation.toString("hex"),
  }));
  const canonicalVector = encodeSchemaValue(
    schema,
    "aggregate-participant-vector",
    canonicalParticipants,
  );
  const reversedCanonicalVector = encodeSchemaValue(
    schema,
    "aggregate-participant-vector",
    [...canonicalParticipants].reverse(),
  );
  const validNodeSendFrames = [commandHeader(schema, "nodeSend"), negotiatedEnvelope];
  const invalidCommandHeader = Buffer.from(validNodeSendFrames[0]);
  invalidCommandHeader[0] ^= 1;
  const invalidNodeSendFlags = commandHeader(schema, "nodeSend", flags.get("extension"));
  const actorSendValue = {
    operation: { high: 1, low: 1 },
    messageFollowHopCount: 0,
    sourceActor: { actorId: null },
    targetActor: {
      actor: { actorId: "a", objectGeneration: 1 },
      targetNodeRid: Buffer.from("n"),
      targetNodeGeneration: 1,
      expectedAuthorityOwnerGeneration: 1,
      expectedOwnerLeaseGeneration: 1,
    },
  };
  const actorSendAccept = encodeCommandFrame(schema, "actorSend", 0, actorSendValue);
  const actorSendReject = encodeCommandFrame(
    schema,
    "actorSend",
    flags.get("boundSession"),
    actorSendValue,
  );
  const descriptorValue = Object.fromEntries(requiredValues);
  const encodedDescriptor = tlvEnvelope(schema, tlv, requiredItems);
  const tlvLimitRecipe = (totalBytes) => {
    const totalLengthBytes = 4;
    const unknownHeaderBytes = 5;
    const repeatedBytes = totalBytes - totalLengthBytes
      - requiredItems.reduce((sum, item) => sum + item.length, 0)
      - unknownHeaderBytes;
    const unknownHeader = Buffer.concat([
      unsignedBytes(schema, tlv.fieldIdType.$ref, unknownId),
      unsignedBytes(schema, tlv.fieldLengthType.$ref, repeatedBytes),
    ]);
    return byteRecipe([
      unsignedBytes(schema, tlv.totalLengthType.$ref, totalBytes - totalLengthBytes),
      ...requiredItems,
      unknownHeader,
      { byte: 0x7f, count: repeatedBytes },
    ]);
  };
  const payloadMiB = 1024 * 1024;
  const largePayloadRecipe = byteRecipe([
    unsignedBytes(schema, "u32", payloadMiB),
    { byte: 0x7f, count: payloadMiB },
  ]);
  const logicalBytes = encodeGoldenBody(logical.name, logicalFixture.decoded);
  const splitPoints = [1, Math.floor(logicalBytes.length / 2), logicalBytes.length - 1];
  const logicalChunks = splitPoints.reduce((result, point, index) => {
    const start = index === 0 ? 0 : splitPoints[index - 1];
    result.push(logicalBytes.subarray(start, point));
    if (index === splitPoints.length - 1) result.push(logicalBytes.subarray(point));
    return result;
  }, []);
  const boundaryCases = [
    boundaryCase("integer", "accept", "signed-maximum", surface("type", "application-version"), integerAccept, { directions: ["encode", "decode"] }),
    boundaryCase("integer", "reject", "negative-forbidden", surface("type", "application-version"), integerReject, { directions: ["decode"] }),
    boundaryCase("enum", "accept", "known-value", surface("type", "bool8"), boolAccept, { directions: ["encode", "decode"] }),
    boundaryCase("enum", "reject", "unknown-value", surface("type", "bool8"), boolReject, { directions: ["decode"] }),
    boundaryCase("length-prefixed", "accept", "minimum-bytes", surface("type", "rid"), ridAccept, { directions: ["encode", "decode"] }),
    boundaryCase("length-prefixed", "reject", "below-minimum-bytes", surface("type", "rid"), ridReject, { directions: ["decode"] }),
    boundaryCase("text-validation", "accept", "bom-preserved", surface("type", "text8"), bomText, { directions: ["encode", "decode"], decoded: "\ufeffa" }),
    boundaryCase("text-validation", "reject", "overlong-utf-8", surface("type", "text8"), overlongText, { directions: ["decode"] }),
    boundaryCase("field", "accept", "absent-field-sentinel", surface("type", "optional-actor-ref"), absentActor, { directions: ["encode", "decode"], input: { actorId: null, generation: null } }),
    boundaryCase("field", "reject", "field-forbidden-when-absent", surface("type", "optional-actor-ref"), absentActorWithGeneration, { directions: ["decode"] }),
    boundaryCase("struct", "accept", "sequential-complete", surface("type", "actor-ref"), actorRef, { directions: ["encode", "decode"] }),
    boundaryCase("struct", "reject", "sequential-truncated", surface("type", "actor-ref"), actorRef.subarray(0, actorRef.length - 1), { directions: ["decode"] }),
    boundaryCase("vector", "accept", "count-matches-items", surface("type", "sorted-text8-vector"), sortedText, { directions: ["encode", "decode"] }),
    boundaryCase("vector", "reject", "count-exceeds-items", surface("type", "sorted-text8-vector"), truncatedVector, { directions: ["decode"] }),
    boundaryCase("versioned-vector", "accept", "version-and-count", surface("type", "metadata-frame"), metadataVector, { directions: ["encode", "decode"] }),
    boundaryCase("versioned-vector", "reject", "version-mismatch", surface("type", "metadata-frame"), invalidMetadataVersion, { directions: ["decode"] }),
    boundaryCase("bounded-reader", "accept", "exact-boundary", surface(durable.name, durable.body.$ref), envelope, { directions: ["decode"] }),
    boundaryCase("bounded-reader", "reject", "trailing-byte", surface(durable.name, durable.body.$ref), trailing, { directions: ["decode"] }),
    boundaryCase("versioned-length-delimited", "accept", "version-and-body-length", surface("type", "application-payload-envelope-v1"), negotiatedEnvelope, { directions: ["encode", "decode"] }),
    boundaryCase("versioned-length-delimited", "reject", "version-mismatch", surface("type", "application-payload-envelope-v1"), Buffer.concat([Buffer.from([2]), negotiatedEnvelope.subarray(1)]), { directions: ["decode"] }),
    boundaryCase("discriminator", "accept", "variant-tag-agreement", surface("type", "relocation-object-identity"), encodedActorIdentity, { directions: ["encode", "decode"] }),
    boundaryCase("discriminator", "reject", "variant-tag-mismatch", surface("type", "relocation-object-identity"), encodedActorIdentity, { directions: ["encode"], input: { ...actorIdentity, objectKind: "userSpot", variant: "actor" } }),
    boundaryCase("conditional-union", "accept", "selected-case-complete", surface("type", "relocation-object-identity"), encodedActorIdentity, { directions: ["encode", "decode"] }),
    boundaryCase("conditional-union", "reject", "selected-case-trailing", surface("type", "relocation-object-identity"), invalidUnionBody, { directions: ["decode"] }),
    boundaryCase("tlv32", "accept", "required-fields-present", surface("type", tlv.name), encodedDescriptor, { directions: ["encode", "decode"], input: descriptorValue }),
    boundaryCase("tlv32", "reject", "required-field-missing", surface("type", tlv.name), missingRequiredTlv, { directions: ["decode"] }),
    boundaryCase("constraint", "accept", "canonical-authority-key-order", surface("type", "aggregate-participant-vector"), canonicalVector, { directions: ["encode", "decode"], comparisonKeys: canonicalKeys, input: canonicalParticipantInput }),
    boundaryCase("constraint", "reject", "canonical-authority-key-order", surface("type", "aggregate-participant-vector"), reversedCanonicalVector, { directions: ["encode", "decode"], comparisonKeys: [...canonicalKeys].reverse(), input: [...canonicalParticipantInput].reverse() }),
    boundaryCase("command-header", "accept", "magic-major-command", surface("command", null, "nodeSend", 16), validNodeSendFrames, { directions: ["decode"] }),
    boundaryCase("command-header", "reject", "magic-mismatch", surface("command", null, "nodeSend", 16), [invalidCommandHeader, negotiatedEnvelope], { directions: ["decode"] }),
    boundaryCase("flags", "accept", "allowed-flags", surface("command", null, "nodeSend", 16), validNodeSendFrames, { directions: ["encode", "decode"] }),
    boundaryCase("flags", "reject", "unknown-command-flag", surface("command", null, "nodeSend", 16), [invalidNodeSendFlags, negotiatedEnvelope], { directions: ["decode"] }),
    boundaryCase("flag-constraint", "accept", "all-or-none", surface("command", null, "actorSend", 24), [actorSendAccept, negotiatedEnvelope], { directions: ["encode", "decode"] }),
    boundaryCase("flag-constraint", "reject", "all-or-none", surface("command", null, "actorSend", 24), [actorSendReject, negotiatedEnvelope], { directions: ["decode"] }),
    boundaryCase("metadata-flag-frame", "accept", "frame-required", surface("command", null, "nodeSend", 16), [commandHeader(schema, "nodeSend", flags.get("metadata")), metadataVector, negotiatedEnvelope], { directions: ["encode", "decode"] }),
    boundaryCase("metadata-flag-frame", "reject", "frame-required", surface("command", null, "nodeSend", 16), [commandHeader(schema, "nodeSend", flags.get("metadata")), negotiatedEnvelope], { directions: ["decode"] }),
    boundaryCase("payload", "accept", "required-payload", surface("command", null, "nodeSend", 16), validNodeSendFrames, { directions: ["encode", "decode"] }),
    boundaryCase("payload", "reject", "required-payload-missing", surface("command", null, "nodeSend", 16), [commandHeader(schema, "nodeSend")], { directions: ["decode"] }),
    boundaryCase("durable-header", "accept", "exact-header", surface(durable.name, durable.body.$ref), envelope, { directions: ["encode", "decode"] }),
    boundaryCase("durable-header", "reject", "flags-exact", surface(durable.name, durable.body.$ref), invalidFlags, { directions: ["decode"] }),
    boundaryCase("checksum", "accept", "checksum-match-before-body", surface(durable.name, durable.body.$ref), envelope, { directions: ["decode"] }),
    boundaryCase("checksum", "reject", "checksum-mismatch", surface(durable.name, durable.body.$ref), invalidChecksum, { directions: ["decode"] }),
    boundaryCase("encoded-limit", "accept", "complete-tlv-encoded-maximum", surface("type", tlv.name), tlvLimitRecipe(1048576), { directions: ["decode"] }),
    boundaryCase("encoded-limit", "reject", "complete-tlv-encoded-over-maximum", surface("type", tlv.name), tlvLimitRecipe(1048580), { directions: ["decode"] }),
    boundaryCase("negotiated-bound", "accept", "measured-equals-context", surface("type", "application-payload-bytes"), negotiatedPayload, { directions: ["encode", "decode"], context: { effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 4 } }),
    boundaryCase("negotiated-bound", "reject", "measured-exceeds-context", surface("type", "application-payload-bytes"), negotiatedPayload, { directions: ["encode", "decode"], context: { effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 3 } }),
    boundaryCase("logical-stream", "accept", "mid-field-chunk-split", surface(logical.name, logical.body.$ref), { chunks: logicalChunks }, { directions: ["decode"], chunkSplit: "within-field" }),
    boundaryCase("logical-stream", "reject", "truncated-final-chunk", surface(logical.name, logical.body.$ref), { chunks: logicalChunks.slice(0, -1) }, { directions: ["decode"], final: true }),
    boundaryCase("runtime-predicate", "accept", "terminal-success-integrity", surface("semantic", null, "reply", 20), Buffer.alloc(0), { directions: ["encode", "decode"], input: { terminalResult: successTerminal, failureCode: "none" } }),
    boundaryCase("runtime-predicate", "reject", "terminal-failure-integrity", surface("semantic", null, "reply", 20), Buffer.alloc(0), { directions: ["encode", "decode"], input: { terminalResult: successTerminal, failureCode: nonzeroFailure } }),
  ];
  const additionalBoundaryCases = [
    operationCase("text-lone-surrogate", "text-validation", "lone-surrogate", "reject", surface("type", "text8"), Buffer.alloc(0), { directions: ["encode"], input: "\ud800" }),
    operationCase("text-surrogate-code-point", "text-validation", "surrogate-code-point", "reject", surface("type", "text8"), Buffer.concat([unsignedBytes(schema, text.lengthType.$ref, 3), Buffer.from([0xed, 0xa0, 0x80])]), { directions: ["decode"] }),
    operationCase("field-absent-with-internal-generation", "field", "internal-absence", "reject", surface("type", "optional-actor-ref"), absentActor, { directions: ["encode"], input: { actorId: null, generation: 1 } }),
    operationCase("constraint-utf8-length-independent-order", "constraint", "utf-8-bytes", "accept", surface("type", "sorted-text8-vector"), sortedText, { directions: ["encode", "decode"], input: ["aa", "z"] }),
    operationCase("tlv-encode-required-missing", "tlv32", "required-field", "reject", surface("type", tlv.name), encodedDescriptor, { directions: ["encode"], input: { ...descriptorValue, runtimeState: null } }),
    operationCase("tlv-encode-capability-missing", "constraint", "contains-protocol-required-capability", "reject", surface("type", tlv.name), encodedDescriptor, { directions: ["encode"], input: { ...descriptorValue, protocolCapabilities: ["other"] } }),
    operationCase("tlv-encode-field-range", "field", "maximum", "reject", surface("type", tlv.name), encodedDescriptor, { directions: ["encode"], input: { ...descriptorValue, placementWeight: 101 } }),
    {
      name: "length-prefixed-large-payload",
      operation: "length-prefixed",
      rule: "encode-through-declared-maximum",
      expect: "accept",
      surface: surface("type", "application-payload-bytes"),
      byteRecipe: largePayloadRecipe,
      directions: ["encode", "decode"],
      context: { effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: payloadMiB },
      input: { repeatByte: 0x7f, count: payloadMiB },
    },
  ];
  return [
    ...boundaryCases,
    ...additionalBoundaryCases,
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
    operationCase(
      "tlv-unknown-with-missing-required",
      "tlv32",
      `required-field:${omittedRequiredField.name}`,
      "reject",
      surface("type", tlv.name),
      unknownWithMissingRequired,
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
      [actorSendReject, negotiatedEnvelope],
    ),
    operationCase(
      "metadata-frame-required",
      "metadata-flag-frame",
      "frame-required",
      "reject",
      surface("command", null, "nodeSend", 16),
      [commandHeader(schema, "nodeSend", flags.get("metadata")), applicationPayloadBytes(schema)],
    ),
    operationCase(
      "conditional-union-case-constraint",
      "constraint",
      "field-less-than-or-equal:replayCursor:inboxSequence",
      "reject",
      surface(durable.name, durable.body.$ref),
      invalidActivationRecovery,
    ),
    operationCase(
      "metadata-nonadjacent-duplicate-key",
      "constraint",
      "unique:key:utf-8-bytes",
      "reject",
      surface("type", "metadata-frame"),
      duplicateMetadataKeys,
    ),
    operationCase(
      "client-server-negotiated-payload-bound",
      "negotiated-bound",
      "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead",
      "reject",
      surface("type", "application-payload-bytes"),
      negotiatedPayload,
      {
        directions: ["encode", "decode"],
        context: {
          effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 3,
        },
      },
    ),
    operationCase(
      "client-server-negotiated-envelope-bound",
      "negotiated-bound",
      "effectiveCompleteMessageBytes",
      "reject",
      surface("type", "application-payload-envelope-v1"),
      negotiatedEnvelope,
      {
        directions: ["encode", "decode"],
        context: {
          effectiveCompleteMessageBytes: negotiatedEnvelope.length - 1,
          effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 1,
        },
      },
    ),
    operationCase(
      "client-server-negotiated-context-missing",
      "negotiated-bound",
      "context-required",
      "reject",
      surface("type", "application-payload-bytes"),
      negotiatedPayload,
      { directions: ["encode", "decode"] },
    ),
    operationCase(
      "client-server-negotiated-context-negative",
      "negotiated-bound",
      "context-negative",
      "reject",
      surface("type", "application-payload-bytes"),
      negotiatedPayload,
      {
        directions: ["encode", "decode"],
        context: {
          effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: -1,
        },
      },
    ),
    operationCase(
      "client-server-negotiated-context-above-absolute-maximum",
      "negotiated-bound",
      "context-absolute-maximum",
      "reject",
      surface("type", "application-payload-bytes"),
      negotiatedPayload,
      {
        directions: ["encode", "decode"],
        context: {
          effectiveCompleteMessageBytesMinusActualEnvelopeOverhead: 4294966775,
        },
      },
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
    version: 3,
    fixtures,
    operationCases: buildOperationCases(schema),
  };
}

function renderIndex(index) {
  return `${JSON.stringify(index, null, 2)}\n`;
}

function validateIndexReferences(index) {
  if (index === null || typeof index !== "object" || index.version !== 3
      || !Array.isArray(index.fixtures)) {
    throw new Error("fixture index must use version 3 and contain a fixtures array");
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
  const boundaryCoverage = new Map(OPERATION_KINDS.map((operation) => [operation, new Set()]));
  for (const [caseIndex, entry] of index.operationCases.entries()) {
    if (!operationKinds.has(entry.operation) || !["accept", "reject"].includes(entry.expect)) {
      throw new Error(`fixture index operationCases[${caseIndex}] is invalid`);
    }
    if (caseNames.has(entry.name)) {
      throw new Error(`fixture index operation case is duplicated: ${entry.name}`);
    }
    if (entry.operation === "negotiated-bound"
        && JSON.stringify(entry.directions) !== JSON.stringify(["encode", "decode"])) {
      throw new Error(
        `fixture index operationCases[${caseIndex}] negotiated-bound directions are invalid`,
      );
    }
    if (entry.boundaryPair !== undefined) {
      if (entry.boundaryPair !== entry.operation) {
        throw new Error(`fixture index operationCases[${caseIndex}] boundary pair is invalid`);
      }
      boundaryCoverage.get(entry.operation).add(entry.expect);
    }
    caseNames.add(entry.name);
  }
  for (const [operation, expectations] of boundaryCoverage) {
    if (!expectations.has("accept") || !expectations.has("reject")) {
      throw new Error(`fixture index operation boundary pair is incomplete: ${operation}`);
    }
  }
}

function operationCountSummary(index) {
  return OPERATION_KINDS.map((operation) => {
    const entries = index.operationCases.filter((entry) => entry.operation === operation);
    return `${operation}:${entries.filter((entry) => entry.expect === "accept").length}`
      + `/${entries.filter((entry) => entry.expect === "reject").length}`;
  }).join(", ");
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
    console.log(`service wire operation accept/reject counts: ${operationCountSummary(index)}`);
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
  console.log(`service wire operation accept/reject counts: ${operationCountSummary(actualIndex)}`);
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

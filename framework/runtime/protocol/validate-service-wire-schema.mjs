#!/usr/bin/env node

import fs from "node:fs";
import crypto from "node:crypto";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const PAYLOAD_POLICIES = new Set(["forbidden", "optional", "required"]);
const COMMAND_DOMAINS = new Set(["application", "infrastructure"]);
const TYPE_KINDS = new Set([
  "integer",
  "enum",
  "length-prefixed-bytes",
  "length-prefixed-text",
  "struct",
  "versioned-vector",
  "conditional-union",
  "vector",
  "tlv32",
  "versioned-length-delimited",
]);
const INTEGER_ENCODINGS = new Map([
  ["u8", [0n, 255n]],
  ["u16", [0n, 65535n]],
  ["u32", [0n, 4294967295n]],
  ["u64", [0n, 18446744073709551615n]],
  ["i64", [-9223372036854775808n, 9223372036854775807n]],
]);
const VECTOR_COMPARISONS = new Set([
  "canonical-authority-key-bytes",
  "utf-8-bytes",
  "wire-value-then-utf-8-bytes",
  "unsigned-wire-value",
]);

class SchemaValidationError extends Error {
  constructor(errors) {
    super(`service wire schema validation failed (${errors.length} error(s))`);
    this.name = "SchemaValidationError";
    this.errors = errors;
  }
}

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function hasOwn(value, key) {
  return Object.prototype.hasOwnProperty.call(value, key);
}

function clone(value) {
  return structuredClone(value);
}

function toBigInt(value) {
  if (typeof value === "number" && Number.isSafeInteger(value)) {
    return BigInt(value);
  }
  if (typeof value === "string" && /^(0|[1-9][0-9]*)$/.test(value)) {
    return BigInt(value);
  }
  return null;
}

function validateSchema(schema) {
  const errors = [];
  const usedBounds = new Set();
  const fail = (location, message) => errors.push(`${location}: ${message}`);

  if (!isObject(schema)) {
    throw new SchemaValidationError(["$: root must be an object"]);
  }
  // Keep validation reusable by generators that retain the parsed schema.
  schema = clone(schema);

  walk(schema, "$", (value, location) => {
    if (typeof value === "number" && !Number.isSafeInteger(value)) {
      fail(location, "numeric literals must be safe integers; use a decimal string for 64-bit bounds");
    }
  });

  if (schema.schemaDialect !== "zlink-service-wire-schema-v1") {
    fail("$.schemaDialect", "must be zlink-service-wire-schema-v1");
  }
  if (schema.schemaVersion !== 1) {
    fail("$.schemaVersion", "must be 1");
  }

  validateProtocol(schema.protocol, fail);

  const bounds = buildNamedMap(schema.bounds, "bounds", "$.bounds", fail);
  for (const [name, entry] of bounds) {
    const value = toBigInt(entry.value);
    if (value === null || value <= 0n) {
      fail(`$.bounds[${entry.__index}].value`, `bound ${name} must be a positive safe integer or decimal string`);
    }
  }
  const amendmentBounds = new Map([
    ["creationIntentBytes", 1048576n],
    ["creationTerminalEnvelopeBytes", 1048576n],
    ["maintenanceAggregateParticipants", 1024n],
    ["relocationResourceParticipants", 2048n],
    ["maintenanceAggregateBytes", 1048576n],
    ["messageFollowHopCount", 8n],
    ["messageFollowMessages", 1024n],
    ["messageFollowBytes", 16777216n],
    ["routingIdCollisionAttempts", 8n],
    ["nodeActiveCapacityDefault", 10000n],
    ["nodePendingCapacityDefault", 128n],
    ["objectTypeCapacityMaximum", 2147483647n],
  ]);
  for (const [name, expected] of amendmentBounds) {
    const entry = bounds.get(name);
    if (entry === undefined) {
      fail("$.bounds", `missing amendment bound ${name}`);
    } else if (toBigInt(entry.value) !== expected) {
      fail(`$.bounds[${entry.__index}].value`,
        `bound ${name} must equal ${expected}`);
    }
  }

  const flags = buildNamedMap(schema.flags, "flags", "$.flags", fail);
  const flagBits = new Map();
  for (const [name, flag] of flags) {
    const location = `$.flags[${flag.__index}]`;
    if (!Number.isSafeInteger(flag.bit) || flag.bit <= 0 || flag.bit > 0xff
        || (flag.bit & (flag.bit - 1)) !== 0) {
      fail(`${location}.bit`, `flag ${name} must use one unique bit in the u8 range`);
    } else if (flagBits.has(flag.bit)) {
      fail(`${location}.bit`, `duplicates flag bit used by ${flagBits.get(flag.bit)}`);
    } else {
      flagBits.set(flag.bit, name);
    }
    if (hasOwn(flag, "frame")) {
      if (flag.whenSet !== "frame-required" || flag.whenClear !== "frame-forbidden") {
        fail(location, "a frame-bearing flag must require the frame when set and forbid it when clear");
      }
    }
  }

  const contexts = buildNamedMap(
    schema.semanticContexts,
    "semanticContexts",
    "$.semanticContexts",
    fail,
  );

  const types = buildNamedMap(schema.types, "types", "$.types", fail);
  for (const [name, type] of types) {
    validateType(name, type, types, bounds, contexts, fail);
  }

  walk(schema, "$", (value, location) => {
    if (!isObject(value)) {
      return;
    }
    if (hasOwn(value, "$ref")) {
      if (typeof value.$ref !== "string" || !types.has(value.$ref)) {
        fail(`${location}.$ref`, `undefined type ${JSON.stringify(value.$ref)}`);
      }
    }
    if (hasOwn(value, "$bound")) {
      if (typeof value.$bound !== "string" || !bounds.has(value.$bound)) {
        fail(`${location}.$bound`, `undefined bound ${JSON.stringify(value.$bound)}`);
      } else {
        usedBounds.add(value.$bound);
      }
    }
  });

  validateTypeCycles(types, fail);
  validateSemanticConstraints(schema.semanticConstraints, contexts, fail);
  validateCommands(schema.commands, schema.reservedCommandRanges, flags, contexts, types, bounds, fail);
  validateLivenessProfile(schema.livenessProfile, schema.commands, fail);
  validateFanoutLivenessProfile(schema.fanoutLivenessProfile, fail);
  validateAuthorityKeyFormat(schema.authorityKeyFormat, fail);
  validateAuthorityStoreAccessProfile(schema.authorityStoreAccessProfile, fail);
  validateDescriptorEnumerationProfile(schema.descriptorEnumerationProfile, fail);
  validateAuthorityStoreGenerationProfile(schema.authorityStoreGenerationProfile, fail);
  validateAuthorityStoreDurabilityProfile(schema.authorityStoreDurabilityProfile, fail);
  validateOwnerLeaseAuthorityProfile(schema.ownerLeaseAuthorityProfile, fail);
  validateRelocationRetentionPolicy(schema.relocationRetentionPolicy, fail);
  validateRelocationStorageProfile(schema.relocationStorageProfile, fail);
  validateFrameworkJsonV1Profile(schema.frameworkJsonV1Profile, fail);
  validateFrameworkMultipartV1Profile(schema.frameworkMultipartV1Profile, fail);
  validateMaintenanceAdmissionProfile(schema.maintenanceAdmissionProfile, fail);
  validateTerminationResultProfile(schema.terminationResultProfile, fail);
  validateDurableFormats(schema.durableFormats, types, bounds, fail);
  validateRelocationStateMachine(schema.relocationStateMachine, schema.commands, types, fail);
  validateServiceInvariants(schema, types, fail);

  for (const name of bounds.keys()) {
    if (!usedBounds.has(name)) {
      fail("$.bounds", `declared bound ${name} is not connected to any field`);
    }
  }

  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }

  return {
    bounds: bounds.size,
    flags: flags.size,
    types: types.size,
    commands: schema.commands.length,
  };
}

function validateProtocol(protocol, fail) {
  if (!isObject(protocol)) {
    fail("$.protocol", "must be an object");
    return;
  }
  if (!Array.isArray(protocol.magic) || protocol.magic.length !== 2
      || protocol.magic.some((value) => !Number.isSafeInteger(value) || value < 0 || value > 255)) {
    fail("$.protocol.magic", "must contain exactly two u8 values");
  }
  if (!Number.isSafeInteger(protocol.wireMajor) || protocol.wireMajor <= 0
      || protocol.wireMajor > 255) {
    fail("$.protocol.wireMajor", "must be a non-zero u8 value");
  }
  if (protocol.byteOrder !== "big-endian") {
    fail("$.protocol.byteOrder", "must be big-endian");
  }
  if (protocol.headPrefixBytes !== 5) {
    fail("$.protocol.headPrefixBytes", "must account for magic, major, command and flags");
  }
  if (typeof protocol.requiredCapability !== "string" || protocol.requiredCapability.length === 0) {
    fail("$.protocol.requiredCapability", "must be a non-empty string");
  }
}

function buildNamedMap(entries, label, location, fail) {
  const result = new Map();
  if (!Array.isArray(entries)) {
    fail(location, `${label} must be an array so duplicate names remain detectable`);
    return result;
  }
  entries.forEach((entry, index) => {
    if (!isObject(entry)) {
      fail(`${location}[${index}]`, "must be an object");
      return;
    }
    if (typeof entry.name !== "string" || entry.name.length === 0) {
      fail(`${location}[${index}].name`, "must be a non-empty string");
      return;
    }
    if (result.has(entry.name)) {
      fail(`${location}[${index}].name`, `duplicates ${entry.name}`);
      return;
    }
    Object.defineProperty(entry, "__index", {
      configurable: true,
      enumerable: false,
      value: index,
    });
    result.set(entry.name, entry);
  });
  return result;
}

function validateType(name, type, types, bounds, contexts, fail) {
  const location = `$.types[${type.__index}]`;
  if (!TYPE_KINDS.has(type.kind)) {
    fail(`${location}.kind`, `type ${name} has unknown kind ${JSON.stringify(type.kind)}`);
    return;
  }

  if (type.kind === "integer") {
    const minimum = resolveInteger(type.minimum, bounds);
    const maximum = resolveInteger(type.maximum, bounds);
    if (minimum === null || maximum === null || minimum > maximum) {
      fail(location, `integer type ${name} must have an ordered numeric range`);
    } else if (!INTEGER_ENCODINGS.has(type.encoding)) {
      fail(`${location}.encoding`, `integer type ${name} has unsupported encoding ${type.encoding}`);
    } else {
      const [encodingMinimum, encodingMaximum] = INTEGER_ENCODINGS.get(type.encoding);
      if (minimum < encodingMinimum || maximum > encodingMaximum) {
        fail(location, `integer type ${name} range does not fit ${type.encoding}`);
      }
    }
  }

  if (type.kind === "enum") {
    validateEnum(type, location, fail);
  }

  if (type.kind === "length-prefixed-bytes" || type.kind === "length-prefixed-text") {
    const minimum = resolveInteger(type.minimumBytes, bounds);
    const maximum = resolveInteger(type.maximumBytes, bounds);
    if (minimum === null || maximum === null || minimum < 0n || minimum > maximum) {
      fail(location, `length-prefixed type ${name} must have an ordered non-negative byte range`);
    }
    if (!isObject(type.lengthType) || typeof type.lengthType.$ref !== "string") {
      fail(`${location}.lengthType`, "must reference an integer length type");
    } else {
      validateLengthCapacity(type.lengthType.$ref, maximum, types, bounds,
        `${location}.maximumBytes`, fail);
    }
    if (type.kind === "length-prefixed-text"
        && (type.encoding !== "utf-8" || type.nul !== "forbidden")) {
      fail(location, "text types must require valid UTF-8 and reject NUL");
    }
  }

  if (type.kind === "struct") {
    validateFields(type.fields, `${location}.fields`, contexts, null, types, bounds, fail);
    validateStructConstraints(type, location, fail);
  }

  if (type.kind === "vector") {
    if (!isObject(type.countType) || !isObject(type.item)) {
      fail(location, `vector type ${name} requires countType and item references`);
    } else {
      const maximumItems = hasOwn(type, "maximumItems")
        ? resolveInteger(type.maximumItems, bounds)
        : resolveReferencedMaximum(type.countType.$ref, types, bounds);
      if (maximumItems === null || maximumItems < 0n) {
        fail(`${location}.maximumItems`, "must be a non-negative integer or bound");
      } else {
        validateLengthCapacity(type.countType.$ref, maximumItems, types, bounds,
          `${location}.maximumItems`, fail);
      }
      validateVectorConstraints(type, location, types, fail);
    }
  }

  if (type.kind === "versioned-vector") {
    if (!Array.isArray(type.layout) || type.layout.length !== 3) {
      fail(`${location}.layout`, "versioned-vector must declare version, count and repeated entries");
    }
    if (type.trailingBytes !== "forbidden") {
      fail(`${location}.trailingBytes`, "versioned-vector must reject trailing bytes");
    }
    validateVectorConstraints(type, location, types, fail, "entries");
  }

  if (type.kind === "versioned-length-delimited") {
    if (!isObject(type.version) || !hasOwn(type.version, "constant")) {
      fail(`${location}.version`, "must define an encoded constant version");
    }
    if (!isObject(type.length) || type.length.covers !== "body") {
      fail(`${location}.length`, "must delimit the complete body");
    }
    validateNamedValue(type.version?.$ref, type.version?.constant, types,
      `${location}.version.constant`, fail);
    if (typeof type.length?.$ref !== "string"
        || resolveReferencedMaximum(type.length.$ref, types, bounds) === null) {
      fail(`${location}.length`, "must reference an integer length type");
    }
    validateFields(type.body, `${location}.body`, contexts, null, types, bounds, fail);
    if (typeof type.length?.$ref === "string" && hasOwn(type, "maximumEncodedBytes")) {
      validateLengthCapacity(type.length.$ref, resolveInteger(type.maximumEncodedBytes, bounds),
        types, bounds, `${location}.maximumEncodedBytes`, fail);
    }
    if (type.trailingBytes !== "forbidden") {
      fail(`${location}.trailingBytes`, "length-delimited type must reject trailing bytes");
    }
  }

  if (type.kind === "conditional-union") {
    validateConditionalUnion(type, location, types, bounds, contexts, fail);
  }

  if (type.kind === "tlv32") {
    validateTlv(type, location, contexts, types, bounds, fail);
  }

  validateContainerCapacity(type, location, types, bounds, fail);

  validateConditions(type, location, contexts, null, fail);
}

function resolveInteger(value, bounds) {
  if (isObject(value) && typeof value.$bound === "string" && bounds.has(value.$bound)) {
    return toBigInt(bounds.get(value.$bound).value);
  }
  return toBigInt(value);
}

function resolveReferencedMaximum(typeName, types, bounds) {
  const type = types.get(typeName);
  if (!type || type.kind !== "integer") {
    return null;
  }
  return resolveInteger(type.maximum, bounds);
}

function validateLengthCapacity(typeName, requestedMaximum, types, bounds, location, fail) {
  const capacity = resolveReferencedMaximum(typeName, types, bounds);
  if (capacity === null) {
    fail(location, `length/count type ${typeName} must reference an integer type`);
    return;
  }
  if (requestedMaximum === null || requestedMaximum > capacity) {
    fail(location, `declared maximum does not fit ${typeName}`);
  }
}

function encodedIntegerBytes(typeName, types) {
  const encoding = types.get(typeName)?.encoding;
  return new Map([["u8", 1n], ["u16", 2n], ["u32", 4n], ["u64", 8n], ["i64", 8n]])
    .get(encoding) ?? null;
}

function sumFieldMaximums(fields, types, bounds, memo, visiting) {
  let total = 0n;
  for (const field of fields ?? []) {
    const maximum = encodedTypeMaximum(field.$ref, types, bounds, memo, visiting);
    if (maximum === null) {
      return null;
    }
    total += maximum;
  }
  return total;
}

function encodedTypeMaximum(typeName, types, bounds, memo = new Map(), visiting = new Set()) {
  if (memo.has(typeName)) {
    return memo.get(typeName);
  }
  if (visiting.has(typeName)) {
    return null;
  }
  const type = types.get(typeName);
  if (!type) {
    return null;
  }
  visiting.add(typeName);
  let maximum = null;
  if (type.kind === "integer" || type.kind === "enum") {
    maximum = encodedIntegerBytes(typeName, types);
  } else if (type.kind === "length-prefixed-bytes" || type.kind === "length-prefixed-text") {
    const prefix = encodedIntegerBytes(type.lengthType?.$ref, types);
    const content = resolveInteger(type.maximumBytes, bounds);
    maximum = prefix === null || content === null ? null : prefix + content;
  } else if (type.kind === "struct") {
    maximum = sumFieldMaximums(type.fields, types, bounds, memo, visiting);
  } else if (type.kind === "vector") {
    const prefix = encodedIntegerBytes(type.countType?.$ref, types);
    const count = hasOwn(type, "maximumItems")
      ? resolveInteger(type.maximumItems, bounds)
      : resolveReferencedMaximum(type.countType?.$ref, types, bounds);
    const item = encodedTypeMaximum(type.item?.$ref, types, bounds, memo, visiting);
    maximum = prefix === null || count === null || item === null ? null : prefix + count * item;
  } else if (type.kind === "versioned-vector") {
    const versionField = type.layout?.[0];
    const countField = type.layout?.[1];
    const repeatField = type.layout?.[2];
    const version = encodedTypeMaximum(versionField?.$ref, types, bounds, memo, visiting);
    const countBytes = encodedTypeMaximum(countField?.$ref, types, bounds, memo, visiting);
    const count = resolveReferencedMaximum(countField?.$ref, types, bounds);
    const item = encodedTypeMaximum(repeatField?.item?.$ref, types, bounds, memo, visiting);
    maximum = [version, countBytes, count, item].some((value) => value === null)
      ? null : version + countBytes + count * item;
  } else if (type.kind === "versioned-length-delimited") {
    const version = encodedTypeMaximum(type.version?.$ref, types, bounds, memo, visiting);
    const length = encodedTypeMaximum(type.length?.$ref, types, bounds, memo, visiting);
    const body = sumFieldMaximums(type.body, types, bounds, memo, visiting);
    maximum = version === null || length === null || body === null ? null : version + length + body;
    const explicitMaximum = resolveInteger(type.maximumEncodedBytes, bounds);
    if (maximum !== null && explicitMaximum !== null && maximum > explicitMaximum) {
      maximum = explicitMaximum;
    }
  } else if (type.kind === "conditional-union") {
    let prefix = 0n;
    for (const discriminator of type.discriminators ?? []) {
      if (discriminator.source === "wire") {
        const size = encodedTypeMaximum(discriminator.$ref, types, bounds, memo, visiting);
        if (size === null) {
          prefix = null;
          break;
        }
        prefix += size;
      }
    }
    const length = hasOwn(type, "bodyLengthType")
      ? encodedTypeMaximum(type.bodyLengthType?.$ref, types, bounds, memo, visiting)
      : 0n;
    let largestCase = 0n;
    for (const unionCase of type.cases ?? []) {
      const size = sumFieldMaximums(unionCase.fields, types, bounds, memo, visiting);
      if (size === null) {
        largestCase = null;
        break;
      }
      if (size > largestCase) {
        largestCase = size;
      }
    }
    maximum = prefix === null || length === null || largestCase === null
      ? null : prefix + length + largestCase;
    const explicitMaximum = resolveInteger(type.maximumEncodedBytes, bounds);
    if (maximum !== null && explicitMaximum !== null && maximum > explicitMaximum) {
      maximum = explicitMaximum;
    }
  } else if (type.kind === "tlv32") {
    const totalLength = encodedTypeMaximum(type.totalLengthType?.$ref, types, bounds, memo, visiting);
    const fieldId = encodedTypeMaximum(type.fieldIdType?.$ref, types, bounds, memo, visiting);
    const fieldLength = encodedTypeMaximum(type.fieldLengthType?.$ref, types, bounds, memo, visiting);
    let body = 0n;
    for (const field of type.fields ?? []) {
      const value = encodedTypeMaximum(field.$ref, types, bounds, memo, visiting);
      if (fieldId === null || fieldLength === null || value === null) {
        body = null;
        break;
      }
      body += fieldId + fieldLength + value;
    }
    maximum = totalLength === null || body === null ? null : totalLength + body;
  }
  visiting.delete(typeName);
  const explicitMaximum = resolveInteger(type.maximumEncodedBytes, bounds);
  if (maximum !== null && explicitMaximum !== null && maximum > explicitMaximum) {
    maximum = explicitMaximum;
  }
  if (maximum !== null) {
    memo.set(typeName, maximum);
  }
  return maximum;
}

function validateContainerCapacity(type, location, types, bounds, fail) {
  if (type.kind === "conditional-union" && isObject(type.bodyLengthType)) {
    const capacity = resolveReferencedMaximum(type.bodyLengthType.$ref, types, bounds);
    const explicitMaximum = resolveInteger(type.maximumEncodedBytes, bounds);
    let prefixBytes = 0n;
    for (const discriminator of type.discriminators ?? []) {
      if (discriminator.source !== "wire") {
        continue;
      }
      const size = encodedTypeMaximum(discriminator.$ref, types, bounds);
      if (size === null) {
        prefixBytes = null;
        break;
      }
      prefixBytes += size;
    }
    const lengthBytes = encodedTypeMaximum(type.bodyLengthType.$ref, types, bounds);
    const explicitBodyMaximum = explicitMaximum === null || prefixBytes === null || lengthBytes === null
      ? null : explicitMaximum - prefixBytes - lengthBytes;
    for (const [index, unionCase] of (type.cases ?? []).entries()) {
      const body = sumFieldMaximums(unionCase.fields, types, bounds, new Map(), new Set());
      if (capacity !== null && body !== null && body > capacity && explicitBodyMaximum === null) {
        fail(`${location}.cases[${index}]`, "selected case maximum does not fit body length prefix");
      }
      if (explicitBodyMaximum !== null && explicitBodyMaximum > capacity) {
        fail(`${location}.maximumEncodedBytes`, "explicit maximum does not fit body length prefix");
      }
    }
  }
  if (type.kind === "tlv32") {
    const fieldCapacity = resolveReferencedMaximum(type.fieldLengthType?.$ref, types, bounds);
    const totalCapacity = resolveReferencedMaximum(type.totalLengthType?.$ref, types, bounds);
    const idBytes = encodedIntegerBytes(type.fieldIdType?.$ref, types);
    const lengthBytes = encodedIntegerBytes(type.fieldLengthType?.$ref, types);
    let total = 0n;
    for (const [index, field] of (type.fields ?? []).entries()) {
      const value = encodedTypeMaximum(field.$ref, types, bounds);
      if (fieldCapacity !== null && value !== null && value > fieldCapacity) {
        fail(`${location}.fields[${index}]`, "TLV value maximum does not fit field length prefix");
      }
      if (value !== null && idBytes !== null && lengthBytes !== null) {
        total += idBytes + lengthBytes + value;
      }
    }
    if (totalCapacity !== null && total > totalCapacity) {
      fail(location, "TLV maximum fields do not fit total length prefix");
    }
  }
}

function fieldPathExists(type, fieldPath, types) {
  const parts = fieldPath.split(".");
  let current = type;
  for (const part of parts) {
    if (!current || current.kind !== "struct" || !Array.isArray(current.fields)) {
      return false;
    }
    const field = current.fields.find((candidate) => candidate.name === part);
    if (!field || typeof field.$ref !== "string") {
      return false;
    }
    current = types.get(field.$ref);
  }
  return true;
}

function validateVectorConstraints(type, location, types, fail, repeatedFieldName = null) {
  let itemRef = type.item?.$ref;
  if (repeatedFieldName !== null && Array.isArray(type.layout)) {
    const repeated = type.layout.find((field) => field.name === repeatedFieldName);
    itemRef = repeated?.item?.$ref;
  }
  const itemType = types.get(itemRef);
  if (!itemType) {
    return;
  }
  if (!Array.isArray(type.constraints)) {
    return;
  }
  const signatures = new Set();
  type.constraints.forEach((constraint, index) => {
    const constraintLocation = `${location}.constraints[${index}]`;
    if (!isObject(constraint) || !["sorted", "unique"].includes(constraint.kind)) {
      fail(constraintLocation, "vector constraint must be sorted or unique");
      return;
    }
    const paths = typeof constraint.field === "string"
      ? [constraint.field]
      : constraint.fields;
    if (paths !== undefined) {
      if (!Array.isArray(paths) || paths.length === 0 || new Set(paths).size !== paths.length) {
        fail(constraintLocation, "field list must contain unique paths");
      } else {
        for (const fieldPath of paths) {
          if (typeof fieldPath !== "string" || !fieldPathExists(itemType, fieldPath, types)) {
            fail(constraintLocation, `references unknown item field ${fieldPath}`);
          }
        }
      }
    }
    if (constraint.kind === "sorted" && !VECTOR_COMPARISONS.has(constraint.comparison)) {
      fail(`${constraintLocation}.comparison`, "sorted constraint needs a canonical comparison");
    }
    const signature = `${constraint.kind}:${JSON.stringify(paths ?? [])}`;
    if (signatures.has(signature)) {
      fail(constraintLocation, "duplicates vector constraint");
    }
    signatures.add(signature);
  });
}

function validateStructConstraints(type, location, fail) {
  if (!Array.isArray(type.constraints)) {
    return;
  }
  const fields = new Set((type.fields ?? []).map((field) => field.name));
  type.constraints.forEach((constraint, index) => {
    const constraintLocation = `${location}.constraints[${index}]`;
    if (constraint.kind === "not-both-zero") {
      if ((type.fields ?? []).length < 2) {
        fail(constraintLocation, "not-both-zero requires at least two fields");
      }
      if (hasOwn(constraint, "unless")
          && constraint.unless !== "one-way-record-without-terminal-completion") {
        fail(`${constraintLocation}.unless`, "unknown not-both-zero exception");
      }
      return;
    }
    if (constraint.kind === "field-less-than-or-equal") {
      if (!fields.has(constraint.left) || !fields.has(constraint.right)) {
        fail(constraintLocation, "comparison must reference fields in the same struct");
      }
      return;
    }
    fail(constraintLocation, `unknown struct constraint ${JSON.stringify(constraint.kind)}`);
  });
}

function validateEnum(type, location, fail) {
  if (!Array.isArray(type.values) || type.values.length === 0) {
    fail(`${location}.values`, "enum must declare at least one value");
    return;
  }
  const names = new Set();
  const values = new Set();
  const encodingMaximum = new Map([
    ["u8", 255n],
    ["u16", 65535n],
    ["u32", 4294967295n],
    ["u64", 18446744073709551615n],
  ]).get(type.encoding);
  if (encodingMaximum === undefined) {
    fail(`${location}.encoding`, "enum encoding must be u8, u16, u32 or u64");
  }
  type.values.forEach((entry, index) => {
    const itemLocation = `${location}.values[${index}]`;
    if (!isObject(entry) || typeof entry.name !== "string" || entry.name.length === 0) {
      fail(itemLocation, "enum entry must have a non-empty name");
      return;
    }
    if (!Number.isSafeInteger(entry.value) || entry.value < 0) {
      fail(`${itemLocation}.value`, "enum value must be a non-negative safe integer");
    } else if (encodingMaximum !== undefined && BigInt(entry.value) > encodingMaximum) {
      fail(`${itemLocation}.value`, `enum value does not fit ${type.encoding}`);
    }
    if (names.has(entry.name)) {
      fail(`${itemLocation}.name`, `duplicates enum name ${entry.name}`);
    }
    if (values.has(entry.value)) {
      fail(`${itemLocation}.value`, `duplicates enum value ${entry.value}`);
    }
    names.add(entry.name);
    values.add(entry.value);
  });
}

function validateFields(fields, location, contexts, allowedFlags, types, bounds, fail) {
  if (!Array.isArray(fields)) {
    fail(location, "fields must be an array");
    return;
  }
  const names = new Set();
  const priorFields = new Map();
  fields.forEach((field, index) => {
    const fieldLocation = `${location}[${index}]`;
    if (!isObject(field) || typeof field.name !== "string" || field.name.length === 0) {
      fail(fieldLocation, "field must have a non-empty name");
      return;
    }
    if (names.has(field.name)) {
      fail(`${fieldLocation}.name`, `duplicates field ${field.name}`);
    }
    if (!hasOwn(field, "$ref") && !hasOwn(field, "kind")) {
      fail(fieldLocation, "encoded field must reference a type or declare an inline kind");
    }
    if (isObject(field.when) && typeof field.when.fieldPresent === "string"
        && !names.has(field.when.fieldPresent)) {
      fail(`${fieldLocation}.when.fieldPresent`, "must reference an earlier field");
    }
    if (isObject(field.when) && isObject(field.when.fieldEquals)
        && !names.has(field.when.fieldEquals.name)) {
      fail(`${fieldLocation}.when.fieldEquals.name`, "must reference an earlier field");
    }
    if (isObject(field.when) && isObject(field.when.fieldEquals)) {
      const referenced = priorFields.get(field.when.fieldEquals.name);
      validateNamedValue(referenced?.$ref, field.when.fieldEquals.value, types,
        `${fieldLocation}.when.fieldEquals.value`, fail);
    }
    if (hasOwn(field, "constant")) {
      validateNamedValue(field.$ref, field.constant, types, `${fieldLocation}.constant`, fail);
    }
    if (typeof field.$ref === "string" && (hasOwn(field, "minimum") || hasOwn(field, "maximum"))) {
      const referenced = types.get(field.$ref);
      if (referenced?.kind !== "integer") {
        fail(fieldLocation, "numeric range override requires an integer field type");
      } else {
        const minimum = resolveInteger(field.minimum ?? referenced.minimum, bounds);
        const maximum = resolveInteger(field.maximum ?? referenced.maximum, bounds);
        const baseMinimum = resolveInteger(referenced.minimum, bounds);
        const baseMaximum = resolveInteger(referenced.maximum, bounds);
        if (minimum === null || maximum === null || minimum > maximum
            || minimum < baseMinimum || maximum > baseMaximum) {
          fail(fieldLocation, "field range must be ordered and contained by its referenced type");
        }
      }
    }
    validateFieldConstraints(field, fieldLocation, types, fail);
    names.add(field.name);
    priorFields.set(field.name, field);
    if (hasOwn(field, "when") && field.otherwise !== "forbidden") {
      fail(fieldLocation, "a conditional encoded field must explicitly forbid the field otherwise");
    }
    validateConditions(field, fieldLocation, contexts, allowedFlags, fail);
  });
}

function validateFieldConstraints(field, location, types, fail) {
  if (!hasOwn(field, "constraints")) {
    return;
  }
  if (!Array.isArray(field.constraints) || field.constraints.length === 0) {
    fail(`${location}.constraints`, "field constraints must be a non-empty array");
    return;
  }
  const signatures = new Set();
  for (const [index, constraint] of field.constraints.entries()) {
    const constraintLocation = `${location}.constraints[${index}]`;
    if (!isObject(constraint) || constraint.kind !== "contains-protocol-required-capability") {
      fail(constraintLocation, `unknown field constraint ${JSON.stringify(constraint?.kind)}`);
      continue;
    }
    if (field.$ref !== "sorted-text8-vector") {
      fail(constraintLocation, "capability constraint requires sorted-text8-vector");
    }
    if (signatures.has(constraint.kind)) {
      fail(constraintLocation, `duplicates field constraint ${constraint.kind}`);
    }
    signatures.add(constraint.kind);
  }
}

function validateNamedValue(typeName, value, types, location, fail) {
  if (typeof typeName !== "string") {
    return;
  }
  const type = types.get(typeName);
  if (type?.kind === "enum") {
    const matches = type.values.some((entry) => entry.name === value || entry.value === value);
    if (!matches) {
      fail(location, `value ${JSON.stringify(value)} is outside ${typeName}`);
    }
  } else if (type?.kind === "integer" && toBigInt(value) === null) {
    fail(location, `value ${JSON.stringify(value)} is not an integer`);
  }
}

function validateConditionalUnion(type, location, types, bounds, contexts, fail) {
  if (!Array.isArray(type.discriminators) || type.discriminators.length === 0) {
    fail(`${location}.discriminators`, "conditional union needs a discriminator");
    return;
  }
  const discriminators = new Map();
  let hasWireDiscriminator = false;
  type.discriminators.forEach((entry, index) => {
    const itemLocation = `${location}.discriminators[${index}]`;
    if (!isObject(entry) || typeof entry.name !== "string" || entry.name.length === 0) {
      fail(itemLocation, "discriminator must have a non-empty name");
      return;
    }
    if (discriminators.has(entry.name)) {
      fail(`${itemLocation}.name`, `duplicates discriminator ${entry.name}`);
    }
    discriminators.set(entry.name, entry);
    hasWireDiscriminator ||= entry.source === "wire";
    if (isObject(entry.source) && hasOwn(entry.source, "context")
        && !contexts.has(entry.source.context)) {
      fail(`${itemLocation}.source.context`, `undefined semantic context ${entry.source.context}`);
    }
  });
  if (!Array.isArray(type.cases) || type.cases.length === 0) {
    fail(`${location}.cases`, "conditional union must define cases");
  } else {
    const signatures = new Set();
    type.cases.forEach((entry, index) => {
      const caseLocation = `${location}.cases[${index}]`;
      if (!isObject(entry) || !isObject(entry.when)) {
        fail(caseLocation, "case must define a when object");
        return;
      }
      const keys = Object.keys(entry.when).sort();
      const expected = [...discriminators.keys()].sort();
      if (JSON.stringify(keys) !== JSON.stringify(expected)) {
        fail(`${caseLocation}.when`, "case must assign every discriminator exactly once");
      }
      const signature = conditionSignature(entry.when);
      if (signatures.has(signature)) {
        fail(`${caseLocation}.when`, "duplicates a conditional union case");
      }
      signatures.add(signature);
      for (const [key, value] of Object.entries(entry.when)) {
        const discriminator = discriminators.get(key);
        if (!discriminator || typeof discriminator.$ref !== "string") {
          continue;
        }
        const enumType = types.get(discriminator.$ref);
        if (enumType?.kind === "enum"
            && !enumType.values.some((candidate) => candidate.name === value)) {
          fail(`${caseLocation}.when.${key}`, `unknown ${discriminator.$ref} value ${value}`);
        }
      }
      validateFields(entry.fields, `${caseLocation}.fields`, contexts, null, types, bounds, fail);
    });

    if (type.otherwise === "protocol-error") {
      const domains = [];
      let combinationCount = 1;
      for (const [name, discriminator] of discriminators) {
        const enumType = types.get(discriminator.$ref);
        if (enumType?.kind !== "enum") {
          fail(`${location}.discriminators`, "a closed wire union must use enum discriminators");
          combinationCount = 0;
          break;
        }
        const values = enumType.values.map((entry) => entry.name);
        domains.push({ name, values });
        combinationCount *= values.length;
      }
      if (combinationCount > 4096) {
        fail(`${location}.discriminators`, "closed union discriminator product exceeds validation limit");
      } else if (combinationCount > 0) {
        for (const combination of discriminatorCombinations(domains)) {
          if (!signatures.has(conditionSignature(combination))) {
            fail(`${location}.cases`, `missing closed union case ${conditionSignature(combination)}`);
          }
        }
      }
    }
  }
  if (!hasOwn(type, "otherwise")) {
    fail(`${location}.otherwise`, "conditional union must define the unmatched discriminator result");
  } else if (isObject(type.otherwise)) {
    validateFields(type.otherwise.fields, `${location}.otherwise.fields`, contexts, null,
      types, bounds, fail);
  } else if (type.otherwise !== "protocol-error") {
    fail(`${location}.otherwise`, "must be protocol-error or an explicit field list");
  }
  if (type.trailingBytes !== "forbidden") {
    fail(`${location}.trailingBytes`, "conditional union must reject trailing bytes");
  }
  if (hasWireDiscriminator && type.otherwise === "protocol-error"
      && (!isObject(type.bodyLengthType) || typeof type.bodyLengthType.$ref !== "string"
          || type.bodyLengthCovers !== "selected-case")) {
    fail(location, "closed wire union must length-delimit the selected case body");
  }
}

function conditionSignature(value) {
  return JSON.stringify(
    Object.fromEntries(Object.entries(value).sort(([left], [right]) => left.localeCompare(right))),
  );
}

function discriminatorCombinations(domains, index = 0, current = {}) {
  if (index === domains.length) {
    return [{ ...current }];
  }
  const output = [];
  const domain = domains[index];
  for (const value of domain.values) {
    current[domain.name] = value;
    output.push(...discriminatorCombinations(domains, index + 1, current));
  }
  delete current[domain.name];
  return output;
}

function validateTlv(type, location, contexts, types, bounds, fail) {
  if (!Array.isArray(type.fields)) {
    fail(`${location}.fields`, "TLV fields must be an array so duplicate IDs remain detectable");
    return;
  }
  const ids = new Set();
  const names = new Set();
  let previousId = 0;
  type.fields.forEach((field, index) => {
    const fieldLocation = `${location}.fields[${index}]`;
    if (!isObject(field) || !Number.isSafeInteger(field.id) || field.id <= 0 || field.id > 255) {
      fail(`${fieldLocation}.id`, "TLV field ID must be in 1..255");
      return;
    }
    if (ids.has(field.id)) {
      fail(`${fieldLocation}.id`, `duplicates TLV field ID ${field.id}`);
    }
    ids.add(field.id);
    if (field.id <= previousId) {
      fail(`${fieldLocation}.id`, "TLV fields must be declared in ascending field ID order");
    }
    previousId = field.id;
    if (typeof field.name !== "string" || field.name.length === 0) {
      fail(`${fieldLocation}.name`, "TLV field needs a non-empty name");
    } else if (names.has(field.name)) {
      fail(`${fieldLocation}.name`, `duplicates TLV field ${field.name}`);
    } else {
      names.add(field.name);
    }
    if (typeof field.required !== "boolean") {
      fail(`${fieldLocation}.required`, "must be true or false");
    }
    if (typeof field.$ref !== "string" || !types.has(field.$ref)) {
      fail(`${fieldLocation}.$ref`, "must reference a declared type");
    }
    validateFieldConstraints(field, fieldLocation, types, fail);
  });

  if (Array.isArray(type.presenceRules)) {
    type.presenceRules.forEach((rule, index) => {
      const ruleLocation = `${location}.presenceRules[${index}]`;
      if (!isObject(rule) || !isObject(rule.when)) {
        fail(ruleLocation, "presence rule must define a condition");
        return;
      }
      validateConditions(rule.when, `${ruleLocation}.when`, contexts, null, fail);
      for (const key of ["require", "forbid"]) {
        if (!hasOwn(rule, key)) {
          continue;
        }
        if (!Array.isArray(rule[key]) || new Set(rule[key]).size !== rule[key].length) {
          fail(`${ruleLocation}.${key}`, "must be an array of unique field names");
          continue;
        }
        for (const fieldName of rule[key]) {
          if (!names.has(fieldName)) {
            fail(`${ruleLocation}.${key}`, `references undefined TLV field ${fieldName}`);
          }
        }
      }
      const required = new Set(rule.require ?? []);
      for (const fieldName of rule.forbid ?? []) {
        if (required.has(fieldName)) {
          fail(ruleLocation, `field ${fieldName} is both required and forbidden`);
        }
      }
    });
  }
  if (type.duplicateField !== "protocol-error" || type.trailingBytes !== "forbidden") {
    fail(location, "TLV must reject duplicate fields and trailing bytes");
  }
  if (type.encodingOrder !== "ascending-field-id") {
    fail(`${location}.encodingOrder`, "TLV encoding order must be ascending-field-id");
  }
  for (const lengthMember of ["totalLengthType", "fieldLengthType"]) {
    const ref = type[lengthMember]?.$ref;
    if (typeof ref !== "string" || resolveReferencedMaximum(ref, types, bounds) === null) {
      fail(`${location}.${lengthMember}`, "must reference an integer type");
    }
  }
}

function validateTypeCycles(types, fail) {
  const edges = new Map();
  for (const [name, type] of types) {
    const refs = new Set();
    walk(type, `type:${name}`, (value) => {
      if (isObject(value) && typeof value.$ref === "string" && types.has(value.$ref)) {
        refs.add(value.$ref);
      }
    });
    edges.set(name, refs);
  }
  const state = new Map();
  const stack = [];
  const visit = (name) => {
    const current = state.get(name) ?? 0;
    if (current === 2) {
      return;
    }
    if (current === 1) {
      const start = stack.indexOf(name);
      fail("$.types", `recursive type cycle: ${[...stack.slice(start), name].join(" -> ")}`);
      return;
    }
    state.set(name, 1);
    stack.push(name);
    for (const dependency of edges.get(name) ?? []) {
      visit(dependency);
    }
    stack.pop();
    state.set(name, 2);
  };
  for (const name of types.keys()) {
    visit(name);
  }
}

function validateSemanticConstraints(constraints, contexts, fail) {
  if (!Array.isArray(constraints)) {
    fail("$.semanticConstraints", "must be an array");
    return;
  }
  const seenKinds = new Set();
  const expectedExact = new Map([
    ["authority-operation-state-integrity", {
      authorityType: "authority-payload-v1",
      rules: [
        {
          operationKind: "steady",
          objectKind: "actor",
          relocationState: "absent",
          activationRecoveryState: "absent",
        },
        {
          operationKind: "steady",
          objectKind: "spot",
          spotKinds: ["entry", "user"],
          relocationState: "absent",
          activationRecoveryState: "absent",
        },
        {
          operationKind: "steady",
          objectKind: "spot",
          spotKind: "instance",
          instanceAuthorityStates: ["ready"],
          relocationState: "absent",
          activationRecoveryState:
            "optional-until-durable-first-handler-terminal-and-replay-cursor-update",
        },
        {
          operationKind: "coldActivation",
          objectKind: "spot",
          spotKind: "instance",
          relocationState: "absent",
          authorityStates: ["coldActivating"],
          activationRecoveryState: "absent",
        },
        {
          operationKind: "maintenanceRelocation",
          objectKinds: ["actor", "spot"],
          spotKinds: ["user", "instance"],
          relocationState: "present",
          relocationPhase: "non-none",
          userAuthorityStates: ["relocating"],
          instanceAuthorityStates: ["relocating"],
          instancePhaseStates: {
            relocating: [
              "preparing", "captured", "prepared", "committed", "activating",
              "activated", "cleaning", "completed", "aborted",
            ],
          },
          activationRecoveryState: "absent",
        },
        {
          operationKind: "close",
          objectKind: "spot",
          spotKinds: ["entry", "user", "instance"],
          relocationState: "absent",
          authorityStates: ["closing"],
          activationRecoveryState: "absent",
        },
      ],
    }],
    ["spot-authority-aggregate-integrity", {
      authorityObjectType: "authority-object-identity",
      canonicalKeyKind: "spot",
      canonicalKeyComponents: ["spotId"],
      spotKinds: ["entry", "user", "instance"],
      singleAuthoritativeRow: true,
      typedSpotLocation: "framework-decoded-projection-not-separate-authority",
      createTransitions: {
        entry: "new-object-cas",
        user: "new-object-cas",
        instance: "new-object-cas",
      },
      sharedGeneration: "provider-object-generation-per-canonical-spot-key",
      kindTransition: "requires-prior-row-delete-then-new-object-cas-on-same-key",
      providerPayloadInterpretation: "forbidden",
    }],
    ["relocation-journal-integrity", {
      relocationType: "relocation-envelope-v1",
      progressField: "participantProgress",
      journalField: "journal",
      completionField: "terminalCompletions",
      participantField: "participantId",
      sequenceField: "sequence",
      acceptedBoundaryField: "acceptedBoundary",
      operationField: "operationId",
      participantMustExistInProgress: true,
      sequenceAtOrBelowAcceptedBoundary: true,
      terminalCompletionMustMatchRequestRecord: true,
      requestReplayBinding: "operation-id-exact-request-source-fence-and-reply-route-id-are-one-immutable-accepted-record-identity",
      operationIdRole: "dedupe-identity-never-a-reply-route-substitute",
      authorityRelocationAtomicity: {
        mutationEvents: ["completion-append", "replyRelayAck", "requestSourceLeaseExpired"],
        writeOrder: "new-immutable-relocation-root-then-one-expected-store-version-authority-cas",
        casFields: ["relocation-reference", "relocation-checksum", "terminalCompletionCount", "pendingRelayCount"],
        terminalCompletionCount: "equals-referenced-relocation-terminal-completion-entry-count",
        pendingRelayCount: "equals-referenced-relocation-deliveryState-pending-count",
        completedGate: "accepted-request-count-equals-terminal-completion-count-and-pending-relay-count-zero",
        deliveryStateTransition: "pending-to-terminalReceived-or-alreadyTerminal-or-sourceLeaseExpired-only",
        mismatch: "recovery-error-and-completed-forbidden",
      },
      requestRecordKinds: [
        "nodeRequest", "channelRequest", "spotRequest", "actorRequest", "instanceSpotActivation",
      ],
      instanceRequestDiscriminator: "request",
    }],
    ["location-relocation-storage-integrity", {
      locationAuthorityOwns: [
        "descriptor-and-owner-lease",
        "object-authority-and-reservation",
        "aggregate-generation-and-canonical-participant-mutations",
        "inventory-digest-and-relocation-reference",
        "participant-replay-cursors-and-terminal-completion-count",
      ],
      relocationRootRole: "immutable-payload-lookup-projection-never-authority",
      publicationOrder: [
        "write-all-immutable-relocation-chunks",
        "write-immutable-relocation-root-manifest",
        "read-and-verify-complete-root-from-relocation-provider",
        "one-location-expected-store-version-cas-publishes-reference-checksum-replay-and-completion-counts",
      ],
      aggregateAuthority: {
        format: "maintenance-aggregate-v1",
        participantOrder: "canonical-authority-key-bytes",
        atomicFields: [
          "aggregateId",
          "aggregateGeneration",
          "participants-and-mutations",
          "inventoryDigestSha256",
          "relocationReference-and-checksum",
        ],
        relocationManifestDigest: "must-equal-location-authority-inventory-digest",
        manifestAuthority: "forbidden",
      },
      replacement: "write-and-verify-new-root-before-one-location-authority-cas-replaces-reference-and-counts",
      orphanCleanup: "unpublished-or-replaced-root-is-not-authority-and-is-deleted-or-expires",
      deleteOrder: "release-or-replace-location-authority-reference-before-idempotent-relocation-root-delete",
      backend: "location-and-relocation-providers-may-use-different-backends-connections-and-failure-domains",
      clock: "retention-and-renewal-use-relocation-provider-storeNow-and-expiresAt-only",
      publishedRelocationDataLoss: {
        closedTerminalTriggers: [
          "permanent-published-payload-missing",
          "published-payload-checksum-mismatch",
          "published-payload-inventory-digest-mismatch",
        ],
        failureCode: "relocationDataLost",
        retriable: false,
        rollback: "forbidden",
        publication: "Ready-and-Completed-forbidden",
      },
    }],
    ["reply-relay-context-integrity", {
      command: "replyRelay",
      coldActivation: {
        readyBarrierAuthorityState: "ready",
        readyBarrierOperationKind: "steady",
        activationFailureAuthorityState: "coldActivating",
        activationFailureOperationKind: "coldActivation",
        activationFailureForbidsTerminalResult: "ok",
      },
      maintenanceRelocation: {
        requiresRelocationId: true,
        requiresTargetAttemptGenerationAsPeerFence: true,
        requiresParticipantSequence: true,
      },
      acknowledgement: {
        command: "replyRelayAck",
        scope: "maintenanceRelocation-only",
        identity: ["stable-relocation-id", "exact-request-source-fence", "operation-id", "reply-route-id"],
        requestSource: "authenticated-exact-source-owner-lease-node-rid-and-generation",
        statuses: ["terminalReceived", "alreadyTerminal"],
        targetPersistence: "relocation-root-cas-before-ack-effect",
        targetRetry: "retransmit-same-terminal-across-connection-replacement-until-ack-or-exact-request-source-owner-lease-expiry",
        sourceDuplicate: "reply-already-terminal-still-emits-alreadyTerminal-ack",
        completionGate: "pending-relay-count-zero-and-each-accepted-request-has-authenticated-ack-or-store-confirmed-exact-request-source-owner-lease-expiry",
        physicalConnectionClose: "never-terminal-proof",
        retireWhileSourceLeaseValid: "forceStopped-and-retain-relocation-root-and-reply-bytes-for-retention-window",
      },
      terminalOwnership: "stable-relocation-id-exact-request-source-fence-operation-id-and-reply-route-id-once-independent-of-target-attempt",
      replyRoute: "request-and-ack-echo-only",
    }],
    ["instance-placement-authority-fence", {
      routeType: "instance-route-v1",
      requestBoundFields: [
        "targetNodeRid",
        "targetNodeGeneration",
        "targetSpotId",
        "ready.authority-or-coldActivation.stableType-and-targetDescriptorVersion",
        "coldActivation.deadlineUnixMs",
      ],
      targetComparison: "exact-current-authority-before-mailbox-admission",
      sourceOrder: "resolve-select-target-and-submit-complete-first-message-activation-envelope-without-owner-claim-or-reservation",
      newObjectGenerations: "nonzero-object-and-authority-owner-generations-allocated-by-provider-cas",
      targetClaim: "target-rechecks-current-authority-persists-complete-activation-recovery-root-and-cas-winner-reserves-before-factory",
      durableActivationIntent: "coldActivating-row-owned-by-exact-target-host-with-provider-issued-pending-creation-projection",
      targetRecovery: "serving-gate-initial-authority-scan-and-background-bounded-reconcile-resume-owned-coldActivating-with-exact-reservation-and-complete-first-message-envelope",
      readyOrdering: "durable-activation-inbox-first-record-before-ready-commit-handler-behind-barrier-ready-retains-recovery-root-and-cursor-queue-head-restore-before-barrier-open",
      recoveryRelease: "durable-first-handler-terminal-completion-then-replay-cursor-equals-inbox-sequence-then-preserve-cas-release-never-queue-admission",
      orphanRule: "recovery-root-put-before-reserve-or-conflict-loser-is-unpublished-orphan-retention-or-idempotent-delete",
      activationRegistry: "object-key-object-generation-authority-owner-generation-and-owner-token-converge-late-submit-and-scan-to-one-local-barrier",
      staleTargetRecovery: "expected-store-version-newOwner-cas-preserves-object-generation-and-selects-eligible-owner",
      originalOperationAfterLostSubmit: "no-hidden-resubmit-caller-normal-timeout-or-failure",
      activationFailure: "seal-local-barrier-terminal-once-typed-request-failures-and-one-way-drop-events-then-exact-fenced-delete",
      failureDeleteFence: "same-store-version-object-generation-authority-owner-generation-owner-id-and-owner-lease-generation",
      failureDeleteAmbiguity: "exact-read-reconcile-registry-remains-failed-until-missing-confirmed",
      nextActivation: "only-next-caller-after-missing-may-newObject-claim-new-object-and-owner-generations",
      failureCrashRecovery: "owned-coldActivating-scan-may-retry-retry-safe-factory-before-delete-is-confirmed",
      deadline: "resolve-select-claim-and-outbound-admission-share-one-source-send-deadline",
      oneWay: "after-outbound-admission-does-not-wait-for-factory-or-ready",
      leaseDeadline: "store-time-to-local-monotonic-from-read-start",
      publicExposure: "forbidden",
    }],
    ["actor-route-authority-fence", {
      routeType: "actor-route-fence",
      membershipSensitiveCommands: [
        "actorSend",
        "actorRequest",
        "actorDestroy",
        "actorJoin",
        "boundSessionSend",
        "boundSessionBind",
      ],
      frozenRecordKinds: ["actorSend", "actorRequest"],
      messageFollowKey: [
        "actorId",
        "objectGeneration",
        "sourceAuthorityOwnerGeneration",
        "targetAuthorityOwnerGeneration",
        "sourceOwnerLeaseGeneration",
        "targetOwnerLeaseGeneration",
      ],
      targetComparison: "exact-current-owner-authority-before-mailbox-admission",
      publicExposure: "forbidden",
    }],
    ["instance-operation-timeout-ownership", {
      command: "instanceSpot",
      wireField: "forbidden",
      owner: "source-operation-table",
      deadline: "single-monotonic-deadline-before-resolve",
      replay: "does-not-restart-or-extend-deadline",
      send: "no-operation-timeout",
    }],
    ["user-spot-terminal-operation-integrity", {
      commands: {
        create: "userSpotCreate",
        close: "userSpotClose",
        terminal: "reply",
      },
      scope: "route-mesh-object-client-or-server-to-exact-object-server-only",
      operationIdentity: "source-node-rid-source-node-generation-and-operation-id-terminal-once",
      deadline: "one-source-deadline-covering-resolve-reservation-remote-execution-and-terminal-reply-never-restarted",
      create: {
        authorityKey: "global-spot-id-user-kind-only",
        reservation: "provider-issued-reservation-id-exact-store-version-object-generation-authority-owner-generation-target-node-lifecycle-owner-lease-and-pending-capacity",
        content: "target-exact-reads-immutable-pending-creation-content-from-location-store-never-command-payload",
        admissionOrder: [
          "authenticate-source-and-exact-target-lifecycle",
          "exact-read-pending-user-spot-authority",
          "compare-key-stable-type-reservation-store-version-owner-and-target-fences",
          "factory-and-initialize",
          "same-reservation-commit",
        ],
        terminalStates: ["existing", "created", "rejected"],
        terminalTail: "exact-spot-ref-for-all-three-states",
        applicationReply: "forbidden-for-existing-optional-for-created-or-rejected",
      },
      close: {
        target: "exact-spot-ref-target-node-lifecycle-authority-owner-generation-and-store-version",
        admissionOrder: [
          "authenticate-source-and-exact-target-lifecycle",
          "exact-read-current-user-spot-authority",
          "reject-object-generation-or-authority-owner-generation-or-store-version-mismatch",
          "reject-relocation-or-closing-conflict",
          "reject-nonempty-active-actor-membership",
          "closing-cas-and-local-admission-seal",
        ],
        terminalTail: "closed-bool-false-only-for-idempotent-missing-or-active-membership",
        retarget: "forbidden",
      },
      polling: "location-row-polling-is-not-terminal-completion",
      applicationControlPacket: "forbidden",
    }],
    ["actor-create-terminal-operation-integrity", {
      commands: {
        create: "actorCreate",
        terminal: "reply",
      },
      scope: "route-mesh-object-client-or-server-to-exact-object-server-only",
      operationIdentity: "source-node-rid-source-node-generation-and-operation-id-terminal-once",
      deadline: "one-source-deadline-covering-resolve-reservation-remote-execution-and-terminal-reply-never-restarted",
      creationInput: "immutable-content-reference-in-reserved-authority-command-carries-no-application-payload",
      concurrency: "one-callback-per-actor-reservation-distinct-operation-waits-then-ready-existing-or-rejected-cleanup-new-reservation",
      terminalResult: "same-operation-only-existing-created-or-rejected-with-optional-application-payload",
      terminalEnvelopeMaximumBytes: { $bound: "creationTerminalEnvelopeBytes" },
      terminalRetention: "original-operation-deadline-plus-300000ms-provider-store-time",
      rejectedPublication: "no-ready-authority-no-active-capacity-exact-reserved-cleanup",
      applicationControlPacket: "forbidden",
    }],
    ["relocation-authority-phase-boundaries", {
      authorityType: "authority-payload-v1",
      writes: "exact-store-version-cas-each-phase",
      rules: [
        {
          phase: "preparing",
          after: ["local-admission-seal", "accepted-boundaries-fixed"],
          relocation: "absent",
          targetReservation: "absent",
        },
        {
          phase: "captured",
          after: ["immutable-relocation-put"],
          relocation: "present",
          targetReservation: "absent",
        },
        {
          phase: "prepared",
          after: [
            "relocation-reserved-ack-validated",
            "target-factory-restore-complete",
            "journal-timer-staging-complete",
          ],
          relocation: "present",
          targetReservation: "present",
        },
      ],
      closedOwnerTargetRules: {
        preparingAndCaptured: "main-owner-is-immutable-source-target-attempt-token-and-reservation-absent",
        prepared: "main-owner-is-source-exact-nonzero-target-attempt-owner-lease-node-reservation-and-relocation-present",
        committedThroughCompleted: "main-owner-is-exact-current-target-and-same-attempt-reservation-relocation-present",
        aborted: "main-owner-is-source-until-session-abort-ack-relocation-orphan-cleanup-and-steady-source-normalization",
      },
      preparedToCommitted: "one-newOwner-cas",
      sourceFence: "source-owner-id-lease-generation-node-rid-and-generation-immutable-through-terminal",
      replacementMutation: "target-attempt-target-owner-lease-node-and-reservation-only-postcommit-reenters-committed",
      readyProjection: "forbidden-while-maintenance-relocation-payload-present-including-aborted",
    }],
    ["complete-message-bound-integrity", {
      admissionType: "service-admission",
      wireField: "normalizedEffectiveMaxMessageBytes",
      range: "1..4294967295",
      normalization: {
        publicZero: "binding-or-transport-effective-receive-maximum",
        unlimitedTransport: 4294967295,
        positiveAboveWireMaximum: "startup-configuration-error",
      },
      senderLimit: "minimum-local-and-remote-normalized-bounds",
      receiverLimit: "own-admitted-normalized-bound",
      lifetimeStability: "startup-only-immutable-for-admitted-connection-lifetime",
      liveUpdate: "forbidden-weight-only-remains-live-updatable",
      enforcement: "immediately-after-complete-envelope-length-prefix-before-allocation",
      payloadLimit: "negotiated-complete-message-bound-minus-actual-envelope-overhead",
      mismatch: "oversize-protocol-error-and-connection-not-ready",
    }],
    ["participant-sequence-domain", {
      sequenceFields: [
        "journal-entry.sequence",
        "relocation-pending-timer-tick.sequence",
        "request-completion-entry.sequence",
        "relocationData.sequence",
        "reply-relay-context.maintenanceRelocation.sequence",
      ],
      relocationQueueOrdering: "journal-entry-and-pending-timer-tick-share-one-strictly-increasing-participant-sequence",
      crossVectorDuplicate: "forbidden-between-journal-entry-and-relocation-pending-timer-tick-only-completion-and-relay-reference-the-original-sequence",
      sequenceStart: 1,
      zeroMeaning: "no-accepted-or-replayed-record",
      progressFields: ["acceptedBoundary", "replayCursor", "highWater"],
      overflow: "seal-participant-and-fail-new-admission-terminally",
      wrap: "forbidden",
    }],
    ["terminal-failure-integrity", {
      failureCodeType: "framework-error-code",
      fields: [
        "request-completion-entry.failureCode",
        "frozen-record-body.completion.failureCode",
        "reply.failureCode",
        "replyRelay.failureCode",
        "relocation-control-data.failureCode",
      ],
      success: {
        terminalResult: "ok",
        failureCode: "none",
        applicationPayload: "operation-contract-dependent",
      },
      boundaryFailure: {
        terminalResults: [
          "timedOut", "terminated", "busy", "notConnected", "invalidArgument",
          "invalidState", "notSupported", "backpressured",
        ],
        failureCode: "none",
        applicationPayload: "forbidden",
      },
      typedFrameworkFailure: {
        terminalResults: ["notFound", "protocolError", "internalError", "rejected", "conflict"],
        failureCode: "non-none",
        applicationPayload: "forbidden",
        exactResultByFailureCode: {
          actorRouteNotFound: "notFound",
          actorCreateFailed: "internalError",
          actorAlreadyExists: "conflict",
          actorTypeMismatch: "conflict",
          spotCreateFailed: "internalError",
          spotRouteNotFound: "notFound",
          spotTypeMismatch: "conflict",
          actorSessionNotBound: "notFound",
          handlerNotFound: "notFound",
          routeHandlerNotFound: "notFound",
          actorDispatchHandlerNotFound: "notFound",
          payloadDecodeFailed: "protocolError",
          routeNotConnected: "internalError",
          requestTargetNotFound: "notFound",
          requestRejected: "rejected",
          requestProtocolError: "protocolError",
          requestFailed: "internalError",
          workerQueueFull: "rejected",
          workerTimedOut: "internalError",
          workerFailed: "internalError",
          actorLocationStale: "conflict",
          actorCreateRejected: "rejected",
          spotGenerationStale: "conflict",
          spotMoving: "conflict",
          relocationDataLost: "internalError",
        },
      },
      unknownFailureCode: "protocol-error-before-application-dispatch",
      publicMapping: "wire-value-minus-one",
      reservedWireValues: {
        first: 23,
        last: 32,
        reason: "public-only-framework-errors-not-valid-on-service-wire",
      },
    }],
    ["actor-route-transition-integrity", {
      spotRefType: "spot-ref",
      optionalSpotRefType: "optional-spot-ref",
      membershipType: "spot-membership",
      optionalMembershipType: "optional-spot-membership",
      actorJoined: {
        previous: "optional",
        current: "required",
        currentAuthorityOwnerGeneration: "exact-current-authority",
        existingPreviousOwnerOrder: "current-greater-than-previous",
      },
      actorLeft: {
        previous: "required",
        currentAuthorityOwnerGeneration: "exact-current-authority",
        authorityOwnerGenerationOrder: "current-greater-than-previous",
      },
      frozenSpotControl: {
        created: "current-only",
        joined: "optional-previous-and-required-current",
        left: "previous-and-current",
        disconnected: "current-only",
        destroyed: "previous-only",
        sameActorAcrossTransition: "exact-actor-id-and-generation",
        joinedCommittedOwnerOrder: "current-greater-than-previous",
        leftAuthorityOwnerGenerationOrder: "current-greater-than-previous",
        authorityComparison: "exact-current-authority-before-replay-dispatch",
        failureDelivery: "terminal-completion-not-lifecycle-record",
      },
      authorityOwnerGenerationOverflow: "terminal-authority-error-no-wire-emission",
    }],
    ["relocation-participant-resource-integrity", {
      participantIdentity: {
        standaloneObjectMailbox: "exactly-participant-id-1-no-binding-fields",
        userSpotObjectMailboxes: "participant-id-1-is-user-spot-then-member-actors-from-2-in-ascending-global-actor-id-order-no-binding-fields",
        boundSession: "participant-id-after-all-object-mailboxes-in-owning-object-participant-id-then-ascending-session-rid-order-with-nonzero-binding-generation",
        boundSessionOwner: "exact-owning-actor-from-current-binding-record",
        sessionOwnerRoute: "node-rid-lifecycle-generation-owner-id-and-lease-generation-match-current-descriptor-and-host-lease-before-seal-or-route-switch",
        sessionRid: "unique-within-relocation-transaction",
        stability: "same-id-and-fence-for-relocation-recovery-and-retransmit",
      },
      cardinality: {
        actor: "exactly-one-objectMailbox-and-zero-or-one-boundSession",
        instanceSpot: "exactly-one-objectMailbox-and-no-boundSession",
        userSpotAggregate: "exactly-one-user-spot-objectMailbox-and-one-objectMailbox-per-canonical-member-actor-plus-zero-or-one-boundSession-per-member-actor",
        userSpotObjectMaximum: { $bound: "maintenanceAggregateParticipants" },
        resourceMaximum: { $bound: "relocationResourceParticipants" },
      },
      relocationReady: {
        targetToSource: {
          role: "target",
          offeredMessages: "nonzero",
          offeredBytes: "nonzero",
          participants: "empty",
        },
        sourceToTarget: {
          role: "source",
          offeredMessages: 0,
          offeredBytes: 0,
          participants: "nonempty",
          participantIdentity: "required-with-zero-capable-exact-allowances",
          checkedAllowanceSums: "less-than-or-equal-stored-target-offer",
        },
      },
      relocationData: {
        participant: "must-be-negotiated",
        sequence: "nonzero-and-less-than-or-equal-allowanceMessages",
        cumulativeBytes: "sum-unique-canonical-frozen-record-encoded-bytes-less-than-or-equal-allowanceBytes",
        duplicateSameBytes: "idempotent-no-capacity-recharge",
        duplicateDifferentBytes: "protocol-error",
      },
      relocationAck: {
        participant: "must-be-negotiated",
        highWater: "less-than-or-equal-allowanceMessages",
      },
      relocationSeal: {
        request: {
          response: false,
          direction: "target-to-source",
          participants: "empty",
        },
        response: {
          response: true,
          direction: "source-to-target",
          participants: "exact-negotiated-set-once",
          highWater: "less-than-or-equal-allowanceMessages",
        },
      },
      checkedU64Overflow: "protocol-error",
    }],
    ["relocation-reservation-handshake-integrity", {
      sequence: [
        "source-relocationPrepare-exact-sealed-inventory",
        "target-relocationReady-capacity-offer",
        "source-relocationReady-exact-accept",
        "target-relocationReserved-reservation-ack",
        "source-prepared-authority-cas",
      ],
      identity: ["stable-relocation-id", "target-attempt-generation", "object-identity"],
      prepare: {
        phase: "captured",
        relocation: "required",
        requirements: "exact-participant-set-and-checked-message-byte-sums",
      },
      offer: "nonzero-capacity-and-empty-participant-vector",
      accept: "exact-prepare-participant-set-and-requirements-within-offer",
      reservationAck: "exact-accepted-participants-target-node-owner-lease-and-nonzero-reservation-generation",
      preparedGate: "matching-reservation-ack-and-exact-current-target-host-lease-read-before-cas",
      committedGate: "exact-current-target-host-lease-read-before-cas",
      activationGate: "exact-current-target-host-lease-read-before-activation",
      duplicate: "idempotent-if-identical-else-protocol-error",
      crossCandidateReplay: "forbidden",
    }],
    ["relocation-replacement-round-integrity", {
      authorityType: "authority-payload-v1",
      fenceType: "relocation-coordinator-fence",
      rounds: {
        initial: {
          phase: "captured",
          initiatorRoles: ["source"],
          proposedTargetAttemptGeneration: "one",
        },
        preparedReplacement: {
          phase: "prepared",
          initiatorRoles: ["source", "coordinator"],
          proposedTargetAttemptGeneration: "current-target-attempt-generation-plus-one",
        },
        postCommitReplacement: {
          phases: ["committed", "activating", "activated", "cleaning"],
          initiatorRoles: ["coordinator"],
          proposedTargetAttemptGeneration: "current-target-attempt-generation-plus-one",
        },
      },
      requirementsSource: "exact-current-relocation-participant-inventory",
      sequence: [
        "initiator-relocationPrepare",
        "candidate-relocationReady-offer",
        "initiator-relocationReady-accept",
        "candidate-relocationReserved-ack",
        "expected-version-authority-cas",
      ],
      replacementCas: "after-ack-atomically-replace-target-attempt-generation-target-and-reservation-prepared-stays-prepared-post-commit-resets-to-committed",
      oldTargetFence: "old-target-attempt-generation-or-authority-store-version-rejected",
      relocationIdentity: "stable-relocation-id-relocation-root-and-journal-never-rewritten-only-for-target-replacement",
      targetActivationRetry: "factory-and-restore-are-at-least-once-across-attempts-and-stale-attempts-may-overlap",
      targetCommitFence: "only-current-exact-owner-and-target-attempt-may-commit-completion-or-open-admission",
      callbackContract: "retry-safe-no-exactly-once-external-side-effect-guarantee-and-no-public-relocation-id",
      crossCandidateReplay: "candidate-node-rid-and-generation-must-match-admitted-peer",
      ackBeforeCasCrash: "retry-identical-round-or-release-orphan-reservation-before-new-candidate",
      casBeforeReplyCrash: "read-current-authority-and-continue-new-target-without-second-cas",
    }],
    ["relocation-coordinator-authorization-integrity", {
      authorityType: "authority-payload-v1",
      fenceType: "relocation-coordinator-fence",
      receiverValidation: [
        "exact-read-current-authority-by-object-key",
        "compare-relocation-id-target-attempt-generation-when-present-phase-and-expected-store-version",
        "compare-coordinator-owner-id-lease-generation-node-rid-and-node-generation",
        "read-current-owner-lease-token",
        "match-authenticated-admitted-peer-rid-and-generation-to-declared-sender-role",
      ],
      coordinatorSender: "allowed-only-when-authenticated-peer-matches-current-coordinator-node-and-lease-token",
      coordinatorTakeover: "expected-version-cas-updates-only-coordinator-fence-unless-object-owner-also-changes",
      staleTakeover: "protocol-error-no-mutation",
      aba: "global-lease-generation-prevents-owner-id-reuse-from-revalidating-old-control",
    }],
    ["bound-session-relocation-barrier-integrity", {
      sequence: [
        "source-sessionRelocationSeal",
        "session-owner-reversible-ingress-seal",
        "session-owner-sessionRelocationSealed-high-water",
        "source-receives-every-sequence-through-high-water",
        "source-captured-relocation",
        "target-restores-and-replays-through-high-water",
        "target-stage-bound-session-route-without-switch-or-unseal",
        "durable-source-cleanup-state-cas",
        "completed-authority-cas",
        "target-sessionRelocationRoute-commit",
        "session-owner-atomic-route-switch-after-completed",
        "session-owner-sessionRelocationRouted-ack",
        "maintenance-authority-normalized-to-steady",
        "target-open-application-admission-and-publish-ready",
      ],
      participantIdentity: "session-owner-node-rid-generation-owner-id-lease-generation-session-rid-binding-generation",
      recordSequence: "bound-session-actorSend-or-actorRequest-sourceSessionSequence",
      relocationRelation: "participant-sequence-equals-session-sequence-and-accepted-boundary-equals-sealed-high-water",
      commitFence: "binding-generation-actor-object-generation-previous-and-target-owner-generation-session-owner-node-generation",
      sessionOwnerLeaseFence: "owner-id-and-lease-generation-exact-descriptor-and-current-host-lease-read",
      senderAdmissionDeadline: "local-monotonic-deadline-derived-from-last-successful-host-lease-read",
      staleSessionOwnerLease: "protocol-error-no-seal-or-route-switch",
      commitPhase: "completed-only",
      targetAdmission: "sealed-through-activated-cleaning-completed-and-route-ack-open-only-after-steady-normalization",
      abort: "source-remains-sealed-aborted-authority-cas-first-then-abort-route-and-routed-ack-cleanup-steady-source-normalization-then-reopen",
      duplicate: "idempotent-if-identical-else-protocol-error",
      missingSequence: "do-not-capture-or-commit",
      sessionOwnerRestart: "stale-node-generation-protocol-error-no-route-switch",
    }],
    ["relocation-complete-integrity", {
      command: "relocationComplete",
      direction: "source-or-current-coordinator-to-current-target",
      preconditions: [
        "all-negotiated-participants-sealed",
        "target-activation-succeeded",
        "durable-source-cleanup-state-is-completed-or-sourceLeaseExpired",
        "authority-phase-activated-or-cleaning-or-completed",
      ],
      sourceProof: "completed-requires-authenticated-exact-stored-source-token-sourceLeaseExpired-requires-current-coordinator-exact-read-of-that-token-as-missing-or-stale",
      targetEffect: "validate-durable-source-cleanup-state-and-notify-finalization-once",
      cleanupGate: "target-activation-complete-and-durable-source-cleanup-state-terminal",
      servingGate: "completed-authority-cas-then-bound-session-route-acks-before-application-admission-or-ready-publish",
      replacementWindow: "all-nonterminal-phases-before-completed-from-immutable-relocation",
      afterCompletedFailure: "ordinary-owner-loss-never-replays-retired-relocation",
      finalization: "after-completed-and-bound-session-route-acks-cas-maintenance-authority-to-steady-without-relocation-before-ready-projection",
      resolverProjection: "maintenance-relocation-authority-is-never-ready-even-when-target-owner-is-recorded",
      duplicate: "idempotent-by-stable-relocation-id-and-durable-source-cleanup-state-target-attempt-is-only-a-peer-fence",
      reorder: "hold-until-preconditions",
      wrongDirection: "protocol-error",
    }],
    ["transport-admission-integrity", {
      admissionType: "service-admission",
      topologies: {
        routeMesh: {
          descriptor: "route-mesh-admission",
          meshName: "required",
        },
        clientServer: {
          descriptor: "client-server-admission",
          meshName: "forbidden",
          helloRole: "client",
          admitAndUpdateRole: "server",
          direction: "clientToServer",
          clientToServerCommands: ["channelSend", "channelRequest", "livenessProbe", "livenessAck"],
          serverToClientCommands: ["reply", "livenessProbe", "livenessAck", "update", "reject"],
          allOtherServiceCommands: "protocol-error",
        },
      },
      generation: "nonzero-opaque-lifecycle-equality-token-no-numeric-ordering-store-backed-fenced-by-exact-owner-lease-manual-generated-by-csprng-and-fenced-by-current-connection-handover",
      revision: "same-generation-strictly-increasing-update-only",
      securityIdentity: "exact-transport-authenticated-identity-match",
      connectionLifetime: "admission-bound-to-current-physical-connection-identity-no-cross-connection-replay",
      crossTopologyReplay: "protocol-error-before-application-dispatch",
      oversizeDescriptor: "reject-before-allocation",
    }],
    ["admission-reject-integrity", {
      reasonType: "reject-reason",
      unknownReason: "protocol-error",
      localErrnoOnWire: "forbidden",
    }],
    ["frozen-record-integrity", {
      recordType: "frozen-record",
      operationMatrix: {
        nodeSend: { operationKind: "none", operationId: "zero" },
        nodeRequest: { operationKind: "nodeRequest", operationId: "nonzero" },
        channelSend: { operationKind: "none", operationId: "zero" },
        channelRequest: { operationKind: "channelRequest", operationId: "nonzero" },
        spotSend: {
          operationKind: "none",
          operationId: "nonzero-for-message-follow-dedupe",
        },
        spotRequest: { operationKind: "spotRequest", operationId: "nonzero" },
        spotMulticast: { operationKind: "none", operationId: "zero" },
        spotControl: {
          operationKinds: ["none", "actorJoin", "actorLeave", "actorDestroy"],
          operationId: "nonzero-iff-operation-kind-non-none",
        },
        actorSend: {
          operationKind: "none",
          operationId: "nonzero-for-message-follow-dedupe",
        },
        actorRequest: { operationKind: "actorRequest", operationId: "nonzero" },
        completion: { operationKind: "non-none", operationId: "nonzero" },
        sendReady: { operationKind: "none", operationId: "zero" },
        relocationControl: { operationKind: "none", operationId: "zero" },
        instanceSpotActivation: {
          innerSend: ["none", "zero"],
          innerRequest: ["instanceSpotRequest", "nonzero"],
        },
      },
      replyRoute: {
        requiredNonzeroOperationKinds: ["nodeRequest", "channelRequest", "spotRequest", "actorRequest", "instanceSpotRequest"],
        allOtherOperationKinds: "forbidden",
        source: "exact-original-request-correlation",
        operationIdRole: "dedupe-only-not-route",
      },
      metadata: {
        allowedRecordKinds: [
          "nodeSend", "nodeRequest", "channelSend", "channelRequest", "spotSend",
          "spotRequest", "spotMulticast", "actorSend", "actorRequest", "instanceSpotActivation",
        ],
        allOtherRecordKinds: "forbidden",
      },
      sourceIdentity: {
        applicationRecordKinds: "node-spot-actor-or-boundSession-closed-union-with-node-lifecycle-generation",
        infrastructureRecordKinds: "node-source-only-with-lifecycle-generation",
        boundSession: "actor-session-rid-sequence-and-nonzero-binding-generation-all-required",
        remoteUseFence: "source-node-generation-must-exactly-match-current-admitted-descriptor-and-connection-without-numeric-comparison",
        runtimeLifetimeUnion: "leaseBacked-owner-id-and-lease-generation-or-connectionBound-current-physical-connection-lifetime",
        durableRecord: "leaseBacked-only-all-frozen-sources-carry-exact-owner-id-and-lease-generation",
        preCapturedDrain: "all-connectionBound-accepted-work-and-all-boundSession-requests-must-reach-terminal-before-captured-cas",
        drainFailure: "pre-captured-abort-and-retire-blocked-relocationDisabled-then-restore-admission",
        connectionBoundFrozenRecord: "forbidden",
      },
      sourceIdentityMismatch: "protocol-error-before-replay-or-relocation-admission",
    }],
    ["durable-operation-identity-integrity", {
      relocationId: "runtime-generated-nonzero-128-bit-csprng",
      relocationIdScope: "unique-across-active-and-retained-relocation-roots-collision-rejected-and-regenerated",
      operationId: "nonzero-unique-within-source-owner-lifecycle-for-terminal-dedupe",
      replyRouteId: "nonzero-unique-within-source-owner-lifecycle-for-correlation-only",
      counterWrapOrReuse: "forbidden-terminal-runtime-error",
      terminalIdentity: "stable-relocation-id-plus-exact-request-source-fence-plus-operation-id",
      publicExposure: "forbidden",
    }],
    ["owner-lease-timing-integrity", {
      renewIntervalMs: 5000,
      ttlMs: 15000,
      renewTimeoutMs: 3000,
      fencingMarginMs: 5000,
      startupRelation: "renewInterval-plus-renewTimeout-strictly-less-than-ttl-minus-fencingMargin",
      scope: "all-location-store-hosts-independent-of-routing-allocation",
    }],
    ["local-creation-publication-integrity", {
      objectKinds: ["actor", "userSpot"],
      newObjectCas: "allocates-final-object-and-authority-owner-generations-and-creating-row-before-factory",
      actorCreate: "typed-factory-initialize-and-initial-entry-membership-before-ready-cas",
      userSpotCreate: "factory-configure-and-initialize-before-ready-cas",
      readyCas: "same-store-version-object-generation-authority-owner-generation-and-exact-owner-lease",
      remoteVisibility: "resolver-and-remote-messaging-ready-only",
      failure: "failed-sealed-local-barrier-exact-fenced-delete-and-read-reconcile-next-caller-only-newObject",
      lateCallback: "stale-fence-no-mutation",
      entrySpot: "startup-initialization-complete-before-host-serving-and-publication",
    }],
    ["contract-amendment-limit-integrity", {
      creationIntentBytesMaximum: { $bound: "creationIntentBytes" },
      creationTerminalEnvelopeBytesMaximum: { $bound: "creationTerminalEnvelopeBytes" },
      aggregateParticipantsMaximum: { $bound: "maintenanceAggregateParticipants" },
      aggregateEncodedBytesMaximum: { $bound: "maintenanceAggregateBytes" },
      aggregateId: "nonzero-128-bit",
      messageFollowHopCountMaximum: { $bound: "messageFollowHopCount" },
      messageFollowQueuedMessagesMaximum: { $bound: "messageFollowMessages" },
      messageFollowQueuedBytesMaximum: { $bound: "messageFollowBytes" },
      routingIdEntropyBits: 128,
      routingIdCollisionAttemptsMaximum: { $bound: "routingIdCollisionAttempts" },
      nodeActiveCapacityDefault: { $bound: "nodeActiveCapacityDefault" },
      nodePendingCapacityDefault: { $bound: "nodePendingCapacityDefault" },
      objectTypeCapacityMaximum: { $bound: "objectTypeCapacityMaximum" },
    }],
    ["actor-retire-membership-integrity", {
      relocatable: "source-entry-spot-current-member-only",
      userSpotMember: "preflight-blocked-relocationDisabled-state-and-admission-unchanged",
      targetOffer: "compatible-initialized-target-entry-spot-id-object-generation-and-kind",
      commit: "newOwner-cas-atomically-updates-owner-authority-owner-generation-and-current-target-entry-spot",
      callbackOrder: "factory-restore-and-staging-then-owner-entry-membership-commit-then-target-entry-onActorRelocated-then-source-entry-onLeaveActor-and-old-entry-membership-durable-cleanup-or-source-crash-durable-cleanup-terminal-then-journal-replay",
      targetCallback: "target-entry-onActorRelocated-after-commit-never-onJoinedActor",
      sourceCleanup: "source-entry-onLeaveActor-and-old-entry-membership-removal-durable-before-replay-or-source-crash-durable-cleanup-terminal-substitutes",
      userSpotAggregateCallbacks: "onActorJoin-onJoinedActor-onActorRelocated-onLeaveActor-all-forbidden",
      targetAdmission: "sealed-through-completed-route-acks-and-steady-normalization",
      callbacks: "retry-safe-at-least-once-failure-never-rolls-back-committed-authority",
    }],
    ["stateful-capability-integrity", {
      entryType: "stateful-capability-entry",
      key: ["objectKind", "type"],
      disabled: { hasSnapshotAdapter: false },
      recreate: { hasSnapshotAdapter: false },
      snapshot: { hasSnapshotAdapter: true },
      eligibility: "same-entry-exact-object-kind-type-policy-snapshot-adapter-availability-and-positive-available",
      "flat-set-cross-product": "forbidden",
      startupValidation: "derive-complete-descriptor-then-validate-before-host-start",
      bounds: {
        encodedDescriptorBytesMaximum: 1048576,
        typeCapabilityEntriesMaximum: 1024,
      },
      overflow: "atomic-configuration-failure-never-truncate-split-or-publish-partial",
    }],
    ["relocation-application-state-integrity", {
      stateType: "relocation-application-state",
      recreate: "hasState-false-no-payload",
      snapshot: "hasState-true-opaque-payload-empty-valid",
      participantMapping: "exactly-one-sorted-application-state-entry-per-objectMailbox-participant-id-and-no-boundSession-entry",
      participantPayloadMaximumBytes: { $bound: "relocationChunkBytes" },
      relocationPresence: "every-relocation-including-empty-recreate-writes-one-deterministic-envelope",
      storage: "canonical-logical-stream-split-into-immutable-chunks-and-one-root-manifest",
      chunking: "ordered-byte-stream-may-split-within-a-frozen-record",
      applicationInterpretation: "forbidden-framework-validates-byte-count-and-relocation-checksum-only",
    }],
    ["framework-json-v1-integrity", {
      profile: "frameworkJsonV1Profile",
      applicationPayloadBytes: "opaque-after-profile-validation-no-byte-canonicalization",
      goldenFixture: "golden/framework-json-v1.json",
    }],
    ["service-admission-update-integrity", {
      command: "update",
      connection: "current-admitted-physical-connection-only",
      immutableExact: [
        "topologyKind", "meshName-or-channelName", "securityIdentity", "endpoint-connection-identity",
        "rid", "lifecycleGeneration", "normalizedEffectiveMaxMessageBytes", "channelMembershipKeys",
        "protocolCapabilities", "spotTypes", "statefulCapabilities", "applicationVersion",
      ],
      revision: "strictly-increasing-same-revision-identical-bytes-idempotent-lower-stale-same-revision-different-protocol-error",
      mutableOnly: ["existingChannelWeights", "runtimeState", "placementCapacity", "maintenanceWave"],
      failure: "immutable-or-capability-mutation-makes-connection-not-ready-and-is-protocol-error",
    }],
  ]);
  constraints.forEach((constraint, index) => {
    const location = `$.semanticConstraints[${index}]`;
    if (!isObject(constraint) || typeof constraint.kind !== "string") {
      fail(location, "semantic constraint must declare a known kind");
      return;
    }
    if (seenKinds.has(constraint.kind)) {
      fail(`${location}.kind`, `duplicates semantic constraint ${constraint.kind}`);
    }
    seenKinds.add(constraint.kind);
    if (constraint.kind === "implies") {
      if (!isObject(constraint.if) || !isObject(constraint.then)) {
        fail(location, "implication must define if and then conditions");
        return;
      }
      validateConditions(constraint, location, contexts, null, fail);
      return;
    }
    const expected = expectedExact.get(constraint.kind);
    if (!expected) {
      fail(`${location}.kind`, `unknown semantic constraint ${constraint.kind}`);
      return;
    }
    if (constraint.kind === "owner-lease-timing-integrity"
        && constraint.renewIntervalMs + constraint.renewTimeoutMs
          >= constraint.ttlMs - constraint.fencingMarginMs) {
      fail(location, "owner lease renew interval plus timeout must be strictly less than TTL minus fencing margin");
    }
    const actual = { ...constraint };
    delete actual.kind;
    if (JSON.stringify(actual) !== JSON.stringify(expected)) {
      fail(location, `${constraint.kind} does not match the v1 contract`);
    }
  });
  for (const kind of ["implies", ...expectedExact.keys()]) {
    if (!seenKinds.has(kind)) {
      fail("$.semanticConstraints", `missing required semantic constraint ${kind}`);
    }
  }
}

function validateDurableFormats(formats, types, bounds, fail) {
  if (!Array.isArray(formats) || formats.length === 0) {
    fail("$.durableFormats", "must define versioned durable formats");
    return;
  }
  const names = new Set();
  const magics = new Set();
  formats.forEach((format, index) => {
    const location = `$.durableFormats[${index}]`;
    if (!isObject(format) || typeof format.name !== "string" || format.name.length === 0) {
      fail(location, "durable format needs a name");
      return;
    }
    if (names.has(format.name)) {
      fail(`${location}.name`, `duplicates durable format ${format.name}`);
    }
    names.add(format.name);
    if (!Array.isArray(format.magic) || format.magic.length !== 4
        || format.magic.some((value) => !Number.isSafeInteger(value) || value < 0 || value > 255)) {
      fail(`${location}.magic`, "durable magic must contain exactly four u8 values");
    } else {
      const signature = format.magic.join(",");
      if (magics.has(signature)) {
        fail(`${location}.magic`, "durable magic must be unique");
      }
      magics.add(signature);
    }
    if (!Number.isSafeInteger(format.formatVersion)
        || format.formatVersion <= 0 || format.formatVersion > 255) {
      fail(`${location}.formatVersion`, "must be a non-zero u8");
    }
    if (format.flags !== 0 || format.flagsType?.$ref !== "u16"
        || format.byteOrder !== "big-endian") {
      fail(location, "durable header must use zero u16 flags and big-endian byte order");
    }
    const lengthRef = format.bodyLengthType?.$ref;
    const maximum = resolveInteger(format.maximumEncodedBytes, bounds);
    validateLengthCapacity(lengthRef, maximum, types, bounds,
      `${location}.maximumEncodedBytes`, fail);
    if (typeof format.body?.$ref !== "string" || !types.has(format.body.$ref)) {
      fail(`${location}.body`, "must reference the exact durable body type");
    }
    const checksum = format.checksum;
    if (!isObject(checksum) || checksum.algorithm !== "crc32c-castagnoli"
        || checksum.encoding !== "u32-big-endian"
        || checksum.coverage !== "magic-through-body"
        || checksum.position !== "trailing") {
      fail(`${location}.checksum`, "must define trailing CRC32C over magic through body");
    }
    if (format.providerInterpretation !== "opaque-bytes") {
      fail(`${location}.providerInterpretation`, "Store providers must treat durable values as opaque bytes");
    }
    if (typeof format.goldenFixture !== "string" || format.goldenFixture.length === 0
        || path.isAbsolute(format.goldenFixture) || format.goldenFixture.split(/[\\/]/).includes("..")) {
      fail(`${location}.goldenFixture`, "must name a repository-local golden fixture");
    }
  });
}

function crc32c(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) === 0 ? 0 : 0x82f63b78);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

class FixtureReader {
  constructor(bytes) {
    this.bytes = bytes;
    this.offset = 0;
  }

  require(size) {
    if (!Number.isSafeInteger(size) || size < 0 || this.offset + size > this.bytes.length) {
      throw new Error("fixture body is truncated");
    }
  }

  u8() {
    this.require(1);
    return this.bytes[this.offset++];
  }

  u16() {
    this.require(2);
    const value = this.bytes.readUInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  u32() {
    this.require(4);
    const value = this.bytes.readUInt32BE(this.offset);
    this.offset += 4;
    return value;
  }

  u64() {
    this.require(8);
    const value = this.bytes.readBigUInt64BE(this.offset).toString();
    this.offset += 8;
    return value;
  }

  i64() {
    this.require(8);
    const value = this.bytes.readBigInt64BE(this.offset).toString();
    this.offset += 8;
    return value;
  }

  bytesOf(size) {
    this.require(size);
    const value = this.bytes.subarray(this.offset, this.offset + size);
    this.offset += size;
    return value;
  }

  utf8(bytes) {
    const value = bytes.toString("utf8");
    if (!Buffer.from(value, "utf8").equals(bytes)) {
      throw new Error("fixture text is not valid UTF-8");
    }
    return value;
  }

  text8() {
    return this.utf8(this.bytesOf(this.u8()));
  }

  text16() {
    return this.utf8(this.bytesOf(this.u16()));
  }

  bytes32() {
    return this.bytesOf(this.u32());
  }

  bytes64() {
    const length = BigInt(this.u64());
    if (length > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new Error("fixture byte length exceeds validator safe integer range");
    }
    return this.bytesOf(Number(length));
  }

  end() {
    if (this.offset !== this.bytes.length) {
      throw new Error("fixture body has trailing bytes");
    }
  }
}

class FixtureWriter {
  constructor() {
    this.parts = [];
  }

  raw(value) {
    this.parts.push(Buffer.from(value));
    return this;
  }

  u8(value) {
    const bytes = Buffer.alloc(1);
    bytes.writeUInt8(Number(value));
    return this.raw(bytes);
  }

  u16(value) {
    const bytes = Buffer.alloc(2);
    bytes.writeUInt16BE(Number(value));
    return this.raw(bytes);
  }

  u32(value) {
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32BE(Number(value));
    return this.raw(bytes);
  }

  u64(value) {
    const bytes = Buffer.alloc(8);
    bytes.writeBigUInt64BE(BigInt(value));
    return this.raw(bytes);
  }

  i64(value) {
    const bytes = Buffer.alloc(8);
    bytes.writeBigInt64BE(BigInt(value));
    return this.raw(bytes);
  }

  text8(value) {
    const bytes = Buffer.from(value, "utf8");
    return this.u8(bytes.length).raw(bytes);
  }

  text16(value) {
    const bytes = Buffer.from(value, "utf8");
    return this.u16(bytes.length).raw(bytes);
  }

  bytes32(value) {
    const bytes = Buffer.from(value);
    return this.u32(bytes.length).raw(bytes);
  }

  bytes64(value) {
    const bytes = Buffer.from(value);
    return this.u64(bytes.length).raw(bytes);
  }

  finish() {
    return Buffer.concat(this.parts);
  }
}

const FIXTURE_ENUMS = {
  authorityOperation: new Map([[0, "steady"], [1, "coldActivation"], [2, "maintenanceRelocation"], [3, "close"]]),
  objectKind: new Map([[1, "actor"], [2, "userSpot"], [3, "instanceSpot"]]),
  authorityObjectKind: new Map([[1, "actor"], [2, "spot"]]),
  spotKind: new Map([[1, "entry"], [2, "user"], [3, "instance"]]),
  actorSpotKind: new Map([[1, "entry"], [2, "user"]]),
  domainSpotAuthorityState: new Map([[0, "creating"], [1, "ready"], [2, "closing"]]),
  actorAuthorityState: new Map([[0, "creating"], [1, "ready"]]),
  authorityState: new Map([[1, "coldActivating"], [2, "ready"], [3, "closing"], [4, "relocating"]]),
  frozenSourceKind: new Map([[1, "node"], [2, "spot"], [3, "actor"], [4, "boundSession"]]),
  timerOverrunPolicy: new Map([
    [1, "skipLateTicks"], [2, "catchUpBounded"], [3, "delayNextTick"],
  ]),
  relocationPhase: new Map([
    [0, "none"], [1, "preparing"], [2, "captured"], [3, "prepared"], [4, "committed"],
    [5, "activating"], [6, "activated"], [7, "cleaning"], [8, "completed"], [9, "aborted"],
  ]),
  recordKind: new Map([
    [1, "nodeSend"], [2, "nodeRequest"], [3, "channelSend"], [4, "channelRequest"],
    [5, "spotSend"], [6, "spotRequest"], [7, "spotMulticast"], [8, "spotControl"],
    [9, "actorSend"], [10, "actorRequest"], [11, "completion"], [12, "sendReady"],
    [13, "relocationControl"], [14, "instanceSpotActivation"],
  ]),
  meshOperation: new Map([
    [0, "none"], [1, "nodeRequest"], [2, "channelRequest"], [3, "spotRequest"],
    [4, "actorRequest"], [5, "actorLookup"], [6, "actorDestroy"], [7, "actorJoin"],
    [8, "actorLeave"], [9, "streamBind"], [10, "streamUnbind"], [11, "streamClose"],
    [12, "instanceSpotRequest"],
  ]),
  instanceOperation: new Map([[1, "send"], [2, "request"]]),
  terminalResult: new Map([
    [0, "ok"], [101, "timedOut"], [102, "notFound"], [103, "terminated"],
    [104, "protocolError"], [105, "internalError"], [106, "rejected"], [107, "conflict"],
    [108, "busy"], [109, "notConnected"], [110, "invalidArgument"], [111, "invalidState"],
    [112, "notSupported"], [113, "backpressured"],
  ]),
  completionDelivery: new Map([
    [0, "pending"], [1, "terminalReceived"], [2, "alreadyTerminal"], [3, "sourceLeaseExpired"],
  ]),
  sourceCleanup: new Map([[0, "pending"], [1, "completed"], [2, "sourceLeaseExpired"]]),
};

function fixtureEnum(map, value, label) {
  const decoded = map.get(value);
  if (!decoded) {
    throw new Error(`unknown fixture ${label} ${value}`);
  }
  return decoded;
}

function fixtureEnumValue(map, name, label) {
  for (const [value, candidate] of map) {
    if (candidate === name) {
      return value;
    }
  }
  throw new Error(`unknown fixture ${label} ${name}`);
}

function decodeOperationId(reader) {
  return { high: reader.u64(), low: reader.u64() };
}

function decodeRelocationObject(reader) {
  const kind = fixtureEnum(FIXTURE_ENUMS.objectKind, reader.u8(), "object kind");
  const body = new FixtureReader(reader.bytesOf(reader.u16()));
  let decoded;
  if (kind === "actor") {
    decoded = {
      objectKind: kind,
      actorId: body.text8(),
      actorGeneration: body.u64(),
      expectedAuthorityOwnerGeneration: body.u64(),
    };
  } else if (kind === "userSpot") {
    decoded = {
      objectKind: kind,
      spotIdUtf8Fixture: body.text8(),
      spotGeneration: body.u64(),
      expectedAuthorityOwnerGeneration: body.u64(),
    };
  } else {
    decoded = {
      objectKind: kind,
      instanceType: body.text8(),
      spotIdUtf8Fixture: body.text8(),
      spotGeneration: body.u64(),
    };
  }
  body.end();
  return decoded;
}

function decodeAuthorityObject(reader) {
  const kind = fixtureEnum(FIXTURE_ENUMS.authorityObjectKind, reader.u8(), "authority object kind");
  const body = new FixtureReader(reader.bytesOf(reader.u16()));
  let decoded;
  if (kind === "actor") {
    decoded = {
      objectKind: kind,
      actorType: body.text8(),
      actorId: body.text8(),
      state: fixtureEnum(FIXTURE_ENUMS.actorAuthorityState, body.u8(), "Actor authority state"),
      currentSpotIdUtf8Fixture: body.text8(),
      currentSpotGeneration: body.u64(),
      currentSpotKind: fixtureEnum(FIXTURE_ENUMS.actorSpotKind, body.u8(), "Actor Spot kind"),
    };
  } else {
    const spotKind = fixtureEnum(FIXTURE_ENUMS.spotKind, body.u8(), "Spot kind");
    const spotBody = new FixtureReader(body.bytesOf(body.u16()));
    if (spotKind !== "instance") {
      decoded = {
        objectKind: kind,
        spotKind,
        spotIdUtf8Fixture: spotBody.text8(),
        spotType: spotBody.text8(),
        state: fixtureEnum(
          FIXTURE_ENUMS.domainSpotAuthorityState,
          spotBody.u8(),
          "domain Spot authority state",
        ),
      };
      spotBody.end();
      body.end();
      return decoded;
    }
    const authorityState = fixtureEnum(
      FIXTURE_ENUMS.authorityState,
      spotBody.u8(),
      "Instance authority state",
    );
    const stateBody = new FixtureReader(spotBody.bytesOf(spotBody.u16()));
    decoded = {
      objectKind: kind,
      spotKind,
      authorityState,
      instanceType: stateBody.text8(),
      spotIdUtf8Fixture: stateBody.text8(),
    };
    stateBody.end();
    spotBody.end();
  }
  body.end();
  return decoded;
}

function decodeApplicationPayload(reader) {
  if (reader.u8() !== 1) {
    throw new Error("application payload fixture must use version 1");
  }
  const body = new FixtureReader(reader.bytesOf(reader.u32()));
  const decoded = {
    packetName: body.text8(),
    contentType: body.text8(),
    payloadUtf8Fixture: body.bytes32().toString("utf8"),
  };
  body.end();
  return decoded;
}

function decodeParticipantProgress(reader) {
  const count = reader.u32();
  const progress = [];
  for (let index = 0; index < count; index += 1) {
    progress.push({
      participantId: reader.u64(),
      acceptedBoundary: reader.u64(),
      replayCursor: reader.u64(),
    });
  }
  return progress;
}

function decodeRelocationApplicationState(reader) {
  const hasState = reader.u8();
  const stateBody = new FixtureReader(reader.bytes64());
  let applicationState;
  if (hasState === 0) {
    applicationState = { hasState: false };
  } else if (hasState === 1) {
    applicationState = {
      hasState: true,
      payloadUtf8Fixture: stateBody.bytes64().toString("utf8"),
    };
  } else {
    throw new Error("relocation application state has invalid presence flag");
  }
  stateBody.end();
  return applicationState;
}

function decodeParticipantApplicationStates(reader) {
  const count = reader.u32();
  const states = [];
  for (let index = 0; index < count; index += 1) {
    states.push({
      participantId: reader.u64(),
      applicationState: decodeRelocationApplicationState(reader),
    });
  }
  return states;
}

function decodeTimerRegistrations(reader) {
  const count = reader.u32();
  const registrations = [];
  for (let index = 0; index < count; index += 1) {
    const participantId = reader.u64();
    const name = reader.text8();
    const handlerType = reader.text8();
    const periodMilliseconds = reader.u64();
    const overrunPolicy = fixtureEnum(
      FIXTURE_ENUMS.timerOverrunPolicy,
      reader.u8(),
      "timer overrun policy",
    );
    const maxCatchUpTicks = reader.u64();
    const stopOnUnhandledExceptionValue = reader.u8();
    if (stopOnUnhandledExceptionValue > 1) {
      throw new Error("timer stopOnUnhandledException has invalid bool8 value");
    }
    registrations.push({
      participantId,
      name,
      handlerType,
      periodMilliseconds,
      overrunPolicy,
      maxCatchUpTicks,
      stopOnUnhandledException: stopOnUnhandledExceptionValue === 1,
      lastCompletedDeliveryIndex: reader.u64(),
      lastCompletedScheduledIndex: reader.u64(),
      nextScheduledAtUnixMilliseconds: reader.u64(),
    });
  }
  return registrations;
}

function decodePendingTimerTicks(reader) {
  const count = reader.u32();
  const ticks = [];
  for (let index = 0; index < count; index += 1) {
    ticks.push({
      participantId: reader.u64(),
      sequence: reader.u64(),
      timerName: reader.text8(),
      deliveryIndex: reader.u64(),
      scheduledIndex: reader.u64(),
      scheduledAtUnixMilliseconds: reader.u64(),
      skippedTicks: reader.u64(),
    });
  }
  return ticks;
}

function decodeRequestCompletion(reader) {
  const operationId = decodeOperationId(reader);
  const requestSource = {
    sourceOwnerId: reader.text8(),
    sourceOwnerLeaseGeneration: reader.u64(),
    sourceNodeRidUtf8Fixture: reader.text8(),
    sourceNodeGeneration: reader.u64(),
  };
  const participantId = reader.u64();
  const sequence = reader.u64();
  const terminalResult = fixtureEnum(FIXTURE_ENUMS.terminalResult, reader.u32(), "terminal result");
  const failureCode = reader.u32();
  const deliveryState = fixtureEnum(
    FIXTURE_ENUMS.completionDelivery,
    reader.u8(),
    "completion delivery state",
  );
  const hasPayload = reader.u8();
  if (hasPayload !== 0 && hasPayload !== 1) {
    throw new Error("completion has invalid payload presence flag");
  }
  return {
    operationId,
    requestSource,
    participantId,
    sequence,
    terminalResult,
    failureCode,
    deliveryState,
    payload: hasPayload === 1 ? decodeApplicationPayload(reader) : null,
  };
}

function decodeCompletionVector(reader) {
  const count = reader.u32();
  const completions = [];
  for (let index = 0; index < count; index += 1) {
    completions.push(decodeRequestCompletion(reader));
  }
  return completions;
}

function decodeRelocationRootPointer(reader) {
  const present = reader.u8();
  const body = new FixtureReader(reader.bytesOf(reader.u16()));
  if (present === 0) {
    body.end();
    return null;
  }
  if (present !== 1) {
    throw new Error("relocation pointer has invalid presence flag");
  }
  const decoded = {
    referenceUtf8Fixture: body.text16(),
    checksumCrc32c: body.u32(),
  };
  body.end();
  return decoded;
}

function decodeInstancePlacement(reader) {
  if (reader.u8() !== 1) {
    throw new Error("Instance placement fixture must use version 1");
  }
  const body = new FixtureReader(reader.bytesOf(reader.u16()));
  const decoded = {
    targetNodeRidUtf8Fixture: body.text8(),
    targetNodeGeneration: body.u64(),
    targetSpotIdUtf8Fixture: body.text8(),
    authority: {
      objectGeneration: body.u64(),
      ownerId: body.text8(),
      authorityOwnerGeneration: body.u64(),
      leaseGeneration: body.u64(),
      storeVersion: body.text16(),
    },
    instanceType: body.text8(),
  };
  body.end();
  return decoded;
}

function decodeFrozenRecord(reader) {
  const recordKind = fixtureEnum(FIXTURE_ENUMS.recordKind, reader.u8(), "record kind");
  const sourceKind = fixtureEnum(FIXTURE_ENUMS.frozenSourceKind, reader.u8(), "frozen source kind");
  const sourceBody = new FixtureReader(reader.bytesOf(reader.u16()));
  const source = {
    sourceKind,
    sourceNodeRidUtf8Fixture: sourceBody.text8(),
    sourceNodeGeneration: sourceBody.u64(),
    sourceOwnerId: sourceBody.text8(),
    sourceOwnerLeaseGeneration: sourceBody.u64(),
  };
  if (sourceKind === "spot") {
    source.sourceSpotIdUtf8Fixture = sourceBody.text8();
  } else if (sourceKind === "actor" || sourceKind === "boundSession") {
    source.sourceActor = { actorId: sourceBody.text8(), generation: sourceBody.u64() };
    if (sourceKind === "boundSession") {
      source.sourceSessionRidUtf8Fixture = sourceBody.text8();
      source.sourceBindingGeneration = sourceBody.u64();
      source.sourceSessionSequence = sourceBody.u64();
    }
  }
  sourceBody.end();
  const hasMetadata = reader.u8();
  if (hasMetadata !== 0) {
    throw new Error("rich golden fixture keeps optional metadata absent");
  }
  const operationId = decodeOperationId(reader);
  const operationKind = fixtureEnum(FIXTURE_ENUMS.meshOperation, reader.u32(), "mesh operation kind");
  const replyRouteBody = new FixtureReader(reader.bytesOf(reader.u16()));
  const requestOperationKinds = new Set([
    "nodeRequest", "channelRequest", "spotRequest", "actorRequest", "instanceSpotRequest",
  ]);
  const replyRouteId = requestOperationKinds.has(operationKind) ? replyRouteBody.u64() : null;
  replyRouteBody.end();
  if (recordKind === "spotRequest") {
    return {
      recordKind,
      source,
      metadata: null,
      operationId,
      operationKind,
      replyRouteId,
      body: {
        targetSpot: {
          spotIdUtf8Fixture: reader.text8(),
          spotGeneration: reader.u64(),
          targetNodeRidUtf8Fixture: reader.text8(),
          targetNodeGeneration: reader.u64(),
          expectedAuthorityOwnerGeneration: reader.u64(),
          expectedOwnerLeaseGeneration: reader.u64(),
        },
        payload: decodeApplicationPayload(reader),
      },
    };
  }
  if (recordKind !== "instanceSpotActivation") {
    throw new Error("rich golden fixture must exercise a Spot request record");
  }
  const placement = decodeInstancePlacement(reader);
  const sourceNodeGeneration = reader.u64();
  const instanceOperationKind = fixtureEnum(
    FIXTURE_ENUMS.instanceOperation,
    reader.u8(),
    "Instance operation kind",
  );
  const payload = decodeApplicationPayload(reader);
  return {
    recordKind,
    source,
    metadata: null,
    operationId,
    operationKind,
    replyRouteId,
    body: {
      placement,
      sourceNodeGeneration,
      operationKind: instanceOperationKind,
      payload,
    },
  };
}

function decodeJournal(reader) {
  const count = reader.u32();
  const journal = [];
  for (let index = 0; index < count; index += 1) {
    journal.push({
      participantId: reader.u64(),
      sequence: reader.u64(),
      record: decodeFrozenRecord(reader),
    });
  }
  return journal;
}

function decodeGoldenBody(formatName, bytes) {
  const reader = new FixtureReader(bytes);
  let decoded;
  if (formatName === "authority-payload-v1") {
    decoded = {
      operationKind: fixtureEnum(
        FIXTURE_ENUMS.authorityOperation,
        reader.u8(),
        "authority operation kind",
      ),
      object: decodeAuthorityObject(reader),
      ownerId: reader.text8(),
      ownerLeaseGeneration: reader.u64(),
      ownerMeshName: reader.text8(),
      ownerNodeRidUtf8Fixture: reader.text8(),
      ownerNodeGeneration: reader.u64(),
      relocationState: null,
      activationRecoveryState: null,
    };
    const hasRelocation = reader.u8();
    const relocationBody = new FixtureReader(reader.bytesOf(reader.u32()));
    if (hasRelocation === 1) {
      decoded.relocationState = {
        relocationHigh: relocationBody.u64(),
        relocationLow: relocationBody.u64(),
        targetAttemptGeneration: relocationBody.u64(),
        sourceNodeRidUtf8Fixture: relocationBody.text8(),
        sourceNodeGeneration: relocationBody.u64(),
        sourceOwnerId: relocationBody.text8(),
        sourceOwnerLeaseGeneration: relocationBody.u64(),
        targetNodeRidUtf8Fixture: relocationBody.text8(),
        targetNodeGeneration: relocationBody.u64(),
        targetOwnerId: relocationBody.text8(),
        targetOwnerLeaseGeneration: relocationBody.u64(),
        reservationGeneration: relocationBody.u64(),
        coordinatorOwnerId: relocationBody.text8(),
        coordinatorLeaseGeneration: relocationBody.u64(),
        coordinatorNodeRidUtf8Fixture: relocationBody.text8(),
        coordinatorNodeGeneration: relocationBody.u64(),
        phase: fixtureEnum(FIXTURE_ENUMS.relocationPhase, relocationBody.u8(), "relocation phase"),
        relocationRoot: decodeRelocationRootPointer(relocationBody),
        applicationVersion: relocationBody.i64(),
        participantProgress: decodeParticipantProgress(relocationBody),
        terminalCompletionCount: relocationBody.u32(),
        pendingRelayCount: relocationBody.u32(),
        sourceCleanupState: fixtureEnum(
          FIXTURE_ENUMS.sourceCleanup,
          relocationBody.u8(),
          "source cleanup state",
        ),
      };
    } else if (hasRelocation !== 0) {
      throw new Error("authority relocation state has invalid presence flag");
    }
    relocationBody.end();
    const hasActivationRecovery = reader.u8();
    const activationBody = new FixtureReader(reader.bytesOf(reader.u32()));
    if (hasActivationRecovery === 1) {
      decoded.activationRecoveryState = {
        referenceUtf8Fixture: activationBody.text8(),
        sha256Hex: activationBody.bytesOf(32).toString("hex"),
        encodedSize: activationBody.u32(),
        inboxSequence: activationBody.u64(),
        replayCursor: activationBody.u64(),
      };
    } else if (hasActivationRecovery !== 0) {
      throw new Error("authority activation recovery state has invalid presence flag");
    }
    activationBody.end();
  } else if (formatName === "instance-activation-recovery-v1") {
    const targetSpotIdUtf8Fixture = reader.text8();
    const stableType = reader.text8();
    const targetMeshName = reader.text8();
    const targetNodeRidUtf8Fixture = reader.text8();
    const targetNodeGeneration = reader.u64();
    const targetDescriptorVersion = reader.text8();
    const sourceNodeRidUtf8Fixture = reader.text8();
    const sourceNodeGeneration = reader.u64();
    const hasSourceSpot = reader.u8();
    let sourceSpotIdUtf8Fixture = null;
    if (hasSourceSpot === 1) {
      sourceSpotIdUtf8Fixture = reader.text8();
    } else if (hasSourceSpot !== 0) {
      throw new Error("Instance activation source Spot presence flag is invalid");
    }
    const operationKind = fixtureEnum(
      FIXTURE_ENUMS.instanceOperation,
      reader.u8(),
      "Instance operation kind",
    );
    const operationHigh = reader.u64();
    const operationLow = reader.u64();
    let replyRouteId = null;
    if (operationKind === "request") {
      replyRouteId = reader.u64();
    }
    const deadlineUnixMs = reader.u64();
    const hasMetadata = reader.u8();
    let metadata = null;
    if (hasMetadata === 1) {
      const version = reader.u8();
      const count = reader.u8();
      const entries = [];
      for (let index = 0; index < count; index += 1) {
        entries.push({ key: reader.text8(), value: reader.text16() });
      }
      metadata = { version, entries };
    } else if (hasMetadata !== 0) {
      throw new Error("Instance activation metadata presence flag is invalid");
    }
    const payloadVersion = reader.u8();
    const payloadBody = new FixtureReader(reader.bytesOf(reader.u32()));
    const applicationPayload = {
      version: payloadVersion,
      packetName: payloadBody.text8(),
      contentType: payloadBody.text8(),
      payloadHex: payloadBody.bytes32().toString("hex"),
    };
    payloadBody.end();
    decoded = {
      targetSpotIdUtf8Fixture,
      stableType,
      targetMeshName,
      targetNodeRidUtf8Fixture,
      targetNodeGeneration,
      targetDescriptorVersion,
      sourceNodeRidUtf8Fixture,
      sourceNodeGeneration,
      sourceSpotIdUtf8Fixture,
      operationKind,
      operationHigh,
      operationLow,
      replyRouteId,
      deadlineUnixMs,
      metadata,
      applicationPayload,
    };
  } else if (formatName === "relocation-data-chunk-v1") {
    decoded = {
      order: reader.u32(),
      dataHex: reader.bytes32().toString("hex"),
    };
  } else if (formatName === "relocation-manifest-v1") {
    const logicalFormatVersion = reader.u8();
    const totalLength = reader.u64();
    const totalChecksumCrc32c = reader.u32();
    const inventoryDigestSha256Hex = reader.bytesOf(reader.u8()).toString("hex");
    const count = reader.u32();
    const chunks = [];
    for (let index = 0; index < count; index += 1) {
      chunks.push({
        order: reader.u32(),
        referenceUtf8Fixture: reader.text16(),
        length: reader.u64(),
        checksumCrc32c: reader.u32(),
      });
    }
    decoded = {
      logicalFormatVersion,
      totalLength,
      totalChecksumCrc32c,
      inventoryDigestSha256Hex,
      chunks,
    };
  } else if (formatName === "relocation-envelope-v1") {
    const relocationHigh = reader.u64();
    const relocationLow = reader.u64();
    const object = decodeRelocationObject(reader);
    const applicationVersion = reader.i64();
    decoded = {
      relocationHigh,
      relocationLow,
      object,
      applicationVersion,
      applicationStates: decodeParticipantApplicationStates(reader),
      participantProgress: decodeParticipantProgress(reader),
      journal: decodeJournal(reader),
      timerRegistrations: decodeTimerRegistrations(reader),
      pendingTimerTicks: decodePendingTimerTicks(reader),
      terminalCompletions: decodeCompletionVector(reader),
    };
  } else {
    throw new Error(`no golden decoder for ${formatName}`);
  }
  reader.end();
  return decoded;
}

function encodeOperationId(writer, operationId) {
  writer.u64(operationId.high).u64(operationId.low);
}

function encodeRelocationObject(writer, object) {
  const body = new FixtureWriter();
  if (object.objectKind === "actor") {
    body.text8(object.actorId).u64(object.actorGeneration)
      .u64(object.expectedAuthorityOwnerGeneration);
  } else if (object.objectKind === "userSpot") {
    body.text8(object.spotIdUtf8Fixture).u64(object.spotGeneration)
      .u64(object.expectedAuthorityOwnerGeneration);
  } else {
    body.text8(object.instanceType).text8(object.spotIdUtf8Fixture)
      .u64(object.spotGeneration);
  }
  const bytes = body.finish();
  writer.u8(fixtureEnumValue(FIXTURE_ENUMS.objectKind, object.objectKind, "object kind"))
    .u16(bytes.length).raw(bytes);
}

function encodeAuthorityObject(writer, object) {
  const body = new FixtureWriter();
  if (object.objectKind === "actor") {
    body.text8(object.actorType).text8(object.actorId)
      .u8(fixtureEnumValue(FIXTURE_ENUMS.actorAuthorityState, object.state, "Actor authority state"))
      .text8(object.currentSpotIdUtf8Fixture).u64(object.currentSpotGeneration)
      .u8(fixtureEnumValue(FIXTURE_ENUMS.actorSpotKind, object.currentSpotKind, "Actor Spot kind"));
  } else {
    const spotBody = new FixtureWriter();
    if (object.spotKind === "instance") {
      const stateBody = new FixtureWriter();
      stateBody.text8(object.instanceType).text8(object.spotIdUtf8Fixture);
      const stateBytes = stateBody.finish();
      spotBody.u8(fixtureEnumValue(FIXTURE_ENUMS.authorityState, object.authorityState, "authority state"))
        .u16(stateBytes.length).raw(stateBytes);
    } else {
      spotBody.text8(object.spotIdUtf8Fixture).text8(object.spotType)
        .u8(fixtureEnumValue(
          FIXTURE_ENUMS.domainSpotAuthorityState,
          object.state,
          "domain Spot authority state",
        ));
    }
    const spotBytes = spotBody.finish();
    body.u8(fixtureEnumValue(FIXTURE_ENUMS.spotKind, object.spotKind, "Spot kind"))
      .u16(spotBytes.length).raw(spotBytes);
  }
  const bytes = body.finish();
  writer.u8(fixtureEnumValue(FIXTURE_ENUMS.authorityObjectKind, object.objectKind, "authority object kind"))
    .u16(bytes.length).raw(bytes);
}

function encodeApplicationPayload(writer, payload) {
  const body = new FixtureWriter();
  body.text8(payload.packetName).text8(payload.contentType)
    .bytes32(Buffer.from(payload.payloadUtf8Fixture, "utf8"));
  const bytes = body.finish();
  writer.u8(1).u32(bytes.length).raw(bytes);
}

function encodeParticipantProgress(writer, progress) {
  writer.u32(progress.length);
  for (const entry of progress) {
    writer.u64(entry.participantId).u64(entry.acceptedBoundary).u64(entry.replayCursor);
  }
}

function encodeRelocationApplicationState(writer, applicationState) {
  const state = new FixtureWriter();
  if (applicationState.hasState) {
    state.bytes64(Buffer.from(applicationState.payloadUtf8Fixture, "utf8"));
  }
  const stateBytes = state.finish();
  writer.u8(applicationState.hasState ? 1 : 0).u64(stateBytes.length).raw(stateBytes);
}

function encodeParticipantApplicationStates(writer, states) {
  writer.u32(states.length);
  for (const state of states) {
    writer.u64(state.participantId);
    encodeRelocationApplicationState(writer, state.applicationState);
  }
}

function encodeTimerRegistrations(writer, registrations) {
  writer.u32(registrations.length);
  for (const registration of registrations) {
    writer.u64(registration.participantId)
      .text8(registration.name)
      .text8(registration.handlerType)
      .u64(registration.periodMilliseconds)
      .u8(fixtureEnumValue(
        FIXTURE_ENUMS.timerOverrunPolicy,
        registration.overrunPolicy,
        "timer overrun policy",
      ))
      .u64(registration.maxCatchUpTicks)
      .u8(registration.stopOnUnhandledException ? 1 : 0)
      .u64(registration.lastCompletedDeliveryIndex)
      .u64(registration.lastCompletedScheduledIndex)
      .u64(registration.nextScheduledAtUnixMilliseconds);
  }
}

function encodePendingTimerTicks(writer, ticks) {
  writer.u32(ticks.length);
  for (const tick of ticks) {
    writer.u64(tick.participantId)
      .u64(tick.sequence)
      .text8(tick.timerName)
      .u64(tick.deliveryIndex)
      .u64(tick.scheduledIndex)
      .u64(tick.scheduledAtUnixMilliseconds)
      .u64(tick.skippedTicks);
  }
}

function encodeRequestCompletion(writer, completion) {
  encodeOperationId(writer, completion.operationId);
  writer.text8(completion.requestSource.sourceOwnerId)
    .u64(completion.requestSource.sourceOwnerLeaseGeneration)
    .text8(completion.requestSource.sourceNodeRidUtf8Fixture)
    .u64(completion.requestSource.sourceNodeGeneration)
    .u64(completion.participantId).u64(completion.sequence)
    .u32(fixtureEnumValue(FIXTURE_ENUMS.terminalResult, completion.terminalResult, "terminal result"))
    .u32(completion.failureCode)
    .u8(fixtureEnumValue(
      FIXTURE_ENUMS.completionDelivery,
      completion.deliveryState,
      "completion delivery state",
    ))
    .u8(completion.payload === null ? 0 : 1);
  if (completion.payload !== null) {
    encodeApplicationPayload(writer, completion.payload);
  }
}

function encodeCompletionVector(writer, completions) {
  writer.u32(completions.length);
  for (const completion of completions) {
    encodeRequestCompletion(writer, completion);
  }
}

function encodeRelocationRootPointer(writer, relocationRoot) {
  const body = new FixtureWriter();
  if (relocationRoot !== null) {
    body.text16(relocationRoot.referenceUtf8Fixture).u32(relocationRoot.checksumCrc32c);
  }
  const bytes = body.finish();
  writer.u8(relocationRoot === null ? 0 : 1).u16(bytes.length).raw(bytes);
}

function encodeInstancePlacement(writer, placement) {
  const body = new FixtureWriter();
  body.text8(placement.targetNodeRidUtf8Fixture).u64(placement.targetNodeGeneration)
    .text8(placement.targetSpotIdUtf8Fixture)
    .u64(placement.authority.objectGeneration)
    .text8(placement.authority.ownerId)
    .u64(placement.authority.authorityOwnerGeneration)
    .u64(placement.authority.leaseGeneration)
    .text16(placement.authority.storeVersion)
    .text8(placement.instanceType);
  const bytes = body.finish();
  writer.u8(1).u16(bytes.length).raw(bytes);
}

function encodeFrozenRecord(writer, record) {
  writer.u8(fixtureEnumValue(FIXTURE_ENUMS.recordKind, record.recordKind, "record kind"))
    .u8(fixtureEnumValue(FIXTURE_ENUMS.frozenSourceKind, record.source.sourceKind, "frozen source kind"));
  const sourceBody = new FixtureWriter();
  sourceBody.text8(record.source.sourceNodeRidUtf8Fixture)
    .u64(record.source.sourceNodeGeneration)
    .text8(record.source.sourceOwnerId)
    .u64(record.source.sourceOwnerLeaseGeneration);
  if (record.source.sourceKind === "spot") {
    sourceBody.text8(record.source.sourceSpotIdUtf8Fixture);
  } else if (record.source.sourceKind === "actor" || record.source.sourceKind === "boundSession") {
    sourceBody.text8(record.source.sourceActor.actorId).u64(record.source.sourceActor.generation);
    if (record.source.sourceKind === "boundSession") {
      sourceBody.text8(record.source.sourceSessionRidUtf8Fixture)
        .u64(record.source.sourceBindingGeneration)
        .u64(record.source.sourceSessionSequence);
    }
  }
  const sourceBytes = sourceBody.finish();
  writer.u16(sourceBytes.length).raw(sourceBytes).u8(record.metadata === null ? 0 : 1);
  if (record.metadata !== null) {
    throw new Error("golden fixture encoder only supports absent metadata");
  }
  encodeOperationId(writer, record.operationId);
  writer.u32(fixtureEnumValue(FIXTURE_ENUMS.meshOperation, record.operationKind, "mesh operation kind"));
  const replyRoute = new FixtureWriter();
  if (record.replyRouteId !== null) {
    replyRoute.u64(record.replyRouteId);
  }
  const replyRouteBytes = replyRoute.finish();
  writer.u16(replyRouteBytes.length).raw(replyRouteBytes);
  if (record.recordKind === "spotRequest") {
    writer.text8(record.body.targetSpot.spotIdUtf8Fixture)
      .u64(record.body.targetSpot.spotGeneration)
      .text8(record.body.targetSpot.targetNodeRidUtf8Fixture)
      .u64(record.body.targetSpot.targetNodeGeneration)
      .u64(record.body.targetSpot.expectedAuthorityOwnerGeneration)
      .u64(record.body.targetSpot.expectedOwnerLeaseGeneration);
    encodeApplicationPayload(writer, record.body.payload);
    return;
  }
  if (record.recordKind !== "instanceSpotActivation") {
    throw new Error("golden fixture encoder requires a Spot request record");
  }
  encodeInstancePlacement(writer, record.body.placement);
  writer.u64(record.body.sourceNodeGeneration)
    .u8(fixtureEnumValue(FIXTURE_ENUMS.instanceOperation, record.body.operationKind, "Instance operation kind"));
  encodeApplicationPayload(writer, record.body.payload);
}

function encodeJournal(writer, journal) {
  writer.u32(journal.length);
  for (const entry of journal) {
    writer.u64(entry.participantId).u64(entry.sequence);
    encodeFrozenRecord(writer, entry.record);
  }
}

function encodeGoldenBody(formatName, decoded) {
  const writer = new FixtureWriter();
  if (formatName === "authority-payload-v1") {
    writer.u8(fixtureEnumValue(
      FIXTURE_ENUMS.authorityOperation,
      decoded.operationKind,
      "authority operation kind",
    ));
    encodeAuthorityObject(writer, decoded.object);
    writer.text8(decoded.ownerId).u64(decoded.ownerLeaseGeneration)
      .text8(decoded.ownerMeshName)
      .text8(decoded.ownerNodeRidUtf8Fixture)
      .u64(decoded.ownerNodeGeneration);
    const relocation = new FixtureWriter();
    if (decoded.relocationState !== null) {
      relocation.u64(decoded.relocationState.relocationHigh).u64(decoded.relocationState.relocationLow)
        .u64(decoded.relocationState.targetAttemptGeneration)
        .text8(decoded.relocationState.sourceNodeRidUtf8Fixture)
        .u64(decoded.relocationState.sourceNodeGeneration)
        .text8(decoded.relocationState.sourceOwnerId)
        .u64(decoded.relocationState.sourceOwnerLeaseGeneration)
        .text8(decoded.relocationState.targetNodeRidUtf8Fixture)
        .u64(decoded.relocationState.targetNodeGeneration)
        .text8(decoded.relocationState.targetOwnerId)
        .u64(decoded.relocationState.targetOwnerLeaseGeneration)
        .u64(decoded.relocationState.reservationGeneration)
        .text8(decoded.relocationState.coordinatorOwnerId)
        .u64(decoded.relocationState.coordinatorLeaseGeneration)
        .text8(decoded.relocationState.coordinatorNodeRidUtf8Fixture)
        .u64(decoded.relocationState.coordinatorNodeGeneration)
        .u8(fixtureEnumValue(FIXTURE_ENUMS.relocationPhase, decoded.relocationState.phase, "relocation phase"));
      encodeRelocationRootPointer(relocation, decoded.relocationState.relocationRoot);
      relocation.i64(decoded.relocationState.applicationVersion);
      encodeParticipantProgress(relocation, decoded.relocationState.participantProgress);
      relocation.u32(decoded.relocationState.terminalCompletionCount)
        .u32(decoded.relocationState.pendingRelayCount)
        .u8(fixtureEnumValue(
          FIXTURE_ENUMS.sourceCleanup,
          decoded.relocationState.sourceCleanupState,
          "source cleanup state",
        ));
    }
    const bytes = relocation.finish();
    writer.u8(decoded.relocationState === null ? 0 : 1).u32(bytes.length).raw(bytes);
    const activation = new FixtureWriter();
    if (decoded.activationRecoveryState !== null) {
      activation.text8(decoded.activationRecoveryState.referenceUtf8Fixture)
        .raw(Buffer.from(decoded.activationRecoveryState.sha256Hex, "hex"))
        .u32(decoded.activationRecoveryState.encodedSize)
        .u64(decoded.activationRecoveryState.inboxSequence)
        .u64(decoded.activationRecoveryState.replayCursor);
    }
    const activationBytes = activation.finish();
    writer.u8(decoded.activationRecoveryState === null ? 0 : 1)
      .u32(activationBytes.length)
      .raw(activationBytes);
    return writer.finish();
  }
  if (formatName === "instance-activation-recovery-v1") {
    writer.text8(decoded.targetSpotIdUtf8Fixture)
      .text8(decoded.stableType)
      .text8(decoded.targetMeshName)
      .text8(decoded.targetNodeRidUtf8Fixture)
      .u64(decoded.targetNodeGeneration)
      .text8(decoded.targetDescriptorVersion);
    writer.text8(decoded.sourceNodeRidUtf8Fixture)
      .u64(decoded.sourceNodeGeneration);
    if (decoded.sourceSpotIdUtf8Fixture === null) {
      writer.u8(0);
    } else {
      writer.u8(1).text8(decoded.sourceSpotIdUtf8Fixture);
    }
    writer.u8(fixtureEnumValue(
      FIXTURE_ENUMS.instanceOperation,
      decoded.operationKind,
      "Instance operation kind",
    )).u64(decoded.operationHigh).u64(decoded.operationLow);
    if (decoded.operationKind === "request") {
      writer.u64(decoded.replyRouteId);
    }
    writer.u64(decoded.deadlineUnixMs);
    if (decoded.metadata === null) {
      writer.u8(0);
    } else {
      writer.u8(1)
        .u8(decoded.metadata.version)
        .u8(decoded.metadata.entries.length);
      for (const entry of decoded.metadata.entries) {
        writer.text8(entry.key).text16(entry.value);
      }
    }
    const payload = new FixtureWriter();
    payload.text8(decoded.applicationPayload.packetName)
      .text8(decoded.applicationPayload.contentType)
      .bytes32(Buffer.from(decoded.applicationPayload.payloadHex, "hex"));
    const payloadBytes = payload.finish();
    writer.u8(decoded.applicationPayload.version).u32(payloadBytes.length).raw(payloadBytes);
    return writer.finish();
  }
  if (formatName === "relocation-envelope-v1") {
    writer.u64(decoded.relocationHigh).u64(decoded.relocationLow);
    encodeRelocationObject(writer, decoded.object);
    writer.i64(decoded.applicationVersion);
    encodeParticipantApplicationStates(writer, decoded.applicationStates);
    encodeParticipantProgress(writer, decoded.participantProgress);
    encodeJournal(writer, decoded.journal);
    encodeTimerRegistrations(writer, decoded.timerRegistrations);
    encodePendingTimerTicks(writer, decoded.pendingTimerTicks);
    encodeCompletionVector(writer, decoded.terminalCompletions);
    return writer.finish();
  }
  if (formatName === "relocation-data-chunk-v1") {
    writer.u32(decoded.order).bytes32(Buffer.from(decoded.dataHex, "hex"));
    return writer.finish();
  }
  if (formatName === "relocation-manifest-v1") {
    const inventoryDigest = Buffer.from(decoded.inventoryDigestSha256Hex, "hex");
    writer.u8(decoded.logicalFormatVersion)
      .u64(decoded.totalLength)
      .u32(decoded.totalChecksumCrc32c)
      .u8(inventoryDigest.length)
      .raw(inventoryDigest)
      .u32(decoded.chunks.length);
    for (const chunk of decoded.chunks) {
      writer.u32(chunk.order)
        .text16(chunk.referenceUtf8Fixture)
        .u64(chunk.length)
        .u32(chunk.checksumCrc32c);
    }
    return writer.finish();
  }
  throw new Error(`no golden encoder for ${formatName}`);
}

function encodeGoldenEnvelope(format, decoded) {
  const body = encodeGoldenBody(format.name, decoded);
  const header = Buffer.alloc(11);
  Buffer.from(format.magic).copy(header, 0);
  header.writeUInt8(format.formatVersion, 4);
  header.writeUInt16BE(format.flags, 5);
  header.writeUInt32BE(body.length, 7);
  const withoutChecksum = Buffer.concat([header, body]);
  const checksum = Buffer.alloc(4);
  checksum.writeUInt32BE(crc32c(withoutChecksum));
  return Buffer.concat([withoutChecksum, checksum]);
}

function compareUnsignedTuple(left, right) {
  for (let index = 0; index < left.length; index += 1) {
    const leftValue = BigInt(left[index]);
    const rightValue = BigInt(right[index]);
    if (leftValue < rightValue) {
      return -1;
    }
    if (leftValue > rightValue) {
      return 1;
    }
  }
  return 0;
}

function validateGoldenOrder(entries, key, location, fail) {
  for (let index = 1; index < entries.length; index += 1) {
    const comparison = compareUnsignedTuple(key(entries[index - 1]), key(entries[index]));
    if (comparison >= 0) {
      fail(location, "must be strictly sorted and unique by unsigned wire value");
      return;
    }
  }
}

function validateGoldenFixtureSemantics(formatName, decoded, location, fail) {
  if (formatName === "authority-payload-v1") {
    if (decoded.operationKind !== "steady"
        || decoded.object?.objectKind !== "spot"
        || decoded.object?.spotKind !== "instance"
        || decoded.object?.authorityState !== "ready"
        || decoded.relocationState !== null
        || decoded.activationRecoveryState === null
        || !/^[0-9a-f]{64}$/.test(decoded.activationRecoveryState.sha256Hex)
        || decoded.activationRecoveryState.encodedSize <= 0
        || BigInt(decoded.activationRecoveryState.inboxSequence) === 0n
        || BigInt(decoded.activationRecoveryState.replayCursor)
          > BigInt(decoded.activationRecoveryState.inboxSequence)) {
      fail(location, "authority golden must exercise Ready Instance cold activation recovery state");
      return;
    }
    return;
  }
  if (formatName === "instance-activation-recovery-v1") {
    const metadataKeys = decoded.metadata?.entries?.map((entry) => entry.key) ?? [];
    if (
      decoded.operationKind !== "request"
      || BigInt(decoded.operationHigh) === 0n && BigInt(decoded.operationLow) === 0n
      || BigInt(decoded.replyRouteId) === 0n
      || BigInt(decoded.deadlineUnixMs) === 0n
      || decoded.metadata?.version !== 1
      || decoded.metadata.entries.length === 0
      || metadataKeys.some((key) => key.length === 0)
      || new Set(metadataKeys).size !== metadataKeys.length
      || decoded.applicationPayload.version !== 1
      || decoded.applicationPayload.packetName.length === 0
      || decoded.applicationPayload.contentType.length === 0
    ) {
      fail(location, "Instance activation golden must exercise a complete recoverable request envelope");
    }
    return;
  }
  if (formatName === "relocation-data-chunk-v1") {
    if (!Number.isSafeInteger(decoded.order) || decoded.order !== 0
        || typeof decoded.dataHex !== "string" || !/^(?:[0-9a-f]{2})+$/.test(decoded.dataHex)) {
      fail(location, "relocation chunk golden must exercise the first non-empty immutable chunk");
    }
    return;
  }
  if (formatName === "relocation-manifest-v1") {
    if (decoded.logicalFormatVersion !== 1 || decoded.chunks.length !== 1
        || !/^[0-9a-f]{64}$/.test(decoded.inventoryDigestSha256Hex)
        || decoded.chunks[0].order !== 0
        || BigInt(decoded.totalLength) !== BigInt(decoded.chunks[0].length)
        || decoded.chunks[0].referenceUtf8Fixture.length === 0) {
      fail(location, "relocation manifest golden must contain one ordered chunk matching total length");
    }
    return;
  }
  if (formatName !== "relocation-envelope-v1") {
    return;
  }
  const progress = decoded.participantProgress;
  validateGoldenOrder(progress, (entry) => [entry.participantId], `${location}.participantProgress`, fail);
  validateGoldenOrder(
    decoded.applicationStates,
    (entry) => [entry.participantId],
    `${location}.applicationStates`,
    fail,
  );
  validateGoldenOrder(
    decoded.journal,
    (entry) => [entry.participantId, entry.sequence],
    `${location}.journal`,
    fail,
  );
  validateGoldenOrder(
    decoded.terminalCompletions,
    (entry) => [entry.participantId, entry.sequence],
    `${location}.terminalCompletions`,
    fail,
  );
  validateGoldenOrder(
    decoded.timerRegistrations,
    (entry) => [
      entry.participantId,
      `0x${Buffer.from(entry.name, "utf8").toString("hex")}`,
    ],
    `${location}.timerRegistrations`,
    fail,
  );
  validateGoldenOrder(
    decoded.pendingTimerTicks,
    (entry) => [entry.participantId, entry.sequence],
    `${location}.pendingTimerTicks`,
    fail,
  );
  const participantIds = progress.map((entry) => entry.participantId);
  const applicationStateParticipantIds = decoded.applicationStates.map(
    (entry) => entry.participantId,
  );
  if (JSON.stringify(applicationStateParticipantIds) !== JSON.stringify(participantIds)
      || !decoded.applicationStates.some(
        (entry) => entry.applicationState.hasState
          && entry.applicationState.payloadUtf8Fixture.length === 0,
      )) {
    fail(location, "relocation golden must carry one application-state entry per participant and exercise an empty Snapshot payload");
  }
  if (decoded.journal.length === 0 || decoded.timerRegistrations.length === 0
      || decoded.pendingTimerTicks.length === 0
      || decoded.terminalCompletions.length === 0 || progress.length === 0) {
    fail(location, "relocation golden must contain progress, a frozen request, logical timer state and a completion");
    return;
  }
  for (const entry of decoded.journal) {
    const participant = progress.find((candidate) => candidate.participantId === entry.participantId);
    if (!participant || BigInt(entry.sequence) > BigInt(participant.acceptedBoundary)) {
      fail(location, "journal entry must stay within its participant accepted boundary");
    }
    if (entry.record.source.sourceOwnerId.length === 0
        || BigInt(entry.record.source.sourceOwnerLeaseGeneration) === 0n) {
      fail(location, "every durable journal record must have an exact lease-backed source fence");
    }
    if (entry.record.source.sourceKind === "boundSession"
        && entry.record.operationKind === "actorRequest") {
      fail(location, "bound-session requests must reach terminal before Captured and cannot enter the journal");
    }
  }
  for (const completion of decoded.terminalCompletions) {
    const matching = decoded.journal.find((entry) => (
      entry.participantId === completion.participantId
      && entry.sequence === completion.sequence
      && JSON.stringify(entry.record.operationId) === JSON.stringify(completion.operationId)
      && entry.record.source.sourceOwnerId === completion.requestSource.sourceOwnerId
      && entry.record.source.sourceOwnerLeaseGeneration
        === completion.requestSource.sourceOwnerLeaseGeneration
      && entry.record.source.sourceNodeRidUtf8Fixture
        === completion.requestSource.sourceNodeRidUtf8Fixture
      && entry.record.source.sourceNodeGeneration
        === completion.requestSource.sourceNodeGeneration
      && entry.record.replyRouteId !== null
      && entry.record.source.sourceOwnerId.length > 0
      && BigInt(entry.record.source.sourceOwnerLeaseGeneration) > 0n
      && (
        (entry.record.recordKind === "instanceSpotActivation"
          && entry.record.body.operationKind === "request")
        || (entry.record.recordKind === "spotRequest"
          && entry.record.operationKind === "spotRequest")
      )
    ));
    if (!matching) {
      fail(location, "terminal completion must match an accepted request record");
    }
    if (!["pending", "terminalReceived", "alreadyTerminal", "sourceLeaseExpired"].includes(
      completion.deliveryState,
    )) {
      fail(location, "terminal completion must carry a closed monotonic delivery state");
    }
  }
  for (const registration of decoded.timerRegistrations) {
    if (BigInt(registration.periodMilliseconds) === 0n
        || BigInt(registration.maxCatchUpTicks) === 0n
        || registration.name.length === 0
        || registration.handlerType.length === 0) {
      fail(location, "logical timer registration must be restorable without a native timer handle");
    }
  }
  for (const tick of decoded.pendingTimerTicks) {
    const participant = progress.find((candidate) => candidate.participantId === tick.participantId);
    const registration = decoded.timerRegistrations.find(
      (candidate) => candidate.participantId === tick.participantId
        && candidate.name === tick.timerName,
    );
    if (!participant || BigInt(tick.sequence) > BigInt(participant.acceptedBoundary)
        || !registration) {
      fail(location, "pending timer tick must reference a registered timer within its participant boundary");
    }
  }
  const mergedQueue = [
    ...decoded.journal.map((entry) => ({
      participantId: entry.participantId,
      sequence: entry.sequence,
    })),
    ...decoded.pendingTimerTicks.map((entry) => ({
      participantId: entry.participantId,
      sequence: entry.sequence,
    })),
  ].sort((left, right) => compareUnsignedTuple(
    [left.participantId, left.sequence],
    [right.participantId, right.sequence],
  ));
  for (let index = 1; index < mergedQueue.length; index += 1) {
    if (compareUnsignedTuple(
      [mergedQueue[index - 1].participantId, mergedQueue[index - 1].sequence],
      [mergedQueue[index].participantId, mergedQueue[index].sequence],
    ) === 0) {
      fail(location, "journal and pending timer tick cannot reuse one participant sequence");
    }
  }
}

function validateGoldenFixtureData(format, fixture, location, fail) {
  if (!isObject(fixture) || fixture.format !== format.name) {
    fail(`${location}.format`, `must identify ${format.name}`);
    return;
  }
  const expectedConsumers = ["cpp", "dotnet", "jvm", "node"];
  if (JSON.stringify(fixture.consumers) !== JSON.stringify(expectedConsumers)) {
    fail(`${location}.consumers`, "must list cpp, dotnet, jvm and node in canonical order");
  }
  if (typeof fixture.encodedHex !== "string" || !/^(?:[0-9a-f]{2})+$/.test(fixture.encodedHex)) {
    fail(`${location}.encodedHex`, "must be non-empty lowercase whole-byte hex");
    return;
  }
  if (!isObject(fixture.decoded)) {
    fail(`${location}.decoded`, "must provide the language-neutral semantic value");
  }
  const bytes = Buffer.from(fixture.encodedHex, "hex");
  if (bytes.length < 15) {
    fail(`${location}.encodedHex`, "durable envelope is shorter than its fixed header and checksum");
    return;
  }
  if (!Buffer.from(format.magic).equals(bytes.subarray(0, 4))) {
    fail(`${location}.encodedHex`, "magic does not match the durable format");
  }
  if (bytes[4] !== format.formatVersion || bytes.readUInt16BE(5) !== format.flags) {
    fail(`${location}.encodedHex`, "version or flags do not match the durable format");
  }
  const bodyLength = bytes.readUInt32BE(7);
  if (bytes.length !== 11 + bodyLength + 4) {
    fail(`${location}.encodedHex`, "body length does not cover the exact body");
    return;
  }
  const expectedChecksum = bytes.readUInt32BE(bytes.length - 4);
  const actualChecksum = crc32c(bytes.subarray(0, bytes.length - 4));
  if (actualChecksum !== expectedChecksum) {
    fail(`${location}.encodedHex`, "trailing CRC32C does not cover magic through body");
  }
  try {
    const encodedBody = bytes.subarray(11, bytes.length - 4);
    const decoded = decodeGoldenBody(format.name, encodedBody);
    if (JSON.stringify(decoded) !== JSON.stringify(fixture.decoded)) {
      fail(`${location}.decoded`, "does not match the canonical bytes");
    }
    const reencoded = encodeGoldenBody(format.name, fixture.decoded);
    if (!reencoded.equals(encodedBody)) {
      fail(`${location}.decoded`, "does not re-encode to the canonical body bytes");
    }
    validateGoldenFixtureSemantics(format.name, decoded, `${location}.decoded`, fail);
  } catch (error) {
    fail(`${location}.encodedHex`, error.message);
  }
}

function validateGoldenFixtures(schema, schemaPath) {
  const errors = [];
  const fail = (location, message) => errors.push(`${location}: ${message}`);
  const directory = path.dirname(schemaPath);
  const fixtures = new Map();
  for (const [index, format] of schema.durableFormats.entries()) {
    const fixturePath = path.resolve(directory, format.goldenFixture);
    let fixture;
    try {
      fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
    } catch (error) {
      fail(`fixture:${format.name}`, `cannot read ${format.goldenFixture}: ${error.message}`);
      continue;
    }
    validateGoldenFixtureData(format, fixture, `fixture:${format.name}`, fail);
    fixtures.set(format.name, fixture);
  }
  const authorityFixture = fixtures.get("authority-payload-v1");
  const activationFixture = fixtures.get("instance-activation-recovery-v1");
  if (authorityFixture && activationFixture) {
    const activationBytes = Buffer.from(activationFixture.encodedHex, "hex");
    const activationSha256 = crypto.createHash("sha256").update(activationBytes).digest("hex");
    if (authorityFixture.decoded.activationRecoveryState?.encodedSize !== activationBytes.length
        || authorityFixture.decoded.activationRecoveryState?.sha256Hex !== activationSha256) {
      fail("fixture:authority-payload-v1",
        "Ready Instance activation pointer must reference the exact ZLIA golden bytes");
    }
  }
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return schema.durableFormats.length;
}

function validateRelocationLogicalFixture(schema, schemaPath) {
  const errors = [];
  const fail = (location, message) => errors.push(`${location}: ${message}`);
  const profile = schema.relocationLogicalStreamFormat;
  const fixturePath = path.resolve(path.dirname(schemaPath), profile.goldenFixture);
  let fixture;
  try {
    fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  } catch (error) {
    throw new SchemaValidationError([`fixture:${profile.name}: cannot read fixture: ${error.message}`]);
  }
  const location = `fixture:${profile.name}`;
  if (fixture.format !== profile.name
      || JSON.stringify(fixture.consumers) !== JSON.stringify(["cpp", "dotnet", "jvm", "node"])) {
    fail(location, "must identify the logical relocation and all four runtime consumers");
  }
  if (fixture.durabilityBoundary !== schema.maintenanceAdmissionProfile.durableReplayBoundary) {
    fail(`${location}.durabilityBoundary`, "must begin only after the complete root is linked by Captured CAS");
  }
  if (typeof fixture.logicalHex !== "string" || !/^(?:[0-9a-f]{2})+$/.test(fixture.logicalHex)) {
    fail(`${location}.logicalHex`, "must be non-empty lowercase whole-byte hex without a provider envelope");
  } else {
    try {
      const bytes = Buffer.from(fixture.logicalHex, "hex");
      const decoded = decodeGoldenBody(profile.name, bytes);
      if (JSON.stringify(decoded) !== JSON.stringify(fixture.decoded)) {
        fail(`${location}.decoded`, "does not match the canonical logical bytes");
      }
      if (!encodeGoldenBody(profile.name, fixture.decoded).equals(bytes)) {
        fail(`${location}.decoded`, "does not re-encode to the logical bytes");
      }
      validateGoldenFixtureSemantics(profile.name, decoded, `${location}.decoded`, fail);

      const chunkFixture = JSON.parse(fs.readFileSync(path.resolve(
        path.dirname(schemaPath),
        schema.relocationStorageProfile.chunkFormat === "relocation-data-chunk-v1"
          ? "golden/relocation-data-chunk-v1.json" : "",
      ), "utf8"));
      const manifestFixture = JSON.parse(fs.readFileSync(path.resolve(
        path.dirname(schemaPath),
        schema.relocationStorageProfile.manifestFormat === "relocation-manifest-v1"
          ? "golden/relocation-manifest-v1.json" : "",
      ), "utf8"));
      const amendmentFixture = JSON.parse(fs.readFileSync(path.resolve(
        path.dirname(schemaPath),
        "golden/contract-amendment-v1.json",
      ), "utf8"));
      const chunkBytes = Buffer.from(chunkFixture.decoded.dataHex, "hex");
      const manifest = manifestFixture.decoded;
      if (!chunkBytes.equals(bytes)
          || BigInt(manifest.totalLength) !== BigInt(bytes.length)
          || manifest.totalChecksumCrc32c !== crc32c(bytes)
          || manifest.chunks.length !== 1
          || BigInt(manifest.chunks[0].length) !== BigInt(bytes.length)
          || manifest.chunks[0].checksumCrc32c !== crc32c(bytes)) {
        fail(location, "logical bytes, immutable chunk and root manifest length/checksum relation must match exactly");
      }
      if (manifest.inventoryDigestSha256Hex !== amendmentFixture.aggregate.inventoryDigestSha256Hex
          || amendmentFixture.aggregate.relocationManifestDigestSha256Hex
            !== amendmentFixture.aggregate.inventoryDigestSha256Hex
          || amendmentFixture.aggregate.relocationManifestAuthority !== false) {
        fail(location, "Location aggregate and lookup-only Relocation manifest inventory digests must match exactly");
      }
    } catch (error) {
      fail(`${location}.logicalHex`, error.message);
    }
  }
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return 1;
}

function runRelocationLogicalFixtureSelfTests(schema, schemaPath) {
  const profile = schema.relocationLogicalStreamFormat;
  const fixturePath = path.resolve(path.dirname(schemaPath), profile.goldenFixture);
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const tests = [
    ["journal and timer reuse participant sequence", (candidate) => {
      candidate.decoded.pendingTimerTicks[0].sequence = candidate.decoded.journal[0].sequence;
    }],
    ["participant application state omitted", (candidate) => {
      candidate.decoded.applicationStates.pop();
    }],
    ["participant application states out of order", (candidate) => {
      candidate.decoded.applicationStates.reverse();
    }],
  ];
  for (const [label, mutate] of tests) {
    const candidate = clone(fixture);
    mutate(candidate);
    const bytes = encodeGoldenBody(profile.name, candidate.decoded);
    const decoded = decodeGoldenBody(profile.name, bytes);
    const errors = [];
    validateGoldenFixtureSemantics(
      profile.name,
      decoded,
      `logical-fixture-self-test:${label}`,
      (location, message) => errors.push(`${location}: ${message}`),
    );
    if (errors.length === 0) {
      throw new Error(`negative self-test did not fail: ${profile.name}: ${label}`);
    }
  }
  return tests.length;
}

function validateFrameworkJsonFixture(schema, schemaPath) {
  const profile = schema.frameworkJsonV1Profile;
  const fixturePath = path.resolve(path.dirname(schemaPath), profile.goldenFixture);
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const errors = [];
  const fail = (message) => errors.push(`fixture:${profile.name}: ${message}`);
  if (fixture.format !== profile.name
      || JSON.stringify(fixture.consumers) !== JSON.stringify(["cpp", "dotnet", "jvm", "node"])) {
    fail("must identify framework-json-v1 and all four runtime consumers");
  }
  const required = ["signed64", "unsigned64", "int32", "ratio", "state", "bytes", "nullable", "labels"];
  if (JSON.stringify(fixture.contract) !== JSON.stringify({
    requiredProperties: required,
    enumNames: ["Ready", "Closed"],
  })) {
    fail("contract fixture does not match the exact primitive profile");
  }
  const project = (jsonUtf8) => {
    if (Buffer.from(jsonUtf8, "utf8").subarray(0, 3).equals(Buffer.from([0xef, 0xbb, 0xbf]))) {
      throw new Error("BOM is forbidden");
    }
    const names = [...jsonUtf8.matchAll(/"([^"\\]*(?:\\.[^"\\]*)*)"\s*:/g)].map((match) => match[1]);
    if (new Set(names).size !== names.length) {
      throw new Error("duplicate property");
    }
    const value = JSON.parse(jsonUtf8);
    for (const name of required) {
      if (!hasOwn(value, name)) {
        throw new Error(`missing ${name}`);
      }
    }
    if (!/^(?:0|-?[1-9][0-9]*)$/.test(value.signed64)
        || BigInt(value.signed64) < -9223372036854775808n
        || BigInt(value.signed64) > 9223372036854775807n
        || !/^(?:0|[1-9][0-9]*)$/.test(value.unsigned64)
        || BigInt(value.unsigned64) > 18446744073709551615n) {
      throw new Error("invalid 64-bit decimal string");
    }
    if (!Number.isInteger(value.int32) || value.int32 < -2147483648 || value.int32 > 2147483647
        || typeof value.ratio !== "number" || !Number.isFinite(value.ratio)
        || !fixture.contract.enumNames.includes(value.state)
        || typeof value.bytes !== "string"
        || Buffer.from(value.bytes, "base64").toString("base64") !== value.bytes
        || value.nullable !== null || !isObject(value.labels)) {
      throw new Error("primitive profile mismatch");
    }
    return Object.fromEntries(required.map((name) => [name, value[name]]));
  };
  let first;
  for (const item of fixture.valid ?? []) {
    try {
      const projected = project(item.jsonUtf8);
      first ??= projected;
      if (JSON.stringify(projected) !== JSON.stringify(first)) {
        fail(`valid case ${item.name} is not semantically equivalent`);
      }
    } catch (error) {
      fail(`valid case ${item.name} failed: ${error.message}`);
    }
  }
  const expectedInvalid = [
    ["duplicate-property", "duplicateProperties"],
    ["property-case", "missingRequiredProperties"],
    ["numeric-u64", "signedAndUnsigned64"],
    ["unpadded-base64", "bytes"],
    ["fractional-int32", "integersUpTo32Bits"],
    ["unknown-enum-case", "enums"],
  ];
  if (JSON.stringify((fixture.invalid ?? []).map(({ name, reason }) => [name, reason]))
      !== JSON.stringify(expectedInvalid)) {
    fail("invalid cases must cover the exact primitive rejection matrix");
  }
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return 1;
}

function decodeFrameworkMultipartFrame(profile, encodedHex) {
  if (typeof encodedHex !== "string" || !/^(?:[0-9a-f]{2})+$/.test(encodedHex)) {
    throw new Error("must be non-empty lowercase whole-byte hex");
  }
  const bytes = Buffer.from(encodedHex, "hex");
  if (bytes.length < 11 || bytes[0] !== 1) {
    throw new Error("invalid application envelope header");
  }
  const bodyLength = bytes.readUInt32BE(1);
  if (bodyLength !== bytes.length - 5) {
    throw new Error("body length does not cover the exact envelope body");
  }
  let offset = 5;
  const packetLength = bytes[offset++];
  if (packetLength === 0 || bytes.length - offset < packetLength + 1) {
    throw new Error("packet name is truncated or empty");
  }
  const packetName = bytes.subarray(offset, offset + packetLength).toString("utf8");
  offset += packetLength;
  if (packetName !== profile.packetName) {
    throw new Error("packet name does not match framework-multipart-v1");
  }
  const contentLength = bytes[offset++];
  if (contentLength === 0 || bytes.length - offset < contentLength + 4) {
    throw new Error("content type is truncated or empty");
  }
  const contentType = bytes.subarray(offset, offset + contentLength).toString("utf8");
  offset += contentLength;
  if (contentType !== profile.contentType) {
    throw new Error("content type does not match framework-multipart-v1");
  }
  const payloadLength = bytes.readUInt32BE(offset);
  offset += 4;
  if (payloadLength !== bytes.length - offset) {
    throw new Error("payload length does not cover the exact payload");
  }

  const payload = bytes.subarray(offset);
  if (payload.length < 4) {
    throw new Error("multipart payload is missing the part count");
  }
  const count = payload.readUInt32BE(0);
  if (count < profile.minimumParts) {
    throw new Error("multipart payload must contain at least one part");
  }
  const remaining = payload.length - 4;
  if (count > Math.floor(remaining / 4)) {
    throw new Error("multipart part count exceeds the remaining length");
  }

  const partsHex = [];
  offset = 4;
  for (let index = 0; index < count; index++) {
    if (payload.length - offset < 4) {
      throw new Error("multipart part length is truncated");
    }
    const length = payload.readUInt32BE(offset);
    offset += 4;
    if (length > payload.length - offset) {
      throw new Error("multipart part is truncated");
    }
    partsHex.push(payload.subarray(offset, offset + length).toString("hex"));
    offset += length;
  }
  if (offset !== payload.length) {
    throw new Error("multipart payload has trailing bytes");
  }
  return { partsHex };
}

function encodeFrameworkMultipartFrame(profile, partsHex) {
  if (!Array.isArray(partsHex) || partsHex.length < profile.minimumParts) {
    throw new Error("multipart fixture must contain at least one part");
  }
  const parts = partsHex.map((part, index) => {
    if (typeof part !== "string" || !/^(?:[0-9a-f]{2})*$/.test(part)) {
      throw new Error(`part ${index} must be lowercase whole-byte hex`);
    }
    return Buffer.from(part, "hex");
  });
  const payloadLength = 4 + parts.reduce((total, part) => total + 4 + part.length, 0);
  const payload = Buffer.alloc(payloadLength);
  payload.writeUInt32BE(parts.length, 0);
  let payloadOffset = 4;
  for (const part of parts) {
    payload.writeUInt32BE(part.length, payloadOffset);
    payloadOffset += 4;
    part.copy(payload, payloadOffset);
    payloadOffset += part.length;
  }
  const packet = Buffer.from(profile.packetName, "utf8");
  const contentType = Buffer.from(profile.contentType, "utf8");
  const bodyLength = 1 + packet.length + 1 + contentType.length + 4 + payload.length;
  const frame = Buffer.alloc(1 + 4 + bodyLength);
  frame[0] = 1;
  frame.writeUInt32BE(bodyLength, 1);
  let offset = 5;
  frame[offset++] = packet.length;
  packet.copy(frame, offset);
  offset += packet.length;
  frame[offset++] = contentType.length;
  contentType.copy(frame, offset);
  offset += contentType.length;
  frame.writeUInt32BE(payload.length, offset);
  offset += 4;
  payload.copy(frame, offset);
  return frame;
}

function validateFrameworkMultipartFixtureData(fixture, profile, location, fail) {
  if (fixture.format !== profile.name
      || JSON.stringify(fixture.consumers) !== JSON.stringify(profile.consumers)
      || fixture.encoding !== profile.encoding
      || fixture.trailingBytes !== profile.trailingBytes) {
    fail(location, "must identify framework-multipart-v1, its four runtime consumers and exact encoding rules");
  }

  const valid = fixture.valid ?? [];
  if (valid.length !== 1) {
    fail(`${location}.valid`, "must contain exactly one canonical multipart fixture");
  }
  for (const item of valid) {
    try {
      const decoded = decodeFrameworkMultipartFrame(profile, item.encodedHex);
      if (JSON.stringify(decoded.partsHex) !== JSON.stringify(item.partsHex)) {
        fail(`${location}.valid.${item.name}`, "decoded parts do not match the canonical fixture");
      }
      const reencoded = encodeFrameworkMultipartFrame(profile, item.partsHex);
      if (reencoded.toString("hex") !== item.encodedHex) {
        fail(`${location}.valid.${item.name}`, "parts do not re-encode to the canonical frame bytes");
      }
    } catch (error) {
      fail(`${location}.valid.${item.name}`, `failed: ${error.message}`);
    }
  }

  const expectedInvalid = [
    ["wrong-content-type", "contentType"],
    ["zero-count", "minimumParts"],
    ["truncated-part", "partLength"],
    ["trailing-byte", "trailingBytes"],
  ];
  if (JSON.stringify((fixture.invalid ?? []).map(({ name, reason }) => [name, reason]))
      !== JSON.stringify(expectedInvalid)) {
    fail(`${location}.invalid`, "must cover the exact multipart rejection matrix");
  }
  for (const item of fixture.invalid ?? []) {
    try {
      decodeFrameworkMultipartFrame(profile, item.encodedHex);
      fail(`${location}.invalid.${item.name}`, "negative fixture was accepted");
    } catch {
      // The fixture is valid only when the common decoder rejects it.
    }
  }
}

function validateFrameworkMultipartFixture(schema, schemaPath) {
  const profile = schema.frameworkMultipartV1Profile;
  const fixturePath = path.resolve(path.dirname(schemaPath), profile.goldenFixture);
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const errors = [];
  validateFrameworkMultipartFixtureData(
    fixture,
    profile,
    `fixture:${profile.name}`,
    (location, message) => errors.push(`${location}: ${message}`),
  );
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return 1;
}

function isAuthorityKeyUnreserved(byte) {
  return (byte >= 0x41 && byte <= 0x5a)
    || (byte >= 0x61 && byte <= 0x7a)
    || (byte >= 0x30 && byte <= 0x39)
    || [0x2d, 0x2e, 0x5f, 0x7e].includes(byte);
}

function encodeAuthorityKeyComponent(bytes) {
  let encoded = "";
  for (const byte of bytes) {
    encoded += isAuthorityKeyUnreserved(byte)
      ? String.fromCharCode(byte)
      : `%${byte.toString(16).toUpperCase().padStart(2, "0")}`;
  }
  return encoded;
}

function decodeAuthorityKeyComponent(encoded) {
  const bytes = [];
  for (let index = 0; index < encoded.length;) {
    const code = encoded.charCodeAt(index);
    if (code === 0x25) {
      const hex = encoded.slice(index + 1, index + 3);
      if (!/^[0-9A-F]{2}$/.test(hex)) {
        throw new Error("authority key percent escapes must use uppercase two-digit hex");
      }
      bytes.push(Number.parseInt(hex, 16));
      index += 3;
      continue;
    }
    if (code > 0x7f || !isAuthorityKeyUnreserved(code)) {
      throw new Error("authority key literal bytes must be RFC3986 unreserved ASCII");
    }
    bytes.push(code);
    index += 1;
  }
  return Buffer.from(bytes);
}

function encodeAuthorityKey(kind, identityBytes) {
  const discriminator = kind === "actor" ? "a" : kind === "spot" ? "s" : null;
  if (discriminator === null) {
    throw new Error(`unknown authority key kind ${kind}`);
  }
  return `zla1:${discriminator}:${identityBytes.length}:${encodeAuthorityKeyComponent(identityBytes)}`;
}

function decodeAuthorityKey(encoded) {
  const parts = encoded.split(":");
  if (parts.length !== 4 || parts[0] !== "zla1" || !["a", "s"].includes(parts[1])) {
    throw new Error("authority key must use the exact prefix, kind and component order");
  }
  const parseLength = (value) => {
    if (!/^[1-9][0-9]*$/.test(value)) {
      throw new Error("authority key lengths must be base-10 without a leading zero");
    }
    const parsed = Number(value);
    if (!Number.isSafeInteger(parsed) || parsed < 1 || parsed > 255) {
      throw new Error("authority key component length must be in 1..255");
    }
    return parsed;
  };
  const identityLength = parseLength(parts[2]);
  const identityBytes = decodeAuthorityKeyComponent(parts[3]);
  if (identityLength < 1 || identityLength > 255) {
    throw new Error("authority identity must contain 1..255 raw bytes");
  }
  if (identityBytes.length !== identityLength) {
    throw new Error("authority key raw byte length does not match its declared length");
  }
  if (!Buffer.from(identityBytes.toString("utf8"), "utf8").equals(identityBytes)) {
    throw new Error(`authority key ${parts[1] === "a" ? "Actor" : "Spot"} ID must be valid UTF-8`);
  }
  const objectKind = parts[1] === "a" ? "actor" : "spot";
  if (encodeAuthorityKey(objectKind, identityBytes) !== encoded) {
    throw new Error("authority key is not in canonical encoding");
  }
  return { objectKind, identityBytes };
}

function validateAuthorityKeyFixtureData(fixture, location, fail) {
  if (!isObject(fixture) || fixture.format !== "authority-key-v1") {
    fail(`${location}.format`, "must identify authority-key-v1");
    return;
  }
  if (JSON.stringify(fixture.consumers) !== JSON.stringify(["cpp", "dotnet", "jvm", "node"])) {
    fail(`${location}.consumers`, "must list cpp, dotnet, jvm and node in canonical order");
  }
  if (!Array.isArray(fixture.cases) || fixture.cases.length < 2
      || !fixture.cases.some((entry) => entry?.objectKind === "actor")
      || !fixture.cases.some((entry) => entry?.objectKind === "spot")
      || fixture.cases.findIndex((entry) => entry?.objectKind === "spot")
        < fixture.cases.map((entry) => entry?.objectKind).lastIndexOf("actor")) {
    fail(`${location}.cases`, "must contain Actor cases followed by shared Spot cases");
    return;
  }
  const encodedKeys = new Set();
  fixture.cases.forEach((entry, index) => {
    const caseLocation = `${location}.cases[${index}]`;
    if (typeof entry.identityHex !== "string"
        || !/^(?:[0-9a-f]{2})+$/.test(entry.identityHex)
        || typeof entry.encoded !== "string") {
      fail(caseLocation, "must provide lowercase identity hex and encoded key");
      return;
    }
    const identityBytes = Buffer.from(entry.identityHex, "hex");
    if (entry.objectKind === "spot"
        && JSON.stringify(entry.sharedBySpotKinds) !== JSON.stringify(["entry", "user", "instance"])) {
      fail(caseLocation, "Spot key must be shared by Entry, User and Instance kinds");
    }
    try {
      const decoded = decodeAuthorityKey(entry.encoded);
      if (decoded.objectKind !== entry.objectKind
          || !decoded.identityBytes.equals(identityBytes)
          || encodeAuthorityKey(entry.objectKind, identityBytes) !== entry.encoded) {
        fail(caseLocation, "does not match the canonical semantic value");
      }
      if (encodedKeys.has(entry.encoded)) {
        fail(caseLocation, "duplicates a canonical authority key");
      }
      encodedKeys.add(entry.encoded);
    } catch (error) {
      fail(`${caseLocation}.encoded`, error.message);
    }
  });
}

function validateAuthorityKeyFixture(schema, schemaPath) {
  const errors = [];
  const fail = (location, message) => errors.push(`${location}: ${message}`);
  const fixturePath = path.resolve(path.dirname(schemaPath), schema.authorityKeyFormat.goldenFixture);
  let fixture;
  try {
    fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  } catch (error) {
    throw new SchemaValidationError([`fixture:authority-key-v1: cannot read fixture: ${error.message}`]);
  }
  validateAuthorityKeyFixtureData(fixture, "fixture:authority-key-v1", fail);
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return 1;
}

function validateRelocationStateMachine(machine, commands, types, fail) {
  if (!isObject(machine)) {
    fail("$.relocationStateMachine", "must define relocation phases and command rules");
    return;
  }
  const phaseType = types.get(machine.phaseType?.$ref);
  if (phaseType?.kind !== "enum") {
    fail("$.relocationStateMachine.phaseType", "must reference the relocation phase enum");
    return;
  }
  const phases = new Set(phaseType.values.map((entry) => entry.name));
  const expectedCommitOrder = [
    {
      phase: "preparing",
      after: ["local-admission-seal", "accepted-boundaries-fixed"],
      relocation: "absent",
      targetReservation: "absent",
    },
    {
      phase: "captured",
      after: ["immutable-relocation-put"],
      relocation: "present",
      targetReservation: "absent",
    },
    {
      phase: "prepared",
      after: [
        "relocation-reserved-ack-validated",
        "target-factory-restore-complete",
        "journal-timer-staging-complete",
      ],
      relocation: "present",
      targetReservation: "present",
    },
  ];
  if (JSON.stringify(machine.authorityCommitOrder) !== JSON.stringify(expectedCommitOrder)) {
    fail("$.relocationStateMachine.authorityCommitOrder",
      "must CAS Preparing after the local seal, Captured after relocation Put, then Prepared after target restore and staging");
  }
  const transitionSet = new Set();
  if (!Array.isArray(machine.transitions)) {
    fail("$.relocationStateMachine.transitions", "must be an array");
  } else {
    machine.transitions.forEach((transition, index) => {
      const location = `$.relocationStateMachine.transitions[${index}]`;
      if (!phases.has(transition?.from) || !phases.has(transition?.to)) {
        fail(location, "transition references an unknown phase");
        return;
      }
      const signature = `${transition.from}->${transition.to}`;
      if (transitionSet.has(signature)) {
        fail(location, "duplicates relocation transition");
      }
      transitionSet.add(signature);
      if (["completed", "aborted"].includes(transition.from)) {
        fail(location, "terminal relocation phase cannot have an outgoing transition");
      }
    });
  }
  const expectedTransitions = new Set([
    "none->preparing",
    "preparing->captured",
    "captured->prepared",
    "prepared->committed",
    "committed->activating",
    "activating->committed",
    "activating->activated",
    "activated->committed",
    "activated->cleaning",
    "cleaning->committed",
    "cleaning->completed",
    "preparing->aborted",
    "captured->aborted",
    "prepared->aborted",
  ]);
  if (transitionSet.size !== expectedTransitions.size
      || [...transitionSet].some((transition) => !expectedTransitions.has(transition))) {
    fail("$.relocationStateMachine.transitions", "must match the exact v1 relocation transition graph");
  }
  const commandNames = new Set((commands ?? []).map((command) => command.name));
  const roleType = types.get("relocation-role");
  const roles = new Set(roleType?.values.map((entry) => entry.name) ?? []);
  const ruleCommands = new Set();
  if (!Array.isArray(machine.commandRules)) {
    fail("$.relocationStateMachine.commandRules", "must be an array");
    return;
  }
  const expectedRules = new Map([
    ["sessionRelocationSeal", {
      command: "sessionRelocationSeal",
      senderRoles: ["source", "coordinator"],
      phases: ["preparing"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-preparing-authority",
      loss: "retransmit-until-sealed-or-deadline",
    }],
    ["sessionRelocationSealed", {
      command: "sessionRelocationSealed",
      senderKind: "sessionOwner",
      phases: ["preparing"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-matching-sessionRelocationSeal",
      loss: "retransmit-until-captured-or-abort",
    }],
    ["sessionRelocationRoute", {
      command: "sessionRelocationRoute",
      senderRoles: ["source", "target", "coordinator"],
      phases: ["aborted", "completed"],
      actionRules: {
        abort: { senderRoles: ["source", "coordinator"], phases: ["aborted"] },
        commit: { senderRoles: ["target", "coordinator"], phases: ["completed"] },
      },
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "commit-after-replay-or-abort-after-authority-decision",
      loss: "retransmit-until-routed-ack",
    }],
    ["sessionRelocationRouted", {
      command: "sessionRelocationRouted",
      senderKind: "sessionOwner",
      phases: ["aborted", "completed"],
      actionRules: {
        abort: { phases: ["aborted"] },
        commit: { phases: ["completed"] },
      },
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-matching-route-action",
      loss: "retransmit-until-source-reopen-or-target-ready",
    }],
    ["relocationPrepare", {
      command: "relocationPrepare",
      senderRoles: ["source", "coordinator"],
      phases: ["captured", "prepared", "committed", "activating", "activated", "cleaning"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-captured-authority",
      loss: "retransmit-until-deadline",
    }],
    ["relocationReady", {
      command: "relocationReady",
      senderRoles: ["source", "target", "coordinator"],
      phases: ["captured", "prepared", "committed", "activating", "activated", "cleaning"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-matching-relocationPrepare-or-target-offer",
      loss: "retransmit-until-deadline",
    }],
    ["relocationReserved", {
      command: "relocationReserved",
      senderRoles: ["target"],
      phases: ["captured", "prepared", "committed", "activating", "activated", "cleaning"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-matching-source-accept",
      loss: "retransmit-until-prepared-or-deadline",
    }],
    ["relocationData", {
      command: "relocationData",
      senderRoles: ["source", "coordinator"],
      phases: ["prepared", "committed", "activating"],
      duplicate: "idempotent-by-stable-relocation-id-target-attempt-generation-participant-sequence",
      reorder: "stage-by-participant-sequence",
      loss: "recover-from-relocation-or-retransmit",
    }],
    ["relocationAck", {
      command: "relocationAck",
      senderRoles: ["target"],
      phases: ["prepared", "committed", "activating"],
      duplicate: "keep-monotonic-high-water-by-stable-relocation-id-target-attempt-generation-participant",
      reorder: "ignore-lower-high-water",
      loss: "data-retransmit-regenerates-ack",
    }],
    ["replyRelay", {
      command: "replyRelay",
      contextRules: [
        {
          context: "coldActivation",
          senderRoles: ["target"],
          authorityStateCompletionPairs: [
            { authorityState: "ready", completionKind: "readyBarrier" },
            { authorityState: "coldActivating", completionKind: "activationFailure" },
          ],
          duplicate: "terminal-once-by-operation-id",
          reorder: "hold-until-operation-known",
          loss: "source-operation-deadline-only-no-replay-restart",
        },
        {
          context: "maintenanceRelocation",
          senderRoles: ["target"],
          phases: ["committed", "activating", "activated", "cleaning"],
          duplicate: "terminal-once-by-stable-relocation-id-exact-request-source-fence-and-operation-id-target-attempt-is-peer-fence-only",
          reorder: "hold-until-operation-known",
          loss: "recover-from-durable-completion",
        },
      ],
    }],
    ["relocationSeal", {
      command: "relocationSeal",
      senderRoles: ["source", "target", "coordinator"],
      phases: ["prepared", "committed", "activating"],
      duplicate: "idempotent-if-identical-else-protocol-error",
      reorder: "hold-until-participant-boundary-reached",
      loss: "retransmit-until-deadline",
    }],
    ["replyRelayAck", {
      command: "replyRelayAck",
      senderKind: "requestSource",
      phases: ["committed", "activating", "activated", "cleaning"],
      duplicate: "idempotent-by-stable-relocation-id-exact-request-source-fence-operation-id-reply-route-id-and-status",
      reorder: "hold-until-matching-durable-completion-or-retransmit-causes-source-reack",
      loss: "target-retransmits-terminal-until-ack-or-exact-request-source-owner-lease-expiry",
    }],
    ["relocationComplete", {
      command: "relocationComplete",
      senderRoles: ["source", "coordinator"],
      phases: ["activated", "cleaning", "completed"],
      duplicate: "idempotent-by-stable-relocation-id-target-attempt-generation-and-source-cleanup-state",
      reorder: "hold-until-activated-sealed-and-durable-source-cleanup-terminal",
      loss: "recover-from-durable-authority",
    }],
  ]);
  machine.commandRules.forEach((rule, index) => {
    const location = `$.relocationStateMachine.commandRules[${index}]`;
    if (!commandNames.has(rule?.command)) {
      fail(`${location}.command`, "references an unknown command");
    } else if (ruleCommands.has(rule.command)) {
      fail(`${location}.command`, "duplicates command state rule");
    }
    ruleCommands.add(rule?.command);
    const expected = expectedRules.get(rule?.command);
    if (!expected || JSON.stringify(rule) !== JSON.stringify(expected)) {
      fail(location, `command state rule for ${rule?.command} does not match the exact v1 policy`);
    }
  });
  for (const command of [
    "sessionRelocationSeal", "sessionRelocationSealed", "sessionRelocationRoute", "sessionRelocationRouted",
    "relocationPrepare", "relocationReady", "relocationReserved", "relocationData", "relocationAck",
    "replyRelay", "replyRelayAck", "relocationSeal", "relocationComplete",
  ]) {
    if (!ruleCommands.has(command)) {
      fail("$.relocationStateMachine.commandRules", `missing state rule for ${command}`);
    }
  }
}

function validateServiceInvariants(schema, types, fail) {
  const fieldShape = (fields) => (fields ?? []).map((field) => {
    const shape = { name: field.name, $ref: field.$ref };
    if (hasOwn(field, "constant")) {
      shape.constant = field.constant;
    }
    return shape;
  });
  const requireFields = (fields, expected, location, message) => {
    if (JSON.stringify(fieldShape(fields)) !== JSON.stringify(expected)) {
      fail(location, message);
    }
  };
  const commands = new Map(schema.commands.map((command) => [command.name, command]));
  const obsoleteFenceNames = new Set([
    "transactionGeneration", "authorityTransactionGeneration", "membershipEpoch",
    "activationEpoch", "locationGeneration", "sourceConnectionClosed", "ownerGeneration",
  ]);
  walk(schema, "$", (value, location) => {
    if ((location.endsWith(".name") || location.endsWith(".value"))
        && obsoleteFenceNames.has(value)) {
      fail(location, `obsolete or ambiguous fence name ${value} is forbidden`);
    }
  });
  for (const name of [
    "nodeSend", "nodeRequest", "channelSend", "channelRequest", "spotSend", "spotRequest",
    "logicalMulticast", "actorSend", "actorRequest", "boundSessionSend", "instanceSpot",
  ]) {
    if (commands.get(name)?.payloadType?.$ref !== "application-payload-envelope-v1") {
      fail("$.commands", `${name} must use the typed application payload envelope`);
    }
  }
  const applicationEnvelope = types.get("application-payload-envelope-v1");
  requireFields(applicationEnvelope?.body, [
    { name: "packetName", $ref: "packet-name" },
    { name: "contentType", $ref: "content-type" },
    { name: "payload", $ref: "application-payload-bytes" },
  ], "$.types", "application payload envelope must contain only packetName, contentType and payload");
  if (applicationEnvelope?.version?.constant !== 1
      || applicationEnvelope?.length?.$ref !== "u32"
      || applicationEnvelope?.trailingBytes !== "forbidden") {
    fail("$.types", "application payload envelope must use the exact closed v1 framing");
  }
  const frameworkError = types.get("framework-error-code");
  const expectedFrameworkErrors = [
    "none",
    "actorRouteNotFound", "actorCreateFailed", "actorAlreadyExists", "actorTypeMismatch",
    "spotCreateFailed", "spotRouteNotFound", "spotTypeMismatch", "actorSessionNotBound",
    "handlerNotFound", "routeHandlerNotFound", "actorDispatchHandlerNotFound",
    "payloadDecodeFailed", "routeNotConnected", "requestTargetNotFound", "requestRejected",
    "requestProtocolError", "requestFailed", "workerQueueFull", "workerTimedOut", "workerFailed",
    "actorLocationStale", "actorCreateRejected",
  ].map((name, value) => ({ name, value }));
  expectedFrameworkErrors.push(
    { name: "spotGenerationStale", value: 33 },
    { name: "spotMoving", value: 34 },
  );
  expectedFrameworkErrors.push({ name: "relocationDataLost", value: 35 });
  if (frameworkError?.encoding !== "u32"
      || JSON.stringify(frameworkError.values) !== JSON.stringify(expectedFrameworkErrors)) {
    fail("$.types", "framework failure codes must use stable none=0 and public-kind-plus-one wire values, including RelocationDataLost=35");
  }
  for (const [name, minimum] of [["nonzero-u64", "1"], ["ordinal-or-zero", "0"]]) {
    const ordinal = types.get(name);
    if (ordinal?.encoding !== "u64" || String(ordinal.minimum) !== minimum
        || String(ordinal.maximum) !== "9223372036854775807") {
      fail("$.types", `${name} must fit the positive JVM signed-long ordinal domain`);
    }
  }
  const rejectReason = types.get("reject-reason");
  const expectedRejectReasons = [
    "protocolVersionUnsupported", "topologyMismatch", "identityMismatch",
    "securityIdentityMismatch", "channelMismatch", "lifecycleGenerationStale",
    "descriptorRevisionStale", "capabilityMismatch", "runtimeNotServing",
    "duplicateConnection", "invalidDescriptor", "resourceLimit",
  ].map((name, index) => ({ name, value: index + 1 }));
  if (rejectReason?.encoding !== "u32"
      || JSON.stringify(rejectReason.values) !== JSON.stringify(expectedRejectReasons)) {
    fail("$.types", "admission reject reason must use the stable closed v1 table");
  }
  const expectedDeliveryStates = [
    { name: "pending", value: 0 },
    { name: "terminalReceived", value: 1 },
    { name: "alreadyTerminal", value: 2 },
    { name: "sourceLeaseExpired", value: 3 },
  ];
  if (JSON.stringify(types.get("completion-delivery-state")?.values)
      !== JSON.stringify(expectedDeliveryStates)) {
    fail("$.types", "completion delivery state must exclude physical connection closure and remain monotonic");
  }
  if (JSON.stringify(types.get("reply-relay-ack-status")?.values) !== JSON.stringify([
    { name: "terminalReceived", value: 1 },
    { name: "alreadyTerminal", value: 2 },
  ])) {
    fail("$.types", "reply relay acknowledgement status must use the closed two-value terminal table");
  }
  for (const name of ["relocationData", "relocationAck", "relocationSeal", "relocationComplete"]) {
    const body = commands.get(name)?.body ?? [];
    if (body[0]?.name !== "relocation" || body[1]?.name !== "targetAttemptGeneration"
        || body[1]?.$ref !== "nonzero-u64") {
      fail("$.commands", `${name} must preserve targetAttemptGeneration immediately after stable relocation ID`);
    }
  }
  for (const name of [
    "relocationReady", "relocationData", "relocationAck", "relocationSeal", "relocationComplete",
    "relocationPrepare", "relocationReserved", "sessionRelocationSeal", "sessionRelocationSealed",
    "sessionRelocationRoute", "sessionRelocationRouted", "replyRelayAck",
  ]) {
    const coordinator = commands.get(name)?.body?.find((field) => field.name === "coordinator");
    if (coordinator?.$ref !== "relocation-coordinator-fence") {
      fail("$.commands", `${name} must carry the current coordinator and authority StoreVersion fence`);
    }
  }
  for (const name of [
    "relocationData", "relocationAck", "relocationSeal", "relocationComplete",
    "sessionRelocationSeal", "sessionRelocationRoute",
  ]) {
    const senderRole = commands.get(name)?.body?.find((field) => field.name === "senderRole");
    if (senderRole?.$ref !== "relocation-role") {
      fail("$.commands", `${name} must declare the authenticated relocation sender role`);
    }
  }
  requireFields(commands.get("relocationData")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "senderRole", $ref: "relocation-role" },
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "sequence", $ref: "nonzero-u64" },
    { name: "record", $ref: "frozen-record" },
  ], "$.commands", "relocationData must carry its sender and current coordinator authorization fence");
  requireFields(commands.get("relocationAck")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "senderRole", $ref: "relocation-role" },
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "highWater", $ref: "ordinal-or-zero" },
  ], "$.commands", "relocationAck must carry its target role and current coordinator fence");
  requireFields(commands.get("relocationSeal")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "senderRole", $ref: "relocation-role" },
    { name: "response", $ref: "bool8" },
    { name: "participants", $ref: "participant-terminal-vector" },
  ], "$.commands", "relocationSeal must carry its sender and current coordinator fence");
  requireFields(commands.get("relocationComplete")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "senderRole", $ref: "relocation-role" },
    { name: "source", $ref: "relocation-source-cleanup-fence" },
    { name: "sourceCleanupState", $ref: "source-cleanup-state" },
  ], "$.commands", "relocationComplete must carry its source or coordinator authorization");
  requireFields(commands.get("replyRelayAck")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "operation", $ref: "operation-id" },
    { name: "replyRouteId", $ref: "nonzero-u64" },
    { name: "requestSource", $ref: "request-source-fence" },
    { name: "status", $ref: "reply-relay-ack-status" },
  ], "$.commands", "replyRelayAck must echo the operation and reply route with the stable completion identity and exact request-source fence");
  if (commands.get("replyRelayAck")?.id !== 46
      || commands.get("replyRelayAck")?.domain !== "infrastructure"
      || commands.get("replyRelayAck")?.payload !== "forbidden"
      || JSON.stringify(commands.get("replyRelayAck")?.allowedFlags) !== "[]"
      || JSON.stringify(commands.get("replyRelayAck")?.requiredFlags) !== "[]") {
    fail("$.commands", "replyRelayAck must use fixed id 46 with no flags, metadata or payload");
  }
  const relocationData = commands.get("relocationData");
  if (relocationData?.payload !== "forbidden"
      || relocationData.body.find((field) => field.name === "record")?.$ref !== "frozen-record") {
    fail("$.commands", "relocationData must carry one canonical frozen record in its body");
  }
  const instanceFields = new Set((commands.get("instanceSpot")?.body ?? []).map((field) => field.name));
  for (const obsolete of ["redirected", "redirectedSpotGeneration", "relaySerial", "timeoutMs"]) {
    if (instanceFields.has(obsolete)) {
      fail("$.commands", `instanceSpot retains obsolete field ${obsolete}`);
    }
  }
  if (!instanceFields.has("replyRoute")) {
    fail("$.commands", "instanceSpot must encode its request-only reply route");
  }
  for (const commandName of ["hello", "admit", "update"]) {
    const command = commands.get(commandName);
    if (JSON.stringify(command?.allowedFlags) !== "[]"
        || JSON.stringify(command?.requiredFlags) !== "[]"
        || JSON.stringify(fieldShape(command?.body)) !== JSON.stringify([
          { name: "admission", $ref: "service-admission" },
        ])) {
      fail("$.commands", `${commandName} must carry the closed topology-specific admission union`);
    }
  }
  const serviceAdmission = types.get("service-admission");
  if (serviceAdmission?.bodyLengthType?.$ref !== "u32"
      || serviceAdmission?.maximumEncodedBytes?.$bound !== "descriptorEnvelopeBytes"
      || JSON.stringify((serviceAdmission?.cases ?? []).map((entry) => entry.when))
        !== JSON.stringify([{ topologyKind: "routeMesh" }, { topologyKind: "clientServer" }])) {
    fail("$.types", "service admission must be a bounded RouteMesh or ClientServer union");
  }
  const clientServerAdmission = types.get("client-server-admission");
  const clientAdmission = clientServerAdmission?.cases?.find((entry) => entry.when?.role === "client");
  const serverAdmission = clientServerAdmission?.cases?.find((entry) => entry.when?.role === "server");
  requireFields(clientAdmission?.fields, [
    { name: "channelName", $ref: "text8" },
    { name: "direction", $ref: "client-server-direction", constant: "clientToServer" },
    { name: "securityIdentity", $ref: "text8" },
    { name: "normalizedEffectiveMaxMessageBytes", $ref: "nonzero-u32" },
  ], "$.types", "ClientServer client admission must contain only channel, direction and security identity");
  requireFields(serverAdmission?.fields, [
    { name: "channelName", $ref: "text8" },
    { name: "direction", $ref: "client-server-direction", constant: "clientToServer" },
    { name: "serverRid", $ref: "rid" },
    { name: "lifecycleGeneration", $ref: "nonzero-u64" },
    { name: "descriptorRevision", $ref: "nonzero-u64" },
    { name: "weight", $ref: "u32" },
    { name: "runtimeState", $ref: "runtime-state" },
    { name: "securityIdentity", $ref: "text8" },
    { name: "normalizedEffectiveMaxMessageBytes", $ref: "nonzero-u32" },
    { name: "advertisedEndpoint", $ref: "endpoint" },
  ], "$.types", "ClientServer server admission must contain its exact independent descriptor");
  requireFields(types.get("route-mesh-admission")?.fields, [
    { name: "meshName", $ref: "text8" },
    { name: "securityIdentity", $ref: "text8" },
    { name: "normalizedEffectiveMaxMessageBytes", $ref: "nonzero-u32" },
    { name: "lifecycleGeneration", $ref: "nonzero-u64" },
    { name: "descriptorRevision", $ref: "nonzero-u64" },
    { name: "advertisedEndpoint", $ref: "endpoint" },
    { name: "channels", $ref: "channel-vector" },
    { name: "extension", $ref: "descriptor-extension" },
  ], "$.types", "RouteMesh admission must exchange one normalized complete-message bound");
  const payloadBytes = types.get("application-payload-bytes");
  const payloadRuntimeBound = payloadBytes?.runtimeMaximumBytes;
  const envelope = types.get("application-payload-envelope-v1");
  const envelopeRuntimeBound = envelope?.runtimeMaximumEncodedBytes;
  if (payloadBytes?.maximumBytes?.$bound !== "applicationPayloadAbsoluteBytes"
      || payloadRuntimeBound?.$negotiatedBound
        !== "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead"
      || payloadRuntimeBound?.absoluteMaximum?.$bound !== "applicationPayloadAbsoluteBytes"
      || envelope?.maximumEncodedBytes?.$bound !== "wireU32CompleteMessageBytes"
      || envelopeRuntimeBound?.$negotiatedBound !== "effectiveCompleteMessageBytes"
      || envelopeRuntimeBound?.absoluteMaximum?.$bound !== "wireU32CompleteMessageBytes") {
    fail("$.types", "application envelope must use negotiated runtime and absolute u32 bounds");
  }
  const instanceRoute = types.get("instance-route-v1");
  const readyInstanceRoute = instanceRoute?.cases?.find(
    (entry) => entry.when?.routeKind === "ready");
  const coldInstanceRoute = instanceRoute?.cases?.find(
    (entry) => entry.when?.routeKind === "coldActivation");
  requireFields(readyInstanceRoute?.fields, [
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "targetSpotId", $ref: "text8" },
    { name: "authority", $ref: "authority-generation-fence" },
  ], "$.types", "Ready Instance route must carry the exact current authority fence");
  requireFields(coldInstanceRoute?.fields, [
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "targetSpotId", $ref: "text8" },
    { name: "targetMeshName", $ref: "text8" },
    { name: "stableType", $ref: "text8" },
    { name: "targetDescriptorVersion", $ref: "text8" },
    { name: "deadlineUnixMs", $ref: "nonzero-u64" },
  ], "$.types", "Cold Instance route must carry descriptor-fenced placement intent without authority generations");
  requireFields(commands.get("instanceSpot")?.body, [
    { name: "route", $ref: "instance-route-v1" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceSpotId", $ref: "optional-text8" },
    { name: "operationKind", $ref: "instance-operation-kind" },
    { name: "operation", $ref: "operation-id" },
    { name: "replyRoute", $ref: "instance-reply-route" },
  ], "$.commands", "Instance operation must use the closed Ready-or-cold-activation route union");

  const creationIntent = types.get("object-creation-intent-v1");
  requireFields(creationIntent?.body, [
    { name: "key", $ref: "object-creation-key" },
    { name: "stableType", $ref: "text8" },
    { name: "initialMeshName", $ref: "text8" },
    { name: "requestContentReference", $ref: "creation-content-reference" },
    { name: "requestSha256", $ref: "sha256-bytes" },
    { name: "requestEncodedSize", $ref: "creation-request-size" },
  ], "$.types", "creation intent must preserve immutable type, placement, content reference and hash");
  if (creationIntent?.maximumEncodedBytes?.$bound !== "creationIntentBytes") {
    fail("$.types", "creation intent must use the 1 MiB amendment bound");
  }
  const creationContentReference = types.get("creation-content-reference");
  if (creationContentReference?.lengthType?.$ref !== "u16"
      || creationContentReference?.maximumBytes?.$bound !== "creationContentReferenceBytes") {
    fail("$.types", "creation content references must remain separate from relocation references");
  }
  requireFields(types.get("object-reservation-fence")?.fields, [
    { name: "reservationId", $ref: "text8" },
    { name: "expectedStoreVersion", $ref: "authority-store-version" },
    { name: "objectGeneration", $ref: "nonzero-u64" },
    { name: "authorityOwnerGeneration", $ref: "nonzero-u64" },
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "targetOwnerId", $ref: "text8" },
    { name: "targetOwnerLeaseGeneration", $ref: "nonzero-u64" },
    { name: "pendingCapacityDelta", $ref: "nonzero-u32" },
  ], "$.types", "generic reservation must carry exact object, target owner and capacity fences");
  requireFields(commands.get("userSpotCreate")?.body, [
    { name: "correlation", $ref: "nonzero-u64" },
    { name: "operation", $ref: "operation-id" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "spotId", $ref: "text8" },
    { name: "stableType", $ref: "text8" },
    { name: "reservation", $ref: "object-reservation-fence" },
    { name: "deadlineUnixMs", $ref: "nonzero-u64" },
  ], "$.commands", "User Spot create must carry exact operation, source lifecycle, authority key, type, reservation and deadline");
  requireFields(types.get("user-spot-close-fence-v1")?.body, [
    { name: "spot", $ref: "spot-ref" },
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "expectedAuthorityOwnerGeneration", $ref: "nonzero-u64" },
    { name: "expectedStoreVersion", $ref: "authority-store-version" },
  ], "$.types", "User Spot close target must carry exact ref, target lifecycle, authority generation and StoreVersion");
  requireFields(commands.get("userSpotClose")?.body, [
    { name: "correlation", $ref: "nonzero-u64" },
    { name: "operation", $ref: "operation-id" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "target", $ref: "user-spot-close-fence-v1" },
    { name: "deadlineUnixMs", $ref: "nonzero-u64" },
  ], "$.commands", "User Spot close must carry exact operation, source lifecycle, target authority fence and deadline");
  requireFields(commands.get("actorCreate")?.body, [
    { name: "correlation", $ref: "nonzero-u64" },
    { name: "operation", $ref: "operation-id" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "actorId", $ref: "text8" },
    { name: "stableType", $ref: "text8" },
    { name: "reservation", $ref: "object-reservation-fence" },
    { name: "deadlineUnixMs", $ref: "nonzero-u64" },
  ], "$.commands", "Actor create must carry exact operation, source lifecycle, authority key, type, reservation and deadline");
  for (const [name, id] of [["userSpotCreate", 47], ["userSpotClose", 48], ["actorCreate", 49]]) {
    const command = commands.get(name);
    if (command?.id !== id
        || command?.domain !== "infrastructure"
        || command?.payload !== "forbidden"
        || JSON.stringify(command?.allowedFlags) !== "[]"
        || JSON.stringify(command?.requiredFlags) !== "[]") {
      fail("$.commands", `${name} must use its fixed infrastructure command ID without flags, metadata or payload`);
    }
  }
  const operationKinds = types.get("mesh-operation-kind");
  const expectedOperationKinds = [
    "none", "nodeRequest", "channelRequest", "spotRequest", "actorRequest", "actorLookup",
    "actorDestroy", "actorJoin", "actorLeave", "streamBind", "streamUnbind", "streamClose",
    "instanceSpotRequest", "userSpotCreate", "userSpotClose", "actorCreate",
  ].map((name, value) => ({ name, value }));
  if (operationKinds?.encoding !== "u32"
      || JSON.stringify(operationKinds?.values) !== JSON.stringify(expectedOperationKinds)) {
    fail("$.types", "operation discriminator must keep stable values and add Actor create=15 after User Spot create=13 and close=14");
  }
  const requestTail = types.get("request-specific-tail");
  const expectedRequestTailCases = new Map([
    ["actorLookup:ok", [
      { name: "actor", $ref: "actor-ref" },
      { name: "spotId", $ref: "text8" },
      { name: "spotGeneration", $ref: "nonzero-u64" },
      { name: "authorityOwnerGeneration", $ref: "nonzero-u64" },
    ]],
    ["actorJoin:ok", [{ name: "join", $ref: "actor-join-reply-tail" }]],
    ["streamBind:ok", [
      { name: "bindingGeneration", $ref: "nonzero-u64" },
      { name: "authorityOwnerGeneration", $ref: "nonzero-u64" },
    ]],
    ["userSpotCreate:ok", [
      { name: "createResult", $ref: "user-spot-create-result" },
      { name: "spot", $ref: "spot-ref" },
    ]],
    ["userSpotClose:ok", [{ name: "closed", $ref: "bool8" }]],
    ["actorCreate:ok", [{ name: "creation", $ref: "actor-create-terminal" }]],
  ]);
  const requestTailCases = requestTail?.cases ?? [];
  if (requestTailCases.length !== expectedRequestTailCases.size
      || requestTail?.otherwise?.fields?.length !== 0) {
    fail("$.types", "reply tail must remain closed with exactly six successful operation-specific cases");
  } else {
    const seenReplyCases = new Set();
    for (const replyCase of requestTailCases) {
      const key = `${replyCase.when?.originalOperationKind}:${replyCase.when?.terminalResult}`;
      if (seenReplyCases.has(key)
          || !expectedRequestTailCases.has(key)
          || JSON.stringify(fieldShape(replyCase.fields))
            !== JSON.stringify(expectedRequestTailCases.get(key))) {
        fail("$.types", `reply tail case ${key} violates the exact operation discriminator or tail cardinality`);
      }
      seenReplyCases.add(key);
    }
  }
  const createResults = types.get("user-spot-create-result");
  if (createResults?.encoding !== "u8"
      || JSON.stringify(createResults?.values) !== JSON.stringify([
        { name: "existing", value: 1 },
        { name: "created", value: 2 },
        { name: "rejected", value: 3 },
      ])) {
    fail("$.types", "User Spot create result must be the closed Existing, Created or Rejected discriminator");
  }
  const actorCreateResults = types.get("actor-create-result");
  if (actorCreateResults?.encoding !== "u8"
      || JSON.stringify(actorCreateResults?.values) !== JSON.stringify([
        { name: "existing", value: 1 },
        { name: "created", value: 2 },
        { name: "rejected", value: 3 },
      ])) {
    fail("$.types", "Actor create result must be the closed Existing, Created or Rejected discriminator");
  }
  const actorCreateTerminal = types.get("actor-create-terminal");
  if (actorCreateTerminal?.kind !== "conditional-union"
      || actorCreateTerminal?.bodyLengthType?.$ref !== "u16"
      || actorCreateTerminal?.bodyLengthCovers !== "selected-case"
      || actorCreateTerminal?.otherwise !== "protocol-error"
      || actorCreateTerminal?.trailingBytes !== "forbidden"
      || JSON.stringify(actorCreateTerminal?.discriminators) !== JSON.stringify([{
        name: "createResult", source: "wire", $ref: "actor-create-result",
      }])) {
    fail("$.types", "Actor create terminal must be a closed length-delimited union discriminated by createResult");
  } else {
    const expectedActorCreateTerminalCases = new Map([
      ["existing", [{ name: "actor", $ref: "actor-ref" }]],
      ["created", [{ name: "actor", $ref: "actor-ref" }]],
      ["rejected", []],
    ]);
    const cases = actorCreateTerminal.cases ?? [];
    if (cases.length !== expectedActorCreateTerminalCases.size) {
      fail("$.types", "Actor create terminal must declare exactly Existing, Created and Rejected cases");
    } else {
      const seen = new Set();
      for (const entry of cases) {
        const result = entry.when?.createResult;
        if (seen.has(result)
            || !expectedActorCreateTerminalCases.has(result)
            || JSON.stringify(fieldShape(entry.fields))
              !== JSON.stringify(expectedActorCreateTerminalCases.get(result))) {
          fail("$.types", `Actor create terminal case ${result} violates the exact result payload contract`);
        }
        seen.add(result);
      }
    }
  }
  const creationOperationTerminal = types.get("creation-operation-terminal-v1");
  if (creationOperationTerminal?.kind !== "versioned-length-delimited"
      || creationOperationTerminal?.version?.$ref !== "u8"
      || creationOperationTerminal?.version?.constant !== 1
      || creationOperationTerminal?.length?.$ref !== "u32"
      || creationOperationTerminal?.length?.covers !== "body"
      || creationOperationTerminal?.maximumEncodedBytes?.$bound
        !== "creationTerminalEnvelopeBytes"
      || creationOperationTerminal?.correlationFields !== "forbidden"
      || creationOperationTerminal?.trailingBytes !== "forbidden") {
    fail("$.types", "creation operation terminal must be a bounded correlation-free v1 semantic envelope");
  }
  requireFields(creationOperationTerminal?.body, [
    { name: "terminalResult", $ref: "request-terminal-result" },
    { name: "failureCode", $ref: "framework-error-code" },
    { name: "hasCreation", $ref: "bool8" },
    { name: "creation", $ref: "actor-create-terminal" },
    { name: "hasApplicationPayload", $ref: "bool8" },
    { name: "applicationPayload", $ref: "application-payload-envelope-v1" },
  ], "$.types", "creation operation terminal must preserve semantic result without request correlation");
  const creationTerminalCreation = creationOperationTerminal?.body
    ?.find((field) => field.name === "creation");
  const creationTerminalPayload = creationOperationTerminal?.body
    ?.find((field) => field.name === "applicationPayload");
  if (creationTerminalCreation?.when?.fieldEquals?.name !== "hasCreation"
      || creationTerminalCreation?.when?.fieldEquals?.value !== "true"
      || creationTerminalCreation?.otherwise !== "forbidden"
      || creationTerminalPayload?.when?.fieldEquals?.name !== "hasApplicationPayload"
      || creationTerminalPayload?.when?.fieldEquals?.value !== "true"
      || creationTerminalPayload?.otherwise !== "forbidden") {
    fail("$.types", "creation operation terminal optional fields must be controlled by their exact presence flags");
  }
  const expectedCreationTerminalConstraints = [
    {
      kind: "terminal-success-shape",
      when: { terminalResult: "ok" },
      requires: { failureCode: "none", hasCreation: "true" },
    },
    {
      kind: "terminal-failure-shape",
      when: { terminalResultNot: "ok" },
      requires: { hasCreation: "false", hasApplicationPayload: "false" },
    },
    {
      kind: "existing-has-no-application-payload",
      when: { "creation.createResult": "existing" },
      requires: { hasApplicationPayload: "false" },
    },
  ];
  if (JSON.stringify(creationOperationTerminal?.constraints)
      !== JSON.stringify(expectedCreationTerminalConstraints)) {
    fail("$.types", "creation operation terminal success, failure and Existing payload shapes are closed");
  }
  const reservation = types.get("generic-object-reservation-v1");
  if (JSON.stringify((reservation?.cases ?? []).map((entry) => entry.when))
      !== JSON.stringify([
        { operationKind: "reserve" },
        { operationKind: "commit" },
        { operationKind: "abort" },
      ])) {
    fail("$.types", "generic reservation must be the closed Reserve, Commit or Abort operation");
  }
  const reserveCase = reservation?.cases?.find((entry) => entry.when?.operationKind === "reserve");
  requireFields(reserveCase?.fields, [
    { name: "intent", $ref: "object-creation-intent-v1" },
    { name: "target", $ref: "object-creation-target-v1" },
    { name: "creatingPayload", $ref: "durable-blob" },
    { name: "pendingCapacityDelta", $ref: "nonzero-u32" },
  ], "$.types", "Reserve input cannot require the provider-issued reservation fence");
  requireFields(types.get("pending-object-creation-v1")?.fields, [
    { name: "reservationId", $ref: "text8" },
    { name: "requestContentReference", $ref: "creation-content-reference" },
    { name: "requestSha256", $ref: "sha256-bytes" },
    { name: "requestEncodedSize", $ref: "creation-request-size" },
  ], "$.types", "Pending authority must expose exact creation recovery projection");
  requireFields(types.get("instance-activation-recovery-v1")?.fields, [
    { name: "targetSpotId", $ref: "text8" },
    { name: "stableType", $ref: "text8" },
    { name: "targetMeshName", $ref: "text8" },
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "targetDescriptorVersion", $ref: "text8" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "sourceSpotId", $ref: "optional-text8" },
    { name: "operationKind", $ref: "instance-operation-kind" },
    { name: "operation", $ref: "operation-id" },
    { name: "replyRoute", $ref: "instance-reply-route" },
    { name: "deadlineUnixMs", $ref: "nonzero-u64" },
    { name: "hasMetadata", $ref: "bool8" },
    { name: "metadata", $ref: "metadata-frame" },
    { name: "applicationPayload", $ref: "application-payload-envelope-v1" },
  ], "$.types", "Instance activation recovery must preserve the complete first-message envelope");
  const activationRecoveryEnvelope = types.get("instance-activation-recovery-v1");
  if (activationRecoveryEnvelope?.scope !== "target-owned-instance-spot-cold-activation-only"
      || activationRecoveryEnvelope?.metadataMeaning
        !== "exact-command-39-metadata-flag-presence-and-immutable-frame-bytes"
      || activationRecoveryEnvelope?.fields?.find((field) => field.name === "metadata")?.when
        ?.fieldEquals?.name !== "hasMetadata"
      || activationRecoveryEnvelope?.fields?.find((field) => field.name === "metadata")
        ?.otherwise !== "forbidden") {
    fail("$.types", "ZLIA must preserve command 39 metadata presence and frame only for target-owned Instance cold activation");
  }
  const aggregate = types.get("maintenance-aggregate-v1");
  if (aggregate?.maximumEncodedBytes?.$bound !== "maintenanceAggregateBytes"
      || types.get("aggregate-participant-vector")?.maximumItems?.$bound
        !== "maintenanceAggregateParticipants") {
    fail("$.types", "maintenance aggregate must use the 1 MiB and 1024 participant bounds");
  }
  requireFields(types.get("maintenance-aggregate-participant-v1")?.fields, [
    { name: "object", $ref: "relocation-object-identity" },
    { name: "expectedStoreVersion", $ref: "authority-store-version" },
    { name: "mutation", $ref: "aggregate-participant-mutation-bytes" },
  ], "$.types", "Location aggregate participants must bind canonical object, version and mutation");
  requireFields(aggregate?.body, [
    { name: "aggregateId", $ref: "aggregate-id" },
    { name: "aggregateGeneration", $ref: "nonzero-u64" },
    { name: "ownerSpot", $ref: "spot-ref" },
    { name: "participants", $ref: "aggregate-participant-vector" },
    { name: "inventoryDigestSha256", $ref: "sha256-bytes" },
    { name: "relocationRoot", $ref: "relocation-root-pointer" },
  ], "$.types", "Location aggregate must own canonical participants, generation and inventory digest");
  const aggregateVector = types.get("aggregate-participant-vector");
  if (aggregateVector?.item?.$ref !== "maintenance-aggregate-participant-v1"
      || JSON.stringify(aggregateVector?.constraints) !== JSON.stringify([
        { kind: "sorted", field: "object", comparison: "canonical-authority-key-bytes" },
        { kind: "unique", field: "object" },
      ])) {
    fail("$.types", "Location aggregate participants must be canonical, bounded and unique");
  }
  const relocationObjectKinds = (types.get("relocation-object-identity")?.cases ?? [])
    .map((entry) => entry.when?.objectKind);
  if (JSON.stringify(relocationObjectKinds)
      !== JSON.stringify(["actor", "userSpot", "instanceSpot"])) {
    fail("$.types", "relocation inventory must include Actor, User Spot and Instance Spot");
  }
  requireFields(types.get("spot-route-fence")?.fields, [
    { name: "spot", $ref: "spot-ref" },
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "expectedAuthorityOwnerGeneration", $ref: "nonzero-u64" },
    { name: "expectedOwnerLeaseGeneration", $ref: "nonzero-u64" },
  ], "$.types", "Spot route fence must carry exact ref, node and authority generations");
  const messageFollowRoute = types.get("message-follow-route-v1");
  requireFields(messageFollowRoute?.body, [
    { name: "source", $ref: "message-follow-route" },
    { name: "target", $ref: "message-follow-route" },
    { name: "hopCount", $ref: "u8" },
    { name: "queuedMessages", $ref: "u32" },
    { name: "queuedBytes", $ref: "u32" },
    { name: "originalOperation", $ref: "operation-id" },
    { name: "originalReplyRouteId", $ref: "u64" },
  ], "$.types", "Message Follow route must preserve exact route, queue accounting and reply identity");
  if (messageFollowRoute?.maximumEncodedBytes?.$bound !== "messageFollowBytes") {
    fail("$.types", "Message Follow route must use the bounded queue byte limit");
  }

  requireFields(types.get("actor-route-fence")?.fields, [
    { name: "actor", $ref: "actor-ref" },
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "expectedAuthorityOwnerGeneration", $ref: "nonzero-u64" },
    { name: "expectedOwnerLeaseGeneration", $ref: "nonzero-u64" },
  ], "$.types", "Actor route fence must pair ActorRef with exact node and authority generations");
  requireFields(types.get("spot-ref")?.fields, [
    { name: "spotId", $ref: "text8" },
    { name: "objectGeneration", $ref: "nonzero-u64" },
  ], "$.types", "Spot reference must pair a non-empty Spot ID with a non-zero generation");
  requireFields(types.get("spot-membership")?.fields, [
    { name: "spot", $ref: "spot-ref" },
  ], "$.types", "Spot membership must carry exactly one shared Spot generation fence");
  for (const [typeName, discriminator, presentField, presentType] of [
    ["optional-spot-ref", "hasSpot", "spot", "spot-ref"],
    ["optional-spot-membership", "hasMembership", "membership", "spot-membership"],
  ]) {
    const optional = types.get(typeName);
    const absent = optional?.cases?.find((entry) => entry.when?.[discriminator] === "false");
    const present = optional?.cases?.find((entry) => entry.when?.[discriminator] === "true");
    if (optional?.bodyLengthType?.$ref !== "u16"
        || JSON.stringify(absent?.fields) !== "[]"
        || JSON.stringify(fieldShape(present?.fields)) !== JSON.stringify([
          { name: presentField, $ref: presentType },
        ])) {
      fail("$.types", `${typeName} must have one canonical absent or complete present representation`);
    }
  }
  const bindingTransition = types.get("bound-session-binding-transition");
  const activeBinding = bindingTransition?.cases?.find(
    (entry) => entry.when?.bindingState === "active",
  );
  const retiredBinding = bindingTransition?.cases?.find(
    (entry) => entry.when?.bindingState === "tombstone",
  );
  if (bindingTransition?.bodyLengthType?.$ref !== "u16"
      || JSON.stringify(fieldShape(activeBinding?.fields)) !== JSON.stringify([
        { name: "bindingGeneration", $ref: "nonzero-u64" },
      ])
      || JSON.stringify(fieldShape(retiredBinding?.fields)) !== JSON.stringify([
        { name: "retiredBindingGeneration", $ref: "nonzero-u64" },
      ])) {
    fail("$.types", "bound session binding must be a closed active or tombstone transition");
  }
  requireFields(types.get("actor-membership-snapshot")?.fields, [
    { name: "actor", $ref: "actor-ref" },
    { name: "membership", $ref: "spot-membership" },
  ], "$.types", "Actor lifecycle snapshot must contain one complete Actor and Spot membership");
  const optionalActorSnapshot = types.get("optional-actor-membership-snapshot");
  const absentActorSnapshot = optionalActorSnapshot?.cases?.find(
    (entry) => entry.when?.hasSnapshot === "false",
  );
  const presentActorSnapshot = optionalActorSnapshot?.cases?.find(
    (entry) => entry.when?.hasSnapshot === "true",
  );
  if (optionalActorSnapshot?.bodyLengthType?.$ref !== "u16"
      || JSON.stringify(absentActorSnapshot?.fields) !== "[]"
      || JSON.stringify(fieldShape(presentActorSnapshot?.fields)) !== JSON.stringify([
        { name: "snapshot", $ref: "actor-membership-snapshot" },
      ])) {
    fail("$.types", "optional Actor membership snapshot must have one canonical absent or complete representation");
  }
  const actorControl = types.get("actor-control-data");
  const expectedActorControlCases = new Map([
    ["created", [{ name: "current", $ref: "actor-membership-snapshot" }]],
    ["joined", [
      { name: "previous", $ref: "optional-actor-membership-snapshot" },
      { name: "current", $ref: "actor-membership-snapshot" },
    ]],
    ["left", [
      { name: "previous", $ref: "actor-membership-snapshot" },
      { name: "current", $ref: "actor-membership-snapshot" },
    ]],
    ["disconnected", [{ name: "current", $ref: "actor-membership-snapshot" }]],
    ["destroyed", [{ name: "previous", $ref: "actor-membership-snapshot" }]],
  ]);
  if (actorControl?.bodyLengthType?.$ref !== "u16"
      || (actorControl?.cases ?? []).length !== expectedActorControlCases.size) {
    fail("$.types", "frozen Actor control must be a closed lifecycle union");
  } else {
    for (const lifecycleCase of actorControl.cases) {
      const kind = lifecycleCase.when?.lifecycleKind;
      if (!expectedActorControlCases.has(kind)
          || JSON.stringify(fieldShape(lifecycleCase.fields))
            !== JSON.stringify(expectedActorControlCases.get(kind))) {
        fail("$.types", `frozen Actor control ${kind} does not match its closed lifecycle shape`);
      }
    }
  }
  for (const [commandName, fieldName] of [
    ["actorSend", "targetActor"],
    ["actorRequest", "targetActor"],
    ["actorDestroy", "actor"],
    ["actorJoin", "actor"],
    ["boundSessionSend", "actor"],
    ["boundSessionBind", "actor"],
  ]) {
    const field = commands.get(commandName)?.body?.find((entry) => entry.name === fieldName);
    if (field?.$ref !== "actor-route-fence") {
      fail("$.commands", `${commandName}.${fieldName} must carry the internal Actor membership fence`);
    }
  }
  const sendReady = types.get("send-ready-destination");
  for (const kind of ["actor", "boundSession"]) {
    const routeCase = sendReady?.cases?.find((entry) => entry.when?.destinationKind === kind);
    if (routeCase?.fields?.find((field) => field.name === "targetActor")?.$ref !== "actor-route-fence") {
      fail("$.types", `send-ready ${kind} destination must retain the Actor membership fence`);
    }
  }
  requireFields(commands.get("boundSessionBind")?.body, [
    { name: "correlation", $ref: "nonzero-u64" },
    { name: "actor", $ref: "actor-route-fence" },
    { name: "sessionRid", $ref: "rid" },
    { name: "binding", $ref: "bound-session-binding-transition" },
  ], "$.commands", "boundSessionBind must carry exactly one active generation or tombstone fence");
  requireFields(commands.get("actorLeft")?.body, [
    { name: "actor", $ref: "actor-ref" },
    { name: "previousMembership", $ref: "spot-membership" },
    { name: "currentAuthorityOwnerGeneration", $ref: "nonzero-u64" },
  ], "$.commands", "actorLeft must carry one complete previous membership and current authority owner generation");
  requireFields(commands.get("actorJoined")?.body, [
    { name: "actor", $ref: "actor-ref" },
    { name: "previousMembership", $ref: "optional-spot-membership" },
    { name: "currentMembership", $ref: "spot-membership" },
    { name: "currentAuthorityOwnerGeneration", $ref: "nonzero-u64" },
  ], "$.commands", "actorJoined must carry canonical memberships and the current authority owner generation");
  const participant = types.get("participant-entry");
  requireFields(participant?.fields, [
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "identity", $ref: "relocation-participant-identity" },
    { name: "allowanceMessages", $ref: "ordinal-or-zero" },
    { name: "allowanceBytes", $ref: "ordinal-or-zero" },
  ], "$.types", "negotiated participant must use its closed mailbox or bound-session identity");
  requireFields(types.get("optional-bound-session-tail")?.fields, [
    { name: "sourceSessionRid", $ref: "rid" },
    { name: "sourceBindingGeneration", $ref: "nonzero-u64" },
    { name: "sourceSessionSequence", $ref: "nonzero-u64" },
  ], "$.types", "bound-session Actor records must carry the session sequence assigned at ingress admission");
  requireFields(commands.get("relocationPrepare")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "roundKind", $ref: "relocation-reservation-round-kind" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "candidate", $ref: "relocation-reservation-candidate" },
    { name: "initiatorRole", $ref: "relocation-role" },
    { name: "object", $ref: "relocation-object-identity" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "requiredMessages", $ref: "ordinal-or-zero" },
    { name: "requiredBytes", $ref: "ordinal-or-zero" },
    { name: "requirements", $ref: "participant-vector" },
    { name: "relocationRoot", $ref: "relocation-root-pointer" },
    { name: "applicationVersion", $ref: "application-version" },
  ], "$.commands", "relocationPrepare must carry the exact sealed inventory and durable relocation pointer");
  requireFields(commands.get("relocationReady")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "roundKind", $ref: "relocation-reservation-round-kind" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "candidate", $ref: "relocation-reservation-candidate" },
    { name: "object", $ref: "relocation-object-identity" },
    { name: "role", $ref: "relocation-role" },
    { name: "offeredMessages", $ref: "ordinal-or-zero" },
    { name: "offeredBytes", $ref: "ordinal-or-zero" },
    { name: "participants", $ref: "participant-vector" },
    { name: "extension", $ref: "relocation-extension" },
  ], "$.commands", "relocationReady must bind offer and acceptance to one target attempt");
  requireFields(commands.get("relocationReserved")?.body, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "roundKind", $ref: "relocation-reservation-round-kind" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "candidate", $ref: "relocation-reservation-candidate" },
    { name: "reservationGeneration", $ref: "nonzero-u64" },
    { name: "participants", $ref: "participant-vector" },
  ], "$.commands", "relocationReserved must acknowledge the exact accepted participant reservation");
  const sessionCommandFields = {
    sessionRelocationSeal: [
      { name: "relocation", $ref: "relocation-id" },
      { name: "coordinator", $ref: "relocation-coordinator-fence" },
      { name: "senderRole", $ref: "relocation-role" },
      { name: "actor", $ref: "actor-route-fence" },
      { name: "sessionOwnerNodeRid", $ref: "rid" },
      { name: "sessionOwnerNodeGeneration", $ref: "nonzero-u64" },
      { name: "sessionOwnerId", $ref: "text8" },
      { name: "sessionOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sessionRid", $ref: "rid" },
      { name: "bindingGeneration", $ref: "nonzero-u64" },
    ],
    sessionRelocationSealed: [
      { name: "relocation", $ref: "relocation-id" },
      { name: "coordinator", $ref: "relocation-coordinator-fence" },
      { name: "actor", $ref: "actor-route-fence" },
      { name: "sessionOwnerNodeRid", $ref: "rid" },
      { name: "sessionOwnerNodeGeneration", $ref: "nonzero-u64" },
      { name: "sessionOwnerId", $ref: "text8" },
      { name: "sessionOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sessionRid", $ref: "rid" },
      { name: "bindingGeneration", $ref: "nonzero-u64" },
      { name: "lastAcceptedSessionSequence", $ref: "ordinal-or-zero" },
    ],
    sessionRelocationRoute: [
      { name: "relocation", $ref: "relocation-id" },
      { name: "coordinator", $ref: "relocation-coordinator-fence" },
      { name: "senderRole", $ref: "relocation-role" },
      { name: "actor", $ref: "actor-ref" },
      { name: "sessionOwnerNodeRid", $ref: "rid" },
      { name: "sessionOwnerNodeGeneration", $ref: "nonzero-u64" },
      { name: "sessionOwnerId", $ref: "text8" },
      { name: "sessionOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sessionRid", $ref: "rid" },
      { name: "bindingGeneration", $ref: "nonzero-u64" },
      { name: "route", $ref: "session-relocation-route-update" },
    ],
    sessionRelocationRouted: [
      { name: "relocation", $ref: "relocation-id" },
      { name: "coordinator", $ref: "relocation-coordinator-fence" },
      { name: "actor", $ref: "actor-ref" },
      { name: "sessionOwnerNodeRid", $ref: "rid" },
      { name: "sessionOwnerNodeGeneration", $ref: "nonzero-u64" },
      { name: "sessionOwnerId", $ref: "text8" },
      { name: "sessionOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sessionRid", $ref: "rid" },
      { name: "bindingGeneration", $ref: "nonzero-u64" },
      { name: "action", $ref: "session-relocation-route-action" },
      { name: "currentAuthorityOwnerGeneration", $ref: "nonzero-u64" },
      { name: "lastAcceptedSessionSequence", $ref: "ordinal-or-zero" },
    ],
  };
  for (const [commandName, fields] of Object.entries(sessionCommandFields)) {
    requireFields(commands.get(commandName)?.body, fields, "$.commands",
      `${commandName} does not match the bound-session relocation barrier contract`);
  }
  const sessionRoute = types.get("session-relocation-route-update");
  const sessionCommit = sessionRoute?.cases?.find((entry) => entry.when?.action === "commit");
  const sessionAbort = sessionRoute?.cases?.find((entry) => entry.when?.action === "abort");
  if (sessionRoute?.bodyLengthType?.$ref !== "u16"
      || JSON.stringify(fieldShape(sessionCommit?.fields)) !== JSON.stringify([
        { name: "previousAuthorityOwnerGeneration", $ref: "nonzero-u64" },
        { name: "targetAuthorityOwnerGeneration", $ref: "nonzero-u64" },
        { name: "targetNodeRid", $ref: "rid" },
        { name: "targetNodeGeneration", $ref: "nonzero-u64" },
        { name: "replayedHighWater", $ref: "ordinal-or-zero" },
      ])
      || JSON.stringify(fieldShape(sessionAbort?.fields)) !== JSON.stringify([
        { name: "currentAuthorityOwnerGeneration", $ref: "nonzero-u64" },
      ])) {
    fail("$.types", "session relocation route must be a closed commit or abort update");
  }
  const participantIdentity = types.get("relocation-participant-identity");
  const mailboxIdentity = participantIdentity?.cases?.find(
    (entry) => entry.when?.participantKind === "objectMailbox",
  );
  const sessionIdentity = participantIdentity?.cases?.find(
    (entry) => entry.when?.participantKind === "boundSession",
  );
  if (participantIdentity?.bodyLengthType?.$ref !== "u16"
      || JSON.stringify(mailboxIdentity?.fields) !== "[]"
      || JSON.stringify(fieldShape(sessionIdentity?.fields)) !== JSON.stringify([
        { name: "sessionOwnerNodeRid", $ref: "rid" },
        { name: "sessionOwnerNodeGeneration", $ref: "nonzero-u64" },
        { name: "sessionOwnerId", $ref: "text8" },
        { name: "sessionOwnerLeaseGeneration", $ref: "nonzero-u64" },
        { name: "sessionRid", $ref: "rid" },
        { name: "bindingGeneration", $ref: "nonzero-u64" },
      ])) {
    fail("$.types", "relocation participant identity must distinguish object mailbox and bound session");
  }

  const expectedParticipantConstraints = [
    { kind: "sorted", field: "participantId", comparison: "unsigned-wire-value" },
    { kind: "unique", field: "participantId" },
  ];
  for (const name of [
    "participant-vector", "participant-terminal-vector", "participant-progress-vector",
  ]) {
    const vector = types.get(name);
    if (vector?.maximumItems?.$bound !== "relocationResourceParticipants"
        || JSON.stringify(vector.constraints) !== JSON.stringify(expectedParticipantConstraints)) {
      fail("$.types", `${name} must use the common bounded, sorted and unique participant contract`);
    }
  }
  const completions = types.get("request-completion-vector");
  const expectedCompletionConstraints = [
    {
      kind: "sorted",
      fields: ["participantId", "sequence"],
      comparison: "unsigned-wire-value",
    },
    { kind: "unique", fields: ["participantId", "sequence"] },
    {
      kind: "unique",
      fields: [
        "requestSource.sourceOwnerId",
        "requestSource.sourceOwnerLeaseGeneration",
        "requestSource.sourceNodeRid",
        "requestSource.sourceNodeGeneration",
        "operationId.high",
        "operationId.low",
      ],
    },
  ];
  if (completions?.maximumItems?.$bound !== "journalRecordCount"
      || JSON.stringify(completions.constraints) !== JSON.stringify(expectedCompletionConstraints)) {
    fail("$.types", "terminal completion vector must use the common bound and canonical operation order");
  }

  const extensionFields = new Set((types.get("relocation-extension")?.fields ?? []).map((field) => field.name));
  if (!extensionFields.has("participantProgress")
      || extensionFields.has("acceptedBoundary") || extensionFields.has("replayCursor")) {
    fail("$.types", "relocation extension must use participant progress vector boundaries");
  }
  for (const name of ["descriptor-extension", "relocation-extension"]) {
    const extension = types.get(name);
    if (extension?.kind !== "tlv32" || extension?.totalLengthType?.$ref !== "u32"
        || extension?.fieldLengthType?.$ref !== "u32") {
      fail("$.types", `${name} must use u32-delimited TLV framing`);
    }
  }
  const capabilityField = types.get("descriptor-extension")?.fields?.find(
    (field) => field.name === "protocolCapabilities",
  );
  if (capabilityField?.required !== true
      || JSON.stringify(capabilityField.constraints)
        !== JSON.stringify([{ kind: "contains-protocol-required-capability" }])) {
    fail("$.types", "descriptor capabilities must contain the protocol-required capability");
  }
  const descriptor = types.get("descriptor-extension");
  if (JSON.stringify(types.get("object-role")?.values) !== JSON.stringify([
    { name: "none", value: 0 },
    { name: "client", value: 1 },
    { name: "server", value: 2 },
  ])) {
    fail("$.types", "object role must be the closed None, Client or Server enum");
  }
  if (descriptor?.maximumEncodedBytes?.$bound !== "descriptorEnvelopeBytes"
      || JSON.stringify((descriptor?.fields ?? []).map(({ id, name, $ref, required }) => ({
        id, name, $ref, required,
      }))) !== JSON.stringify([
        { id: 1, name: "runtimeState", $ref: "runtime-state", required: true },
        { id: 2, name: "applicationVersion", $ref: "application-version", required: true },
        { id: 3, name: "spotTypes", $ref: "sorted-text8-vector", required: false },
        { id: 4, name: "statefulCapabilities", $ref: "stateful-capability-vector", required: false },
        { id: 5, name: "maintenanceWave", $ref: "optional-text8", required: false },
        { id: 6, name: "protocolCapabilities", $ref: "sorted-text8-vector", required: true },
        { id: 7, name: "objectRole", $ref: "object-role", required: true },
        { id: 8, name: "placementWeight", $ref: "u32", required: true },
        { id: 9, name: "activeCapacityLimit", $ref: "object-capacity-limit", required: true },
        { id: 10, name: "pendingCapacityLimit", $ref: "object-pending-capacity-limit", required: true },
        { id: 11, name: "activeCapacityUsed", $ref: "u32", required: true },
        { id: 12, name: "pendingCapacityUsed", $ref: "u32", required: true },
      ])) {
    fail("$.types", "descriptor extension must use the bounded per-object capability model");
  }
  requireFields(types.get("stateful-capability-entry")?.fields, [
    { name: "objectKind", $ref: "stateful-object-kind" },
    { name: "type", $ref: "text8" },
    { name: "relocationPolicy", $ref: "relocation-policy-kind" },
    { name: "activeCapacityLimit", $ref: "object-capacity-limit" },
    { name: "pendingCapacityLimit", $ref: "object-pending-capacity-limit" },
    { name: "hasSnapshotAdapter", $ref: "bool8" },
    { name: "available", $ref: "ordinal-or-zero" },
  ], "$.types", "stateful capability must keep kind, type, policy, Snapshot adapter presence and capacity in one record");
  if ((types.get("stateful-capability-entry")?.fields ?? []).some(
    (field) => field.name === "readableStateContractIds",
  )) {
    fail("$.types", "stateful capability cannot publish application state contract IDs");
  }
  const relocationReference = types.get("relocation-reference");
  if (relocationReference?.lengthType?.$ref !== "u16"
      || relocationReference?.maximumBytes?.$bound !== "relocationReferenceBytes") {
    fail("$.types", "relocation references must use their separate bounded text type");
  }
  requireFields(types.get("authority-generation-fence")?.fields, [
    { name: "objectGeneration", $ref: "nonzero-u64" },
    { name: "ownerId", $ref: "text8" },
    { name: "authorityOwnerGeneration", $ref: "nonzero-u64" },
    { name: "leaseGeneration", $ref: "nonzero-u64" },
    { name: "storeVersion", $ref: "authority-store-version" },
  ], "$.types", "wire authority fence must carry provider metadata and opaque StoreVersion exactly once");
  requireFields(types.get("relocation-coordinator-fence")?.fields, [
    { name: "coordinatorOwnerId", $ref: "text8" },
    { name: "coordinatorLeaseGeneration", $ref: "nonzero-u64" },
    { name: "coordinatorNodeRid", $ref: "rid" },
    { name: "coordinatorNodeGeneration", $ref: "nonzero-u64" },
    { name: "expectedAuthorityStoreVersion", $ref: "authority-store-version" },
  ], "$.types", "relocation coordinator fence must bind lease, admitted node and authority version");
  requireFields(types.get("relocation-reservation-candidate")?.fields, [
    { name: "targetNodeRid", $ref: "rid" },
    { name: "targetNodeGeneration", $ref: "nonzero-u64" },
    { name: "targetOwnerId", $ref: "text8" },
    { name: "targetOwnerLeaseGeneration", $ref: "nonzero-u64" },
  ], "$.types", "relocation reservation candidate must carry the admitted target lifecycle identity");

  requireFields(types.get("authority-payload-v1")?.fields, [
    { name: "operationKind", $ref: "authority-operation-kind" },
    { name: "object", $ref: "authority-object-identity" },
    { name: "ownerId", $ref: "text8" },
    { name: "ownerLeaseGeneration", $ref: "nonzero-u64" },
    { name: "ownerMeshName", $ref: "text8" },
    { name: "ownerNodeRid", $ref: "rid" },
    { name: "ownerNodeGeneration", $ref: "nonzero-u64" },
    { name: "relocationState", $ref: "authority-relocation-state" },
    { name: "activationRecoveryState", $ref: "authority-activation-recovery-state" },
  ], "$.types", "authority payload must keep generations in provider metadata and use one closed object identity");
  const authorityRelocation = types.get("authority-relocation-state");
  if (authorityRelocation?.maximumEncodedBytes?.$bound !== "authorityEnvelopeBytes"
      || authorityRelocation?.bodyLengthType?.$ref !== "u32") {
    fail("$.types", "authority relocation aggregate must use the durable bound and u32 body length");
  }
  const relocationCase = authorityRelocation?.cases?.find((entry) => entry.when?.hasRelocation === "true");
  requireFields(relocationCase?.fields, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "ordinal-or-zero" },
    { name: "sourceNodeRid", $ref: "rid" },
    { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
    { name: "sourceOwnerId", $ref: "text8" },
    { name: "sourceOwnerLeaseGeneration", $ref: "nonzero-u64" },
    { name: "targetNodeRid", $ref: "optional-rid" },
    { name: "targetNodeGeneration", $ref: "ordinal-or-zero" },
    { name: "targetOwnerId", $ref: "optional-text8" },
    { name: "targetOwnerLeaseGeneration", $ref: "ordinal-or-zero" },
    { name: "reservationGeneration", $ref: "ordinal-or-zero" },
    { name: "coordinatorOwnerId", $ref: "text8" },
    { name: "coordinatorLeaseGeneration", $ref: "nonzero-u64" },
    { name: "coordinatorNodeRid", $ref: "rid" },
    { name: "coordinatorNodeGeneration", $ref: "nonzero-u64" },
    { name: "phase", $ref: "relocation-phase" },
    { name: "relocationRoot", $ref: "relocation-root-pointer" },
    { name: "applicationVersion", $ref: "application-version" },
    { name: "participantProgress", $ref: "participant-progress-vector" },
    { name: "terminalCompletionCount", $ref: "u32" },
    { name: "pendingRelayCount", $ref: "u32" },
    { name: "sourceCleanupState", $ref: "source-cleanup-state" },
  ], "$.types", "early relocation phases must allow an absent target fence until Prepared");
  const activationRecovery = types.get("authority-activation-recovery-state");
  const activationPresent = activationRecovery?.cases?.find(
    (entry) => entry.when?.hasActivationRecovery === "true",
  );
  requireFields(activationPresent?.fields, [
    { name: "reference", $ref: "creation-content-reference" },
    { name: "sha256", $ref: "sha256-bytes" },
    { name: "encodedSize", $ref: "creation-request-size" },
    { name: "inboxSequence", $ref: "nonzero-u64" },
    { name: "replayCursor", $ref: "ordinal-or-zero" },
  ], "$.types", "Ready Instance activation recovery must preserve exact root and cursor");
  if (activationRecovery?.presence
        !== "ready-instance-spot-cold-activation-only-and-forbidden-for-actor-entry-user-closing-relocating-or-coldActivating-authority"
      || activationRecovery?.release
        !== "expected-store-version-preserve-cas-only-after-durable-first-handler-terminal-completion-and-replay-cursor-equals-inbox-sequence-before-relocation-store-delete"
      || activationRecovery?.constraints?.[0]?.rule
        !== "replayCursor-less-than-or-equal-to-inboxSequence") {
    fail("$.types", "activation recovery pointer must be Ready Instance-only and release only after durable terminal completion and cursor update");
  }

  const authorityOperation = types.get("authority-operation-kind");
  if (JSON.stringify(authorityOperation?.values) !== JSON.stringify([
    { name: "steady", value: 0 },
    { name: "coldActivation", value: 1 },
    { name: "maintenanceRelocation", value: 2 },
    { name: "close", value: 3 },
  ])) {
    fail("$.types", "authority operation kind must include steady owner authority as value zero");
  }
  const authorityObject = types.get("authority-object-identity");
  if (JSON.stringify((authorityObject?.cases ?? []).map((entry) => entry.when))
      !== JSON.stringify([{ objectKind: "actor" }, { objectKind: "spot" }])
      || authorityObject?.discriminators?.[0]?.$ref !== "authority-object-kind") {
    fail("$.types", "authority object identity must be a closed Actor or shared Spot aggregate union");
  }
  const authoritySpotCase = authorityObject?.cases?.find(
    (entry) => entry.when?.objectKind === "spot",
  );
  requireFields(authoritySpotCase?.fields, [
    { name: "spot", $ref: "spot-authority-identity" },
  ], "$.types", "all Spot kinds must use the single state-bearing Spot authority aggregate");
  const authorityActorCase = authorityObject?.cases?.find(
    (entry) => entry.when?.objectKind === "actor",
  );
  requireFields(authorityActorCase?.fields, [
    { name: "actor", $ref: "actor-authority-identity" },
  ], "$.types", "Actor authority must use the complete current routing identity");
  requireFields(types.get("actor-authority-identity")?.fields, [
    { name: "actorType", $ref: "text8" },
    { name: "actorId", $ref: "text8" },
    { name: "state", $ref: "actor-authority-state" },
    { name: "currentSpot", $ref: "spot-ref" },
    { name: "currentSpotKind", $ref: "actor-spot-kind" },
  ], "$.types", "Actor authority must atomically carry immutable type and current Spot projection");
  const spotAuthority = types.get("spot-authority-identity");
  const expectedSpotKinds = new Map([
    ["entry", [
      { name: "spotId", $ref: "text8" },
      { name: "spotType", $ref: "text8" },
      { name: "state", $ref: "entry-user-spot-authority-state" },
    ]],
    ["user", [
      { name: "spotId", $ref: "text8" },
      { name: "spotType", $ref: "text8" },
      { name: "state", $ref: "entry-user-spot-authority-state" },
    ]],
    ["instance", [{ name: "instance", $ref: "instance-authority-identity" }]],
  ]);
  if (spotAuthority?.discriminators?.[0]?.$ref !== "spot-kind"
      || (spotAuthority?.cases ?? []).length !== expectedSpotKinds.size) {
    fail("$.types", "Spot authority must distinguish Entry, User and Instance in one closed aggregate");
  }
  for (const spotCase of spotAuthority?.cases ?? []) {
    const kind = spotCase.when?.spotKind;
    if (!expectedSpotKinds.has(kind)
        || JSON.stringify(fieldShape(spotCase.fields)) !== JSON.stringify(expectedSpotKinds.get(kind))) {
      fail("$.types", `Spot authority kind ${kind} does not match the shared Spot ID aggregate contract`);
    }
  }
  const instanceAuthority = types.get("instance-authority-identity");
  const expectedAuthorityStates = new Map([
    ["coldActivating", [
      { name: "instanceType", $ref: "text8" },
      { name: "spotId", $ref: "text8" },
    ]],
    ["ready", [
      { name: "instanceType", $ref: "text8" },
      { name: "spotId", $ref: "text8" },
    ]],
    ["closing", [
      { name: "instanceType", $ref: "text8" },
      { name: "spotId", $ref: "text8" },
    ]],
    ["relocating", [
      { name: "instanceType", $ref: "text8" },
      { name: "spotId", $ref: "text8" },
    ]],
  ]);
  if ((instanceAuthority?.cases ?? []).length !== expectedAuthorityStates.size) {
    fail("$.types", "Instance authority identity must distinguish cold activation, ready, closing and maintenance relocation");
  }
  for (const authorityCase of instanceAuthority?.cases ?? []) {
    const state = authorityCase.when?.authorityState;
    if (!expectedAuthorityStates.has(state)
        || JSON.stringify(fieldShape(authorityCase.fields))
          !== JSON.stringify(expectedAuthorityStates.get(state))) {
      fail("$.types", `Instance authority state ${state} does not match its generation and owner-token contract`);
    }
  }

  requireFields(commands.get("replyRelay")?.body, [
    { name: "operation", $ref: "operation-id" },
    { name: "replyRouteId", $ref: "nonzero-u64" },
    { name: "context", $ref: "reply-relay-context" },
    { name: "terminalResult", $ref: "request-terminal-result" },
    { name: "failureCode", $ref: "framework-error-code" },
  ], "$.commands", "replyRelay must use the closed cold-activation or maintenance-relocation context");
  requireFields(commands.get("reply")?.body, [
    { name: "correlation", $ref: "nonzero-u64" },
    { name: "terminalResult", $ref: "request-terminal-result" },
    { name: "failureCode", $ref: "framework-error-code" },
    { name: "tail", $ref: "request-specific-tail" },
  ], "$.commands", "reply must carry the closed terminal result and Framework failure code");
  requireFields(types.get("request-completion-entry")?.fields, [
    { name: "operationId", $ref: "operation-id" },
    { name: "requestSource", $ref: "request-source-fence" },
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "sequence", $ref: "nonzero-u64" },
    { name: "terminalResult", $ref: "request-terminal-result" },
    { name: "failureCode", $ref: "framework-error-code" },
    { name: "deliveryState", $ref: "completion-delivery-state" },
    { name: "hasPayload", $ref: "bool8" },
    { name: "payload", $ref: "application-payload-envelope-v1" },
  ], "$.types", "durable completion must use the stable Framework failure code");
  requireFields(types.get("relocation-control-data")?.fields, [
    { name: "phase", $ref: "relocation-phase" },
    { name: "role", $ref: "relocation-role" },
    { name: "relocation", $ref: "relocation-id" },
    { name: "object", $ref: "relocation-object-identity" },
    { name: "terminalResult", $ref: "request-terminal-result" },
    { name: "failureCode", $ref: "framework-error-code" },
  ], "$.types", "frozen relocation control must use the same closed terminal outcome as live replies");
  const terminalFrozenBody = types.get("frozen-record-body");
  const frozenCompletion = terminalFrozenBody?.cases?.find(
    (entry) => entry.when?.recordKind === "completion",
  );
  if (frozenCompletion?.fields?.find((field) => field.name === "failureCode")?.$ref
      !== "framework-error-code") {
    fail("$.types", "frozen completion must use the stable Framework failure code");
  }
  const relocationEnvelope = types.get("relocation-envelope-v1");
  const relocationApplicationStates = types.get(
    "relocation-participant-application-state-vector",
  );
  if (relocationApplicationStates?.maximumItems?.$bound
        !== "maintenanceAggregateParticipants"
      || relocationEnvelope?.fields?.find((field) => field.name === "applicationStates")?.$ref
      !== "relocation-participant-application-state-vector"
      || (relocationEnvelope?.fields ?? []).some((field) => [
        "applicationState", "capturedStoreTimeMs", "stateContractId", "serializerIdentity",
      ].includes(field.name))) {
    fail("$.types", "relocation application state must use the participant-indexed opaque state vector");
  }
  requireFields(relocationEnvelope?.fields, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "object", $ref: "relocation-object-identity" },
    { name: "applicationVersion", $ref: "application-version" },
    {
      name: "applicationStates",
      $ref: "relocation-participant-application-state-vector",
    },
    { name: "participantProgress", $ref: "participant-progress-vector" },
    { name: "journal", $ref: "journal-vector" },
    { name: "timerRegistrations", $ref: "relocation-timer-registration-vector" },
    { name: "pendingTimerTicks", $ref: "relocation-pending-timer-tick-vector" },
    { name: "terminalCompletions", $ref: "request-completion-vector" },
  ], "$.types", "Relocation envelope must carry deterministic queue, timer and completion state");
  requireFields(types.get("relocation-manifest-v1")?.fields, [
    { name: "logicalFormatVersion", $ref: "u8", constant: 1 },
    { name: "totalLength", $ref: "relocation-logical-length" },
    { name: "totalChecksumCrc32c", $ref: "u32" },
    { name: "inventoryDigestSha256", $ref: "sha256-bytes" },
    { name: "chunks", $ref: "relocation-chunk-vector-v1" },
  ], "$.types", "Relocation manifest must carry a lookup-only digest matching Location authority inventory");
  const relocationState = types.get("relocation-application-state");
  const recreateState = relocationState?.cases?.find((entry) => entry.when?.hasState === "false");
  const snapshotState = relocationState?.cases?.find((entry) => entry.when?.hasState === "true");
  if (relocationState?.bodyLengthType?.$ref !== "u64"
      || JSON.stringify(recreateState?.fields) !== "[]"
      || JSON.stringify(fieldShape(snapshotState?.fields)) !== JSON.stringify([
        { name: "payload", $ref: "durable-state-blob" },
      ])) {
    fail("$.types", "relocation state must be a closed Recreate or opaque Snapshot union");
  }
  const durableStateBlob = types.get("durable-state-blob");
  if (durableStateBlob?.minimumBytes !== 0
      || durableStateBlob?.maximumBytes?.$bound !== "relocationChunkBytes") {
    fail("$.types", "participant Snapshot bytes must allow empty payloads and cap each adapter result at 64 MiB");
  }
  requireFields(types.get("relocation-participant-application-state")?.fields, [
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "applicationState", $ref: "relocation-application-state" },
  ], "$.types", "each relocation participant must carry exactly one policy-selected application state");
  requireFields(types.get("relocation-timer-registration")?.fields, [
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "name", $ref: "text8" },
    { name: "handlerType", $ref: "text8" },
    { name: "periodMilliseconds", $ref: "nonzero-u64" },
    { name: "overrunPolicy", $ref: "timer-overrun-policy-kind" },
    { name: "maxCatchUpTicks", $ref: "nonzero-u64" },
    { name: "stopOnUnhandledException", $ref: "bool8" },
    { name: "lastCompletedDeliveryIndex", $ref: "ordinal-or-zero" },
    { name: "lastCompletedScheduledIndex", $ref: "ordinal-or-zero" },
    { name: "nextScheduledAtUnixMilliseconds", $ref: "u64" },
  ], "$.types", "logical timer registration must carry only deterministic Framework-owned state");
  if ((types.get("relocation-timer-registration")?.fields ?? []).some(
    (field) => /native|timerHandle|backend|continuation/i.test(field.name),
  )) {
    fail("$.types", "logical timer registration cannot contain native handle or backend state");
  }
  requireFields(types.get("relocation-pending-timer-tick")?.fields, [
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "sequence", $ref: "nonzero-u64" },
    { name: "timerName", $ref: "text8" },
    { name: "deliveryIndex", $ref: "nonzero-u64" },
    { name: "scheduledIndex", $ref: "nonzero-u64" },
    { name: "scheduledAtUnixMilliseconds", $ref: "u64" },
    { name: "skippedTicks", $ref: "ordinal-or-zero" },
  ], "$.types", "pending timer tick must carry deterministic mailbox ordering state");
  const relayContext = types.get("reply-relay-context");
  if (JSON.stringify((relayContext?.cases ?? []).map((entry) => entry.when))
      !== JSON.stringify([{ contextKind: "coldActivation" }, { contextKind: "maintenanceRelocation" }])) {
    fail("$.types", "reply relay context must be a closed cold-activation or maintenance-relocation union");
  }
  const maintenanceRelay = relayContext?.cases?.find(
    (entry) => entry.when?.contextKind === "maintenanceRelocation",
  );
  requireFields(maintenanceRelay?.fields, [
    { name: "relocation", $ref: "relocation-id" },
    { name: "targetAttemptGeneration", $ref: "nonzero-u64" },
    { name: "coordinator", $ref: "relocation-coordinator-fence" },
    { name: "participantId", $ref: "nonzero-u64" },
    { name: "sequence", $ref: "nonzero-u64" },
  ], "$.types", "maintenance reply relay must carry the complete relocation dedupe key");

  let obsoleteFinalSequence = false;
  walk(schema, "$", (value, location) => {
    if ((location.endsWith(".name") && value === "finalSequence")
        || (typeof value === "string" && value.includes("finalSequence"))) {
      obsoleteFinalSequence = true;
    }
  });
  if (obsoleteFinalSequence) {
    fail("$", "the removed scalar finalSequence contract must not remain in v1");
  }
  const frozen = types.get("frozen-record");
  requireFields(frozen?.fields, [
    { name: "recordKind", $ref: "mesh-record-kind" },
    { name: "source", $ref: "frozen-source-identity" },
    { name: "hasMetadata", $ref: "bool8" },
    { name: "metadata", $ref: "metadata-frame" },
    { name: "operationId", $ref: "operation-id" },
    { name: "operationKind", $ref: "mesh-operation-kind" },
    { name: "replyRoute", $ref: "frozen-reply-route" },
    { name: "body", $ref: "frozen-record-body" },
  ], "$.types", "frozen record must preserve only the exact request reply route, not transport-local opaque state");
  const frozenReplyRoute = types.get("frozen-reply-route");
  const requestKinds = ["nodeRequest", "channelRequest", "spotRequest", "actorRequest", "instanceSpotRequest"];
  if (frozenReplyRoute?.bodyLengthType?.$ref !== "u16"
      || JSON.stringify((frozenReplyRoute?.cases ?? []).map((entry) => ({
        when: entry.when,
        fields: fieldShape(entry.fields),
      }))) !== JSON.stringify(requestKinds.map((originalOperationKind) => ({
        when: { originalOperationKind },
        fields: [{ name: "replyRouteId", $ref: "nonzero-u64" }],
      })))
      || JSON.stringify(frozenReplyRoute?.otherwise) !== JSON.stringify({ fields: [] })) {
    fail("$.types", "frozen reply route must be present only for the closed request operation set");
  }
  const frozenSource = types.get("frozen-source-identity");
  const expectedFrozenSources = new Map([
    ["node", [
      { name: "sourceNodeRid", $ref: "rid" },
      { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
      { name: "sourceOwnerId", $ref: "text8" },
      { name: "sourceOwnerLeaseGeneration", $ref: "nonzero-u64" },
    ]],
    ["spot", [
      { name: "sourceNodeRid", $ref: "rid" },
      { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
      { name: "sourceOwnerId", $ref: "text8" },
      { name: "sourceOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sourceSpotId", $ref: "text8" },
    ]],
    ["actor", [
      { name: "sourceNodeRid", $ref: "rid" },
      { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
      { name: "sourceOwnerId", $ref: "text8" },
      { name: "sourceOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sourceActor", $ref: "actor-ref" },
    ]],
    ["boundSession", [
      { name: "sourceNodeRid", $ref: "rid" },
      { name: "sourceNodeGeneration", $ref: "nonzero-u64" },
      { name: "sourceOwnerId", $ref: "text8" },
      { name: "sourceOwnerLeaseGeneration", $ref: "nonzero-u64" },
      { name: "sourceActor", $ref: "actor-ref" },
      { name: "sourceSessionRid", $ref: "rid" },
      { name: "sourceBindingGeneration", $ref: "nonzero-u64" },
      { name: "sourceSessionSequence", $ref: "nonzero-u64" },
    ]],
  ]);
  if ((frozenSource?.cases ?? []).length !== expectedFrozenSources.size) {
    fail("$.types", "frozen source identity must be a closed node, Spot, Actor or bound-session union");
  } else {
    for (const sourceCase of frozenSource.cases) {
      const kind = sourceCase.when?.sourceKind;
      if (!expectedFrozenSources.has(kind)
          || JSON.stringify(fieldShape(sourceCase.fields)) !== JSON.stringify(expectedFrozenSources.get(kind))) {
        fail("$.types", `frozen source identity ${kind} has an invalid field combination`);
      }
    }
  }
  if ((frozen?.fields ?? []).some((field) => field.name === "kindData")) {
    fail("$.types", "frozen record cannot contain opaque kindData");
  }
  const recordEnum = types.get("mesh-record-kind");
  const frozenBody = types.get("frozen-record-body");
  if (recordEnum?.values.length !== frozenBody?.cases.length) {
    fail("$.types", "frozen record body must cover every record kind exactly once");
  }
  for (const kind of ["actorSend", "actorRequest"]) {
    const actorCase = frozenBody?.cases?.find((entry) => entry.when?.recordKind === kind);
    if (actorCase?.fields?.find((field) => field.name === "targetActor")?.$ref !== "actor-route-fence") {
      fail("$.types", `frozen ${kind} must preserve the accepted Actor authority fence`);
    }
  }
  const frozenInstance = frozenBody?.cases?.find(
    (entry) => entry.when?.recordKind === "instanceSpotActivation",
  );
  if ((frozenInstance?.fields ?? []).some((field) => field.name === "timeoutMs")) {
    fail("$.types", "frozen Instance activation must not copy the source-owned operation timeout");
  }
  for (const [typeName, fieldName] of [
    ["journal-entry", "sequence"],
    ["request-completion-entry", "sequence"],
  ]) {
    if (types.get(typeName)?.fields?.find((field) => field.name === fieldName)?.$ref !== "nonzero-u64") {
      fail("$.types", `${typeName}.${fieldName} must start at one; zero means no record only in progress values`);
    }
  }
  if (commands.get("relocationData")?.body?.find((field) => field.name === "sequence")?.$ref
      !== "nonzero-u64") {
    fail("$.commands", "relocationData sequence must be non-zero and must never wrap");
  }
  for (const type of types.values()) {
    if (type.kind === "struct") {
      validateEnclosingFieldReferences(type.fields, types, `type:${type.name}`, fail);
    } else if (type.kind === "versioned-length-delimited") {
      validateEnclosingFieldReferences(type.body, types, `type:${type.name}`, fail);
    } else if (type.kind === "conditional-union") {
      for (const [index, unionCase] of (type.cases ?? []).entries()) {
        validateEnclosingFieldReferences(
          unionCase.fields,
          types,
          `type:${type.name}.cases[${index}]`,
          fail,
        );
      }
    }
  }
  for (const command of schema.commands) {
    validateEnclosingFieldReferences(command.body, types, `command:${command.name}`, fail);
  }
}

function validateEnclosingFieldReferences(fields, types, location, fail) {
  const prior = new Map();
  for (const field of fields ?? []) {
    const referenced = types.get(field.$ref);
    if (referenced?.kind === "conditional-union") {
      for (const discriminator of referenced.discriminators ?? []) {
        const enclosing = discriminator.source?.enclosingField;
        if (typeof enclosing !== "string") {
          continue;
        }
        const owner = prior.get(enclosing);
        if (!owner || owner.$ref !== discriminator.$ref) {
          fail(location,
            `${field.name} discriminator ${enclosing} must reference an earlier field of ${discriminator.$ref}`);
        }
      }
    }
    prior.set(field.name, field);
  }
}

function validateCommands(commands, ranges, flags, contexts, types, bounds, fail) {
  const normalizedRanges = validateReservedRanges(ranges, fail);
  if (!Array.isArray(commands)) {
    fail("$.commands", "commands must be an array so duplicate IDs remain detectable");
    return;
  }
  const ids = new Set();
  const names = new Set();
  commands.forEach((command, index) => {
    const location = `$.commands[${index}]`;
    if (!isObject(command)) {
      fail(location, "command must be an object");
      return;
    }
    if (!Number.isSafeInteger(command.id) || command.id <= 0 || command.id > 255) {
      fail(`${location}.id`, "command ID must be in 1..255");
    } else {
      if (ids.has(command.id)) {
        fail(`${location}.id`, `duplicates command ID ${command.id}`);
      }
      ids.add(command.id);
      if (normalizedRanges.some(({ first, last }) => command.id >= first && command.id <= last)) {
        fail(`${location}.id`, `command ID ${command.id} is reserved`);
      }
    }
    if (typeof command.name !== "string" || command.name.length === 0) {
      fail(`${location}.name`, "command needs a non-empty name");
    } else if (names.has(command.name)) {
      fail(`${location}.name`, `duplicates command name ${command.name}`);
    } else {
      names.add(command.name);
    }
    if (!COMMAND_DOMAINS.has(command.domain)) {
      fail(`${location}.domain`, "must be application or infrastructure");
    }
    if (!PAYLOAD_POLICIES.has(command.payload)) {
      fail(`${location}.payload`, "must be forbidden, optional or required");
    }
    if (command.payload === "forbidden") {
      if (hasOwn(command, "payloadType")) {
        fail(`${location}.payloadType`, "forbidden payload command cannot declare a payload type");
      }
    } else if (!isObject(command.payloadType)
        || typeof command.payloadType.$ref !== "string"
        || !types.has(command.payloadType.$ref)) {
      fail(`${location}.payloadType`, "allowed payload must reference its exact encoded type");
    }

    const allowedFlags = validateFlagList(command.allowedFlags, `${location}.allowedFlags`, flags, fail);
    const requiredFlags = validateFlagList(command.requiredFlags, `${location}.requiredFlags`, flags, fail);
    for (const name of requiredFlags) {
      if (!allowedFlags.has(name)) {
        fail(`${location}.requiredFlags`, `required flag ${name} is not allowed`);
      }
    }

    if (hasOwn(command, "flagConstraints")) {
      if (!Array.isArray(command.flagConstraints)) {
        fail(`${location}.flagConstraints`, "must be an array");
      } else {
        command.flagConstraints.forEach((constraint, constraintIndex) => {
          const constraintLocation = `${location}.flagConstraints[${constraintIndex}]`;
          if (!isObject(constraint) || !["all-or-none", "implies"].includes(constraint.kind)) {
            fail(constraintLocation, "unknown flag constraint");
            return;
          }
          const referenced = constraint.kind === "all-or-none"
            ? constraint.flags
            : [constraint.if, ...(constraint.then ?? [])];
          if (!Array.isArray(referenced) || referenced.length < 2) {
            fail(constraintLocation, "flag constraint must reference at least two flags");
            return;
          }
          for (const name of referenced) {
            if (!allowedFlags.has(name)) {
              fail(constraintLocation, `constraint references flag ${name} that the command does not allow`);
            }
          }
        });
      }
    }

    validateFields(command.body, `${location}.body`, contexts, allowedFlags, types, bounds, fail);
    validateConditions(command, location, contexts, allowedFlags, fail);
  });
}

function validateReservedRanges(ranges, fail) {
  if (!Array.isArray(ranges)) {
    fail("$.reservedCommandRanges", "must be an array");
    return [];
  }
  const normalized = [];
  ranges.forEach((range, index) => {
    const location = `$.reservedCommandRanges[${index}]`;
    if (!isObject(range) || !Number.isSafeInteger(range.first)
        || !Number.isSafeInteger(range.last) || range.first < 0
        || range.last > 255 || range.first > range.last) {
      fail(location, "range must be ordered within 0..255");
      return;
    }
    for (const prior of normalized) {
      if (range.first <= prior.last && prior.first <= range.last) {
        fail(location, "overlaps another reserved command range");
      }
    }
    normalized.push({ first: range.first, last: range.last });
  });
  return normalized;
}

function validateLivenessProfile(profile, commands, fail) {
  const location = "$.livenessProfile";
  const expected = {
    probeCommand: "livenessProbe",
    ackCommand: "livenessAck",
    scope: "admitted-physical-connection-lifetime",
    probeIdRule: "connection-local-nonzero-u64-exact-echo",
    initialReady: "successful-admission-starts-peer-timeout-deadline",
    outstandingProbeMaximum: 1,
    probeSchedule: "every-five-seconds-independent-of-application-traffic",
    probeTickWithoutOutstanding: "allocate-and-send-new-nonzero-connection-local-id",
    probeTickWithOutstanding: "retransmit-same-id-without-growing-outstanding-set",
    deadlineRefreshEvidence: "first-ack-matching-current-outstanding-id-on-same-connection-only",
    matchingAckEffect: "refresh-peer-timeout-deadline-and-clear-outstanding-id",
    duplicatePreviousOrOtherConnectionAck: "ignore-without-deadline-refresh",
    otherInboundTraffic: "diagnostics-only-never-resets-round-trip-deadline",
    idleProbeIntervalMs: 5000,
    peerTimeoutMs: 15000,
    orderlyDisconnect: "immediate-not-ready",
    staleAck: "ignore-by-outstanding-id-and-connection-identity",
    scheduler: "infrastructure-reserve",
    applicationDelivery: "forbidden",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail(location, "must define the exact fixed service-liveness contract");
    return;
  }

  const commandMap = new Map((commands ?? []).map((command) => [command.name, command]));
  const expectedCommands = [
    { name: profile.probeCommand, id: 5 },
    { name: profile.ackCommand, id: 6 },
  ];
  for (const expectedCommand of expectedCommands) {
    const command = commandMap.get(expectedCommand.name);
    const body = command?.body ?? [];
    if (command?.id !== expectedCommand.id
        || command?.domain !== "infrastructure"
        || JSON.stringify(command?.allowedFlags) !== "[]"
        || JSON.stringify(command?.requiredFlags) !== "[]"
        || command?.payload !== "forbidden"
        || hasOwn(command ?? {}, "payloadType")
        || JSON.stringify(body) !== JSON.stringify([
          { name: "probeId", $ref: "nonzero-u64" },
        ])) {
      fail("$.commands", `${expectedCommand.name} must use its fixed ID, closed probe ID body, and no flags or payload`);
    }
  }
}

function validateFanoutLivenessProfile(profile, fail) {
  const location = "$.fanoutLivenessProfile";
  const expected = {
    scope: "classic-fanout-publisher-lifetime",
    direction: "publisher-to-subscriber",
    subscriberSocket: "one-dedicated-socket-per-publisher",
    publisherAssociation: "automatic-descriptor-or-manual-endpoint",
    topicFrameBytes: [1, 90, 76, 70, 49],
    reservedTopicMatch: "exact-only",
    publicTopicSources: ["explicit-topic", "typed-packet-name-derived-topic"],
    publicTopicValidation: "reject-exact-reserved-topic-after-derivation-before-transport",
    publicTopicFailure: "argument-or-configuration-error",
    payloadFrameBytes: [90, 70, 1, 1],
    multipartFrameCount: 2,
    subscriberBeaconSubscription: "exact-reserved-topic-always-in-addition-to-application-filters",
    malformedReservedTopic: "protocol-error-immediate-not-ready",
    receiveActivity: "valid-application-record-or-exact-beacon-on-dedicated-socket",
    initialReady: "first-valid-receive",
    beaconIntervalMs: 5000,
    beaconSchedule: "periodic-independent-of-all-application-topic-traffic",
    publisherTimeoutMs: 15000,
    ack: "forbidden",
    orderlyDisconnect: "immediate-not-ready",
    scheduler: "infrastructure-reserve",
    applicationDelivery: "forbidden",
    publicOption: "forbidden",
    locationLease: "independent",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail(location, "must define the exact fixed one-way fanout-liveness contract");
  }
}

function validateAuthorityKeyFormat(format, fail) {
  const expected = {
    name: "authority-key-v1",
    encoding: "canonical-ascii-utf8",
    prefix: "zla1",
    separator: ":",
    kindDiscriminators: [
      { objectKind: "actor", wire: "a", components: ["actorId"] },
      { objectKind: "spot", wire: "s", components: ["spotId"] },
    ],
    componentLayout: "decimal-raw-byte-length-colon-percent-encoded-bytes",
    componentRawBytesMinimum: 1,
    componentRawBytesMaximum: 255,
    decimalLength: "base10-no-leading-zero",
    escaping: "rfc3986-unreserved-literal-otherwise-uppercase-percent-hex",
    unicodeNormalization: "none",
    maximumEncodedBytes: 776,
    goldenFixture: "golden/authority-key-v1.json",
  };
  if (!isObject(format) || JSON.stringify(format) !== JSON.stringify(expected)) {
    fail("$.authorityKeyFormat", "must define the exact canonical authority-key-v1 contract");
  }
}

function validateAuthorityStoreAccessProfile(profile, fail) {
  const expected = {
    directResolve: "exact-canonical-key-read",
    enumerationPrefixes: [
      { objectKind: "actor", prefix: "zla1:a:" },
      { objectKind: "spot", prefix: "zla1:s:" },
    ],
    pageSizeMinimum: 1,
    pageSizeMaximum: 1000,
    pageEncodedBytesMaximum: { $bound: "authorityScanPageBytes" },
    pageFill: "return-fewer-items-before-byte-cap-and-reject-single-row-over-authority-envelope-bound",
    item: "opaque-canonical-key-payload-store-version-and-generation-metadata",
    providerInterpretation: "forbidden",
    continuationToken: {
      type: "opaque-provider-issued-authority-scan-cursor",
      firstPage: "absent",
      nextPage: "optional-nonempty",
      maximumEncodedBytes: 4096,
      frameworkInterpretationOrComposition: "forbidden",
      replayOrCrossScanMix: "scanExpired",
    },
    pageOrder: "unsigned-canonical-key-bytes-ascending",
    scanConsistency: "first-page-captures-provider-snapshot-watermark",
    snapshotCoverage: "every-row-incarnation-existing-at-watermark-exactly-once-across-pages",
    concurrentMutation: "delete-may-be-missing-on-exact-read-and-post-watermark-create-or-recreate-waits-next-scan",
    candidateValidation: "exact-read-then-expected-store-version-cas",
    startupRecoveryGate: "complete-snapshot-scan-for-all-registered-mesh-scopes-before-serving",
    backgroundRecovery: "repeat-snapshot-scans-after-serving",
    operationalEnumeration: "may-reuse-pages-but-cannot-weaken-recovery-scan",
    typedListProjection: "framework-decoded-observability-only-not-routing-authority",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.authorityStoreAccessProfile", "must define exact-key routing reads and opaque paged prefix enumeration");
  }
}

function validateDescriptorEnumerationProfile(profile, fail) {
  const expected = {
    pageSizeMinimum: 1,
    pageSizeMaximum: 1000,
    pageEncodedBytesMaximum: { $bound: "descriptorPageBytes" },
    pageFill: "return-fewer-items-before-byte-cap-and-reject-single-descriptor-over-descriptor-envelope-bound",
    continuationToken: "opaque-provider-cursor",
    stableSnapshot: "read-scope-change-stamp-before-pages-and-after-pages-apply-only-when-equal-otherwise-retry",
    unboundedProviderMaterialization: "forbidden",
    startupDescriptorValidation: "derive-complete-descriptor-and-validate-atomically-before-publish",
    descriptorRevision: "framework-caller-issued-nonzero-u64-monotonic-per-descriptor-never-provider-counter",
    descriptorRevisionExhaustion: "host-error-seal-no-wrap-no-publish",
    overflow: "configuration-failure-never-truncate-split-or-publish-partial",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.descriptorEnumerationProfile", "must define bounded stable descriptor pages and atomic startup validation");
  }
}

function validateAuthorityStoreGenerationProfile(profile, fail) {
  const expected = {
    metadata: ["objectGeneration", "authorityOwnerGeneration"],
    generationEncoding: "nonzero-u64-provider-issued",
    providerDomainCounters: ["objectGeneration", "authorityOwnerGeneration", "storeRevision"],
    expectation: {
      kind: "closed-missing-or-found",
      missing: "assert-current-absence",
      found: "exact-store-version",
    },
    mutationTransitions: {
      preserve: "found-only-keep-object-and-owner-generations-consume-store-revision",
      newOwner: "found-only-keep-object-generation-consume-authority-owner-generation-and-store-revision",
      newObject: "missing-only-consume-object-generation-authority-owner-generation-and-store-revision",
      delete: "found-only-consume-store-revision-remove-row-and-current-index-entry",
    },
    atomicity: "validate-expectation-before-atomically-consuming-counters-and-mutating-row-index",
    authorityOwnerToken: "framework-ownerId-plus-provider-authorityOwnerGeneration",
    delete: "remove-row-with-no-per-key-generation-or-version-tombstone",
    scanTombstone: "retain-only-until-all-older-active-scan-leases-finish-or-expire",
    generationMaximum: "9223372036854775807",
    overflow: "GenerationExhausted-non-retriable-no-row-index-or-counter-mutation-or-consumption",
    opaquePayloadGenerationDuplication: "forbidden",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.authorityStoreGenerationProfile", "must define provider-issued atomic object and owner generations");
  }
}

function validateAuthorityStoreDurabilityProfile(profile, fail) {
  const expected = {
    rowLifetime: "durable-until-explicit-fenced-delete",
    rowTtl: "forbidden",
    snapshotLeaseFields: "forbidden",
    putLeaseDuration: "forbidden",
    ownerAndCoordinatorLiveness: "separate-owner-lease-token",
    expiredOwnerRecovery: "expected-store-version-cas-with-newOwner-transition",
    terminalCleanup: "explicit-expected-store-version-delete",
    generationCountersAfterDelete: "global-provider-domain-counters-only-no-per-key-retention",
    providerPayloadInterpretation: "forbidden",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.authorityStoreDurabilityProfile", "must keep authority rows durable and separate owner liveness leases");
  }
}

function validateOwnerLeaseAuthorityProfile(profile, fail) {
  const expected = {
    ownerId: "framework-generated-lifecycle-unique-not-application-configurable-or-reusable",
    token: ["ownerId", "leaseGeneration"],
    generationRange: "1..9223372036854775807",
    generationSource: "one-durable-global-counter-per-provider-transaction-domain",
    operations: {
      claimOwnerLease: {
        input: ["ownerId", "ttl"],
        result: "claimed-token-expiresAt-storeNow-or-conflict-or-GenerationExhausted",
      },
      readOwnerLease: {
        input: ["ownerId"],
        result: "found-token-expiresAt-storeNow-or-missing",
      },
      renewOwnerLease: {
        input: ["token", "ttl"],
        result: "renewed-token-expiresAt-storeNow-or-stale",
      },
      releaseOwnerLease: {
        input: ["token"],
        result: "released-or-stale",
      },
    },
    scope: "one-host-process-lifecycle-token-shared-by-all-host-descriptors-and-object-authorities",
    descriptorAndListSnapshot: "preserve-current-owner-leaseGeneration-with-route-rid-owned-separately",
    admissionRecheck: "exact-read-ownerId-and-leaseGeneration-immediately-before-target-admission",
    listUse: "observability-only-never-routing-or-resolve-authority",
    activeRow: "ttl-backed-and-deleted-on-expiry-or-release",
    sameOwnerIdReuse: "new-global-leaseGeneration-keeps-old-token-stale-without-tombstone",
    activeIndex: "bounded-to-active-leases-and-pruned-on-expiry-or-release",
    delayedOperation: "stale-no-mutation",
    overflow: "claim-or-takeover-GenerationExhausted-non-retriable-no-row-index-or-counter-mutation-or-consumption-renew-and-release-excluded",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.ownerLeaseAuthorityProfile", "must define provider-issued lifecycle owner tokens and stale-operation fencing");
  }
}

function validateRelocationRetentionPolicy(policy, fail) {
  const expected = {
    retentionMs: 86400000,
    renewThresholdMs: 43200000,
    leaseOwner: "current-authority-owner-or-recovery-coordinator",
    referenced: "renew-current-reference-before-expiry-using-provider-store-time",
    renewOperation: "RenewManifestTree(root-reference-fixed-retention)",
    renewResult: "all-manifest-and-chunks-renewed-expiresAt-storeNow-or-missing-partial-failure",
    partialRenewFailure: "retry-before-threshold-and-fail-closed-before-any-expiry",
    renewFence: "authority-key-revision-and-current-reference-exact-check-immediately-before-renew",
    stagedWriteTracking: "track-provider-expiresAt-and-storeNow-for-every-unlinked-chunk-and-manifest",
    preLinkGate: "immediately-before-captured-and-prepared-cas-verify-or-renew-complete-tree-to-more-than-renew-threshold-remaining",
    preLinkFailure: "missing-or-partial-renew-aborts-precommit-and-never-links-root",
    concurrentReplacement: "one-stale-renew-may-extend-orphan-only-never-authorizes-read",
    orphan: "not-renewed-and-provider-deletes-at-expiry",
    cleanupOrder: [
      "cas-authority-reference-removal-or-replacement",
      "idempotent-delete-no-longer-referenced-manifest-and-all-chunks",
    ],
    reader: "current-authority-reference-only",
    providerClock: "relocation-storeNow-and-expiresAt-independent-of-location-provider-clock",
    recoveryHorizon: "permanent-published-root-missing-is-non-retriable-RelocationDataLost-no-rollback",
    publicOption: "forbidden",
  };
  if (!isObject(policy) || JSON.stringify(policy) !== JSON.stringify(expected)) {
    fail("$.relocationRetentionPolicy", "must use the fixed renewable 24-hour relocation lease and bounded recovery horizon");
  }
}

function validateRelocationStorageProfile(profile, fail) {
  const expected = {
    logicalStreamType: "relocation-envelope-v1",
    manifestFormat: "relocation-manifest-v1",
    chunkFormat: "relocation-data-chunk-v1",
    chunkDataMaximumBytes: 67108864,
    chunkCountMaximum: 4096,
    logicalMaximumBytes: 274877906944,
    manifestFields: [
      "logicalFormatVersion", "totalLength", "totalChecksumCrc32c",
      "inventoryDigestSha256",
      "ordered-chunk-reference-length-checksum",
    ],
    writeOrder: [
      "all-immutable-chunks", "immutable-root-manifest", "read-and-verify-complete-root",
      "location-authority-reference-cas",
    ],
    authority: "forbidden-payload-lookup-projection-only",
    backend: "may-differ-from-location-store",
    clock: "relocation-provider-storeNow-and-expiresAt-only",
    writeFailure: "unreferenced-chunks-or-manifest-are-orphans-never-authority",
    emptyRelocation: "deterministic-zero-data-manifest-with-no-chunks",
    streaming: "chunk-validated-and-incrementally-decoded-with-bounded-memory",
    recordSplit: "allowed-across-chunk-boundaries",
    oversize: "reversible-seal-rollback-and-blocked-state-incompatible-before-draining",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.relocationStorageProfile", "must define the bounded manifest and immutable relocation chunk contract");
  }
}

function validateFrameworkJsonV1Profile(profile, fail) {
  const expected = {
    name: "framework-json-v1",
    encoding: "utf-8-without-bom",
    byteCanonicalization: "not-required-property-order-and-insignificant-whitespace-ignored",
    propertyNames: "registered-contract-names-exact-and-case-sensitive",
    duplicateProperties: "reject-before-typed-dispatch",
    unknownProperties: "ignore-for-forward-compatible-reader",
    missingRequiredProperties: "reject",
    signedAndUnsigned64: "canonical-decimal-string-without-plus-or-redundant-leading-zero-with-declared-range-check",
    integersUpTo32Bits: "json-number-in-declared-range-without-fraction",
    floatingPoint: "finite-json-number-only-nan-and-infinity-rejected",
    enums: "declared-case-sensitive-string-name",
    bytes: "rfc4648-standard-base64-with-required-padding",
    objectsAndMaps: "object-members-order-insensitive-map-keys-string-only",
    null: "allowed-only-for-contract-declared-nullable-value",
    noImplicitCrossLanguageTypes: ["dateTime", "decimal", "uuid", "languageCustomType"],
    explicitRepresentationForOtherTypes: "contract-defined-string-or-dto",
    applicationBytes: "framework-validates-then-stores-or-forwards-opaque-original-bytes",
    goldenFixture: "golden/framework-json-v1.json",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.frameworkJsonV1Profile", "must define the exact cross-language framework-json-v1 semantic profile");
  }
}

function validateFrameworkMultipartV1Profile(profile, fail) {
  const expected = {
    name: "framework-multipart-v1",
    consumers: ["cpp", "dotnet", "jvm", "node"],
    packetName: "ZLinkFrameworkMultipart",
    contentType: "application/x-zlink-multipart",
    encoding: "u32-big-endian-part-count-then-u32-big-endian-length-prefixed-opaque-parts",
    minimumParts: 1,
    partCountValidation: "validate-count-against-remaining-length-before-part-array-allocation",
    trailingBytes: "forbidden",
    applicationInterpretation: "framework-only-message-part-storage",
    goldenFixture: "golden/framework-multipart-v1.json",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail(
      "$.frameworkMultipartV1Profile",
      "must define the exact cross-language framework-multipart-v1 profile",
    );
  }
}

function validateMaintenanceAdmissionProfile(profile, fail) {
  const expected = {
    preflight: "validate-capability-eligibility-and-bounded-headroom-without-final-reservation",
    reversibleSeal: "hold-new-submissions-without-admission-result",
    sizingSnapshot: "exact-participant-boundaries-messages-and-bytes-after-seal",
    timerAdmissionSeal: "stop-new-tick-admission-at-turn-boundary-current-handler-only-completes",
    acceptedTimerTurns: "unstarted-pending-ticks-enter-deterministic-relocation-envelope",
    futureTimerSchedule: "logical-registration-and-next-schedule-cursor-relocated-and-restored-by-target-framework",
    timerNativeState: "native-handle-backend-state-and-callback-continuation-forbidden",
    readinessScheduling: "infrastructure-notification-per-unit-ready-turn-first-no-kind-or-registration-order",
    permitAcquisition: "nonblocking-all-or-nothing-outbound-inbound-capture-restore-and-byte-before-source-seal",
    permitFailure: "release-all-provisional-permits-keep-source-admission-open-and-requeue-notification",
    defaultPermits: {
      activeOutbound: 64,
      activeInbound: 64,
      captureCallbacks: 8,
      restoreCallbacks: 8,
      encodedPayloadBytesInFlight: 268435456,
    },
    permitConfiguration: "five-application-options-positive-runtime-update-affects-new-admission-only",
    aggregateUnit: "user-spot-and-member-actors-share-one-unit-permit-and-commit-generation",
    byteReservation: "before-seal-participant-64MiB-plus-deterministic-framework-owned-upper-bound",
    byteReconciliation: "after-capture-shrink-to-actual-encoded-size-never-increase",
    oversizedUnit: "single-unit-only-when-byte-window-empty-exclusive-until-byte-permit-release",
    requestDrainBeforeCaptured: "all-connectionBound-accepted-work-and-boundSession-requests-must-be-terminal-and-are-never-journaled",
    requestDrainFailure: "abort-before-captured-retire-blocked-relocationDisabled-and-restore-admission",
    phaseOrder: [
      "preparing-cas",
      "relocation-put",
      "captured-cas",
      "target-reserve",
      "target-factory-and-restore",
      "journal-and-timer-staging",
      "prepared-cas",
    ],
    durableReplayBoundary: "complete-root-linked-by-captured-cas",
    sourceCrashBeforeCaptured: "fenced-abort-no-maintenance-continuity-original-request-uses-normal-connection-failure-timeout-or-cancellation-terminal",
    unlinkedPutAfterCrash: "orphan-cleanup-never-replay-authority",
    rollbackBeforeDraining: "abort-transactions-release-reservations-delete-orphans-and-reopen-serving",
    drainingPublish: "after-all-transactions-prepared",
    queueGrowthAfterSeal: "forbidden",
  };
  if (!isObject(profile) || JSON.stringify(profile) !== JSON.stringify(expected)) {
    fail("$.maintenanceAdmissionProfile", "must close the preflight-to-seal queue-growth race with the exact reversible-seal contract");
  }
}

function validateTerminationResultProfile(profile, fail) {
  const location = "$.terminationResultProfile";
  if (!isObject(profile)) {
    fail(location, "must define the closed host termination result contract");
    return;
  }

  const expectedOutcomes = [
    ["Stopped", 0],
    ["Blocked", 1],
    ["ForceStopped", 2],
  ];
  const expectedReasons = [
    ["None", 0],
    ["TargetUnavailable", 1],
    ["StoreUnavailable", 2],
    ["RelocationDisabled", 3],
    ["StateIncompatible", 4],
    ["DeadlineExceeded", 5],
    ["RelocationFailed", 6],
    ["TeardownFailed", 7],
    ["RuntimeNotReady", 8],
    ["ManualTopologyUnsupported", 9],
  ];
  const expectedPairs = new Map([
    ["Stopped", ["None"]],
    ["Blocked", [
      "TargetUnavailable", "StoreUnavailable", "RelocationDisabled", "StateIncompatible",
      "DeadlineExceeded", "RuntimeNotReady", "ManualTopologyUnsupported",
    ]],
    ["ForceStopped", ["DeadlineExceeded", "RelocationFailed", "TeardownFailed"]],
  ]);

  const validateValues = (entries, expected, field) => {
    const expectedObjects = expected.map(([name, wireValue]) => ({ name, wireValue }));
    if (!Array.isArray(entries)
        || JSON.stringify(entries) !== JSON.stringify(expectedObjects)) {
      fail(`${location}.${field}`, "must preserve the exact closed names and wire values");
    }
  };
  validateValues(profile.outcomes, expectedOutcomes, "outcomes");
  validateValues(profile.reasons, expectedReasons, "reasons");

  const outcomeNames = new Set(expectedOutcomes.map(([name]) => name));
  const reasonNames = new Set(expectedReasons.map(([name]) => name));
  const pairs = new Map();
  if (!Array.isArray(profile.allowedPairs)) {
    fail(`${location}.allowedPairs`, "must contain one entry for each outcome");
  } else {
    for (let index = 0; index < profile.allowedPairs.length; index += 1) {
      const entry = profile.allowedPairs[index];
      const entryLocation = `${location}.allowedPairs[${index}]`;
      if (!isObject(entry) || !outcomeNames.has(entry.outcome) || !Array.isArray(entry.reasons)) {
        fail(entryLocation, "must name one known outcome and its closed reason list");
        continue;
      }
      if (pairs.has(entry.outcome)) {
        fail(entryLocation, `duplicates outcome ${entry.outcome}`);
        continue;
      }
      if (entry.reasons.some((reason) => !reasonNames.has(reason))) {
        fail(`${entryLocation}.reasons`, "contains an unknown termination reason");
      }
      if (new Set(entry.reasons).size !== entry.reasons.length) {
        fail(`${entryLocation}.reasons`, "contains a duplicate termination reason");
      }
      pairs.set(entry.outcome, entry.reasons);
    }
  }
  for (const [outcome, reasons] of expectedPairs) {
    if (JSON.stringify(pairs.get(outcome)) !== JSON.stringify(reasons)) {
      fail(`${location}.allowedPairs`, `${outcome} must allow only ${reasons.join(", ")}`);
    }
  }
  if (pairs.size !== expectedPairs.size) {
    fail(`${location}.allowedPairs`, "must not define outcomes outside the closed result contract");
  }

  const exactFields = {
    undefinedPair: "protocol-error",
    relocationCeilingBeforeDraining: "Blocked/StateIncompatible",
    peerReadinessDeadline: "Blocked/TargetUnavailable",
    preCapturedDeadline: "Blocked/DeadlineExceeded",
    postCapturedDeadline: "ForceStopped/DeadlineExceeded",
    teardownFailure: "ForceStopped/TeardownFailed",
  };
  for (const [field, expected] of Object.entries(exactFields)) {
    if (profile[field] !== expected) {
      fail(`${location}.${field}`, `must be ${expected}`);
    }
  }
}

function validateFlagList(list, location, flags, fail) {
  const result = new Set();
  if (!Array.isArray(list)) {
    fail(location, "must be an array");
    return result;
  }
  for (const name of list) {
    if (typeof name !== "string" || !flags.has(name)) {
      fail(location, `references undefined flag ${JSON.stringify(name)}`);
      continue;
    }
    if (result.has(name)) {
      fail(location, `duplicates flag ${name}`);
    }
    result.add(name);
  }
  return result;
}

function validateConditions(value, location, contexts, allowedFlags, fail) {
  walk(value, location, (candidate, candidateLocation) => {
    if (!isObject(candidate)) {
      return;
    }
    for (const key of ["allFlagsSet", "anyFlagsSet"]) {
      if (!hasOwn(candidate, key)) {
        continue;
      }
      if (!Array.isArray(candidate[key]) || candidate[key].length === 0) {
        fail(`${candidateLocation}.${key}`, "must be a non-empty flag array");
        continue;
      }
      for (const name of candidate[key]) {
        if (allowedFlags !== null && !allowedFlags.has(name)) {
          fail(`${candidateLocation}.${key}`, `references flag ${name} that the command does not allow`);
        }
      }
    }
    if (hasOwn(candidate, "contextEquals")) {
      const condition = candidate.contextEquals;
      if (!isObject(condition) || typeof condition.name !== "string"
          || !contexts.has(condition.name) || !hasOwn(condition, "value")) {
        fail(`${candidateLocation}.contextEquals`, "must reference a declared context and value");
      }
    }
  });
}

function walk(value, location, visitor) {
  visitor(value, location);
  if (Array.isArray(value)) {
    value.forEach((entry, index) => walk(entry, `${location}[${index}]`, visitor));
    return;
  }
  if (!isObject(value)) {
    return;
  }
  for (const [key, entry] of Object.entries(value)) {
    walk(entry, `${location}.${key}`, visitor);
  }
}

function expectInvalid(schema, label, mutate) {
  const candidate = clone(schema);
  mutate(candidate);
  try {
    validateSchema(candidate);
  } catch (error) {
    if (error instanceof SchemaValidationError) {
      return;
    }
    throw error;
  }
  throw new Error(`negative self-test did not fail: ${label}`);
}

function runSelfTests(schema) {
  const tests = [
    ["undefined type", (candidate) => {
      candidate.commands[0].body[0].$ref = "undefined-type";
    }],
    ["unsafe numeric literal", (candidate) => {
      candidate.bounds[0].value = Number.MAX_SAFE_INTEGER + 1;
    }],
    ["unknown flag", (candidate) => {
      candidate.commands[0].allowedFlags.push("unknownFlag");
    }],
    ["required flag outside allowed set", (candidate) => {
      candidate.commands[0].requiredFlags.push("metadata");
    }],
    ["broken flag condition", (candidate) => {
      const actorSend = candidate.commands.find((command) => command.name === "actorSend");
      actorSend.body.at(-1).when.allFlagsSet.push("extension");
    }],
    ["conditional field without forbidden branch", (candidate) => {
      const actorSend = candidate.commands.find((command) => command.name === "actorSend");
      delete actorSend.body.at(-1).otherwise;
    }],
    ["missing closed discriminator case", (candidate) => {
      const identity = candidate.types.find((type) => type.name === "relocation-object-identity");
      identity.cases.pop();
    }],
    ["discriminator case value mismatch", (candidate) => {
      const identity = candidate.types.find((type) => type.name === "relocation-object-identity");
      identity.cases[1].when.objectKind = "spot";
    }],
    ["closed wire union without case length", (candidate) => {
      const identity = candidate.types.find((type) => type.name === "relocation-object-identity");
      delete identity.bodyLengthType;
    }],
    ["reserved command ID", (candidate) => {
      candidate.commands[0].id = 7;
    }],
    ["liveness probe zero-capable ID", (candidate) => {
      candidate.commands.find((command) => command.name === "livenessProbe").body[0].$ref = "u64";
    }],
    ["liveness ack payload", (candidate) => {
      const ack = candidate.commands.find((command) => command.name === "livenessAck");
      ack.payload = "optional";
      ack.payloadType = { $ref: "application-payload-envelope-v1" };
    }],
    ["liveness probe metadata", (candidate) => {
      candidate.commands.find((command) => command.name === "livenessProbe").allowedFlags.push("metadata");
    }],
    ["liveness ack ID", (candidate) => {
      candidate.commands.find((command) => command.name === "livenessAck").id = 7;
    }],
    ["liveness profile timeout", (candidate) => {
      candidate.livenessProfile.peerTimeoutMs = 30000;
    }],
    ["liveness application delivery", (candidate) => {
      candidate.livenessProfile.applicationDelivery = "allowed";
    }],
    ["fanout shared subscriber socket", (candidate) => {
      candidate.fanoutLivenessProfile.subscriberSocket = "one-shared-socket";
    }],
    ["fanout liveness ack", (candidate) => {
      candidate.fanoutLivenessProfile.ack = "required";
    }],
    ["fanout reserved topic", (candidate) => {
      candidate.fanoutLivenessProfile.topicFrameBytes[0] = 2;
    }],
    ["fanout beacon frame", (candidate) => {
      candidate.fanoutLivenessProfile.payloadFrameBytes[3] = 2;
    }],
    ["fanout topic prefix reservation", (candidate) => {
      candidate.fanoutLivenessProfile.reservedTopicMatch = "prefix";
    }],
    ["fanout typed derived topic bypass", (candidate) => {
      candidate.fanoutLivenessProfile.publicTopicSources = ["explicit-topic"];
    }],
    ["fanout malformed reserved frame delivery", (candidate) => {
      candidate.fanoutLivenessProfile.malformedReservedTopic = "application-delivery";
    }],
    ["fanout liveness timeout", (candidate) => {
      candidate.fanoutLivenessProfile.publisherTimeoutMs = 30000;
    }],
    ["fanout beacon depends on application traffic", (candidate) => {
      candidate.fanoutLivenessProfile.beaconSchedule = "idle-only";
    }],
    ["fanout beacon subscription omitted", (candidate) => {
      delete candidate.fanoutLivenessProfile.subscriberBeaconSubscription;
    }],
    ["fanout public liveness option", (candidate) => {
      candidate.fanoutLivenessProfile.publicOption = "allowed";
    }],
    ["duplicate command ID", (candidate) => {
      candidate.commands.push(clone(candidate.commands[0]));
    }],
    ["duplicate TLV field ID", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "descriptor-extension");
      extension.fields.push(clone(extension.fields[0]));
    }],
    ["integer range exceeds encoding", (candidate) => {
      const u8 = candidate.types.find((type) => type.name === "u8");
      u8.maximum = 256;
    }],
    ["length maximum exceeds prefix capacity", (candidate) => {
      candidate.bounds.find((bound) => bound.name === "packetNameBytes").value = 256;
    }],
    ["vector maximum exceeds count capacity", (candidate) => {
      candidate.bounds.find((bound) => bound.name === "journalRecordCount").value = "4294967296";
    }],
    ["sorted vector references missing field", (candidate) => {
      const vector = candidate.types.find((type) => type.name === "stateful-capability-vector");
      vector.constraints[0].fields[1] = "missingField";
    }],
    ["invalid enum constant", (candidate) => {
      const tail = candidate.types.find((type) => type.name === "actor-join-reply-tail");
      tail.cases[1].when.joinResult = "unknownJoinResult";
    }],
    ["field range exceeds referenced type", (candidate) => {
      const entry = candidate.types.find((type) => type.name === "channel-entry");
      entry.fields[1].maximum = 4294967296;
    }],
    ["conditional discriminator references missing enclosing field", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "instanceSpot");
      command.body.find((field) => field.name === "operationKind").name = "renamedOperationKind";
    }],
    ["TLV declaration order", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "descriptor-extension");
      [extension.fields[0], extension.fields[1]] = [extension.fields[1], extension.fields[0]];
    }],
    ["allowed payload without exact type", (candidate) => {
      delete candidate.commands.find((entry) => entry.name === "nodeSend").payloadType;
    }],
    ["relocation generation omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "relocationAck");
      command.body.splice(1, 1);
    }],
    ["opaque frozen kind data restored", (candidate) => {
      const frozen = candidate.types.find((type) => type.name === "frozen-record");
      frozen.fields.push({ name: "kindData", $ref: "blob16" });
    }],
    ["scalar accepted boundary restored", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "relocation-extension");
      extension.fields.push({
        id: 10, name: "acceptedBoundary", $ref: "u64", required: true,
      });
    }],
    ["obsolete Instance redirect restored", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "instanceSpot");
      command.body.push({ name: "redirected", $ref: "bool8" });
    }],
    ["Instance wire timeout restored", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "instanceSpot");
      command.body.splice(-1, 0, { name: "timeoutMs", $ref: "u32" });
    }],
    ["User Spot create reservation omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "userSpotCreate");
      command.body = command.body.filter((field) => field.name !== "reservation");
    }],
    ["User Spot create source lifecycle omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "userSpotCreate");
      command.body = command.body.filter((field) => field.name !== "sourceNodeGeneration");
    }],
    ["User Spot create deadline omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "userSpotCreate");
      command.body = command.body.filter((field) => field.name !== "deadlineUnixMs");
    }],
    ["User Spot close StoreVersion omitted", (candidate) => {
      const fence = candidate.types.find((type) => type.name === "user-spot-close-fence-v1");
      fence.body = fence.body.filter((field) => field.name !== "expectedStoreVersion");
    }],
    ["User Spot close authority generation omitted", (candidate) => {
      const fence = candidate.types.find((type) => type.name === "user-spot-close-fence-v1");
      fence.body = fence.body.filter(
        (field) => field.name !== "expectedAuthorityOwnerGeneration",
      );
    }],
    ["User Spot close target lifecycle omitted", (candidate) => {
      const fence = candidate.types.find((type) => type.name === "user-spot-close-fence-v1");
      fence.body = fence.body.filter((field) => field.name !== "targetNodeGeneration");
    }],
    ["User Spot operation discriminator changed", (candidate) => {
      const operationKinds = candidate.types.find((type) => type.name === "mesh-operation-kind");
      operationKinds.values.find((entry) => entry.name === "userSpotCreate").value = 14;
    }],
    ["User Spot create reply tail loses exact SpotRef", (candidate) => {
      const tail = candidate.types.find((type) => type.name === "request-specific-tail");
      const create = tail.cases.find(
        (entry) => entry.when?.originalOperationKind === "userSpotCreate",
      );
      create.fields = create.fields.filter((field) => field.name !== "spot");
    }],
    ["User Spot close reply tail duplicated", (candidate) => {
      const tail = candidate.types.find((type) => type.name === "request-specific-tail");
      tail.cases.push(clone(tail.cases.find(
        (entry) => entry.when?.originalOperationKind === "userSpotClose",
      )));
    }],
    ["User Spot create accepts command payload", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "userSpotCreate");
      command.payload = "optional";
      command.payloadType = { $ref: "application-payload-envelope-v1" };
    }],
    ["Actor create reservation omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "actorCreate");
      command.body = command.body.filter((field) => field.name !== "reservation");
    }],
    ["Actor create source lifecycle omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "actorCreate");
      command.body = command.body.filter((field) => field.name !== "sourceNodeGeneration");
    }],
    ["Actor create accepts command payload", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "actorCreate");
      command.payload = "optional";
      command.payloadType = { $ref: "application-payload-envelope-v1" };
    }],
    ["Actor create result discriminator changed", (candidate) => {
      const result = candidate.types.find((type) => type.name === "actor-create-result");
      result.values.find((entry) => entry.name === "rejected").value = 4;
    }],
    ["Actor rejected terminal exposes ActorRef", (candidate) => {
      const terminal = candidate.types.find((type) => type.name === "actor-create-terminal");
      terminal.cases.find(
        (entry) => entry.when?.createResult === "rejected",
      ).fields.push({ name: "actor", $ref: "actor-ref" });
    }],
    ["Actor creation terminal stores correlation", (candidate) => {
      const terminal = candidate.types.find(
        (type) => type.name === "creation-operation-terminal-v1",
      );
      terminal.correlationFields = "included";
    }],
    ["Actor creation terminal loses size bound", (candidate) => {
      const terminal = candidate.types.find(
        (type) => type.name === "creation-operation-terminal-v1",
      );
      delete terminal.maximumEncodedBytes;
    }],
    ["Actor creation terminal allows Existing payload", (candidate) => {
      const terminal = candidate.types.find(
        (type) => type.name === "creation-operation-terminal-v1",
      );
      terminal.constraints = terminal.constraints.filter(
        (entry) => entry.kind !== "existing-has-no-application-payload",
      );
    }],
    ["Actor create reply tail omitted", (candidate) => {
      const tail = candidate.types.find((type) => type.name === "request-specific-tail");
      tail.cases = tail.cases.filter(
        (entry) => entry.when?.originalOperationKind !== "actorCreate",
      );
    }],
    ["legacy reply envelope field changed", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "reply");
      command.body[0].name = "operation";
    }],
    ["Instance owner lease fence omitted", (candidate) => {
      const fence = candidate.types.find((type) => type.name === "authority-generation-fence");
      fence.fields = fence.fields.filter((field) => field.name !== "leaseGeneration");
    }],
    ["Actor authority fence omitted", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "actorSend");
      command.body.find((field) => field.name === "targetActor").$ref = "actor-ref";
    }],
    ["durable checksum coverage", (candidate) => {
      candidate.durableFormats[0].checksum.coverage = "body-only";
    }],
    ["durable provider interpretation", (candidate) => {
      candidate.durableFormats[0].providerInterpretation = "parsed-fields";
    }],
    ["relocation state rule phase", (candidate) => {
      candidate.relocationStateMachine.commandRules[0].phases[0] = "unknown";
    }],
    ["application packet name omitted", (candidate) => {
      const envelope = candidate.types.find((type) => type.name === "application-payload-envelope-v1");
      envelope.body.shift();
    }],
    ["application codec identity restored", (candidate) => {
      const envelope = candidate.types.find((type) => type.name === "application-payload-envelope-v1");
      envelope.body.splice(2, 0, { name: "codecIdentity", $ref: "serializer-identity" });
    }],
    ["Instance message contract restored", (candidate) => {
      const route = candidate.types.find((type) => type.name === "instance-route-v1");
      route.cases.find((entry) => entry.when.routeKind === "ready")
        .fields.push({ name: "messageContractId", $ref: "text8" });
    }],
    ["Instance routeKind accepts an unknown value", (candidate) => {
      const routeKind = candidate.types.find((type) => type.name === "instance-route-kind");
      routeKind.values.push({ name: "unknown", value: 3 });
    }],
    ["Ready Instance route carries cold-activation fields", (candidate) => {
      const route = candidate.types.find((type) => type.name === "instance-route-v1");
      route.cases.find((entry) => entry.when.routeKind === "ready")
        .fields.push({ name: "stableType", $ref: "text8" });
    }],
    ["Cold Instance route carries an authority fence", (candidate) => {
      const route = candidate.types.find((type) => type.name === "instance-route-v1");
      route.cases.find((entry) => entry.when.routeKind === "coldActivation")
        .fields.push({ name: "authority", $ref: "authority-generation-fence" });
    }],
    ["participant progress ordering removed", (candidate) => {
      const progress = candidate.types.find((type) => type.name === "participant-progress-vector");
      progress.constraints = [];
    }],
    ["participant progress aggregate bound reduced", (candidate) => {
      const progress = candidate.types.find((type) => type.name === "participant-progress-vector");
      progress.maximumItems = 2;
    }],
    ["relocation application state aggregate bound removed", (candidate) => {
      const states = candidate.types.find(
        (type) => type.name === "relocation-participant-application-state-vector",
      );
      delete states.maximumItems;
    }],
    ["terminal completion bound diverges", (candidate) => {
      const completions = candidate.types.find((type) => type.name === "request-completion-vector");
      completions.maximumItems = 1;
    }],
    ["terminal completion request source removed", (candidate) => {
      const completion = candidate.types.find((type) => type.name === "request-completion-entry");
      completion.fields = completion.fields.filter((field) => field.name !== "requestSource");
    }],
    ["terminal completion identity weakened", (candidate) => {
      const completions = candidate.types.find((type) => type.name === "request-completion-vector");
      completions.constraints = [{
        kind: "sorted",
        fields: ["operationId.high", "operationId.low"],
        comparison: "unsigned-wire-value",
      }];
    }],
    ["unknown struct constraint", (candidate) => {
      const operation = candidate.types.find((type) => type.name === "operation-id");
      operation.constraints.push({ kind: "accept-anything" });
    }],
    ["relocation state graph skip", (candidate) => {
      candidate.relocationStateMachine.transitions.push({ from: "preparing", to: "completed" });
    }],
    ["arbitrary duplicate policy", (candidate) => {
      candidate.relocationStateMachine.commandRules[1].duplicate = "accept-all";
    }],
    ["protocol capability constraint removed", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "descriptor-extension");
      delete extension.fields.find((field) => field.name === "protocolCapabilities").constraints;
    }],
    ["authority state integrity weakened", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "authority-operation-state-integrity",
      );
      constraint.rules.find((rule) => rule.operationKind === "coldActivation")
        .authorityStates.push("closing");
    }],
    ["Actor authority activation recovery allowed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "authority-operation-state-integrity",
      );
      constraint.rules.find(
        (rule) => rule.operationKind === "steady" && rule.objectKind === "actor",
      ).activationRecoveryState = "optional";
    }],
    ["relocating authority activation recovery allowed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "authority-operation-state-integrity",
      );
      constraint.rules.find((rule) => rule.operationKind === "maintenanceRelocation")
        .activationRecoveryState = "optional";
    }],
    ["activation recovery release on admission", (candidate) => {
      const recovery = candidate.types.find(
        (type) => type.name === "authority-activation-recovery-state",
      );
      recovery.release = "after-admission";
    }],
    ["activation recovery presence widened", (candidate) => {
      const recovery = candidate.types.find(
        (type) => type.name === "authority-activation-recovery-state",
      );
      recovery.presence = "all-ready-authorities";
    }],
    ["ZLIA metadata frame omitted", (candidate) => {
      const recovery = candidate.types.find(
        (type) => type.name === "instance-activation-recovery-v1",
      );
      recovery.fields = recovery.fields.filter((field) => field.name !== "metadata");
    }],
    ["Cold Instance route and ZLIA placement intent diverge", (candidate) => {
      const recovery = candidate.types.find(
        (type) => type.name === "instance-activation-recovery-v1",
      );
      recovery.fields.find((field) => field.name === "targetDescriptorVersion").name =
        "recoveryDescriptorVersion";
    }],
    ["creation intent bound widened", (candidate) => {
      const bound = candidate.bounds.find((entry) => entry.name === "creationIntentBytes");
      bound.value += 1;
    }],
    ["maintenance aggregate bound widened", (candidate) => {
      const bound = candidate.bounds.find((entry) => entry.name === "maintenanceAggregateBytes");
      bound.value += 1;
    }],
    ["Message Follow hop bound widened", (candidate) => {
      const bound = candidate.bounds.find((entry) => entry.name === "messageFollowHopCount");
      bound.value += 1;
    }],
    ["node pending capacity default changed", (candidate) => {
      const bound = candidate.bounds.find((entry) => entry.name === "nodePendingCapacityDefault");
      bound.value += 1;
    }],
    ["creation intent loses content hash", (candidate) => {
      const intent = candidate.types.find((type) => type.name === "object-creation-intent-v1");
      intent.body = intent.body.filter((field) => field.name !== "requestSha256");
    }],
    ["creation intent reuses relocation reference", (candidate) => {
      const intent = candidate.types.find((type) => type.name === "object-creation-intent-v1");
      intent.body.find((field) => field.name === "requestContentReference").$ref =
        "relocation-reference";
    }],
    ["cold Instance operation loses descriptor version", (candidate) => {
      const route = candidate.types.find((entry) => entry.name === "instance-route-v1");
      const cold = route.cases.find((entry) => entry.when.routeKind === "coldActivation");
      cold.fields = cold.fields.filter((field) => field.name !== "targetDescriptorVersion");
    }],
    ["generic reservation loses capacity fence", (candidate) => {
      const fence = candidate.types.find((type) => type.name === "object-reservation-fence");
      fence.fields.pop();
    }],
    ["User Spot aggregate inventory removed", (candidate) => {
      const identity = candidate.types.find((type) => type.name === "relocation-object-identity");
      identity.cases = identity.cases.filter((entry) => entry.when.objectKind !== "userSpot");
    }],
    ["descriptor object role removed", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "descriptor-extension");
      extension.fields = extension.fields.filter((field) => field.name !== "objectRole");
    }],
    ["Spot exact route loses node generation", (candidate) => {
      const route = candidate.types.find((type) => type.name === "spot-route-fence");
      route.fields = route.fields.filter((field) => field.name !== "targetNodeGeneration");
    }],
    ["global authority loses owner Mesh", (candidate) => {
      const authority = candidate.types.find((type) => type.name === "authority-payload-v1");
      authority.fields = authority.fields.filter((field) => field.name !== "ownerMeshName");
    }],
    ["maintenance Instance uses cold activation state", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "authority-operation-state-integrity",
      );
      constraint.rules.find((rule) => rule.operationKind === "maintenanceRelocation")
        .instanceAuthorityStates[0] = "coldActivating";
    }],
    ["steady authority operation omitted", (candidate) => {
      const operation = candidate.types.find((type) => type.name === "authority-operation-kind");
      operation.values.shift();
    }],
    ["relocation cross-vector rule weakened", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "relocation-journal-integrity",
      );
      delete constraint.sequenceAtOrBelowAcceptedBoundary;
    }],
    ["reply relay state pair changed", (candidate) => {
      const relayRule = candidate.relocationStateMachine.commandRules.find(
        (entry) => entry.command === "replyRelay",
      );
      relayRule.contextRules[0].authorityStateCompletionPairs[0].authorityState = "activating";
    }],
    ["final sequence restored", (candidate) => {
      const relocationControl = candidate.types.find((type) => type.name === "relocation-control-data");
      relocationControl.fields.push({ name: "finalSequence", $ref: "u64" });
    }],
    ["relocation reference bound widened", (candidate) => {
      const reference = candidate.types.find((type) => type.name === "relocation-reference");
      reference.maximumBytes = { $bound: "blobBytes" };
    }],
    ["relocation manifest loses inventory digest", (candidate) => {
      const manifest = candidate.types.find((type) => type.name === "relocation-manifest-v1");
      manifest.fields = manifest.fields.filter((field) => field.name !== "inventoryDigestSha256");
    }],
    ["Location aggregate permits Relocation manifest authority", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.aggregateAuthority.manifestAuthority = "allowed";
    }],
    ["Location aggregate weakens inventory digest match", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.aggregateAuthority.inventoryDigestMatch = "optional";
    }],
    ["published payload missing terminal trigger omitted", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.publishedRelocationDataLoss.closedTerminalTriggers =
        constraint.publishedRelocationDataLoss.closedTerminalTriggers.filter(
          (trigger) => trigger !== "permanent-published-payload-missing",
        );
    }],
    ["published payload checksum mismatch terminal trigger omitted", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.publishedRelocationDataLoss.closedTerminalTriggers =
        constraint.publishedRelocationDataLoss.closedTerminalTriggers.filter(
          (trigger) => trigger !== "published-payload-checksum-mismatch",
        );
    }],
    ["published payload inventory digest mismatch terminal trigger omitted", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.publishedRelocationDataLoss.closedTerminalTriggers =
        constraint.publishedRelocationDataLoss.closedTerminalTriggers.filter(
          (trigger) => trigger !== "published-payload-inventory-digest-mismatch",
        );
    }],
    ["published Relocation loss becomes retriable", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "location-relocation-storage-integrity",
      );
      constraint.publishedRelocationDataLoss.retriable = true;
    }],
    ["RelocationDataLost failure omitted", (candidate) => {
      const errorCodes = candidate.types.find((type) => type.name === "framework-error-code");
      errorCodes.values = errorCodes.values.filter(
        (entry) => entry.name !== "relocationDataLost",
      );
    }],
    ["RelocationDataLost wire value changed", (candidate) => {
      const errorCodes = candidate.types.find((type) => type.name === "framework-error-code");
      errorCodes.values.find((entry) => entry.name === "relocationDataLost").value = 23;
    }],
    ["SpotGenerationStale failure omitted", (candidate) => {
      const errorCodes = candidate.types.find((type) => type.name === "framework-error-code");
      errorCodes.values = errorCodes.values.filter(
        (entry) => entry.name !== "spotGenerationStale",
      );
    }],
    ["SpotMoving failure omitted", (candidate) => {
      const errorCodes = candidate.types.find((type) => type.name === "framework-error-code");
      errorCodes.values = errorCodes.values.filter(
        (entry) => entry.name !== "spotMoving",
      );
    }],
    ["SpotGenerationStale wire value changed", (candidate) => {
      const errorCodes = candidate.types.find((type) => type.name === "framework-error-code");
      errorCodes.values.find((entry) => entry.name === "spotGenerationStale").value = 34;
    }],
    ["framework error reserved wire gap narrowed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "terminal-failure-integrity",
      );
      constraint.reservedWireValues.last = 31;
    }],
    ["SpotMoving mapping changed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "terminal-failure-integrity",
      );
      constraint.typedFrameworkFailure.exactResultByFailureCode.spotMoving = "internalError";
    }],
    ["RelocationDataLost mapping changed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "terminal-failure-integrity",
      );
      constraint.typedFrameworkFailure.exactResultByFailureCode.relocationDataLost = "unavailable";
    }],
    ["authority aggregate bound omitted", (candidate) => {
      const relocation = candidate.types.find((type) => type.name === "authority-relocation-state");
      delete relocation.maximumEncodedBytes;
    }],
    ["Location aggregate loses Relocation root", (candidate) => {
      const aggregate = candidate.types.find((type) => type.name === "maintenance-aggregate-v1");
      aggregate.body = aggregate.body.filter((field) => field.name !== "relocationRoot");
    }],
    ["relocation TLV narrowed to u16", (candidate) => {
      const extension = candidate.types.find((type) => type.name === "relocation-extension");
      extension.totalLengthType.$ref = "u16";
      extension.fieldLengthType.$ref = "u16";
    }],
    ["relocation Snapshot adds application contract metadata", (candidate) => {
      const state = candidate.types.find((type) => type.name === "relocation-application-state");
      state.cases.find((entry) => entry.when.hasState === "true").fields.unshift(
        { name: "stateContractId", $ref: "text8" },
      );
    }],
    ["relocation Snapshot rejects empty bytes", (candidate) => {
      const state = candidate.types.find((type) => type.name === "durable-state-blob");
      state.minimumBytes = 1;
    }],
    ["relocation participant state widens beyond 64 MiB", (candidate) => {
      const state = candidate.types.find((type) => type.name === "durable-state-blob");
      state.maximumBytes = { $bound: "relocationLogicalBytes" };
    }],
    ["relocation aggregate collapses participant states", (candidate) => {
      const envelope = candidate.types.find((type) => type.name === "relocation-envelope-v1");
      const field = envelope.fields.find((entry) => entry.name === "applicationStates");
      field.name = "applicationState";
      field.$ref = "relocation-application-state";
    }],
    ["relocation timer registration omitted", (candidate) => {
      const envelope = candidate.types.find((type) => type.name === "relocation-envelope-v1");
      envelope.fields = envelope.fields.filter((field) => field.name !== "timerRegistrations");
    }],
    ["relocation timer leaks native handle", (candidate) => {
      const registration = candidate.types.find(
        (type) => type.name === "relocation-timer-registration",
      );
      registration.fields.push({ name: "nativeTimerHandle", $ref: "u64" });
    }],
    ["maintenance drains pending timer ticks on source", (candidate) => {
      candidate.maintenanceAdmissionProfile.acceptedTimerTurns =
        "drain-on-source-before-relocation";
    }],
    ["maintenance Entry callback uses ordinary join", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "actor-retire-membership-integrity",
      );
      constraint.targetCallback = "target-entry-onJoinedActor";
    }],
    ["maintenance Entry replay precedes durable source cleanup", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "actor-retire-membership-integrity",
      );
      constraint.sourceCleanup = "source-entry-onLeaveActor-before-completed";
    }],
    ["User Spot aggregate invokes Actor relocation callback", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "actor-retire-membership-integrity",
      );
      constraint.userSpotAggregateCallbacks = "onActorRelocated-allowed";
    }],
    ["pending timer tick leaves shared sequence domain", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "participant-sequence-domain",
      );
      constraint.sequenceFields = constraint.sequenceFields.filter(
        (field) => field !== "relocation-pending-timer-tick.sequence",
      );
    }],
    ["maintenance permit defaults narrowed", (candidate) => {
      candidate.maintenanceAdmissionProfile.defaultPermits.activeOutbound = 4;
    }],
    ["maintenance permit configuration accepts zero", (candidate) => {
      candidate.maintenanceAdmissionProfile.permitConfiguration =
        "application-options-may-be-zero";
    }],
    ["maintenance permit acquired after source seal", (candidate) => {
      candidate.maintenanceAdmissionProfile.permitAcquisition =
        "source-seal-before-blocking-permit-acquisition";
    }],
    ["maintenance byte permit can grow after Capture", (candidate) => {
      candidate.maintenanceAdmissionProfile.byteReconciliation =
        "after-capture-grow-or-shrink-to-actual-encoded-size";
    }],
    ["maintenance oversized aggregate overlaps normal unit", (candidate) => {
      candidate.maintenanceAdmissionProfile.oversizedUnit =
        "may-overlap-normal-unit";
    }],
    ["unknown semantic constraint", (candidate) => {
      candidate.semanticConstraints.push({ kind: "accept-all" });
    }],
    ["relocation authority commit order changed", (candidate) => {
      candidate.relocationStateMachine.authorityCommitOrder[1].phase = "prepared";
    }],
    ["Prepared CAS precedes target restore", (candidate) => {
      candidate.relocationStateMachine.authorityCommitOrder[2].after =
        ["relocation-reserved-ack-validated"];
    }],
    ["participant sequence accepts zero", (candidate) => {
      const command = candidate.commands.find((entry) => entry.name === "relocationData");
      command.body.find((field) => field.name === "sequence").$ref = "u64";
    }],
    ["authority key escaping changed", (candidate) => {
      candidate.authorityKeyFormat.escaping = "lowercase-percent-hex";
    }],
    ["relocation renew result loses store clock", (candidate) => {
      candidate.relocationRetentionPolicy.renewResult = "renewed-or-missing";
    }],
    ["maintenance reserves before seal", (candidate) => {
      candidate.maintenanceAdmissionProfile.phaseOrder = [
        "target-reserve", "preparing-cas", "relocation-put", "captured-cas", "prepared-cas",
      ];
    }],
    ["maintenance claims durable replay before Captured", (candidate) => {
      candidate.maintenanceAdmissionProfile.durableReplayBoundary = "preparing-cas";
    }],
    ["termination Blocked deadline omitted", (candidate) => {
      const blocked = candidate.terminationResultProfile.allowedPairs.find(
        (entry) => entry.outcome === "Blocked",
      );
      blocked.reasons = blocked.reasons.filter((reason) => reason !== "DeadlineExceeded");
    }],
    ["termination forbidden Blocked teardown pair", (candidate) => {
      const blocked = candidate.terminationResultProfile.allowedPairs.find(
        (entry) => entry.outcome === "Blocked",
      );
      blocked.reasons.push("TeardownFailed");
    }],
    ["termination relocation reason widened", (candidate) => {
      candidate.terminationResultProfile.reasons.push({ name: "RelocationTooLarge", wireValue: 9 });
    }],
    ["termination relocation ceiling pair changed", (candidate) => {
      candidate.terminationResultProfile.relocationCeilingBeforeDraining =
        "Blocked/RelocationTooLarge";
    }],
    ["termination teardown outcome changed", (candidate) => {
      candidate.terminationResultProfile.teardownFailure = "Failed/TeardownFailed";
    }],
    ["reply relay acknowledgement omitted", (candidate) => {
      candidate.commands = candidate.commands.filter((command) => command.name !== "replyRelayAck");
    }],
    ["reply relay acknowledgement trusts relocation source", (candidate) => {
      const rule = candidate.relocationStateMachine.commandRules.find(
        (entry) => entry.command === "replyRelayAck",
      );
      delete rule.senderKind;
      rule.senderRoles = ["source"];
    }],
    ["reply relay acknowledgement drops request source fence identity", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "reply-relay-context-integrity",
      );
      constraint.acknowledgement.identity =
        ["stable-relocation-id", "operation-id", "reply-route-id"];
      constraint.terminalOwnership =
        "stable-relocation-id-operation-id-and-reply-route-id-once-independent-of-target-attempt";
    }],
    ["reply relay acknowledgement drops reply route identity", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "reply-relay-context-integrity",
      );
      constraint.acknowledgement.identity =
        ["stable-relocation-id", "exact-request-source-fence", "operation-id"];
      constraint.terminalOwnership =
        "stable-relocation-id-exact-request-source-fence-and-operation-id-once-independent-of-target-attempt";
    }],
    ["frozen request reply route omitted", (candidate) => {
      const frozen = candidate.types.find((type) => type.name === "frozen-record");
      frozen.fields = frozen.fields.filter((field) => field.name !== "replyRoute");
    }],
    ["frozen send reply route admitted", (candidate) => {
      const route = candidate.types.find((type) => type.name === "frozen-reply-route");
      route.cases.push({
        when: { originalOperationKind: "none" },
        fields: [{ name: "replyRouteId", $ref: "nonzero-u64" }],
      });
    }],
    ["physical connection closure restored as terminal proof", (candidate) => {
      const delivery = candidate.types.find((type) => type.name === "completion-delivery-state");
      delivery.values[3].name = "sourceConnectionClosed";
    }],
    ["authority relocation relay count atomicity removed", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "relocation-journal-integrity",
      );
      delete constraint.authorityRelocationAtomicity.pendingRelayCount;
    }],
    ["authority phase owner shape weakened", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "relocation-authority-phase-boundaries",
      );
      constraint.closedOwnerTargetRules.preparingAndCaptured = "target-may-be-present";
    }],
    ["abort route allowed before durable abort", (candidate) => {
      const rule = candidate.relocationStateMachine.commandRules.find(
        (entry) => entry.command === "sessionRelocationRoute",
      );
      rule.actionRules.abort.phases = ["prepared"];
    }],
    ["liveness permits two outstanding probes", (candidate) => {
      candidate.livenessProfile.outstandingProbeMaximum = 2;
    }],
    ["descriptor page cap widened", (candidate) => {
      candidate.descriptorEnumerationProfile.pageEncodedBytesMaximum = 8388608;
    }],
    ["authority scan cursor made parseable", (candidate) => {
      candidate.authorityStoreAccessProfile.continuationToken.frameworkInterpretationOrComposition = "allowed";
    }],
    ["descriptor overflow truncates", (candidate) => {
      candidate.descriptorEnumerationProfile.overflow = "truncate";
    }],
    ["descriptor revision wraps", (candidate) => {
      candidate.descriptorEnumerationProfile.descriptorRevisionExhaustion = "wrap-to-one";
    }],
    ["framework JSON permits unpadded base64", (candidate) => {
      candidate.frameworkJsonV1Profile.bytes = "unpadded-base64";
    }],
    ["framework JSON profile claims relocation restore", (candidate) => {
      candidate.frameworkJsonV1Profile.duplicateProperties =
        "reject-before-typed-dispatch-or-restore";
    }],
    ["service update restores state contract capability", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "service-admission-update-integrity",
      );
      constraint.immutableExact.push("stateContractCapabilities");
    }],
    ["service update mutates immutable message bound", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "service-admission-update-integrity",
      );
      constraint.mutableOnly.push("normalizedEffectiveMaxMessageBytes");
    }],
    ["staged relocation can link without renew", (candidate) => {
      candidate.relocationRetentionPolicy.preLinkGate = "skip";
    }],
    ["target replacement claims exactly once callbacks", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "relocation-replacement-round-integrity",
      );
      constraint.targetActivationRetry = "exactly-once";
    }],
    ["authority generation exhaustion mutates state", (candidate) => {
      candidate.authorityStoreGenerationProfile.overflow = "GenerationExhausted-after-row-mutation";
    }],
    ["owner lease generation exhaustion omitted", (candidate) => {
      candidate.ownerLeaseAuthorityProfile.operations.claimOwnerLease.result =
        "claimed-token-expiresAt-storeNow-or-conflict";
    }],
    ["owner lease timing relation weakened", (candidate) => {
      const constraint = candidate.semanticConstraints.find(
        (entry) => entry.kind === "owner-lease-timing-integrity",
      );
      constraint.fencingMarginMs = 1000;
    }],
  ];
  for (const [label, mutate] of tests) {
    expectInvalid(schema, label, mutate);
  }
  return tests.length;
}

function runGoldenFixtureSelfTests(schema, schemaPath) {
  let count = 0;
  const replaceEnvelopeByte = (candidate, offset, value) => {
    const bytes = Buffer.from(candidate.encodedHex, "hex");
    bytes[offset] = value;
    candidate.encodedHex = bytes.toString("hex");
  };
  const reencode = (format, candidate) => {
    candidate.encodedHex = encodeGoldenEnvelope(format, candidate.decoded).toString("hex");
  };
  for (const format of schema.durableFormats) {
    const fixturePath = path.resolve(path.dirname(schemaPath), format.goldenFixture);
    const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
    const tests = [
      ["magic", (candidate) => replaceEnvelopeByte(candidate, 0, 0)],
      ["version", (candidate) => replaceEnvelopeByte(candidate, 4, 2)],
      ["body length", (candidate) => replaceEnvelopeByte(candidate, 9, 0xff)],
      ["checksum", (candidate) => replaceEnvelopeByte(candidate, candidate.encodedHex.length / 2 - 1, 0)],
      ["consumer order", (candidate) => {
        [candidate.consumers[0], candidate.consumers[1]] = [
          candidate.consumers[1], candidate.consumers[0],
        ];
      }],
    ];
    if (format.name === "authority-payload-v1") {
      tests.push(
        ["activation pointer on non-steady operation", (candidate) => {
          candidate.decoded.operationKind = "coldActivation";
          reencode(format, candidate);
        }],
        ["activation pointer missing from Ready golden", (candidate) => {
          candidate.decoded.activationRecoveryState = null;
          reencode(format, candidate);
        }],
        ["activation replay cursor beyond inbox sequence", (candidate) => {
          candidate.decoded.activationRecoveryState.replayCursor = "2";
          candidate.decoded.activationRecoveryState.inboxSequence = "1";
          reencode(format, candidate);
        }],
      );
    } else if (format.name === "instance-activation-recovery-v1") {
      tests.push(
        ["metadata presence removed", (candidate) => {
          candidate.decoded.metadata = null;
          reencode(format, candidate);
        }],
        ["metadata version changed", (candidate) => {
          candidate.decoded.metadata.version = 2;
          reencode(format, candidate);
        }],
        ["metadata entries removed", (candidate) => {
          candidate.decoded.metadata.entries = [];
          reencode(format, candidate);
        }],
        ["metadata key duplicated", (candidate) => {
          candidate.decoded.metadata.entries.push({ ...candidate.decoded.metadata.entries[0] });
          reencode(format, candidate);
        }],
      );
    } else if (format.name === "relocation-data-chunk-v1") {
      tests.push(["empty chunk", (candidate) => {
        candidate.decoded.dataHex = "";
        reencode(format, candidate);
      }]);
    } else if (format.name === "relocation-manifest-v1") {
      tests.push(
        ["semantic relation", (candidate) => {
          candidate.decoded.totalLength = "391";
          reencode(format, candidate);
        }],
        ["canonical order", (candidate) => {
          candidate.decoded.chunks.push({ ...candidate.decoded.chunks[0] });
          reencode(format, candidate);
        }],
      );
    }
    for (const [label, mutate] of tests) {
      const candidate = clone(fixture);
      mutate(candidate);
      const errors = [];
      validateGoldenFixtureData(format, candidate, `fixture-self-test:${format.name}:${label}`,
        (location, message) => errors.push(`${location}: ${message}`));
      if (errors.length === 0) {
        throw new Error(`negative self-test did not fail: ${format.name}: ${label}`);
      }
      count += 1;
    }
  }
  return count;
}

function runAuthorityKeyFixtureSelfTests(schema, schemaPath) {
  const fixturePath = path.resolve(
    path.dirname(schemaPath),
    schema.authorityKeyFormat.goldenFixture,
  );
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const tests = [
    ["lowercase percent escape", (candidate) => {
      candidate.cases[0].encoded = candidate.cases[0].encoded.replace("%3A", "%3a");
    }],
    ["declared byte length", (candidate) => {
      candidate.cases[0].encoded = candidate.cases[0].encoded.replace(":7:", ":8:");
    }],
    ["unknown kind", (candidate) => {
      candidate.cases[0].encoded = candidate.cases[0].encoded.replace("zla1:a:", "zla1:x:");
    }],
    ["literal separator in component", (candidate) => {
      candidate.cases[0].encoded = candidate.cases[0].encoded.replace("%3A", ":");
    }],
    ["consumer order", (candidate) => {
      [candidate.consumers[0], candidate.consumers[1]] = [
        candidate.consumers[1], candidate.consumers[0],
      ];
    }],
    ["legacy Mesh component", (candidate) => {
      candidate.cases[0].encoded = "zla1:a:4:main:7:user%3A42";
    }],
    ["empty global identity", (candidate) => {
      candidate.cases[0].identityHex = "";
      candidate.cases[0].encoded = "zla1:a:0:";
    }],
    ["global identity exceeds 255 bytes", (candidate) => {
      candidate.cases[0].identityHex = "61".repeat(256);
      candidate.cases[0].encoded = `zla1:a:256:${"a".repeat(256)}`;
    }],
    ["invalid UTF-8 Spot ID", (candidate) => {
      const spot = candidate.cases.find((entry) => entry.objectKind === "spot");
      spot.identityHex = "ff";
      spot.encoded = "zla1:s:1:%FF";
    }],
    ["Actor and Spot domain collision", (candidate) => {
      const actor = candidate.cases.find((entry) => entry.objectKind === "actor");
      const spot = candidate.cases.find((entry) => entry.objectKind === "spot");
      spot.identityHex = actor.identityHex;
      spot.encoded = actor.encoded;
    }],
  ];
  for (const [label, mutate] of tests) {
    const candidate = clone(fixture);
    mutate(candidate);
    const errors = [];
    validateAuthorityKeyFixtureData(candidate, `authority-key-self-test:${label}`,
      (location, message) => errors.push(`${location}: ${message}`));
    if (errors.length === 0) {
      throw new Error(`negative self-test did not fail: authority-key-v1: ${label}`);
    }
  }
  return tests.length;
}

function validateContractAmendmentFixtureData(fixture, location, fail) {
  const expected = {
    format: "contract-amendment-v1",
    consumers: ["cpp", "dotnet", "jvm", "node"],
    descriptor: {
      objectRole: "server", placementWeight: 100, activeCapacityLimit: 10000,
      pendingCapacityLimit: 128, objectKind: "userSpot", stableType: "chat-room",
      typeCapacityLimit: 4096,
    },
    creationIntent: {
      objectKind: "instanceSpot", globalIdHex: "73706f742d31", stableType: "presence",
      initialMeshName: "main",
      requestContentReference: "create/1",
      requestSha256Hex: "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
      requestEncodedSize: 1048576,
    },
    reservation: {
      operationOrder: ["reserve", "commit", "abort"], pendingCapacityDelta: 1,
      objectGeneration: "2", authorityOwnerGeneration: "3",
      targetNodeRidUtf8Fixture: "node-a", targetNodeGeneration: "4",
      targetOwnerLeaseGeneration: "5",
      pendingCreation: {
        reservationId: "reservation-1",
        requestContentReference: "create/1",
        requestSha256Hex: "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        requestEncodedSize: 1048576,
      },
    },
    aggregate: {
      aggregateHigh: "0", aggregateLow: "1", aggregateGeneration: "6",
      participantKinds: ["userSpot", "actor"],
      authorityOwner: "location",
      canonicalParticipantOrder: ["spot:room-1", "actor:actor-1"],
      participantMutations: ["spot-owner-and-membership", "actor-owner-and-membership"],
      inventoryDigestSha256Hex: "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
      relocationManifestDigestSha256Hex: "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
      relocationReferenceUtf8Fixture: "relocation/aggregate-1",
      relocationChecksumCrc32c: 305419896,
      relocationManifestAuthority: false,
      participantMaximum: 1024,
      encodedMaximum: 1048576,
    },
    messageFollow: {
      objectKinds: ["actor", "spot"], hopMaximum: 8, queuedMessageMaximum: 1024,
      queuedByteMaximum: 16777216,
      preserves: ["operationId", "objectGeneration", "payload", "replyRoute"],
    },
    exactRefRoute: {
      actor: ["actorId", "objectGeneration", "targetNodeRid", "targetNodeGeneration", "authorityOwnerGeneration"],
      spot: ["spotId", "objectGeneration", "targetNodeRid", "targetNodeGeneration", "authorityOwnerGeneration"],
    },
    normalInstanceMessageFields: [
      "spotId", "objectGeneration", "targetNodeRid", "targetNodeGeneration",
      "authorityOwnerGeneration",
    ],
    terminalServiceOperations: {
      commands: {
        userSpotCreate: 47,
        userSpotClose: 48,
        actorCreate: 49,
        terminalReply: 20,
        messageFollow: 50,
        reservedFirst: 51,
      },
      replyEnvelopeFields: ["correlation", "terminalResult", "failureCode", "tail"],
      createRequestFields: [
        "correlation", "operation", "sourceNodeRid", "sourceNodeGeneration", "spotId",
        "stableType", "reservation", "deadlineUnixMs",
      ],
      createReservationFences: [
        "reservationId", "expectedStoreVersion", "objectGeneration",
        "authorityOwnerGeneration", "targetNodeRid", "targetNodeGeneration", "targetOwnerId",
        "targetOwnerLeaseGeneration", "pendingCapacityDelta",
      ],
      createTerminalStates: ["existing", "created", "rejected"],
      createTerminalTail: ["createResult", "spot"],
      createContentSource: "location-pending-creation-exact-read",
      actorCreateRequestFields: [
        "correlation", "operation", "sourceNodeRid", "sourceNodeGeneration", "actorId",
        "stableType", "reservation", "deadlineUnixMs",
      ],
      actorCreateTerminalStates: ["existing", "created", "rejected"],
      actorCreateTerminalTail: ["creation"],
      actorCreateConcurrency: "distinct-operation-waits-then-ready-existing-or-rejected-cleanup-new-reservation",
      closeRequestFields: [
        "correlation", "operation", "sourceNodeRid", "sourceNodeGeneration", "target",
        "deadlineUnixMs",
      ],
      closeTargetFences: [
        "spot", "targetNodeRid", "targetNodeGeneration",
        "expectedAuthorityOwnerGeneration", "expectedStoreVersion",
      ],
      closeTerminalTail: ["closed"],
      terminalOnceIdentity: ["sourceNodeRid", "sourceNodeGeneration", "operation"],
    },
  };
  if (JSON.stringify(fixture) !== JSON.stringify(expected)) {
    fail(location, "must match the exact CA-D01..D56 amendment fixture");
  }
}

function validateContractAmendmentFixture(schemaPath) {
  const fixturePath = path.resolve(path.dirname(schemaPath), "golden/contract-amendment-v1.json");
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const errors = [];
  validateContractAmendmentFixtureData(fixture, fixturePath,
    (location, message) => errors.push(`${location}: ${message}`));
  if (errors.length > 0) {
    throw new SchemaValidationError(errors);
  }
  return 1;
}

function runContractAmendmentFixtureSelfTests(schemaPath) {
  const fixturePath = path.resolve(path.dirname(schemaPath), "golden/contract-amendment-v1.json");
  const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
  const tests = [
    ["normal message restores stable type", (candidate) => {
      candidate.normalInstanceMessageFields.push("stableType");
    }],
    ["aggregate drops User Spot", (candidate) => {
      candidate.aggregate.participantKinds.shift();
    }],
    ["aggregate and Relocation manifest digest diverge", (candidate) => {
      candidate.aggregate.relocationManifestDigestSha256Hex = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    }],
    ["aggregate drops Relocation reference", (candidate) => {
      delete candidate.aggregate.relocationReferenceUtf8Fixture;
    }],
    ["aggregate drops Relocation checksum", (candidate) => {
      delete candidate.aggregate.relocationChecksumCrc32c;
    }],
    ["reservation changes operation order", (candidate) => {
      candidate.reservation.operationOrder.reverse();
    }],
    ["exact ref drops generation", (candidate) => {
      candidate.exactRefRoute.actor.splice(1, 1);
    }],
    ["User Spot create drops StoreVersion fence", (candidate) => {
      candidate.terminalServiceOperations.createReservationFences =
        candidate.terminalServiceOperations.createReservationFences.filter(
          (field) => field !== "expectedStoreVersion",
        );
    }],
    ["User Spot close drops target lifecycle", (candidate) => {
      candidate.terminalServiceOperations.closeTargetFences =
        candidate.terminalServiceOperations.closeTargetFences.filter(
          (field) => field !== "targetNodeGeneration",
        );
    }],
    ["User Spot reply envelope changes legacy order", (candidate) => {
      candidate.terminalServiceOperations.replyEnvelopeFields.reverse();
    }],
    ["User Spot terminal identity drops source lifecycle", (candidate) => {
      candidate.terminalServiceOperations.terminalOnceIdentity.splice(1, 1);
    }],
  ];
  for (const [label, mutate] of tests) {
    const candidate = clone(fixture);
    mutate(candidate);
    const errors = [];
    validateContractAmendmentFixtureData(candidate, `contract-amendment-self-test:${label}`,
      (location, message) => errors.push(`${location}: ${message}`));
    if (errors.length === 0) {
      throw new Error(`negative self-test did not fail: contract-amendment-v1: ${label}`);
    }
  }
  return tests.length;
}

function printFailure(error) {
  if (!(error instanceof SchemaValidationError)) {
    console.error(error.stack ?? String(error));
    return;
  }
  console.error(error.message);
  for (const detail of error.errors) {
    console.error(`- ${detail}`);
  }
}

const scriptPath = fileURLToPath(import.meta.url);
if (process.argv[1] && scriptPath === path.resolve(process.argv[1])) {
  const scriptDirectory = path.dirname(scriptPath);
  const argumentsWithoutNode = process.argv.slice(2);
  const selfTest = argumentsWithoutNode[0] === "--self-test";
  const emitGolden = argumentsWithoutNode[0] === "--emit-golden";
  const optionOffset = selfTest || emitGolden ? 1 : 0;
  const schemaArgument = argumentsWithoutNode[optionOffset];
  if (argumentsWithoutNode.length > (optionOffset + 1)) {
    console.error("usage: validate-service-wire-schema.mjs [--self-test|--emit-golden] [schema-path]");
    process.exit(2);
  }
  const schemaPath = path.resolve(
    schemaArgument ?? path.join(scriptDirectory, "service-wire-v1.schema.json"),
  );

  try {
    const schema = JSON.parse(fs.readFileSync(schemaPath, "utf8"));
    if (emitGolden) {
      for (const format of schema.durableFormats) {
        const fixturePath = path.resolve(path.dirname(schemaPath), format.goldenFixture);
        const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
        console.log(`${format.name} ${encodeGoldenEnvelope(format, fixture.decoded).toString("hex")}`);
      }
      const logicalProfile = schema.relocationLogicalStreamFormat;
      const logicalFixturePath = path.resolve(
        path.dirname(schemaPath),
        logicalProfile.goldenFixture,
      );
      const logicalFixture = JSON.parse(fs.readFileSync(logicalFixturePath, "utf8"));
      const logicalBytes = encodeGoldenBody(logicalProfile.name, logicalFixture.decoded);
      console.log(
        `${logicalProfile.name} ${logicalBytes.toString("hex")}`,
      );
      console.log(
        `${logicalProfile.name}-metadata length=${logicalBytes.length} crc32c=${crc32c(logicalBytes)}`,
      );
      process.exit(0);
    }
    const summary = validateSchema(schema);
    const fixtureCount = validateGoldenFixtures(schema, schemaPath);
    const logicalFixtureCount = validateRelocationLogicalFixture(schema, schemaPath);
    const jsonFixtureCount = validateFrameworkJsonFixture(schema, schemaPath);
    const multipartFixtureCount = validateFrameworkMultipartFixture(schema, schemaPath);
    const authorityKeyFixtureCount = validateAuthorityKeyFixture(schema, schemaPath);
    const amendmentFixtureCount = validateContractAmendmentFixture(schemaPath);
    const selfTestCount = selfTest
      ? runSelfTests(schema) + runGoldenFixtureSelfTests(schema, schemaPath)
        + runRelocationLogicalFixtureSelfTests(schema, schemaPath)
        + runAuthorityKeyFixtureSelfTests(schema, schemaPath)
        + runContractAmendmentFixtureSelfTests(schemaPath)
      : 0;
    const suffix = selfTest ? `; ${selfTestCount} negative self-tests passed` : "";
    console.log(
      `service wire schema valid: ${summary.commands} commands, ${summary.types} types, `
        + `${summary.flags} flags, ${summary.bounds} bounds, ${fixtureCount} durable fixtures, `
        + `${logicalFixtureCount} logical fixture, ${jsonFixtureCount} JSON fixture, `
        + `${multipartFixtureCount} multipart fixture, `
        + `${authorityKeyFixtureCount} authority key fixture, `
        + `${amendmentFixtureCount} amendment fixture${suffix}`,
    );
  } catch (error) {
    printFailure(error);
    process.exit(1);
  }
}

export {
  SchemaValidationError,
  crc32c,
  encodeGoldenBody,
  validateGoldenFixtures,
  validateSchema,
};

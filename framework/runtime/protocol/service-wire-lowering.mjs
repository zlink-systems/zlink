#!/usr/bin/env node

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import {
  buildNamedMap,
  CONDITION_KINDS,
  conditionSignature,
  hasOwn,
  isObject,
  resolveInteger,
  resolveReference,
} from "./service-wire-schema-model.mjs";
import { SchemaValidationError, validateSchema } from "./validate-service-wire-schema.mjs";

const TYPE_KEYS = new Map([
  ["integer", ["name", "kind", "encoding", "minimum", "maximum"]],
  ["enum", ["name", "kind", "encoding", "values"]],
  ["length-prefixed-bytes", [
    "name", "kind", "lengthType", "minimumBytes", "maximumBytes", "runtimeMaximumBytes",
    "zeroLengthMeaning",
  ]],
  ["length-prefixed-text", [
    "name", "kind", "lengthType", "minimumBytes", "maximumBytes", "runtimeMaximumBytes",
    "zeroLengthMeaning", "encoding", "nul",
  ]],
  ["struct", [
    "name", "kind", "fields", "constraints", "maximumEncodedBytes", "trailingBytes",
    "presence", "scope", "storage", "metadataMeaning", "queueMeaning",
  ]],
  ["vector", [
    "name", "kind", "countType", "maximumItems", "item", "constraints",
    "participantIdDerivation",
  ]],
  ["versioned-vector", [
    "name", "kind", "layout", "constraints", "maximumEncodedBytes", "trailingBytes",
  ]],
  ["versioned-length-delimited", [
    "name", "kind", "version", "length", "body", "maximumEncodedBytes",
    "runtimeMaximumEncodedBytes", "trailingBytes", "constraints", "correlationFields",
    "description",
  ]],
  ["conditional-union", [
    "name", "kind", "discriminators", "bodyLengthType", "bodyLengthCovers", "cases",
    "otherwise", "maximumEncodedBytes", "trailingBytes", "presence", "release",
  ]],
  ["tlv32", [
    "name", "kind", "totalLengthType", "fieldIdType", "fieldLengthType", "fields",
    "presenceRules", "encodingOrder", "duplicateField", "unknownField", "maximumEncodedBytes",
    "trailingBytes",
  ]],
]);

const FIELD_KEYS = new Set([
  "name", "$ref", "constant", "minimum", "maximum", "when", "otherwise", "required", "id",
  "constraints", "description",
]);
const PROTOCOL_KEYS = new Set([
  "name", "magic", "wireMajor", "byteOrder", "headPrefixBytes", "requiredCapability",
]);
const BOUND_KEYS = new Set(["name", "value", "description"]);
const COMMAND_KEYS = new Set([
  "id", "name", "domain", "allowedFlags", "requiredFlags", "flagConstraints", "body", "payload",
  "payloadType", "semanticConstraints",
]);
const FLAG_KEYS = new Set(["name", "bit", "frame", "whenSet", "whenClear", "controls"]);
const CONTEXT_KEYS = new Set(["name", "valueType", "valueKind", "source"]);
const VALUE_KEYS = new Set(["name", "value"]);
const LAYOUT_KEYS = new Set([
  ...FIELD_KEYS, "kind", "counts", "countFrom", "item",
]);
const DISCRIMINATOR_KEYS = new Set(["name", "source", "$ref"]);
const CASE_KEYS = new Set(["when", "fields"]);
const PRESENCE_RULE_KEYS = new Set(["when", "require", "forbid"]);
const TYPE_CONSTRAINT_KEYS = new Set([
  "kind", "field", "fields", "left", "right", "comparison", "unless", "requires", "when",
]);
const FIELD_CONSTRAINT_KEYS = new Set(["kind"]);
const FLAG_CONSTRAINT_KEYS = new Set(["kind", "flags", "if", "then"]);
const DURABLE_FORMAT_KEYS = new Set([
  "name", "magic", "formatVersion", "flags", "flagsType", "byteOrder", "bodyLengthType",
  "maximumEncodedBytes", "body", "checksum", "goldenFixture", "providerInterpretation",
]);
const CHECKSUM_KEYS = new Set(["algorithm", "encoding", "coverage", "position"]);
const LOGICAL_STREAM_KEYS = new Set([
  "name", "encoding", "body", "generatedObjectTree", "maximumBytes", "chunkSplit", "replay",
  "goldenFixture",
]);
const GENERATED_OBJECT_TREE_KEYS = new Set([
  "root", "applicationStates", "savedWork", "timerRegistrations", "pendingTimerTicks",
]);
const CONDITION_KEYS = CONDITION_KINDS;
const CONDITIONAL_FALSE_BEHAVIOR = {
  encoder: "reject-present-value",
  decoder: "consume-no-bytes",
};

class LoweringCoverageError extends Error {
  constructor(errors) {
    super(`service wire lowering coverage failed (${errors.length} error(s))`);
    this.name = "LoweringCoverageError";
    this.errors = errors;
  }
}

function jsonInteger(value) {
  if (value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)) {
    return Number(value);
  }
  return value.toString();
}

function readSchema(schemaPathOrObject) {
  if (typeof schemaPathOrObject === "string") {
    return JSON.parse(fs.readFileSync(path.resolve(schemaPathOrObject), "utf8"));
  }
  return structuredClone(schemaPathOrObject);
}

function namedMap(entries, label) {
  const errors = [];
  const result = buildNamedMap(entries, label, `$.${label}`, (location, message) => {
    errors.push(`${location}: ${message}`);
  });
  if (errors.length > 0) {
    throw new LoweringCoverageError(errors);
  }
  return result;
}

function lowerValue(value, model) {
  if (Array.isArray(value)) {
    return value.map((entry) => lowerValue(entry, model));
  }
  if (!isObject(value)) {
    return value;
  }
  if (hasOwn(value, "$bound")) {
    const resolved = resolveInteger(value, model.bounds);
    if (resolved === null) {
      throw new LoweringCoverageError([`unresolved bound ${JSON.stringify(value.$bound)}`]);
    }
    return jsonInteger(resolved);
  }
  if (hasOwn(value, "$ref") && resolveReference(value, model.types) === null) {
    throw new LoweringCoverageError([`unresolved type ${JSON.stringify(value.$ref)}`]);
  }
  return Object.fromEntries(
    Object.entries(value).map(([key, entry]) => [key, lowerValue(entry, model)]),
  );
}

function flagOperand(name) {
  return { kind: "flag", name };
}

function fieldOperand(name) {
  return { kind: "field", name };
}

function fieldPathOperand(path) {
  return { kind: "fieldPath", path };
}

function lowerPredicateAtom(kind, value, model) {
  if (kind === "fieldPresent") {
    return { kind, operand: { kind: "field", name: value } };
  }
  if (kind === "fieldEquals") {
    return {
      kind,
      operand: { kind: "field", name: value.name },
      value: lowerValue(value.value, model),
    };
  }
  if (kind === "allFlagsSet" || kind === "anyFlagsSet") {
    return { kind, operands: value.map(flagOperand) };
  }
  return {
    kind,
    operand: { kind: "context", name: value.name },
    value: lowerValue(value.value, model),
  };
}

function lowerCondition(condition, model) {
  return {
    all: [...CONDITION_KINDS]
      .filter((kind) => hasOwn(condition, kind))
      .map((kind) => lowerPredicateAtom(kind, condition[kind], model)),
  };
}

function lowerConstraint(constraint, owner, model) {
  const node = lowerValue(constraint, model);
  if (owner.kind === "struct") {
    if (constraint.kind === "not-both-zero") {
      node.fields = owner.fields.map((field) => fieldOperand(field.name));
    } else {
      node.left = fieldOperand(constraint.left);
      node.right = fieldOperand(constraint.right);
    }
  } else if (owner.kind === "vector" || owner.kind === "versioned-vector") {
    if (hasOwn(constraint, "field")) {
      node.field = fieldPathOperand(constraint.field);
    }
    if (hasOwn(constraint, "fields")) {
      node.fields = constraint.fields.map(fieldPathOperand);
    }
  } else if (owner.kind === "field") {
    node.requiredCapability = { kind: "protocol", name: "requiredCapability" };
  }
  return node;
}

function lowerField(field, model) {
  const node = lowerValue(field, model);
  if (hasOwn(field, "when")) {
    node.when = lowerCondition(field.when, model);
    node.whenFalse = { ...CONDITIONAL_FALSE_BEHAVIOR };
  }
  if (hasOwn(field, "constraints")) {
    node.constraints = field.constraints.map(
      (constraint) => lowerConstraint(constraint, { kind: "field" }, model),
    );
  }
  return node;
}

function lowerFields(fields, model) {
  return fields.map((field) => lowerField(field, model));
}

function lowerDiscriminator(discriminator, model) {
  const node = lowerValue(discriminator, model);
  if (discriminator.source === "wire") {
    node.source = { kind: "wire" };
  } else if (hasOwn(discriminator.source, "enclosingField")) {
    node.source = {
      kind: "enclosingField",
      name: discriminator.source.enclosingField,
    };
  } else {
    node.source = { kind: "context", name: discriminator.source.context };
  }
  return node;
}

function lowerUnionOtherwise(otherwise, model) {
  if (otherwise === "protocol-error") {
    return { kind: "protocol-error" };
  }
  return { kind: "fields", fields: lowerFields(otherwise.fields, model) };
}

function lowerType(type, model) {
  const keys = TYPE_KEYS.get(type.kind);
  const node = Object.fromEntries(
    keys.filter((key) => hasOwn(type, key)).map((key) => [key, lowerValue(type[key], model)]),
  );
  if (type.kind === "struct") {
    node.fields = lowerFields(type.fields, model);
  } else if (type.kind === "versioned-length-delimited") {
    node.body = lowerFields(type.body, model);
  } else if (type.kind === "conditional-union") {
    node.discriminators = type.discriminators.map((entry) => lowerDiscriminator(entry, model));
    node.bodyLengthType = hasOwn(type, "bodyLengthType")
      ? lowerValue(type.bodyLengthType, model)
      : null;
    node.bodyLengthCovers = type.bodyLengthCovers ?? null;
    node.cases = Object.fromEntries(type.cases.map((entry) => [
      conditionSignature(entry.when),
      { fields: lowerFields(entry.fields, model) },
    ]));
    node.otherwise = lowerUnionOtherwise(type.otherwise, model);
  } else if (type.kind === "tlv32") {
    node.fields = lowerFields(type.fields, model);
    if (hasOwn(type, "presenceRules")) {
      node.presenceRules = type.presenceRules.map((rule) => ({
        when: lowerCondition(rule.when, model),
        ...(hasOwn(rule, "require") ? { require: [...rule.require] } : {}),
        ...(hasOwn(rule, "forbid") ? { forbid: [...rule.forbid] } : {}),
      }));
    }
  }
  if (hasOwn(type, "constraints")) {
    node.constraints = type.constraints.map((constraint) => lowerConstraint(constraint, type, model));
  }
  return node;
}

function lowerFlagConstraint(constraint) {
  if (constraint.kind === "all-or-none") {
    return { kind: constraint.kind, flags: constraint.flags.map(flagOperand) };
  }
  return {
    kind: constraint.kind,
    if: flagOperand(constraint.if),
    then: constraint.then.map(flagOperand),
  };
}

function lowerCommand(command, model) {
  const node = Object.fromEntries(
    [...COMMAND_KEYS]
      .filter((key) => hasOwn(command, key))
      .map((key) => [key, lowerValue(command[key], model)]),
  );
  node.body = lowerFields(command.body, model);
  node.allowedFlags = command.allowedFlags.map(flagOperand);
  node.requiredFlags = command.requiredFlags.map(flagOperand);
  if (hasOwn(command, "flagConstraints")) {
    node.flagConstraints = command.flagConstraints.map(lowerFlagConstraint);
  }
  node.payload = { policy: command.payload };
  if (hasOwn(command, "payloadType")) {
    node.payload.type = lowerValue(command.payloadType, model);
    delete node.payloadType;
  }
  return node;
}

function lowerDurableFormat(format, model) {
  return lowerValue(format, model);
}

function lowerLogicalStreamFormat(format, model) {
  return lowerValue(format, model);
}

function unknownKeys(value, allowed) {
  return Object.keys(value).filter((key) => !allowed.has(key));
}

function assertKeys(value, allowed, location, errors) {
  if (!isObject(value)) {
    errors.push(`${location}: expected an object`);
    return;
  }
  for (const key of unknownKeys(value, allowed)) {
    errors.push(`${location}.${key}: unknown keyword`);
  }
}

function assertCondition(condition, location, errors) {
  if (!isObject(condition)) {
    return;
  }
  assertKeys(condition, CONDITION_KEYS, location, errors);
  if (isObject(condition.fieldEquals)) {
    assertKeys(condition.fieldEquals, new Set(["name", "value"]), `${location}.fieldEquals`, errors);
  }
  if (isObject(condition.contextEquals)) {
    assertKeys(condition.contextEquals, new Set(["name", "value"]), `${location}.contextEquals`, errors);
  }
}

function assertBoundReferences(value, location, errors) {
  if (Array.isArray(value)) {
    value.forEach((entry, index) => assertBoundReferences(entry, `${location}[${index}]`, errors));
    return;
  }
  if (!isObject(value)) {
    return;
  }
  if (hasOwn(value, "$bound")) {
    assertKeys(value, new Set(["$bound"]), location, errors);
  }
  for (const [key, entry] of Object.entries(value)) {
    assertBoundReferences(entry, `${location}.${key}`, errors);
  }
}

function assertConstraints(constraints, allowed, location, errors) {
  for (const [index, constraint] of (constraints ?? []).entries()) {
    assertKeys(constraint, allowed, `${location}[${index}]`, errors);
  }
}

function assertFields(fields, location, errors) {
  for (const [index, field] of fields.entries()) {
    const fieldLocation = `${location}[${index}]`;
    assertKeys(field, FIELD_KEYS, fieldLocation, errors);
    if (hasOwn(field, "when")) {
      assertCondition(field.when, `${fieldLocation}.when`, errors);
    }
    assertConstraints(field.constraints, FIELD_CONSTRAINT_KEYS, `${fieldLocation}.constraints`, errors);
  }
}

function assertTypeKeywords(type, index, errors) {
  const location = `$.types[${index}]`;
  const allowed = TYPE_KEYS.get(type.kind);
  if (allowed === undefined) {
    errors.push(`${location}.kind: unknown kind ${JSON.stringify(type.kind)}`);
    return;
  }
  assertKeys(type, new Set(allowed), location, errors);
  assertConstraints(type.constraints, TYPE_CONSTRAINT_KEYS, `${location}.constraints`, errors);
  if (type.kind === "enum") {
    type.values.forEach((value, valueIndex) => {
      assertKeys(value, VALUE_KEYS, `${location}.values[${valueIndex}]`, errors);
    });
  }
  if (type.kind === "struct") {
    assertFields(type.fields, `${location}.fields`, errors);
  } else if (type.kind === "versioned-vector") {
    type.layout.forEach((field, fieldIndex) => {
      assertKeys(field, LAYOUT_KEYS, `${location}.layout[${fieldIndex}]`, errors);
    });
  } else if (type.kind === "versioned-length-delimited") {
    assertKeys(type.version, new Set(["$ref", "constant"]), `${location}.version`, errors);
    assertKeys(type.length, new Set(["$ref", "covers"]), `${location}.length`, errors);
    assertFields(type.body, `${location}.body`, errors);
  } else if (type.kind === "conditional-union") {
    type.discriminators.forEach((entry, discriminatorIndex) => {
      assertKeys(entry, DISCRIMINATOR_KEYS,
        `${location}.discriminators[${discriminatorIndex}]`, errors);
    });
    type.cases.forEach((entry, caseIndex) => {
      const caseLocation = `${location}.cases[${caseIndex}]`;
      assertKeys(entry, CASE_KEYS, caseLocation, errors);
      assertFields(entry.fields, `${caseLocation}.fields`, errors);
    });
    if (isObject(type.otherwise)) {
      assertKeys(type.otherwise, new Set(["fields"]), `${location}.otherwise`, errors);
      assertFields(type.otherwise.fields, `${location}.otherwise.fields`, errors);
    }
  } else if (type.kind === "tlv32") {
    assertFields(type.fields, `${location}.fields`, errors);
    for (const [ruleIndex, rule] of (type.presenceRules ?? []).entries()) {
      const ruleLocation = `${location}.presenceRules[${ruleIndex}]`;
      assertKeys(rule, PRESENCE_RULE_KEYS, ruleLocation, errors);
      assertCondition(rule.when, `${ruleLocation}.when`, errors);
    }
  }
}

function assertLoweringCoverage(schema, ir) {
  const errors = [];
  assertKeys(schema.protocol, PROTOCOL_KEYS, "$.protocol", errors);
  schema.bounds.forEach((bound, index) => {
    assertKeys(bound, BOUND_KEYS, `$.bounds[${index}]`, errors);
  });
  schema.types.forEach((type, index) => assertTypeKeywords(type, index, errors));
  schema.flags.forEach((flag, index) => assertKeys(flag, FLAG_KEYS, `$.flags[${index}]`, errors));
  schema.semanticContexts.forEach((context, index) => {
    assertKeys(context, CONTEXT_KEYS, `$.semanticContexts[${index}]`, errors);
  });
  schema.commands.forEach((command, index) => {
    const location = `$.commands[${index}]`;
    assertKeys(command, COMMAND_KEYS, location, errors);
    assertFields(command.body, `${location}.body`, errors);
    assertConstraints(command.flagConstraints, FLAG_CONSTRAINT_KEYS,
      `${location}.flagConstraints`, errors);
  });
  schema.durableFormats.forEach((format, index) => {
    const location = `$.durableFormats[${index}]`;
    assertKeys(format, DURABLE_FORMAT_KEYS, location, errors);
    assertKeys(format.checksum, CHECKSUM_KEYS, `${location}.checksum`, errors);
  });
  assertKeys(schema.relocationLogicalStreamFormat, LOGICAL_STREAM_KEYS,
    "$.relocationLogicalStreamFormat", errors);
  assertKeys(schema.relocationLogicalStreamFormat.generatedObjectTree,
    GENERATED_OBJECT_TREE_KEYS, "$.relocationLogicalStreamFormat.generatedObjectTree", errors);
  assertBoundReferences(schema.types, "$.types", errors);
  assertBoundReferences(schema.flags, "$.flags", errors);
  assertBoundReferences(schema.semanticContexts, "$.semanticContexts", errors);
  assertBoundReferences(schema.commands, "$.commands", errors);
  assertBoundReferences(schema.durableFormats, "$.durableFormats", errors);
  assertBoundReferences(schema.relocationLogicalStreamFormat,
    "$.relocationLogicalStreamFormat", errors);

  const sourceTypeNames = schema.types.map((type) => type.name);
  const loweredTypeNames = ir.types.map((type) => type.name);
  if (JSON.stringify(loweredTypeNames) !== JSON.stringify(sourceTypeNames)) {
    errors.push(`$.types: lowered type inventory differs from the schema`);
  }
  const sourceCommands = schema.commands.map((command) => `${command.id}:${command.name}`);
  const loweredCommands = ir.commands.map((command) => `${command.id}:${command.name}`);
  if (JSON.stringify(loweredCommands) !== JSON.stringify(sourceCommands)) {
    errors.push(`$.commands: lowered command inventory differs from the schema`);
  }
  const kinds = new Set(ir.types.map((type) => type.kind));
  for (const kind of TYPE_KEYS.keys()) {
    if (!kinds.has(kind)) {
      errors.push(`$.types: kind ${kind} did not reach the IR`);
    }
  }
  if (ir.semanticConstraints.length !== schema.semanticConstraints.length) {
    errors.push(`$.semanticConstraints: lowered constraint inventory differs from the schema`);
  }
  const sourceUnions = schema.types.filter((type) => type.kind === "conditional-union");
  const loweredUnions = ir.types.filter((type) => type.kind === "conditional-union");
  if (loweredUnions.length !== sourceUnions.length) {
    errors.push(`$.types: lowered conditional-union inventory differs from the schema`);
  }
  for (const source of sourceUnions) {
    const lowered = loweredUnions.find((type) => type.name === source.name);
    if (lowered === undefined
        || Object.keys(lowered.cases).length !== source.cases.length
        || lowered.discriminators.length !== source.discriminators.length) {
      errors.push(`type:${source.name}: conditional-union cases or discriminators did not reach the IR`);
    }
  }
  const sourceDurableNames = schema.durableFormats.map((format) => format.name);
  const loweredDurableNames = ir.durableFormats.map((format) => format.name);
  if (JSON.stringify(loweredDurableNames) !== JSON.stringify(sourceDurableNames)) {
    errors.push(`$.durableFormats: lowered durable format inventory differs from the schema`);
  }
  for (const source of schema.durableFormats) {
    const lowered = ir.durableFormats.find((format) => format.name === source.name);
    if (lowered === undefined
        || JSON.stringify(lowered.magic) !== JSON.stringify(source.magic)
        || lowered.body?.$ref !== source.body?.$ref
        || lowered.goldenFixture !== source.goldenFixture
        || JSON.stringify(lowered.checksum) !== JSON.stringify(source.checksum)) {
      errors.push(`durableFormat:${source.name}: envelope declaration did not reach the IR`);
    }
  }
  const sourceLogical = schema.relocationLogicalStreamFormat;
  const loweredLogical = ir.relocationLogicalStreamFormat;
  const logicalTreeMatches = Object.entries(sourceLogical.generatedObjectTree ?? {})
    .every(([name, reference]) => loweredLogical?.generatedObjectTree?.[name]?.$ref === reference.$ref);
  if (loweredLogical?.name !== sourceLogical.name
      || loweredLogical?.body?.$ref !== sourceLogical.body?.$ref
      || loweredLogical?.goldenFixture !== sourceLogical.goldenFixture
      || Object.keys(loweredLogical?.generatedObjectTree ?? {}).length
        !== Object.keys(sourceLogical.generatedObjectTree ?? {}).length
      || !logicalTreeMatches) {
    errors.push(`$.relocationLogicalStreamFormat: logical stream declaration did not reach the IR`);
  }
  if (errors.length > 0) {
    throw new LoweringCoverageError(errors);
  }
  return {
    types: ir.types.length,
    commands: ir.commands.length,
    kinds: kinds.size,
    conditionalUnions: loweredUnions.length,
    durableFormats: ir.durableFormats.length,
    logicalStreams: 1,
  };
}

function lowerSchema(schemaPathOrObject) {
  const schema = readSchema(schemaPathOrObject);
  validateSchema(schema);
  const model = {
    bounds: namedMap(schema.bounds, "bounds"),
    types: namedMap(schema.types, "types"),
  };
  const ir = {
    schemaDialect: schema.schemaDialect,
    schemaVersion: schema.schemaVersion,
    protocol: lowerValue(schema.protocol, model),
    bounds: schema.bounds.map((bound) => ({
      ...lowerValue(bound, model),
      value: jsonInteger(resolveInteger(bound.value, model.bounds)),
    })),
    flags: schema.flags.map((flag) => lowerValue(flag, model)),
    semanticContexts: schema.semanticContexts.map((context) => ({
      ...lowerValue(context, model),
      parameter: "decoder-context",
    })),
    semanticConstraints: schema.semanticConstraints.map((constraint) => constraint.kind
      === "terminal-failure-integrity"
      ? {
        kind: constraint.kind,
        runtimePredicate: {
          asset: "service-wire-constants",
          name: "valid-terminal-failure",
        },
      }
      : { kind: constraint.kind }),
    types: schema.types.map((type) => lowerType(type, model)),
    commands: schema.commands.map((command) => lowerCommand(command, model)),
    durableFormats: schema.durableFormats.map((format) => lowerDurableFormat(format, model)),
    relocationLogicalStreamFormat: lowerLogicalStreamFormat(
      schema.relocationLogicalStreamFormat,
      model,
    ),
  };
  assertLoweringCoverage(schema, ir);
  return ir;
}

function expectValidatorFailure(schema, mutate) {
  const candidate = structuredClone(schema);
  mutate(candidate);
  assert.throws(() => lowerSchema(candidate), SchemaValidationError);
}

function runSelfTests(schemaPath) {
  const schema = readSchema(schemaPath);
  const ir = lowerSchema(schemaPath);
  const types = new Map(ir.types.map((type) => [type.name, type]));
  const expected = [
    ["integer", "u8", (type) => [type.encoding, type.minimum, type.maximum], ["u8", 0, 255]],
    ["enum", "bool8", (type) => type.values, [{ name: "false", value: 0 }, { name: "true", value: 1 }]],
    ["length-prefixed-bytes", "rid", (type) => [type.lengthType.$ref, type.minimumBytes, type.maximumBytes], ["u8", 1, 255]],
    ["length-prefixed-text", "text8", (type) => [type.lengthType.$ref, type.maximumBytes, type.encoding, type.nul], ["u8", 255, "utf-8", "forbidden"]],
    ["struct", "metadata-entry", (type) => type.fields.map((field) => field.$ref), ["text8", "metadata-value"]],
    ["vector", "aggregate-participant-vector", (type) => [type.countType.$ref, type.maximumItems, type.item.$ref], ["u16", 1024, "maintenance-aggregate-participant-v1"]],
    ["versioned-vector", "metadata-frame", (type) => [type.maximumEncodedBytes, type.layout[2].countFrom, type.layout[2].item.$ref], [1024, "count", "metadata-entry"]],
    ["versioned-length-delimited", "application-payload-envelope-v1", (type) => [type.version.constant, type.length.$ref, type.maximumEncodedBytes], [1, "u32", 4294967295]],
    ["conditional-union", "actor-join-reply-tail", (type) => [type.discriminators[0].source.kind, Object.keys(type.cases), type.otherwise.kind], ["wire", ["{\"joinResult\":\"accepted\"}", "{\"joinResult\":\"rejected\"}"], "protocol-error"]],
    ["tlv32", "descriptor-extension", (type) => [type.totalLengthType.$ref, type.fields[0].id, type.fields[0].required, type.maximumEncodedBytes], ["u32", 1, true, 1048576]],
  ];
  for (const [kind, name, project, wanted] of expected) {
    const type = types.get(name);
    assert.equal(type.kind, kind);
    assert.deepEqual(project(type), wanted);
  }

  expectValidatorFailure(schema, (candidate) => {
    candidate.types[0].kind = "unknown-kind";
  });
  expectValidatorFailure(schema, (candidate) => {
    candidate.commands[0].body[0].$ref = "undefined-type";
  });
  const optionalActor = types.get("optional-actor-ref");
  const fieldPresent = optionalActor.fields.find((field) => field.name === "generation");
  assert.deepEqual(fieldPresent.when, {
    all: [{ kind: "fieldPresent", operand: { kind: "field", name: "actorId" } }],
  });
  assert.deepEqual(fieldPresent.whenFalse, CONDITIONAL_FALSE_BEHAVIOR);

  const creationTerminal = types.get("creation-operation-terminal-v1");
  const fieldEquals = creationTerminal.body.find((field) => field.name === "creation");
  assert.deepEqual(fieldEquals.when, {
    all: [{
      kind: "fieldEquals",
      operand: { kind: "field", name: "hasCreation" },
      value: "true",
    }],
  });
  assert.deepEqual(fieldEquals.whenFalse, CONDITIONAL_FALSE_BEHAVIOR);

  const commands = new Map(ir.commands.map((command) => [command.name, command]));
  const actorSend = commands.get("actorSend");
  const allFlagsSet = actorSend.body.find((field) => field.name === "boundSessionTail");
  assert.deepEqual(allFlagsSet.when, {
    all: [{
      kind: "allFlagsSet",
      operands: [flagOperand("boundSession"), flagOperand("sourceSpotId")],
    }],
  });
  assert.deepEqual(allFlagsSet.whenFalse, CONDITIONAL_FALSE_BEHAVIOR);

  const anyFlagsSchema = structuredClone(schema);
  const anyFlagsCommand = anyFlagsSchema.commands.find((command) => command.name === "actorSend");
  anyFlagsCommand.body.at(-1).when = { anyFlagsSet: ["boundSession", "sourceSpotId"] };
  const anyFlagsIr = lowerSchema(anyFlagsSchema);
  const anyFlagsSet = anyFlagsIr.commands.find((command) => command.name === "actorSend")
    .body.at(-1);
  assert.deepEqual(anyFlagsSet.when, {
    all: [{
      kind: "anyFlagsSet",
      operands: [flagOperand("boundSession"), flagOperand("sourceSpotId")],
    }],
  });
  assert.deepEqual(anyFlagsSet.whenFalse, CONDITIONAL_FALSE_BEHAVIOR);

  expectValidatorFailure(schema, (candidate) => {
    const type = candidate.types.find((entry) => entry.name === "optional-actor-ref");
    type.fields.find((field) => field.name === "generation").when.fieldPresent = "missing";
  });
  expectValidatorFailure(schema, (candidate) => {
    const type = candidate.types.find((entry) => entry.name === "creation-operation-terminal-v1");
    type.body.find((field) => field.name === "creation").when.fieldEquals.value = "unknown";
  });
  expectValidatorFailure(schema, (candidate) => {
    const command = candidate.commands.find((entry) => entry.name === "actorSend");
    command.body.at(-1).when.allFlagsSet.push("extension");
  });
  expectValidatorFailure(schema, (candidate) => {
    const command = candidate.commands.find((entry) => entry.name === "actorSend");
    command.body.at(-1).when = { anyFlagsSet: ["extension"] };
  });

  const schemaUnions = schema.types.filter((type) => type.kind === "conditional-union");
  const irUnions = ir.types.filter((type) => type.kind === "conditional-union");
  assert.equal(irUnions.length, 30);
  for (const source of schemaUnions) {
    const lowered = irUnions.find((type) => type.name === source.name);
    assert.deepEqual(Object.keys(lowered.cases), source.cases.map((entry) => conditionSignature(entry.when)));
    assert.equal(lowered.bodyLengthType === null, !hasOwn(source, "bodyLengthType"));
    assert.equal(lowered.bodyLengthCovers, source.bodyLengthCovers ?? null);
    assert.equal(
      lowered.otherwise.kind,
      source.otherwise === "protocol-error" ? "protocol-error" : "fields",
    );
  }
  assert.deepEqual(
    new Set(irUnions.flatMap((type) => type.discriminators.map((entry) => entry.source.kind))),
    new Set(["wire", "enclosingField", "context"]),
  );
  assert(irUnions.some((type) => type.otherwise.kind === "fields"
    && type.otherwise.fields.length === 0));

  assert(ir.semanticContexts.every((context) => context.parameter === "decoder-context"));
  assert.deepEqual(actorSend.allowedFlags, [
    flagOperand("metadata"),
    flagOperand("boundSession"),
    flagOperand("sourceSpotId"),
  ]);
  assert.deepEqual(actorSend.requiredFlags, []);
  assert.deepEqual(actorSend.flagConstraints, [{
    kind: "all-or-none",
    flags: [flagOperand("boundSession"), flagOperand("sourceSpotId")],
  }]);
  assert.deepEqual(actorSend.payload, {
    policy: "required",
    type: { $ref: "application-payload-envelope-v1" },
  });
  const requiredFlagSchema = structuredClone(schema);
  requiredFlagSchema.commands.find((command) => command.name === "nodeSend")
    .requiredFlags.push("metadata");
  const requiredFlagIr = lowerSchema(requiredFlagSchema);
  assert.deepEqual(
    requiredFlagIr.commands.find((command) => command.name === "nodeSend").requiredFlags,
    [flagOperand("metadata")],
  );
  const impliesSchema = structuredClone(schema);
  impliesSchema.commands.find((command) => command.name === "actorSend").flagConstraints.push({
    kind: "implies",
    if: "boundSession",
    then: ["sourceSpotId"],
  });
  const impliesIr = lowerSchema(impliesSchema);
  assert.deepEqual(
    impliesIr.commands.find((command) => command.name === "actorSend").flagConstraints.at(-1),
    {
      kind: "implies",
      if: flagOperand("boundSession"),
      then: [flagOperand("sourceSpotId")],
    },
  );
  assert.deepEqual(
    new Set(ir.commands.map((command) => command.payload.policy)),
    new Set(["forbidden", "optional", "required"]),
  );
  assert.deepEqual(
    ir.commands.find((command) => command.name === "instanceSpot").semanticConstraints,
    schema.commands.find((command) => command.name === "instanceSpot").semanticConstraints,
  );

  assert.deepEqual(types.get("operation-id").constraints, [{
    kind: "not-both-zero",
    fields: [fieldOperand("high"), fieldOperand("low")],
  }]);
  assert.deepEqual(types.get("aggregate-participant-vector").constraints.map((entry) => entry.kind),
    ["sorted", "unique"]);
  assert.deepEqual(types.get("aggregate-participant-vector").constraints.map((entry) => entry.field),
    [fieldPathOperand("object"), fieldPathOperand("object")]);
  assert.deepEqual(types.get("metadata-frame").constraints[0].field,
    fieldPathOperand("key"));
  assert.deepEqual(creationTerminal.constraints.map((entry) => entry.kind), [
    "terminal-success-shape",
    "terminal-failure-shape",
    "existing-has-no-application-payload",
  ]);
  const descriptor = types.get("descriptor-extension");
  assert.deepEqual(
    descriptor.fields.find((field) => field.name === "protocolCapabilities").constraints,
    [{
      kind: "contains-protocol-required-capability",
      requiredCapability: { kind: "protocol", name: "requiredCapability" },
    }],
  );

  const comparisonSchema = structuredClone(schema);
  comparisonSchema.types.find((type) => type.name === "retired-bound-session-route-fence")
    .constraints = [{
      kind: "field-less-than-or-equal",
      left: "sessionOwnerNodeGeneration",
      right: "sessionOwnerLeaseGeneration",
    }];
  const comparisonIr = lowerSchema(comparisonSchema);
  assert.deepEqual(
    comparisonIr.types.find((type) => type.name === "retired-bound-session-route-fence").constraints,
    [{
      kind: "field-less-than-or-equal",
      left: fieldOperand("sessionOwnerNodeGeneration"),
      right: fieldOperand("sessionOwnerLeaseGeneration"),
    }],
  );

  const presenceSchema = structuredClone(schema);
  presenceSchema.types.find((type) => type.name === "descriptor-extension").presenceRules = [{
    when: { contextEquals: { name: "durableRelocationPresent", value: true } },
    require: ["runtimeState"],
    forbid: ["spotTypes"],
  }];
  const presenceIr = lowerSchema(presenceSchema);
  assert.deepEqual(
    presenceIr.types.find((type) => type.name === "descriptor-extension").presenceRules,
    [{
      when: {
        all: [{
          kind: "contextEquals",
          operand: { kind: "context", name: "durableRelocationPresent" },
          value: true,
        }],
      },
      require: ["runtimeState"],
      forbid: ["spotTypes"],
    }],
  );

  assert.deepEqual(ir.durableFormats.map((format) => format.name), [
    "authority-payload-v1",
    "instance-activation-recovery-v1",
    "relocation-data-chunk-v1",
    "relocation-manifest-v1",
  ]);
  assert.deepEqual(ir.durableFormats[0], {
    name: "authority-payload-v1",
    magic: [90, 76, 65, 85],
    formatVersion: 1,
    flags: 0,
    flagsType: { $ref: "u16" },
    byteOrder: "big-endian",
    bodyLengthType: { $ref: "u32" },
    maximumEncodedBytes: 1048576,
    body: { $ref: "authority-payload-v1" },
    checksum: {
      algorithm: "crc32c-castagnoli",
      encoding: "u32-big-endian",
      coverage: "magic-through-body",
      position: "trailing",
    },
    goldenFixture: "golden/durable-authority-v1.json",
    providerInterpretation: "opaque-bytes",
  });
  assert(ir.durableFormats.every((format) => format.checksum.algorithm === "crc32c-castagnoli"
    && format.checksum.position === "trailing"));
  assert.deepEqual(ir.relocationLogicalStreamFormat, {
    name: "relocation-envelope-v1",
    encoding: "canonical-big-endian-field-stream-without-monolithic-provider-envelope",
    body: { $ref: "relocation-envelope-v1" },
    generatedObjectTree: {
      root: { $ref: "relocation-envelope-v1" },
      applicationStates: { $ref: "relocation-participant-application-state-vector" },
      savedWork: { $ref: "saved-work-vector" },
      timerRegistrations: { $ref: "relocation-timer-registration-vector" },
      pendingTimerTicks: { $ref: "relocation-pending-timer-tick-vector" },
    },
    maximumBytes: 274877906944,
    chunkSplit: "any-byte-boundary-including-within-frozen-record",
    replay: "bounded-incremental-decode-without-whole-stream-allocation",
    goldenFixture: "golden/relocation-envelope-v1.json",
  });
  expectValidatorFailure(schema, (candidate) => {
    candidate.durableFormats[0].checksum.algorithm = "unknown-checksum";
  });
  expectValidatorFailure(schema, (candidate) => {
    candidate.relocationLogicalStreamFormat.generatedObjectTree.savedWork.$ref = "undefined-type";
  });

  const unknownKeyword = structuredClone(schema);
  unknownKeyword.types[0].futureKeyword = true;
  assert.throws(() => lowerSchema(unknownKeyword), LoweringCoverageError);
  const unknownDurableKeyword = structuredClone(schema);
  unknownDurableKeyword.durableFormats[0].futureKeyword = true;
  assert.throws(() => lowerSchema(unknownDurableKeyword), LoweringCoverageError);
  assert.deepEqual(JSON.parse(JSON.stringify(ir)), ir);
  assert.equal(ir.types.length, 156);
  assert.equal(ir.commands.length, 40);
  assert.equal(ir.semanticConstraints.filter((constraint) => constraint.runtimePredicate).length, 1);
  assert.equal(
    ir.semanticConstraints.find((constraint) => constraint.runtimePredicate).kind,
    "terminal-failure-integrity",
  );
  assert.deepEqual(
    ir.semanticConstraints.find((constraint) => constraint.runtimePredicate).runtimePredicate,
    { asset: "service-wire-constants", name: "valid-terminal-failure" },
  );
  assert(ir.semanticConstraints
    .filter((constraint) => constraint.kind !== "terminal-failure-integrity")
    .every((constraint) => Object.keys(constraint).length === 1));
  return {
    conditionForms: 4,
    validatorNegativeConditions: 4,
    conditionalUnions: irUnions.length,
    layoutConstraintKinds: 8,
    tlvPresenceRules: 1,
    durableFormats: ir.durableFormats.length,
    logicalStreams: 1,
  };
}

function printFailure(error) {
  console.error(error.message ?? String(error));
  for (const detail of error.errors ?? []) {
    console.error(`- ${detail}`);
  }
}

const scriptPath = fileURLToPath(import.meta.url);
if (process.argv[1] && scriptPath === path.resolve(process.argv[1])) {
  const argumentsWithoutNode = process.argv.slice(2);
  const option = argumentsWithoutNode[0];
  const selfTest = option === "--self-test";
  const check = option === "--check";
  const optionOffset = selfTest || check ? 1 : 0;
  if ((!selfTest && !check) || argumentsWithoutNode.length > optionOffset + 1) {
    console.error("usage: service-wire-lowering.mjs --check|--self-test [schema-path]");
    process.exit(2);
  }
  const schemaPath = path.resolve(
    argumentsWithoutNode[optionOffset]
      ?? path.join(path.dirname(scriptPath), "service-wire-v1.schema.json"),
  );
  try {
    if (selfTest) {
      const result = runSelfTests(schemaPath);
      console.log(
        `service wire lowering self-test passed: ${result.conditionForms} condition forms, `
          + `${result.validatorNegativeConditions} validator-negative conditions, `
          + `${result.conditionalUnions} conditional unions, `
          + `${result.layoutConstraintKinds} layout constraint kinds, `
          + `${result.tlvPresenceRules} TLV presence rule, `
          + `${result.durableFormats} durable formats, `
          + `${result.logicalStreams} logical stream, JSON round-trip`,
      );
    } else {
      const schema = readSchema(schemaPath);
      const ir = lowerSchema(schema);
      const coverage = assertLoweringCoverage(schema, ir);
      console.log(
        `service wire lowering valid: ${coverage.types} types, ${coverage.commands} commands, `
          + `${coverage.kinds} kinds, ${coverage.conditionalUnions} conditional unions, `
          + `${coverage.durableFormats} durable formats, ${coverage.logicalStreams} logical stream`,
      );
    }
  } catch (error) {
    printFailure(error);
    process.exit(1);
  }
}

export {
  LoweringCoverageError,
  assertLoweringCoverage,
  lowerSchema,
};

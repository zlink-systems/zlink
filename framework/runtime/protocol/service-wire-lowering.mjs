#!/usr/bin/env node

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import {
  buildNamedMap,
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
  "kind", "field", "fields", "comparison", "unless", "requires", "when",
]);
const FIELD_CONSTRAINT_KEYS = new Set(["kind"]);
const FLAG_CONSTRAINT_KEYS = new Set(["kind", "flags", "if", "then"]);
const CONDITION_KEYS = new Set([
  "fieldPresent", "fieldEquals", "allFlagsSet", "anyFlagsSet", "contextEquals",
]);

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

function lowerFields(fields, model) {
  return fields.map((field) => lowerValue(field, model));
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
    node.cases = type.cases.map((entry) => ({
      when: JSON.parse(conditionSignature(lowerValue(entry.when, model))),
      fields: lowerFields(entry.fields, model),
    }));
    if (isObject(type.otherwise)) {
      node.otherwise = { fields: lowerFields(type.otherwise.fields, model) };
    }
  } else if (type.kind === "tlv32") {
    node.fields = lowerFields(type.fields, model);
  }
  return node;
}

function lowerCommand(command, model) {
  const node = Object.fromEntries(
    [...COMMAND_KEYS]
      .filter((key) => hasOwn(command, key))
      .map((key) => [key, lowerValue(command[key], model)]),
  );
  node.body = lowerFields(command.body, model);
  return node;
}

function unknownKeys(value, allowed) {
  return Object.keys(value).filter((key) => !allowed.has(key));
}

function assertKeys(value, allowed, location, errors) {
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
  assertBoundReferences(schema.types, "$.types", errors);
  assertBoundReferences(schema.flags, "$.flags", errors);
  assertBoundReferences(schema.semanticContexts, "$.semanticContexts", errors);
  assertBoundReferences(schema.commands, "$.commands", errors);

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
  if (errors.length > 0) {
    throw new LoweringCoverageError(errors);
  }
  return {
    types: ir.types.length,
    commands: ir.commands.length,
    kinds: kinds.size,
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
    semanticContexts: schema.semanticContexts.map((context) => lowerValue(context, model)),
    semanticConstraints: schema.semanticConstraints.map((constraint) => constraint.kind
      === "terminal-failure-integrity"
      ? { kind: constraint.kind, runtime: true }
      : { kind: constraint.kind }),
    types: schema.types.map((type) => lowerType(type, model)),
    commands: schema.commands.map((command) => lowerCommand(command, model)),
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
    ["conditional-union", "actor-join-reply-tail", (type) => [type.discriminators[0].$ref, type.cases.map((entry) => entry.when.joinResult), type.otherwise], ["actor-join-result", ["accepted", "rejected"], "protocol-error"]],
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
  expectValidatorFailure(schema, (candidate) => {
    candidate.commands[0].body[0].when = { fieldPresent: "laterField" };
    candidate.commands[0].body[0].otherwise = "forbidden";
  });
  const unknownKeyword = structuredClone(schema);
  unknownKeyword.types[0].futureKeyword = true;
  assert.throws(() => lowerSchema(unknownKeyword), LoweringCoverageError);
  assert.deepEqual(JSON.parse(JSON.stringify(ir)), ir);
  assert.equal(ir.types.length, 155);
  assert.equal(ir.commands.length, 40);
  assert.equal(ir.semanticConstraints.filter((constraint) => constraint.runtime).length, 1);
  assert.equal(
    ir.semanticConstraints.find((constraint) => constraint.runtime).kind,
    "terminal-failure-integrity",
  );
  return 15;
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
      const count = runSelfTests(schemaPath);
      console.log(
        `service wire lowering self-test passed: ${count} cases `
          + `(10 kinds, 3 validator-negative, 1 coverage-negative, JSON round-trip)`,
      );
    } else {
      const schema = readSchema(schemaPath);
      const ir = lowerSchema(schema);
      const coverage = assertLoweringCoverage(schema, ir);
      console.log(
        `service wire lowering valid: ${coverage.types} types, ${coverage.commands} commands, `
          + `${coverage.kinds} kinds`,
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

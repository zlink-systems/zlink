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
  TYPE_KINDS,
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
const CASE_KEYS = new Set(["when", "fields", "constraints"]);
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
const INTEGER_WIDTHS = Object.freeze({ u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 });
const SINGLE_ARRAY_REPRESENTATION_CEILING = 2 ** 31 - 1;
const OPERATION_KINDS = Object.freeze([
  "integer",
  "enum",
  "length-prefixed",
  "text-validation",
  "field",
  "struct",
  "vector",
  "versioned-vector",
  "bounded-reader",
  "versioned-length-delimited",
  "discriminator",
  "conditional-union",
  "tlv32",
  "constraint",
  "command-header",
  "flags",
  "flag-constraint",
  "metadata-flag-frame",
  "payload",
  "durable-header",
  "checksum",
  "encoded-limit",
  "negotiated-bound",
  "logical-stream",
  "runtime-predicate",
]);
const OPERATION_KIND_SET = new Set(OPERATION_KINDS);

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

function fieldPresence(fieldName, fields, model) {
  const field = fields.find((entry) => entry.name === fieldName);
  const type = field === undefined ? null : resolveReference({ $ref: field.$ref }, model.types);
  if (field === undefined || type?.zeroLengthMeaning !== "absent") {
    throw new LoweringCoverageError([
      `fieldPresent:${fieldName}: no wire absence declaration is available`,
    ]);
  }
  return {
    wire: {
      kind: "length-prefix-sentinel",
      lengthType: lowerValue(type.lengthType, model),
      absent: 0,
      present: { minimum: 1 },
    },
    internal: {
      absent: null,
      present: "non-null",
    },
  };
}

function lowerPredicateAtom(kind, value, model, fields = []) {
  if (kind === "fieldPresent") {
    return {
      kind,
      operand: { kind: "field", name: value },
      presence: fieldPresence(value, fields, model),
    };
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

function lowerCondition(condition, model, fields = []) {
  return {
    all: [...CONDITION_KINDS]
      .filter((kind) => hasOwn(condition, kind))
      .map((kind) => lowerPredicateAtom(kind, condition[kind], model, fields)),
  };
}

function constraintOperation(constraint) {
  return { op: "constraint", ...constraint };
}

function unionCaseOwner(typeName, signature) {
  return {
    kind: "conditional-union-case",
    path: ["types", typeName, "cases", signature],
  };
}

function negotiatedBoundOperation(runtimeMaximum, measured) {
  const clientServer = runtimeMaximum.clientServer;
  const contextName = clientServer.$negotiatedBound;
  return {
    op: "negotiated-bound",
    topology: "clientServer",
    context: {
      name: contextName,
      absoluteMaximum: clientServer.absoluteMaximum,
      missing: "protocol-error",
      negative: "protocol-error",
      aboveAbsoluteMaximum: "protocol-error",
    },
    applications: {
      encode: { context: { kind: "encoder-context", name: contextName } },
      decode: { context: { kind: "decoder-context", name: contextName } },
    },
    measured,
    comparison: "less-than-or-equal",
  };
}

function encodedLimitOperation(maximumEncodedBytes, kind) {
  const includes = kind === "tlv32"
    ? ["totalLength", "fields"]
    : kind === "durable-format"
      ? ["header", "body", "checksum"]
      : kind === "logical-stream"
        ? ["logical-stream-bytes"]
        : ["all-prefixes", "body"];
  return {
    op: "encoded-limit",
    maximumEncodedBytes,
    measured: "complete-encoded-value",
    includes,
    applications: ["encode", "decode"],
    exceeded: "protocol-error",
  };
}

function representationCapacity(declaredMaximumBytes) {
  return {
    declaredMaximumBytes,
    representationCeiling: SINGLE_ARRAY_REPRESENTATION_CEILING,
    requiredThroughBytes: Math.min(
      declaredMaximumBytes,
      SINGLE_ARRAY_REPRESENTATION_CEILING,
    ),
    applications: ["encode", "decode"],
    aboveDeclaredMaximum: "protocol-error",
    aboveRepresentationCeiling: "capacity-error",
  };
}

function fieldOperation(field, model, fields) {
  const operation = {
    op: "field",
    name: field.name,
    type: lowerValue({ $ref: field.$ref }, model),
  };
  for (const key of ["constant", "minimum", "maximum", "required", "id", "otherwise"]) {
    if (hasOwn(field, key)) {
      operation[key] = lowerValue(field[key], model);
    }
  }
  if (hasOwn(field, "when")) {
    operation.when = lowerCondition(field.when, model, fields);
    operation.whenFalse = { ...CONDITIONAL_FALSE_BEHAVIOR };
  }
  if (hasOwn(field, "constraints")) {
    operation.constraints = field.constraints.map((constraint) => constraintOperation(
      lowerConstraint(constraint, { kind: "field" }, model),
    ));
  }
  return operation;
}

function fieldOperations(fields, model) {
  return fields.map((field) => fieldOperation(field, model, fields));
}

function runtimePredicateOperation(predicate, targets) {
  return {
    op: "runtime-predicate",
    reference: { asset: predicate.asset, name: predicate.name },
    targets: targets.map(fieldPathOperand),
  };
}

function ownerRuntimePredicates(name, runtimePredicates) {
  return runtimePredicates
    .map((predicate) => ({
      ...predicate,
      targets: predicate.targets.filter((target) => target === name
        || target.startsWith(`${name}.`)),
    }))
    .filter((predicate) => predicate.targets.length > 0)
    .map((predicate) => runtimePredicateOperation(predicate.reference, predicate.targets));
}

function vectorComparisonSources(constraint) {
  if (hasOwn(constraint, "fields")) return constraint.fields;
  if (hasOwn(constraint, "field")) return [constraint.field];
  return [null];
}

function vectorItemFieldType(owner, fieldName, model) {
  const itemReference = owner.item
    ?? owner.layout?.find((entry) => entry.kind === "repeat")?.item;
  const item = resolveReference(itemReference, model.types);
  if (fieldName === null) return item;
  const field = item?.fields?.find((entry) => entry.name === fieldName);
  return field === undefined ? null : resolveReference({ $ref: field.$ref }, model.types);
}

function comparisonPart(owner, fieldName, comparison, model) {
  const source = fieldName === null ? { kind: "item" } : fieldPathOperand(fieldName);
  const type = vectorItemFieldType(owner, fieldName, model);
  if (comparison === "utf-8-bytes") {
    return {
      kind: "utf-8-bytes",
      source,
      lengthPrefix: "excluded",
    };
  }
  if (comparison === "unsigned-wire-value") {
    return {
      kind: "unsigned-wire-value",
      source,
      encoding: type?.encoding,
      byteOrder: "big-endian",
    };
  }
  if (comparison === "wire-value-then-utf-8-bytes") {
    if (type?.kind === "length-prefixed-text") {
      return { kind: "utf-8-bytes", source, lengthPrefix: "excluded" };
    }
    return {
      kind: "wire-value",
      source,
      encoding: type?.encoding,
      byteOrder: "big-endian",
    };
  }
  throw new LoweringCoverageError([
    `type:${owner.name}: unsupported comparison ${JSON.stringify(comparison)}`,
  ]);
}

function comparisonKey(constraint, owner, model) {
  if (constraint.comparison === "canonical-authority-key-bytes") {
    return {
      kind: "canonical-authority-key-bytes",
      source: fieldPathOperand(constraint.field),
      format: lowerValue(model.authorityKeyFormat, model),
      variants: {
        actor: { objectKind: "actor", components: ["actor.actorId"] },
        userSpot: { objectKind: "spot", components: ["spot.spotId"] },
        instanceSpot: { objectKind: "spot", components: ["spotId"] },
      },
      excluded: [
        "wire-length-prefixes",
        "object-generation",
        "expected-authority-owner-generation",
      ],
    };
  }
  return {
    kind: "tuple",
    parts: vectorComparisonSources(constraint).map(
      (fieldName) => comparisonPart(owner, fieldName, constraint.comparison, model),
    ),
  };
}

function lowerConstraints(constraints, owner, model) {
  let inheritedComparison = null;
  return constraints.map((constraint) => {
    const effective = constraint.comparison ?? inheritedComparison;
    if (constraint.comparison !== undefined) inheritedComparison = constraint.comparison;
    const node = lowerConstraint(constraint, owner, model);
    if (constraint.kind === "sorted" || constraint.kind === "unique") {
      if (effective === null) {
        throw new LoweringCoverageError([
          `type:${owner.name}: ${constraint.kind} has no comparison key`,
        ]);
      }
      node.comparison = effective;
      node.key = comparisonKey({ ...constraint, comparison: effective }, owner, model);
    }
    return node;
  });
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

function lowerField(field, model, fields) {
  const node = lowerValue(field, model);
  if (hasOwn(field, "when")) {
    node.when = lowerCondition(field.when, model, fields);
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
  return fields.map((field) => lowerField(field, model, fields));
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

function typeOperations(type, node, model, runtimePredicates) {
  const predicates = ownerRuntimePredicates(type.name, runtimePredicates);
  if (type.kind === "integer") {
    return [{
      op: "integer",
      encoding: node.encoding,
      width: INTEGER_WIDTHS[node.encoding],
      byteOrder: "big-endian",
      minimum: node.minimum,
      maximum: node.maximum,
    }, ...predicates];
  }
  if (type.kind === "enum") {
    return [{
      op: "enum",
      encoding: node.encoding,
      width: INTEGER_WIDTHS[node.encoding],
      byteOrder: "big-endian",
      values: node.values,
      unknown: "protocol-error",
    }, ...predicates];
  }
  if (type.kind === "length-prefixed-bytes" || type.kind === "length-prefixed-text") {
    const operations = [{
      op: "length-prefixed",
      lengthType: node.lengthType,
      content: type.kind === "length-prefixed-text" ? "text" : "bytes",
      minimumBytes: node.minimumBytes,
      maximumBytes: node.maximumBytes,
      ...(hasOwn(node, "zeroLengthMeaning")
        ? { zeroLengthMeaning: node.zeroLengthMeaning }
        : {}),
      encodeCapacity: representationCapacity(node.maximumBytes),
    }];
    if (hasOwn(node, "runtimeMaximumBytes")) {
      operations.push(negotiatedBoundOperation(node.runtimeMaximumBytes, "content-bytes"));
    }
    if (type.kind === "length-prefixed-text") {
      operations.push({
        op: "text-validation",
        encoding: node.encoding,
        malformed: "protocol-error",
        nul: node.nul,
        decode: {
          bom: "preserve",
          overlong: "protocol-error",
          surrogateCodePoint: "protocol-error",
        },
        encode: {
          loneSurrogate: "protocol-error",
        },
      });
    }
    return [...operations, ...predicates];
  }
  if (type.kind === "struct") {
    return [{
      op: "struct",
      order: "sequential",
      fields: fieldOperations(type.fields, model),
      constraints: (node.constraints ?? []).map(constraintOperation),
      trailingBytes: node.trailingBytes ?? "allowed",
    }, ...(hasOwn(node, "maximumEncodedBytes")
      ? [encodedLimitOperation(node.maximumEncodedBytes, type.kind)]
      : []), ...predicates];
  }
  if (type.kind === "vector") {
    return [{
      op: "vector",
      countType: node.countType,
      item: node.item,
      constraints: (node.constraints ?? []).map(constraintOperation),
      ...(hasOwn(node, "maximumItems") ? { maximumItems: node.maximumItems } : {}),
    }, ...predicates];
  }
  if (type.kind === "versioned-vector") {
    return [{
      op: "versioned-vector",
      order: "sequential",
      layout: lowerValue(type.layout, model),
      constraints: (node.constraints ?? []).map(constraintOperation),
      trailingBytes: node.trailingBytes,
    }, encodedLimitOperation(node.maximumEncodedBytes, type.kind), ...predicates];
  }
  if (type.kind === "versioned-length-delimited") {
    const operations = [{
      op: "versioned-length-delimited",
      version: node.version,
      length: node.length,
      reader: {
        op: "bounded-reader",
        boundary: node.length.covers,
        trailingBytes: node.trailingBytes,
      },
      fields: fieldOperations(type.body, model),
      constraints: (node.constraints ?? []).map(constraintOperation),
    }];
    if (hasOwn(node, "maximumEncodedBytes")) {
      operations.push(encodedLimitOperation(node.maximumEncodedBytes, type.kind));
    }
    if (hasOwn(node, "runtimeMaximumEncodedBytes")) {
      operations.push(negotiatedBoundOperation(
        node.runtimeMaximumEncodedBytes,
        "encoded-bytes",
      ));
    }
    return [...operations, ...predicates];
  }
  if (type.kind === "conditional-union") {
    const operations = [{
      op: "conditional-union",
      discriminators: node.discriminators.map((discriminator) => ({
        op: "discriminator",
        name: discriminator.name,
        source: discriminator.source,
        type: { $ref: discriminator.$ref },
      })),
      bodyLengthType: node.bodyLengthType,
      bodyLengthCovers: node.bodyLengthCovers,
      reader: node.bodyLengthType === null ? null : {
        op: "bounded-reader",
        boundary: node.bodyLengthCovers,
        trailingBytes: node.trailingBytes,
      },
      cases: Object.fromEntries(type.cases.map((entry) => {
        const signature = conditionSignature(entry.when);
        return [signature, {
          operations: [
            ...fieldOperations(entry.fields, model),
            ...(entry.constraints ?? []).map((constraint) => ({
              ...constraintOperation(lowerConstraint(
                constraint,
                { kind: "struct", fields: entry.fields },
                model,
              )),
              owner: unionCaseOwner(type.name, signature),
            })),
          ],
        }];
      })),
      otherwise: type.otherwise === "protocol-error"
        ? { kind: "protocol-error" }
        : { kind: "fields", fields: fieldOperations(type.otherwise.fields, model) },
      encode: {
        selection: "variant",
        discriminatorAgreement: "required",
        mismatch: "protocol-error",
      },
    }];
    if (hasOwn(node, "maximumEncodedBytes")) {
      operations.push(encodedLimitOperation(node.maximumEncodedBytes, type.kind));
    }
    return [...operations, ...predicates];
  }
  if (type.kind === "tlv32") {
    return [{
      op: "tlv32",
      totalLengthType: node.totalLengthType,
      fieldIdType: node.fieldIdType,
      fieldLengthType: node.fieldLengthType,
      reader: { op: "bounded-reader", boundary: "totalLength", trailingBytes: node.trailingBytes },
      fields: fieldOperations(type.fields, model),
      requiredFields: type.fields.filter((field) => field.required).map((field) => field.name),
      presenceRules: node.presenceRules ?? [],
      encodingOrder: node.encodingOrder,
      duplicateField: node.duplicateField,
      unknownField: node.unknownField,
    }, encodedLimitOperation(node.maximumEncodedBytes, type.kind), ...predicates];
  }
  throw new LoweringCoverageError([`type:${type.name}: no operation lowering for ${type.kind}`]);
}

function lowerType(type, model, runtimePredicates) {
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
      {
        fields: lowerFields(entry.fields, model),
        ...(hasOwn(entry, "constraints") ? {
          constraints: entry.constraints.map((constraint) => lowerConstraint(
            constraint,
            { kind: "struct", fields: entry.fields },
            model,
          )),
        } : {}),
      },
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
    node.constraints = lowerConstraints(type.constraints, type, model);
  }
  node.operations = typeOperations(type, node, model, runtimePredicates);
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

function lowerCommand(command, model, runtimePredicates) {
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
  const metadata = model.flags.get("metadata");
  node.operations = [{
    op: "command-header",
    magic: [...model.protocol.magic],
    wireMajor: model.protocol.wireMajor,
    commandId: command.id,
    byteOrder: model.protocol.byteOrder,
  }, {
    op: "flags",
    allowed: node.allowedFlags,
    required: node.requiredFlags,
    unknown: "protocol-error",
  }, ...(node.flagConstraints ?? []).map((constraint) => ({
    op: "flag-constraint",
    ...constraint,
  })), ...(command.allowedFlags.includes("metadata") ? [{
    op: "metadata-flag-frame",
    flag: flagOperand("metadata"),
    frame: lowerValue(metadata.frame, model),
    whenSet: metadata.whenSet,
    whenClear: metadata.whenClear,
  }] : []), ...fieldOperations(command.body, model), {
    op: "payload",
    ...node.payload,
  }, ...ownerRuntimePredicates(command.name, runtimePredicates)];
  return node;
}

function lowerDurableFormat(format, model, runtimePredicates) {
  const node = lowerValue(format, model);
  node.operations = [{
    op: "durable-header",
    magic: node.magic,
    formatVersion: node.formatVersion,
    flags: node.flags,
    flagsType: node.flagsType,
    byteOrder: node.byteOrder,
    bodyLengthType: node.bodyLengthType,
    flagsComparison: "exact",
  }, {
    op: "bounded-reader",
    boundary: "bodyLength",
    trailingBytes: "checksum-only",
  }, encodedLimitOperation(node.maximumEncodedBytes, "durable-format"), {
    op: "checksum",
    ...node.checksum,
    mismatch: "protocol-error",
    verification: "before-body-interpretation",
  }, {
    op: "field",
    name: "body",
    type: node.body,
    interpretation: "after-checksum-verification",
  }, ...ownerRuntimePredicates(format.name, runtimePredicates)];
  return node;
}

function lowerLogicalStreamFormat(format, model, runtimePredicates) {
  const node = lowerValue(format, model);
  node.operations = [{
    op: "logical-stream",
    encoding: node.encoding,
    body: node.body,
    maximumBytes: node.maximumBytes,
    chunkSplit: node.chunkSplit,
    replay: node.replay,
    generatedObjectTree: node.generatedObjectTree,
  }, encodedLimitOperation(node.maximumBytes, "logical-stream"),
  ...ownerRuntimePredicates(format.name, runtimePredicates)];
  return node;
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
      assertConstraints(entry.constraints, TYPE_CONSTRAINT_KEYS,
        `${caseLocation}.constraints`, errors);
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
  for (const kind of TYPE_KINDS) {
    if (!kinds.has(kind)) {
      errors.push(`$.types: kind ${kind} did not reach the IR`);
    }
  }
  if (ir.semanticConstraints.length !== schema.semanticConstraints.length) {
    errors.push(`$.semanticConstraints: lowered constraint inventory differs from the schema`);
  }
  for (const [index, source] of schema.semanticConstraints.entries()) {
    const lowered = structuredClone(ir.semanticConstraints[index]);
    delete lowered.runtimePredicate;
    if (JSON.stringify(lowered) !== JSON.stringify(source)) {
      errors.push(`$.semanticConstraints[${index}]: literal tuple did not reach the IR`);
    }
  }
  if (JSON.stringify(ir.relocationStateMachine) !== JSON.stringify(schema.relocationStateMachine)) {
    errors.push(`$.relocationStateMachine: declaration did not reach the IR`);
  }
  const operationOwners = [
    ...ir.types.map((entry) => [`type:${entry.name}`, entry.operations]),
    ...ir.commands.map((entry) => [`command:${entry.name}`, entry.operations]),
    ...ir.durableFormats.map((entry) => [`durable:${entry.name}`, entry.operations]),
    [`logical:${ir.relocationLogicalStreamFormat.name}`,
      ir.relocationLogicalStreamFormat.operations],
  ];
  const seenOperations = new Set();
  function inspectOperations(value, location) {
    if (Array.isArray(value)) {
      value.forEach((entry, index) => inspectOperations(entry, `${location}[${index}]`));
      return;
    }
    if (!isObject(value)) return;
    if (hasOwn(value, "op")) {
      if (!OPERATION_KIND_SET.has(value.op)) {
        errors.push(`${location}.op: unknown operation ${JSON.stringify(value.op)}`);
      } else {
        seenOperations.add(value.op);
      }
    }
    for (const [key, entry] of Object.entries(value)) {
      inspectOperations(entry, `${location}.${key}`);
    }
  }
  for (const [owner, operations] of operationOwners) {
    if (!Array.isArray(operations) || operations.length === 0) {
      errors.push(`${owner}: operation sequence is missing`);
    } else {
      inspectOperations(operations, owner);
    }
  }
  for (const operation of OPERATION_KINDS) {
    if (!seenOperations.has(operation)) {
      errors.push(`$.operationVocabulary: operation ${operation} has no schema coverage`);
    }
  }
  if (JSON.stringify(ir.operationVocabulary) !== JSON.stringify(OPERATION_KINDS)) {
    errors.push(`$.operationVocabulary: closed vocabulary differs from the lowering owner`);
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
      continue;
    }
    const unionOperation = lowered.operations.find((operation) => operation.op === "conditional-union");
    for (const sourceCase of source.cases) {
      const signature = conditionSignature(sourceCase.when);
      const caseOperations = unionOperation?.cases?.[signature]?.operations;
      const expectedConstraints = (lowered.cases[signature].constraints ?? []).map(
        (constraint) => ({
          ...constraintOperation(constraint),
          owner: unionCaseOwner(source.name, signature),
        }),
      );
      const constraintOffset = sourceCase.fields.length;
      const constraints = (caseOperations ?? []).slice(constraintOffset);
      if (!Array.isArray(caseOperations)
          || caseOperations.slice(0, constraintOffset).some((operation) => operation.op !== "field")
          || JSON.stringify(constraints) !== JSON.stringify(expectedConstraints)) {
        errors.push(`type:${source.name}: case ${signature} constraints did not reach ordered operations`);
      }
    }
  }
  for (const source of schema.types.filter((type) => hasOwn(type, "maximumEncodedBytes"))) {
    const lowered = ir.types.find((type) => type.name === source.name);
    const limit = lowered.operations.find((operation) => operation.op === "encoded-limit");
    if (JSON.stringify(limit) !== JSON.stringify(
      encodedLimitOperation(lowered.maximumEncodedBytes, source.kind),
    )) {
      errors.push(`type:${source.name}: complete encoded limit did not reach operations`);
    }
  }
  for (const source of schema.types) {
    const lowered = ir.types.find((type) => type.name === source.name);
    let inheritedComparison = null;
    for (const [index, constraint] of (source.constraints ?? []).entries()) {
      if (constraint.comparison !== undefined) inheritedComparison = constraint.comparison;
      if (["sorted", "unique"].includes(constraint.kind)) {
        const candidate = lowered.constraints[index];
        if (candidate.comparison !== inheritedComparison || candidate.key === undefined) {
          errors.push(`type:${source.name}: ${constraint.kind} comparison key did not reach operations`);
        }
      }
    }
    const fields = source.fields ?? source.body ?? [];
    for (const field of fields.filter((entry) => entry.when?.fieldPresent !== undefined)) {
      const loweredField = (lowered.fields ?? lowered.body).find((entry) => entry.name === field.name);
      const atom = loweredField.when?.all.find((entry) => entry.kind === "fieldPresent");
      if (atom?.presence?.wire?.absent !== 0 || atom?.presence?.internal?.absent !== null) {
        errors.push(`type:${source.name}.${field.name}: field presence meaning did not reach IR`);
      }
    }
    if (source.kind === "length-prefixed-text") {
      const validation = lowered.operations.find((operation) => operation.op === "text-validation");
      if (validation?.decode?.bom !== "preserve"
          || validation.decode.overlong !== "protocol-error"
          || validation.decode.surrogateCodePoint !== "protocol-error"
          || validation.encode?.loneSurrogate !== "protocol-error") {
        errors.push(`type:${source.name}: strict UTF-8 rules did not reach operations`);
      }
    }
    if (source.kind === "length-prefixed-text" || source.kind === "length-prefixed-bytes") {
      const lengthPrefixed = lowered.operations.find(
        (operation) => operation.op === "length-prefixed",
      );
      if (JSON.stringify(lengthPrefixed?.encodeCapacity)
          !== JSON.stringify(representationCapacity(lowered.maximumBytes))) {
        errors.push(`type:${source.name}: representation capacity did not reach operations`);
      }
    }
    if (source.kind === "conditional-union") {
      const union = lowered.operations.find((operation) => operation.op === "conditional-union");
      if (union?.encode?.discriminatorAgreement !== "required"
          || union.encode.mismatch !== "protocol-error") {
        errors.push(`type:${source.name}: encode discriminator agreement did not reach operations`);
      }
    }
  }
  for (const source of schema.types.filter((type) => (
    hasOwn(type, "runtimeMaximumBytes") || hasOwn(type, "runtimeMaximumEncodedBytes")
  ))) {
    const lowered = ir.types.find((type) => type.name === source.name);
    const runtimeMaximum = lowered.runtimeMaximumBytes ?? lowered.runtimeMaximumEncodedBytes;
    const negotiated = lowered.operations.find((operation) => operation.op === "negotiated-bound");
    const contextName = runtimeMaximum.clientServer.$negotiatedBound;
    const expectedContext = {
      name: contextName,
      absoluteMaximum: runtimeMaximum.clientServer.absoluteMaximum,
      missing: "protocol-error",
      negative: "protocol-error",
      aboveAbsoluteMaximum: "protocol-error",
    };
    const expectedApplications = {
      encode: { context: { kind: "encoder-context", name: contextName } },
      decode: { context: { kind: "decoder-context", name: contextName } },
    };
    if (JSON.stringify(negotiated?.context) !== JSON.stringify(expectedContext)
        || JSON.stringify(negotiated?.applications) !== JSON.stringify(expectedApplications)
        || negotiated.comparison !== "less-than-or-equal") {
      errors.push(`type:${source.name}: ClientServer negotiated bound did not reach operations`);
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
    const checksumIndex = lowered?.operations.findIndex((operation) => operation.op === "checksum");
    const bodyIndex = lowered?.operations.findIndex(
      (operation) => operation.op === "field" && operation.name === "body",
    );
    if (checksumIndex < 0 || bodyIndex <= checksumIndex
        || lowered.operations[checksumIndex].verification !== "before-body-interpretation") {
      errors.push(`durableFormat:${source.name}: checksum must precede body interpretation`);
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
    operations: seenOperations.size,
  };
}

function lowerSchema(schemaPathOrObject) {
  const schema = readSchema(schemaPathOrObject);
  validateSchema(schema);
  const model = {
    bounds: namedMap(schema.bounds, "bounds"),
    types: namedMap(schema.types, "types"),
    flags: namedMap(schema.flags, "flags"),
    protocol: schema.protocol,
    authorityKeyFormat: schema.authorityKeyFormat,
  };
  const runtimePredicates = schema.semanticConstraints
    .filter((constraint) => constraint.kind === "terminal-failure-integrity")
    .map((constraint) => ({
      reference: { asset: "service-wire-constants", name: "valid-terminal-failure" },
      targets: [...constraint.fields],
    }));
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
    semanticConstraints: schema.semanticConstraints.map((constraint) => ({
      ...structuredClone(constraint),
      ...(constraint.kind === "terminal-failure-integrity" ? {
        runtimePredicate: {
          asset: "service-wire-constants",
          name: "valid-terminal-failure",
          targets: constraint.fields.map(fieldPathOperand),
          operation: runtimePredicateOperation(runtimePredicates[0].reference, constraint.fields),
        },
      } : {}),
    })),
    relocationStateMachine: structuredClone(schema.relocationStateMachine),
    operationVocabulary: [...OPERATION_KINDS],
    types: schema.types.map((type) => lowerType(type, model, runtimePredicates)),
    commands: schema.commands.map((command) => lowerCommand(command, model, runtimePredicates)),
    durableFormats: schema.durableFormats.map(
      (format) => lowerDurableFormat(format, model, runtimePredicates),
    ),
    relocationLogicalStreamFormat: lowerLogicalStreamFormat(
      schema.relocationLogicalStreamFormat,
      model,
      runtimePredicates,
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
    all: [{
      kind: "fieldPresent",
      operand: { kind: "field", name: "actorId" },
      presence: {
        wire: {
          kind: "length-prefix-sentinel",
          lengthType: { $ref: "u8" },
          absent: 0,
          present: { minimum: 1 },
        },
        internal: { absent: null, present: "non-null" },
      },
    }],
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
  const activationRecovery = types.get("authority-activation-recovery-state");
  const activationPresent = activationRecovery.cases['{"hasActivationRecovery":"true"}'];
  assert.deepEqual(activationPresent.constraints, [{
    kind: "field-less-than-or-equal",
    left: fieldOperand("replayCursor"),
    right: fieldOperand("inboxSequence"),
  }]);
  assert.deepEqual(
    activationRecovery.operations[0].cases['{"hasActivationRecovery":"true"}'].operations.slice(-1),
    [{
      op: "constraint",
      kind: "field-less-than-or-equal",
      left: fieldOperand("replayCursor"),
      right: fieldOperand("inboxSequence"),
      owner: {
        kind: "conditional-union-case",
        path: [
          "types",
          "authority-activation-recovery-state",
          "cases",
          '{"hasActivationRecovery":"true"}',
        ],
      },
    }],
  );
  assert.deepEqual(
    types.get("generic-object-reservation-v1").operations.at(-1),
    {
      op: "encoded-limit",
      maximumEncodedBytes: 1048576,
      measured: "complete-encoded-value",
      includes: ["all-prefixes", "body"],
      applications: ["encode", "decode"],
      exceeded: "protocol-error",
    },
  );
  assert.deepEqual(
    types.get("application-payload-bytes").operations.find(
      (operation) => operation.op === "negotiated-bound",
    ),
    {
      op: "negotiated-bound",
      topology: "clientServer",
      context: {
        name: "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead",
        absoluteMaximum: 4294966774,
        missing: "protocol-error",
        negative: "protocol-error",
        aboveAbsoluteMaximum: "protocol-error",
      },
      applications: {
        encode: {
          context: {
            kind: "encoder-context",
            name: "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead",
          },
        },
        decode: {
          context: {
            kind: "decoder-context",
            name: "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead",
          },
        },
      },
      measured: "content-bytes",
      comparison: "less-than-or-equal",
    },
  );
  assert.deepEqual(
    types.get("application-payload-envelope-v1").operations.find(
      (operation) => operation.op === "negotiated-bound",
    ),
    {
      op: "negotiated-bound",
      topology: "clientServer",
      context: {
        name: "effectiveCompleteMessageBytes",
        absoluteMaximum: 4294967295,
        missing: "protocol-error",
        negative: "protocol-error",
        aboveAbsoluteMaximum: "protocol-error",
      },
      applications: {
        encode: {
          context: {
            kind: "encoder-context",
            name: "effectiveCompleteMessageBytes",
          },
        },
        decode: {
          context: {
            kind: "decoder-context",
            name: "effectiveCompleteMessageBytes",
          },
        },
      },
      measured: "encoded-bytes",
      comparison: "less-than-or-equal",
    },
  );

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
  const authorityKeyConstraint = types.get("aggregate-participant-vector").constraints[0];
  assert.equal(authorityKeyConstraint.key.kind, "canonical-authority-key-bytes");
  assert.deepEqual(authorityKeyConstraint.key.excluded, [
    "wire-length-prefixes",
    "object-generation",
    "expected-authority-owner-generation",
  ]);
  assert.deepEqual(
    types.get("sorted-text8-vector").constraints[0].key.parts,
    [{
      kind: "utf-8-bytes",
      source: { kind: "item" },
      lengthPrefix: "excluded",
    }],
  );
  assert.deepEqual(
    types.get("stateful-capability-vector").constraints[0].key.parts.map(
      (part) => [part.kind, part.encoding ?? part.lengthPrefix],
    ),
    [["wire-value", "u8"], ["utf-8-bytes", "excluded"]],
  );
  assert.deepEqual(
    types.get("saved-work-vector").constraints[0].key.parts.map(
      (part) => [part.kind, part.encoding, part.byteOrder],
    ),
    [
      ["unsigned-wire-value", "u64", "big-endian"],
      ["unsigned-wire-value", "u64", "big-endian"],
    ],
  );
  assert.deepEqual(
    types.get("aggregate-participant-vector").constraints[1].key,
    authorityKeyConstraint.key,
  );
  assert.deepEqual(types.get("metadata-frame").constraints[0].field,
    fieldPathOperand("key"));
  assert.deepEqual(creationTerminal.constraints.map((entry) => entry.kind), [
    "terminal-success-shape",
    "terminal-failure-shape",
    "existing-has-no-application-payload",
  ]);
  const descriptor = types.get("descriptor-extension");
  assert.deepEqual(
    descriptor.operations.find((operation) => operation.op === "encoded-limit"),
    {
      op: "encoded-limit",
      maximumEncodedBytes: 1048576,
      measured: "complete-encoded-value",
      includes: ["totalLength", "fields"],
      applications: ["encode", "decode"],
      exceeded: "protocol-error",
    },
  );
  assert.deepEqual(
    descriptor.fields.find((field) => field.name === "protocolCapabilities").constraints,
    [{
      kind: "contains-protocol-required-capability",
      requiredCapability: { kind: "protocol", name: "requiredCapability" },
    }],
  );
  const textValidation = types.get("text8").operations.find(
    (operation) => operation.op === "text-validation",
  );
  assert.deepEqual(textValidation.decode, {
    bom: "preserve",
    overlong: "protocol-error",
    surrogateCodePoint: "protocol-error",
  });
  assert.deepEqual(textValidation.encode, { loneSurrogate: "protocol-error" });
  assert.deepEqual(
    types.get("application-payload-bytes").operations[0].encodeCapacity,
    {
      declaredMaximumBytes: 4294966774,
      representationCeiling: 2147483647,
      requiredThroughBytes: 2147483647,
      applications: ["encode", "decode"],
      aboveDeclaredMaximum: "protocol-error",
      aboveRepresentationCeiling: "capacity-error",
    },
  );
  assert.deepEqual(
    types.get("relocation-object-identity").operations[0].encode,
    {
      selection: "variant",
      discriminatorAgreement: "required",
      mismatch: "protocol-error",
    },
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
  const { operations: authorityOperations, ...authorityDeclaration } = ir.durableFormats[0];
  assert.deepEqual(authorityDeclaration, {
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
  assert.deepEqual(authorityOperations.map((operation) => operation.op), [
    "durable-header", "bounded-reader", "encoded-limit", "checksum", "field",
  ]);
  assert.equal(authorityOperations[3].verification, "before-body-interpretation");
  assert.equal(authorityOperations[4].interpretation, "after-checksum-verification");
  assert(ir.durableFormats.every((format) => format.checksum.algorithm === "crc32c-castagnoli"
    && format.checksum.position === "trailing"));
  const { operations: logicalOperations, ...logicalDeclaration } = ir.relocationLogicalStreamFormat;
  assert.deepEqual(logicalDeclaration, {
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
  assert.deepEqual(logicalOperations.map((operation) => operation.op), [
    "logical-stream", "encoded-limit",
  ]);
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
  const terminalConstraint = ir.semanticConstraints.find(
    (constraint) => constraint.runtimePredicate,
  );
  assert.deepEqual(terminalConstraint.runtimePredicate.targets,
    schema.semanticConstraints.find(
      (constraint) => constraint.kind === "terminal-failure-integrity",
    ).fields.map(fieldPathOperand));
  assert.deepEqual(
    terminalConstraint.runtimePredicate.operation.reference,
    { asset: "service-wire-constants", name: "valid-terminal-failure" },
  );
  for (const [index, constraint] of schema.semanticConstraints.entries()) {
    const lowered = structuredClone(ir.semanticConstraints[index]);
    delete lowered.runtimePredicate;
    assert.deepEqual(lowered, constraint);
  }
  assert.deepEqual(ir.relocationStateMachine, schema.relocationStateMachine);
  const operationCoverage = assertLoweringCoverage(schema, ir);
  assert.deepEqual(ir.operationVocabulary, OPERATION_KINDS);
  assert.equal(operationCoverage.operations, OPERATION_KINDS.length);
  assert(ir.types.every((type) => type.operations.length > 0));
  assert(ir.commands.every((command) => command.operations.length > 0));
  assert(ir.durableFormats.every((format) => format.operations.length > 0));
  assert(ir.relocationLogicalStreamFormat.operations.length > 0);
  return {
    conditionForms: 4,
    validatorNegativeConditions: 4,
    conditionalUnions: irUnions.length,
    layoutConstraintKinds: 8,
    tlvPresenceRules: 1,
    durableFormats: ir.durableFormats.length,
    logicalStreams: 1,
    semanticConstraints: ir.semanticConstraints.length,
    operations: operationCoverage.operations,
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
          + `${result.logicalStreams} logical stream, `
          + `${result.semanticConstraints} semantic constraints, `
          + `${result.operations} operation kinds, JSON round-trip`,
      );
    } else {
      const schema = readSchema(schemaPath);
      const ir = lowerSchema(schema);
      const coverage = assertLoweringCoverage(schema, ir);
      console.log(
        `service wire lowering valid: ${coverage.types} types, ${coverage.commands} commands, `
          + `${coverage.kinds} kinds, ${coverage.conditionalUnions} conditional unions, `
          + `${coverage.durableFormats} durable formats, ${coverage.logicalStreams} logical stream, `
          + `${coverage.operations} operation kinds`,
      );
    }
  } catch (error) {
    printFailure(error);
    process.exit(1);
  }
}

export {
  LoweringCoverageError,
  OPERATION_KINDS,
  assertLoweringCoverage,
  lowerSchema,
};

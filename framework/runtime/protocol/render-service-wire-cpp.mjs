#!/usr/bin/env node
// SPDX-License-Identifier: FSL-1.1-ALv2

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema, OPERATION_KINDS } from "./service-wire-lowering.mjs";

const cppKeywords = new Set([
  "alignas",
  "alignof",
  "and",
  "and_eq",
  "asm",
  "auto",
  "bitand",
  "bitor",
  "bool",
  "break",
  "case",
  "catch",
  "char",
  "class",
  "compl",
  "concept",
  "const",
  "consteval",
  "constexpr",
  "constinit",
  "const_cast",
  "continue",
  "co_await",
  "co_return",
  "co_yield",
  "decltype",
  "default",
  "delete",
  "do",
  "double",
  "dynamic_cast",
  "else",
  "enum",
  "explicit",
  "export",
  "extern",
  "false",
  "float",
  "for",
  "friend",
  "goto",
  "if",
  "inline",
  "int",
  "long",
  "mutable",
  "namespace",
  "new",
  "noexcept",
  "not",
  "not_eq",
  "nullptr",
  "operator",
  "or",
  "or_eq",
  "private",
  "protected",
  "public",
  "register",
  "reinterpret_cast",
  "requires",
  "return",
  "short",
  "signed",
  "sizeof",
  "static",
  "static_assert",
  "static_cast",
  "struct",
  "switch",
  "template",
  "this",
  "thread_local",
  "throw",
  "true",
  "try",
  "typedef",
  "typeid",
  "typename",
  "union",
  "unsigned",
  "using",
  "virtual",
  "void",
  "volatile",
  "wchar_t",
  "while",
  "xor",
  "xor_eq",
]);
const identifier = (name) => {
  const value = name.replaceAll(/[^A-Za-z0-9_]/g, "_");
  return cppKeywords.has(value) ? `${value}_` : value;
};
const typeName = (reference) => `${identifier(reference.$ref)}_t`;
const internalDecode = (reference) =>
  `decode_value_${identifier(reference.$ref)}`;
const internalEncode = (reference) =>
  `encode_value_${identifier(reference.$ref)}`;
const unsignedLiteral = (value) => `${value}ull`;
const signedLiteral = (value) =>
  String(value) === "-9223372036854775808"
    ? "std::numeric_limits<std::int64_t>::min()"
    : `${value}ll`;

let types = new Map();
let flags = new Map();
let decoderContextFields = [];

function primaryOperation(owner) {
  if (!Array.isArray(owner.operations) || owner.operations.length === 0) {
    throw new Error(`${owner.name}: missing operation sequence`);
  }
  return owner.operations[0];
}

function enumValue(reference, name) {
  const declaration = types.get(reference.$ref);
  const operation = declaration?.operations.find(
    (entry) => entry.op === "enum",
  );
  const value = operation?.values.find((entry) => entry.name === name);
  if (!value) throw new Error(`${reference.$ref}: unknown enum value ${name}`);
  return `${typeName(reference)}::${identifier(value.name)}`;
}

function scalarCppType(operation) {
  return {
    u8: "std::uint8_t",
    u16: "std::uint16_t",
    u32: "std::uint32_t",
    u64: "std::uint64_t",
    i64: "std::int64_t",
  }[operation.encoding];
}

function valueCppType(reference) {
  return scalarCppType(primaryOperation(types.get(reference.$ref)));
}

function externalDiscriminators(owner) {
  const result = new Map();
  const visiting = new Set();
  function inspect(current, root = false) {
    if (visiting.has(current.name)) return;
    visiting.add(current.name);
    const operation = primaryOperation(current);
    if (operation.op === "conditional-union") {
      for (const discriminator of operation.discriminators) {
        if (
          discriminator.source.kind === "context" ||
          (root && discriminator.source.kind === "enclosingField")
        ) {
          result.set(discriminator.name, discriminator.type);
        }
      }
    }
    const visitField = (field) => inspect(types.get(field.type.$ref), false);
    if (
      ["struct", "versioned-length-delimited", "tlv32"].includes(operation.op)
    ) {
      operation.fields.forEach(visitField);
    } else if (["vector", "versioned-vector"].includes(operation.op)) {
      const reference =
        operation.op === "vector"
          ? operation.item
          : operation.layout.find((entry) => entry.kind === "repeat").item;
      inspect(types.get(reference.$ref), false);
    } else if (operation.op === "conditional-union") {
      Object.values(operation.cases).forEach((entry) =>
        entry.operations
          .filter((caseOperation) => caseOperation.op === "field")
          .forEach(visitField),
      );
      if (operation.otherwise.kind === "fields") {
        (operation.otherwise.operations ?? operation.otherwise.fields)
          .filter((caseOperation) => caseOperation.op === "field")
          .forEach(visitField);
      }
    }
    visiting.delete(current.name);
  }
  inspect(owner, true);
  return [...result].map(([name, reference]) => ({ name, type: reference }));
}

function parameters(owner, prefix = ", ") {
  return externalDiscriminators(owner)
    .map(
      (entry) => `${prefix}${typeName(entry.type)} ${identifier(entry.name)}`,
    )
    .join("");
}

function parameterArguments(owner, prefix = ", ") {
  return externalDiscriminators(owner)
    .map((entry) => `${prefix}${identifier(entry.name)}`)
    .join("");
}

function negotiatedBounds(owner) {
  const result = new Map();
  const visiting = new Set();
  function inspect(current) {
    if (!current || visiting.has(current)) return;
    visiting.add(current);
    for (const operation of current.operations) {
      if (operation.op === "negotiated-bound") {
        result.set(operation.context.name, operation);
      }
    }
    for (const reference of operationReferences(current.operations)) {
      inspect(types.get(reference));
    }
    visiting.delete(current);
  }
  inspect(owner);
  return [...result.values()];
}

function decodeParameters(owner) {
  return `${parameters(owner)}${
    negotiatedBounds(owner).length === 0
      ? ""
      : ", const codec_context_t& context"
  }`;
}

function decodeParameterArguments(owner) {
  return `${parameterArguments(owner)}${negotiatedBounds(owner).length === 0 ? "" : ", context"}`;
}

function encodeParameters(owner) {
  return `${parameters(owner)}${
    negotiatedBounds(owner).length === 0
      ? ""
      : ", const codec_context_t& context"
  }`;
}

function encodeParameterArguments(owner) {
  return `${parameterArguments(owner)}${negotiatedBounds(owner).length === 0 ? "" : ", context"}`;
}

function fieldArguments(field, value) {
  const child = types.get(field.type.$ref);
  const operation = primaryOperation(child);
  if (operation.op !== "conditional-union") return parameterArguments(child);
  const arguments_ = [];
  for (const discriminator of operation.discriminators) {
    if (discriminator.source.kind === "enclosingField") {
      arguments_.push(`${value}.${identifier(discriminator.source.name)}`);
    } else if (discriminator.source.kind === "context") {
      arguments_.push(identifier(discriminator.source.name));
    }
  }
  return arguments_.map((entry) => `, ${entry}`).join("");
}

function fieldDecodeArguments(field, value) {
  const child = types.get(field.type.$ref);
  return `${fieldArguments(field, value)}${negotiatedBounds(child).length === 0 ? "" : ", context"}`;
}

function fieldEncodeArguments(field, value) {
  const child = types.get(field.type.$ref);
  return `${fieldArguments(field, value)}${negotiatedBounds(child).length === 0 ? "" : ", context"}`;
}

function fieldDeclaration(field) {
  const valueType = typeName(field.type);
  return `    ${field.when || field.required === false ? `std::optional<${valueType}>` : valueType} ${identifier(field.name)}{};`;
}

function fieldsDto(fields) {
  return fields.map(fieldDeclaration).join("\n");
}

function conditionalCases(operation) {
  const result = Object.entries(operation.cases).map(
    ([signature, entry], index) => ({
      index,
      signature: JSON.parse(signature),
      operations: entry.operations,
      fields: entry.operations.filter((operation) => operation.op === "field"),
      name: `case_${index}_t`,
      tag: `case_${index}`,
    }),
  );
  if (operation.otherwise.kind === "fields") {
    const operations =
      operation.otherwise.operations ?? operation.otherwise.fields;
    result.push({
      index: result.length,
      signature: null,
      operations,
      fields: operations.filter((operation) => operation.op === "field"),
      name: "otherwise_t",
      tag: "otherwise",
    });
  }
  return result;
}

function dtoInteger(owner, operation) {
  return `struct ${identifier(owner.name)}_t {\n    ${scalarCppType(operation)} value{};\n    bool operator==(const ${identifier(owner.name)}_t&) const = default;\n};`;
}

function dtoEnum(owner, operation) {
  return `enum class ${identifier(owner.name)}_t : ${scalarCppType(operation)} {\n${operation.values
    .map((entry) => `    ${identifier(entry.name)} = ${entry.value},`)
    .join("\n")}\n};`;
}

function dtoLengthPrefixed(owner, operation) {
  const value =
    operation.content === "text" ? "std::string" : "std::vector<std::uint8_t>";
  const semanticValue =
    operation.zeroLengthMeaning === "absent"
      ? `std::optional<${value}>`
      : value;
  return `struct ${identifier(owner.name)}_t {\n    ${semanticValue} value;\n    bool operator==(const ${identifier(owner.name)}_t&) const = default;\n};`;
}

function dtoFields(owner, operation) {
  return `struct ${identifier(owner.name)}_t {\n${fieldsDto(operation.fields)}\n    bool operator==(const ${identifier(owner.name)}_t&) const = default;\n};`;
}

function dtoVector(owner, operation) {
  const item =
    operation.op === "vector"
      ? operation.item
      : operation.layout.find((entry) => entry.kind === "repeat").item;
  return `struct ${identifier(owner.name)}_t {\n    std::vector<${typeName(item)}> items;\n    bool operator==(const ${identifier(owner.name)}_t&) const = default;\n};`;
}

function dtoConditional(owner, operation) {
  const n = identifier(owner.name);
  const cases = conditionalCases(operation);
  const wire = operation.discriminators.filter(
    (entry) => entry.source.kind === "wire",
  );
  return `struct ${n}_t {\n${wire.map((entry) => `    ${typeName(entry.type)} ${identifier(entry.name)}{};`).join("\n")}
${cases.map((entry) => `    struct ${entry.name} {\n${fieldsDto(entry.fields)}\n        bool operator==(const ${entry.name}&) const = default;\n    };`).join("\n")}
    enum class tag_t : std::uint8_t { ${cases.map((entry) => entry.tag).join(", ")} };
    tag_t tag{tag_t::${cases[0].tag}};
    std::variant<${cases.map((entry) => entry.name).join(", ")}> value{${cases[0].name}{}};
    bool operator==(const ${n}_t&) const = default;
};`;
}

function dtoTlv(owner, operation) {
  const fields = operation.fields
    .map(
      (field) =>
        `    std::optional<${typeName(field.type)}> ${identifier(field.name)}{};`,
    )
    .join("\n");
  return `struct ${identifier(owner.name)}_t {\n${fields}
    std::vector<unknown_tlv_field_t> unknownFields;
    bool operator==(const ${identifier(owner.name)}_t&) const = default;
};`;
}

const dtoEmitters = new Map([
  ["integer", dtoInteger],
  ["enum", dtoEnum],
  ["length-prefixed", dtoLengthPrefixed],
  ["struct", dtoFields],
  ["vector", dtoVector],
  ["versioned-vector", dtoVector],
  ["versioned-length-delimited", dtoFields],
  ["conditional-union", dtoConditional],
  ["tlv32", dtoTlv],
]);

function emitDto(owner) {
  const operation = primaryOperation(owner);
  const emitter = dtoEmitters.get(operation.op);
  if (!emitter)
    throw new Error(
      `${owner.name}: operation ${operation.op} cannot declare a DTO`,
    );
  return emitter(owner, operation);
}

function operationReferences(operation) {
  const result = new Set();
  function visit(value) {
    if (Array.isArray(value)) value.forEach(visit);
    else if (value && typeof value === "object") {
      if (typeof value.$ref === "string") result.add(value.$ref);
      Object.values(value).forEach(visit);
    }
  }
  visit(operation);
  return result;
}

function orderedTypes(ir) {
  const result = [];
  const done = new Set();
  const active = new Set();
  function visit(owner) {
    if (done.has(owner.name)) return;
    if (active.has(owner.name))
      throw new Error(`recursive generated DTO dependency: ${owner.name}`);
    active.add(owner.name);
    for (const reference of operationReferences(owner.operations)) {
      if (reference !== owner.name) visit(types.get(reference));
    }
    active.delete(owner.name);
    done.add(owner.name);
    result.push(owner);
  }
  ir.types.forEach(visit);
  return result;
}

function numericExpression(expression, reference) {
  const operation = primaryOperation(types.get(reference.$ref));
  if (operation.op === "integer") return `${expression}.value`;
  if (operation.op === "enum")
    return `static_cast<${scalarCppType(operation)}>(${expression})`;
  throw new Error(
    `${reference.$ref}: numeric operation required, got ${operation.op}`,
  );
}

function expectedExpression(reference, value) {
  const operation = primaryOperation(types.get(reference.$ref));
  return operation.op === "enum"
    ? enumValue(reference, value)
    : `${typeName(reference)}{${value}}`;
}

function fieldsOf(owner) {
  const operation = primaryOperation(owner);
  if (
    ["struct", "versioned-length-delimited", "tlv32"].includes(operation.op)
  ) {
    return operation.fields;
  }
  return [];
}

function findField(owner, name) {
  const field = fieldsOf(owner).find((entry) => entry.name === name);
  if (!field) throw new Error(`${owner.name}: unknown field ${name}`);
  return field;
}

function conditionExpression(
  condition,
  owner,
  value,
  flagsName = "flagsValue",
) {
  if (!condition) return "true";
  return (
    condition.all
      .map((atom) => {
        if (atom.kind === "fieldPresent") {
          const field = findField(owner, atom.operand.name);
          const member = `${value}.${identifier(atom.operand.name)}`;
          const operation = primaryOperation(types.get(field.type.$ref));
          if (
            atom.presence.internal.absent !== null ||
            atom.presence.internal.present !== "non-null" ||
            atom.presence.wire.kind !== "length-prefix-sentinel" ||
            atom.presence.wire.absent !== 0 ||
            atom.presence.wire.present.minimum !== 1
          ) {
            throw new Error(
              `${owner.name}.${field.name}: unsupported field presence operation`,
            );
          }
          if (field.when || field.required === false)
            return `${member}.has_value()`;
          if (
            operation.op === "length-prefixed" &&
            operation.zeroLengthMeaning === "absent"
          ) {
            return `${member}.value.has_value()`;
          }
          throw new Error(
            `${owner.name}.${field.name}: fieldPresent requires semantic presence`,
          );
        }
        if (atom.kind === "fieldEquals") {
          const field = findField(owner, atom.operand.name);
          const member = `${value}.${identifier(field.name)}`;
          return `${member} == ${expectedExpression(field.type, atom.value)}`;
        }
        if (atom.kind === "allFlagsSet" || atom.kind === "anyFlagsSet") {
          const checks = atom.operands.map(
            (entry) => `((${flagsName}) & ${flags.get(entry.name)}u) != 0u`,
          );
          return `(${checks.join(atom.kind === "allFlagsSet" ? " && " : " || ")})`;
        }
        if (atom.kind === "contextEquals") {
          return `${identifier(atom.operand.name)} == ${atom.value}`;
        }
        throw new Error(
          `${owner.name}: unsupported condition atom ${atom.kind}`,
        );
      })
      .join(" && ") || "true"
  );
}

function fieldRangeChecks(field, expression) {
  const checks = [];
  if (field.constant !== undefined) {
    const expected =
      typeof field.constant === "string"
        ? expectedExpression(field.type, field.constant)
        : `${typeName(field.type)}{${field.constant}}`;
    checks.push(
      `if (!(${expression} == ${expected})) return error_code::constant;`,
    );
  }
  if (field.minimum !== undefined) {
    checks.push(
      `if (${numericExpression(expression, field.type)} < ${unsignedLiteral(field.minimum)}) return error_code::range;`,
    );
  }
  if (field.maximum !== undefined) {
    checks.push(
      `if (${numericExpression(expression, field.type)} > ${unsignedLiteral(field.maximum)}) return error_code::range;`,
    );
  }
  return checks.join("\n");
}

function emitFieldDecode(
  field,
  owner,
  value = "out",
  reader = "reader",
  flagsName = "flagsValue",
) {
  const member = `${value}.${identifier(field.name)}`;
  const optional = field.when || field.required === false;
  const target = optional ? `*${member}` : member;
  const decode = `if (const auto error = ${internalDecode(field.type)}(${reader}, ${target}${fieldDecodeArguments(field, value)}); error != error_code::ok) return error;`;
  const checks = fieldRangeChecks(field, target);
  if (field.when) {
    const guard = conditionExpression(field.when, owner, value, flagsName);
    return `if (${guard}) {\n${member}.emplace();\n${decode}\n${checks}\n} else {\n${member}.reset();\n}`;
  }
  if (field.required === false) {
    throw new Error(
      `${owner.name}.${field.name}: optional field requires an enclosing TLV operation`,
    );
  }
  return `${decode}\n${checks}`;
}

function emitFieldEncode(
  field,
  owner,
  value = "value",
  writer = "writer",
  flagsName = "flagsValue",
) {
  const member = `${value}.${identifier(field.name)}`;
  const target = field.when || field.required === false ? `*${member}` : member;
  const encode = `if (const auto error = ${internalEncode(field.type)}(${writer}, ${target}${fieldEncodeArguments(field, value)}); error != error_code::ok) return error;`;
  const checks = fieldRangeChecks(field, target);
  if (field.when) {
    const guard = conditionExpression(field.when, owner, value, flagsName);
    return `if (${guard}) {\nif (!${member}) return error_code::required;\n${checks}\n${encode}\n} else if (${member}) {\nreturn error_code::forbidden;\n}`;
  }
  if (field.required === false)
    return `if (${member}) {\n${checks}\n${encode}\n}`;
  return `${checks}\n${encode}`;
}

function pathInfo(owner, path, root) {
  const parts = path.split(".");
  let currentOwner = owner;
  let expression = root;
  let reference = null;
  let optional = false;
  for (const part of parts) {
    const operation = primaryOperation(currentOwner);
    if (operation.op === "conditional-union") {
      const discriminator = operation.discriminators.find(
        (entry) => entry.name === part,
      );
      if (!discriminator)
        throw new Error(`${owner.name}: unsupported conditional path ${path}`);
      expression += `${optional ? "->" : "."}${identifier(part)}`;
      reference = discriminator.type;
      currentOwner = types.get(reference.$ref);
      optional = false;
      continue;
    }
    const field = operation.fields?.find((entry) => entry.name === part);
    if (!field) throw new Error(`${owner.name}: unknown path ${path}`);
    expression += `${optional ? "->" : "."}${identifier(part)}`;
    reference = field.type;
    optional = Boolean(field.when || field.required === false);
    currentOwner = types.get(reference.$ref);
  }
  return { expression, reference, optional };
}

function constraintSourceInfo(itemOwner, source, item) {
  if (source.kind === "item")
    return { expression: item, reference: { $ref: itemOwner.name } };
  if (source.kind === "fieldPath")
    return pathInfo(itemOwner, source.path, item);
  throw new Error(
    `${itemOwner.name}: unsupported constraint key source ${source.kind}`,
  );
}

function canonicalAuthorityKeyExpression(info, key) {
  const unionOwner = types.get(info.reference.$ref);
  const operation = primaryOperation(unionOwner);
  if (
    operation.op !== "conditional-union" ||
    key.format.encoding !== "canonical-ascii-utf8" ||
    key.format.componentLayout !==
      "decimal-raw-byte-length-colon-percent-encoded-bytes"
  ) {
    throw new Error(`${unionOwner.name}: unsupported canonical authority key`);
  }
  const cases = conditionalCases(operation);
  const branches = Object.entries(key.variants)
    .map(([variantName, variant]) => {
      const entry = cases.find(
        (candidate) =>
          candidate.signature &&
          Object.values(candidate.signature).includes(variantName),
      );
      const discriminator = key.format.kindDiscriminators.find(
        (candidate) => candidate.objectKind === variant.objectKind,
      );
      if (!entry || !discriminator)
        throw new Error(
          `${unionOwner.name}: authority key variant ${variantName} missing`,
        );
      const caseOwner = {
        name: `${unionOwner.name}.${entry.name}`,
        operations: [
          {
            op: "struct",
            fields: entry.fields,
            constraints: [],
          },
        ],
      };
      const components = variant.components
        .map((path) => {
          const component = pathInfo(caseOwner, path, "selected");
          const componentOperation = primaryOperation(
            types.get(component.reference.$ref),
          );
          if (
            componentOperation.op !== "length-prefixed" ||
            componentOperation.content !== "text"
          ) {
            throw new Error(
              `${unionOwner.name}: authority key component ${path} is not text`,
            );
          }
          return `key.push_back(${JSON.stringify(key.format.separator)}[0]);\nappendCanonicalAuthorityComponent(key, ${component.expression}.value);`;
        })
        .join("\n");
      return `case ${typeName({ $ref: unionOwner.name })}::tag_t::${entry.tag}: {\nconst auto& selected = std::get<${typeName({ $ref: unionOwner.name })}::${entry.name}>(authority.value);\nstd::string key = ${JSON.stringify(`${key.format.prefix}${key.format.separator}${discriminator.wire}`)};\n${components}\nreturn key;\n}`;
    })
    .join("\n");
  return `[&]() -> std::string {\nconst auto& authority = ${info.expression};\nswitch (authority.tag) {\n${branches}\n}\nreturn {};\n}()`;
}

function constraintKeyParts(key, itemOwner, item) {
  if (key.kind === "tuple")
    return key.parts.flatMap((part) =>
      constraintKeyParts(part, itemOwner, item),
    );
  const info = constraintSourceInfo(itemOwner, key.source, item);
  if (key.kind === "utf-8-bytes") {
    if (key.lengthPrefix !== "excluded")
      throw new Error(`${itemOwner.name}: UTF-8 key includes length prefix`);
    return [`${info.expression}.value`];
  }
  if (key.kind === "wire-value" || key.kind === "unsigned-wire-value") {
    return [numericExpression(info.expression, info.reference)];
  }
  if (key.kind === "canonical-authority-key-bytes") {
    return [canonicalAuthorityKeyExpression(info, key)];
  }
  throw new Error(`${itemOwner.name}: unsupported constraint key ${key.kind}`);
}

function vectorConstraint(operation, owner) {
  const lines = [];
  const consumed = new Set();
  const itemReference =
    operation.op === "vector"
      ? operation.item
      : operation.layout.find((part) => part.kind === "repeat").item;
  const itemOwner = types.get(itemReference.$ref);
  for (
    let constraintIndex = 0;
    constraintIndex < operation.constraints.length;
    ++constraintIndex
  ) {
    if (consumed.has(constraintIndex)) continue;
    const constraint = operation.constraints[constraintIndex];
    const left = constraintKeyParts(
      constraint.key,
      itemOwner,
      "value.items[left]",
    );
    const right = constraintKeyParts(
      constraint.key,
      itemOwner,
      "value.items[right]",
    );
    const less = left
      .map((entry, index) => {
        const equalPrefix = left
          .slice(0, index)
          .map((prior, priorIndex) => `${prior} == ${right[priorIndex]}`)
          .join(" && ");
        return `${equalPrefix ? `${equalPrefix} && ` : ""}${entry} < ${right[index]}`;
      })
      .join(" || ");
    const equal = left
      .map((entry, index) => `${entry} == ${right[index]}`)
      .join(" && ");
    if (constraint.kind === "sorted") {
      const uniqueIndex = operation.constraints.findIndex(
        (candidate, candidateIndex) =>
          candidateIndex !== constraintIndex &&
          candidate.kind === "unique" &&
          JSON.stringify(candidate.key) === JSON.stringify(constraint.key),
      );
      if (uniqueIndex >= 0) consumed.add(uniqueIndex);
      lines.push(
        `try {\nfor (std::size_t right = 1; right < value.items.size(); ++right) {\nconst auto left = right - 1;\n${uniqueIndex >= 0 ? `if (${equal}) return error_code::duplicate;\n` : ""}if (!(${less}) && !(${equal})) return error_code::order;\n}\n} catch (const std::bad_alloc&) { return error_code::capacity; } catch (const std::length_error&) { return error_code::capacity; }`,
      );
    } else if (constraint.kind === "unique") {
      lines.push(
        `try {\nfor (std::size_t left = 0; left < value.items.size(); ++left)\nfor (std::size_t right = left + 1; right < value.items.size(); ++right)\nif (${equal}) return error_code::duplicate;\n} catch (const std::bad_alloc&) { return error_code::capacity; } catch (const std::length_error&) { return error_code::capacity; }`,
      );
    } else {
      throw new Error(
        `${owner.name}: unsupported vector constraint ${constraint.kind}`,
      );
    }
  }
  return lines.join("\n");
}

function aggregateConstraint(constraint, owner, value) {
  if (constraint.kind === "not-both-zero") {
    return `if (${constraint.fields
      .map(
        (entry) =>
          `${numericExpression(`${value}.${identifier(entry.name)}`, findField(owner, entry.name).type)} == 0`,
      )
      .join(" && ")}) return error_code::constraint;`;
  }
  if (constraint.kind === "field-less-than-or-equal") {
    const left = findField(owner, constraint.left.name);
    const right = findField(owner, constraint.right.name);
    return `if (${numericExpression(`${value}.${identifier(left.name)}`, left.type)} > ${numericExpression(`${value}.${identifier(right.name)}`, right.type)}) return error_code::constraint;`;
  }
  if (
    constraint.kind === "terminal-success-shape" ||
    constraint.kind === "terminal-failure-shape" ||
    constraint.kind === "existing-has-no-application-payload"
  ) {
    const tests = [];
    for (const [path, expected] of Object.entries(constraint.when)) {
      if (path.endsWith("Not")) {
        const actual = path.slice(0, -3);
        const field = findField(owner, actual);
        tests.push(
          `${value}.${identifier(actual)} != ${expectedExpression(field.type, expected)}`,
        );
      } else {
        const info = pathInfo(owner, path, value);
        const guard = path.includes(".")
          ? `${value}.${identifier(path.split(".")[0])}.has_value() && `
          : "";
        tests.push(
          `${guard}${info.expression} == ${expectedExpression(info.reference, expected)}`,
        );
      }
    }
    const requirements = Object.entries(constraint.requires).map(
      ([path, expected]) => {
        const info = pathInfo(owner, path, value);
        return `${info.expression} == ${expectedExpression(info.reference, expected)}`;
      },
    );
    return `if (${tests.join(" && ")} && !(${requirements.join(" && ")})) return error_code::constraint;`;
  }
  throw new Error(
    `${owner.name}: unsupported aggregate constraint ${constraint.kind}`,
  );
}

function aggregateConstraints(operation, owner, value) {
  return operation.constraints
    .map((entry) => aggregateConstraint(entry, owner, value))
    .join("\n");
}

function runtimePredicate(owner, operation, value) {
  if (
    operation.reference.asset !== "service-wire-constants" ||
    operation.reference.name !== "valid-terminal-failure"
  ) {
    throw new Error(
      `${owner.name}: unsupported runtime predicate ${JSON.stringify(operation.reference)}`,
    );
  }
  const target = operation.targets[0].path.split(".").slice(1);
  if (
    target.length === 1 ||
    primaryOperation(owner).op !== "conditional-union"
  ) {
    const failure = findField(owner, target.at(-1));
    const terminal = findField(owner, "terminalResult");
    return `if (!::zlink::framework::runtime::protocol::valid_terminal_failure(\nstatic_cast<std::uint32_t>(${value}.${identifier(terminal.name)}),\nstatic_cast<::zlink::framework::runtime::protocol::framework_error_code>(${value}.${identifier(failure.name)}))) return error_code::predicate;`;
  }
  const primary = primaryOperation(owner);
  if (primary.op !== "conditional-union")
    throw new Error(`${owner.name}: nested predicate target is not a union`);
  const caseEntry = conditionalCases(primary).find(
    (entry) =>
      entry.signature && Object.values(entry.signature).includes(target[0]),
  );
  if (!caseEntry)
    throw new Error(`${owner.name}: predicate case ${target[0]} missing`);
  const terminal = caseEntry.fields.find(
    (entry) => entry.name === "terminalResult",
  );
  const failure = caseEntry.fields.find((entry) => entry.name === target[1]);
  return `if (${value}.tag == ${identifier(owner.name)}_t::tag_t::${caseEntry.tag}) {\nconst auto& predicateValue = std::get<${identifier(owner.name)}_t::${caseEntry.name}>(${value}.value);\nif (!::zlink::framework::runtime::protocol::valid_terminal_failure(\nstatic_cast<std::uint32_t>(predicateValue.${identifier(terminal.name)}),\nstatic_cast<::zlink::framework::runtime::protocol::framework_error_code>(predicateValue.${identifier(failure.name)}))) return error_code::predicate;\n}`;
}

function trailingRuntimePredicates(owner, value) {
  return owner.operations
    .filter((entry) => entry.op === "runtime-predicate")
    .map((entry) => runtimePredicate(owner, entry, value))
    .join("\n");
}

function negotiatedBoundCheck(owner, operation, application, measured) {
  const applied = operation.applications[application];
  if (
    operation.topology !== "clientServer" ||
    operation.comparison !== "less-than-or-equal" ||
    operation.context.missing !== "protocol-error" ||
    operation.context.negative !== "protocol-error" ||
    operation.context.aboveAbsoluteMaximum !== "protocol-error" ||
    applied.context.name !== operation.context.name
  ) {
    throw new Error(`${owner.name}: unsupported negotiated-bound operation`);
  }
  const context = `context.${identifier(applied.context.name)}`;
  return `if (!${context} || *${context} < 0 || static_cast<std::uint64_t>(*${context}) > ${unsignedLiteral(operation.context.absoluteMaximum)}) return error_code::context;\nif (${measured} > static_cast<std::uint64_t>(*${context})) return error_code::limit;`;
}

function negotiatedBoundChecks(owner, application, measuredByKind) {
  return owner.operations
    .filter((entry) => entry.op === "negotiated-bound")
    .map((operation) => {
      const measured = measuredByKind[operation.measured];
      if (measured === undefined) {
        throw new Error(
          `${owner.name}: unsupported negotiated-bound measurement ${operation.measured}`,
        );
      }
      return negotiatedBoundCheck(owner, operation, application, measured);
    })
    .join("\n");
}

function emitIntegerCodec(owner, operation) {
  const n = identifier(owner.name);
  const signed = operation.encoding === "i64";
  const minimum = signed
    ? signedLiteral(operation.minimum)
    : unsignedLiteral(operation.minimum);
  const maximum = signed
    ? signedLiteral(operation.maximum)
    : unsignedLiteral(operation.maximum);
  const read = signed
    ? `std::int64_t raw{};\nif (const auto error = reader.signedInteger(${operation.width}, raw); error != error_code::ok) return error;`
    : `std::uint64_t raw{};\nif (const auto error = reader.unsignedInteger(${operation.width}, raw); error != error_code::ok) return error;`;
  const write = signed
    ? `return writer.signedInteger(${operation.width}, value.value);`
    : `return writer.unsignedInteger(${operation.width}, value.value);`;
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out) {\n${read}\nif (raw < ${minimum} || raw > ${maximum}) return error_code::range;\nout.value = static_cast<${scalarCppType(operation)}>(raw);\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value) {\nif (value.value < ${minimum} || value.value > ${maximum}) return error_code::range;\n${write}\n}`;
}

function emitEnumCodec(owner, operation) {
  const n = identifier(owner.name);
  const cases = operation.values
    .map((entry) => `case ${entry.value}:`)
    .join("\n");
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out) {\nstd::uint64_t raw{};\nif (const auto error = reader.unsignedInteger(${operation.width}, raw); error != error_code::ok) return error;\nswitch (raw) {\n${cases}\nout = static_cast<${n}_t>(raw);\nreturn error_code::ok;\ndefault:\nreturn error_code::enum_value;\n}\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value) {\nconst auto raw = static_cast<std::uint64_t>(value);\nswitch (raw) {\n${cases}\nreturn writer.unsignedInteger(${operation.width}, raw);\ndefault:\nreturn error_code::enum_value;\n}\n}`;
}

function emitLengthCodec(owner, operation) {
  const n = identifier(owner.name);
  const validateText = owner.operations.find(
    (entry) => entry.op === "text-validation",
  );
  const capacity = operation.encodeCapacity;
  if (
    capacity.declaredMaximumBytes !== operation.maximumBytes ||
    typeof capacity.representationCeiling !== "number" ||
    typeof capacity.requiredThroughBytes !== "number" ||
    capacity.applications.length !== 2 ||
    !capacity.applications.includes("encode") ||
    !capacity.applications.includes("decode") ||
    capacity.aboveDeclaredMaximum !== "protocol-error" ||
    capacity.aboveRepresentationCeiling !== "capacity-error"
  ) {
    throw new Error(
      `${owner.name}: unsupported length-prefixed encode capacity`,
    );
  }
  if (
    validateText &&
    (validateText.encoding !== "utf-8" ||
      validateText.malformed !== "protocol-error" ||
      validateText.decode.bom !== "preserve" ||
      validateText.decode.overlong !== "protocol-error" ||
      validateText.decode.surrogateCodePoint !== "protocol-error" ||
      validateText.encode.loneSurrogate !== "protocol-error")
  ) {
    throw new Error(`${owner.name}: unsupported text validation operation`);
  }
  const absent = operation.zeroLengthMeaning === "absent";
  const decodedValue = absent ? "(*out.value)" : "out.value";
  const encodedValue = absent ? "(*value.value)" : "value.value";
  const textCheck = validateText
    ? `if (${absent ? "value.value && " : ""}!validUtf8(${encodedValue}, ${validateText.nul === "forbidden" ? "true" : "false"})) return error_code::utf8;`
    : "";
  const assign =
    operation.content === "text"
      ? `${absent ? "out.value.emplace();" : ""}${decodedValue}.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());`
      : `${absent ? "out.value.emplace();" : ""}${decodedValue}.assign(bytes.begin(), bytes.end());`;
  const byteView =
    operation.content === "text"
      ? `std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(${encodedValue}.data()), ${encodedValue}.size())`
      : `std::span<const std::uint8_t>(${encodedValue})`;
  const decodeNegotiated = negotiatedBoundChecks(owner, "decode", {
    "content-bytes": "size",
  });
  const encodeNegotiated = negotiatedBoundChecks(owner, "encode", {
    "content-bytes": "size",
  });
  const lengthWidth = primaryOperation(
    types.get(operation.lengthType.$ref),
  ).width;
  const capacityCheck = `if (size > ${unsignedLiteral(capacity.requiredThroughBytes)}) {\nif (size > ${unsignedLiteral(capacity.representationCeiling)}) return error_code::capacity;\nif (size > ${unsignedLiteral(capacity.declaredMaximumBytes)}) return error_code::range;\n}`;
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\n${typeName(operation.lengthType)} length{};\nif (const auto error = ${internalDecode(operation.lengthType)}(reader, length); error != error_code::ok) return error;\nconst auto size = static_cast<std::uint64_t>(length.value);\nif (size < ${unsignedLiteral(operation.minimumBytes)}) return error_code::range;\n${capacityCheck}\n${decodeNegotiated}\n${absent ? "if (size == 0) { out.value.reset(); return error_code::ok; }" : ""}\nstd::span<const std::uint8_t> bytes;\nif (const auto error = reader.take(size, bytes); error != error_code::ok) return error;\n${assign}\n${validateText ? `if (!validUtf8(${decodedValue}, ${validateText.nul === "forbidden" ? "true" : "false"})) return error_code::utf8;` : ""}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto size = static_cast<std::uint64_t>(${absent ? "value.value ? value.value->size() : 0" : "value.value.size()"});\nif (size < ${unsignedLiteral(operation.minimumBytes)}) return error_code::range;\n${capacityCheck}\n${encodeNegotiated}\n${textCheck}\nwriter.data.reserve(writer.data.size() + ${lengthWidth}u + static_cast<std::size_t>(size));\n${typeName(operation.lengthType)} length{static_cast<${valueCppType(operation.lengthType)}>(size)};\nif (const auto error = ${internalEncode(operation.lengthType)}(writer, length); error != error_code::ok) return error;\n${absent ? "if (!value.value) return error_code::ok;" : ""}\nwriter.bytes(${byteView});\nreturn error_code::ok;\n}`;
}

function emitStructCodec(owner, operation) {
  const n = identifier(owner.name);
  const decode = operation.fields
    .map((field) => emitFieldDecode(field, owner))
    .join("\n");
  const encode = operation.fields
    .map((field) => emitFieldEncode(field, owner))
    .join("\n");
  const constraints = aggregateConstraints(operation, owner, "out");
  const encodeConstraints = aggregateConstraints(operation, owner, "value");
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\nconst auto encodedStart = reader.position();\n${decode}\n${constraints}\n${trailingRuntimePredicates(owner, "out")}\n${encodedLimitCheck(owner, "reader.position() - encodedStart", "decode")}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto encodedStart = writer.data.size();\n${encodeConstraints}\n${trailingRuntimePredicates(owner, "value")}\n${encode}\n${encodedLimitCheck(owner, "writer.data.size() - encodedStart", "encode")}\nreturn error_code::ok;\n}`;
}

function emitVectorCodec(owner, operation) {
  const n = identifier(owner.name);
  const item = operation.item;
  const maximum =
    operation.maximumItems === undefined
      ? ""
      : `if (count > ${unsignedLiteral(operation.maximumItems)}) return error_code::range;`;
  const constraints = vectorConstraint(operation, owner);
  return `inline error_code validate_constraints_${n}(const ${n}_t& value) {\n${constraints}\nreturn error_code::ok;\n}\ninline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\n${typeName(operation.countType)} countValue{};\nif (const auto error = ${internalDecode(operation.countType)}(reader, countValue); error != error_code::ok) return error;\nconst auto count = static_cast<std::uint64_t>(countValue.value);\n${maximum}\nout.items.clear();\nout.items.reserve(static_cast<std::size_t>(count));\nfor (std::uint64_t index = 0; index < count; ++index) {\n${typeName(item)} item{};\nif (const auto error = ${internalDecode(item)}(reader, item${decodeParameterArguments(types.get(item.$ref))}); error != error_code::ok) return error;\nout.items.push_back(std::move(item));\n}\nif (const auto error = validate_constraints_${n}(out); error != error_code::ok) return error;\n${trailingRuntimePredicates(owner, "out")}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto count = static_cast<std::uint64_t>(value.items.size());\n${maximum}\nif (const auto error = validate_constraints_${n}(value); error != error_code::ok) return error;\n${trailingRuntimePredicates(owner, "value")}\n${typeName(operation.countType)} countValue{static_cast<decltype(${typeName(operation.countType)}::value)>(count)};\nif (const auto error = ${internalEncode(operation.countType)}(writer, countValue); error != error_code::ok) return error;\nfor (const auto& item : value.items) {\nif (const auto error = ${internalEncode(item)}(writer, item${encodeParameterArguments(types.get(item.$ref))}); error != error_code::ok) return error;\n}\nreturn error_code::ok;\n}`;
}

function emitVersionedVectorCodec(owner, operation) {
  const n = identifier(owner.name);
  const version = operation.layout.find(
    (entry) => entry.constant !== undefined,
  );
  const count = operation.layout.find((entry) => entry.counts);
  const repeat = operation.layout.find((entry) => entry.kind === "repeat");
  const vectorOperation = {
    ...operation,
    op: "vector",
    countType: { $ref: count.$ref },
    item: repeat.item,
  };
  const base = emitVectorCodec(owner, vectorOperation)
    .replace(
      `decode_value_${n}(reader_t& reader, ${n}_t& out`,
      `decode_items_${n}(reader_t& reader, ${n}_t& out`,
    )
    .replace(
      `encode_value_${n}(writer_t& writer, const ${n}_t& value`,
      `encode_items_${n}(writer_t& writer, const ${n}_t& value`,
    );
  return `${base}\ninline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\nconst auto encodedStart = reader.position();\n${typeName({ $ref: version.$ref })} versionValue{};\nif (const auto error = ${internalDecode({ $ref: version.$ref })}(reader, versionValue); error != error_code::ok) return error;\nif (versionValue.value != ${version.constant}) return error_code::constant;\nif (const auto error = decode_items_${n}(reader, out${decodeParameterArguments(owner)}); error != error_code::ok) return error;\n${encodedLimitCheck(owner, "reader.position() - encodedStart", "decode")}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto encodedStart = writer.data.size();\n${typeName({ $ref: version.$ref })} versionValue{${version.constant}};\nif (const auto error = ${internalEncode({ $ref: version.$ref })}(writer, versionValue); error != error_code::ok) return error;\nif (const auto error = encode_items_${n}(writer, value${encodeParameterArguments(owner)}); error != error_code::ok) return error;\n${encodedLimitCheck(owner, "writer.data.size() - encodedStart", "encode")}\nreturn error_code::ok;\n}`;
}

function emitDelimitedCodec(owner, operation) {
  const n = identifier(owner.name);
  const decodeFields = operation.fields
    .map((field) => emitFieldDecode(field, owner, "out", "body"))
    .join("\n");
  const encodeFields = operation.fields
    .map((field) => emitFieldEncode(field, owner, "value", "body"))
    .join("\n");
  const overhead =
    primaryOperation(types.get(operation.version.$ref)).width +
    primaryOperation(types.get(operation.length.$ref)).width;
  const decodeNegotiated = negotiatedBoundChecks(owner, "decode", {
    "encoded-bytes": `static_cast<std::uint64_t>(${overhead}u) + ${numericExpression("length", operation.length)}`,
  });
  const encodeNegotiated = negotiatedBoundChecks(owner, "encode", {
    "encoded-bytes": `static_cast<std::uint64_t>(${overhead}u) + body.data.size()`,
  });
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\nconst auto encodedStart = reader.position();\n${typeName(operation.version)} version{};\nif (const auto error = ${internalDecode(operation.version)}(reader, version); error != error_code::ok) return error;\nif (${numericExpression("version", operation.version)} != ${operation.version.constant}) return error_code::constant;\n${typeName(operation.length)} length{};\nif (const auto error = ${internalDecode(operation.length)}(reader, length); error != error_code::ok) return error;\n${decodeNegotiated}\n${encodedLimitCheck(owner, `static_cast<std::uint64_t>(${overhead}u) + ${numericExpression("length", operation.length)}`, "decode")}\nreader_t body;\nif (const auto error = reader.subreader(${numericExpression("length", operation.length)}, body); error != error_code::ok) return error;\n${decodeFields}\nif (!body.empty()) return error_code::trailing;\n${aggregateConstraints(operation, owner, "out")}\n${trailingRuntimePredicates(owner, "out")}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto encodedStart = writer.data.size();\n${aggregateConstraints(operation, owner, "value")}\n${trailingRuntimePredicates(owner, "value")}\nwriter_t body;\n${encodeFields}\n${encodeNegotiated}\n${encodedLimitCheck(owner, `static_cast<std::uint64_t>(${overhead}u) + body.data.size()`, "encode")}\n${typeName(operation.version)} version{${operation.version.constant}};\nif (const auto error = ${internalEncode(operation.version)}(writer, version); error != error_code::ok) return error;\n${typeName(operation.length)} length{static_cast<${valueCppType(operation.length)}>(body.data.size())};\nif (const auto error = ${internalEncode(operation.length)}(writer, length); error != error_code::ok) return error;\nwriter.bytes(body.data);\nreturn error_code::ok;\n}`;
}

function encodedLimitCheck(owner, measured, application) {
  return owner.operations
    .filter((entry) => entry.op === "encoded-limit")
    .map((operation) => {
      if (
        operation.measured !== "complete-encoded-value" ||
        operation.exceeded !== "protocol-error" ||
        !operation.applications.includes(application)
      ) {
        throw new Error(`${owner.name}: unsupported encoded-limit operation`);
      }
      return `if (${measured} > ${unsignedLiteral(operation.maximumEncodedBytes)}) return error_code::limit;`;
    })
    .join("\n");
}

function discriminatorExpression(discriminator, value) {
  return discriminator.source.kind === "wire"
    ? `${value}.${identifier(discriminator.name)}`
    : identifier(discriminator.name);
}

function caseCondition(operation, entry, value) {
  if (entry.signature === null) return "true";
  return Object.entries(entry.signature)
    .map(([name, expected]) => {
      const discriminator = operation.discriminators.find(
        (candidate) => candidate.name === name,
      );
      return `${discriminatorExpression(discriminator, value)} == ${enumValue(discriminator.type, expected)}`;
    })
    .join(" && ");
}

function emitConditionalCodec(owner, operation) {
  if (
    operation.encode.selection !== "variant" ||
    operation.encode.discriminatorAgreement !== "required" ||
    operation.encode.mismatch !== "protocol-error"
  ) {
    throw new Error(
      `${owner.name}: unsupported conditional-union encode operation`,
    );
  }
  const n = identifier(owner.name);
  const cases = conditionalCases(operation);
  const wireDiscriminators = operation.discriminators.filter(
    (entry) => entry.source.kind === "wire",
  );
  const decodeDiscriminators = wireDiscriminators
    .map(
      (entry) =>
        `if (const auto error = ${internalDecode(entry.type)}(reader, out.${identifier(entry.name)}); error != error_code::ok) return error;`,
    )
    .join("\n");
  const encodeDiscriminators = wireDiscriminators
    .map(
      (entry) =>
        `if (const auto error = ${internalEncode(entry.type)}(writer, value.${identifier(entry.name)}); error != error_code::ok) return error;`,
    )
    .join("\n");
  const decodeCases = cases
    .map((entry) => {
      const caseOwner = {
        name: `${owner.name}.${entry.name}`,
        operations: [
          { op: "struct", fields: entry.fields, constraints: [] },
          ...entry.operations.filter((item) => item.op === "runtime-predicate"),
        ],
      };
      const syntax = entry.operations
        .map((caseOperation) => {
          if (caseOperation.op === "field")
            return emitFieldDecode(
              caseOperation,
              caseOwner,
              "selected",
              "body",
            );
          if (caseOperation.op === "constraint")
            return aggregateConstraint(caseOperation, caseOwner, "selected");
          if (caseOperation.op === "runtime-predicate")
            return trailingRuntimePredicates(caseOwner, "selected");
          throw new Error(
            `${caseOwner.name}: unsupported case operation ${caseOperation.op}`,
          );
        })
        .join("\n");
      const exact =
        operation.bodyLengthType === null
          ? ""
          : "if (!body.empty()) return error_code::trailing;";
      return `if (${caseCondition(operation, entry, "out")}) {\nout.tag = ${n}_t::tag_t::${entry.tag};\nout.value.template emplace<${n}_t::${entry.name}>();\nauto& selected = std::get<${n}_t::${entry.name}>(out.value);\n${syntax}\n${exact}\n${trailingRuntimePredicates(owner, "out")}\n${encodedLimitCheck(owner, "reader.position() - encodedStart", "decode")}\nreturn error_code::ok;\n}`;
    })
    .join("\n");
  const encodeCases = cases
    .map((entry) => {
      const caseOwner = {
        name: `${owner.name}.${entry.name}`,
        operations: [
          { op: "struct", fields: entry.fields, constraints: [] },
          ...entry.operations.filter((item) => item.op === "runtime-predicate"),
        ],
      };
      const syntax = entry.operations
        .map((caseOperation) => {
          if (caseOperation.op === "field")
            return emitFieldEncode(
              caseOperation,
              caseOwner,
              "selected",
              "bodyWriter",
            );
          if (caseOperation.op === "constraint")
            return aggregateConstraint(caseOperation, caseOwner, "selected");
          if (caseOperation.op === "runtime-predicate")
            return trailingRuntimePredicates(caseOwner, "selected");
          throw new Error(
            `${caseOwner.name}: unsupported case operation ${caseOperation.op}`,
          );
        })
        .join("\n");
      return `if (${caseCondition(operation, entry, "value")}) {\nif (value.tag != ${n}_t::tag_t::${entry.tag}) return error_code::union_case;\nconst auto* selectedPointer = std::get_if<${n}_t::${entry.name}>(&value.value);\nif (selectedPointer == nullptr) return error_code::union_case;\nconst auto& selected = *selectedPointer;\n${syntax}\n${trailingRuntimePredicates(owner, "value")}\nreturn error_code::ok;\n}`;
    })
    .join("\n");
  const decodeBody =
    operation.bodyLengthType === null
      ? `reader_t& body = reader;\n${decodeCases}\nreturn error_code::union_case;`
      : `${typeName(operation.bodyLengthType)} length{};\nif (const auto error = ${internalDecode(operation.bodyLengthType)}(reader, length); error != error_code::ok) return error;\nreader_t body;\nif (const auto error = reader.subreader(${numericExpression("length", operation.bodyLengthType)}, body); error != error_code::ok) return error;\n${decodeCases}\nreturn error_code::union_case;`;
  const encodeBody =
    operation.bodyLengthType === null
      ? `writer_t& bodyWriter = writer;\n${encodeCases}\nreturn error_code::union_case;`
      : `writer_t bodyWriter;\nauto encodeSelected = [&]() -> error_code {\n${encodeCases}\nreturn error_code::union_case;\n};\nif (const auto error = encodeSelected(); error != error_code::ok) return error;\n${typeName(operation.bodyLengthType)} length{static_cast<decltype(${typeName(operation.bodyLengthType)}::value)>(bodyWriter.data.size())};\nif (const auto error = ${internalEncode(operation.bodyLengthType)}(writer, length); error != error_code::ok) return error;\nwriter.bytes(bodyWriter.data);\n${encodedLimitCheck(owner, "writer.data.size() - encodedStart", "encode")}\nreturn error_code::ok;`;
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\nconst auto encodedStart = reader.position();\n${decodeDiscriminators}\n${decodeBody}\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\nconst auto encodedStart = writer.data.size();\n${encodeDiscriminators}\n${encodeBody}\n}`;
}

function tlvPresenceRules(operation, owner, value) {
  return operation.presenceRules
    .map((rule) => {
      const guard = conditionExpression(rule.when, owner, value);
      const required =
        (rule.require ?? [])
          .map((name) => `${value}.${identifier(name)}.has_value()`)
          .join(" && ") || "true";
      const forbidden =
        (rule.forbid ?? [])
          .map((name) => `!${value}.${identifier(name)}.has_value()`)
          .join(" && ") || "true";
      return `if (${guard} && !((${required}) && (${forbidden}))) return error_code::required;`;
    })
    .join("\n");
}

function tlvFieldConstraints(field, expression) {
  return (field.constraints ?? [])
    .map((constraint) => {
      if (constraint.kind !== "contains-protocol-required-capability") {
        throw new Error(
          `${field.name}: unsupported TLV field constraint ${constraint.kind}`,
        );
      }
      return `if (std::none_of(${expression}.items.begin(), ${expression}.items.end(), [](const auto& item) {\nreturn item.value == ::zlink::framework::runtime::protocol::required_capability;\n})) return error_code::constraint;`;
    })
    .join("\n");
}

function emitTlvCodec(owner, operation) {
  const n = identifier(owner.name);
  const totalLengthWidth = primaryOperation(
    types.get(operation.totalLengthType.$ref),
  ).width;
  const seen = operation.fields
    .map((field) => `bool seen_${identifier(field.name)} = false;`)
    .join("\n");
  const decodeCases = operation.fields
    .map((field) => {
      const member = `out.${identifier(field.name)}`;
      const target = `(*${member})`;
      return `case ${field.id}u: {\nif (seen_${identifier(field.name)}) return error_code::duplicate;\nseen_${identifier(field.name)} = true;\n${member}.emplace();\nif (const auto error = ${internalDecode(field.type)}(itemReader, ${target}${fieldDecodeArguments(field, "out")}); error != error_code::ok) return error;\nif (!itemReader.empty()) return error_code::trailing;\n${fieldRangeChecks(field, target)}\n${tlvFieldConstraints(field, target)}\nbreak;\n}`;
    })
    .join("\n");
  const required = operation.fields
    .filter((field) => field.required)
    .map(
      (field) =>
        `if (!seen_${identifier(field.name)}) return error_code::required;`,
    )
    .join("\n");
  const encodeFields = operation.fields
    .map((field) => {
      const member = `value.${identifier(field.name)}`;
      const target = `(*${member})`;
      const required = field.required
        ? `if (!${member}) return error_code::required;\n`
        : "";
      return `${required}if (${member}) {\nwriter_t itemWriter;\n${fieldRangeChecks(field, target)}\n${tlvFieldConstraints(field, target)}\nif (const auto error = ${internalEncode(field.type)}(itemWriter, ${target}${fieldEncodeArguments(field, "value")}); error != error_code::ok) return error;\nentries.push_back(tlv_entry_t{${field.id}u, std::move(itemWriter.data)});\n}`;
    })
    .join("\n");
  return `inline error_code decode_value_${n}(reader_t& reader, ${n}_t& out${decodeParameters(owner)}) {\n${typeName(operation.totalLengthType)} totalLength{};\nif (const auto error = ${internalDecode(operation.totalLengthType)}(reader, totalLength); error != error_code::ok) return error;\n${encodedLimitCheck(owner, `static_cast<std::uint64_t>(${totalLengthWidth}u) + ${numericExpression("totalLength", operation.totalLengthType)}`, "decode")}\nreader_t body;\nif (const auto error = reader.subreader(${numericExpression("totalLength", operation.totalLengthType)}, body); error != error_code::ok) return error;\n${seen}\nout.unknownFields.clear();\nbool first = true;\nstd::uint64_t previous{};\nwhile (!body.empty()) {\n${typeName(operation.fieldIdType)} idValue{};\n${typeName(operation.fieldLengthType)} lengthValue{};\nif (const auto error = ${internalDecode(operation.fieldIdType)}(body, idValue); error != error_code::ok) return error;\nif (const auto error = ${internalDecode(operation.fieldLengthType)}(body, lengthValue); error != error_code::ok) return error;\nconst auto id = static_cast<std::uint64_t>(idValue.value);\nif (!first && id <= previous) return id == previous ? error_code::duplicate : error_code::order;\nfirst = false;\nprevious = id;\nreader_t itemReader;\nif (const auto error = body.subreader(lengthValue.value, itemReader); error != error_code::ok) return error;\nswitch (id) {\n${decodeCases}\ndefault: {\nstd::span<const std::uint8_t> unknown;\nif (const auto error = itemReader.take(itemReader.remaining(), unknown); error != error_code::ok) return error;\nout.unknownFields.push_back({id, {unknown.begin(), unknown.end()}});\nbreak;\n}\n}\n}\n${required}\n${tlvPresenceRules(operation, owner, "out")}\n${trailingRuntimePredicates(owner, "out")}\nreturn error_code::ok;\n}\ninline error_code encode_value_${n}(writer_t& writer, const ${n}_t& value${encodeParameters(owner)}) {\n${tlvPresenceRules(operation, owner, "value")}\n${trailingRuntimePredicates(owner, "value")}\nstd::vector<tlv_entry_t> entries;\n${encodeFields}\nfor (const auto& unknown : value.unknownFields) entries.push_back({unknown.id, unknown.value});\nstd::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.id < right.id; });\nfor (std::size_t index = 1; index < entries.size(); ++index) if (entries[index - 1].id == entries[index].id) return error_code::duplicate;\nwriter_t body;\nfor (const auto& entry : entries) {\n${typeName(operation.fieldIdType)} idValue{static_cast<decltype(${typeName(operation.fieldIdType)}::value)>(entry.id)};\n${typeName(operation.fieldLengthType)} lengthValue{static_cast<decltype(${typeName(operation.fieldLengthType)}::value)>(entry.value.size())};\nif (const auto error = ${internalEncode(operation.fieldIdType)}(body, idValue); error != error_code::ok) return error;\nif (const auto error = ${internalEncode(operation.fieldLengthType)}(body, lengthValue); error != error_code::ok) return error;\nbody.bytes(entry.value);\n}\n${encodedLimitCheck(owner, `static_cast<std::uint64_t>(${totalLengthWidth}u) + body.data.size()`, "encode")}\n${typeName(operation.totalLengthType)} totalLength{static_cast<decltype(${typeName(operation.totalLengthType)}::value)>(body.data.size())};\nif (const auto error = ${internalEncode(operation.totalLengthType)}(writer, totalLength); error != error_code::ok) return error;\nwriter.bytes(body.data);\nreturn error_code::ok;\n}`;
}

const codecEmitters = new Map([
  ["integer", emitIntegerCodec],
  ["enum", emitEnumCodec],
  ["length-prefixed", emitLengthCodec],
  ["struct", emitStructCodec],
  ["vector", emitVectorCodec],
  ["versioned-vector", emitVersionedVectorCodec],
  ["versioned-length-delimited", emitDelimitedCodec],
  ["conditional-union", emitConditionalCodec],
  ["tlv32", emitTlvCodec],
]);

function emitTypeCodec(owner) {
  const operation = primaryOperation(owner);
  const emitter = codecEmitters.get(operation.op);
  if (!emitter)
    throw new Error(
      `${owner.name}: operation ${operation.op} has no C++ codec emitter`,
    );
  return emitter(owner, operation);
}

function commandFields(command) {
  return command.operations.filter((entry) => entry.op === "field");
}

function commandSemanticOwner(command) {
  return {
    name: command.name,
    operations: [
      { op: "struct", fields: commandFields(command), constraints: [] },
      ...command.operations.filter((entry) => entry.op === "runtime-predicate"),
    ],
  };
}

function flagMask(operands) {
  return operands.reduce((mask, entry) => mask | flags.get(entry.name), 0);
}

function commandFlagConstraints(command, flagsExpression) {
  return command.operations
    .filter((entry) => entry.op === "flag-constraint")
    .map((operation) => {
      if (operation.kind === "all-or-none") {
        const mask = flagMask(operation.flags);
        return `if (((${flagsExpression}) & ${mask}u) != 0u && ((${flagsExpression}) & ${mask}u) != ${mask}u) return error_code::flags;`;
      }
      if (operation.kind === "implies") {
        const source = flags.get(operation.if.name);
        const target = flagMask(operation.then);
        return `if (((${flagsExpression}) & ${source}u) != 0u && ((${flagsExpression}) & ${target}u) != ${target}u) return error_code::flags;`;
      }
      throw new Error(
        `${command.name}: unsupported flag constraint ${operation.kind}`,
      );
    })
    .join("\n");
}

function emitCommand(command) {
  const n = identifier(`${command.name}_${command.id}`);
  const semanticOwner = commandSemanticOwner(command);
  const header = command.operations.find(
    (entry) => entry.op === "command-header",
  );
  const flagsOperation = command.operations.find(
    (entry) => entry.op === "flags",
  );
  const metadata = command.operations.find(
    (entry) => entry.op === "metadata-flag-frame",
  );
  const payload = command.operations.find((entry) => entry.op === "payload");
  const fields = commandFields(command);
  const allowed = flagMask(flagsOperation.allowed);
  const required = flagMask(flagsOperation.required);
  const dto = `struct ${n}_t {\n    std::uint8_t flags{};\n${fieldsDto(fields)}${metadata ? `\n    std::optional<${typeName(metadata.frame)}> metadata;` : ""}${payload.type ? `\n    std::optional<${typeName(payload.type)}> payload;` : ""}\n    bool operator==(const ${n}_t&) const = default;\n};`;
  const decodeFields = fields
    .map((field) =>
      emitFieldDecode(field, semanticOwner, "out", "reader", "out.flags"),
    )
    .join("\n");
  const encodeFields = fields
    .map((field) =>
      emitFieldEncode(field, semanticOwner, "value", "writer", "value.flags"),
    )
    .join("\n");
  const predicateDecode = trailingRuntimePredicates(semanticOwner, "out");
  const predicateEncode = trailingRuntimePredicates(semanticOwner, "value");
  const contexts = externalDiscriminators(semanticOwner);
  const params = contexts
    .map((entry) => `, ${typeName(entry.type)} ${identifier(entry.name)}`)
    .join("");
  const args = contexts.map((entry) => `, ${identifier(entry.name)}`).join("");
  const decodeHeadParams = decodeParameters(semanticOwner);
  const decodeHeadArgs = decodeParameterArguments(semanticOwner);
  const encodeHeadParams = encodeParameters(semanticOwner);
  const encodeHeadArgs = encodeParameterArguments(semanticOwner);
  const decodeParams = `${params}${negotiatedBounds(command).length === 0 ? "" : ", const codec_context_t& context"}`;
  const encodeParams = `${params}${negotiatedBounds(command).length === 0 ? "" : ", const codec_context_t& context"}`;
  const metadataDecode = metadata
    ? `const bool hasMetadata = (out.flags & ${flags.get(metadata.flag.name)}u) != 0u;\nif (hasMetadata) {\nif (index >= frames.size()) return {error_code::required, {}};\nout.metadata.emplace();\nreader_t metadataReader{frames[index++]};\nif (const auto error = ${internalDecode(metadata.frame)}(metadataReader, *out.metadata${negotiatedBounds(types.get(metadata.frame.$ref)).length === 0 ? "" : ", context"}); error != error_code::ok || !metadataReader.empty()) return {error == error_code::ok ? error_code::trailing : error, {}};\n} else {\nout.metadata.reset();\n}`
    : "";
  const metadataEncode = metadata
    ? `const bool hasMetadata = (value.flags & ${flags.get(metadata.flag.name)}u) != 0u;\nif (hasMetadata != value.metadata.has_value()) return {hasMetadata ? error_code::required : error_code::forbidden, {}};\nif (value.metadata) {\nwriter_t metadataWriter;\nif (const auto error = ${internalEncode(metadata.frame)}(metadataWriter, *value.metadata${negotiatedBounds(types.get(metadata.frame.$ref)).length === 0 ? "" : ", context"}); error != error_code::ok) return {error, {}};\nframes.push_back(std::move(metadataWriter.data));\n}`
    : "";
  let payloadDecode = "";
  let payloadEncode = "";
  if (payload.policy === "forbidden") {
    payloadDecode =
      "if (index != frames.size()) return {error_code::forbidden, {}};";
  } else {
    payloadDecode = `${payload.policy === "required" ? "if (index >= frames.size()) return {error_code::required, {}};" : ""}\nif (index < frames.size()) {\nout.payload.emplace();\nreader_t payloadReader{frames[index++]};\nif (const auto error = ${internalDecode(payload.type)}(payloadReader, *out.payload${negotiatedBounds(types.get(payload.type.$ref)).length === 0 ? "" : ", context"}); error != error_code::ok || !payloadReader.empty()) return {error == error_code::ok ? error_code::trailing : error, {}};\n}\nif (index != frames.size()) return {error_code::trailing, {}};`;
    payloadEncode = `if (${payload.policy === "required" ? "!value.payload" : "false"}) return {error_code::required, {}};\nif (value.payload) {\nwriter_t payloadWriter;\nif (const auto error = ${internalEncode(payload.type)}(payloadWriter, *value.payload${negotiatedBounds(types.get(payload.type.$ref)).length === 0 ? "" : ", context"}); error != error_code::ok) return {error, {}};\nframes.push_back(std::move(payloadWriter.data));\n}`;
  }
  const magic = header.magic
    .map((entry) => `writer.data.push_back(${entry}u);`)
    .join("\n");
  const magicDecode = header.magic
    .map(
      (entry) =>
        `if (const auto error = reader.expect(${entry}u); error != error_code::ok) return error;`,
    )
    .join("\n");
  return `${dto}\ninline error_code decode_head_${n}(reader_t& reader, ${n}_t& out${decodeHeadParams}) {\n${magicDecode}\nif (const auto error = reader.expect(${header.wireMajor}u); error != error_code::ok) return error;\nif (const auto error = reader.expect(${header.commandId}u); error != error_code::ok) return error;\nstd::uint64_t flagsValue{};\nif (const auto error = reader.unsignedInteger(1, flagsValue); error != error_code::ok) return error;\nout.flags = static_cast<std::uint8_t>(flagsValue);\nif ((out.flags & ~${allowed}u) != 0u || (out.flags & ${required}u) != ${required}u) return error_code::flags;\n${commandFlagConstraints(command, "out.flags")}\n${decodeFields}\nif (!reader.empty()) return error_code::trailing;\n${predicateDecode}\nreturn error_code::ok;\n}\ninline error_code encode_head_${n}(writer_t& writer, const ${n}_t& value${encodeHeadParams}) {\nif ((value.flags & ~${allowed}u) != 0u || (value.flags & ${required}u) != ${required}u) return error_code::flags;\n${commandFlagConstraints(command, "value.flags")}\n${predicateEncode}\n${magic}\nwriter.data.push_back(${header.wireMajor}u);\nwriter.data.push_back(${header.commandId}u);\nwriter.data.push_back(value.flags);\n${encodeFields}\nreturn error_code::ok;\n}\ninline result_t<${n}_t> decode_${n}_frames(const std::vector<std::vector<std::uint8_t>>& frames${decodeParams}) {\nif (frames.empty()) return {error_code::required, {}};\n${n}_t out{};\nreader_t headReader{frames.front()};\nif (const auto error = decode_head_${n}(headReader, out${decodeHeadArgs}); error != error_code::ok) return {error, {}};\nstd::size_t index = 1;\n${metadataDecode}\n${payloadDecode}\nreturn {error_code::ok, std::move(out)};\n}\ninline result_t<std::vector<std::vector<std::uint8_t>>> encode_${n}_frames(const ${n}_t& value${encodeParams}) {\nwriter_t headWriter;\nif (const auto error = encode_head_${n}(headWriter, value${encodeHeadArgs}); error != error_code::ok) return {error, {}};\nstd::vector<std::vector<std::uint8_t>> frames;\nframes.push_back(std::move(headWriter.data));\n${metadataEncode}\n${payloadEncode}\nreturn {error_code::ok, std::move(frames)};\n}\ninline bool validate_${n}_runtime_predicates(const ${n}_t& value) {\nreturn [&]() -> error_code {\n${predicateEncode}\nreturn error_code::ok;\n}() == error_code::ok;\n}`;
}

function emitDurable(format) {
  const n = identifier(`durable_${format.name}`);
  const header = format.operations.find(
    (entry) => entry.op === "durable-header",
  );
  const checksum = format.operations.find((entry) => entry.op === "checksum");
  const limit = format.operations.find((entry) => entry.op === "encoded-limit");
  const bodyField = format.operations.find(
    (entry) => entry.op === "field" && entry.name === "body",
  );
  if (
    checksum.algorithm !== "crc32c-castagnoli" ||
    checksum.verification !== "before-body-interpretation" ||
    bodyField.interpretation !== "after-checksum-verification" ||
    limit.measured !== "complete-encoded-value" ||
    limit.exceeded !== "protocol-error" ||
    !limit.applications.includes("encode") ||
    !limit.applications.includes("decode")
  ) {
    throw new Error(`${format.name}: unsupported durable operation sequence`);
  }
  const magicDecode = header.magic
    .map(
      (entry) =>
        `if (const auto error = reader.expect(${entry}u); error != error_code::ok) return {error, {}};`,
    )
    .join("\n");
  const magicEncode = header.magic
    .map((entry) => `writer.data.push_back(${entry}u);`)
    .join("\n");
  const contextParameter =
    negotiatedBounds(format).length === 0
      ? ""
      : ", const codec_context_t& context";
  const bodyContextArgument =
    negotiatedBounds(types.get(bodyField.type.$ref)).length === 0
      ? ""
      : ", context";
  return `struct ${n}_t {\n    ${typeName(bodyField.type)} body{};\n    bool operator==(const ${n}_t&) const = default;\n};\ninline result_t<${n}_t> decode_${n}(std::span<const std::uint8_t> bytes${contextParameter}) {\nif (bytes.size() > ${unsignedLiteral(limit.maximumEncodedBytes)}) return {error_code::limit, {}};\nreader_t reader{bytes};\n${magicDecode}\nif (const auto error = reader.expect(${header.formatVersion}u); error != error_code::ok) return {error, {}};\n${typeName(header.flagsType)} flagsValue{};\nif (const auto error = ${internalDecode(header.flagsType)}(reader, flagsValue); error != error_code::ok) return {error, {}};\nif (${numericExpression("flagsValue", header.flagsType)} != ${header.flags}) return {error_code::flags, {}};\n${typeName(header.bodyLengthType)} bodyLength{};\nif (const auto error = ${internalDecode(header.bodyLengthType)}(reader, bodyLength); error != error_code::ok) return {error, {}};\nreader_t bodyReader;\nif (const auto error = reader.subreader(${numericExpression("bodyLength", header.bodyLengthType)}, bodyReader); error != error_code::ok) return {error, {}};\nconst auto checksumPosition = reader.position();\nstd::uint64_t checksumValue{};\nif (const auto error = reader.unsignedInteger(4, checksumValue); error != error_code::ok) return {error, {}};\nif (!reader.empty()) return {error_code::trailing, {}};\nif (checksumValue != crc32c(bytes.first(checksumPosition))) return {error_code::checksum, {}};\n${n}_t out{};\nif (const auto error = ${internalDecode(bodyField.type)}(bodyReader, out.body${bodyContextArgument}); error != error_code::ok || !bodyReader.empty()) return {error == error_code::ok ? error_code::trailing : error, {}};\nreturn {error_code::ok, std::move(out)};\n}\ninline result_t<std::vector<std::uint8_t>> encode_${n}(const ${n}_t& value${contextParameter}) {\nwriter_t bodyWriter;\nif (const auto error = ${internalEncode(bodyField.type)}(bodyWriter, value.body${bodyContextArgument}); error != error_code::ok) return {error, {}};\nwriter_t writer;\n${magicEncode}\nwriter.data.push_back(${header.formatVersion}u);\n${typeName(header.flagsType)} flagsValue{${header.flags}};\nif (const auto error = ${internalEncode(header.flagsType)}(writer, flagsValue); error != error_code::ok) return {error, {}};\n${typeName(header.bodyLengthType)} bodyLength{static_cast<${valueCppType(header.bodyLengthType)}>(bodyWriter.data.size())};\nif (const auto error = ${internalEncode(header.bodyLengthType)}(writer, bodyLength); error != error_code::ok) return {error, {}};\nwriter.bytes(bodyWriter.data);\nconst auto checksumValue = crc32c(writer.data);\nif (const auto error = writer.unsignedInteger(4, checksumValue); error != error_code::ok) return {error, {}};\nif (writer.data.size() > ${unsignedLiteral(limit.maximumEncodedBytes)}) return {error_code::limit, {}};\nreturn {error_code::ok, std::move(writer.data)};\n}`;
}

function logicalCppFrameName(reference) {
  return `logical_${identifier(reference.$ref)}_frame_t`;
}
function logicalCppFrameCall(reference, end, environment = new Map()) {
  const target = types.get(reference.$ref),
    operation = primaryOperation(target),
    arguments_ = [end];
  if (operation.op === "conditional-union") {
    for (const discriminator of operation.discriminators.filter(
      (entry) => entry.source.kind !== "wire",
    )) {
      if (discriminator.source.kind === "enclosingField")
        arguments_.push(environment.get(discriminator.source.name));
      else
        throw new Error(
          `${target.name}: logical context discriminator is not available`,
        );
    }
  }
  if (arguments_.some((entry) => entry === undefined))
    throw new Error(`${target.name}: missing logical discriminator`);
  return `std::make_unique<${logicalCppFrameName(reference)}>(${arguments_.join(", ")})`;
}
function logicalCppFields(
  owner,
  fields,
  startState,
  endExpression,
  root = "out",
  base = new Map(),
) {
  const environment = new Map(base),
    lines = [];
  let state = startState;
  for (const field of fields) {
    const member = `${root}.${identifier(field.name)}`,
      optional = Boolean(field.when || field.required === false);
    const target = optional ? `*${member}` : member;
    const condition = conditionExpression(field.when, owner, root);
    environment.set(field.name, target);
    lines.push(
      `case ${state}: { ${field.when ? `if (!(${condition})) { ${member}.reset(); state = ${state + 2}; return error_code::ok; }` : ""} state = ${state + 1}; machine.push(${logicalCppFrameCall(field.type, endExpression, environment)}); return error_code::ok; }`,
    );
    lines.push(
      `case ${state + 1}: { ${optional ? `${member}.emplace(std::any_cast<${typeName(field.type)}>(take_child()));` : `${member} = std::any_cast<${typeName(field.type)}>(take_child());`} ${fieldRangeChecks(field, target)} state = ${state + 2}; return error_code::ok; }`,
    );
    state += 2;
  }
  return { lines, environment, nextState: state };
}
function logicalCppLimit(owner) {
  return encodedLimitCheck(owner, "machine.consumed() - start", "decode");
}
function logicalCppFrame(owner) {
  const operation = primaryOperation(owner),
    n = identifier(owner.name),
    frame = logicalCppFrameName({ $ref: owner.name }),
    limit = logicalCppLimit(owner);
  if (operation.op === "integer") {
    const raw =
      operation.encoding === "i64"
        ? "static_cast<std::int64_t>(machine.uint(*bytes))"
        : `static_cast<${scalarCppType(operation)}>(machine.uint(*bytes))`;
    const min =
        operation.encoding === "i64"
          ? signedLiteral(operation.minimum)
          : unsignedLiteral(operation.minimum),
      max =
        operation.encoding === "i64"
          ? signedLiteral(operation.maximum)
          : unsignedLiteral(operation.maximum);
    return `struct ${frame} final : logical_frame_t { explicit ${frame}(std::uint64_t end) : logical_frame_t{end} {} error_code resume(logical_machine_t& machine) override { auto bytes = machine.take(*this, ${operation.width}u, true); if (!bytes) return machine.failure(); const auto value = ${raw}; if (value < ${min} || value > ${max}) return error_code::range; ${limit} machine.complete(${n}_t{value}); return error_code::ok; } };`;
  }
  if (operation.op === "enum") {
    const cases_ = operation.values
      .map(
        (entry) =>
          `case ${entry.value}: machine.complete(static_cast<${n}_t>(raw)); return error_code::ok;`,
      )
      .join(" ");
    return `struct ${frame} final : logical_frame_t { explicit ${frame}(std::uint64_t end) : logical_frame_t{end} {} error_code resume(logical_machine_t& machine) override { auto bytes = machine.take(*this, ${operation.width}u, true); if (!bytes) return machine.failure(); const auto raw = machine.uint(*bytes); switch (raw) { ${cases_} default: return error_code::enum_value; } } };`;
  }
  if (operation.op === "length-prefixed") {
    const absent = operation.zeroLengthMeaning === "absent",
      text = operation.content === "text",
      validate = owner.operations.find(
        (entry) => entry.op === "text-validation",
      ),
      negotiated = negotiatedBoundChecks(owner, "decode", {
        "content-bytes": "length",
      });
    const value = absent ? "*out.value" : "out.value",
      initialize = absent ? "out.value.emplace();" : "",
      prefix = `case 0: state=1; machine.push(${logicalCppFrameCall(operation.lengthType, "end")}); return error_code::ok; case 1: { const auto encoded=static_cast<std::uint64_t>(std::any_cast<${typeName(operation.lengthType)}>(take_child()).value); if(encoded>${unsignedLiteral(operation.encodeCapacity.representationCeiling)}) return error_code::capacity; if(encoded<${unsignedLiteral(operation.minimumBytes)} || encoded>${unsignedLiteral(operation.encodeCapacity.declaredMaximumBytes)}) return error_code::range; if(encoded>std::numeric_limits<std::size_t>::max()) return error_code::capacity; length=static_cast<std::size_t>(encoded); ${negotiated} ${absent ? `if(length==0){ ${limit} out.value.reset(); machine.complete(std::move(out)); return error_code::ok; }` : ""} ${initialize}`;
    if (text) {
      const forbidNul = validate?.nul === "forbidden";
      return `struct ${frame} final : logical_frame_t { ${n}_t out{}; std::size_t length{},remaining{}; std::uint32_t code_point{},minimum{}; std::uint8_t needed{}; explicit ${frame}(std::uint64_t end) : logical_frame_t{end} {} error_code append(std::uint8_t item){try{${value}.push_back(static_cast<char>(item));}catch(const std::bad_alloc&){return error_code::capacity;}catch(const std::length_error&){return error_code::capacity;}return error_code::ok;} error_code resume(logical_machine_t& machine) override { const auto& context = machine.context(); switch (state) { ${prefix} remaining=length; state=2; return error_code::ok; } case 2: { while(remaining>0){auto next=machine.read_byte(*this,0);if(!next)return machine.failure();const auto item=*next;--remaining;if(needed==0){if(item<=0x7f){${forbidNul ? "if(item==0)return error_code::utf8;" : ""}if(const auto error=append(item);error!=error_code::ok)return error;continue;}if(item>=0xc2&&item<=0xdf){code_point=item&0x1f;needed=1;minimum=0x80;}else if(item>=0xe0&&item<=0xef){code_point=item&0x0f;needed=2;minimum=0x800;}else if(item>=0xf0&&item<=0xf4){code_point=item&0x07;needed=3;minimum=0x10000;}else return error_code::utf8;if(const auto error=append(item);error!=error_code::ok)return error;continue;}if((item&0xc0)!=0x80)return error_code::utf8;code_point=(code_point<<6)|(item&0x3f);if(const auto error=append(item);error!=error_code::ok)return error;if(--needed==0&&(code_point<minimum||code_point>0x10ffff||(code_point>=0xd800&&code_point<=0xdfff)${forbidNul ? "||code_point==0" : ""}))return error_code::utf8;}if(needed!=0)return error_code::utf8;${limit}machine.complete(std::move(out));return error_code::ok;} default:return error_code::invalid_state; } } };`;
    }
    return `struct ${frame} final : logical_frame_t { ${n}_t out{}; std::size_t length{}; explicit ${frame}(std::uint64_t end) : logical_frame_t{end} {} error_code resume(logical_machine_t& machine) override { const auto& context = machine.context(); switch (state) { ${prefix} try{${value}.resize(length);}catch(const std::bad_alloc&){return error_code::capacity;}catch(const std::length_error&){return error_code::capacity;}state=2;return error_code::ok;} case 2: { if(!machine.fill(*this,std::span<std::uint8_t>{${value}}))return machine.failure();${limit}machine.complete(std::move(out));return error_code::ok;} default:return error_code::invalid_state; } } };`;
  }
  if (
    operation.op === "struct" ||
    operation.op === "versioned-length-delimited"
  ) {
    const delimited = operation.op === "versioned-length-delimited";
    let prefix = "",
      start = 0,
      end = "end";
    if (delimited) {
      prefix = `case 0: state=1; machine.push(${logicalCppFrameCall(operation.version, "end")}); return error_code::ok; case 1: { auto version=std::any_cast<${typeName(operation.version)}>(take_child()); if(${numericExpression("version", operation.version)} != ${operation.version.constant}) return error_code::constant; state=2; machine.push(${logicalCppFrameCall(operation.length, "end")}); return error_code::ok; } case 2: { auto bounded=machine.bound(end,static_cast<std::uint64_t>(std::any_cast<${typeName(operation.length)}>(take_child()).value)); if(!bounded) return machine.failure(); body_end=*bounded; state=3; return error_code::ok; }`;
      start = 3;
      end = "body_end";
    }
    const fields = logicalCppFields(owner, operation.fields, start, end),
      constraints = aggregateConstraints(operation, owner, "out"),
      predicates = trailingRuntimePredicates(owner, "out");
    return `struct ${frame} final : logical_frame_t { ${n}_t out{}; ${delimited ? "std::uint64_t body_end{};" : ""} explicit ${frame}(std::uint64_t end):logical_frame_t{end}{} error_code resume(logical_machine_t& machine) override { const auto& context=machine.context(); switch(state){ ${prefix} ${fields.lines.join(" ")} case ${fields.nextState}: { ${delimited ? "if(machine.consumed()!=body_end)return error_code::range;" : ""} ${constraints} ${predicates} ${limit} machine.complete(std::move(out)); return error_code::ok; } default:return error_code::invalid_state; } } };`;
  }
  if (operation.op === "vector" || operation.op === "versioned-vector") {
    const versioned = operation.op === "versioned-vector",
      repeat = versioned
        ? operation.layout.find((entry) => entry.kind === "repeat")
        : { item: operation.item },
      count = versioned
        ? operation.layout.find((entry) => entry.counts)
        : operation.countType;
    let prefix, loop;
    if (versioned) {
      const version = operation.layout[0];
      prefix = `case 0: state=1; machine.push(${logicalCppFrameCall(version, "end")}); return error_code::ok; case 1: { auto value=std::any_cast<${typeName(version)}>(take_child()); if(${numericExpression("value", version)} != ${version.constant}) return error_code::constant; state=2; machine.push(${logicalCppFrameCall(count, "end")}); return error_code::ok; } case 2: count=static_cast<std::uint64_t>(std::any_cast<${typeName(count)}>(take_child()).value); ${operation.maximumItems === undefined ? "" : `if(count>${unsignedLiteral(operation.maximumItems)})return error_code::range;`} if(count>std::numeric_limits<std::size_t>::max())return error_code::capacity; state=3; return error_code::ok;`;
      loop = 3;
    } else {
      prefix = `case 0: state=1; machine.push(${logicalCppFrameCall(count, "end")}); return error_code::ok; case 1: count=static_cast<std::uint64_t>(std::any_cast<${typeName(count)}>(take_child()).value); ${operation.maximumItems === undefined ? "" : `if(count>${unsignedLiteral(operation.maximumItems)})return error_code::range;`} if(count>std::numeric_limits<std::size_t>::max())return error_code::capacity; state=2; return error_code::ok;`;
      loop = 2;
    }
    return `struct ${frame} final : logical_frame_t { ${n}_t out{}; std::uint64_t count{},index{}; explicit ${frame}(std::uint64_t end):logical_frame_t{end}{} error_code resume(logical_machine_t& machine) override { const auto& context=machine.context(); switch(state){ ${prefix} case ${loop}: if(index==count){ if(const auto error=validate_constraints_${n}(out);error!=error_code::ok)return error; ${trailingRuntimePredicates(owner, "out")} ${limit} machine.complete(std::move(out)); return error_code::ok; } state=${loop + 1}; machine.push(${logicalCppFrameCall(repeat.item, "end")}); return error_code::ok; case ${loop + 1}: try{out.items.push_back(std::any_cast<${typeName(repeat.item)}>(take_child()));}catch(const std::bad_alloc&){return error_code::capacity;}catch(const std::length_error&){return error_code::capacity;} ++index; state=${loop}; return error_code::ok; default:return error_code::invalid_state; } } };`;
  }
  if (operation.op === "conditional-union") {
    const cases_ = conditionalCases(operation),
      wire = operation.discriminators.filter(
        (entry) => entry.source.kind === "wire",
      ),
      external = operation.discriminators.filter(
        (entry) => entry.source.kind !== "wire",
      ),
      members = external
        .map(
          (entry) =>
            `${typeName(entry.type)} external_${identifier(entry.name)}{};`,
        )
        .join(" "),
      parameters_ = external
        .map((entry) => `, ${typeName(entry.type)} ${identifier(entry.name)}`)
        .join(""),
      initializers = external
        .map(
          (entry) =>
            `, external_${identifier(entry.name)}{${identifier(entry.name)}}`,
        )
        .join("");
    const environment = new Map(),
      lines = [];
    let state = 0;
    for (const d of operation.discriminators) {
      const expression =
        d.source.kind === "wire"
          ? `out.${identifier(d.name)}`
          : `external_${identifier(d.name)}`;
      environment.set(d.name, expression);
      if (d.source.kind === "wire") {
        lines.push(
          `case ${state}: state=${state + 1}; machine.push(${logicalCppFrameCall(d.type, "end", environment)}); return error_code::ok;`,
          `case ${state + 1}: out.${identifier(d.name)}=std::any_cast<${typeName(d.type)}>(take_child()); state=${state + 2}; return error_code::ok;`,
        );
        state += 2;
      }
    }
    let selectedEnd = "end";
    if (operation.bodyLengthType) {
      lines.push(
        `case ${state}: state=${state + 1}; machine.push(${logicalCppFrameCall(operation.bodyLengthType, "end")}); return error_code::ok;`,
        `case ${state + 1}: { auto bounded=machine.bound(end,static_cast<std::uint64_t>(std::any_cast<${typeName(operation.bodyLengthType)}>(take_child()).value)); if(!bounded)return machine.failure(); body_end=*bounded; state=${state + 2}; return error_code::ok; }`,
      );
      state += 2;
      selectedEnd = "body_end";
    }
    const select = state,
      blocks = [];
    let next = state + 1;
    for (const entry of cases_) {
      const test =
          entry.signature === null
            ? "true"
            : Object.entries(entry.signature)
                .map(([name, value]) => {
                  const d = operation.discriminators.find(
                    (x) => x.name === name,
                  );
                  return `${environment.get(name)}==${expectedExpression(d.type, value)}`;
                })
                .join(" && "),
        caseType = `${n}_t::${entry.name}`,
        root = `std::get<${caseType}>(out.value)`,
        caseOwner = {
          name: `${owner.name}.${entry.name}`,
          operations: [{ op: "struct", fields: entry.fields, constraints: [] }],
        },
        fields = logicalCppFields(
          caseOwner,
          entry.fields,
          next,
          selectedEnd,
          root,
          environment,
        );
      blocks.push(
        `${entry.index ? "else " : ""}if(${test}){out.tag=${n}_t::tag_t::${entry.tag};out.value=${caseType}{};state=${next};return error_code::ok;}`,
      );
      lines.push(...fields.lines);
      const checks = entry.operations
          .filter((item) => item.op === "constraint")
          .map((item) => aggregateConstraint(item, caseOwner, "selected"))
          .join(" "),
        predicates = entry.operations
          .filter((item) => item.op === "runtime-predicate")
          .map((item) => runtimePredicate(caseOwner, item, "selected"))
          .join(" ");
      lines.push(
        `case ${fields.nextState}: { ${operation.bodyLengthType ? "if(machine.consumed()!=body_end)return error_code::range;" : ""} auto& selected=${root}; ${checks} ${predicates} ${limit} machine.complete(std::move(out)); return error_code::ok; }`,
      );
      next = fields.nextState + 1;
    }
    lines.push(
      `case ${select}: ${blocks.join(" ")} return error_code::union_case;`,
    );
    return `struct ${frame} final : logical_frame_t { ${n}_t out{}; ${members} ${operation.bodyLengthType ? "std::uint64_t body_end{};" : ""} explicit ${frame}(std::uint64_t end${parameters_}):logical_frame_t{end}${initializers}{} error_code resume(logical_machine_t& machine) override { const auto& context=machine.context(); switch(state){ ${lines.join(" ")} default:return error_code::invalid_state; } } };`;
  }
  throw new Error(`${owner.name}: unsupported logical frame ${operation.op}`);
}

function emitLogical(format) {
  const n = identifier(`logical_${format.name}`);
  const stream = format.operations.find(
    (entry) => entry.op === "logical-stream",
  );
  const limit = format.operations.find((entry) => entry.op === "encoded-limit");
  const machine = stream.decode;
  if (
    JSON.stringify(machine?.replay) !== JSON.stringify(stream.replay) ||
    machine.replay.mode !== "bounded-incremental-decode" ||
    machine.replay.wholeStreamInputAllocation !== "forbidden" ||
    machine.replay.completion !== "success-only-on-final-chunk" ||
    machine.replay.incompleteFinalChunk !== "truncation" ||
    machine.replay.bytesAfterRoot !== "trailing-bytes" ||
    !Array.isArray(machine.frames?.kinds) ||
    !Array.isArray(machine.frames.reachableTypes) ||
    !Number.isInteger(machine.frames.maximumDepth)
  ) {
    throw new Error(`${format.name}: unsupported incremental decode machine`);
  }
  const contextParameter =
    negotiatedBounds(format).length === 0
      ? ""
      : ", const codec_context_t& context";
  const bodyContextArgument =
    negotiatedBounds(types.get(stream.body.$ref)).length === 0
      ? ""
      : ", context";
  const decoderContextMember =
    contextParameter === "" ? "" : "    codec_context_t context_;\n";
  const decoderConstructorParameter =
    contextParameter === "" ? "" : "const codec_context_t& context";
  const decoderConstructorInitializer =
    contextParameter === "" ? "" : ", context_{context}";
  const decoderBodyContextArgument =
    contextParameter === "" ? "" : ", context_";
  const oneShotDecoderArgument = contextParameter === "" ? "" : "context";
  const reachable = new Set(machine.frames.reachableTypes);
  const frames = orderedTypes({
    types: machine.frames.reachableTypes.map((name) => types.get(name)),
  })
    .filter((owner) => reachable.has(owner.name))
    .map(logicalCppFrame)
    .join("\n");
  return `struct ${n}_t {\n    ${typeName(stream.body)} body{};\n    bool operator==(const ${n}_t&) const = default;\n};
enum class logical_decode_progress_t : std::uint8_t { need_more, complete };
struct ${n}_decode_step_t {
    logical_decode_progress_t progress{};
    std::size_t buffered_input_byte_count{};
    std::uint64_t consumed_byte_count{};
    std::size_t continuation_depth{};
    std::optional<${n}_t> value;
};
class logical_machine_t;
struct logical_frame_t {
    std::uint64_t end{}, start{}; std::size_t state{}, filled{}; std::any child;
    std::vector<std::uint8_t> scratch;
    explicit logical_frame_t(std::uint64_t value) : end{value} {}
    virtual ~logical_frame_t() = default;
    std::any take_child() { auto value=std::move(child); child.reset(); return value; }
    virtual error_code resume(logical_machine_t&) = 0;
};
class logical_machine_t {
public:
    explicit logical_machine_t(const codec_context_t& context);
    const codec_context_t& context() const noexcept { return context_; }
    std::uint64_t consumed() const noexcept { return consumed_; }
    std::size_t depth() const noexcept { return stack_.size(); }
    std::size_t buffered() const noexcept { return buffered_; }
    error_code failure() const noexcept { return failure_; }
    static std::uint64_t uint(std::span<const std::uint8_t> bytes) { std::uint64_t value{}; for(auto item:bytes)value=(value<<8)|item; return value; }
    std::optional<std::uint64_t> bound(std::uint64_t parent_end,std::uint64_t length){if(consumed_>parent_end||length>parent_end-consumed_){failure_=error_code::range;return std::nullopt;}return consumed_+length;}
    void push(std::unique_ptr<logical_frame_t> frame){frame->start=consumed_;stack_.push_back(std::move(frame));}
    template<class T> void complete(T value){stack_.pop_back();if(stack_.empty())result_=std::move(value);else stack_.back()->child=std::move(value);}
    std::optional<std::span<const std::uint8_t>> take(logical_frame_t& frame,std::size_t count,bool scalar){if(!scalar||count>8){failure_=error_code::invalid_state;return std::nullopt;}const auto remaining=count-frame.filled;if(consumed_>frame.end||remaining>frame.end-consumed_){failure_=error_code::range;return std::nullopt;}try{if(frame.scratch.size()!=count)frame.scratch.resize(count);}catch(const std::bad_alloc&){failure_=error_code::capacity;return std::nullopt;}catch(const std::length_error&){failure_=error_code::capacity;return std::nullopt;}while(frame.filled<count){const auto available=chunk_.size()-at_;if(available==0){if(final_chunk_){failure_=error_code::truncated;return std::nullopt;}waiting_=true;buffered_=frame.filled;return std::nullopt;}const auto copied=std::min(count-frame.filled,available);std::copy_n(chunk_.begin()+static_cast<std::ptrdiff_t>(at_),copied,frame.scratch.begin()+static_cast<std::ptrdiff_t>(frame.filled));at_+=copied;frame.filled+=copied;consumed_+=copied;}frame.filled=0;buffered_=0;return std::span<const std::uint8_t>{frame.scratch};}
    bool fill(logical_frame_t& frame,std::span<std::uint8_t> value){if(consumed_>frame.end||value.size()-frame.filled>frame.end-consumed_){failure_=error_code::range;return false;}while(frame.filled<value.size()){const auto available=chunk_.size()-at_;if(available==0){if(final_chunk_){failure_=error_code::truncated;return false;}waiting_=true;buffered_=0;return false;}const auto copied=std::min(value.size()-frame.filled,available);std::copy_n(chunk_.begin()+static_cast<std::ptrdiff_t>(at_),copied,value.begin()+static_cast<std::ptrdiff_t>(frame.filled));at_+=copied;frame.filled+=copied;consumed_+=copied;}buffered_=0;return true;}
    std::optional<std::uint8_t> read_byte(logical_frame_t& frame,std::size_t retained){if(consumed_>=frame.end){failure_=error_code::range;return std::nullopt;}if(at_==chunk_.size()){if(final_chunk_){failure_=error_code::truncated;return std::nullopt;}waiting_=true;buffered_=std::min<std::size_t>(retained,7);return std::nullopt;}++consumed_;return chunk_[at_++];}
    error_code run(std::span<const std::uint8_t> input,bool final_input){chunk_=input;struct input_release_t{std::span<const std::uint8_t>& value;~input_release_t(){value={};}}release{chunk_};at_=0;final_chunk_=final_input;waiting_=false;buffered_=0;failure_=error_code::ok;if(awaiting_final_){if(!input.empty())return error_code::trailing;if(final_input)return error_code::ok;waiting_=true;return error_code::ok;}while(!waiting_&&!result_.has_value()){const auto error=stack_.back()->resume(*this);if(error!=error_code::ok)return error;}if(result_.has_value()){if(at_!=input.size())return error_code::trailing;if(final_input)return error_code::ok;awaiting_final_=true;waiting_=true;return error_code::ok;}if(at_!=input.size())return error_code::invalid_state;if(final_input)return error_code::truncated;return error_code::ok;}
    bool complete_result()const noexcept{return result_.has_value()&&!waiting_;}
    std::any take_result(){return std::move(*result_);}
private:
    codec_context_t context_;std::vector<std::unique_ptr<logical_frame_t>> stack_;std::span<const std::uint8_t> chunk_;std::size_t at_{},buffered_{};bool final_chunk_{},waiting_{},awaiting_final_{};std::uint64_t consumed_{};error_code failure_{error_code::ok};std::optional<std::any> result_;
};
${frames}
inline logical_machine_t::logical_machine_t(const codec_context_t& context):context_{context}{push(${logicalCppFrameCall(stream.body, "std::numeric_limits<std::uint64_t>::max()")});}
class ${n}_decoder_t {
public:
    static constexpr std::size_t maximum_continuation_depth = ${machine.frames.maximumDepth};
    explicit ${n}_decoder_t(${decoderConstructorParameter})
      : machine_{${contextParameter === "" ? "codec_context_t{}" : "context"}} {}
    ${n}_decoder_t(const ${n}_decoder_t&) = delete;
    ${n}_decoder_t& operator=(const ${n}_decoder_t&) = delete;
    void set_synthetic_prior_input_byte_count_for_test(std::uint64_t value){if(encoded_size_!=0)throw std::logic_error{"decoder already used"};encoded_size_=value;}
    std::uint64_t failure_offset()const noexcept{return failure_offset_;}
    result_t<${n}_decode_step_t> push(
      std::span<const std::uint8_t> chunk, bool final_chunk) {
        if (terminal_) return {error_code::invalid_state, {}};
        if (chunk.size() > ${unsignedLiteral(limit.maximumEncodedBytes)} - encoded_size_) {
            terminal_ = true; failure_offset_=machine_.consumed();
            return {error_code::limit, {}};
        }
        encoded_size_ += chunk.size();
        const auto error=machine_.run(chunk,final_chunk);
        if(error!=error_code::ok){terminal_=true;failure_offset_=machine_.consumed();return {error,{}};}
        if(machine_.complete_result()){
            terminal_=true;auto body=std::any_cast<${typeName(stream.body)}>(machine_.take_result());
            return {error_code::ok,
              {logical_decode_progress_t::complete, 0, machine_.consumed(), 0, ${n}_t{std::move(body)}}};
        }
        return {error_code::ok,
          {logical_decode_progress_t::need_more, machine_.buffered(),
            machine_.consumed(), machine_.depth(), std::nullopt}};
    }
private:
    logical_machine_t machine_;std::uint64_t encoded_size_{},failure_offset_{};
    bool terminal_{};
};
inline result_t<${n}_t> decode_${n}(std::span<const std::uint8_t> bytes${contextParameter}) {
${n}_decoder_t decoder{${oneShotDecoderArgument}};
const auto step = decoder.push(bytes, true);
if (!step) return {step.error, {}};
if (step.value.progress != logical_decode_progress_t::complete || !step.value.value)
    return {error_code::truncated, {}};
return {error_code::ok, std::move(*step.value.value)};
}
inline result_t<std::vector<std::uint8_t>> encode_${n}(const ${n}_t& value${contextParameter}) {\nwriter_t writer;\nif (const auto error = ${internalEncode(stream.body)}(writer, value.body${bodyContextArgument}); error != error_code::ok) return {error, {}};\nif (writer.data.size() > ${unsignedLiteral(limit.maximumEncodedBytes)}) return {error_code::limit, {}};\nreturn {error_code::ok, std::move(writer.data)};\n}`;
}

const operationSyntaxEmitters = new Map([
  ["integer", emitIntegerCodec],
  ["enum", emitEnumCodec],
  ["length-prefixed", emitLengthCodec],
  ["text-validation", emitLengthCodec],
  ["field", emitFieldDecode],
  ["struct", emitStructCodec],
  ["vector", emitVectorCodec],
  ["versioned-vector", emitVersionedVectorCodec],
  ["bounded-reader", emitDelimitedCodec],
  ["versioned-length-delimited", emitDelimitedCodec],
  ["discriminator", emitConditionalCodec],
  ["conditional-union", emitConditionalCodec],
  ["tlv32", emitTlvCodec],
  ["constraint", aggregateConstraint],
  ["command-header", emitCommand],
  ["flags", emitCommand],
  ["flag-constraint", commandFlagConstraints],
  ["metadata-flag-frame", emitCommand],
  ["payload", emitCommand],
  ["durable-header", emitDurable],
  ["checksum", emitDurable],
  ["encoded-limit", emitLogical],
  ["logical-stream", emitLogical],
  ["negotiated-bound", negotiatedBoundCheck],
  ["runtime-predicate", runtimePredicate],
]);

function assertOperationCoverage(ir) {
  const expected = [...OPERATION_KINDS];
  if (
    expected.length !== operationSyntaxEmitters.size ||
    expected.some((entry) => !operationSyntaxEmitters.has(entry))
  ) {
    throw new Error(
      "C++ renderer operation emitter table differs from the closed IR vocabulary",
    );
  }
  const owners = [
    ...ir.types,
    ...ir.commands,
    ...ir.durableFormats,
    ir.relocationLogicalStreamFormat,
  ];
  function visit(value, location) {
    if (Array.isArray(value)) {
      value.forEach((entry, index) => visit(entry, `${location}[${index}]`));
    } else if (value && typeof value === "object") {
      if (value.op && !operationSyntaxEmitters.has(value.op)) {
        throw new Error(`${location}: unsupported operation ${value.op}`);
      }
      Object.entries(value).forEach(([key, entry]) =>
        visit(entry, `${location}.${key}`),
      );
    }
  }
  owners.forEach((owner) => visit(owner.operations, owner.name));
}

function publicTypeApi(owner) {
  const n = identifier(owner.name);
  return `inline result_t<${n}_t> decode_${n}(std::span<const std::uint8_t> bytes${decodeParameters(owner)}) {\nreader_t reader{bytes};\n${n}_t value{};\nif (const auto error = decode_value_${n}(reader, value${decodeParameterArguments(owner)}); error != error_code::ok) return {error, {}};\nif (!reader.empty()) return {error_code::trailing, {}};\nreturn {error_code::ok, std::move(value)};\n}\ninline result_t<std::vector<std::uint8_t>> encode_${n}(const ${n}_t& value${encodeParameters(owner)}) {\nwriter_t writer;\nif (const auto error = encode_value_${n}(writer, value${encodeParameterArguments(owner)}); error != error_code::ok) return {error, {}};\nreturn {error_code::ok, std::move(writer.data)};\n}`;
}

function render(ir) {
  types = new Map(ir.types.map((entry) => [entry.name, entry]));
  flags = new Map(ir.flags.map((entry) => [entry.name, entry.bit]));
  decoderContextFields = [
    ...new Map(
      ir.types.flatMap((owner) =>
        owner.operations
          .filter((operation) => operation.op === "negotiated-bound")
          .map((operation) => [operation.context.name, operation]),
      ),
    ).values(),
  ];
  assertOperationCoverage(ir);
  const ordered = orderedTypes(ir);
  const dtos = ordered.map(emitDto).join("\n\n");
  const codecs = ordered.map(emitTypeCodec).join("\n\n");
  const typeApis = ordered.map(publicTypeApi).join("\n\n");
  const commands = ir.commands.map(emitCommand).join("\n\n");
  const durable = ir.durableFormats.map(emitDurable).join("\n\n");
  const logical = emitLogical(ir.relocationLogicalStreamFormat);
  const hash = crypto
    .createHash("sha256")
    .update(JSON.stringify(ir))
    .digest("hex");
  return `// <auto-generated> DO NOT EDIT. operation-ir-sha256: ${hash}
#pragma once

#include "service_wire_constants.hpp"

#include <algorithm>
#include <any>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime::protocol::generated::detail {

enum class error_code : std::uint8_t {
    ok,
    truncated,
    range,
    constant,
    enum_value,
    utf8,
    header,
    flags,
    union_case,
    order,
    duplicate,
    required,
    forbidden,
    trailing,
    constraint,
    predicate,
    checksum,
    limit,
    capacity,
    context,
    invalid_state,
};

template<class T>
struct result_t {
    error_code error{};
    T value{};
    explicit operator bool() const noexcept { return error == error_code::ok; }
};

struct codec_context_t {
${decoderContextFields.map((operation) => `    std::optional<std::int64_t> ${identifier(operation.context.name)};`).join("\n")}
};

struct reader_t {
    std::span<const std::uint8_t> data;
    std::size_t limit{};
    std::size_t offset{};

    reader_t() = default;
    explicit reader_t(std::span<const std::uint8_t> bytes)
      : data{bytes}, limit{bytes.size()} {}

    bool empty() const noexcept { return offset == limit; }
    std::size_t remaining() const noexcept { return limit - offset; }
    std::size_t position() const noexcept { return offset; }

    error_code unsignedInteger(std::size_t width, std::uint64_t& value) {
        std::span<const std::uint8_t> bytes;
        if (const auto error = take(width, bytes); error != error_code::ok) return error;
        value = 0;
        for (const auto byte : bytes) value = (value << 8) | byte;
        return error_code::ok;
    }

    error_code signedInteger(std::size_t width, std::int64_t& value) {
        std::uint64_t raw{};
        if (const auto error = unsignedInteger(width, raw); error != error_code::ok) return error;
        if (width < 8 && (raw & (std::uint64_t{1} << (width * 8 - 1))) != 0)
            raw |= (~std::uint64_t{0}) << (width * 8);
        value = static_cast<std::int64_t>(raw);
        return error_code::ok;
    }

    error_code expect(std::uint8_t expected) {
        std::span<const std::uint8_t> bytes;
        if (const auto error = take(1, bytes); error != error_code::ok) return error;
        return bytes.front() == expected ? error_code::ok : error_code::header;
    }

    error_code take(std::uint64_t length, std::span<const std::uint8_t>& value) {
        if (length > remaining()) return error_code::truncated;
        value = data.subspan(offset, static_cast<std::size_t>(length));
        offset += static_cast<std::size_t>(length);
        return error_code::ok;
    }

    error_code subreader(std::uint64_t length, reader_t& value) {
        if (length > remaining() || length > std::numeric_limits<std::size_t>::max())
            return error_code::truncated;
        std::span<const std::uint8_t> bytes;
        if (const auto error = take(length, bytes); error != error_code::ok) return error;
        value = reader_t{bytes};
        return error_code::ok;
    }
};

struct writer_t {
    std::vector<std::uint8_t> data;

    error_code unsignedInteger(std::size_t width, std::uint64_t value) {
        if (width < 8 && value >= (std::uint64_t{1} << (width * 8))) return error_code::range;
        for (std::size_t index = width; index > 0; --index)
            data.push_back(static_cast<std::uint8_t>(value >> ((index - 1) * 8)));
        return error_code::ok;
    }

    error_code signedInteger(std::size_t width, std::int64_t value) {
        return unsignedInteger(width, static_cast<std::uint64_t>(value));
    }

    void bytes(std::span<const std::uint8_t> value) {
        data.insert(data.end(), value.begin(), value.end());
    }
};

inline bool canonicalAuthorityUnreserved(std::uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
        || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_'
        || byte == '~';
}

inline void appendCanonicalAuthorityComponent(std::string& key, std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    key += std::to_string(value.size());
    key.push_back(':');
    for (const auto character : value) {
        const auto byte = static_cast<std::uint8_t>(character);
        if (canonicalAuthorityUnreserved(byte)) {
            key.push_back(character);
        } else {
            key.push_back('%');
            key.push_back(hex[byte >> 4]);
            key.push_back(hex[byte & 0x0f]);
        }
    }
}

inline bool validUtf8(std::string_view value, bool forbidNul) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = bytes[index++];
        if (first == 0 && forbidNul) return false;
        if (first <= 0x7f) continue;
        std::uint32_t codePoint{};
        std::size_t continuation{};
        if (first >= 0xc2 && first <= 0xdf) { codePoint = first & 0x1f; continuation = 1; }
        else if (first >= 0xe0 && first <= 0xef) { codePoint = first & 0x0f; continuation = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { codePoint = first & 0x07; continuation = 3; }
        else return false;
        if (continuation > value.size() - index) return false;
        for (std::size_t part = 0; part < continuation; ++part) {
            const auto next = bytes[index++];
            if ((next & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && codePoint < 0x80)
            || (continuation == 2 && codePoint < 0x800)
            || (continuation == 3 && codePoint < 0x10000)
            || (codePoint >= 0xd800 && codePoint <= 0xdfff)
            || codePoint > 0x10ffff) return false;
    }
    return true;
}

inline std::uint32_t crc32c(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0x82f63b78u : 0u);
    }
    return ~crc;
}

struct unknown_tlv_field_t {
    std::uint64_t id{};
    std::vector<std::uint8_t> value;
    bool operator==(const unknown_tlv_field_t&) const = default;
};
struct tlv_entry_t { std::uint64_t id{}; std::vector<std::uint8_t> value; };

${dtos}

${codecs}

${typeApis}

${commands}

${durable}

${logical}

} // namespace zlink::framework::runtime::protocol::generated::detail
`;
}

const [mode, schemaArgument, outputArgument, ...extraArguments] =
  process.argv.slice(2);
if (
  !["--write", "--check"].includes(mode) ||
  !schemaArgument ||
  !outputArgument ||
  extraArguments.length > 0
) {
  throw new Error(
    "usage: node render-service-wire-cpp.mjs --write|--check <schema> <output>",
  );
}
const output = path.resolve(outputArgument);
const content = render(lowerSchema(path.resolve(schemaArgument))).replaceAll(
  /decltype\(([^)]+)::value\)/g,
  "decltype($1{}.value)",
);
if (mode === "--write") {
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, content);
} else if (
  !fs.existsSync(output) ||
  fs.readFileSync(output, "utf8") !== content
) {
  process.exitCode = 1;
}

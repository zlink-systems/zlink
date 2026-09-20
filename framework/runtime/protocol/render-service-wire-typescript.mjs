#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema } from "./service-wire-lowering.mjs";

const id = (name) => name.split(/[^A-Za-z0-9]+/).filter(Boolean)
  .map((part) => part[0].toUpperCase() + part.slice(1)).join("");
const prop = (name) => /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name) ? name : JSON.stringify(name);
const at = (base, name) => `${base}[${JSON.stringify(name)}]`;
const indent = (text, width = 2) => text.split("\n")
  .map((line) => line ? " ".repeat(width) + line : line).join("\n");
const literal = (value) => typeof value === "bigint" ? `${value}n` : JSON.stringify(value);
let ir;
let types;
let flags;

function op(owner, name) {
  const value = owner.operations.find((entry) => entry.op === name);
  if (!value) throw new Error(`${owner.name}: missing operation ${name}`);
  return value;
}
function root(owner) {
  const value = owner.operations.find((entry) => emitters[entry.op]?.root);
  if (!value) throw new Error(`${owner.name}: missing root operation`);
  return value;
}
const refName = (reference) => id(reference.$ref);
const readRef = (reference, reader = "reader", enclosing = "enclosing", flagValue = "flags") =>
  `read${refName(reference)}(${reader}, context, ${enclosing}, ${flagValue})`;
const writeRef = (reference, value, writer = "writer", enclosing = "enclosing", flagValue = "flags") => {
  const target = types?.get(reference.$ref);
  const targetRoot = target?.operations.find((entry) => entry.op === "integer");
  const expression = targetRoot?.encoding.endsWith("64") ? `numeric(${value})` : value;
  return `write${refName(reference)}(${expression}, ${writer}, context, ${enclosing}, ${flagValue})`;
};
const topRead = (reference, bytes) => `decode${refName(reference)}(${bytes}, context)`;
const topWrite = (reference, value) => `encode${refName(reference)}(${value}, context)`;
const mask = (operands) => operands.reduce((value, entry) => value | flags.get(entry.name).bit, 0);

function condition(value, valueName = "value", flagValue = "flags") {
  return value.all.map((atom) => {
    if (atom.kind === "fieldPresent") {
      if (atom.presence?.internal?.absent !== null || atom.presence.internal.present !== "non-null") throw new Error(`unsupported field presence ${atom.operand.name}`);
      return `${at(valueName, atom.operand.name)} !== null`;
    }
    if (atom.kind === "fieldEquals") return `same(${at(valueName, atom.operand.name)}, ${literal(atom.value)})`;
    if (atom.kind === "contextEquals") return `same(requireContext(context, ${JSON.stringify(atom.operand.name)}), ${literal(atom.value)})`;
    const bits = mask(atom.operands);
    return atom.kind === "allFlagsSet" ? `((${flagValue} & ${bits}) === ${bits})` : `((${flagValue} & ${bits}) !== 0)`;
  }).join(" && ") || "true";
}

function bounds(field, expression) {
  const checks = [];
  if (field.minimum !== undefined) checks.push(`numeric(${expression}) < ${BigInt(field.minimum)}n`);
  if (field.maximum !== undefined) checks.push(`numeric(${expression}) > ${BigInt(field.maximum)}n`);
  if (field.constant !== undefined) checks.push(`!same(${expression}, ${literal(field.constant)})`);
  return checks.length ? `if (${checks.join(" || ")}) fail(${JSON.stringify(field.name + " constraint")});` : "";
}
function fieldConstraints(field, expression) {
  return (field.constraints ?? []).map((constraint) => {
    if (constraint.kind !== "contains-protocol-required-capability") throw new Error(`unsupported field constraint ${constraint.kind}`);
    return `if (!${expression}.includes(${JSON.stringify(ir.protocol[constraint.requiredCapability.name])})) fail(${JSON.stringify(field.name + " required capability")});`;
  }).join("\n");
}
function readField(field, reader = "reader", value = "value", flagValue = "flags") {
  const expression = at(value, field.name);
  const body = `${expression} = ${readRef(field.type, reader, value, flagValue)};\n${bounds(field, expression)}\n${fieldConstraints(field, expression)}`;
  return field.when ? `if (${condition(field.when, value, flagValue)}) {\n${indent(body)}\n}` : body;
}
function writeField(field, writer = "writer", value = "value", flagValue = "flags") {
  const expression = at(value, field.name);
  const body = `if (${expression} === undefined) fail(${JSON.stringify(field.name + " required")});\n${bounds(field, expression)}\n${fieldConstraints(field, expression)}\n${writeRef(field.type, expression, writer, value, flagValue)};`;
  if (!field.when) return body;
  const absent = field.when.all.find((atom) => atom.kind === "fieldPresent")?.presence?.internal?.absent;
  const forbidden = absent === undefined ? `${expression} !== undefined` : `${expression} !== undefined && !same(${expression}, ${literal(absent)})`;
  return `if (${condition(field.when, value, flagValue)}) {\n${indent(body)}\n} else if (${forbidden}) fail(${JSON.stringify(field.name + " forbidden")});`;
}
function fieldsType(value) {
  return value.length ? `{ ${value.map((field) => `readonly ${prop(field.name)}${field.when || field.required === false ? "?" : ""}: ${refName(field.type)}`).join("; ")} }` : "Record<string, never>";
}

const pathValue = (base, pathValueString) => pathValueString.split(".")
  .reduce((value, segment) => at(value, segment), base);
function constraintRootType(owner) {
  const operation = root(owner);
  if (operation.op === "vector") return types.get(operation.item.$ref);
  if (operation.op === "versioned-vector") return types.get(operation.layout.find((entry) => entry.kind === "repeat").item.$ref);
  return owner;
}
function constraintPathType(owner, pathName) {
  let current = constraintRootType(owner);
  if (!pathName) return current;
  for (const segment of pathName.split(".")) {
    const field = (root(current).fields ?? []).find((entry) => entry.name === segment);
    if (!field) throw new Error(`${owner.name}.${pathName}: unresolved constraint path`);
    current = types.get(field.type.$ref);
  }
  return current;
}
function keySource(base, source) {
  if (source.kind === "item") return base;
  if (source.kind === "fieldPath") return pathValue(base, source.path);
  throw new Error(`unsupported comparison source ${source.kind}`);
}
function authorityKey(owner, key, base) {
  if (key.format.encoding !== "canonical-ascii-utf8" || key.format.componentLayout !== "decimal-raw-byte-length-colon-percent-encoded-bytes"
      || key.format.decimalLength !== "base10-no-leading-zero" || key.format.escaping !== "rfc3986-unreserved-literal-otherwise-uppercase-percent-hex"
      || key.format.unicodeNormalization !== "none") throw new Error(`${owner.name}: unsupported authority key format`);
  const value = keySource(base, key.source);
  const variants = Object.entries(key.variants).map(([variantName, variant]) => {
    const discriminator = key.format.kindDiscriminators.find((entry) => entry.objectKind === variant.objectKind);
    if (!discriminator) throw new Error(`${owner.name}: missing authority key discriminator ${variant.objectKind}`);
    const components = variant.components.map((pathName) => `utf8(${pathValue(value, pathName)})`);
    return `same(${at(value, "objectKind")}, ${JSON.stringify(variantName)}) ? canonicalAuthorityKey(${JSON.stringify(key.format.prefix)}, ${JSON.stringify(key.format.separator)}, ${JSON.stringify(discriminator.wire)}, [${components}], ${key.format.componentRawBytesMinimum}, ${key.format.componentRawBytesMaximum}, ${key.format.maximumEncodedBytes})`;
  });
  return `(${variants.join(" : ")} : fail(${JSON.stringify(owner.name + " authority key variant")}))`;
}
function keyPartComparison(owner, part, left, right) {
  const a = keySource(left, part.source); const b = keySource(right, part.source);
  if (part.kind === "unsigned-wire-value") return `compareBigint(numeric(${a}), numeric(${b}))`;
  if (part.kind === "wire-value") {
    const valueType = constraintPathType(owner, part.source.kind === "item" ? "" : part.source.path);
    const wire = root(valueType).op === "enum" ? `enumWire${id(valueType.name)}` : "numeric";
    return `compareBigint(numeric(${wire}(${a})), numeric(${wire}(${b})))`;
  }
  if (part.kind === "utf-8-bytes") return `compareBytes(utf8(${a}), utf8(${b}))`;
  throw new Error(`${owner.name}: unsupported comparison key part ${part.kind}`);
}
function compare(owner, constraint, left, right) {
  if (constraint.key?.kind === "canonical-authority-key-bytes") return `compareBytes(${authorityKey(owner, constraint.key, left)}, ${authorityKey(owner, constraint.key, right)})`;
  if (constraint.key?.kind === "tuple") return `compareResults([${constraint.key.parts.map((part) => keyPartComparison(owner, part, left, right)).join(", ")}])`;
  throw new Error(`${owner.name}: unsupported comparison key ${constraint.key?.kind}`);
}
function collectionChecks(owner, constraints, value) {
  return constraints.map((constraint) => {
    if (constraint.kind === "sorted") return `for (let index = 1; index < ${value}.length; ++index) if (${compare(owner, constraint, `${value}[index - 1]`, `${value}[index]`)} >= 0) fail(${JSON.stringify(owner.name + " sorted")});`;
    if (constraint.kind === "unique") {
      return `for (let left = 0; left < ${value}.length; ++left) for (let right = left + 1; right < ${value}.length; ++right) if (${compare(owner, constraint, `${value}[left]`, `${value}[right]`)} === 0) fail(${JSON.stringify(owner.name + " unique")});`;
    }
    throw new Error(`${owner.name}: unsupported collection constraint ${constraint.kind}`);
  }).join("\n");
}
function objectChecks(owner, constraints, value = "value") {
  return constraints.map((constraint) => {
    if (constraint.kind === "not-both-zero") return `if (${constraint.fields.map((field) => `numeric(${at(value, field.name)}) === 0n`).join(" && ")}) fail(${JSON.stringify(owner.name + " all zero")});`;
    if (constraint.kind === "field-less-than-or-equal") return `if (numeric(${at(value, constraint.left.name)}) > numeric(${at(value, constraint.right.name)})) fail(${JSON.stringify(owner.name + " field order")});`;
    const when = Object.entries(constraint.when ?? {}).map(([name, expected]) => {
      if (name.endsWith("Not")) return `!same(${pathValue(value, name.slice(0, -3))}, ${literal(expected)})`;
      return `same(${pathValue(value, name)}, ${literal(expected)})`;
    }).join(" && ");
    const requires = Object.entries(constraint.requires ?? {}).map(([name, expected]) => `same(${pathValue(value, name)}, ${literal(expected)})`).join(" && ");
    if (["terminal-success-shape", "terminal-failure-shape", "existing-has-no-application-payload"].includes(constraint.kind)) return `if (${when} && !(${requires})) fail(${JSON.stringify(owner.name + " " + constraint.kind)});`;
    throw new Error(`${owner.name}: unsupported object constraint ${constraint.kind}`);
  }).join("\n");
}
function caseConstraint(owner, operation) {
  if (operation.owner?.kind !== "conditional-union-case" || !Array.isArray(operation.owner.path)) throw new Error(`${owner.name}: invalid conditional-union constraint owner`);
  return objectChecks({ ...owner, name: operation.owner.path.join(".") }, [operation]);
}
function encodedLimit(owner, operation, measurements) {
  if (operation.measured !== "complete-encoded-value" || operation.exceeded !== "protocol-error") throw new Error(`${owner.name}: unsupported encoded limit`);
  return Object.fromEntries(operation.applications.map((application) => {
    const measured = measurements[application];
    if (!measured) throw new Error(`${owner.name}: missing encoded-limit ${application} measurement`);
    return [application, `if (${measured} > ${operation.maximumEncodedBytes}) fail(${JSON.stringify(owner.name + " maximum")});`];
  }));
}
function encodeCapacity(owner, policy, measurements) {
  if (!Number.isSafeInteger(policy?.declaredMaximumBytes)
      || !Number.isSafeInteger(policy.representationCeiling)
      || !Number.isSafeInteger(policy.requiredThroughBytes)
      || !Array.isArray(policy.applications)) throw new Error(`${owner.name}: invalid encode capacity`);
  const rejection = (kind, message) => {
    if (kind === "protocol-error") return `fail(${JSON.stringify(message)})`;
    if (kind === "capacity-error") return `capacityFail(${JSON.stringify(message)})`;
    throw new Error(`${owner.name}: unsupported encode capacity rejection ${kind}`);
  };
  return Object.fromEntries(policy.applications.map((application) => {
    const measured = measurements[application];
    if (!measured) throw new Error(`${owner.name}: missing encode capacity ${application} measurement`);
    return [application, `if (${measured} > ${policy.declaredMaximumBytes}) ${rejection(policy.aboveDeclaredMaximum, owner.name + " length")}; if (${measured} > ${policy.representationCeiling}) ${rejection(policy.aboveRepresentationCeiling, owner.name + " capacity")};`];
  }));
}
function negotiatedBound(owner, operation, measurements) {
  if (operation.topology !== "clientServer" || operation.comparison !== "less-than-or-equal"
      || operation.context?.missing !== "protocol-error" || operation.context?.negative !== "protocol-error"
      || operation.context?.aboveAbsoluteMaximum !== "protocol-error") throw new Error(`${owner.name}: unsupported negotiated bound`);
  return Object.fromEntries(Object.entries(operation.applications).map(([application, applied]) => {
    const measured = measurements[application]?.[operation.measured];
    if (!measured || applied.context?.kind !== `${application}r-context`) throw new Error(`${owner.name}: unsupported negotiated ${application} application`);
    const syntax = `const negotiatedMaximum = numeric(requireContext(context, ${JSON.stringify(applied.context.name)})); if (negotiatedMaximum < 0n || negotiatedMaximum > ${BigInt(operation.context.absoluteMaximum)}n || BigInt(${measured}) > negotiatedMaximum) fail(${JSON.stringify(owner.name + " negotiated maximum")});`;
    return [application, syntax];
  }));
}
function predicateChecks(owner, value = "value") {
  return owner.operations.filter((entry) => entry.op === "runtime-predicate").flatMap((entry) => {
    if (entry.reference.asset !== "service-wire-constants" || entry.reference.name !== "valid-terminal-failure") throw new Error(`${owner.name}: unsupported runtime predicate`);
    return entry.targets.map((target) => {
      let local = target.path.startsWith(`${owner.name}.`) ? target.path.slice(owner.name.length + 1) : target.path;
      let guard = "";
      const layout = owner.operations.find((operation) => operation.op === "conditional-union");
      if (layout) {
        const [caseName, ...remaining] = local.split(".");
        const selected = Object.keys(layout.cases).map((signature) => JSON.parse(signature))
          .find((signature) => Object.values(signature).includes(caseName));
        if (selected) {
          guard = Object.entries(selected).map(([name, expected]) => `same(${at(value, name)}, ${literal(expected)})`).join(" && ");
          local = remaining.join(".");
        }
      }
      const failure = pathValue(value, local); const terminal = pathValue(value, local.replace(/failureCode$/, "terminalResult"));
      const valid = `runtimePredicate(context, ${JSON.stringify(`${entry.reference.asset}.${entry.reference.name}`)}, enumWireRequestTerminalResult(${terminal}), enumWireFrameworkErrorCode(${failure}))`;
      return `if (${guard ? `${guard} && !(${valid})` : `!(${valid})`}) fail(${JSON.stringify(owner.name + " runtime predicate")});`;
    });
  }).join("\n");
}

function scalar(owner, operation) {
  const name = id(owner.name); const wide = operation.encoding.endsWith("64");
  const read = operation.encoding === "i64" ? "reader.i64()" : `reader.u(${operation.width})`;
  const write = operation.encoding === "i64" ? "writer.i64(raw)" : `writer.u(raw, ${operation.width})`;
  return { type: `export type ${name} = ${wide ? "bigint" : "number"};`,
    read: `const raw = ${read}; if (raw < ${BigInt(operation.minimum)}n || raw > ${BigInt(operation.maximum)}n) fail(${JSON.stringify(owner.name + " range")}); return ${wide ? "raw" : "Number(raw)"};`,
    write: `const raw = numeric(value); if (raw < ${BigInt(operation.minimum)}n || raw > ${BigInt(operation.maximum)}n) fail(${JSON.stringify(owner.name + " range")}); ${write};` };
}
function enumeration(owner, operation) {
  const name = id(owner.name); const wide = operation.encoding.endsWith("64");
  const values = operation.values.map((entry) => JSON.stringify(entry.name)).join(" | ");
  const decode = operation.values.map((entry) => `case ${wide ? `${entry.value}n` : entry.value}: return ${JSON.stringify(entry.name)};`).join("\n");
  const encode = operation.values.map((entry) => `case ${JSON.stringify(entry.name)}: return ${wide ? `${entry.value}n` : entry.value};`).join("\n");
  return { type: `export type ${name} = ${values};`, extra: `function enumWire${name}(value: ${name}): ${wide ? "bigint" : "number"} { switch (value) {\n${indent(encode)}\n} }`,
    read: `const raw = ${operation.encoding === "i64" ? "reader.i64()" : `reader.u(${operation.width})`}; switch (${wide ? "raw" : "Number(raw)"}) {\n${indent(decode)}\n  default: fail(${JSON.stringify(owner.name + " enum")});\n}`,
    write: `${operation.encoding === "i64" ? "writer.i64" : "writer.u"}(numeric(enumWire${name}(value))${operation.encoding === "i64" ? "" : `, ${operation.width}`});` };
}
function lengthPrefixed(owner, operation) {
  const name = id(owner.name); const nullable = operation.zeroLengthMeaning === "absent";
  const text = operation.content === "text"; const validation = owner.operations.find((entry) => entry.op === "text-validation");
  const capacity = encodeCapacity(owner, operation.encodeCapacity, { encode: "bytes.length", decode: "length" });
  if (text && (validation?.encoding !== "utf-8" || validation.malformed !== "protocol-error"
      || validation.decode?.bom !== "preserve" || validation.decode.overlong !== "protocol-error"
      || validation.decode.surrogateCodePoint !== "protocol-error" || validation.encode?.loneSurrogate !== "protocol-error")) throw new Error(`${owner.name}: unsupported text validation`);
  const decode = text ? `const bytes = reader.take(length); ${validation?.nul === "forbidden" ? `if (bytes.includes(0)) fail(${JSON.stringify(owner.name + " NUL")});` : ""} return decodeUtf8(bytes, ${JSON.stringify(owner.name)});` : "return reader.take(length);";
  const negotiated = owner.operations.filter((entry) => entry.op === "negotiated-bound")
    .map((entry) => emitters[entry.op].syntax(owner, entry, {
      encode: { "content-bytes": "bytes.length" }, decode: { "content-bytes": "length" },
    }));
  const decodeNegotiated = negotiated.map((entry) => entry.decode).join("\n");
  const encodeNegotiated = negotiated.map((entry) => entry.encode).join("\n");
  const encodeCheck = encodeNegotiated ? ` ${encodeNegotiated}` : "";
  return { type: `export type ${name} = ${text ? "string" : "Uint8Array"}${nullable ? " | null" : ""};`,
    read: `const length = Number(${readRef(operation.lengthType)}); ${nullable ? "if (length === 0) return null;" : ""} ${capacity.decode} if (length < ${operation.minimumBytes}) fail(${JSON.stringify(owner.name + " length")}); ${decodeNegotiated} ${decode}`,
    write: `${nullable ? `if (value === null) { ${writeRef(operation.lengthType, "0")}; return; }` : ""} const bytes = ${text ? "utf8(value)" : "value as Uint8Array"}; ${text && validation?.nul === "forbidden" ? `if (bytes.includes(0)) fail(${JSON.stringify(owner.name + " NUL")});` : ""} ${capacity.encode} if (bytes.length < ${operation.minimumBytes}) fail(${JSON.stringify(owner.name + " length")}); ${writeRef(operation.lengthType, "bytes.length")}; writer.put(bytes);${encodeCheck}` };
}
function structure(owner, operation) {
  const checks = objectChecks(owner, operation.constraints);
  return { type: `export type ${id(owner.name)} = ${fieldsType(operation.fields)};`,
    read: `const value: any = {};\n${operation.fields.map((field) => readField(field)).join("\n")}\n${checks}\n${predicateChecks(owner)}\nreturn value;`,
    write: `${checks}\n${predicateChecks(owner)}\n${operation.fields.map((field) => writeField(field)).join("\n")}` };
}
function vector(owner, operation) {
  const checks = collectionChecks(owner, operation.constraints, "value");
  return { type: `export type ${id(owner.name)} = readonly ${refName(operation.item)}[];`,
    read: `const count = Number(${readRef(operation.countType)}); ${operation.maximumItems === undefined ? "" : `if (count > ${operation.maximumItems}) fail(${JSON.stringify(owner.name + " count")});`} const value: any = Array.from({ length: count }, () => ${readRef(operation.item)});\n${checks}\nreturn value;`,
    write: `if (!Array.isArray(value)${operation.maximumItems === undefined ? "" : ` || value.length > ${operation.maximumItems}`}) fail(${JSON.stringify(owner.name + " count")});\n${checks}\n${writeRef(operation.countType, "value.length")}; for (const item of value) ${writeRef(operation.item, "item")};` };
}
function versionedVector(owner, operation) {
  const repeat = operation.layout.find((entry) => entry.kind === "repeat");
  const checks = collectionChecks(owner, operation.constraints, `value.${prop(repeat.name)}`);
  const reads = operation.layout.map((entry) => entry.kind === "repeat"
    ? `value.${prop(entry.name)} = Array.from({ length: Number(layout.${prop(entry.countFrom)}) }, () => ${readRef(entry.item)});`
    : `layout.${prop(entry.name)} = ${readRef(entry)};${entry.constant === undefined ? "" : ` if (!same(layout.${prop(entry.name)}, ${literal(entry.constant)})) fail(${JSON.stringify(owner.name + " constant")});`}`).join("\n");
  const writes = operation.layout.map((entry) => entry.kind === "repeat" ? `for (const item of value.${prop(entry.name)}) ${writeRef(entry.item, "item")};` : `${writeRef(entry, entry.constant === undefined ? `value.${prop(repeat.name)}.length` : literal(entry.constant))};`).join("\n");
  return { type: `export type ${id(owner.name)} = { readonly ${prop(repeat.name)}: readonly ${refName(repeat.item)}[] };`,
    read: `const layout: any = {}; const value: any = {};\n${reads}\n${checks}\nreturn value;`,
    write: `${checks}\n${writes}` };
}
function versionedLength(owner, operation) {
  const checks = objectChecks(owner, operation.constraints);
  const negotiated = owner.operations.filter((entry) => entry.op === "negotiated-bound")
    .map((entry) => emitters[entry.op].syntax(owner, entry, {
      encode: { "encoded-bytes": "writer.length - encodedStart" },
      decode: { "encoded-bytes": "reader.offset - encodedStart" },
    }));
  const decodeNegotiated = negotiated.map((entry) => entry.decode).join("\n");
  const encodeNegotiated = negotiated.map((entry) => entry.encode).join("\n");
  return { type: `export type ${id(owner.name)} = ${fieldsType(operation.fields)};`,
    read: `${negotiated.length ? "const encodedStart = reader.offset; " : ""}const version = ${readRef(operation.version)}; if (!same(version, ${literal(operation.version.constant)})) fail(${JSON.stringify(owner.name + " version")}); const length = Number(${readRef(operation.length)}); const body = reader.bounded(length); const value: any = {};\n${operation.fields.map((field) => readField(field, "body")).join("\n")}\nbody.done(${JSON.stringify(owner.name)});\n${decodeNegotiated}\n${checks}\n${predicateChecks(owner)}\nreturn value;`,
    write: `${negotiated.length ? "const encodedStart = writer.length;\n" : ""}${checks}\n${predicateChecks(owner)}\n${writeRef(operation.version, literal(operation.version.constant))}; const body = new Writer();\n${operation.fields.map((field) => writeField(field, "body")).join("\n")}\nconst bytes = body.result(); ${writeRef(operation.length, "bytes.length")}; writer.put(bytes);\n${encodeNegotiated}` };
}
function discriminator(entry, reader) {
  if (entry.source.kind === "wire") return readRef(entry.type, reader);
  if (entry.source.kind === "enclosingField") return at("enclosing", entry.source.name);
  if (entry.source.kind === "context") return `requireContext(context, ${JSON.stringify(entry.source.name)})`;
  throw new Error(`unsupported discriminator source ${entry.source.kind}`);
}
function selection(owner, operation, body) {
  const branches = Object.entries(operation.cases).map(([signature, selected], index) => {
    const test = Object.entries(JSON.parse(signature)).map(([name, value]) => `same(${at("value", name)}, ${literal(value)})`).join(" && ");
    return `${index ? "else if" : "if"} (${test}) {\n${indent(body(selected, signature))}\n}`;
  });
  branches.push(operation.otherwise.kind === "fields" ? `else {\n${indent(body({ operations: operation.otherwise.fields }))}\n}` : `else fail(${JSON.stringify(owner.name + " discriminator")});`);
  return branches.join(" ");
}
function selectedSyntax(owner, selected, direction) {
  return selected.operations.map((operation) => {
    const syntax = emitters[operation.op]?.[direction];
    if (!syntax) throw new Error(`${owner.name}: operation ${operation.op} has no ${direction} syntax`);
    return syntax(owner, operation);
  }).join("\n");
}
function selectedProperties(owner, selected) {
  return selected.operations.flatMap((operation) => {
    const syntax = emitters[operation.op]?.property;
    if (syntax === undefined) return [];
    return [syntax(owner, operation)];
  });
}
function variantAgreement(owner, operation, selected) {
  if (operation.encode?.selection !== "variant" || operation.encode.discriminatorAgreement !== "required" || operation.encode.mismatch !== "protocol-error") throw new Error(`${owner.name}: unsupported union encode agreement`);
  const selectedFields = new Set(selected.operations.filter((entry) => entry.op === "field").map((entry) => entry.name));
  const otherFields = [...new Set(Object.values(operation.cases).flatMap((entry) => entry.operations)
    .filter((entry) => entry.op === "field" && !selectedFields.has(entry.name)).map((entry) => entry.name))];
  return otherFields.length ? `if (${otherFields.map((name) => `${at("value", name)} !== undefined`).join(" || ")}) fail(${JSON.stringify(owner.name + " discriminator agreement")});` : "";
}
function conditionalUnion(owner, operation) {
  const variants = Object.entries(operation.cases).map(([signature, selected]) => `{ ${Object.entries(JSON.parse(signature)).map(([name, value]) => `readonly ${prop(name)}: ${JSON.stringify(value)}`).concat(selectedProperties(owner, selected)).join("; ")} }`);
  if (operation.otherwise.kind === "fields") variants.push(fieldsType([...operation.discriminators.map((entry) => ({ name: entry.name, type: entry.type })), ...operation.otherwise.fields]));
  const readCases = selection(owner, operation, (selected) => selectedSyntax(owner, selected, "caseRead"));
  const writeCases = selection(owner, operation, (selected) => `${variantAgreement(owner, operation, selected)}\n${selectedSyntax(owner, selected, "caseWrite")}`);
  const readDiscriminators = operation.discriminators.map((entry) => `${at("value", entry.name)} = ${discriminator(entry, "reader")};`).join("\n");
  const writeDiscriminators = operation.discriminators.filter((entry) => entry.source.kind === "wire").map((entry) => `${writeRef(entry.type, at("value", entry.name))};`).join("\n");
  return { type: `export type ${id(owner.name)} = ${variants.join(" | ")};`,
    read: `const value: any = {};\n${readDiscriminators}\n${operation.bodyLengthType ? `const body = reader.bounded(Number(${readRef(operation.bodyLengthType)}));` : "const body = reader;"}\n${readCases}\n${operation.bodyLengthType ? `body.done(${JSON.stringify(owner.name)});` : ""}\n${predicateChecks(owner)}\nreturn value;`,
    write: `${predicateChecks(owner)}\n${writeDiscriminators}\n${operation.bodyLengthType ? "const body = new Writer();" : "const body = writer;"}\n${writeCases}\n${operation.bodyLengthType ? `const bytes = body.result(); ${writeRef(operation.bodyLengthType, "bytes.length")}; writer.put(bytes);` : ""}` };
}

function presenceChecks(operation) {
  return operation.presenceRules.map((rule) => {
    const checks = [...(rule.require ?? []).map((name) => `${at("value", name)} === undefined`), ...(rule.forbid ?? []).map((name) => `${at("value", name)} !== undefined`)];
    return checks.length ? `if (${condition(rule.when)} && (${checks.join(" || ")})) fail("tlv presence rule");` : "";
  }).join("\n");
}
function tlv(owner, operation) {
  const required = operation.requiredFields.map((name) => `${at("value", name)} === undefined`).join(" || ");
  const cases = operation.fields.map((field) => `case ${field.id}:\n${indent(`if (${at("value", field.name)} !== undefined) fail(${JSON.stringify(owner.name + " duplicate")}); ${at("value", field.name)} = ${readRef(field.type, "item", "value")};\n${bounds(field, at("value", field.name))}\n${fieldConstraints(field, at("value", field.name))}\nitem.done(${JSON.stringify(field.name)}); break;`, 6)}`).join("\n");
  const writes = operation.fields.map((field) => `if (${at("value", field.name)} !== undefined) {\n${indent(`${bounds(field, at("value", field.name))}\n${fieldConstraints(field, at("value", field.name))}\nconst item = new Writer(); ${writeRef(field.type, at("value", field.name), "item", "value")}; const itemBytes = item.result(); ${writeRef(operation.fieldIdType, String(field.id), "body", "value")}; ${writeRef(operation.fieldLengthType, "itemBytes.length", "body", "value")}; body.put(itemBytes);`)}\n}`).join("\n");
  return { type: `export type ${id(owner.name)} = { ${operation.fields.map((field) => `readonly ${prop(field.name)}${field.required ? "" : "?"}: ${refName(field.type)}`).join("; ")} };`,
    read: `const total = Number(${readRef(operation.totalLengthType)}); const body = reader.bounded(total); const value: any = {}; let previous = -1;\nwhile (body.remaining > 0) { const fieldId = Number(${readRef(operation.fieldIdType, "body", "value")}); const length = Number(${readRef(operation.fieldLengthType, "body", "value")}); if (fieldId <= previous) fail(${JSON.stringify(owner.name + " order")}); previous = fieldId; const item = body.bounded(length); switch (fieldId) {\n${indent(cases)}\n  default: item.take(item.remaining); break;\n} }\n${required ? `if (${required}) fail(${JSON.stringify(owner.name + " required")});` : ""}\n${presenceChecks(operation)}\nreturn value;`,
    write: `${required ? `if (${required}) fail(${JSON.stringify(owner.name + " required")});` : ""}\n${presenceChecks(operation)}\nconst body = new Writer();\n${writes}\nconst bytes = body.result(); ${writeRef(operation.totalLengthType, "bytes.length")}; writer.put(bytes);` };
}

const emitters = {
  integer: { root: true, syntax: scalar, render: scalar },
  enum: { root: true, syntax: enumeration, render: enumeration },
  "length-prefixed": { root: true, syntax: lengthPrefixed, render: lengthPrefixed },
  "text-validation": { syntax: lengthPrefixed },
  field: { syntax: readField, property: (_owner, operation) => `readonly ${prop(operation.name)}${operation.when ? "?" : ""}: ${refName(operation.type)}`, caseRead: (_owner, operation) => readField(operation, "body"), caseWrite: (_owner, operation) => writeField(operation, "body") },
  struct: { root: true, syntax: structure, render: structure },
  vector: { root: true, syntax: vector, render: vector },
  "versioned-vector": { root: true, syntax: versionedVector, render: versionedVector },
  "bounded-reader": { syntax: versionedLength },
  "versioned-length-delimited": { root: true, syntax: versionedLength, render: versionedLength },
  discriminator: { syntax: discriminator },
  "conditional-union": { root: true, syntax: conditionalUnion, render: conditionalUnion },
  tlv32: { root: true, syntax: tlv, render: tlv },
  constraint: { syntax: objectChecks, caseRead: caseConstraint, caseWrite: caseConstraint },
  "command-header": { syntax: renderCommand },
  flags: { syntax: flagChecks },
  "flag-constraint": { syntax: flagChecks },
  "metadata-flag-frame": { syntax: renderCommand },
  payload: { syntax: renderCommand },
  "durable-header": { syntax: renderDurable },
  checksum: { syntax: renderDurable },
  "encoded-limit": { syntax: encodedLimit },
  "logical-stream": { syntax: renderLogical },
  "negotiated-bound": { syntax: negotiatedBound },
  "runtime-predicate": { syntax: predicateChecks },
};
function verifyVocabulary() {
  if (JSON.stringify(Object.keys(emitters).sort()) !== JSON.stringify([...ir.operationVocabulary].sort())) throw new Error("TypeScript operation vocabulary mismatch");
  for (const [name, emitter] of Object.entries(emitters)) if (typeof emitter.syntax !== "function") throw new Error(`TypeScript operation ${name} has no syntax emitter`);
  const inspect = (value, owner) => {
    if (Array.isArray(value)) return value.forEach((entry) => inspect(entry, owner));
    if (!value || typeof value !== "object") return;
    if (value.op && !emitters[value.op]) throw new Error(`${owner}: unsupported operation ${value.op}`);
    Object.values(value).forEach((entry) => inspect(entry, owner));
  };
  [...ir.types, ...ir.commands, ...ir.durableFormats, ir.relocationLogicalStreamFormat].forEach((owner) => inspect(owner.operations, owner.name));
}
function renderType(owner) {
  const rendered = emitters[root(owner).op].render(owner, root(owner)); const name = id(owner.name);
  const limit = owner.operations.find((entry) => entry.op === "encoded-limit");
  const limitSyntax = limit ? emitters[limit.op].syntax(owner, limit, {
    encode: "writer.length - limitStart", decode: "reader.offset - limitStart",
  }) : null;
  const read = limit
    ? `const limitStart = reader.offset; const decoded = (() => {\n${indent(rendered.read)}\n})();\n${limitSyntax.decode}\nreturn decoded;`
    : rendered.read;
  const write = limit
    ? `const limitStart = writer.length;\n${rendered.write}\n${limitSyntax.encode}`
    : rendered.write;
  return `${rendered.type}\n${rendered.extra ?? ""}\nfunction read${name}(reader: Reader, context: ServiceWireDecoderContext, enclosing: any, flags: number): ${name} {\n  void context; void enclosing; void flags;\n${indent(read)}\n}\nfunction write${name}(input: ${name}, writer: Writer, context: ServiceWireDecoderContext, enclosing: any, flags: number): void {\n  void context; void enclosing; void flags; const value: any = input;\n${indent(write)}\n}\nexport function decode${name}(bytes: Uint8Array, context: ServiceWireDecoderContext): ${name} { const reader = new Reader(bytes); const value = read${name}(reader, context, {}, 0); reader.done(${JSON.stringify(owner.name)}); return value; }\nexport function encode${name}(value: ${name}, context: ServiceWireDecoderContext): Uint8Array { const writer = new Writer(); write${name}(value, writer, context, {}, 0); return writer.result(); }`;
}

function flagChecks(command, expression) {
  const operation = op(command, "flags"); const allowed = mask(operation.allowed); const required = mask(operation.required);
  const lines = [`if ((${expression} & ~${allowed}) !== 0 || (${expression} & ${required}) !== ${required}) fail(${JSON.stringify(command.name + " flags")});`];
  for (const constraint of command.operations.filter((entry) => entry.op === "flag-constraint")) {
    if (constraint.kind === "all-or-none") { const bits = mask(constraint.flags); lines.push(`if ((${expression} & ${bits}) !== 0 && (${expression} & ${bits}) !== ${bits}) fail(${JSON.stringify(command.name + " flags")});`); }
    else if (constraint.kind === "implies") { const source = flags.get(constraint.if.name).bit; const target = mask(constraint.then); lines.push(`if ((${expression} & ${source}) !== 0 && (${expression} & ${target}) !== ${target}) fail(${JSON.stringify(command.name + " flags")});`); }
    else throw new Error(`${command.name}: unsupported flag constraint ${constraint.kind}`);
  }
  return lines.join("\n");
}
function renderCommand(command) {
  const name = id(command.name); const header = op(command, "command-header"); const payload = op(command, "payload");
  const fields = command.operations.filter((entry) => entry.op === "field"); const metadata = command.operations.find((entry) => entry.op === "metadata-flag-frame");
  const properties = fields.map((field) => `readonly ${prop(field.name)}${field.when ? "?" : ""}: ${refName(field.type)};`);
  if (metadata) properties.push(`readonly metadata?: ${refName(metadata.frame)};`);
  if (payload.policy !== "forbidden") properties.push(`readonly payload${payload.policy === "required" ? "" : "?"}: ${refName(payload.type)};`);
  const decodeFrames = []; const encodeFrames = [];
  if (metadata) { const bit = flags.get(metadata.flag.name).bit; decodeFrames.push(`if ((flags & ${bit}) !== 0) { if (frameIndex >= frames.length) fail(${JSON.stringify(command.name + " metadata")}); value.metadata = ${topRead(metadata.frame, "frames[frameIndex++]")}; }`); encodeFrames.push(`if ((value.flags & ${bit}) !== 0) { if (value.metadata === undefined) fail(${JSON.stringify(command.name + " metadata")}); frames.push(${topWrite(metadata.frame, "value.metadata")}); } else if (value.metadata !== undefined) fail(${JSON.stringify(command.name + " metadata forbidden")});`); }
  if (payload.policy === "required") { decodeFrames.push(`if (frameIndex >= frames.length) fail(${JSON.stringify(command.name + " payload")}); value.payload = ${topRead(payload.type, "frames[frameIndex++]")};`); encodeFrames.push(`if (value.payload === undefined) fail(${JSON.stringify(command.name + " payload")}); frames.push(${topWrite(payload.type, "value.payload")});`); }
  if (payload.policy === "optional") { decodeFrames.push(`if (frameIndex < frames.length) value.payload = ${topRead(payload.type, "frames[frameIndex++]")};`); encodeFrames.push(`if (value.payload !== undefined) frames.push(${topWrite(payload.type, "value.payload")});`); }
  const magic = header.magic.map((byte) => `Number(reader.u(1)) !== ${byte}`).join(" || "); const predicates = predicateChecks(command);
  return `export type ${name}Command = { readonly command: ${JSON.stringify(command.name)}; readonly flags: number; ${properties.join(" ")} };\nexport function decode${name}Command(frames: readonly Uint8Array[], context: ServiceWireDecoderContext): ${name}Command { if (!frames.length) fail(${JSON.stringify(command.name + " frames")}); const reader = new Reader(frames[0]); if (${magic} || Number(reader.u(1)) !== ${header.wireMajor} || Number(reader.u(1)) !== ${header.commandId}) fail(${JSON.stringify(command.name + " header")}); const flags = Number(reader.u(1)); ${flagChecks(command, "flags")} const value: any = { command: ${JSON.stringify(command.name)}, flags };\n${indent(fields.map((field) => readField(field)).join("\n"))}\nreader.done(${JSON.stringify(command.name)}); let frameIndex = 1;\n${indent(decodeFrames.join("\n"))}\nif (frameIndex !== frames.length) fail(${JSON.stringify(command.name + " frames")});\n${indent(predicates)}\nreturn value; }\nexport function encode${name}Command(value: ${name}Command, context: ServiceWireDecoderContext): readonly Uint8Array[] { ${flagChecks(command, "value.flags")} ${predicates} const writer = new Writer(); writer.put([${header.magic}, ${header.wireMajor}, ${header.commandId}, value.flags]);\n${indent(fields.map((field) => writeField(field, "writer", "value", "value.flags")).join("\n"))}\nconst frames: Uint8Array[] = [writer.result()];\n${indent(encodeFrames.join("\n"))}\nreturn frames; }\nexport function validate${name}CommandRuntimePredicates(value: any, context: ServiceWireDecoderContext): void { ${predicates || "void value; void context;"} }`;
}

function renderDurable(format) {
  const name = id(format.name); const header = op(format, "durable-header"); const checksum = op(format, "checksum"); const limit = op(format, "encoded-limit");
  const bodyField = format.operations.find((entry) => entry.op === "field" && entry.name === "body");
  if (checksum.algorithm !== "crc32c-castagnoli" || checksum.encoding !== "u32-big-endian" || checksum.coverage !== "magic-through-body"
      || checksum.position !== "trailing" || checksum.mismatch !== "protocol-error" || checksum.verification !== "before-body-interpretation"
      || bodyField?.interpretation !== "after-checksum-verification") throw new Error(`${format.name}: unsupported checksum`);
  const limits = emitters[limit.op].syntax(format, limit, { encode: "bytes.length", decode: "bytes.length" });
  const magic = header.magic.map((byte) => `Number(reader.u(1)) !== ${byte}`).join(" || ");
  return `export function decode${name}DurableFormat(bytes: Uint8Array, context: ServiceWireDecoderContext): ${refName(bodyField.type)} { ${limits.decode} const reader = new Reader(bytes); if (${magic} || Number(reader.u(1)) !== ${header.formatVersion}) fail(${JSON.stringify(format.name + " header")}); const flags = ${readRef(header.flagsType, "reader", "{}", "0")}; if (!same(flags, ${literal(header.flags)})) fail(${JSON.stringify(format.name + " flags")}); const length = Number(${readRef(header.bodyLengthType, "reader", "{}", "0")}); const body = reader.take(length); const checksumOffset = reader.offset; const declared = Number(reader.u(4)); reader.done(${JSON.stringify(format.name)}); if (crc32c(bytes.slice(0, checksumOffset)) !== declared) fail(${JSON.stringify(format.name + " checksum")}); return ${topRead(bodyField.type, "body")}; }\nexport function encode${name}DurableFormat(value: ${refName(bodyField.type)}, context: ServiceWireDecoderContext): Uint8Array { const body = ${topWrite(bodyField.type, "value")}; const writer = new Writer(); writer.put([${header.magic}, ${header.formatVersion}]); ${writeRef(header.flagsType, literal(header.flags), "writer", "{}", "0")}; ${writeRef(header.bodyLengthType, "body.length", "writer", "{}", "0")}; writer.put(body); writer.u(BigInt(crc32c(writer.result())), 4); const bytes = writer.result(); ${limits.encode} return bytes; }`;
}
function renderLogical(format) {
  const name = id(format.name); const logical = op(format, "logical-stream"); const limit = op(format, "encoded-limit");
  const limits = emitters[limit.op].syntax(format, limit, { encode: "bytes.length", decode: "bytes.length" });
  return `export function decode${name}LogicalStream(bytes: Uint8Array, context: ServiceWireDecoderContext): ${refName(logical.body)} { ${limits.decode} return ${topRead(logical.body, "bytes")}; }\nexport function encode${name}LogicalStream(value: ${refName(logical.body)}, context: ServiceWireDecoderContext): Uint8Array { const bytes = ${topWrite(logical.body, "value")}; ${limits.encode} return bytes; }`;
}

function renderRuntime() {
  const negotiatedContext = [...new Set(ir.types.flatMap((owner) => owner.operations
    .filter((operation) => operation.op === "negotiated-bound")
    .flatMap((operation) => Object.values(operation.applications)
      .map((application) => application.context.name))))]
    .map((name) => `readonly ${prop(name)}?: number | bigint;`).join(" ");
  return String.raw`
export type ServiceWireRuntimePredicate = (terminalResult: number, failureCode: number) => boolean;
export type ServiceWireDecoderContext = Readonly<{ originalOperationKind?: MeshOperationKind; durableRelocationPresent?: boolean; applicationSnapshotPresent?: boolean; ${negotiatedContext} runtimePredicates: Readonly<Record<string, ServiceWireRuntimePredicate>> }>;
function fail(message: string): never { throw new RangeError(message); }
class ServiceWireCapacityError extends RangeError { constructor(message: string) { super(message); this.name = "ServiceWireCapacityError"; } }
function capacityFail(message: string): never { throw new ServiceWireCapacityError(message); }
function numeric(value: unknown): bigint { return typeof value === "bigint" ? value : BigInt(value as number); }
function same(left: unknown, right: unknown): boolean { return typeof left === "bigint" || typeof right === "bigint" ? numeric(left) === numeric(right) : left === right; }
function requireContext(context: ServiceWireDecoderContext, name: string): unknown { const value = (context as any)[name]; if (value === undefined) fail("missing decoder context " + name); return value; }
function runtimePredicate(context: ServiceWireDecoderContext, name: string, terminal: number, failure: number): boolean { const predicate = context.runtimePredicates[name]; if (!predicate) fail("missing runtime predicate " + name); return predicate(terminal, failure); }
const encoder = new TextEncoder(); const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
function utf8(value: unknown): Uint8Array { const text = value as string; for (let index = 0; index < text.length; ++index) { const unit = text.charCodeAt(index); if (unit >= 0xd800 && unit <= 0xdbff) { const next = text.charCodeAt(++index); if (!(next >= 0xdc00 && next <= 0xdfff)) fail("lone UTF-16 surrogate"); } else if (unit >= 0xdc00 && unit <= 0xdfff) fail("lone UTF-16 surrogate"); } return encoder.encode(text); }
function decodeUtf8(bytes: Uint8Array, label: string): string { try { return decoder.decode(bytes); } catch { return fail(label + " UTF-8"); } }
class Reader { offset = 0; constructor(readonly bytes: Uint8Array, readonly end = bytes.length) {} get remaining(): number { return this.end - this.offset; } take(length: number): Uint8Array { if (!Number.isSafeInteger(length) || length < 0 || length > this.remaining) fail("truncated service-wire value"); const value = this.bytes.subarray(this.offset, this.offset + length); this.offset += length; return value; } bounded(length: number): Reader { return new Reader(this.take(length)); } u(width: number): bigint { let value = 0n; for (const byte of this.take(width)) value = (value << 8n) | BigInt(byte); return value; } i64(): bigint { const value = this.u(8); return (value & (1n << 63n)) === 0n ? value : value - (1n << 64n); } done(label: string): void { if (this.remaining) fail(label + " trailing bytes"); } }
class Writer { readonly chunks: Uint8Array[] = []; length = 0; put(bytes: Iterable<number>): void { const chunk = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes); if (!chunk.length) return; this.chunks.push(chunk); this.length += chunk.length; } u(value: bigint, width: number): void { const bytes = new Uint8Array(width); for (let index = width - 1; index >= 0; --index) { bytes[index] = Number(value & 255n); value >>= 8n; } this.put(bytes); } i64(value: bigint): void { this.u(BigInt.asUintN(64, value), 8); } result(): Uint8Array { if (!this.chunks.length) return new Uint8Array(); if (this.chunks.length === 1) return this.chunks[0]; const result = new Uint8Array(this.length); let offset = 0; for (const chunk of this.chunks) { result.set(chunk, offset); offset += chunk.length; } return result; } }
function compareBytes(left: Uint8Array, right: Uint8Array): number { const count = Math.min(left.length, right.length); for (let index = 0; index < count; ++index) if (left[index] !== right[index]) return left[index] < right[index] ? -1 : 1; return left.length === right.length ? 0 : left.length < right.length ? -1 : 1; }
function compareBigint(left: bigint, right: bigint): number { return left < right ? -1 : left > right ? 1 : 0; }
function compareResults(results: readonly number[]): number { for (const result of results) if (result) return result; return 0; }
function canonicalAuthorityKey(prefix: string, separator: string, wire: string, components: readonly Uint8Array[], minimum: number, maximum: number, maximumEncoded: number): Uint8Array { const parts = [prefix, wire]; for (const component of components) { if (component.length < minimum || component.length > maximum) fail("authority key component length"); let escaped = ""; for (const byte of component) escaped += (byte >= 0x41 && byte <= 0x5a) || (byte >= 0x61 && byte <= 0x7a) || (byte >= 0x30 && byte <= 0x39) || byte === 0x2d || byte === 0x2e || byte === 0x5f || byte === 0x7e ? String.fromCharCode(byte) : "%" + byte.toString(16).toUpperCase().padStart(2, "0"); parts.push(String(component.length), escaped); } const result = utf8(parts.join(separator)); if (result.length > maximumEncoded) fail("authority key maximum"); return result; }
function crc32c(bytes: Uint8Array): number { let crc = 0xffffffff; for (const byte of bytes) { crc ^= byte; for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0x82f63b78 : 0); } return (~crc) >>> 0; }
`;
}

function render(schemaPath) {
  ir = lowerSchema(schemaPath); types = new Map(ir.types.map((entry) => [entry.name, entry])); flags = new Map(ir.flags.map((entry) => [entry.name, entry])); verifyVocabulary();
  const typeCode = ir.types.map(renderType).join("\n\n"); const commandCode = ir.commands.map(renderCommand).join("\n\n");
  return `/* This file is generated by render-service-wire-typescript.mjs. */\n/* eslint-disable */\n${renderRuntime()}\n${typeCode}\n\n${commandCode}\n\nexport type ServiceWireCommand = ${ir.commands.map((entry) => `${id(entry.name)}Command`).join(" | ")};\n\n${ir.durableFormats.map(renderDurable).join("\n\n")}\n\n${renderLogical(ir.relocationLogicalStreamFormat)}\n`;
}
function main() {
  const [mode, schemaPath, outputPath, ...extra] = process.argv.slice(2);
  if (!["--write", "--check"].includes(mode) || !schemaPath || !outputPath || extra.length) { console.error("usage: render-service-wire-typescript.mjs --write|--check <schema-path> <output-path>"); process.exit(2); }
  const target = path.resolve(outputPath); const output = render(path.resolve(schemaPath));
  if (mode === "--write") { fs.mkdirSync(path.dirname(target), { recursive: true }); fs.writeFileSync(target, output); console.log(`wrote ${target} (${Buffer.byteLength(output)} bytes)`); return; }
  if (!fs.existsSync(target) || fs.readFileSync(target, "utf8") !== output) { console.error(`generated TypeScript codec drift: ${target}`); process.exit(1); }
  console.log(`generated TypeScript codec is current (${ir.types.length} types, ${ir.commands.length} commands)`);
}
main();

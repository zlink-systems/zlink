#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema } from "./service-wire-lowering.mjs";

const JAVA_KEYWORDS = new Set([
  "abstract",
  "assert",
  "boolean",
  "break",
  "byte",
  "case",
  "catch",
  "char",
  "class",
  "const",
  "continue",
  "default",
  "do",
  "double",
  "else",
  "enum",
  "extends",
  "final",
  "finally",
  "float",
  "for",
  "goto",
  "if",
  "implements",
  "import",
  "instanceof",
  "int",
  "interface",
  "long",
  "native",
  "new",
  "package",
  "private",
  "protected",
  "public",
  "return",
  "short",
  "static",
  "strictfp",
  "super",
  "switch",
  "synchronized",
  "this",
  "throw",
  "throws",
  "transient",
  "try",
  "void",
  "volatile",
  "while",
  "record",
  "sealed",
  "permits",
  "yield",
]);

function typeName(value) {
  const result = value
    .split(/[^A-Za-z0-9]+/)
    .filter(Boolean)
    .map((part) => part[0].toUpperCase() + part.slice(1))
    .join("");
  return /^[0-9]/.test(result) ? `N${result}` : result;
}

function fieldName(value) {
  const result = value.replace(/[^A-Za-z0-9_$]/g, "_");
  return JAVA_KEYWORDS.has(result) ? `${result}Value` : result;
}

function enumName(value) {
  const result = value
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/[^A-Za-z0-9]+/g, "_")
    .toUpperCase();
  return /^[0-9]/.test(result) ? `N_${result}` : result;
}

function referenceOf(value) {
  return value.$ref === undefined ? value.type : value;
}
function refType(reference) {
  return typeName(referenceOf(reference).$ref);
}
function integerPrimitive(encoding) {
  return encoding === "u64" || encoding === "i64" ? "long" : "int";
}
function integerWidth(encoding) {
  return { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[encoding];
}
function javaString(value) {
  return JSON.stringify(String(value));
}
function longLiteral(value) {
  const text = String(value);
  return BigInt(text) > 9223372036854775807n
    ? `Long.parseUnsignedLong(${javaString(text)})`
    : `${text}L`;
}

function integerArgument(reference, expression, typeByName) {
  const type = typeByName.get(reference.$ref);
  return integerPrimitive(operation(type, "integer").encoding) === "int"
    ? `(int)(${expression})`
    : expression;
}

function operation(owner, name) {
  const result = owner.operations.find((entry) => entry.op === name);
  if (!result) throw new Error(`${owner.name}: missing ${name} operation`);
  return result;
}

function primaryOperation(owner) {
  return owner.operations.find((entry) =>
    [
      "integer",
      "enum",
      "length-prefixed",
      "struct",
      "vector",
      "versioned-vector",
      "versioned-length-delimited",
      "conditional-union",
      "tlv32",
    ].includes(entry.op),
  );
}

function caseInfo(type, union = operation(type, "conditional-union")) {
  const used = new Map();
  const cases = Object.entries(union.cases).map(
    ([signature, selected], index) => {
      const discriminator = JSON.parse(signature);
      let suffix =
        Object.values(discriminator).map(typeName).join("") || `Case${index}`;
      const count = used.get(suffix) ?? 0;
      used.set(suffix, count + 1);
      if (count > 0) suffix += count + 1;
      return {
        signature,
        discriminator,
        selected,
        name: `${typeName(type.name)}${suffix}`,
      };
    },
  );
  if (union.otherwise.kind === "fields") {
    cases.push({
      signature: null,
      discriminator: null,
      selected: union.otherwise,
      name: `${typeName(type.name)}Otherwise`,
    });
  }
  return cases;
}

function caseOperations(selected) {
  return selected.operations ?? selected.fields;
}
function caseFields(selected) {
  return caseOperations(selected).filter((entry) => entry.op === "field");
}

function recordComponents(fields) {
  return fields
    .map((field) => `${refType(field)} ${fieldName(field.name)}`)
    .join(", ");
}

function declaration(type) {
  const name = typeName(type.name);
  const op = primaryOperation(type);
  switch (op.op) {
    case "integer":
      return `  record ${name}(${integerPrimitive(op.encoding)} value) {}`;
    case "enum":
      return `  enum ${name} { ${op.values.map((v) => `${enumName(v.name)}(${longLiteral(v.value)})`).join(", ")}; final long wire; ${name}(long wire){this.wire=wire;} }`;
    case "length-prefixed":
      return op.content === "bytes"
        ? `  record ${name}(byte[] value) {}`
        : `  record ${name}(String value) {}`;
    case "struct":
      return `  record ${name}(${recordComponents(op.fields)}) {}`;
    case "vector":
      return `  record ${name}(List<${refType(op.item)}> items) { ${name}{items=List.copyOf(items);} }`;
    case "versioned-vector": {
      const repeat = op.layout.find((entry) => entry.kind === "repeat");
      return `  record ${name}(List<${refType(repeat.item)}> ${fieldName(repeat.name)}) { ${name}{${fieldName(repeat.name)}=List.copyOf(${fieldName(repeat.name)});} }`;
    }
    case "versioned-length-delimited":
      return `  record ${name}(${recordComponents(op.fields)}) {}`;
    case "conditional-union": {
      const variants = caseInfo(type, op);
      const records = variants
        .map((entry) => {
          const wire =
            entry.discriminator === null
              ? []
              : op.discriminators.filter((d) => d.source.kind === "wire");
          const fields = [...wire, ...caseFields(entry.selected)];
          return `  record ${entry.name}(${recordComponents(fields)}) implements ${name} {}`;
        })
        .join("\n");
      return `  sealed interface ${name} permits ${variants.map((v) => v.name).join(", ")} {}\n${records}`;
    }
    case "tlv32":
      return `  record ${name}(${recordComponents(op.fields)}) {}`;
    default:
      throw new Error(`${type.name}: unsupported primary operation ${op?.op}`);
  }
}

function conditionExpression(condition, values, flags, flagBits) {
  if (!condition) return "true";
  return condition.all
    .map((atom) => {
      if (atom.kind === "fieldPresent") {
        if (
          atom.presence?.internal?.absent !== null ||
          atom.presence?.internal?.present !== "non-null"
        ) {
          throw new Error(`unsupported field presence ${atom.operand.name}`);
        }
        const value = values.get(atom.operand.name);
        return `${value} != null && ${value}.value() != null`;
      }
      if (atom.kind === "fieldEquals") {
        const variable = values.get(atom.operand.name);
        const expected = atom.value;
        return `${variable} != null && ${variable}.toString().equals(${javaString(String(expected).toUpperCase())})`;
      }
      if (atom.kind === "contextEquals") {
        const expected =
          atom.value === true
            ? "Boolean.TRUE"
            : atom.value === false
              ? "Boolean.FALSE"
              : javaString(atom.value);
        return `Objects.equals(c.${fieldName(atom.operand.name)}(),${expected})`;
      }
      const mask = atom.operands.reduce(
        (result, operand) => result | flagBits.get(operand.name),
        0,
      );
      return atom.kind === "allFlagsSet"
        ? `((${flags} & ${mask}) == ${mask})`
        : `((${flags} & ${mask}) != 0)`;
    })
    .join(" && ");
}

function decoderCall(reference, reader, context, flags, values, typeByName) {
  reference = referenceOf(reference);
  const target = typeByName.get(reference.$ref);
  const argumentsList = [reader, context, flags];
  const union = target.operations.find(
    (entry) => entry.op === "conditional-union",
  );
  if (union) {
    for (const discriminator of union.discriminators.filter(
      (d) => d.source.kind !== "wire",
    )) {
      argumentsList.push(
        discriminator.source.kind === "enclosingField"
          ? values.get(discriminator.source.name)
          : `${context}.${fieldName(discriminator.source.name)}()`,
      );
    }
  }
  return `decode${typeName(reference.$ref)}(${argumentsList.join(", ")})`;
}

function encoderCall(
  reference,
  value,
  writer,
  context,
  flags,
  values,
  typeByName,
) {
  reference = referenceOf(reference);
  const target = typeByName.get(reference.$ref);
  const argumentsList = [value, writer, context, flags];
  const union = target.operations.find(
    (entry) => entry.op === "conditional-union",
  );
  if (union) {
    for (const discriminator of union.discriminators.filter(
      (d) => d.source.kind !== "wire",
    )) {
      argumentsList.push(
        discriminator.source.kind === "enclosingField"
          ? values.get(discriminator.source.name)
          : `${context}.${fieldName(discriminator.source.name)}()`,
      );
    }
  }
  return `encode${typeName(reference.$ref)}(${argumentsList.join(", ")})`;
}

function decodeFields(
  fields,
  reader,
  context,
  flags,
  typeByName,
  flagBits,
  indent = "    ",
) {
  const values = new Map();
  const lines = [];
  for (const field of fields) {
    const variable = fieldName(field.name);
    const javaType = refType(field);
    const expression = decoderCall(
      field,
      reader,
      context,
      flags,
      values,
      typeByName,
    );
    if (field.when) {
      lines.push(`${indent}${javaType} ${variable} = null;`);
      lines.push(
        `${indent}if (${conditionExpression(field.when, values, flags, flagBits)}) ${variable} = ${expression};`,
      );
    } else {
      lines.push(`${indent}${javaType} ${variable} = ${expression};`);
    }
    const target = typeByName.get(field.type.$ref ?? field.$ref);
    const targetOp = primaryOperation(target);
    if (field.constant !== undefined) {
      const expected =
        targetOp.op === "enum"
          ? `${refType(field)}.${enumName(field.constant)}`
          : longLiteral(field.constant);
      const actual = targetOp.op === "enum" ? variable : `${variable}.value()`;
      lines.push(
        `${indent}require(${actual} == ${expected}, ${javaString(field.name + " constant")});`,
      );
    }
    if (field.minimum !== undefined && targetOp.op === "integer")
      lines.push(
        `${indent}requireUnsigned(${variable}.value(), ${targetOp.width}, ${javaString(field.minimum)}, null, ${javaString(field.name)});`,
      );
    if (field.maximum !== undefined && targetOp.op === "integer")
      lines.push(
        `${indent}requireUnsigned(${variable}.value(), ${targetOp.width}, null, ${javaString(field.maximum)}, ${javaString(field.name)});`,
      );
    values.set(field.name, variable);
  }
  return { lines, values };
}

function encodeFields(
  fields,
  owner,
  writer,
  context,
  flags,
  typeByName,
  flagBits,
  indent = "    ",
) {
  const values = new Map(
    fields.map((field) => [field.name, `${owner}.${fieldName(field.name)}()`]),
  );
  const lines = [];
  for (const field of fields) {
    const value = values.get(field.name);
    if (field.when) {
      const present = conditionExpression(field.when, values, flags, flagBits);
      lines.push(
        `${indent}if (${present}) { require(${value} != null, ${javaString(field.name + " required")}); ${encoderCall(field, value, writer, context, flags, values, typeByName)}; } else require(${value} == null, ${javaString(field.name + " forbidden")});`,
      );
    } else {
      lines.push(
        `${indent}${encoderCall(field, value, writer, context, flags, values, typeByName)};`,
      );
    }
  }
  return lines;
}

function structConstraints(type, constraints, owner = null) {
  const access = (name) =>
    owner ? `${owner}.${fieldName(name)}()` : fieldName(name);
  return constraints
    .map((constraint) => {
      if (constraint.kind === "not-both-zero") {
        const values = constraint.fields.map((field) => {
          const value = access(field.name);
          return `${value}.value()!=0`;
        });
        return `require(${values.join("||")},${javaString(type.name + " constraint")});`;
      }
      if (constraint.kind === "field-less-than-or-equal") {
        return `require(Long.compareUnsigned(${access(constraint.left.name)}.value(),${access(constraint.right.name)}.value())<=0,${javaString(type.name + " constraint")});`;
      }
      if (constraint.kind === "terminal-success-shape") {
        return `if(${access("terminalResult")}==RequestTerminalResult.${enumName(constraint.when.terminalResult)})require(${access("failureCode")}==FrameworkErrorCode.${enumName(constraint.requires.failureCode)}&&${access("hasCreation")}==Bool8.${enumName(constraint.requires.hasCreation)},${javaString(type.name + " success shape")});`;
      }
      if (constraint.kind === "terminal-failure-shape") {
        return `if(${access("terminalResult")}!=RequestTerminalResult.${enumName(constraint.when.terminalResultNot)})require(${access("hasCreation")}==Bool8.${enumName(constraint.requires.hasCreation)}&&${access("hasApplicationPayload")}==Bool8.${enumName(constraint.requires.hasApplicationPayload)},${javaString(type.name + " failure shape")});`;
      }
      if (constraint.kind === "existing-has-no-application-payload") {
        const creationField = primaryOperation(type).fields.find(
          (field) => field.name === "creation",
        );
        const creationType = globalTypeByName.get(
          referenceOf(creationField).$ref,
        );
        const existing = caseInfo(creationType).find(
          (entry) =>
            entry.discriminator?.createResult ===
            constraint.when["creation.createResult"],
        );
        if (!existing)
          throw new Error(`${type.name}: existing creation case missing`);
        return `if(${access("creation")} instanceof ${existing.name})require(${access("hasApplicationPayload")}==Bool8.${enumName(constraint.requires.hasApplicationPayload)},${javaString(type.name + " existing shape")});`;
      }
      throw new Error(
        `${type.name}: unsupported struct constraint ${constraint.kind}`,
      );
    })
    .join(" ");
}

function integerMethods(type, op) {
  const name = typeName(type.name),
    primitive = integerPrimitive(op.encoding),
    width = op.width;
  const signed = op.encoding === "i64";
  const widthArgument = signed ? "" : `, ${width}`;
  return (
    `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${primitive} value=${width === 8 ? "r.i64()" : `(int)r.uint(${width})`}; ${signed ? "requireSigned" : "requireUnsigned"}(value${widthArgument}, ${op.minimum === undefined ? "null" : javaString(op.minimum)}, ${op.maximum === undefined ? "null" : javaString(op.maximum)}, ${javaString(type.name)}); return new ${name}(value); }\n` +
    `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${signed ? "requireSigned" : "requireUnsigned"}(value.value()${widthArgument}, ${op.minimum === undefined ? "null" : javaString(op.minimum)}, ${op.maximum === undefined ? "null" : javaString(op.maximum)}, ${javaString(type.name)}); w.uint(${width}, value.value()); }`
  );
}

function enumMethods(type, op) {
  const name = typeName(type.name),
    width = op.width;
  const decodeCases = op.values
    .map(
      (v) =>
        `if(wire==${longLiteral(v.value)})return ${name}.${enumName(v.name)};`,
    )
    .join("");
  return (
    `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { long wire=r.uint(${width});${decodeCases}throw error(${javaString(type.name + " enum")}); }\n` +
    `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags){w.uint(${width},value.wire);}`
  );
}

function vectorConstraintCode(type, op, itemReference, listExpression) {
  const constraints = op.constraints ?? [];
  if (constraints.length === 0) return "";
  const itemType = refType(itemReference);
  const source = (source, variable) =>
    source.kind === "item"
      ? variable
      : source.path
          .split(".")
          .reduce((value, part) => `${value}.${fieldName(part)}()`, variable);
  const sourceType = (source) => {
    let target = globalTypeByName.get(referenceOf(itemReference).$ref);
    if (source.kind === "item") return target;
    for (const part of source.path.split(".")) {
      const field = (primaryOperation(target).fields ?? []).find(
        (candidate) => candidate.name === part,
      );
      if (!field)
        throw new Error(
          `${type.name}: unknown constraint field ${source.path}`,
        );
      target = globalTypeByName.get(referenceOf(field).$ref);
    }
    return target;
  };
  const tuplePart = (part, variable, writer) => {
    const value = source(part.source, variable);
    if (part.kind === "utf-8-bytes") {
      if (part.lengthPrefix !== "excluded")
        throw new Error(`${type.name}: unsupported UTF-8 key`);
      return `${writer}.bytes(strictBytes(${value}.value()));`;
    }
    if (["wire-value", "unsigned-wire-value"].includes(part.kind)) {
      if (part.byteOrder !== "big-endian")
        throw new Error(`${type.name}: unsupported wire key order`);
      const target = sourceType(part.source),
        targetOp = primaryOperation(target);
      const actual =
        targetOp.op === "enum" ? `${value}.wire` : `${value}.value()`;
      return `${writer}.uint(${integerWidth(part.encoding)},${actual});`;
    }
    throw new Error(`${type.name}: unsupported tuple key ${part.kind}`);
  };
  const canonicalKey = (key, variable, writer) => {
    const format = key.format;
    if (
      format.encoding !== "canonical-ascii-utf8" ||
      format.componentLayout !==
        "decimal-raw-byte-length-colon-percent-encoded-bytes" ||
      format.decimalLength !== "base10-no-leading-zero" ||
      format.escaping !==
        "rfc3986-unreserved-literal-otherwise-uppercase-percent-hex" ||
      format.unicodeNormalization !== "none"
    )
      throw new Error(`${type.name}: unsupported canonical authority key`);
    const value = source(key.source, variable);
    const target = sourceType(key.source);
    const variants = Object.entries(key.variants).map(
      ([variantName, variant]) => {
        const selected = caseInfo(target).find((entry) =>
          Object.values(entry.discriminator ?? {}).includes(variantName),
        );
        const discriminator = format.kindDiscriminators.find(
          (entry) => entry.objectKind === variant.objectKind,
        );
        if (!selected || !discriminator)
          throw new Error(
            `${type.name}: incomplete canonical authority key variant`,
          );
        const components = variant.components.map(
          (path) =>
            path
              .split(".")
              .reduce(
                (result, part) => `${result}.${fieldName(part)}()`,
                "variant",
              ) + ".value()",
        );
        return `if(${value} instanceof ${selected.name} variant)canonicalAuthorityKey(${writer},${javaString(format.prefix)},${javaString(discriminator.wire)},${javaString(format.separator)},${components.join(",")});`;
      },
    );
    return `${variants.join("else ")}else throw error(${javaString(type.name + " authority key variant")});`;
  };
  const key = (constraint, variable, writer) => {
    if (constraint.key.kind === "tuple")
      return constraint.key.parts
        .map((part) => tuplePart(part, variable, writer))
        .join("");
    if (constraint.key.kind === "canonical-authority-key-bytes") {
      return canonicalKey(constraint.key, variable, writer);
    }
    throw new Error(
      `${type.name}: unsupported constraint key ${constraint.key.kind}`,
    );
  };
  const consumed = new Set();
  return constraints
    .map((constraint, index) => {
      if (consumed.has(index)) return "";
      if (!["sorted", "unique"].includes(constraint.kind)) {
        throw new Error(
          `${type.name}: unsupported vector constraint ${constraint.kind}`,
        );
      }
      if (constraint.kind === "sorted") {
        const uniqueIndex = constraints.findIndex(
          (candidate, candidateIndex) =>
            candidateIndex !== index &&
            candidate.kind === "unique" &&
            JSON.stringify(candidate.key) === JSON.stringify(constraint.key),
        );
        if (uniqueIndex >= 0) consumed.add(uniqueIndex);
        return `try{for(int i=1;i<${listExpression}.size();i++){${itemType} previous=${listExpression}.get(i-1),current=${listExpression}.get(i);Writer left${index}=new Writer(),right${index}=new Writer();${key(constraint, "previous", `left${index}`)}${key(constraint, "current", `right${index}`)}int compared${index}=compareUnsigned(left${index}.result(),right${index}.result());${uniqueIndex >= 0 ? `require(compared${index}!=0,${javaString(type.name + " unique")});` : ""}require(compared${index}<=0,${javaString(type.name + " sorted")});}}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " constraint capacity")});}`;
      }
      return `try{Set<ByteKey> keys${index}=new HashSet<>();for(${itemType} item:${listExpression}){Writer key${index}=new Writer();${key(constraint, "item", `key${index}`)}require(keys${index}.add(new ByteKey(key${index}.result())),${javaString(type.name + " unique")});}}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " constraint capacity")});}`;
    })
    .join("");
}

function negotiatedBound(type, measured, application, value) {
  const bounds = type.operations.filter(
    (entry) => entry.op === "negotiated-bound" && entry.measured === measured,
  );
  return bounds
    .map((bound) => {
      const applied = bound.applications[application]?.context;
      if (
        !new Set(["encoder-context", "decoder-context"]).has(applied?.kind) ||
        applied.name !== bound.context.name ||
        bound.context.missing !== "protocol-error" ||
        bound.context.negative !== "protocol-error" ||
        bound.context.aboveAbsoluteMaximum !== "protocol-error" ||
        bound.comparison !== "less-than-or-equal"
      ) {
        throw new Error(`${type.name}: unsupported negotiated-bound syntax`);
      }
      const context = `c.${fieldName(bound.context.name)}()`;
      const message = javaString(
        type.name + " " + bound.topology + " negotiated bound",
      );
      return `require(${context}!=null&&${context}>=0&&${context}<=${longLiteral(bound.context.absoluteMaximum)},${message});require((long)${value}<=${context},${message});`;
    })
    .join("");
}

function encodedLimit(type, expression, application) {
  const limits = type.operations.filter(
    (entry) => entry.op === "encoded-limit",
  );
  return limits
    .map((limit) => {
      if (
        limit.measured !== "complete-encoded-value" ||
        limit.exceeded !== "protocol-error" ||
        !limit.applications.includes(application)
      ) {
        throw new Error(`${type.name}: unsupported encoded-limit syntax`);
      }
      return `require((long)${expression}<=${longLiteral(limit.maximumEncodedBytes)},${javaString(type.name + " encoded limit")});`;
    })
    .join("");
}

function encodeCapacity(type, policy, measurements) {
  if (
    !Number.isSafeInteger(policy?.declaredMaximumBytes) ||
    !Number.isSafeInteger(policy.representationCeiling) ||
    !Number.isSafeInteger(policy.requiredThroughBytes) ||
    !Array.isArray(policy.applications)
  ) {
    throw new Error(`${type.name}: invalid encode capacity`);
  }
  if (policy.declaredMaximumBytes !== type.maximumBytes) {
    throw new Error(`${type.name}: encode capacity does not match maximum`);
  }
  const rejection = (kind, message) => {
    if (kind === "protocol-error")
      return `throw error(${javaString(message)});`;
    if (kind === "capacity-error")
      return `throw capacityError(${javaString(message)});`;
    throw new Error(
      `${type.name}: unsupported encode capacity rejection ${kind}`,
    );
  };
  return Object.fromEntries(
    policy.applications.map((application) => {
      const measured = measurements[application];
      if (!measured)
        throw new Error(
          `${type.name}: missing encode capacity ${application} measurement`,
        );
      return [
        application,
        `if(Long.compareUnsigned(${measured},${longLiteral(policy.requiredThroughBytes)})>0){if(Long.compareUnsigned(${measured},${longLiteral(policy.representationCeiling)})>0)${rejection(policy.aboveRepresentationCeiling, type.name + " capacity")}if(Long.compareUnsigned(${measured},${longLiteral(policy.declaredMaximumBytes)})>0)${rejection(policy.aboveDeclaredMaximum, type.name + " length")}}`,
      ];
    }),
  );
}

let globalTypeByName;
let globalFlagBits;

function codecMethods(type, typeByName, flagBits) {
  const name = typeName(type.name);
  const op = primaryOperation(type);
  if (op.op === "integer") return integerMethods(type, op);
  if (op.op === "enum") return enumMethods(type, op);
  if (op.op === "length-prefixed") {
    const isText = op.content === "text";
    const lengthType = refType(op.lengthType);
    const absent = op.zeroLengthMeaning === "absent";
    const readValue = isText
      ? `strictText(r.bytes(length), ${javaString(type.name)})`
      : "r.bytes(length)";
    const bytes = isText ? "strictBytes(value.value())" : "value.value()";
    const capacity = encodeCapacity(type, op.encodeCapacity, {
      encode: "(long)bytes.length",
      decode: "encodedLength",
    });
    const textValidation = type.operations.find(
      (entry) => entry.op === "text-validation",
    );
    if (isText && !textValidation)
      throw new Error(`${type.name}: missing text-validation operation`);
    if (
      isText &&
      (textValidation.encoding !== "utf-8" ||
        textValidation.malformed !== "protocol-error" ||
        textValidation.nul !== "forbidden" ||
        textValidation.decode.bom !== "preserve" ||
        textValidation.decode.overlong !== "protocol-error" ||
        textValidation.decode.surrogateCodePoint !== "protocol-error" ||
        textValidation.encode.loneSurrogate !== "protocol-error")
    )
      throw new Error(`${type.name}: unsupported text validation`);
    const decodeBound = negotiatedBound(
      type,
      "content-bytes",
      "decode",
      "length",
    );
    const encodeBound = negotiatedBound(
      type,
      "content-bytes",
      "encode",
      "bytes.length",
    );
    return (
      `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { long encodedLength=unsignedLength(decode${lengthType}(r,c,flags)); ${capacity.decode} int length=(int)encodedLength; require(length>=${op.minimumBytes ?? 0},${javaString(type.name + " length")}); ${decodeBound} ${absent ? `if(length==0)return new ${name}(null);` : ""} return new ${name}(${readValue}); }\n` +
      `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${isText ? "byte[] bytes=" + bytes + ";" : `byte[] bytes=${bytes};`} ${absent ? "if(bytes==null)bytes=new byte[0];" : `require(bytes!=null,${javaString(type.name)});`} ${capacity.encode} require(bytes.length>=${op.minimumBytes ?? 0},${javaString(type.name + " length")}); ${encodeBound} encode${lengthType}(new ${lengthType}(bytes.length),w,c,flags); w.bytes(bytes); }`
    );
  }
  if (op.op === "struct" || op.op === "versioned-length-delimited") {
    const fields = op.fields;
    const delimited = op.op === "versioned-length-delimited";
    const decoded = decodeFields(
      fields,
      delimited ? "body" : "r",
      "c",
      "flags",
      typeByName,
      flagBits,
    );
    const args = fields.map((f) => fieldName(f.name)).join(", ");
    const decodeEnvelopeBound = negotiatedBound(
      type,
      "encoded-bytes",
      "decode",
      "(r.at-start)",
    );
    const encodeEnvelopeBound = negotiatedBound(
      type,
      "encoded-bytes",
      "encode",
      "(w.size()-start)",
    );
    const decodeLimit = encodedLimit(type, "(r.at-start)", "decode");
    const encodeLimit = encodedLimit(type, "(w.size()-start)", "encode");
    const before = delimited
      ? `${decodeEnvelopeBound || decodeLimit ? "int start=r.at; " : ""}require(decode${refType(op.version)}(r,c,flags).value()==${longLiteral(op.version.constant)},${javaString(type.name + " version")}); Reader body=r.slice(length(decode${refType(op.length)}(r,c,flags)));`
      : decodeLimit
        ? "int start=r.at;"
        : "";
    const after = `${delimited ? `body.end(${javaString(type.name)});` : ""}${decodeEnvelopeBound}${decodeLimit}`;
    const encoded = encodeFields(
      fields,
      "value",
      delimited ? "body" : "w",
      "c",
      "flags",
      typeByName,
      flagBits,
    );
    const encodeBefore = delimited
      ? `${encodeEnvelopeBound || encodeLimit ? "int start=w.size(); " : ""}encode${refType(op.version)}(new ${refType(op.version)}(${integerArgument(op.version, longLiteral(op.version.constant), typeByName)}),w,c,flags); Writer body=new Writer();`
      : encodeLimit
        ? "int start=w.size();"
        : "";
    const encodeAfter = delimited
      ? `byte[] bytes=body.result(); encode${refType(op.length)}(new ${refType(op.length)}(bytes.length),w,c,flags); w.bytes(bytes);${encodeEnvelopeBound}${encodeLimit}`
      : encodeLimit;
    const decodedConstraints = structConstraints(type, op.constraints ?? []);
    const encodedConstraints = structConstraints(
      type,
      op.constraints ?? [],
      "value",
    );
    const decodedPredicate = type.operations.some(
      (entry) => entry.op === "runtime-predicate",
    )
      ? predicateChecks(type, decoded.values)
      : "";
    const encodedValues = new Map(
      fields.map((field) => [field.name, `value.${fieldName(field.name)}()`]),
    );
    const encodedPredicate = type.operations.some(
      (entry) => entry.op === "runtime-predicate",
    )
      ? predicateChecks(type, encodedValues)
      : "";
    return (
      `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${before}\n${decoded.lines.join("\n")}\n    ${after} ${decodedConstraints}${decodedPredicate} return new ${name}(${args}); }\n` +
      `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException {${encodedConstraints}${encodedPredicate}${encodeBefore}\n${encoded.join("\n")}\n    ${encodeAfter} }`
    );
  }
  if (op.op === "vector" || op.op === "versioned-vector") {
    const repeat =
      op.op === "vector"
        ? { name: "items", item: op.item }
        : op.layout.find((entry) => entry.kind === "repeat");
    const list = fieldName(repeat.name),
      itemType = refType(repeat.item);
    const countRef =
      op.op === "vector"
        ? op.countType
        : op.layout.find((entry) => entry.counts);
    const prefix =
      op.op === "versioned-vector"
        ? `require(decode${refType(op.layout[0])}(r,c,flags).value()==${longLiteral(op.layout[0].constant)},${javaString(type.name + " version")});`
        : "";
    const encodePrefix =
      op.op === "versioned-vector"
        ? `encode${refType(op.layout[0])}(new ${refType(op.layout[0])}(${integerArgument(op.layout[0], longLiteral(op.layout[0].constant), typeByName)}),w,c,flags);`
        : "";
    const validation = vectorConstraintCode(type, op, repeat.item, "values");
    const encodeValidation = vectorConstraintCode(
      type,
      op,
      repeat.item,
      `value.${list}()`,
    );
    const decodeLimit = encodedLimit(type, "(r.at-start)", "decode");
    const encodeLimit = encodedLimit(type, "(w.size()-start)", "encode");
    return (
      `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${decodeLimit ? "int start=r.at;" : ""} ${prefix} int count=length(decode${refType(countRef)}(r,c,flags)); ${op.maximumItems === undefined ? "" : `require(count<=${op.maximumItems},${javaString(type.name + " count")});`} List<${itemType}> values=new ArrayList<>(count); for(int i=0;i<count;i++)values.add(${decoderCall(repeat.item, "r", "c", "flags", new Map(), typeByName)}); ${validation}${decodeLimit} return new ${name}(values); }\n` +
      `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${encodeLimit ? "int start=w.size();" : ""} ${encodePrefix} ${op.maximumItems === undefined ? "" : `require(value.${list}().size()<=${op.maximumItems},${javaString(type.name + " count")});`} ${encodeValidation} encode${refType(countRef)}(new ${refType(countRef)}(${integerArgument(countRef, `value.${list}().size()`, typeByName)}),w,c,flags); for(${itemType} item:value.${list}())${encoderCall(repeat.item, "item", "w", "c", "flags", new Map(), typeByName)}; ${encodeLimit} }`
    );
  }
  if (op.op === "conditional-union")
    return unionMethods(type, op, typeByName, flagBits);
  if (op.op === "tlv32") return tlvMethods(type, op, typeByName, flagBits);
  throw new Error(`${type.name}: unsupported operation ${op.op}`);
}

function unionParameters(type) {
  const union = type.operations.find(
    (entry) => entry.op === "conditional-union",
  );
  if (!union) return [];
  return union.discriminators
    .filter((d) => d.source.kind !== "wire")
    .map((d, index) => `${refType(d)} external${index}`);
}

function unionMethods(type, union, typeByName, flagBits) {
  const name = typeName(type.name),
    external = unionParameters(type);
  const decodeParams = [
    "Reader r",
    "DecoderContext c",
    "int flags",
    ...external,
  ].join(", ");
  const encodeParams = [
    `${name} value`,
    "Writer w",
    "DecoderContext c",
    "int flags",
    ...external,
  ].join(", ");
  const discriminatorValues = new Map();
  const decodeLines = [];
  const limitAtDecode = encodedLimit(type, "(r.at-start)", "decode");
  if (limitAtDecode) decodeLines.push("    int start=r.at;");
  let externalIndex = 0;
  for (const d of union.discriminators) {
    const variable = fieldName(d.name);
    if (d.source.kind === "wire")
      decodeLines.push(
        `    ${refType(d)} ${variable}=${decoderCall(d, "r", "c", "flags", discriminatorValues, typeByName)};`,
      );
    else
      decodeLines.push(
        `    ${refType(d)} ${variable}=external${externalIndex++};`,
      );
    discriminatorValues.set(d.name, variable);
  }
  const bodyPrefix = union.bodyLengthType
    ? `Reader selected=r.slice(length(decode${refType(union.bodyLengthType)}(r,c,flags)));`
    : "Reader selected=r;";
  decodeLines.push(`    ${bodyPrefix}`);
  const cases = caseInfo(type, union);
  cases.forEach((entry, index) => {
    const test =
      entry.discriminator === null
        ? "true"
        : Object.entries(entry.discriminator)
            .map(([key, expected]) => {
              const variable = discriminatorValues.get(key);
              const d = union.discriminators.find((item) => item.name === key);
              const target = typeByName.get(referenceOf(d).$ref);
              return primaryOperation(target).op === "enum"
                ? `${variable}==${refType(d)}.${enumName(expected)}`
                : `${variable}.value()==${longLiteral(expected)}`;
            })
            .join(" && ");
    const fields = caseFields(entry.selected);
    const decoded = decodeFields(
      fields,
      "selected",
      "c",
      "flags",
      typeByName,
      flagBits,
      "      ",
    );
    const wireArgs = union.discriminators
      .filter((d) => d.source.kind === "wire")
      .map((d) => discriminatorValues.get(d.name));
    const args = [...wireArgs, ...fields.map((f) => fieldName(f.name))].join(
      ", ",
    );
    const predicate =
      decoded.values.has("terminalResult") && decoded.values.has("failureCode")
        ? predicateChecks(type, decoded.values, caseOperations(entry.selected))
        : "";
    const constraints = caseOperations(entry.selected)
      .filter((item) => item.op === "constraint")
      .map((item) => structConstraints(type, [item]))
      .join("");
    for (const item of caseOperations(entry.selected)) {
      if (!["field", "constraint", "runtime-predicate"].includes(item.op))
        throw new Error(`${type.name}: unsupported case operation ${item.op}`);
    }
    decodeLines.push(
      `    ${index === 0 ? "if" : "else if"}(${test}) {\n${decoded.lines.join("\n")}\n      ${union.bodyLengthType ? `selected.end(${javaString(type.name)});` : ""}${constraints}${predicate}${limitAtDecode} return new ${entry.name}(${args});\n    }`,
    );
  });
  decodeLines.push(
    `    throw error(${javaString(type.name + " discriminator")});`,
  );

  const encodeLines = [];
  const limitAtEncode = encodedLimit(type, "(w.size()-start)", "encode");
  if (limitAtEncode) encodeLines.push("    int start=w.size();");
  for (const entry of cases) {
    encodeLines.push(`    if(value instanceof ${entry.name} item){`);
    if (entry.discriminator !== null) {
      if (
        union.encode?.selection !== "variant" ||
        union.encode.discriminatorAgreement !== "required" ||
        union.encode.mismatch !== "protocol-error"
      ) {
        throw new Error(`${type.name}: unsupported union encode agreement`);
      }
      let contextIndex = 0;
      for (const d of union.discriminators) {
        const expected = entry.discriminator[d.name];
        const target = typeByName.get(referenceOf(d).$ref),
          targetOp = primaryOperation(target);
        const actual =
          d.source.kind === "wire"
            ? `item.${fieldName(d.name)}()`
            : `external${contextIndex++}`;
        const agreement =
          targetOp.op === "enum"
            ? `${actual}==${refType(d)}.${enumName(expected)}`
            : `${actual}.value()==${longLiteral(expected)}`;
        encodeLines.push(
          `      require(${agreement},${javaString(type.name + " discriminator agreement")});`,
        );
      }
    }
    const wire = union.discriminators.filter((d) => d.source.kind === "wire");
    for (const d of wire)
      encodeLines.push(
        `      ${encoderCall(d, `item.${fieldName(d.name)}()`, "w", "c", "flags", new Map(), typeByName)};`,
      );
    if (union.bodyLengthType)
      encodeLines.push("      Writer selected=new Writer();");
    const fields = caseFields(entry.selected);
    encodeLines.push(
      ...encodeFields(
        fields,
        "item",
        union.bodyLengthType ? "selected" : "w",
        "c",
        "flags",
        typeByName,
        flagBits,
        "      ",
      ),
    );
    const encodedValues = new Map(
      fields.map((field) => [field.name, `item.${fieldName(field.name)}()`]),
    );
    const constraints = caseOperations(entry.selected)
      .filter((operation) => operation.op === "constraint")
      .map((operation) => structConstraints(type, [operation], "item"))
      .join("");
    if (constraints) encodeLines.push(`      ${constraints}`);
    if (encodedValues.has("terminalResult") && encodedValues.has("failureCode"))
      encodeLines.push(
        `      ${predicateChecks(type, encodedValues, caseOperations(entry.selected))}`,
      );
    if (union.bodyLengthType)
      encodeLines.push(
        `      byte[] bytes=selected.result(); encode${refType(union.bodyLengthType)}(new ${refType(union.bodyLengthType)}(bytes.length),w,c,flags); w.bytes(bytes);`,
      );
    if (limitAtEncode) encodeLines.push(`      ${limitAtEncode}`);
    encodeLines.push("      return;\n    }");
  }
  encodeLines.push(`    throw error(${javaString(type.name + " case")});`);
  return `  private static ${name} decode${name}(${decodeParams}) throws IOException {\n${decodeLines.join("\n")}\n  }\n  private static void encode${name}(${encodeParams}) throws IOException {\n${encodeLines.join("\n")}\n  }`;
}

function tlvMethods(type, op, typeByName, flagBits) {
  const name = typeName(type.name);
  const vars = op.fields
    .map((f) => `    ${refType(f)} ${fieldName(f.name)}=null;`)
    .join("\n");
  const cases = op.fields
    .map(
      (f) =>
        `case ${f.id} -> ${fieldName(f.name)}=${decoderCall(f, "item", "c", "flags", new Map(), typeByName)};`,
    )
    .join(" ");
  const required = op.requiredFields
    .map(
      (field) =>
        `require(${fieldName(field)}!=null,${javaString(field + " required")});`,
    )
    .join(" ");
  const encodeRequired = op.requiredFields
    .map(
      (field) =>
        `require(value.${fieldName(field)}()!=null,${javaString(field + " required")});`,
    )
    .join(" ");
  const args = op.fields.map((f) => fieldName(f.name)).join(", ");
  const enc = op.fields
    .map(
      (f) =>
        `if(value.${fieldName(f.name)}()!=null){Writer item=new Writer(); ${encoderCall(f, `value.${fieldName(f.name)}()`, "item", "c", "flags", new Map(), typeByName)}; byte[] bytes=item.result(); encode${refType(op.fieldIdType)}(new ${refType(op.fieldIdType)}(${f.id}),body,c,flags); encode${refType(op.fieldLengthType)}(new ${refType(op.fieldLengthType)}(bytes.length),body,c,flags); body.bytes(bytes);}`,
    )
    .join("\n    ");
  const constraintFor = (field, value) =>
    (field.constraints ?? [])
      .map((constraint) => {
        if (constraint.kind !== "contains-protocol-required-capability")
          throw new Error(
            `${type.name}.${field.name}: unsupported field constraint ${constraint.kind}`,
          );
        return `require(${value}!=null&&${value}.items().stream().anyMatch(item->item.value().equals(ServiceWireConstants.REQUIRED_CAPABILITY)),${javaString(field.name + " capability")});`;
      })
      .join(" ");
  const rangeFor = (field, value) => {
    if (field.minimum === undefined && field.maximum === undefined) return "";
    const target = typeByName.get(referenceOf(field).$ref),
      targetOp = primaryOperation(target);
    if (targetOp.op !== "integer")
      throw new Error(`${type.name}.${field.name}: range on non-integer`);
    return `if(${value}!=null)requireUnsigned(${value}.value(),${targetOp.width},${field.minimum === undefined ? "null" : javaString(field.minimum)},${field.maximum === undefined ? "null" : javaString(field.maximum)},${javaString(field.name)});`;
  };
  const decodedFieldChecks = op.fields
    .map(
      (field) =>
        `${rangeFor(field, fieldName(field.name))}${constraintFor(field, fieldName(field.name))}`,
    )
    .join("");
  const encodedFieldChecks = op.fields
    .map((field) => {
      const value = `value.${fieldName(field.name)}()`;
      return `${rangeFor(field, value)}${constraintFor(field, value)}`;
    })
    .join("");
  const values = new Map(
    op.fields.map((field) => [field.name, fieldName(field.name)]),
  );
  const presence = op.presenceRules
    .map((rule) => {
      const checks = [
        ...(rule.require ?? []).map((field) => `${fieldName(field)}!=null`),
        ...(rule.forbid ?? []).map((field) => `${fieldName(field)}==null`),
      ];
      return `if(${conditionExpression(rule.when, values, "flags", flagBits)})require(${checks.join("&&")},${javaString(type.name + " presence")});`;
    })
    .join(" ");
  const encodedValues = new Map(
    op.fields.map((field) => [field.name, `value.${fieldName(field.name)}()`]),
  );
  const encodePresence = op.presenceRules
    .map((rule) => {
      const checks = [
        ...(rule.require ?? []).map(
          (field) => `value.${fieldName(field)}()!=null`,
        ),
        ...(rule.forbid ?? []).map(
          (field) => `value.${fieldName(field)}()==null`,
        ),
      ];
      return `if(${conditionExpression(rule.when, encodedValues, "flags", flagBits)})require(${checks.join("&&")},${javaString(type.name + " presence")});`;
    })
    .join("");
  const decodeLimit = encodedLimit(type, "(r.at-start)", "decode");
  const encodeLimit = encodedLimit(type, "(w.size()-start)", "encode");
  return (
    `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${decodeLimit ? "int start=r.at;" : ""} Reader body=r.slice(length(decode${refType(op.totalLengthType)}(r,c,flags))); ${vars}\n    int previous=0; while(!body.done()){int id=length(decode${refType(op.fieldIdType)}(body,c,flags));require(id>previous,${javaString(type.name + " order")});previous=id;Reader item=body.slice(length(decode${refType(op.fieldLengthType)}(body,c,flags)));switch(id){${cases} default -> item.skipRemaining(); }item.end(${javaString(type.name + " field")});} ${required} ${presence} ${decodedFieldChecks}${decodeLimit} return new ${name}(${args}); }\n` +
    `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${encodeLimit ? "int start=w.size();" : ""}${encodeRequired}${encodePresence}${encodedFieldChecks} Writer body=new Writer(); ${enc} byte[] bytes=body.result(); encode${refType(op.totalLengthType)}(new ${refType(op.totalLengthType)}(bytes.length),w,c,flags);w.bytes(bytes);${encodeLimit} }`
  );
}

function publicMethods(ir) {
  return ir.types
    .map((type) => {
      const name = typeName(type.name),
        external = unionParameters(type);
      const params = external.length ? `, ${external.join(", ")}` : "";
      const suffix = external.length
        ? `, ${external.map((_, i) => `external${i}`).join(", ")}`
        : "";
      return `  static ${name} decode${name}(byte[] bytes,DecoderContext context${params})throws IOException{Reader r=new Reader(bytes);${name} value=decode${name}(r,context,0${suffix});r.end(${javaString(type.name)});return value;}\n  static byte[] encode${name}(${name} value,DecoderContext context${params})throws IOException{Writer w=new Writer();encode${name}(value,w,context,0${suffix});return w.result();}`;
    })
    .join("\n");
}

function commandFields(command) {
  return command.operations.filter((entry) => entry.op === "field");
}

function commandDeclarations(ir) {
  const records = ir.commands.map((command) => {
    const fields = commandFields(command).map(
      (field) => `${refType(field)} ${fieldName(field.name)}`,
    );
    fields.unshift("int flags");
    const metadata = command.operations.find(
      (entry) => entry.op === "metadata-flag-frame",
    );
    const payload = operation(command, "payload");
    if (metadata) fields.push(`${refType(metadata.frame)} metadata`);
    if (payload.type) fields.push(`${refType(payload.type)} payload`);
    return `  record ${typeName(command.name)}Command(${fields.join(", ")}) implements ServiceWireCommand { public int id(){return ${operation(command, "command-header").commandId};} }`;
  });
  return `  sealed interface ServiceWireCommand permits ${ir.commands.map((c) => `${typeName(c.name)}Command`).join(", ")} {int id();int flags();}\n${records.join("\n")}`;
}

function flagValidation(command, flags, flagBits) {
  const op = operation(command, "flags");
  const allowed = op.allowed.reduce(
    (value, flag) => value | flagBits.get(flag.name),
    0,
  );
  const required = op.required.reduce(
    (value, flag) => value | flagBits.get(flag.name),
    0,
  );
  const rules = command.operations
    .filter((entry) => entry.op === "flag-constraint")
    .map((entry) => {
      if (entry.kind === "all-or-none") {
        const mask = entry.flags.reduce(
          (value, flag) => value | flagBits.get(flag.name),
          0,
        );
        return `require(((${flags}&${mask})==0)||((${flags}&${mask})==${mask}),${javaString(command.name + " flag constraint")});`;
      }
      if (entry.kind === "implies") {
        const source = flagBits.get(entry.if.name);
        const mask = entry.then.reduce(
          (value, flag) => value | flagBits.get(flag.name),
          0,
        );
        return `require(((${flags}&${source})==0)||((${flags}&${mask})==${mask}),${javaString(command.name + " flag constraint")});`;
      }
      throw new Error(
        `${command.name}: unsupported flag constraint ${entry.kind}`,
      );
    })
    .join("");
  return `require((${flags}&~${allowed})==0&&(${flags}&${required})==${required},${javaString(command.name + " flags")});${rules}`;
}

function predicateChecks(owner, values, operations = owner.operations) {
  return operations
    .filter((entry) => entry.op === "runtime-predicate")
    .map((entry) => {
      if (
        entry.reference.asset !== "service-wire-constants" ||
        entry.reference.name !== "valid-terminal-failure"
      )
        throw new Error(`${owner.name}: unsupported runtime predicate`);
      return `require(ServiceWireConstants.validTerminalFailure(${values.get("terminalResult")}.wire,${values.get("failureCode")}.wire),${javaString(owner.name + " predicate")});`;
    })
    .join("");
}

function commandMethods(ir, typeByName, flagBits) {
  const decodeMethods = ir.commands
    .map((command) => {
      const fields = commandFields(command),
        decoded = decodeFields(
          fields,
          "r",
          "c",
          "flags",
          typeByName,
          flagBits,
          "        ",
        );
      const metadata = command.operations.find(
          (entry) => entry.op === "metadata-flag-frame",
        ),
        payload = operation(command, "payload");
      const bit = metadata ? flagBits.get(metadata.flag.name) : 0;
      const metadataIndex = metadata
        ? `int next=1;${refType(metadata.frame)} metadata=null;if((flags&${bit})!=0){require(frames.size()>next,${javaString(command.name + " metadata")});metadata=decode${refType(metadata.frame)}(frames.get(next++),c);}`
        : "int next=1;";
      const payloadValue = payload.type
        ? `${refType(payload.type)} payload=null;if(frames.size()>next)payload=decode${refType(payload.type)}(frames.get(next++),c);`
        : "";
      const policy =
        payload.policy === "required"
          ? `require(payload!=null,${javaString(command.name + " payload")});`
          : payload.policy === "forbidden"
            ? `require(frames.size()==next,${javaString(command.name + " payload")});`
            : "";
      const args = [
        "flags",
        ...fields.map((field) => fieldName(field.name)),
        ...(metadata ? ["metadata"] : []),
        ...(payload.type ? ["payload"] : []),
      ].join(", ");
      const name = `${typeName(command.name)}Command`;
      return `  private static ${name} decode${name}(List<byte[]> frames,DecoderContext c,Reader r,int flags)throws IOException{${flagValidation(command, "flags", flagBits)}${metadataIndex}${payloadValue}${policy}require(frames.size()==next,${javaString(command.name + " frames")});\n${decoded.lines.join("\n")}\n${predicateChecks(command, decoded.values)}r.end(${javaString(command.name)});return new ${name}(${args});}`;
    })
    .join("\n");
  const decodeCases = ir.commands
    .map(
      (command) =>
        `case ${operation(command, "command-header").commandId}->decode${typeName(command.name)}Command(frames,c,r,flags);`,
    )
    .join("");
  const encodeMethods = ir.commands
    .map((command) => {
      const name = `${typeName(command.name)}Command`,
        fields = commandFields(command);
      const encoded = encodeFields(
        fields,
        "item",
        "w",
        "c",
        "item.flags()",
        typeByName,
        flagBits,
        "        ",
      );
      const metadata = command.operations.find(
          (entry) => entry.op === "metadata-flag-frame",
        ),
        payload = operation(command, "payload");
      const bit = metadata ? flagBits.get(metadata.flag.name) : 0;
      const metadataFrame = metadata
        ? `if((item.flags()&${bit})!=0){require(item.metadata()!=null,${javaString(command.name + " metadata")});frames.add(encode${refType(metadata.frame)}(item.metadata(),c));}else require(item.metadata()==null,${javaString(command.name + " metadata")});`
        : "";
      const payloadFrame = payload.type
        ? `${payload.policy === "required" ? `require(item.payload()!=null,${javaString(command.name + " payload")});` : ""}if(item.payload()!=null)frames.add(encode${refType(payload.type)}(item.payload(),c));`
        : "";
      const values = new Map(
        fields.map((field) => [field.name, `item.${fieldName(field.name)}()`]),
      );
      return `  private static List<byte[]> encode${name}(${name} item,DecoderContext c,Writer w)throws IOException{${flagValidation(command, "item.flags()", flagBits)}${predicateChecks(command, values)}\n${encoded.join("\n")}\nList<byte[]> frames=new ArrayList<>();frames.add(w.result());${metadataFrame}${payloadFrame}return frames;}`;
    })
    .join("\n");
  const encodeCases = ir.commands
    .map((command) => {
      const name = `${typeName(command.name)}Command`;
      return `case ${operation(command, "command-header").commandId}->encode${name}((${name})value,c,w);`;
    })
    .join("");
  const validators = ir.commands
    .filter((command) =>
      command.operations.some((entry) => entry.op === "runtime-predicate"),
    )
    .map((command) => {
      const fields = commandFields(command),
        terminal = fields.find((field) => field.name === "terminalResult"),
        failure = fields.find((field) => field.name === "failureCode");
      if (!terminal || !failure)
        throw new Error(`${command.name}: runtime predicate operands missing`);
      return `  static void validate${typeName(command.name)}Predicate(${refType(terminal)} terminalResult,${refType(failure)} failureCode)throws IOException{${predicateChecks(
        command,
        new Map([
          ["terminalResult", "terminalResult"],
          ["failureCode", "failureCode"],
        ]),
      )}}`;
    })
    .join("\n");
  const header = operation(ir.commands[0], "command-header");
  return `${decodeMethods}\n${encodeMethods}\n  static ServiceWireCommand decodeCommand(List<byte[]> frames,DecoderContext c)throws IOException{require(!frames.isEmpty(),"command frame");Reader r=new Reader(frames.get(0));require(r.u8()==${header.magic[0]}&&r.u8()==${header.magic[1]}&&r.u8()==${header.wireMajor},"header");int id=r.u8(),flags=r.u8();return switch(id){${decodeCases}default->throw error("command id");};}\n  static List<byte[]> encodeCommandFrames(ServiceWireCommand value,DecoderContext c)throws IOException{Writer w=new Writer();w.u8(${header.magic[0]});w.u8(${header.magic[1]});w.u8(${header.wireMajor});w.u8(value.id());w.u8(value.flags());return switch(value.id()){${encodeCases}default->throw error("command id");};}\n  static byte[] encodeCommand(ServiceWireCommand value,DecoderContext c)throws IOException{return encodeCommandFrames(value,c).get(0);}\n${validators}`;
}

function durableMethods(ir, typeByName) {
  const decode = ir.durableFormats
    .map(
      (format) =>
        `case ${javaString(format.name)} -> decodeDurable${typeName(format.name)}(bytes,c)`,
    )
    .join("; ");
  const encode = ir.durableFormats
    .map((format) => {
      const body = operation(format, "field");
      return `case ${javaString(format.name)} -> encodeDurable${typeName(format.name)}((${refType(body)})value,c)`;
    })
    .join("; ");
  const methods = ir.durableFormats
    .map((format) => {
      const header = operation(format, "durable-header"),
        checksum = operation(format, "checksum"),
        limit = operation(format, "encoded-limit"),
        bodyField = operation(format, "field");
      if (
        checksum.algorithm !== "crc32c-castagnoli" ||
        checksum.position !== "trailing" ||
        checksum.verification !== "before-body-interpretation" ||
        bodyField.interpretation !== "after-checksum-verification" ||
        limit.measured !== "complete-encoded-value" ||
        !limit.applications.includes("encode") ||
        !limit.applications.includes("decode")
      )
        throw new Error(
          `${format.name}: unsupported durable operation sequence`,
        );
      const body = refType(bodyField),
        name = typeName(format.name),
        flagsType = refType(header.flagsType);
      return `  private static ${body} decodeDurable${name}(byte[] bytes,DecoderContext c)throws IOException{Reader r=new Reader(bytes);${header.magic.map((b) => `require(r.u8()==${b},"magic");`).join("")}require(r.u8()==${header.formatVersion},"version");${flagsType} headerFlags=decode${flagsType}(r,c,0);require(headerFlags.value()==${longLiteral(header.flags)},"flags");Reader bodyReader=r.slice(length(decode${refType(header.bodyLengthType)}(r,c,0)));require(bytes.length<=${limit.maximumEncodedBytes},${javaString(format.name + " encoded limit")});int checksum=(int)r.uint(4);r.end(${javaString(format.name)});CRC32C crc=new CRC32C();crc.update(bytes,0,bytes.length-4);require((int)crc.getValue()==checksum,"checksum");${body} value=decode${body}(bodyReader,c,0);bodyReader.end(${javaString(format.name)});return value;}\n  private static byte[] encodeDurable${name}(${body} value,DecoderContext c)throws IOException{Writer bodyWriter=new Writer();encode${body}(value,bodyWriter,c,0);byte[] body=bodyWriter.result();Writer w=new Writer();${header.magic.map((b) => `w.u8(${b});`).join("")}w.u8(${header.formatVersion});encode${flagsType}(new ${flagsType}(${integerArgument(header.flagsType, longLiteral(header.flags), typeByName)}),w,c,0);encode${refType(header.bodyLengthType)}(new ${refType(header.bodyLengthType)}(${integerArgument(header.bodyLengthType, "body.length", typeByName)}),w,c,0);w.bytes(body);CRC32C crc=new CRC32C();byte[] covered=w.result();crc.update(covered,0,covered.length);w.uint(4,crc.getValue());byte[] result=w.result();require(result.length<=${limit.maximumEncodedBytes},${javaString(format.name + " encoded limit")});return result;}`;
    })
    .join("\n");
  return `${methods}\n  static Object decodeDurable(String name,byte[] bytes,DecoderContext c)throws IOException{return switch(name){${decode};default->throw error("durable format");};}\n  static byte[] encodeDurable(String name,Object value,DecoderContext c)throws IOException{return switch(name){${encode};default->throw error("durable format");};}`;
}

function logicalJavaFieldCheck(field, expression, typeByName) {
  const target = typeByName.get(referenceOf(field).$ref);
  const targetOp = primaryOperation(target);
  const lines = [];
  if (field.constant !== undefined) {
    const expected =
      targetOp.op === "enum"
        ? `${refType(field)}.${enumName(field.constant)}`
        : longLiteral(field.constant);
    const actual =
      targetOp.op === "enum" ? expression : `${expression}.value()`;
    lines.push(
      `require(${actual}==${expected},${javaString(field.name + " constant")});`,
    );
  }
  if (field.minimum !== undefined && targetOp.op === "integer") {
    lines.push(
      `requireUnsigned(${expression}.value(),${targetOp.width},${javaString(field.minimum)},null,${javaString(field.name)});`,
    );
  }
  if (field.maximum !== undefined && targetOp.op === "integer") {
    lines.push(
      `requireUnsigned(${expression}.value(),${targetOp.width},null,${javaString(field.maximum)},${javaString(field.name)});`,
    );
  }
  return lines.join("");
}

function logicalJavaFrameCall(reference, end, values, typeByName) {
  const target = typeByName.get(referenceOf(reference).$ref);
  const union = target.operations.find(
    (entry) => entry.op === "conditional-union",
  );
  const argumentsList = [end];
  if (union) {
    for (const discriminator of union.discriminators.filter(
      (entry) => entry.source.kind !== "wire",
    )) {
      argumentsList.push(
        discriminator.source.kind === "enclosingField"
          ? values.get(discriminator.source.name)
          : `m.context().${fieldName(discriminator.source.name)}()`,
      );
    }
  }
  return `new Logical${typeName(target.name)}Frame(${argumentsList.join(",")})`;
}

function logicalJavaFields(
  type,
  fields,
  startState,
  endExpression,
  typeByName,
  flagBits,
  valueArray = "values",
) {
  const values = new Map();
  const lines = [];
  let state = startState;
  fields.forEach((field, index) => {
    const typed = `((${refType(field)})${valueArray}[${index}])`;
    const condition = conditionExpression(field.when, values, "0", flagBits);
    lines.push(
      `case ${state}->{if(!(${condition})){${valueArray}[${index}]=null;state=${state + 2};return;}state=${state + 1};m.push(${logicalJavaFrameCall(field, endExpression, values, typeByName)});}`,
    );
    lines.push(
      `case ${state + 1}->{${valueArray}[${index}]=takeChild();${logicalJavaFieldCheck(field, typed, typeByName)}state=${state + 2};}`,
    );
    values.set(field.name, typed);
    state += 2;
  });
  return { lines, values, nextState: state };
}

function logicalJavaFrame(type, typeByName, flagBits) {
  const op = primaryOperation(type),
    name = typeName(type.name),
    frame = `Logical${name}Frame`;
  const decodeLimit = encodedLimit(type, "(m.consumed()-start)", "decode");
  if (op.op === "integer") {
    const primitive = integerPrimitive(op.encoding),
      signed = op.encoding === "i64";
    return `  private static final class ${frame} extends LogicalFrame{${frame}(long end){super(end);}void resume(LogicalMachine m)throws IOException{byte[] bytes=m.take(this,${op.width},true);if(bytes==null)return;long raw=LogicalMachine.uint(bytes);${primitive} value=${primitive === "int" ? "(int)raw" : "raw"};${signed ? "requireSigned(value" : `requireUnsigned(value,${op.width}`},${op.minimum === undefined ? "null" : javaString(op.minimum)},${op.maximum === undefined ? "null" : javaString(op.maximum)},${javaString(type.name)});${decodeLimit}m.complete(new ${name}(value));}}`;
  }
  if (op.op === "enum") {
    const cases = op.values
      .map(
        (entry) =>
          `if(raw==${longLiteral(entry.value)}){${decodeLimit}m.complete(${name}.${enumName(entry.name)});return;}`,
      )
      .join("");
    return `  private static final class ${frame} extends LogicalFrame{${frame}(long end){super(end);}void resume(LogicalMachine m)throws IOException{byte[] bytes=m.take(this,${op.width},true);if(bytes==null)return;long raw=LogicalMachine.uint(bytes);${cases}throw error(${javaString(type.name + " enum")});}}`;
  }
  if (op.op === "length-prefixed") {
    const lengthName = refType(op.lengthType),
      absent = op.zeroLengthMeaning === "absent";
    const capacity = encodeCapacity(type, op.encodeCapacity, {
      decode: "encodedLength",
      encode: "encodedLength",
    }).decode;
    const negotiated = negotiatedBound(
      type,
      "content-bytes",
      "decode",
      "length",
    ).replaceAll("c.", "m.context().");
    const prefix = `case 0->{state=1;m.push(new Logical${lengthName}Frame(end));}case 1->{long encodedLength=unsignedLength(takeChild());${capacity}try{length=Math.toIntExact(encodedLength);}catch(ArithmeticException failure){throw capacityError(${javaString(type.name + " capacity")});}require(length>=${op.minimumBytes ?? 0},${javaString(type.name + " length")});${negotiated}${absent ? `if(length==0){${decodeLimit}m.complete(new ${name}(null));return;}` : ""}`;
    if (op.content === "text") {
      const validation = type.operations.find(
          (entry) => entry.op === "text-validation",
        ),
        forbidNul = validation?.nul === "forbidden";
      return `  private static final class ${frame} extends LogicalFrame{long length,remaining;int codePoint,needed,minimum;final StringBuilder value=new StringBuilder();${frame}(long end){super(end);}void append(int item)throws IOException{try{value.appendCodePoint(item);}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " capacity")});}}String finish()throws IOException{try{return value.toString();}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " capacity")});}}void resume(LogicalMachine m)throws IOException{switch(state){${prefix}remaining=length;state=2;}case 2->{while(remaining>0){int item=m.readByte(this,0);if(item<0)return;remaining--;if(needed==0){if(item<=0x7f){${forbidNul ? `if(item==0)throw error(${javaString(type.name + " NUL")});` : ""}append(item);continue;}if(item>=0xc2&&item<=0xdf){codePoint=item&0x1f;needed=1;minimum=0x80;continue;}if(item>=0xe0&&item<=0xef){codePoint=item&0x0f;needed=2;minimum=0x800;continue;}if(item>=0xf0&&item<=0xf4){codePoint=item&0x07;needed=3;minimum=0x10000;continue;}throw error(${javaString(type.name + " UTF-8")});}if((item&0xc0)!=0x80)throw error(${javaString(type.name + " UTF-8")});codePoint=(codePoint<<6)|(item&0x3f);if(--needed==0){if(codePoint<minimum||codePoint>0x10ffff||(codePoint>=0xd800&&codePoint<=0xdfff)${forbidNul ? "||codePoint==0" : ""})throw error(${javaString(type.name + " UTF-8")});append(codePoint);}}if(needed!=0)throw error(${javaString(type.name + " UTF-8")});${decodeLimit}m.complete(new ${name}(finish()));}default->throw new IllegalStateException();}}}`;
    }
    return `  private static final class ${frame} extends LogicalFrame{long length;byte[] value;${frame}(long end){super(end);}void resume(LogicalMachine m)throws IOException{switch(state){${prefix}try{value=new byte[(int)length];}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " capacity")});}state=2;}case 2->{if(!m.fill(this,value))return;${decodeLimit}m.complete(new ${name}(value));}default->throw new IllegalStateException();}}}`;
  }
  if (op.op === "struct" || op.op === "versioned-length-delimited") {
    const delimited = op.op === "versioned-length-delimited";
    let prefix = "",
      startState = 0,
      bodyEnd = "end";
    if (delimited) {
      prefix = `case 0->{state=1;m.push(new Logical${refType(op.version)}Frame(end));}case 1->{${refType(op.version)} version=(${refType(op.version)})takeChild();require(version.value()==${longLiteral(op.version.constant)},${javaString(type.name + " version")});state=2;m.push(new Logical${refType(op.length)}Frame(end));}case 2->{long length=unsignedLength(takeChild());bodyEnd=m.bound(end,length);state=3;}`;
      startState = 3;
      bodyEnd = "bodyEnd";
    }
    const fields = logicalJavaFields(
      type,
      op.fields,
      startState,
      bodyEnd,
      typeByName,
      flagBits,
    );
    const args = op.fields
      .map((field, index) => `((${refType(field)})values[${index}])`)
      .join(",");
    const constraints = structConstraints(type, op.constraints ?? [], "value");
    const predicateValues = new Map(
      op.fields.map((field) => [
        field.name,
        `value.${fieldName(field.name)}()`,
      ]),
    );
    const predicates = predicateChecks(type, predicateValues);
    const finish = `${delimited ? `m.exact(bodyEnd,${javaString(type.name)});` : ""}var value=new ${name}(${args});${constraints}${predicates}${decodeLimit}m.complete(value);`;
    return `  private static final class ${frame} extends LogicalFrame{final Object[] values=new Object[${op.fields.length}];${delimited ? "long bodyEnd;" : ""}${frame}(long end){super(end);}void resume(LogicalMachine m)throws IOException{switch(state){${prefix}${fields.lines.join("")}case ${fields.nextState}->{${finish}}default->throw new IllegalStateException();}}}`;
  }
  if (op.op === "vector" || op.op === "versioned-vector") {
    const versioned = op.op === "versioned-vector";
    const repeat = versioned
      ? op.layout.find((entry) => entry.kind === "repeat")
      : { name: "items", item: op.item };
    const countRef = versioned
      ? op.layout.find((entry) => entry.counts)
      : op.countType;
    const prefix = versioned
      ? `case 0->{state=1;m.push(new Logical${refType(op.layout[0])}Frame(end));}case 1->{${refType(op.layout[0])} version=(${refType(op.layout[0])})takeChild();require(version.value()==${longLiteral(op.layout[0].constant)},${javaString(type.name + " version")});state=2;m.push(new Logical${refType(countRef)}Frame(end));}case 2->{long encodedCount=unsignedLength(takeChild());if(encodedCount>Integer.MAX_VALUE)throw capacityError(${javaString(type.name + " capacity")});count=(int)encodedCount;${op.maximumItems === undefined ? "" : `require(count<=${op.maximumItems},${javaString(type.name + " count")});`}values=new ArrayList<>();state=3;}`
      : `case 0->{state=1;m.push(new Logical${refType(countRef)}Frame(end));}case 1->{long encodedCount=unsignedLength(takeChild());if(encodedCount>Integer.MAX_VALUE)throw capacityError(${javaString(type.name + " capacity")});count=(int)encodedCount;${op.maximumItems === undefined ? "" : `require(count<=${op.maximumItems},${javaString(type.name + " count")});`}values=new ArrayList<>();state=2;}`;
    const loopState = versioned ? 3 : 2;
    const validation = vectorConstraintCode(type, op, repeat.item, "values");
    return `  private static final class ${frame} extends LogicalFrame{int count,index;List<${refType(repeat.item)}> values;${frame}(long end){super(end);}void resume(LogicalMachine m)throws IOException{switch(state){${prefix}case ${loopState}->{if(index==count){${validation}${decodeLimit}m.complete(new ${name}(values));return;}state=${loopState + 1};m.push(${logicalJavaFrameCall(repeat.item, "end", new Map(), typeByName)});}case ${loopState + 1}->{try{values.add((${refType(repeat.item)})takeChild());}catch(OutOfMemoryError failure){throw capacityError(${javaString(type.name + " capacity")});}index++;state=${loopState};}default->throw new IllegalStateException();}}}`;
  }
  if (op.op === "conditional-union") {
    const discriminators = op.discriminators;
    const external = discriminators.filter(
      (entry) => entry.source.kind !== "wire",
    );
    const externalParameters = external
      .map((entry, index) => `${refType(entry)} external${index}`)
      .join(",");
    const externalAssignments = [];
    let externalIndex = 0;
    const discriminatorValues = new Map();
    discriminators.forEach((entry, index) => {
      if (entry.source.kind !== "wire")
        externalAssignments.push(
          `values[${index}]=external${externalIndex++};`,
        );
      discriminatorValues.set(
        entry.name,
        `((${refType(entry)})values[${index}])`,
      );
    });
    const lines = [];
    let state = 0;
    discriminators.forEach((entry, index) => {
      if (entry.source.kind === "wire") {
        lines.push(
          `case ${state}->{state=${state + 1};m.push(${logicalJavaFrameCall(entry, "end", discriminatorValues, typeByName)});}`,
        );
        lines.push(
          `case ${state + 1}->{values[${index}]=takeChild();state=${state + 2};}`,
        );
        state += 2;
      }
    });
    if (op.bodyLengthType) {
      lines.push(
        `case ${state}->{state=${state + 1};m.push(new Logical${refType(op.bodyLengthType)}Frame(end));}`,
      );
      lines.push(
        `case ${state + 1}->{bodyEnd=m.bound(end,unsignedLength(takeChild()));state=${state + 2};}`,
      );
      state += 2;
    }
    const selectState = state;
    const cases = caseInfo(type, op);
    const caseBlocks = [];
    let next = state + 1;
    cases.forEach((entry, caseIndex) => {
      const test =
        entry.discriminator === null
          ? "true"
          : Object.entries(entry.discriminator)
              .map(([key, expected]) => {
                const d = discriminators.find((item) => item.name === key),
                  target = typeByName.get(referenceOf(d).$ref);
                const value = discriminatorValues.get(key);
                return primaryOperation(target).op === "enum"
                  ? `${value}==${refType(d)}.${enumName(expected)}`
                  : `${value}.value()==${longLiteral(expected)}`;
              })
              .join("&&");
      const fields = caseFields(entry.selected);
      caseBlocks.push(
        `${caseIndex ? "else " : ""}if(${test}){selected=${caseIndex};caseValues=new Object[${fields.length}];state=${next};return;}`,
      );
      const fieldStates = logicalJavaFields(
        type,
        fields,
        next,
        op.bodyLengthType ? "bodyEnd" : "end",
        typeByName,
        flagBits,
        "caseValues",
      );
      lines.push(...fieldStates.lines);
      const wireArgs = discriminators
        .filter((d) => d.source.kind === "wire")
        .map((d) => discriminatorValues.get(d.name));
      const args = [
        ...wireArgs,
        ...fields.map(
          (field, index) => `((${refType(field)})caseValues[${index}])`,
        ),
      ].join(",");
      const operations = caseOperations(entry.selected);
      const constraints = operations
        .filter((item) => item.op === "constraint")
        .map((item) => structConstraints(type, [item], "value"))
        .join("");
      const predicateMap = new Map(
        fields.map((field) => [field.name, `value.${fieldName(field.name)}()`]),
      );
      const predicates =
        predicateMap.has("terminalResult") && predicateMap.has("failureCode")
          ? predicateChecks(type, predicateMap, operations)
          : "";
      lines.push(
        `case ${fieldStates.nextState}->{${op.bodyLengthType ? `m.exact(bodyEnd,${javaString(type.name)});` : ""}var value=new ${entry.name}(${args});${constraints}${predicates}${decodeLimit}m.complete(value);}`,
      );
      next = fieldStates.nextState + 1;
    });
    lines.push(
      `case ${selectState}->{${caseBlocks.join("")}throw error(${javaString(type.name + " discriminator")});}`,
    );
    return `  private static final class ${frame} extends LogicalFrame{final Object[] values=new Object[${discriminators.length}];Object[] caseValues;int selected;${op.bodyLengthType ? "long bodyEnd;" : ""}${frame}(long end${externalParameters ? `,${externalParameters}` : ""}){super(end);${externalAssignments.join("")}}void resume(LogicalMachine m)throws IOException{switch(state){${lines.join("")}default->throw new IllegalStateException();}}}`;
  }
  throw new Error(`${type.name}: unsupported logical frame ${op.op}`);
}

function logicalMethods(
  ir,
  typeByName = globalTypeByName,
  flagBits = globalFlagBits,
) {
  const format = ir.relocationLogicalStreamFormat,
    stream = operation(format, "logical-stream"),
    limit = operation(format, "encoded-limit"),
    body = refType(stream.body),
    name = typeName(format.name);
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
  const frames = machine.frames.reachableTypes
    .map((type) => logicalJavaFrame(typeByName.get(type), typeByName, flagBits))
    .join("\n");
  return `  enum LogicalDecodeProgress { NEED_MORE, COMPLETE }
  enum LogicalFailureKind { TRUNCATED, TRAILING, CAPACITY, LIMIT, PROTOCOL }
  static final class LogicalDecodeException extends IOException { private final LogicalFailureKind kind; private final long offset; LogicalDecodeException(LogicalFailureKind kind,long offset,Throwable cause){super(kind.toString().toLowerCase()+" at byte "+offset,cause);this.kind=kind;this.offset=offset;} LogicalFailureKind kind(){return kind;} long offset(){return offset;} }
  private static final class LogicalInternalException extends IOException { final LogicalFailureKind kind; LogicalInternalException(LogicalFailureKind kind,String message){super(message);this.kind=kind;} }
  record LogicalDecodeStep<T>(LogicalDecodeProgress progress,T value,int bufferedInputByteCount,long consumedByteCount,int continuationDepth) {}
  private abstract static class LogicalFrame { final long end; long start; int state,filled; Object child; byte[] scratch; LogicalFrame(long end){this.end=end;} Object takeChild(){Object value=child;child=null;return value;} abstract void resume(LogicalMachine machine)throws IOException; }
  private static final class LogicalMachine {
    private final DecoderContext context; private final ArrayDeque<LogicalFrame> stack=new ArrayDeque<>(); private byte[] chunk; private int at,buffered; private boolean finalChunk,waiting,awaitingFinal; private long consumed; private Object result;
    LogicalMachine(DecoderContext context){this.context=context;push(new Logical${body}Frame(Long.MAX_VALUE));}
    DecoderContext context(){return context;} long consumed(){return consumed;} int depth(){return stack.size();} int buffered(){return buffered;}
    static long uint(byte[] bytes){long value=0;for(byte item:bytes)value=(value<<8)|(item&255L);return value;}
    long bound(long parentEnd,long length)throws IOException{if(length<0||consumed>parentEnd||length>parentEnd-consumed)throw error("incremental bounded length");return consumed+length;}
    void exact(long expected,String name)throws IOException{if(consumed!=expected)throw error(name+": bounded body remainder");}
    void push(LogicalFrame frame){frame.start=consumed;stack.push(frame);}
    void complete(Object value){stack.pop();if(stack.isEmpty())result=value;else stack.peek().child=value;}
    byte[] take(LogicalFrame frame,int count,boolean scalar)throws IOException{if(!scalar||count<0||count>8)throw new IllegalStateException("incremental scalar read");int remaining=count-frame.filled;if(remaining<0||consumed>frame.end||remaining>frame.end-consumed)throw error("bounded logical value");if(frame.scratch==null)try{frame.scratch=new byte[count];}catch(OutOfMemoryError failure){throw capacityError("incremental scalar capacity");}while(frame.filled<count){int available=chunk.length-at;if(available==0){if(finalChunk)throw new EOFException("truncated logical stream");waiting=true;buffered=frame.filled;return null;}int copied=Math.min(count-frame.filled,available);System.arraycopy(chunk,at,frame.scratch,frame.filled,copied);at+=copied;frame.filled+=copied;consumed+=copied;}byte[] value=frame.scratch;frame.scratch=null;frame.filled=0;buffered=0;return value;}
    boolean fill(LogicalFrame frame,byte[] value)throws IOException{if(consumed>frame.end||value.length-frame.filled>frame.end-consumed)throw error("bounded logical value");while(frame.filled<value.length){int available=chunk.length-at;if(available==0){if(finalChunk)throw new EOFException("truncated logical stream");waiting=true;buffered=0;return false;}int copied=Math.min(value.length-frame.filled,available);System.arraycopy(chunk,at,value,frame.filled,copied);at+=copied;frame.filled+=copied;consumed+=copied;}buffered=0;return true;}
    int readByte(LogicalFrame frame,int retained)throws IOException{if(consumed>=frame.end)throw error("bounded logical value");if(at==chunk.length){if(finalChunk)throw new EOFException("truncated logical stream");waiting=true;buffered=Math.min(retained,7);return -1;}consumed++;return chunk[at++]&255;}
    Object run(byte[] input,boolean finalInput)throws IOException{chunk=input;at=0;finalChunk=finalInput;waiting=false;buffered=0;try{if(awaitingFinal){if(input.length!=0)throw new LogicalInternalException(LogicalFailureKind.TRAILING,"logical stream trailing bytes");if(finalInput)return result;waiting=true;return null;}while(!waiting&&result==null)stack.peek().resume(this);if(result!=null){if(at!=input.length)throw new LogicalInternalException(LogicalFailureKind.TRAILING,"logical stream trailing bytes");if(finalInput)return result;awaitingFinal=true;waiting=true;return null;}if(at!=input.length)throw new IllegalStateException("logical decoder did not consume chunk");if(finalInput)throw new EOFException("truncated logical stream");return null;}finally{chunk=null;}}
  }
${frames}
  static final class Logical${name}Decoder {
    static final int MAXIMUM_CONTINUATION_DEPTH=${machine.frames.maximumDepth};
    private final LogicalMachine machine; private boolean terminal; private long totalInputByteCount;
    Logical${name}Decoder(DecoderContext context){machine=new LogicalMachine(context);}
    void setSyntheticPriorInputByteCountForTest(long value){if(totalInputByteCount!=0)throw new IllegalStateException();totalInputByteCount=value;}
    LogicalDecodeStep<${body}> push(byte[] chunk,boolean finalChunk)throws IOException{if(terminal)throw new IllegalStateException("terminal logical decoder push");if((long)chunk.length>${longLiteral(limit.maximumEncodedBytes)}-totalInputByteCount){terminal=true;throw new LogicalDecodeException(LogicalFailureKind.LIMIT,machine.consumed(),error(${javaString(format.name + " encoded limit")}));}totalInputByteCount+=chunk.length;try{Object value=machine.run(chunk,finalChunk);if(value==null)return new LogicalDecodeStep<>(LogicalDecodeProgress.NEED_MORE,null,machine.buffered(),machine.consumed(),machine.depth());terminal=true;return new LogicalDecodeStep<>(LogicalDecodeProgress.COMPLETE,(${body})value,0,machine.consumed(),0);}catch(IOException failure){terminal=true;LogicalFailureKind kind=failure instanceof LogicalInternalException internal?internal.kind:failure instanceof CapacityException?LogicalFailureKind.CAPACITY:failure instanceof EOFException?LogicalFailureKind.TRUNCATED:LogicalFailureKind.PROTOCOL;throw new LogicalDecodeException(kind,machine.consumed(),failure);}}
  }
  static ${body} decodeLogical${name}(byte[] bytes,DecoderContext c)throws IOException{LogicalDecodeStep<${body}> step=new Logical${name}Decoder(c).push(bytes,true);if(step.progress()!=LogicalDecodeProgress.COMPLETE)throw new EOFException("truncated logical stream");return step.value();}
  static byte[] encodeLogical${name}(${body} value,DecoderContext c)throws IOException{byte[] bytes=encode${body}(value,c);require((long)bytes.length<=${longLiteral(limit.maximumEncodedBytes)},${javaString(format.name + " encoded limit")});return bytes;}`;
}

function render(ir) {
  const typeByName = new Map(ir.types.map((type) => [type.name, type]));
  globalTypeByName = typeByName;
  assertOperationVocabulary(ir);
  const flagBits = new Map(ir.flags.map((flag) => [flag.name, flag.bit]));
  globalFlagBits = flagBits;
  const negotiatedContexts = [
    ...new Set(
      ir.types.flatMap((type) =>
        type.operations
          .filter((entry) => entry.op === "negotiated-bound")
          .map((entry) => entry.context.name),
      ),
    ),
  ]
    .sort()
    .map((name) => ({ name, negotiated: true }));
  const context = [...ir.semanticContexts, ...negotiatedContexts]
    .map((entry) => {
      if (entry.negotiated) return `Long ${fieldName(entry.name)}`;
      const type = entry.valueType ? refType(entry.valueType) : "Boolean";
      return `${type} ${fieldName(entry.name)}`;
    })
    .join(", ");
  const declarations = ir.types.map(declaration).join("\n");
  const codecs = ir.types
    .map((type) => codecMethods(type, typeByName, flagBits))
    .join("\n");
  return `// <auto-generated> DO NOT EDIT. Generated solely from validated service-wire operation IR.\npackage systems.zlink.framework.runtime.internal.service;\n\nimport java.io.*;\nimport java.math.BigInteger;\nimport java.nio.*;\nimport java.nio.charset.*;\nimport java.util.*;\nimport java.util.zip.CRC32C;\nimport systems.zlink.framework.runtime.protocol.ServiceWireConstants;\n\nfinal class ServiceWireCodec {\n  private ServiceWireCodec(){}\n  record DecoderContext(${context}) {}\n${declarations}\n${commandDeclarations(ir)}\n${runtimeHelpers()}\n${codecs}\n${publicMethods(ir)}\n${commandMethods(ir, typeByName, flagBits)}\n${durableMethods(ir, typeByName)}\n${logicalMethods(ir)}\n}\n`;
}

function assertOperationVocabulary(ir) {
  const implemented = new Set([
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
    "logical-stream",
    "runtime-predicate",
    "negotiated-bound",
  ]);
  for (const name of ir.operationVocabulary)
    if (!implemented.delete(name))
      throw new Error(`unsupported operation ${name}`);
  if (implemented.size !== 0)
    throw new Error(
      `operation vocabulary missing ${[...implemented].join(",")}`,
    );
}

function runtimeHelpers() {
  return String.raw`  private static final class Reader { final byte[] bytes; int at; Reader(byte[] bytes){this.bytes=bytes;} int u8()throws IOException{return(int)uint(1);} long uint(int n)throws IOException{long v=0;for(byte b:bytes(n))v=(v<<8)|Byte.toUnsignedLong(b);return v;} long i64()throws IOException{return uint(8);} byte[] bytes(int n)throws IOException{need(n);return Arrays.copyOfRange(bytes,at,at+=n);} Reader slice(int n)throws IOException{need(n);return new Reader(bytes(n));} void skipRemaining(){at=bytes.length;} int remaining(){return bytes.length-at;} boolean done(){return remaining()==0;} void end(String name)throws IOException{require(done(),name+" trailing");} void need(int n)throws IOException{if(n<0||n>remaining())throw new EOFException("truncated field");} }
  private static final class Writer { final List<byte[]> chunks=new ArrayList<>(); int length; void u8(int v){bytes(new byte[]{(byte)v});} void uint(int n,long v){byte[] bytes=new byte[n];for(int i=n-1;i>=0;i--)bytes[n-1-i]=(byte)(v>>>(8*i));bytes(bytes);} void bytes(byte[] v){if(v.length==0)return;chunks.add(v);length=Math.addExact(length,v.length);} int size(){return length;} byte[] result(){if(chunks.isEmpty())return new byte[0];if(chunks.size()==1)return chunks.get(0);byte[] result=new byte[length];int at=0;for(byte[] chunk:chunks){System.arraycopy(chunk,0,result,at,chunk.length);at+=chunk.length;}return result;} }
  private record ByteKey(byte[] value) { ByteKey{value=value.clone();} public boolean equals(Object other){return other instanceof ByteKey key&&Arrays.equals(value,key.value);} public int hashCode(){return Arrays.hashCode(value);} }
  private static final class CapacityException extends IOException { CapacityException(String value){super(value);} } private static IOException error(String value){return new IOException(value);} private static IOException capacityError(String value){return new CapacityException(value);} private static void require(boolean ok,String message)throws IOException{if(!ok)throw error(message);}
  private static void requireUnsigned(long value,int width,String minimum,String maximum,String name)throws IOException{BigInteger actual=new BigInteger(width==8?Long.toUnsignedString(value):Long.toString(Integer.toUnsignedLong((int)value)));if(minimum!=null&&actual.compareTo(new BigInteger(minimum))<0||maximum!=null&&actual.compareTo(new BigInteger(maximum))>0)throw error(name+" range");}
  private static void requireSigned(long value,String minimum,String maximum,String name)throws IOException{if(minimum!=null&&value<Long.parseLong(minimum)||maximum!=null&&value>Long.parseLong(maximum))throw error(name+" range");}
  private static long unsignedLength(Object value)throws IOException{return switch(value){case U8 x->x.value();case U16 x->x.value();case U32 x->Integer.toUnsignedLong(x.value());case U64 x->x.value();default->throw error("length type");};} private static int length(Object value)throws IOException{try{long length=unsignedLength(value);if(length<0)throw error("length range");return Math.toIntExact(length);}catch(ArithmeticException e){throw error("length range");}}
  private static byte[] strictBytes(String value)throws IOException{if(value==null)return null;try{ByteBuffer encoded=StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value));byte[] bytes=new byte[encoded.remaining()];encoded.get(bytes);for(byte b:bytes)if(b==0)throw error("NUL text");return bytes;}catch(CharacterCodingException e){throw error("invalid UTF-16 text");}}
  private static String strictText(byte[] value,String name)throws IOException{for(byte b:value)if(b==0)throw error(name+" NUL");try{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();}catch(CharacterCodingException e){throw error(name+" UTF-8");}}
  private static int compareUnsigned(byte[] left,byte[] right){int count=Math.min(left.length,right.length);for(int i=0;i<count;i++){int compared=Integer.compare(Byte.toUnsignedInt(left[i]),Byte.toUnsignedInt(right[i]));if(compared!=0)return compared;}return Integer.compare(left.length,right.length);}
  private static void canonicalAuthorityKey(Writer writer,String prefix,String kind,String separator,String...components)throws IOException{writer.bytes((prefix+separator+kind).getBytes(StandardCharsets.US_ASCII));for(String component:components){byte[] raw=strictBytes(component);writer.bytes((separator+raw.length+separator).getBytes(StandardCharsets.US_ASCII));for(byte value:raw){int current=Byte.toUnsignedInt(value);if(current>='A'&&current<='Z'||current>='a'&&current<='z'||current>='0'&&current<='9'||current=='-'||current=='.'||current=='_'||current=='~')writer.u8(current);else{writer.u8('%');writer.u8("0123456789ABCDEF".charAt(current>>>4));writer.u8("0123456789ABCDEF".charAt(current&15));}}}}
`;
}

const [mode, schema, outputArgument, ...rest] = process.argv.slice(2);
if (
  !schema ||
  !outputArgument ||
  rest.length ||
  !["--write", "--check"].includes(mode)
) {
  console.error(
    "usage: node render-service-wire-java.mjs --write|--check <schema> <output>",
  );
  process.exit(2);
}
const output = path.resolve(outputArgument);
try {
  const source = render(lowerSchema(path.resolve(schema))).replace(
    /[ \t]+$/gm,
    "",
  );
  if (mode === "--write") {
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, source);
  } else if (
    !fs.existsSync(output) ||
    fs.readFileSync(output, "utf8") !== source
  ) {
    throw new Error(
      `generated Java service-wire codec drift: ${path.relative(process.cwd(), output)}`,
    );
  }
} catch (error) {
  console.error(error.stack ?? error.message ?? String(error));
  process.exit(1);
}

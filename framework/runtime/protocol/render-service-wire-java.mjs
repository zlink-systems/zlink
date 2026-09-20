#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema } from "./service-wire-lowering.mjs";

const JAVA_KEYWORDS = new Set([
  "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class",
  "const", "continue", "default", "do", "double", "else", "enum", "extends", "final",
  "finally", "float", "for", "goto", "if", "implements", "import", "instanceof", "int",
  "interface", "long", "native", "new", "package", "private", "protected", "public", "return",
  "short", "static", "strictfp", "super", "switch", "synchronized", "this", "throw", "throws",
  "transient", "try", "void", "volatile", "while", "record", "sealed", "permits", "yield",
]);

function typeName(value) {
  const result = value.split(/[^A-Za-z0-9]+/).filter(Boolean)
    .map((part) => part[0].toUpperCase() + part.slice(1)).join("");
  return /^[0-9]/.test(result) ? `N${result}` : result;
}

function fieldName(value) {
  const result = value.replace(/[^A-Za-z0-9_$]/g, "_");
  return JAVA_KEYWORDS.has(result) ? `${result}Value` : result;
}

function enumName(value) {
  const result = value.replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/[^A-Za-z0-9]+/g, "_").toUpperCase();
  return /^[0-9]/.test(result) ? `N_${result}` : result;
}

function referenceOf(value) { return value.$ref === undefined ? value.type : value; }
function refType(reference) { return typeName(referenceOf(reference).$ref); }
function integerPrimitive(encoding) { return encoding === "u64" || encoding === "i64" ? "long" : "int"; }
function integerWidth(encoding) { return { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[encoding]; }
function javaString(value) { return JSON.stringify(String(value)); }
function longLiteral(value) {
  const text = String(value);
  return BigInt(text) > 9223372036854775807n ? `Long.parseUnsignedLong(${javaString(text)})` : `${text}L`;
}

function integerArgument(reference, expression, typeByName) {
  const type = typeByName.get(reference.$ref);
  return integerPrimitive(operation(type, "integer").encoding) === "int" ? `(int)(${expression})` : expression;
}

function operation(owner, name) {
  const result = owner.operations.find((entry) => entry.op === name);
  if (!result) throw new Error(`${owner.name}: missing ${name} operation`);
  return result;
}

function primaryOperation(owner) {
  return owner.operations.find((entry) => [
    "integer", "enum", "length-prefixed", "struct", "vector", "versioned-vector",
    "versioned-length-delimited", "conditional-union", "tlv32",
  ].includes(entry.op));
}

function caseInfo(type, union = operation(type, "conditional-union")) {
  const used = new Map();
  const cases = Object.entries(union.cases).map(([signature, selected], index) => {
    const discriminator = JSON.parse(signature);
    let suffix = Object.values(discriminator).map(typeName).join("") || `Case${index}`;
    const count = used.get(suffix) ?? 0;
    used.set(suffix, count + 1);
    if (count > 0) suffix += count + 1;
    return { signature, discriminator, selected, name: `${typeName(type.name)}${suffix}` };
  });
  if (union.otherwise.kind === "fields") {
    cases.push({ signature: null, discriminator: null, selected: union.otherwise, name: `${typeName(type.name)}Otherwise` });
  }
  return cases;
}

function recordComponents(fields) {
  return fields.map((field) => `${refType(field)} ${fieldName(field.name)}`).join(", ");
}

function declaration(type) {
  const name = typeName(type.name);
  const op = primaryOperation(type);
  switch (op.op) {
    case "integer": return `  record ${name}(${integerPrimitive(op.encoding)} value) {}`;
    case "enum": return `  enum ${name} { ${op.values.map((v) => `${enumName(v.name)}(${longLiteral(v.value)})`).join(", ")}; final long wire; ${name}(long wire){this.wire=wire;} }`;
    case "length-prefixed": return op.content === "bytes"
      ? `  record ${name}(byte[] value) { ${name}{value=value==null?null:value.clone();} public byte[] value(){return value==null?null:value.clone();} }`
      : `  record ${name}(String value) {}`;
    case "struct": return `  record ${name}(${recordComponents(op.fields)}) {}`;
    case "vector": return `  record ${name}(List<${refType(op.item)}> items) { ${name}{items=List.copyOf(items);} }`;
    case "versioned-vector": {
      const repeat = op.layout.find((entry) => entry.kind === "repeat");
      return `  record ${name}(List<${refType(repeat.item)}> ${fieldName(repeat.name)}) { ${name}{${fieldName(repeat.name)}=List.copyOf(${fieldName(repeat.name)});} }`;
    }
    case "versioned-length-delimited": return `  record ${name}(${recordComponents(op.fields)}) {}`;
    case "conditional-union": {
      const variants = caseInfo(type, op);
      const records = variants.map((entry) => {
        const wire = entry.discriminator === null ? [] : op.discriminators.filter((d) => d.source.kind === "wire");
        const fields = [...wire, ...entry.selected.fields];
        return `  record ${entry.name}(${recordComponents(fields)}) implements ${name} {}`;
      }).join("\n");
      return `  sealed interface ${name} permits ${variants.map((v) => v.name).join(", ")} {}\n${records}`;
    }
    case "tlv32": return `  record ${name}(${recordComponents(op.fields)}) {}`;
    default: throw new Error(`${type.name}: unsupported primary operation ${op?.op}`);
  }
}

function conditionExpression(condition, values, flags, flagBits) {
  if (!condition) return "true";
  return condition.all.map((atom) => {
    if (atom.kind === "fieldPresent") return `${values.get(atom.operand.name)} != null`;
    if (atom.kind === "fieldEquals") {
      const variable = values.get(atom.operand.name);
      const expected = atom.value;
      return `${variable} != null && ${variable}.toString().equals(${javaString(String(expected).toUpperCase())})`;
    }
    if (atom.kind === "contextEquals") {
      const expected = atom.value === true ? "Boolean.TRUE" : atom.value === false ? "Boolean.FALSE" : javaString(atom.value);
      return `Objects.equals(c.${fieldName(atom.operand.name)}(),${expected})`;
    }
    const mask = atom.operands.reduce((result, operand) => result | flagBits.get(operand.name), 0);
    return atom.kind === "allFlagsSet" ? `((${flags} & ${mask}) == ${mask})` : `((${flags} & ${mask}) != 0)`;
  }).join(" && ");
}

function decoderCall(reference, reader, context, flags, values, typeByName) {
  reference = referenceOf(reference);
  const target = typeByName.get(reference.$ref);
  const argumentsList = [reader, context, flags];
  const union = target.operations.find((entry) => entry.op === "conditional-union");
  if (union) {
    for (const discriminator of union.discriminators.filter((d) => d.source.kind !== "wire")) {
      argumentsList.push(discriminator.source.kind === "enclosingField"
        ? values.get(discriminator.source.name)
        : `${context}.${fieldName(discriminator.source.name)}()`);
    }
  }
  return `decode${typeName(reference.$ref)}(${argumentsList.join(", ")})`;
}

function encoderCall(reference, value, writer, context, flags, values, typeByName) {
  reference = referenceOf(reference);
  const target = typeByName.get(reference.$ref);
  const argumentsList = [value, writer, context, flags];
  const union = target.operations.find((entry) => entry.op === "conditional-union");
  if (union) {
    for (const discriminator of union.discriminators.filter((d) => d.source.kind !== "wire")) {
      argumentsList.push(discriminator.source.kind === "enclosingField"
        ? values.get(discriminator.source.name)
        : `${context}.${fieldName(discriminator.source.name)}()`);
    }
  }
  return `encode${typeName(reference.$ref)}(${argumentsList.join(", ")})`;
}

function decodeFields(fields, reader, context, flags, typeByName, flagBits, indent = "    ") {
  const values = new Map();
  const lines = [];
  for (const field of fields) {
    const variable = fieldName(field.name);
    const javaType = refType(field);
    const expression = decoderCall(field, reader, context, flags, values, typeByName);
    if (field.when) {
      lines.push(`${indent}${javaType} ${variable} = null;`);
      lines.push(`${indent}if (${conditionExpression(field.when, values, flags, flagBits)}) ${variable} = ${expression};`);
    } else {
      lines.push(`${indent}${javaType} ${variable} = ${expression};`);
    }
    const target = typeByName.get(field.type.$ref ?? field.$ref);
    const targetOp = primaryOperation(target);
    if (field.constant !== undefined) {
      const expected = targetOp.op === "enum"
        ? `${refType(field)}.${enumName(field.constant)}`
        : longLiteral(field.constant);
      const actual = targetOp.op === "enum" ? variable : `${variable}.value()`;
      lines.push(`${indent}require(${actual} == ${expected}, ${javaString(field.name + " constant")});`);
    }
    if (field.minimum !== undefined && targetOp.op === "integer") lines.push(`${indent}requireUnsigned(${variable}.value(), ${targetOp.width}, ${javaString(field.minimum)}, null, ${javaString(field.name)});`);
    if (field.maximum !== undefined && targetOp.op === "integer") lines.push(`${indent}requireUnsigned(${variable}.value(), ${targetOp.width}, null, ${javaString(field.maximum)}, ${javaString(field.name)});`);
    values.set(field.name, variable);
  }
  return { lines, values };
}

function encodeFields(fields, owner, writer, context, flags, typeByName, flagBits, indent = "    ") {
  const values = new Map(fields.map((field) => [field.name, `${owner}.${fieldName(field.name)}()`]));
  const lines = [];
  for (const field of fields) {
    const value = values.get(field.name);
    if (field.when) {
      const present = conditionExpression(field.when, values, flags, flagBits);
      lines.push(`${indent}if (${present}) { require(${value} != null, ${javaString(field.name + " required")}); ${encoderCall(field, value, writer, context, flags, values, typeByName)}; } else require(${value} == null, ${javaString(field.name + " forbidden")});`);
    } else {
      lines.push(`${indent}${encoderCall(field, value, writer, context, flags, values, typeByName)};`);
    }
  }
  return lines;
}

function structConstraints(type, constraints, owner = null) {
  const access = (name) => owner ? `${owner}.${fieldName(name)}()` : fieldName(name);
  return constraints.map((constraint) => {
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
      const creationField = primaryOperation(type).fields.find((field) => field.name === "creation");
      const creationType = globalTypeByName.get(referenceOf(creationField).$ref);
      const existing = caseInfo(creationType).find((entry) => entry.discriminator?.createResult === constraint.when["creation.createResult"]);
      if (!existing) throw new Error(`${type.name}: existing creation case missing`);
      return `if(${access("creation")} instanceof ${existing.name})require(${access("hasApplicationPayload")}==Bool8.${enumName(constraint.requires.hasApplicationPayload)},${javaString(type.name + " existing shape")});`;
    }
    throw new Error(`${type.name}: unsupported struct constraint ${constraint.kind}`);
  }).join(" ");
}

function integerMethods(type, op) {
  const name = typeName(type.name), primitive = integerPrimitive(op.encoding), width = op.width;
  const signed = op.encoding === "i64";
  const widthArgument = signed ? "" : `, ${width}`;
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${primitive} value=${width === 8 ? "r.i64()" : `(int)r.uint(${width})`}; ${signed ? "requireSigned" : "requireUnsigned"}(value${widthArgument}, ${op.minimum === undefined ? "null" : javaString(op.minimum)}, ${op.maximum === undefined ? "null" : javaString(op.maximum)}, ${javaString(type.name)}); return new ${name}(value); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${signed ? "requireSigned" : "requireUnsigned"}(value.value()${widthArgument}, ${op.minimum === undefined ? "null" : javaString(op.minimum)}, ${op.maximum === undefined ? "null" : javaString(op.maximum)}, ${javaString(type.name)}); w.uint(${width}, value.value()); }`;
}

function enumMethods(type, op) {
  const name = typeName(type.name), width = op.width;
  const decodeCases = op.values.map((v) => `if(wire==${longLiteral(v.value)})return ${name}.${enumName(v.name)};`).join("");
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { long wire=r.uint(${width});${decodeCases}throw error(${javaString(type.name + " enum")}); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags){w.uint(${width},value.wire);}`;
}

function vectorConstraintCode(type, op, itemReference, listExpression) {
  const constraints = op.constraints ?? [];
  if (constraints.length === 0) return "";
  const itemType = refType(itemReference);
  const paths = (constraint) => constraint.fields ?? (constraint.field ? [constraint.field] : []);
  const key = (constraint, variable, writer) => {
    const selected = paths(constraint);
    if (selected.length === 0) {
      return `${encoderCall(itemReference, variable, writer, "c", "flags", new Map(), globalTypeByName)};`;
    }
    return selected.map((entry) => {
      const field = fieldName(entry.path);
      const itemTypeOwner = globalTypeByName.get(referenceOf(itemReference).$ref);
      const itemOp = primaryOperation(itemTypeOwner);
      const fieldOp = (itemOp.fields ?? []).find((candidate) => candidate.name === entry.path);
      if (!fieldOp) throw new Error(`${type.name}: unknown constraint field ${entry.path}`);
      return `${encoderCall(fieldOp, `${variable}.${field}()`, writer, "c", "flags", new Map(), globalTypeByName)};`;
    }).join(" ");
  };
  return constraints.map((constraint, index) => {
    if (!["sorted", "unique"].includes(constraint.kind)) {
      throw new Error(`${type.name}: unsupported vector constraint ${constraint.kind}`);
    }
    return `for(int i=1;i<${listExpression}.size();i++){${itemType} previous=${listExpression}.get(i-1),current=${listExpression}.get(i);Writer left${index}=new Writer(),right${index}=new Writer();${key(constraint, "previous", `left${index}`)}${key(constraint, "current", `right${index}`)}int compared${index}=compareUnsigned(left${index}.result(),right${index}.result());require(compared${index}${constraint.kind === "sorted" ? "<=0" : "!=0"},${javaString(type.name + " " + constraint.kind)});}`;
  }).join("");
}

let globalTypeByName;

function codecMethods(type, typeByName, flagBits) {
  const name = typeName(type.name);
  const op = primaryOperation(type);
  if (op.op === "integer") return integerMethods(type, op);
  if (op.op === "enum") return enumMethods(type, op);
  if (op.op === "length-prefixed") {
    const isText = op.content === "text";
    const lengthType = refType(op.lengthType);
    const absent = op.zeroLengthMeaning === "absent";
    const readValue = isText ? `strictText(r.bytes(length), ${javaString(type.name)})` : "r.bytes(length)";
    const bytes = isText ? "strictBytes(value.value())" : "value.value()";
    const maximum = Math.min(op.maximumBytes, 2147483647);
    const textValidation = type.operations.find((entry) => entry.op === "text-validation");
    if (isText && !textValidation) throw new Error(`${type.name}: missing text-validation operation`);
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { int length=length(decode${lengthType}(r,c,flags)); require(length>=${op.minimumBytes ?? 0}&&length<=${maximum},${javaString(type.name + " length")}); ${absent ? `if(length==0)return new ${name}(null);` : ""} return new ${name}(${readValue}); }\n`
      + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${isText ? "byte[] bytes=" + bytes + ";" : `byte[] bytes=${bytes};`} ${absent ? "if(bytes==null)bytes=new byte[0];" : `require(bytes!=null,${javaString(type.name)});`} require(bytes.length>=${op.minimumBytes ?? 0}&&bytes.length<=${maximum},${javaString(type.name + " length")}); encode${lengthType}(new ${lengthType}(bytes.length),w,c,flags); w.bytes(bytes); }`;
  }
  if (op.op === "struct" || op.op === "versioned-length-delimited") {
    const fields = op.fields;
    const delimited = op.op === "versioned-length-delimited";
    const decoded = decodeFields(fields, delimited ? "body" : "r", "c", "flags", typeByName, flagBits);
    const args = fields.map((f) => fieldName(f.name)).join(", ");
    const before = delimited
      ? `require(decode${refType(op.version)}(r,c,flags).value()==${longLiteral(op.version.constant)},${javaString(type.name + " version")}); Reader body=r.slice(length(decode${refType(op.length)}(r,c,flags)));`
      : "";
    const after = delimited ? `body.end(${javaString(type.name)});` : "";
    const encoded = encodeFields(fields, "value", delimited ? "body" : "w", "c", "flags", typeByName, flagBits);
    const encodeBefore = delimited
      ? `encode${refType(op.version)}(new ${refType(op.version)}(${integerArgument(op.version, longLiteral(op.version.constant), typeByName)}),w,c,flags); Writer body=new Writer();`
      : "";
    const encodeAfter = delimited
      ? `byte[] bytes=body.result(); encode${refType(op.length)}(new ${refType(op.length)}(bytes.length),w,c,flags); w.bytes(bytes);`
      : "";
    const decodedConstraints = structConstraints(type, op.constraints ?? []);
    const encodedConstraints = structConstraints(type, op.constraints ?? [], "value");
    const decodedPredicate = type.operations.some((entry) => entry.op === "runtime-predicate") ? predicateChecks(type, decoded.values) : "";
    const encodedValues = new Map(fields.map((field) => [field.name, `value.${fieldName(field.name)}()`]));
    const encodedPredicate = type.operations.some((entry) => entry.op === "runtime-predicate") ? predicateChecks(type, encodedValues) : "";
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${before}\n${decoded.lines.join("\n")}\n    ${after} ${decodedConstraints}${decodedPredicate} return new ${name}(${args}); }\n`
      + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException {${encodedConstraints}${encodedPredicate}${encodeBefore}\n${encoded.join("\n")}\n    ${encodeAfter} }`;
  }
  if (op.op === "vector" || op.op === "versioned-vector") {
    const repeat = op.op === "vector" ? { name: "items", item: op.item } : op.layout.find((entry) => entry.kind === "repeat");
    const list = fieldName(repeat.name), itemType = refType(repeat.item);
    const countRef = op.op === "vector" ? op.countType : op.layout.find((entry) => entry.counts);
    const prefix = op.op === "versioned-vector" ? `require(decode${refType(op.layout[0])}(r,c,flags).value()==${longLiteral(op.layout[0].constant)},${javaString(type.name + " version")});` : "";
    const encodePrefix = op.op === "versioned-vector" ? `encode${refType(op.layout[0])}(new ${refType(op.layout[0])}(${integerArgument(op.layout[0], longLiteral(op.layout[0].constant), typeByName)}),w,c,flags);` : "";
    const validation = vectorConstraintCode(type, op, repeat.item, "values");
    const encodeValidation = vectorConstraintCode(type, op, repeat.item, `value.${list}()`);
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${prefix} int count=length(decode${refType(countRef)}(r,c,flags)); ${op.maximumItems === undefined ? "" : `require(count<=${op.maximumItems},${javaString(type.name + " count")});`} List<${itemType}> values=new ArrayList<>(count); for(int i=0;i<count;i++)values.add(${decoderCall(repeat.item, "r", "c", "flags", new Map(), typeByName)}); ${validation} return new ${name}(values); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${encodePrefix} ${op.maximumItems === undefined ? "" : `require(value.${list}().size()<=${op.maximumItems},${javaString(type.name + " count")});`} ${encodeValidation} encode${refType(countRef)}(new ${refType(countRef)}(${integerArgument(countRef, `value.${list}().size()`, typeByName)}),w,c,flags); for(${itemType} item:value.${list}())${encoderCall(repeat.item, "item", "w", "c", "flags", new Map(), typeByName)}; }`;
  }
  if (op.op === "conditional-union") return unionMethods(type, op, typeByName, flagBits);
  if (op.op === "tlv32") return tlvMethods(type, op, typeByName, flagBits);
  throw new Error(`${type.name}: unsupported operation ${op.op}`);
}

function unionParameters(type) {
  const union = type.operations.find((entry) => entry.op === "conditional-union");
  if (!union) return [];
  return union.discriminators.filter((d) => d.source.kind !== "wire")
    .map((d, index) => `${refType(d)} external${index}`);
}

function unionMethods(type, union, typeByName, flagBits) {
  const name = typeName(type.name), external = unionParameters(type);
  const decodeParams = ["Reader r", "DecoderContext c", "int flags", ...external].join(", ");
  const encodeParams = [`${name} value`, "Writer w", "DecoderContext c", "int flags", ...external].join(", ");
  const discriminatorValues = new Map();
  const decodeLines = [];
  let externalIndex = 0;
  for (const d of union.discriminators) {
    const variable = fieldName(d.name);
    if (d.source.kind === "wire") decodeLines.push(`    ${refType(d)} ${variable}=${decoderCall(d, "r", "c", "flags", discriminatorValues, typeByName)};`);
    else decodeLines.push(`    ${refType(d)} ${variable}=external${externalIndex++};`);
    discriminatorValues.set(d.name, variable);
  }
  const bodyPrefix = union.bodyLengthType ? `Reader selected=r.slice(length(decode${refType(union.bodyLengthType)}(r,c,flags)));` : "Reader selected=r;";
  decodeLines.push(`    ${bodyPrefix}`);
  const cases = caseInfo(type, union);
  cases.forEach((entry, index) => {
    const test = entry.discriminator === null ? "true" : Object.entries(entry.discriminator).map(([key, expected]) => {
      const variable = discriminatorValues.get(key);
      const d = union.discriminators.find((item) => item.name === key);
      const target = typeByName.get(referenceOf(d).$ref);
      return primaryOperation(target).op === "enum" ? `${variable}==${refType(d)}.${enumName(expected)}` : `${variable}.value()==${longLiteral(expected)}`;
    }).join(" && ");
    const decoded = decodeFields(entry.selected.fields, "selected", "c", "flags", typeByName, flagBits, "      ");
    const wireArgs = union.discriminators.filter((d) => d.source.kind === "wire").map((d) => discriminatorValues.get(d.name));
    const args = [...wireArgs, ...entry.selected.fields.map((f) => fieldName(f.name))].join(", ");
    const predicate = decoded.values.has("terminalResult") && decoded.values.has("failureCode") ? predicateChecks(type, decoded.values) : "";
    decodeLines.push(`    ${index === 0 ? "if" : "else if"}(${test}) {\n${decoded.lines.join("\n")}\n      ${union.bodyLengthType ? `selected.end(${javaString(type.name)});` : ""}${predicate} return new ${entry.name}(${args});\n    }`);
  });
  decodeLines.push(`    throw error(${javaString(type.name + " discriminator")});`);

  const encodeLines = [];
  for (const entry of cases) {
    encodeLines.push(`    if(value instanceof ${entry.name} item){`);
    const wire = union.discriminators.filter((d) => d.source.kind === "wire");
    for (const d of wire) encodeLines.push(`      ${encoderCall(d, `item.${fieldName(d.name)}()`, "w", "c", "flags", new Map(), typeByName)};`);
    if (union.bodyLengthType) encodeLines.push("      Writer selected=new Writer();");
    encodeLines.push(...encodeFields(entry.selected.fields, "item", union.bodyLengthType ? "selected" : "w", "c", "flags", typeByName, flagBits, "      "));
    const encodedValues = new Map(entry.selected.fields.map((field) => [field.name, `item.${fieldName(field.name)}()`]));
    if (encodedValues.has("terminalResult") && encodedValues.has("failureCode")) encodeLines.push(`      ${predicateChecks(type, encodedValues)}`);
    if (union.bodyLengthType) encodeLines.push(`      byte[] bytes=selected.result(); encode${refType(union.bodyLengthType)}(new ${refType(union.bodyLengthType)}(bytes.length),w,c,flags); w.bytes(bytes);`);
    encodeLines.push("      return;\n    }");
  }
  encodeLines.push(`    throw error(${javaString(type.name + " case")});`);
  return `  private static ${name} decode${name}(${decodeParams}) throws IOException {\n${decodeLines.join("\n")}\n  }\n  private static void encode${name}(${encodeParams}) throws IOException {\n${encodeLines.join("\n")}\n  }`;
}

function tlvMethods(type, op, typeByName, flagBits) {
  const name = typeName(type.name);
  const vars = op.fields.map((f) => `    ${refType(f)} ${fieldName(f.name)}=null;`).join("\n");
  const cases = op.fields.map((f) => `case ${f.id} -> ${fieldName(f.name)}=${decoderCall(f, "item", "c", "flags", new Map(), typeByName)};`).join(" ");
  const required = op.requiredFields.map((field) => `require(${fieldName(field)}!=null,${javaString(field + " required")});`).join(" ");
  const args = op.fields.map((f) => fieldName(f.name)).join(", ");
  const enc = op.fields.map((f) => `if(value.${fieldName(f.name)}()!=null){Writer item=new Writer(); ${encoderCall(f, `value.${fieldName(f.name)}()`, "item", "c", "flags", new Map(), typeByName)}; byte[] bytes=item.result(); encode${refType(op.fieldIdType)}(new ${refType(op.fieldIdType)}(${f.id}),body,c,flags); encode${refType(op.fieldLengthType)}(new ${refType(op.fieldLengthType)}(bytes.length),body,c,flags); body.bytes(bytes);}`).join("\n    ");
  const fieldConstraints = op.fields.flatMap((field) => (field.constraints ?? []).map((constraint) => {
    if (constraint.kind !== "contains-protocol-required-capability") throw new Error(`${type.name}.${field.name}: unsupported field constraint ${constraint.kind}`);
    return `require(${fieldName(field.name)}!=null&&${fieldName(field.name)}.items().stream().anyMatch(item->item.value().equals(ServiceWireConstants.REQUIRED_CAPABILITY)),${javaString(field.name + " capability")});`;
  })).join(" ");
  const values = new Map(op.fields.map((field) => [field.name, fieldName(field.name)]));
  const presence = op.presenceRules.map((rule) => {
    const checks = [...(rule.require ?? []).map((field) => `${fieldName(field)}!=null`), ...(rule.forbid ?? []).map((field) => `${fieldName(field)}==null`)];
    return `if(${conditionExpression(rule.when, values, "flags", flagBits)})require(${checks.join("&&")},${javaString(type.name + " presence")});`;
  }).join(" ");
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { Reader body=r.slice(length(decode${refType(op.totalLengthType)}(r,c,flags))); ${vars}\n    int previous=0; while(!body.done()){int id=length(decode${refType(op.fieldIdType)}(body,c,flags));require(id>previous,${javaString(type.name + " order")});previous=id;Reader item=body.slice(length(decode${refType(op.fieldLengthType)}(body,c,flags)));switch(id){${cases} default -> item.skipRemaining(); }item.end(${javaString(type.name + " field")});} ${required} ${presence} ${fieldConstraints} return new ${name}(${args}); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { Writer body=new Writer(); ${enc} byte[] bytes=body.result(); encode${refType(op.totalLengthType)}(new ${refType(op.totalLengthType)}(bytes.length),w,c,flags);w.bytes(bytes); }`;
}

function publicMethods(ir) {
  return ir.types.map((type) => {
    const name = typeName(type.name), external = unionParameters(type);
    const params = external.length ? `, ${external.join(", ")}` : "";
    const suffix = external.length ? `, ${external.map((_, i) => `external${i}`).join(", ")}` : "";
    return `  static ${name} decode${name}(byte[] bytes,DecoderContext context${params})throws IOException{Reader r=new Reader(bytes);${name} value=decode${name}(r,context,0${suffix});r.end(${javaString(type.name)});return value;}\n  static byte[] encode${name}(${name} value,DecoderContext context${params})throws IOException{Writer w=new Writer();encode${name}(value,w,context,0${suffix});return w.result();}`;
  }).join("\n");
}

function commandFields(command) { return command.operations.filter((entry) => entry.op === "field"); }

function commandDeclarations(ir) {
  const records = ir.commands.map((command) => {
    const fields = commandFields(command).map((field) => `${refType(field)} ${fieldName(field.name)}`);
    fields.unshift("int flags");
    const metadata = command.operations.find((entry) => entry.op === "metadata-flag-frame");
    const payload = operation(command, "payload");
    if (metadata) fields.push(`${refType(metadata.frame)} metadata`);
    if (payload.type) fields.push(`${refType(payload.type)} payload`);
    return `  record ${typeName(command.name)}Command(${fields.join(", ")}) implements ServiceWireCommand { public int id(){return ${operation(command, "command-header").commandId};} }`;
  });
  return `  sealed interface ServiceWireCommand permits ${ir.commands.map((c) => `${typeName(c.name)}Command`).join(", ")} {int id();int flags();}\n${records.join("\n")}`;
}

function flagValidation(command, flags, flagBits) {
  const op = operation(command, "flags");
  const allowed = op.allowed.reduce((value, flag) => value | flagBits.get(flag.name), 0);
  const required = op.required.reduce((value, flag) => value | flagBits.get(flag.name), 0);
  const rules = command.operations.filter((entry) => entry.op === "flag-constraint").map((entry) => {
    if (entry.kind === "all-or-none") {
      const mask = entry.flags.reduce((value, flag) => value | flagBits.get(flag.name), 0);
      return `require(((${flags}&${mask})==0)||((${flags}&${mask})==${mask}),${javaString(command.name + " flag constraint")});`;
    }
    if (entry.kind === "implies") {
      const source = flagBits.get(entry.if.name);
      const mask = entry.then.reduce((value, flag) => value | flagBits.get(flag.name), 0);
      return `require(((${flags}&${source})==0)||((${flags}&${mask})==${mask}),${javaString(command.name + " flag constraint")});`;
    }
    throw new Error(`${command.name}: unsupported flag constraint ${entry.kind}`);
  }).join("");
  return `require((${flags}&~${allowed})==0&&(${flags}&${required})==${required},${javaString(command.name + " flags")});${rules}`;
}

function predicateChecks(owner, values) {
  return owner.operations.filter((entry) => entry.op === "runtime-predicate").map((entry) => {
    if (entry.reference.asset !== "service-wire-constants" || entry.reference.name !== "valid-terminal-failure") throw new Error(`${owner.name}: unsupported runtime predicate`);
    return `require(ServiceWireConstants.validTerminalFailure(${values.get("terminalResult")}.wire,${values.get("failureCode")}.wire),${javaString(owner.name + " predicate")});`;
  }).join("");
}

function commandMethods(ir, typeByName, flagBits) {
  const decodeMethods = ir.commands.map((command) => {
    const fields = commandFields(command), decoded = decodeFields(fields, "r", "c", "flags", typeByName, flagBits, "        ");
    const metadata = command.operations.find((entry) => entry.op === "metadata-flag-frame"), payload = operation(command, "payload");
    const bit = metadata ? flagBits.get(metadata.flag.name) : 0;
    const metadataIndex = metadata ? `int next=1;${refType(metadata.frame)} metadata=null;if((flags&${bit})!=0){require(frames.size()>next,${javaString(command.name + " metadata")});metadata=decode${refType(metadata.frame)}(frames.get(next++),c);}` : "int next=1;";
    const payloadValue = payload.type ? `${refType(payload.type)} payload=null;if(frames.size()>next)payload=decode${refType(payload.type)}(frames.get(next++),c);` : "";
    const policy = payload.policy === "required" ? `require(payload!=null,${javaString(command.name + " payload")});` : payload.policy === "forbidden" ? `require(frames.size()==next,${javaString(command.name + " payload")});` : "";
    const args = ["flags", ...fields.map((field) => fieldName(field.name)), ...(metadata ? ["metadata"] : []), ...(payload.type ? ["payload"] : [])].join(", ");
    const name = `${typeName(command.name)}Command`;
    return `  private static ${name} decode${name}(List<byte[]> frames,DecoderContext c,Reader r,int flags)throws IOException{${flagValidation(command, "flags", flagBits)}${metadataIndex}${payloadValue}${policy}require(frames.size()==next,${javaString(command.name + " frames")});\n${decoded.lines.join("\n")}\n${predicateChecks(command, decoded.values)}r.end(${javaString(command.name)});return new ${name}(${args});}`;
  }).join("\n");
  const decodeCases = ir.commands.map((command) => `case ${operation(command, "command-header").commandId}->decode${typeName(command.name)}Command(frames,c,r,flags);`).join("");
  const encodeMethods = ir.commands.map((command) => {
    const name = `${typeName(command.name)}Command`, fields = commandFields(command);
    const encoded = encodeFields(fields, "item", "w", "c", "item.flags()", typeByName, flagBits, "        ");
    const metadata = command.operations.find((entry) => entry.op === "metadata-flag-frame"), payload = operation(command, "payload");
    const bit = metadata ? flagBits.get(metadata.flag.name) : 0;
    const metadataFrame = metadata ? `if((item.flags()&${bit})!=0){require(item.metadata()!=null,${javaString(command.name + " metadata")});frames.add(encode${refType(metadata.frame)}(item.metadata(),c));}else require(item.metadata()==null,${javaString(command.name + " metadata")});` : "";
    const payloadFrame = payload.type ? `${payload.policy === "required" ? `require(item.payload()!=null,${javaString(command.name + " payload")});` : ""}if(item.payload()!=null)frames.add(encode${refType(payload.type)}(item.payload(),c));` : "";
    const values = new Map(fields.map((field) => [field.name, `item.${fieldName(field.name)}()`]));
    return `  private static List<byte[]> encode${name}(${name} item,DecoderContext c,Writer w)throws IOException{${flagValidation(command, "item.flags()", flagBits)}${predicateChecks(command, values)}\n${encoded.join("\n")}\nList<byte[]> frames=new ArrayList<>();frames.add(w.result());${metadataFrame}${payloadFrame}return frames;}`;
  }).join("\n");
  const encodeCases = ir.commands.map((command) => { const name = `${typeName(command.name)}Command`; return `case ${operation(command, "command-header").commandId}->encode${name}((${name})value,c,w);`; }).join("");
  const validators = ir.commands.filter((command) => command.operations.some((entry) => entry.op === "runtime-predicate")).map((command) => {
    const fields = commandFields(command), terminal = fields.find((field) => field.name === "terminalResult"), failure = fields.find((field) => field.name === "failureCode");
    if (!terminal || !failure) throw new Error(`${command.name}: runtime predicate operands missing`);
    return `  static void validate${typeName(command.name)}Predicate(${refType(terminal)} terminalResult,${refType(failure)} failureCode)throws IOException{${predicateChecks(command,new Map([["terminalResult","terminalResult"],["failureCode","failureCode"]]))}}`;
  }).join("\n");
  const header = operation(ir.commands[0], "command-header");
  return `${decodeMethods}\n${encodeMethods}\n  static ServiceWireCommand decodeCommand(List<byte[]> frames,DecoderContext c)throws IOException{require(!frames.isEmpty(),"command frame");Reader r=new Reader(frames.get(0));require(r.u8()==${header.magic[0]}&&r.u8()==${header.magic[1]}&&r.u8()==${header.wireMajor},"header");int id=r.u8(),flags=r.u8();return switch(id){${decodeCases}default->throw error("command id");};}\n  static List<byte[]> encodeCommandFrames(ServiceWireCommand value,DecoderContext c)throws IOException{Writer w=new Writer();w.u8(${header.magic[0]});w.u8(${header.magic[1]});w.u8(${header.wireMajor});w.u8(value.id());w.u8(value.flags());return switch(value.id()){${encodeCases}default->throw error("command id");};}\n  static byte[] encodeCommand(ServiceWireCommand value,DecoderContext c)throws IOException{return encodeCommandFrames(value,c).get(0);}\n${validators}`;
}

function durableMethods(ir, typeByName) {
  const decode = ir.durableFormats.map((format) => `case ${javaString(format.name)} -> decodeDurable${typeName(format.name)}(bytes,c)`).join("; ");
  const encode = ir.durableFormats.map((format) => { const header = operation(format, "durable-header"); return `case ${javaString(format.name)} -> encodeDurable${typeName(format.name)}((${refType(header.body)})value,c)`; }).join("; ");
  const methods = ir.durableFormats.map((format) => {
    const header = operation(format, "durable-header"), checksum = operation(format, "checksum"), limit = operation(format, "encoded-limit");
    if (checksum.algorithm !== "crc32c-castagnoli" || checksum.position !== "trailing") throw new Error(`${format.name}: unsupported checksum operation`);
    const body = refType(header.body), name = typeName(format.name), flagsType = refType(header.flagsType);
    return `  private static ${body} decodeDurable${name}(byte[] bytes,DecoderContext c)throws IOException{require(bytes.length<=${limit.maximumEncodedBytes},${javaString(format.name + " encoded limit")});Reader r=new Reader(bytes);${header.magic.map((b) => `require(r.u8()==${b},"magic");`).join("")}require(r.u8()==${header.formatVersion},"version");${flagsType} headerFlags=decode${flagsType}(r,c,0);require(headerFlags.value()==${longLiteral(header.flags)},"flags");Reader bodyReader=r.slice(length(decode${refType(header.bodyLengthType)}(r,c,0)));${body} value=decode${body}(bodyReader,c,0);bodyReader.end(${javaString(format.name)});int checksum=(int)r.uint(4);r.end(${javaString(format.name)});CRC32C crc=new CRC32C();crc.update(bytes,0,bytes.length-4);require((int)crc.getValue()==checksum,"checksum");return value;}\n  private static byte[] encodeDurable${name}(${body} value,DecoderContext c)throws IOException{Writer bodyWriter=new Writer();encode${body}(value,bodyWriter,c,0);byte[] body=bodyWriter.result();Writer w=new Writer();${header.magic.map((b) => `w.u8(${b});`).join("")}w.u8(${header.formatVersion});encode${flagsType}(new ${flagsType}(${integerArgument(header.flagsType, longLiteral(header.flags), typeByName)}),w,c,0);encode${refType(header.bodyLengthType)}(new ${refType(header.bodyLengthType)}(${integerArgument(header.bodyLengthType, "body.length", typeByName)}),w,c,0);w.bytes(body);CRC32C crc=new CRC32C();byte[] covered=w.result();crc.update(covered,0,covered.length);w.uint(4,crc.getValue());byte[] result=w.result();require(result.length<=${limit.maximumEncodedBytes},${javaString(format.name + " encoded limit")});return result;}`;
  }).join("\n");
  return `${methods}\n  static Object decodeDurable(String name,byte[] bytes,DecoderContext c)throws IOException{return switch(name){${decode};default->throw error("durable format");};}\n  static byte[] encodeDurable(String name,Object value,DecoderContext c)throws IOException{return switch(name){${encode};default->throw error("durable format");};}`;
}

function logicalMethods(ir) {
  const format = ir.relocationLogicalStreamFormat, stream = operation(format, "logical-stream"), limit = operation(format, "encoded-limit"), body = refType(stream.body), name = typeName(format.name);
  return `  static ${body} decodeLogical${name}(byte[] bytes,DecoderContext c)throws IOException{require((long)bytes.length<=${longLiteral(limit.maximumEncodedBytes)},${javaString(format.name + " encoded limit")});return decode${body}(bytes,c);}\n  static byte[] encodeLogical${name}(${body} value,DecoderContext c)throws IOException{byte[] bytes=encode${body}(value,c);require((long)bytes.length<=${longLiteral(limit.maximumEncodedBytes)},${javaString(format.name + " encoded limit")});return bytes;}`;
}

function render(ir) {
  const typeByName = new Map(ir.types.map((type) => [type.name, type]));
  globalTypeByName = typeByName;
  assertOperationVocabulary(ir);
  const flagBits = new Map(ir.flags.map((flag) => [flag.name, flag.bit]));
  const context = ir.semanticContexts.map((entry) => {
    const type = entry.valueType ? refType(entry.valueType) : "Boolean";
    return `${type} ${fieldName(entry.name)}`;
  }).join(", ");
  const declarations = ir.types.map(declaration).join("\n");
  const codecs = ir.types.map((type) => codecMethods(type, typeByName, flagBits)).join("\n");
  return `// <auto-generated> DO NOT EDIT. Generated solely from validated service-wire operation IR.\npackage systems.zlink.framework.runtime.internal.service;\n\nimport java.io.*;\nimport java.math.BigInteger;\nimport java.nio.*;\nimport java.nio.charset.*;\nimport java.util.*;\nimport java.util.zip.CRC32C;\nimport systems.zlink.framework.runtime.protocol.ServiceWireConstants;\n\nfinal class ServiceWireCodec {\n  private ServiceWireCodec(){}\n  record DecoderContext(${context}) {}\n${declarations}\n${commandDeclarations(ir)}\n${runtimeHelpers()}\n${codecs}\n${publicMethods(ir)}\n${commandMethods(ir, typeByName, flagBits)}\n${durableMethods(ir, typeByName)}\n${logicalMethods(ir)}\n}\n`;
}

function assertOperationVocabulary(ir) {
  const implemented = new Set([
    "integer", "enum", "length-prefixed", "text-validation", "field", "struct", "vector",
    "versioned-vector", "bounded-reader", "versioned-length-delimited", "discriminator",
    "conditional-union", "tlv32", "constraint", "command-header", "flags",
    "flag-constraint", "metadata-flag-frame", "payload", "durable-header", "checksum",
    "encoded-limit", "logical-stream", "runtime-predicate",
  ]);
  for (const name of ir.operationVocabulary) if (!implemented.delete(name)) throw new Error(`unsupported operation ${name}`);
  if (implemented.size !== 0) throw new Error(`operation vocabulary missing ${[...implemented].join(",")}`);
}

function runtimeHelpers() {
  return String.raw`  private static final class Reader { final byte[] bytes; int at; Reader(byte[] bytes){this.bytes=bytes;} int u8()throws IOException{return(int)uint(1);} long uint(int n)throws IOException{need(n);long v=0;for(int i=0;i<n;i++)v=(v<<8)|Byte.toUnsignedLong(bytes[at++]);return v;} long i64()throws IOException{return uint(8);} byte[] bytes(int n)throws IOException{need(n);return Arrays.copyOfRange(bytes,at,at+=n);} Reader slice(int n)throws IOException{return new Reader(bytes(n));} void skipRemaining(){at=bytes.length;} boolean done(){return at==bytes.length;} void end(String name)throws IOException{require(done(),name+" trailing");} void need(int n)throws IOException{if(n<0||at+n>bytes.length)throw new EOFException("truncated field");} }
  private static final class Writer { final ByteArrayOutputStream out=new ByteArrayOutputStream(); void u8(int v){out.write(v);} void uint(int n,long v){for(int i=n-1;i>=0;i--)out.write((int)(v>>>(i*8))&255);} void bytes(byte[] v){out.writeBytes(v);} byte[] result(){return out.toByteArray();} }
  private static IOException error(String value){return new IOException(value);} private static void require(boolean ok,String message)throws IOException{if(!ok)throw error(message);}
  private static void requireUnsigned(long value,int width,String minimum,String maximum,String name)throws IOException{BigInteger actual=new BigInteger(width==8?Long.toUnsignedString(value):Long.toString(Integer.toUnsignedLong((int)value)));if(minimum!=null&&actual.compareTo(new BigInteger(minimum))<0||maximum!=null&&actual.compareTo(new BigInteger(maximum))>0)throw error(name+" range");}
  private static void requireSigned(long value,String minimum,String maximum,String name)throws IOException{if(minimum!=null&&value<Long.parseLong(minimum)||maximum!=null&&value>Long.parseLong(maximum))throw error(name+" range");}
  private static int length(Object value)throws IOException{try{return switch(value){case U8 x->x.value();case U16 x->x.value();case U32 x->x.value();case U64 x->Math.toIntExact(x.value());default->throw error("length type");};}catch(ArithmeticException e){throw error("length range");}}
  private static byte[] strictBytes(String value)throws IOException{if(value==null)return null;byte[] bytes=value.getBytes(StandardCharsets.UTF_8);for(byte b:bytes)if(b==0)throw error("NUL text");return bytes;}
  private static String strictText(byte[] value,String name)throws IOException{for(byte b:value)if(b==0)throw error(name+" NUL");try{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();}catch(CharacterCodingException e){throw error(name+" UTF-8");}}
  private static int compareUnsigned(byte[] left,byte[] right){int count=Math.min(left.length,right.length);for(int i=0;i<count;i++){int compared=Integer.compare(Byte.toUnsignedInt(left[i]),Byte.toUnsignedInt(right[i]));if(compared!=0)return compared;}return Integer.compare(left.length,right.length);}
`;
}

const [mode, schema, outputArgument, ...rest] = process.argv.slice(2);
if (!schema || !outputArgument || rest.length || !["--write", "--check"].includes(mode)) {
  console.error("usage: node render-service-wire-java.mjs --write|--check <schema> <output>");
  process.exit(2);
}
const output = path.resolve(outputArgument);
try {
  const source = render(lowerSchema(path.resolve(schema))).replace(/[ \t]+$/gm, "");
  if (mode === "--write") {
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, source);
  } else if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== source) {
    throw new Error(`generated Java service-wire codec drift: ${path.relative(process.cwd(), output)}`);
  }
} catch (error) {
  console.error(error.stack ?? error.message ?? String(error));
  process.exit(1);
}

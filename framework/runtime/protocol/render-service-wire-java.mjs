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

function refType(reference) { return typeName(reference.$ref); }
function integerPrimitive(encoding) { return encoding === "u64" || encoding === "i64" ? "long" : "int"; }
function integerWidth(encoding) { return { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[encoding]; }
function javaString(value) { return JSON.stringify(String(value)); }
function longLiteral(value) {
  const text = String(value);
  return BigInt(text) > 9223372036854775807n ? `Long.parseUnsignedLong(${javaString(text)})` : `${text}L`;
}

function integerArgument(reference, expression, typeByName) {
  const type = typeByName.get(reference.$ref);
  return integerPrimitive(type.encoding) === "int" ? `(int)(${expression})` : expression;
}

function caseInfo(type) {
  const used = new Map();
  const cases = Object.entries(type.cases).map(([signature, selected], index) => {
    const discriminator = JSON.parse(signature);
    let suffix = Object.values(discriminator).map(typeName).join("") || `Case${index}`;
    const count = used.get(suffix) ?? 0;
    used.set(suffix, count + 1);
    if (count > 0) suffix += count + 1;
    return { signature, discriminator, selected, name: `${typeName(type.name)}${suffix}` };
  });
  if (type.otherwise.kind === "fields") {
    cases.push({ signature: null, discriminator: null, selected: type.otherwise, name: `${typeName(type.name)}Otherwise` });
  }
  return cases;
}

function recordComponents(fields) {
  return fields.map((field) => `${refType(field)} ${fieldName(field.name)}`).join(", ");
}

function declaration(type) {
  const name = typeName(type.name);
  switch (type.kind) {
    case "integer": return `  public record ${name}(${integerPrimitive(type.encoding)} value) {}`;
    case "enum": return `  public enum ${name} { ${type.values.map((v) => `${enumName(v.name)}(${longLiteral(v.value)})`).join(", ")}; final long wire; ${name}(long wire){this.wire=wire;} }`;
    case "length-prefixed-bytes": return `  public record ${name}(byte[] value) { public ${name}{value=value==null?null:value.clone();} public byte[] value(){return value==null?null:value.clone();} }`;
    case "length-prefixed-text": return `  public record ${name}(String value) {}`;
    case "struct": return `  public record ${name}(${recordComponents(type.fields)}) {}`;
    case "vector": return `  public record ${name}(List<${refType(type.item)}> items) { public ${name}{items=List.copyOf(items);} }`;
    case "versioned-vector": {
      const repeat = type.layout.find((entry) => entry.kind === "repeat");
      return `  public record ${name}(List<${refType(repeat.item)}> ${fieldName(repeat.name)}) { public ${name}{${fieldName(repeat.name)}=List.copyOf(${fieldName(repeat.name)});} }`;
    }
    case "versioned-length-delimited": return `  public record ${name}(${recordComponents(type.body)}) {}`;
    case "conditional-union": {
      const variants = caseInfo(type);
      const records = variants.map((entry) => {
        const wire = entry.discriminator === null ? [] : type.discriminators.filter((d) => d.source.kind === "wire");
        const fields = [...wire, ...entry.selected.fields];
        return `  public record ${entry.name}(${recordComponents(fields)}) implements ${name} {}`;
      }).join("\n");
      return `  public sealed interface ${name} permits ${variants.map((v) => v.name).join(", ")} {}\n${records}`;
    }
    case "tlv32": return `  public record ${name}(${recordComponents(type.fields)}) {}`;
    default: throw new Error(`unsupported IR kind ${type.kind}`);
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
    const mask = atom.operands.reduce((result, operand) => result | flagBits.get(operand.name), 0);
    return atom.kind === "allFlagsSet" ? `((${flags} & ${mask}) == ${mask})` : `((${flags} & ${mask}) != 0)`;
  }).join(" && ");
}

function decoderCall(reference, reader, context, flags, values, typeByName) {
  const target = typeByName.get(reference.$ref);
  const argumentsList = [reader, context, flags];
  if (target.kind === "conditional-union") {
    for (const discriminator of target.discriminators.filter((d) => d.source.kind !== "wire")) {
      argumentsList.push(discriminator.source.kind === "enclosingField"
        ? values.get(discriminator.source.name)
        : `${context}.${fieldName(discriminator.source.name)}()`);
    }
  }
  return `decode${typeName(reference.$ref)}(${argumentsList.join(", ")})`;
}

function encoderCall(reference, value, writer, context, flags, values, typeByName) {
  const target = typeByName.get(reference.$ref);
  const argumentsList = [value, writer, context, flags];
  if (target.kind === "conditional-union") {
    for (const discriminator of target.discriminators.filter((d) => d.source.kind !== "wire")) {
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
    const target = typeByName.get(field.$ref);
    if (field.constant !== undefined) {
      const expected = target.kind === "enum"
        ? `${refType(field)}.${enumName(field.constant)}`
        : longLiteral(field.constant);
      const actual = target.kind === "enum" ? variable : `${variable}.value()`;
      lines.push(`${indent}require(${actual} == ${expected}, ${javaString(field.name + " constant")});`);
    }
    if (field.minimum !== undefined && target.kind === "integer") lines.push(`${indent}requireUnsigned(${variable}.value(), ${integerWidth(target.encoding)}, ${javaString(field.minimum)}, null, ${javaString(field.name)});`);
    if (field.maximum !== undefined && target.kind === "integer") lines.push(`${indent}requireUnsigned(${variable}.value(), ${integerWidth(target.encoding)}, null, ${javaString(field.maximum)}, ${javaString(field.name)});`);
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

function structConstraints(type, owner = null) {
  return (type.constraints ?? []).filter((constraint) => constraint.kind === "not-both-zero")
    .map((constraint) => {
      const values = constraint.fields.map((field) => {
        const value = owner
          ? `${owner}.${fieldName(field.name)}()`
          : fieldName(field.name);
        return `${value}.value()!=0`;
      });
      return `require(${values.join("||")},${javaString(type.name + " constraint")});`;
    }).join(" ");
}

function integerMethods(type) {
  const name = typeName(type.name), primitive = integerPrimitive(type.encoding), width = integerWidth(type.encoding);
  const signed = type.encoding === "i64";
  const widthArgument = signed ? "" : `, ${width}`;
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${primitive} value=${width === 8 ? "r.i64()" : `(int)r.uint(${width})`}; ${signed ? "requireSigned" : "requireUnsigned"}(value${widthArgument}, ${type.minimum === undefined ? "null" : javaString(type.minimum)}, ${type.maximum === undefined ? "null" : javaString(type.maximum)}, ${javaString(type.name)}); return new ${name}(value); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${signed ? "requireSigned" : "requireUnsigned"}(value.value()${widthArgument}, ${type.minimum === undefined ? "null" : javaString(type.minimum)}, ${type.maximum === undefined ? "null" : javaString(type.maximum)}, ${javaString(type.name)}); w.uint(${width}, value.value()); }`;
}

function enumMethods(type) {
  const name = typeName(type.name), width = integerWidth(type.encoding);
  const decodeCases = type.values.map((v) => `if(wire==${longLiteral(v.value)})return ${name}.${enumName(v.name)};`).join("");
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { long wire=r.uint(${width});${decodeCases}throw error(${javaString(type.name + " enum")}); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags){w.uint(${width},value.wire);}`;
}

function codecMethods(type, typeByName, flagBits) {
  const name = typeName(type.name);
  if (type.kind === "integer") return integerMethods(type);
  if (type.kind === "enum") return enumMethods(type);
  if (type.kind === "length-prefixed-bytes" || type.kind === "length-prefixed-text") {
    const isText = type.kind === "length-prefixed-text";
    const lengthType = refType(type.lengthType);
    const absent = type.zeroLengthMeaning === "absent";
    const readValue = isText ? `strictText(r.bytes(length), ${javaString(type.name)})` : "r.bytes(length)";
    const bytes = isText ? "strictBytes(value.value())" : "value.value()";
    const maximum = Math.min(type.maximumBytes, 2147483647);
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { int length=length(decode${lengthType}(r,c,flags)); require(length>=${type.minimumBytes ?? 0}&&length<=${maximum},${javaString(type.name + " length")}); ${absent ? `if(length==0)return new ${name}(null);` : ""} return new ${name}(${readValue}); }\n`
      + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${isText ? "byte[] bytes=" + bytes + ";" : `byte[] bytes=${bytes};`} ${absent ? "if(bytes==null)bytes=new byte[0];" : `require(bytes!=null,${javaString(type.name)});`} require(bytes.length>=${type.minimumBytes ?? 0}&&bytes.length<=${maximum},${javaString(type.name + " length")}); encode${lengthType}(new ${lengthType}(bytes.length),w,c,flags); w.bytes(bytes); }`;
  }
  if (type.kind === "struct" || type.kind === "versioned-length-delimited") {
    const fields = type.kind === "struct" ? type.fields : type.body;
    const decoded = decodeFields(fields, type.kind === "struct" ? "r" : "body", "c", "flags", typeByName, flagBits);
    const args = fields.map((f) => fieldName(f.name)).join(", ");
    const before = type.kind === "versioned-length-delimited"
      ? `require(decode${refType(type.version)}(r,c,flags).value()==${longLiteral(type.version.constant)},${javaString(type.name + " version")}); Reader body=r.slice(length(decode${refType(type.length)}(r,c,flags)));`
      : "";
    const after = type.kind === "versioned-length-delimited" ? `body.end(${javaString(type.name)});` : "";
    const encoded = encodeFields(fields, "value", type.kind === "struct" ? "w" : "body", "c", "flags", typeByName, flagBits);
    const encodeBefore = type.kind === "versioned-length-delimited"
      ? `encode${refType(type.version)}(new ${refType(type.version)}(${integerArgument(type.version, longLiteral(type.version.constant), typeByName)}),w,c,flags); Writer body=new Writer();`
      : "";
    const encodeAfter = type.kind === "versioned-length-delimited"
      ? `byte[] bytes=body.result(); encode${refType(type.length)}(new ${refType(type.length)}(bytes.length),w,c,flags); w.bytes(bytes);`
      : "";
    const decodedConstraints = structConstraints(type);
    const encodedConstraints = structConstraints(type, "value");
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${before}\n${decoded.lines.join("\n")}\n    ${after} ${decodedConstraints} return new ${name}(${args}); }\n`
      + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${encodedConstraints} ${encodeBefore}\n${encoded.join("\n")}\n    ${encodeAfter} }`;
  }
  if (type.kind === "vector" || type.kind === "versioned-vector") {
    const repeat = type.kind === "vector" ? { name: "items", item: type.item } : type.layout.find((entry) => entry.kind === "repeat");
    const list = fieldName(repeat.name), itemType = refType(repeat.item);
    const countRef = type.kind === "vector" ? type.countType : type.layout.find((entry) => entry.counts);
    const prefix = type.kind === "versioned-vector" ? `require(decode${refType(type.layout[0])}(r,c,flags).value()==${longLiteral(type.layout[0].constant)},${javaString(type.name + " version")});` : "";
    const encodePrefix = type.kind === "versioned-vector" ? `encode${refType(type.layout[0])}(new ${refType(type.layout[0])}(${integerArgument(type.layout[0], longLiteral(type.layout[0].constant), typeByName)}),w,c,flags);` : "";
    return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { ${prefix} int count=length(decode${refType(countRef)}(r,c,flags)); ${type.maximumItems === undefined ? "" : `require(count<=${type.maximumItems},${javaString(type.name + " count")});`} List<${itemType}> values=new ArrayList<>(count); for(int i=0;i<count;i++)values.add(${decoderCall(repeat.item, "r", "c", "flags", new Map(), typeByName)}); return new ${name}(values); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { ${encodePrefix} ${type.maximumItems === undefined ? "" : `require(value.${list}().size()<=${type.maximumItems},${javaString(type.name + " count")});`} encode${refType(countRef)}(new ${refType(countRef)}(${integerArgument(countRef, `value.${list}().size()`, typeByName)}),w,c,flags); for(${itemType} item:value.${list}())${encoderCall(repeat.item, "item", "w", "c", "flags", new Map(), typeByName)}; }`;
  }
  if (type.kind === "conditional-union") return unionMethods(type, typeByName, flagBits);
  if (type.kind === "tlv32") return tlvMethods(type, typeByName, flagBits);
  throw new Error(`unsupported ${type.kind}`);
}

function unionParameters(type) {
  if (type.kind !== "conditional-union") return [];
  return type.discriminators.filter((d) => d.source.kind !== "wire")
    .map((d, index) => `${refType(d)} external${index}`);
}

function unionMethods(type, typeByName, flagBits) {
  const name = typeName(type.name), external = unionParameters(type);
  const decodeParams = ["Reader r", "DecoderContext c", "int flags", ...external].join(", ");
  const encodeParams = [`${name} value`, "Writer w", "DecoderContext c", "int flags", ...external].join(", ");
  const discriminatorValues = new Map();
  const decodeLines = [];
  let externalIndex = 0;
  for (const d of type.discriminators) {
    const variable = fieldName(d.name);
    if (d.source.kind === "wire") decodeLines.push(`    ${refType(d)} ${variable}=${decoderCall(d, "r", "c", "flags", discriminatorValues, typeByName)};`);
    else decodeLines.push(`    ${refType(d)} ${variable}=external${externalIndex++};`);
    discriminatorValues.set(d.name, variable);
  }
  const bodyPrefix = type.bodyLengthType ? `Reader selected=r.slice(length(decode${refType(type.bodyLengthType)}(r,c,flags)));` : "Reader selected=r;";
  decodeLines.push(`    ${bodyPrefix}`);
  const cases = caseInfo(type);
  cases.forEach((entry, index) => {
    const test = entry.discriminator === null ? "true" : Object.entries(entry.discriminator).map(([key, expected]) => {
      const variable = discriminatorValues.get(key);
      const d = type.discriminators.find((item) => item.name === key);
      const target = typeByName.get(d.$ref);
      return target.kind === "enum" ? `${variable}==${refType(d)}.${enumName(expected)}` : `${variable}.value()==${longLiteral(expected)}`;
    }).join(" && ");
    const decoded = decodeFields(entry.selected.fields, "selected", "c", "flags", typeByName, flagBits, "      ");
    const wireArgs = type.discriminators.filter((d) => d.source.kind === "wire").map((d) => discriminatorValues.get(d.name));
    const args = [...wireArgs, ...entry.selected.fields.map((f) => fieldName(f.name))].join(", ");
    decodeLines.push(`    ${index === 0 ? "if" : "else if"}(${test}) {\n${decoded.lines.join("\n")}\n      ${type.bodyLengthType ? `selected.end(${javaString(type.name)});` : ""} return new ${entry.name}(${args});\n    }`);
  });
  decodeLines.push(`    throw error(${javaString(type.name + " discriminator")});`);

  const encodeLines = [];
  for (const entry of cases) {
    encodeLines.push(`    if(value instanceof ${entry.name} item){`);
    const wire = type.discriminators.filter((d) => d.source.kind === "wire");
    for (const d of wire) encodeLines.push(`      ${encoderCall(d, `item.${fieldName(d.name)}()`, "w", "c", "flags", new Map(), typeByName)};`);
    if (type.bodyLengthType) encodeLines.push("      Writer selected=new Writer();");
    encodeLines.push(...encodeFields(entry.selected.fields, "item", type.bodyLengthType ? "selected" : "w", "c", "flags", typeByName, flagBits, "      "));
    if (type.bodyLengthType) encodeLines.push(`      byte[] bytes=selected.result(); encode${refType(type.bodyLengthType)}(new ${refType(type.bodyLengthType)}(bytes.length),w,c,flags); w.bytes(bytes);`);
    encodeLines.push("      return;\n    }");
  }
  encodeLines.push(`    throw error(${javaString(type.name + " case")});`);
  return `  private static ${name} decode${name}(${decodeParams}) throws IOException {\n${decodeLines.join("\n")}\n  }\n  private static void encode${name}(${encodeParams}) throws IOException {\n${encodeLines.join("\n")}\n  }`;
}

function tlvMethods(type, typeByName, flagBits) {
  const name = typeName(type.name);
  const vars = type.fields.map((f) => `    ${refType(f)} ${fieldName(f.name)}=null;`).join("\n");
  const cases = type.fields.map((f) => `case ${f.id} -> ${fieldName(f.name)}=${decoderCall(f, "item", "c", "flags", new Map(), typeByName)};`).join(" ");
  const required = type.fields.filter((f) => f.required).map((f) => `require(${fieldName(f.name)}!=null,${javaString(f.name + " required")});`).join(" ");
  const args = type.fields.map((f) => fieldName(f.name)).join(", ");
  const enc = type.fields.map((f) => `if(value.${fieldName(f.name)}()!=null){Writer item=new Writer(); ${encoderCall(f, `value.${fieldName(f.name)}()`, "item", "c", "flags", new Map(), typeByName)}; byte[] bytes=item.result(); encode${refType(type.fieldIdType)}(new ${refType(type.fieldIdType)}(${f.id}),body,c,flags); encode${refType(type.fieldLengthType)}(new ${refType(type.fieldLengthType)}(bytes.length),body,c,flags); body.bytes(bytes);}`).join("\n    ");
  return `  private static ${name} decode${name}(Reader r, DecoderContext c, int flags) throws IOException { Reader body=r.slice(length(decode${refType(type.totalLengthType)}(r,c,flags))); ${vars}\n    int previous=0; while(!body.done()){int id=length(decode${refType(type.fieldIdType)}(body,c,flags));require(id>previous,${javaString(type.name + " order")});previous=id;Reader item=body.slice(length(decode${refType(type.fieldLengthType)}(body,c,flags)));switch(id){${cases} default -> {} }item.end(${javaString(type.name + " field")});} ${required} return new ${name}(${args}); }\n`
    + `  private static void encode${name}(${name} value, Writer w, DecoderContext c, int flags) throws IOException { Writer body=new Writer(); ${enc} byte[] bytes=body.result(); encode${refType(type.totalLengthType)}(new ${refType(type.totalLengthType)}(bytes.length),w,c,flags);w.bytes(bytes); }`;
}

function publicMethods(ir) {
  const typeMethods = ir.types.map((type) => {
    const name = typeName(type.name);
    const external = unionParameters(type);
    const params = external.length ? `, ${external.join(", ")}` : "";
    const args = external.map((_, i) => `external${i}`).join(", ");
    const suffix = args ? `, ${args}` : "";
    return `  public static ${name} decode${name}(byte[] bytes, DecoderContext context${params}) throws IOException {Reader r=new Reader(bytes);${name} value=decode${name}(r,context,0${suffix});r.end(${javaString(type.name)});return value;}\n  public static byte[] encode${name}(${name} value, DecoderContext context${params}) throws IOException {Writer w=new Writer();encode${name}(value,w,context,0${suffix});return w.result();}`;
  }).join("\n");
  return typeMethods;
}

function commandDeclarations(ir) {
  const records = ir.commands.map((command) => {
    const fields = command.body.map((f) => `${refType(f)} ${fieldName(f.name)}`);
    fields.unshift("int flags");
    if (command.payload.type) fields.push(`${refType(command.payload.type)} payload`);
    return `  public record ${typeName(command.name)}Command(${fields.join(", ")}) implements ServiceWireCommand { public int id(){return ${command.id};} }`;
  });
  return `  public sealed interface ServiceWireCommand permits ${ir.commands.map((c) => `${typeName(c.name)}Command`).join(", ")} {int id();int flags();}\n${records.join("\n")}`;
}

function commandMethods(ir, typeByName, flagBits) {
  const decodeCases = ir.commands.map((command) => {
    const decoded = decodeFields(command.body, "r", "c", "flags", typeByName, flagBits, "        ");
    const payload = command.payload.type ? `frames.size()>1?decode${refType(command.payload.type)}(new Reader(frames.get(1)),c,0):null` : null;
    const required = command.payload.policy === "required" ? `require(frames.size()==2,${javaString(command.name + " payload")});` : command.payload.policy === "forbidden" ? `require(frames.size()==1,${javaString(command.name + " frames")});` : `require(frames.size()<=2,${javaString(command.name + " frames")});`;
    const args = ["flags", ...command.body.map((f) => fieldName(f.name)), ...(payload ? [payload] : [])].join(", ");
    return `      case ${command.id} -> {${required}\n${decoded.lines.join("\n")}\n        r.end(${javaString(command.name)}); yield new ${typeName(command.name)}Command(${args});}`;
  }).join("\n");
  const encodeCases = ir.commands.map((command) => {
    const name = `${typeName(command.name)}Command`;
    const encoded = encodeFields(command.body, "item", "w", "c", "item.flags()", typeByName, flagBits, "        ");
    const payload = command.payload.type ? `if(item.payload()!=null)frames.add(encode${refType(command.payload.type)}(item.payload(),c));` : "";
    return `      case ${command.id} -> {${name} item=(${name})value;\n${encoded.join("\n")}\n        List<byte[]> frames=new ArrayList<>();frames.add(w.result());${payload}yield frames;}`;
  }).join("\n");
  return `  public static ServiceWireCommand decodeCommand(List<byte[]> frames) throws IOException{return decodeCommand(frames,new DecoderContext(null,null,null));}\n  public static ServiceWireCommand decodeCommand(List<byte[]> frames,DecoderContext c)throws IOException{require(!frames.isEmpty(),"command frame");Reader r=new Reader(frames.get(0));require(r.u8()==${ir.protocol.magic[0]}&&r.u8()==${ir.protocol.magic[1]}&&r.u8()==${ir.protocol.wireMajor},"header");int id=r.u8(),flags=r.u8();return switch(id){\n${decodeCases}\n      default -> throw error("command id");};}\n`
    + `  public static List<byte[]> encodeCommandFrames(ServiceWireCommand value)throws IOException{return encodeCommandFrames(value,new DecoderContext(null,null,null));}\n  public static List<byte[]> encodeCommandFrames(ServiceWireCommand value,DecoderContext c)throws IOException{Writer w=new Writer();w.u8(${ir.protocol.magic[0]});w.u8(${ir.protocol.magic[1]});w.u8(${ir.protocol.wireMajor});w.u8(value.id());w.u8(value.flags());return switch(value.id()){\n${encodeCases}\n      default -> throw error("command id");};}\n  public static byte[] encodeCommand(ServiceWireCommand value)throws IOException{return encodeCommandFrames(value).get(0);}`;
}

function durableMethods(ir, typeByName) {
  const decode = ir.durableFormats.map((format) => `case ${javaString(format.name)} -> decodeDurable${typeName(format.name)}(bytes,c)`).join("; ");
  const encode = ir.durableFormats.map((format) => `case ${javaString(format.name)} -> encodeDurable${typeName(format.name)}((${refType(format.body)})value,c)`).join("; ");
  const methods = ir.durableFormats.map((format) => {
    const body = refType(format.body), name = typeName(format.name);
    return `  private static ${body} decodeDurable${name}(byte[] bytes,DecoderContext c)throws IOException{Reader r=new Reader(bytes);${format.magic.map((b) => `require(r.u8()==${b},"magic");`).join("")}require(r.u8()==${format.formatVersion},"version");decode${refType(format.flagsType)}(r,c,0);Reader bodyReader=r.slice(length(decode${refType(format.bodyLengthType)}(r,c,0)));${body} value=decode${body}(bodyReader,c,0);bodyReader.end(${javaString(format.name)});int checksum=(int)r.uint(4);r.end(${javaString(format.name)});CRC32C crc=new CRC32C();crc.update(bytes,0,bytes.length-4);require((int)crc.getValue()==checksum,"checksum");return value;}\n  private static byte[] encodeDurable${name}(${body} value,DecoderContext c)throws IOException{Writer bodyWriter=new Writer();encode${body}(value,bodyWriter,c,0);byte[] body=bodyWriter.result();Writer w=new Writer();${format.magic.map((b) => `w.u8(${b});`).join("")}w.u8(${format.formatVersion});encode${refType(format.flagsType)}(new ${refType(format.flagsType)}(${integerArgument(format.flagsType, longLiteral(format.flags), typeByName)}),w,c,0);encode${refType(format.bodyLengthType)}(new ${refType(format.bodyLengthType)}(${integerArgument(format.bodyLengthType, "body.length", typeByName)}),w,c,0);w.bytes(body);CRC32C crc=new CRC32C();byte[] covered=w.result();crc.update(covered,0,covered.length);w.uint(4,crc.getValue());return w.result();}`;
  }).join("\n");
  return `${methods}\n  public static Object decodeDurable(String name,byte[] bytes)throws IOException{return decodeDurable(name,bytes,new DecoderContext(null,null,null));}\n  public static Object decodeDurable(String name,byte[] bytes,DecoderContext c)throws IOException{return switch(name){${decode};default->throw error("durable format");};}\n  public static byte[] encodeDurable(String name,Object value)throws IOException{return encodeDurable(name,value,new DecoderContext(null,null,null));}\n  public static byte[] encodeDurable(String name,Object value,DecoderContext c)throws IOException{return switch(name){${encode};default->throw error("durable format");};}`;
}

function render(ir) {
  const typeByName = new Map(ir.types.map((type) => [type.name, type]));
  const flagBits = new Map(ir.flags.map((flag) => [flag.name, flag.bit]));
  const context = ir.semanticContexts.map((entry) => {
    const type = entry.valueType ? refType(entry.valueType) : "Boolean";
    return `${type} ${fieldName(entry.name)}`;
  }).join(", ");
  const declarations = ir.types.map(declaration).join("\n");
  const codecs = ir.types.map((type) => codecMethods(type, typeByName, flagBits)).join("\n");
  return `// <auto-generated> DO NOT EDIT. Generated solely from validated service-wire IR.\npackage systems.zlink.framework.runtime.protocol;\n\nimport java.io.*;\nimport java.math.BigInteger;\nimport java.nio.*;\nimport java.nio.charset.*;\nimport java.util.*;\nimport java.util.zip.CRC32C;\n\npublic final class ServiceWireCodec {\n  private ServiceWireCodec(){}\n  public record DecoderContext(${context}) {}\n${declarations}\n${commandDeclarations(ir)}\n${runtimeHelpers()}\n${codecs}\n${publicMethods(ir)}\n${commandMethods(ir, typeByName, flagBits)}\n${durableMethods(ir, typeByName)}\n}\n`;
}

function runtimeHelpers() {
  return String.raw`  private static final class Reader { final byte[] bytes; int at; Reader(byte[] bytes){this.bytes=bytes;} int u8()throws IOException{return(int)uint(1);} long uint(int n)throws IOException{need(n);long v=0;for(int i=0;i<n;i++)v=(v<<8)|Byte.toUnsignedLong(bytes[at++]);return v;} long i64()throws IOException{return uint(8);} byte[] bytes(int n)throws IOException{need(n);return Arrays.copyOfRange(bytes,at,at+=n);} Reader slice(int n)throws IOException{return new Reader(bytes(n));} boolean done(){return at==bytes.length;} void end(String name)throws IOException{require(done(),name+" trailing");} void need(int n)throws IOException{if(n<0||at+n>bytes.length)throw new EOFException("truncated field");} }
  private static final class Writer { final ByteArrayOutputStream out=new ByteArrayOutputStream(); void u8(int v){out.write(v);} void uint(int n,long v){for(int i=n-1;i>=0;i--)out.write((int)(v>>>(i*8))&255);} void bytes(byte[] v){out.writeBytes(v);} byte[] result(){return out.toByteArray();} }
  private static IOException error(String value){return new IOException(value);} private static void require(boolean ok,String message)throws IOException{if(!ok)throw error(message);}
  private static void requireUnsigned(long value,int width,String minimum,String maximum,String name)throws IOException{BigInteger actual=new BigInteger(width==8?Long.toUnsignedString(value):Long.toString(Integer.toUnsignedLong((int)value)));if(minimum!=null&&actual.compareTo(new BigInteger(minimum))<0||maximum!=null&&actual.compareTo(new BigInteger(maximum))>0)throw error(name+" range");}
  private static void requireSigned(long value,String minimum,String maximum,String name)throws IOException{if(minimum!=null&&value<Long.parseLong(minimum)||maximum!=null&&value>Long.parseLong(maximum))throw error(name+" range");}
  private static int length(Object value)throws IOException{try{return switch(value){case U8 x->x.value();case U16 x->x.value();case U32 x->x.value();case U64 x->Math.toIntExact(x.value());default->throw error("length type");};}catch(ArithmeticException e){throw error("length range");}}
  private static byte[] strictBytes(String value)throws IOException{if(value==null)return null;byte[] bytes=value.getBytes(StandardCharsets.UTF_8);for(byte b:bytes)if(b==0)throw error("NUL text");return bytes;}
  private static String strictText(byte[] value,String name)throws IOException{for(byte b:value)if(b==0)throw error(name+" NUL");try{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();}catch(CharacterCodingException e){throw error(name+" UTF-8");}}
`;
}

const [mode, schema, outputArgument, ...rest] = process.argv.slice(2);
if (!schema || !outputArgument || rest.length || !["--write", "--check"].includes(mode)) {
  console.error("usage: node render-service-wire-java.mjs --write|--check <schema> <output>");
  process.exit(2);
}
const output = path.resolve(outputArgument);
try {
  const source = render(lowerSchema(path.resolve(schema)));
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

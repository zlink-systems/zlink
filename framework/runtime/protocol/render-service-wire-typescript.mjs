#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { lowerSchema } from "./service-wire-lowering.mjs";

const scriptPath = fileURLToPath(import.meta.url);
const root = path.dirname(scriptPath);
const outputPath = path.join(root, "generated/node/service_wire_codec.generated.ts");

function identifier(name) {
  return name.split(/[^A-Za-z0-9]+/).filter(Boolean)
    .map((part) => part[0].toUpperCase() + part.slice(1)).join("");
}

function property(name) {
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name) ? name : JSON.stringify(name);
}

function typeReference(reference) {
  return identifier(reference.$ref);
}

function integerType(type) {
  return type.encoding === "u64" || type.encoding === "i64" ? "bigint" : "number";
}

function fieldsType(fields) {
  if (fields.length === 0) return "Record<string, never>";
  return `{ ${fields.map((field) => `readonly ${property(field.name)}${field.when ? "?" : ""}: ${typeReference(field)}`).join("; ")} }`;
}

function emittedType(type) {
  switch (type.kind) {
    case "integer": return integerType(type);
    case "enum": return type.values.map((value) => JSON.stringify(value.name)).join(" | ");
    case "length-prefixed-bytes": return `Uint8Array${type.zeroLengthMeaning === "absent" ? " | null" : ""}`;
    case "length-prefixed-text": return `string${type.zeroLengthMeaning === "absent" ? " | null" : ""}`;
    case "struct": return fieldsType(type.fields);
    case "vector": return `readonly ${typeReference(type.item)}[]`;
    case "versioned-vector": {
      const repeat = type.layout.find((entry) => entry.kind === "repeat");
      return `{ readonly ${property(repeat.name)}: readonly ${typeReference(repeat.item)}[] }`;
    }
    case "versioned-length-delimited": return fieldsType(type.body);
    case "conditional-union": {
      const variants = Object.entries(type.cases).map(([signature, selected]) => {
        const discriminator = JSON.parse(signature);
        const discriminants = Object.entries(discriminator)
          .map(([name, value]) => `readonly ${property(name)}: ${JSON.stringify(value)}`);
        const selectedFields = selected.fields.map((field) =>
          `readonly ${property(field.name)}${field.when ? "?" : ""}: ${typeReference(field)}`);
        return `{ ${[...discriminants, ...selectedFields].join("; ")} }`;
      });
      if (type.otherwise.kind === "fields") variants.push(fieldsType(type.otherwise.fields));
      return variants.join(" | ");
    }
    case "tlv32": return `{ ${type.fields.map((field) =>
      `readonly ${property(field.name)}${field.required ? "" : "?"}: ${typeReference(field)}`).join("; ")} }`;
    default: throw new Error(`unsupported IR kind ${type.kind}`);
  }
}

function renderTypes(ir) {
  const lines = ir.types.map((type) => `export type ${identifier(type.name)} = ${emittedType(type)};`);
  for (const command of ir.commands) {
    lines.push(`export type ${identifier(command.name)}Command = { readonly command: ${JSON.stringify(command.name)}; readonly flags: number; ${command.body.map((field) =>
      `readonly ${property(field.name)}${field.when ? "?" : ""}: ${typeReference(field)};`).join(" ")} readonly payload?: ${command.payload.type ? typeReference(command.payload.type) : "never"} };`);
  }
  lines.push(`export type ServiceWireCommand = ${ir.commands.map((command) => identifier(command.name) + "Command").join(" | ")};`);
  return lines.join("\n");
}

const runtime = String.raw`
type JsonObject = Record<string, any>;
export type ServiceWireDecoderContext = Readonly<Record<string, string | number | bigint | boolean>>;
const definitions: JsonObject = IR;
const typeByName = new Map<string, JsonObject>(definitions.types.map((type: JsonObject) => [type.name, type]));
const commandById = new Map<number, JsonObject>(definitions.commands.map((command: JsonObject) => [command.id, command]));
const commandByName = new Map<string, JsonObject>(definitions.commands.map((command: JsonObject) => [command.name, command]));
const flagByName = new Map<string, JsonObject>(definitions.flags.map((flag: JsonObject) => [flag.name, flag]));
const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder("utf-8", { fatal: true });

class Reader {
  offset = 0;
  constructor(readonly bytes: Uint8Array, readonly end = bytes.length) {}
  take(length: number): Uint8Array {
    if (!Number.isSafeInteger(length) || length < 0 || this.offset + length > this.end) throw new RangeError("truncated service-wire value");
    const value = this.bytes.slice(this.offset, this.offset + length); this.offset += length; return value;
  }
  bounded(length: number): Reader { return new Reader(this.take(length)); }
  done(label: string): void { if (this.offset !== this.end) throw new RangeError(label + " trailing bytes"); }
}

class Writer {
  readonly bytes: number[] = [];
  put(bytes: Iterable<number>): void { this.bytes.push(...bytes); }
  result(): Uint8Array { return Uint8Array.from(this.bytes); }
}

function typeOf(reference: JsonObject): JsonObject {
  const type = typeByName.get(reference.$ref); if (type === undefined) throw new Error("unknown IR type " + reference.$ref); return type;
}
function numeric(value: unknown): bigint { return typeof value === "bigint" ? value : BigInt(value as number); }
function limit(value: unknown): bigint { return BigInt(value as string | number); }
function readInteger(type: JsonObject, reader: Reader): number | bigint {
  const widths: Record<string, number> = { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 };
  const width = widths[type.encoding]; const bytes = reader.take(width); let value = 0n;
  for (const octet of bytes) value = (value << 8n) | BigInt(octet);
  if (type.encoding === "i64" && (value & (1n << 63n)) !== 0n) value -= 1n << 64n;
  if (value < limit(type.minimum) || value > limit(type.maximum)) throw new RangeError(type.name + " range");
  return width === 8 ? value : Number(value);
}
function writeInteger(type: JsonObject, value: unknown, writer: Writer): void {
  const widths: Record<string, number> = { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 };
  const width = widths[type.encoding]; let integer = numeric(value);
  if (integer < limit(type.minimum) || integer > limit(type.maximum)) throw new RangeError(type.name + " range");
  if (integer < 0n) integer = BigInt.asUintN(64, integer);
  const bytes = new Array<number>(width);
  for (let index = width - 1; index >= 0; --index) { bytes[index] = Number(integer & 255n); integer >>= 8n; }
  writer.put(bytes);
}
function valuesEqual(left: unknown, right: unknown): boolean {
  if (typeof left === "bigint" || typeof right === "bigint") return numeric(left) === numeric(right);
  return left === right;
}
function pathValue(value: any, path: string): any { return path.split(".").reduce((current, part) => current?.[part], value); }
function predicate(condition: JsonObject | undefined, value: JsonObject, environment: JsonObject): boolean {
  if (condition === undefined) return true;
  return condition.all.every((atom: JsonObject) => {
    if (atom.kind === "fieldPresent") return value[atom.operand.name] !== undefined;
    if (atom.kind === "fieldEquals") return valuesEqual(value[atom.operand.name], atom.value);
    if (atom.kind === "contextEquals") return valuesEqual(environment.context[atom.operand.name], atom.value);
    const mask = atom.operands.reduce((result: number, operand: JsonObject) => result | flagByName.get(operand.name)!.bit, 0);
    return atom.kind === "allFlagsSet" ? (environment.flags & mask) === mask : (environment.flags & mask) !== 0;
  });
}
function readFields(fields: readonly JsonObject[], reader: Reader, environment: JsonObject): JsonObject {
  const value: JsonObject = {};
  for (const field of fields) if (predicate(field.when, value, environment)) value[field.name] = readReference(field, reader, { ...environment, enclosing: value });
  return value;
}
function writeFields(fields: readonly JsonObject[], value: JsonObject, writer: Writer, environment: JsonObject): void {
  for (const field of fields) {
    const present = predicate(field.when, value, environment);
    if (!present && value[field.name] !== undefined) throw new RangeError(field.name + " is forbidden");
    if (present) { if (value[field.name] === undefined) throw new RangeError(field.name + " is required"); writeReference(field, value[field.name], writer, { ...environment, enclosing: value }); }
  }
}
function readReference(reference: JsonObject, reader: Reader, environment: JsonObject): any { const value = readValue(typeOf(reference), reader, environment); checkFieldBounds(reference, value); return value; }
function writeReference(reference: JsonObject, value: any, writer: Writer, environment: JsonObject): void { checkFieldBounds(reference, value); writeValue(typeOf(reference), value, writer, environment); }
function checkFieldBounds(reference: JsonObject, value: any): void {
  if (reference.minimum !== undefined && numeric(value) < limit(reference.minimum)) throw new RangeError(reference.name + " minimum");
  if (reference.maximum !== undefined && numeric(value) > limit(reference.maximum)) throw new RangeError(reference.name + " maximum");
}
function checkConstraints(type: JsonObject, value: any): void {
  for (const constraint of type.constraints ?? []) {
    if (constraint.kind === "not-both-zero" && constraint.unless === undefined && constraint.fields.every((field: JsonObject) => numeric(value[field.name]) === 0n)) throw new RangeError(type.name + " all zero");
    if (constraint.kind === "field-less-than-or-equal" && numeric(value[constraint.left.name]) > numeric(value[constraint.right.name])) throw new RangeError(type.name + " field order");
    if (constraint.kind === "terminal-success-shape" && value.terminalResult === constraint.when.terminalResult && (value.failureCode !== constraint.requires.failureCode || value.hasCreation !== constraint.requires.hasCreation)) throw new RangeError(type.name + " terminal success shape");
    if (constraint.kind === "terminal-failure-shape" && value.terminalResult !== constraint.when.terminalResultNot && (value.hasCreation !== constraint.requires.hasCreation || value.hasApplicationPayload !== constraint.requires.hasApplicationPayload)) throw new RangeError(type.name + " terminal failure shape");
    if (constraint.kind === "existing-has-no-application-payload" && pathValue(value, "creation.createResult") === constraint.when["creation.createResult"] && value.hasApplicationPayload !== constraint.requires.hasApplicationPayload) throw new RangeError(type.name + " existing payload");
  }
}
function stable(value: any): string {
  if (typeof value === "bigint") return value.toString(16).padStart(16, "0");
  if (value instanceof Uint8Array) return Array.from(value, octet => octet.toString(16).padStart(2, "0")).join("");
  if (Array.isArray(value)) return value.map(stable).join("|");
  if (value !== null && typeof value === "object") return Object.keys(value).sort().map(key => key + ":" + stable(value[key])).join("|");
  return String(value);
}
function checkCollection(type: JsonObject, value: readonly any[]): void {
  for (const constraint of type.constraints ?? []) {
    const project = (entry: any) => constraint.field ? pathValue(entry, constraint.field.path) : constraint.fields ? constraint.fields.map((field: JsonObject) => pathValue(entry, field.path)) : entry;
    const keys = value.map(entry => stable(project(entry)));
    if (constraint.kind === "sorted" && keys.some((key, index) => index > 0 && keys[index - 1] >= key)) throw new RangeError(type.name + " not sorted");
    if (constraint.kind === "unique" && new Set(keys).size !== keys.length) throw new RangeError(type.name + " duplicate");
  }
}
function readValue(type: JsonObject, reader: Reader, environment: JsonObject): any {
  switch (type.kind) {
    case "integer": return readInteger(type, reader);
    case "enum": { const wire = readInteger(typeOf({ $ref: type.encoding }), reader); const item = type.values.find((entry: JsonObject) => valuesEqual(entry.value, wire)); if (item === undefined) throw new RangeError(type.name + " enum"); return item.name; }
    case "length-prefixed-bytes": { const length = Number(readReference(type.lengthType, reader, environment)); if (length === 0 && type.zeroLengthMeaning === "absent") return null; if (length < (type.minimumBytes ?? 0) || length > type.maximumBytes) throw new RangeError(type.name + " length"); return reader.take(length); }
    case "length-prefixed-text": { const length = Number(readReference(type.lengthType, reader, environment)); if (length === 0 && type.zeroLengthMeaning === "absent") return null; if (length < (type.minimumBytes ?? 0) || length > type.maximumBytes) throw new RangeError(type.name + " length"); const bytes = reader.take(length); if (bytes.includes(0)) throw new RangeError(type.name + " NUL"); return utf8Decoder.decode(bytes); }
    case "struct": { const value = readFields(type.fields, reader, environment); checkConstraints(type, value); return value; }
    case "vector": { const count = Number(readReference(type.countType, reader, environment)); if (count > type.maximumItems) throw new RangeError(type.name + " count"); const values = Array.from({ length: count }, () => readReference(type.item, reader, environment)); checkCollection(type, values); return values; }
    case "versioned-vector": { const value: JsonObject = {}; for (const entry of type.layout) { if (entry.kind === "repeat") value[entry.name] = Array.from({ length: Number(value[entry.countFrom]) }, () => readReference(entry.item, reader, environment)); else { const fieldValue = readReference(entry, reader, environment); if (entry.constant !== undefined && !valuesEqual(fieldValue, entry.constant)) throw new RangeError(type.name + " constant"); value[entry.name] = fieldValue; } } const repeat = type.layout.find((entry: JsonObject) => entry.kind === "repeat"); checkCollection(type, value[repeat.name]); return { [repeat.name]: value[repeat.name] }; }
    case "versioned-length-delimited": { const version = readReference(type.version, reader, environment); if (!valuesEqual(version, type.version.constant)) throw new RangeError(type.name + " version"); const body = reader.bounded(Number(readReference(type.length, reader, environment))); const value = readFields(type.body, body, environment); body.done(type.name); checkConstraints(type, value); return value; }
    case "conditional-union": return readUnion(type, reader, environment);
    case "tlv32": return readTlv(type, reader, environment);
    default: throw new Error("unsupported IR kind " + type.kind);
  }
}
function writeValue(type: JsonObject, value: any, writer: Writer, environment: JsonObject): void {
  switch (type.kind) {
    case "integer": writeInteger(type, value, writer); return;
    case "enum": { const item = type.values.find((entry: JsonObject) => entry.name === value); if (item === undefined) throw new RangeError(type.name + " enum"); writeInteger(typeOf({ $ref: type.encoding }), item.value, writer); return; }
    case "length-prefixed-bytes": { if (value === null && type.zeroLengthMeaning === "absent") { writeReference(type.lengthType, 0, writer, environment); return; } const bytes = value as Uint8Array; if (!(bytes instanceof Uint8Array) || bytes.length < (type.minimumBytes ?? 0) || bytes.length > type.maximumBytes) throw new RangeError(type.name + " length"); writeReference(type.lengthType, bytes.length, writer, environment); writer.put(bytes); return; }
    case "length-prefixed-text": { if (value === null && type.zeroLengthMeaning === "absent") { writeReference(type.lengthType, 0, writer, environment); return; } const bytes = utf8Encoder.encode(value); if (bytes.includes(0) || bytes.length < (type.minimumBytes ?? 0) || bytes.length > type.maximumBytes) throw new RangeError(type.name + " text"); writeReference(type.lengthType, bytes.length, writer, environment); writer.put(bytes); return; }
    case "struct": checkConstraints(type, value); writeFields(type.fields, value, writer, environment); return;
    case "vector": { if (!Array.isArray(value) || value.length > type.maximumItems) throw new RangeError(type.name + " count"); checkCollection(type, value); writeReference(type.countType, value.length, writer, environment); for (const item of value) writeReference(type.item, item, writer, environment); return; }
    case "versioned-vector": { const repeat = type.layout.find((entry: JsonObject) => entry.kind === "repeat"); const entries = value[repeat.name]; checkCollection(type, entries); for (const entry of type.layout) { if (entry.kind === "repeat") for (const item of entries) writeReference(entry.item, item, writer, environment); else writeReference(entry, entry.constant ?? entries.length, writer, environment); } return; }
    case "versioned-length-delimited": { writeReference(type.version, type.version.constant, writer, environment); const body = new Writer(); checkConstraints(type, value); writeFields(type.body, value, body, environment); const encoded = body.result(); writeReference(type.length, encoded.length, writer, environment); writer.put(encoded); return; }
    case "conditional-union": writeUnion(type, value, writer, environment); return;
    case "tlv32": writeTlv(type, value, writer, environment); return;
    default: throw new Error("unsupported IR kind " + type.kind);
  }
}
function discriminators(type: JsonObject, value: JsonObject, reader: Reader | undefined, environment: JsonObject): JsonObject {
  const result: JsonObject = {};
  for (const discriminator of type.discriminators) {
    if (discriminator.source.kind === "wire") result[discriminator.name] = reader ? readReference(discriminator, reader, environment) : value[discriminator.name];
    else if (discriminator.source.kind === "enclosingField") result[discriminator.name] = environment.enclosing[discriminator.source.name];
    else result[discriminator.name] = environment.context[discriminator.source.name];
  }
  return result;
}
function selectCase(type: JsonObject, values: JsonObject): JsonObject | undefined {
  for (const [signature, selected] of Object.entries(type.cases) as [string, JsonObject][]) if (Object.entries(JSON.parse(signature)).every(([name, value]) => valuesEqual(values[name], value))) return selected;
  if (type.otherwise.kind === "protocol-error") throw new RangeError(type.name + " discriminator");
  return type.otherwise;
}
function readUnion(type: JsonObject, reader: Reader, environment: JsonObject): JsonObject {
  const values = discriminators(type, {}, reader, environment); const selected = selectCase(type, values)!;
  const body = type.bodyLengthType ? reader.bounded(Number(readReference(type.bodyLengthType, reader, environment))) : reader;
  const value = { ...values, ...readFields(selected.fields, body, environment) }; if (type.bodyLengthType) body.done(type.name); return value;
}
function writeUnion(type: JsonObject, value: JsonObject, writer: Writer, environment: JsonObject): void {
  const values = discriminators(type, value, undefined, environment); const selected = selectCase(type, values)!;
  for (const discriminator of type.discriminators) if (discriminator.source.kind === "wire") writeReference(discriminator, values[discriminator.name], writer, environment);
  const body = new Writer(); writeFields(selected.fields, value, body, environment); const encoded = body.result();
  if (type.bodyLengthType) writeReference(type.bodyLengthType, encoded.length, writer, environment); writer.put(encoded);
}
function readTlv(type: JsonObject, reader: Reader, environment: JsonObject): JsonObject {
  const body = reader.bounded(Number(readReference(type.totalLengthType, reader, environment))); const result: JsonObject = {}; let previous = -1;
  while (body.offset < body.end) { const id = Number(readReference(type.fieldIdType, body, environment)); const length = Number(readReference(type.fieldLengthType, body, environment)); if (id <= previous) throw new RangeError(type.name + " field order"); previous = id; const fieldReader = body.bounded(length); const field = type.fields.find((entry: JsonObject) => entry.id === id); if (field !== undefined) { const value = readReference(field, fieldReader, environment); checkFieldBounds(field, value); fieldReader.done(field.name); result[field.name] = value; } }
  for (const field of type.fields) if (field.required && result[field.name] === undefined) throw new RangeError(type.name + " missing " + field.name); return result;
}
function writeTlv(type: JsonObject, value: JsonObject, writer: Writer, environment: JsonObject): void {
  const body = new Writer(); for (const field of type.fields) { const fieldValue = value[field.name]; if (fieldValue === undefined) { if (field.required) throw new RangeError(type.name + " missing " + field.name); continue; } checkFieldBounds(field, fieldValue); const encoded = new Writer(); writeReference(field, fieldValue, encoded, environment); const bytes = encoded.result(); writeReference(type.fieldIdType, field.id, body, environment); writeReference(type.fieldLengthType, bytes.length, body, environment); body.put(bytes); } const bytes = body.result(); writeReference(type.totalLengthType, bytes.length, writer, environment); writer.put(bytes);
}
function environment(context: ServiceWireDecoderContext, flags = 0): JsonObject { return { context, flags, enclosing: {} }; }
export function decodeServiceWireType<T = unknown>(name: string, bytes: Uint8Array, context: ServiceWireDecoderContext = {}): T { const reader = new Reader(bytes); const value = readValue(typeByName.get(name)!, reader, environment(context)); reader.done(name); return value as T; }
export function encodeServiceWireType(name: string, value: unknown, context: ServiceWireDecoderContext = {}): Uint8Array { const writer = new Writer(); writeValue(typeByName.get(name)!, value, writer, environment(context)); return writer.result(); }
export function decodeServiceWireCommand(frames: readonly Uint8Array[], context: ServiceWireDecoderContext = {}): ServiceWireCommand {
  if (frames.length === 0) throw new RangeError("missing service-wire head"); const reader = new Reader(frames[0]);
  for (const expected of definitions.protocol.magic) if (readInteger(typeByName.get("u8")!, reader) !== expected) throw new RangeError("service-wire magic");
  if (readInteger(typeByName.get("u8")!, reader) !== definitions.protocol.wireMajor) throw new RangeError("service-wire major");
  const id = Number(readInteger(typeByName.get("u8")!, reader)); const flags = Number(readInteger(typeByName.get("u8")!, reader)); const command = commandById.get(id); if (command === undefined) throw new RangeError("service-wire command");
  const allowed = command.allowedFlags.reduce((mask: number, flag: JsonObject) => mask | flagByName.get(flag.name)!.bit, 0); const required = command.requiredFlags.reduce((mask: number, flag: JsonObject) => mask | flagByName.get(flag.name)!.bit, 0); if ((flags & ~allowed) !== 0 || (flags & required) !== required) throw new RangeError("service-wire flags");
  const result = { command: command.name, flags, ...readFields(command.body, reader, environment(context, flags)) } as JsonObject; reader.done(command.name);
  const payloadFrames = frames.slice(1); if (command.payload.policy === "forbidden" && payloadFrames.length !== 0 || command.payload.policy === "required" && payloadFrames.length !== 1 || command.payload.policy === "optional" && payloadFrames.length > 1) throw new RangeError(command.name + " payload frames");
  if (payloadFrames.length === 1) result.payload = decodeServiceWireType(command.payload.type.$ref, payloadFrames[0], context); return result as ServiceWireCommand;
}
export function encodeServiceWireCommand(value: ServiceWireCommand, context: ServiceWireDecoderContext = {}): readonly Uint8Array[] {
  const command = commandByName.get(value.command)!; const writer = new Writer(); writer.put(definitions.protocol.magic); writeInteger(typeByName.get("u8")!, definitions.protocol.wireMajor, writer); writeInteger(typeByName.get("u8")!, command.id, writer); writeInteger(typeByName.get("u8")!, value.flags, writer); writeFields(command.body, value as JsonObject, writer, environment(context, value.flags));
  const payload = (value as JsonObject).payload; if (command.payload.policy === "forbidden" && payload !== undefined || command.payload.policy === "required" && payload === undefined) throw new RangeError(command.name + " payload"); return payload === undefined ? [writer.result()] : [writer.result(), encodeServiceWireType(command.payload.type.$ref, payload, context)];
}
function crc32c(bytes: Uint8Array): number { let crc = 0xffffffff; for (const octet of bytes) { crc ^= octet; for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) !== 0 ? 0x82f63b78 : 0); } return (~crc) >>> 0; }
export function decodeServiceWireDurableFormat<T = unknown>(name: string, bytes: Uint8Array, context: ServiceWireDecoderContext = {}): T { const format = definitions.durableFormats.find((entry: JsonObject) => entry.name === name); if (format === undefined || bytes.length > format.maximumEncodedBytes) throw new RangeError("durable format"); const reader = new Reader(bytes); for (const expected of format.magic) if (readInteger(typeByName.get("u8")!, reader) !== expected) throw new RangeError(name + " magic"); if (readInteger(typeByName.get("u8")!, reader) !== format.formatVersion || readReference(format.flagsType, reader, environment(context)) !== format.flags) throw new RangeError(name + " header"); const body = reader.take(Number(readReference(format.bodyLengthType, reader, environment(context)))); const checksumOffset = reader.offset; const checksum = Number(readInteger(typeByName.get("u32")!, reader)); reader.done(name); if (crc32c(bytes.slice(0, checksumOffset)) !== checksum) throw new RangeError(name + " checksum"); return decodeServiceWireType<T>(format.body.$ref, body, context); }
export function encodeServiceWireDurableFormat(name: string, value: unknown, context: ServiceWireDecoderContext = {}): Uint8Array { const format = definitions.durableFormats.find((entry: JsonObject) => entry.name === name); if (format === undefined) throw new RangeError("durable format"); const body = encodeServiceWireType(format.body.$ref, value, context); const writer = new Writer(); writer.put(format.magic); writeInteger(typeByName.get("u8")!, format.formatVersion, writer); writeReference(format.flagsType, format.flags, writer, environment(context)); writeReference(format.bodyLengthType, body.length, writer, environment(context)); writer.put(body); writeInteger(typeByName.get("u32")!, crc32c(writer.result()), writer); const bytes = writer.result(); if (bytes.length > format.maximumEncodedBytes) throw new RangeError(name + " maximum"); return bytes; }
export function decodeRelocationLogicalStream<T = RelocationEnvelopeV1>(bytes: Uint8Array, context: ServiceWireDecoderContext = {}): T { return decodeServiceWireType<T>(definitions.relocationLogicalStreamFormat.body.$ref, bytes, context); }
export function encodeRelocationLogicalStream(value: RelocationEnvelopeV1, context: ServiceWireDecoderContext = {}): Uint8Array { const bytes = encodeServiceWireType(definitions.relocationLogicalStreamFormat.body.$ref, value, context); if (bytes.length > definitions.relocationLogicalStreamFormat.maximumBytes) throw new RangeError("relocation logical stream maximum"); return bytes; }
`;

function render(ir) {
  const json = JSON.stringify(ir, null, 2);
  return `/* This file is generated by render-service-wire-typescript.mjs. */\n/* eslint-disable */\n${renderTypes(ir)}\n${runtime.replace("IR", json)}\n`;
}

function main() {
  const [mode, schemaArgument] = process.argv.slice(2);
  if ((mode !== "--write" && mode !== "--check") || process.argv.length > 4) {
    console.error("usage: render-service-wire-typescript.mjs --write|--check [schema-path]");
    process.exit(2);
  }
  const schemaPath = path.resolve(schemaArgument ?? path.join(root, "service-wire-v1.schema.json"));
  const output = render(lowerSchema(schemaPath));
  if (mode === "--write") {
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, output);
    console.log(`wrote ${path.relative(root, outputPath)} (${Buffer.byteLength(output)} bytes)`);
    return;
  }
  const current = fs.existsSync(outputPath) ? fs.readFileSync(outputPath, "utf8") : "";
  if (current !== output) { console.error(`generated TypeScript codec drift: ${path.relative(root, outputPath)}`); process.exit(1); }
  console.log(`generated TypeScript codec is current (${irCounts(output)})`);
}

function irCounts(output) {
  return `${(output.match(/^export type /gm) ?? []).length} exported types`;
}

main();

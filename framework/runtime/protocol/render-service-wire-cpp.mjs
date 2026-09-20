#!/usr/bin/env node
// SPDX-License-Identifier: FSL-1.1-ALv2

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { lowerSchema } from "./service-wire-lowering.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const output = path.join(root, "protocol/generated/cpp/service_wire_codec.hpp");
const identifier = (name) => name.replaceAll(/[^A-Za-z0-9_]/g, "_");
const literal = (value) => {
  if (typeof value === "string" && /^-?\d+$/.test(value)) return `${value}${value.startsWith("-") ? "" : "ull"}`;
  if (typeof value === "number") return `${value}${value >= 0 ? "ull" : ""}`;
  return "0ull";
};
const ref = (field) => identifier(field.$ref);
const typeMap = (ir) => new Map(ir.types.map((type) => [type.name, type]));

function condition(condition, values) {
  if (!condition) return "true";
  return condition.all.map((atom) => {
    if (atom.kind === "fieldPresent") return values.get(atom.operand.name)?.present ?? "false";
    if (atom.kind === "fieldEquals") {
      const field = values.get(atom.operand.name);
      const declaration = typeMapCache.get(field?.type);
      const enumValue = typeof atom.value === "string"
        ? declaration?.values?.find((entry) => entry.name === atom.value)?.value
        : atom.value;
      return `${field?.expression ?? "0ull"}==${literal(enumValue)}`;
    }
    if (atom.kind === "allFlagsSet") return atom.operands.map((flag) => `(flags&${flagBits.get(flag.name)}u)!=0`).join("&&");
    if (atom.kind === "anyFlagsSet") return atom.operands.map((flag) => `(flags&${flagBits.get(flag.name)}u)!=0`).join("||");
    return "false"; // Context-only unions require their declared decoder context.
  }).join("&&") || "true";
}

let flagBits = new Map();
function emitFields(fields, types, { bounded = false } = {}) {
  const values = new Map();
  const lines = [];
  for (const field of fields) {
    const name = identifier(field.name);
    const type = types.get(field.$ref);
    const guard = condition(field.when, values);
    if (field.when) lines.push(`if(${guard}){`);
    lines.push(`bool ${name}_present=true;`);
    if (type?.kind === "integer" || type?.kind === "enum") {
      lines.push(`std::uint64_t ${name}=0;auto field_error_${name}=read_${ref(field)}(r,${name});if(field_error_${name}!=error_code::ok)return field_error_${name};`);
      values.set(field.name, { expression: name, present: `${name}_present`, type: field.$ref });
    } else {
      const arguments_ = type?.kind === "conditional-union"
        ? type.discriminators.filter((entry) => entry.source.kind !== "wire").map((entry) => {
          if (entry.source.kind === "enclosingField") {
            return values.get(entry.source.name)?.expression ?? "0ull";
          }
          return "0ull";
        })
        : [];
      lines.push(`auto field_error_${name}=validate_${ref(field)}(r${arguments_.map((argument) => `,${argument}`).join("")});if(field_error_${name}!=error_code::ok)return field_error_${name};`);
      values.set(field.name, { expression: name, present: `${name}_present`, type: field.$ref });
    }
    if (field.constant !== undefined) lines.push(`if(${name}!=${literal(field.constant)})return error_code::constant;`);
    if (field.minimum !== undefined) lines.push(`if(${name}<${literal(field.minimum)})return error_code::range;`);
    if (field.maximum !== undefined) lines.push(`if(${name}>${literal(field.maximum)})return error_code::range;`);
    if (field.when) lines.push(`}else{bool ${name}_present=false;(void)${name}_present;}`);
  }
  return lines.join("");
}

function emitType(type, types) {
  const n = identifier(type.name);
  if (type.kind === "integer") {
    const width = { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[type.encoding];
    return `inline error_code read_${n}(reader_t&r,std::uint64_t&v){return r.integer(${width},v,${literal(type.minimum)},${literal(type.maximum)});}inline error_code validate_${n}(reader_t&r){std::uint64_t v;return read_${n}(r,v);}`;
  }
  if (type.kind === "enum") {
    const width = { u8: 1, u16: 2, u32: 4, u64: 8, i64: 8 }[type.encoding];
    const enums = type.values.map((value) => `value_${identifier(value.name)}=${literal(value.value)}`).join(",");
    const allowed = type.values.map((value) => `v==${literal(value.value)}`).join("||");
    return `enum class ${n}_tag:std::uint64_t{${enums}};inline error_code read_${n}(reader_t&r,std::uint64_t&v){auto e=r.integer(${width},v,0ull,18446744073709551615ull);return e==error_code::ok&& !(${allowed})?error_code::enum_value:e;}inline error_code validate_${n}(reader_t&r){std::uint64_t v;return read_${n}(r,v);}`;
  }
  if (type.kind === "length-prefixed-bytes" || type.kind === "length-prefixed-text") {
    const text = type.kind === "length-prefixed-text";
    return `inline error_code validate_${n}(reader_t&r){std::uint64_t n=0;auto e=read_${ref(type.lengthType)}(r,n);if(e!=error_code::ok||n<${literal(type.minimumBytes)}||n>${literal(type.maximumBytes)})return e==error_code::ok?error_code::range:e;return r.bytes(n,${text ? "true" : "false"});}`;
  }
  if (type.kind === "struct") {
    const checks = (type.constraints ?? []).map((constraint) => {
      if (constraint.kind === "not-both-zero") {
        const fields = constraint.fields.map((field) => identifier(field.name));
        return `if(${fields.map((field) => `${field}==0`).join("&&")})return error_code::range;`;
      }
      return `if(${identifier(constraint.left.name)}>${identifier(constraint.right.name)})return error_code::range;`;
    }).join("");
    return `inline error_code validate_${n}(reader_t&r){${emitFields(type.fields, types)}${checks}return error_code::ok;}`;
  }
  if (type.kind === "vector") {
    const maximumItems = type.maximumItems === undefined ? "" : `||count>${literal(type.maximumItems)}`;
    return `inline error_code validate_${n}(reader_t&r){std::uint64_t count=0;auto e=read_${ref(type.countType)}(r,count);if(e!=error_code::ok${maximumItems})return e==error_code::ok?error_code::range:e;for(std::uint64_t i=0;i<count;++i){e=validate_${ref(type.item)}(r);if(e!=error_code::ok)return e;}return error_code::ok;}`;
  }
  if (type.kind === "versioned-vector") {
    const version = type.layout[0], count = type.layout[1], repeat = type.layout[2];
    return `inline error_code validate_${n}(reader_t&r){std::uint64_t version=0,count=0;auto e=read_${ref(version)}(r,version);if(e!=error_code::ok||version!=${literal(version.constant)})return e==error_code::ok?error_code::constant:e;e=read_${ref(count)}(r,count);if(e!=error_code::ok)return e;for(std::uint64_t i=0;i<count;++i){e=validate_${ref(repeat.item)}(r);if(e!=error_code::ok)return e;}return error_code::ok;}`;
  }
  if (type.kind === "versioned-length-delimited") return `inline error_code validate_${n}(reader_t&r){std::uint64_t version=0,length=0;auto e=read_${ref(type.version)}(r,version);if(e!=error_code::ok||version!=${literal(type.version.constant)})return e==error_code::ok?error_code::constant:e;e=read_${ref(type.length)}(r,length);if(e!=error_code::ok)return e;reader_t body;if((e=r.slice(length,body))!=error_code::ok)return e;reader_t saved=r;r=body;${emitFields(type.body, types)}const auto exact=r.at==r.end;r=saved;return exact?error_code::ok:error_code::trailing;}`;
  if (type.kind === "conditional-union") {
    const wires = type.discriminators.filter((d) => d.source.kind === "wire");
    const external = type.discriminators.filter((d) => d.source.kind !== "wire");
    const parameters = external.map((d) => `std::uint64_t ${identifier(d.name)}`).join(",");
    const declarations = wires.map((d) => `std::uint64_t ${identifier(d.name)}=0;auto e_${identifier(d.name)}=read_${ref(d)}(r,${identifier(d.name)});if(e_${identifier(d.name)}!=error_code::ok)return e_${identifier(d.name)};`).join("");
    const cases = Object.entries(type.cases).map(([key, entry]) => {
      const values = JSON.parse(key);
      const discriminatorNames = new Set(type.discriminators.map((entry) => entry.name));
      if (!Object.keys(values).every((name) => discriminatorNames.has(name))) return "";
      const check = Object.entries(values).map(([name, value]) => {
        const discriminator = type.discriminators.find((entry) => entry.name === name);
        const discriminatorType = types.get(discriminator.$ref);
        const number = discriminatorType?.values?.find((entry) => entry.name === value)?.value;
        return `${identifier(name)}==${literal(number)}`;
      }).join("&&");
      return `if(${check}){${emitFields(entry.fields, types)}return error_code::ok;}`;
    }).join("");
    const otherwise = type.otherwise.kind === "fields" ? `${emitFields(type.otherwise.fields, types)}return error_code::ok;` : "return error_code::union_case;";
    const body = `${cases}${otherwise}`;
    const bounded = type.bodyLengthType
      ? `std::uint64_t length=0;auto e=read_${ref(type.bodyLengthType)}(r,length);if(e!=error_code::ok)return e;reader_t body;if((e=r.slice(length,body))!=error_code::ok)return e;auto body_result=[&](reader_t&r)->error_code{${body}}(body);return body_result!=error_code::ok?body_result:(body.at==body.end?error_code::ok:error_code::trailing);`
      : body;
    return `enum class ${n}_tag:std::uint8_t{value};using ${n}_variant=std::variant<std::monostate,std::vector<std::uint8_t>>;inline error_code validate_${n}(reader_t&r${parameters ? `,${parameters}` : ""}){${declarations}${bounded}}`;
  }
  if (type.kind === "tlv32") {
    const cases = type.fields.map((field) => `case ${field.id}u:{reader_t value;if((e=r.slice(length,value))!=error_code::ok)return e;e=validate_${ref(field)}(value);if(e!=error_code::ok||value.at!=value.end)return e==error_code::ok?error_code::trailing:e;seen_${identifier(field.name)}=true;break;}`).join("");
    const seen = type.fields.map((field) => `bool seen_${identifier(field.name)}=false;`).join("");
    const required = type.fields.filter((field) => field.required).map((field) => `if(!seen_${identifier(field.name)})return error_code::required;`).join("");
    return `inline error_code validate_${n}(reader_t&r){std::uint64_t total=0;auto e=read_${ref(type.totalLengthType)}(r,total);if(e!=error_code::ok)return e;reader_t body;if((e=r.slice(total,body))!=error_code::ok)return e;${seen}std::uint64_t previous=0;while(body.at<body.end){std::uint64_t id=0,length=0;e=read_${ref(type.fieldIdType)}(body,id);if(e!=error_code::ok)return e;e=read_${ref(type.fieldLengthType)}(body,length);if(e!=error_code::ok||id<=previous)return e==error_code::ok?error_code::order:e;previous=id;switch(id){${cases}default:e=body.skip(length);if(e!=error_code::ok)return e;}}${required}return error_code::ok;}`;
  }
  throw new Error(`unhandled kind ${type.kind}`);
}

function emitCommand(command, types) {
  const n = identifier(`${command.name}_${command.id}`);
  const allowed = command.allowedFlags.reduce((sum, flag) => sum | flagBits.get(flag.name), 0);
  const required = command.requiredFlags.reduce((sum, flag) => sum | flagBits.get(flag.name), 0);
  const flagChecks = (command.flagConstraints ?? []).map((constraint) => constraint.kind === "all-or-none"
    ? `{const auto bits=${constraint.flags.map((flag) => flagBits.get(flag.name)).join("|")}u;if((flags&bits)!=0&&(flags&bits)!=bits)return error_code::flags;}`
    : `if((flags&${flagBits.get(constraint.if.name)}u)!=0&&(flags&${constraint.then.map((flag) => flagBits.get(flag.name)).join("|" )}u)!=${constraint.then.map((flag) => flagBits.get(flag.name)).join("|")}u)return error_code::flags;`).join("");
  const frames = command.payload.policy === "forbidden" ? "" : `struct ${n}_frames_t{std::vector<std::vector<std::uint8_t>> bytes;};inline result_t<${n}_frames_t> decode_${n}_frames(const std::vector<std::vector<std::uint8_t>>&frames){if(frames.empty()||frames.size()>2)return {error_code::trailing,{}};auto head=decode_${n}(frames.front());if(!head)return {head.error,{}};if(frames.size()==1)return ${command.payload.policy === "required" ? `{error_code::required,{}}` : `{error_code::ok,{frames}}`};auto payload=decode_${ref(command.payload.type)}(frames[1]);return payload?result_t<${n}_frames_t>{error_code::ok,{frames}}:result_t<${n}_frames_t>{payload.error,{}};}inline result_t<std::vector<std::vector<std::uint8_t>>> encode_${n}_frames(const ${n}_frames_t&value){auto decoded=decode_${n}_frames(value.bytes);return decoded?result_t<std::vector<std::vector<std::uint8_t>>>{error_code::ok,value.bytes}:result_t<std::vector<std::vector<std::uint8_t>>>{decoded.error,{}};}`;
  return `struct ${n}_t{std::vector<std::uint8_t> bytes;};inline result_t<${n}_t> decode_${n}(std::span<const std::uint8_t> bytes){reader_t r{bytes};std::uint64_t magic=0,major=0,id=0,flags=0;auto e=r.integer(2,magic,0ull,65535ull);if(e!=error_code::ok||magic!=0x5a4dull)return {error_code::header,{}};e=r.integer(1,major,0ull,255ull);if(e!=error_code::ok||major!=1)return {error_code::header,{}};e=r.integer(1,id,0ull,255ull);if(e!=error_code::ok||id!=${command.id}ull)return {error_code::header,{}};e=r.integer(1,flags,0ull,255ull);if(e!=error_code::ok||(flags&~${allowed}u)!=0||(flags&${required}u)!=${required}u)return {error_code::flags,{}};auto flag_result=[&]()->error_code{${flagChecks}return error_code::ok;}();if(flag_result!=error_code::ok)return {flag_result,{}};auto body_result=[&]()->error_code{${emitFields(command.body, types)}return error_code::ok;}();if(body_result!=error_code::ok)return {body_result,{}};if(r.at!=r.end)return {error_code::trailing,{}};return {error_code::ok,{std::vector<std::uint8_t>(bytes.begin(),bytes.end())}};}inline result_t<std::vector<std::uint8_t>> encode_${n}(const ${n}_t&value){auto decoded=decode_${n}(value.bytes);return decoded?result_t<std::vector<std::uint8_t>>{error_code::ok,value.bytes}:result_t<std::vector<std::uint8_t>>{decoded.error,{}};}${frames}`;
}

function emitDurable(format) {
  const n = identifier(format.name);
  const magic = format.magic.map((value) => `${value}u`).join(",");
  return `struct durable_${n}_t{std::vector<std::uint8_t> bytes;};inline result_t<durable_${n}_t> decode_durable_${n}(std::span<const std::uint8_t> bytes){reader_t r{bytes};const std::uint8_t magic[]={${magic}};for(auto expected:magic){std::uint64_t actual=0;auto e=r.integer(1,actual,0ull,255ull);if(e!=error_code::ok||actual!=expected)return {error_code::header,{}};}std::uint64_t version=0,flags=0,length=0;auto e=r.integer(1,version,0ull,255ull);if(e!=error_code::ok||version!=${literal(format.formatVersion)})return {error_code::header,{}};e=read_${ref(format.flagsType)}(r,flags);if(e!=error_code::ok||flags!=${literal(format.flags)})return {error_code::header,{}};e=read_u32(r,length);if(e!=error_code::ok)return {e,{}};reader_t body;if((e=r.slice(length,body))!=error_code::ok)return {e,{}};e=validate_${ref(format.body)}(body);if(e!=error_code::ok||body.at!=body.end)return {e==error_code::ok?error_code::trailing:e,{}};std::uint64_t checksum=0;if((e=read_u32(r,checksum))!=error_code::ok||r.at!=r.end)return {e==error_code::ok?error_code::trailing:e,{}};if(checksum!=crc32c(bytes.first(bytes.size()-4)))return {error_code::header,{}};return {error_code::ok,{std::vector<std::uint8_t>(bytes.begin(),bytes.end())}};}inline result_t<std::vector<std::uint8_t>> encode_durable_${n}(const durable_${n}_t&value){auto decoded=decode_durable_${n}(value.bytes);return decoded?result_t<std::vector<std::uint8_t>>{error_code::ok,value.bytes}:result_t<std::vector<std::uint8_t>>{decoded.error,{}};}`;
}

function render(ir) {
  flagBits = new Map(ir.flags.map((flag) => [flag.name, flag.bit]));
  typeMapCache = typeMap(ir);
  const enums = ir.types.filter((type) => type.kind === "enum").map((type) => emitType(type, typeMapCache)).join("\n");
  const others = ir.types.filter((type) => type.kind !== "enum").map((type) => emitType(type, typeMapCache)).join("\n");
  const commands = ir.commands.map((command) => emitCommand(command, typeMapCache)).join("\n");
  const durable = ir.durableFormats.map(emitDurable).join("\n");
  const typeApi = ir.types.map((type) => { const n=identifier(type.name); return `struct ${n}_t{std::vector<std::uint8_t> bytes;};inline result_t<${n}_t> decode_${n}(std::span<const std::uint8_t>b){reader_t r{b};auto e=validate_${n}(r);return e==error_code::ok&&r.at==r.end?result_t<${n}_t>{e,{std::vector<std::uint8_t>(b.begin(),b.end())}}:result_t<${n}_t>{e==error_code::ok?error_code::trailing:e,{}};}inline result_t<std::vector<std::uint8_t>> encode_${n}(const ${n}_t&v){auto x=decode_${n}(v.bytes);return x?result_t<std::vector<std::uint8_t>>{error_code::ok,v.bytes}:result_t<std::vector<std::uint8_t>>{x.error,{}};}`; }).join("\n");
  const declarations = ir.types.map((type) => {
    const parameters = type.kind === "conditional-union"
      ? type.discriminators.filter((entry) => entry.source.kind !== "wire")
        .map((entry) => `,std::uint64_t ${identifier(entry.name)}=0ull`).join("")
      : "";
    return `inline error_code validate_${identifier(type.name)}(reader_t&${parameters});`;
  }).join("");
  return `// <auto-generated> DO NOT EDIT. schema-sha256: ${crypto.createHash("sha256").update(JSON.stringify(ir)).digest("hex")}
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace zlink::framework::runtime::protocol::generated {
enum class error_code:std::uint8_t{ok,truncated,range,constant,enum_value,utf8,header,flags,union_case,order,required,trailing};
template<class T> struct result_t{error_code error;T value;explicit operator bool()const{return error==error_code::ok;}};
struct reader_t{std::span<const std::uint8_t>b;std::size_t at=0,end=b.size();error_code integer(std::size_t n,std::uint64_t&v,std::uint64_t min,std::uint64_t max){if(n>end-at)return error_code::truncated;v=0;for(std::size_t i=0;i<n;++i)v=(v<<8)|b[at++];return v<min||v>max?error_code::range:error_code::ok;}error_code skip(std::uint64_t n){if(n>end-at)return error_code::truncated;at+=static_cast<std::size_t>(n);return error_code::ok;}error_code bytes(std::uint64_t n,bool text){if(n>end-at)return error_code::truncated;for(std::size_t i=0;i<n;++i)if(text&&b[at+i]==0)return error_code::utf8;at+=static_cast<std::size_t>(n);return error_code::ok;}error_code slice(std::uint64_t n,reader_t&out){if(n>end-at)return error_code::truncated;out={b.subspan(at,static_cast<std::size_t>(n)),0,static_cast<std::size_t>(n)};at+=static_cast<std::size_t>(n);return error_code::ok;}};inline std::uint32_t crc32c(std::span<const std::uint8_t>b){std::uint32_t crc=0xffffffffu;for(auto byte:b){crc^=byte;for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1u)!=0?0x82f63b78u:0u);}return ~crc;}
${declarations}
${enums}
${others}
${typeApi}
${commands}
${durable}
}
`;
}

let typeMapCache = new Map();
const [mode, schema] = process.argv.slice(2);
if ((mode !== "--write" && mode !== "--check") || !schema) throw new Error("usage: node render-service-wire-cpp.mjs --write|--check <schema>");
const content = render(lowerSchema(path.resolve(schema)));
if (mode === "--check") { if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== content) process.exitCode = 1; }
else { fs.mkdirSync(path.dirname(output), { recursive: true }); fs.writeFileSync(output, content); }

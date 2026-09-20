#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema } from "./service-wire-lowering.mjs";

const [mode, schemaArg, outputArg, ...extra] = process.argv.slice(2);
if (!["--write", "--check"].includes(mode) || !schemaArg || !outputArg || extra.length) {
  console.error("usage: node render-service-wire-dotnet.mjs --write|--check <schema> <output>"); process.exit(2);
}
const ir = lowerSchema(path.resolve(schemaArg));
const output = path.resolve(outputArg);
const types = new Map(ir.types.map(x => [x.name, x]));
const flagBits = new Map(ir.flags.map(x => [x.name, x.bit]));
const seen = new Set();
const id = s => { const x=s.replace(/(^|[^A-Za-z0-9]+)([A-Za-z0-9])/g,(_m,_s,c)=>c.toUpperCase()); return /^\d/.test(x)?`N${x}`:x; };
const rn = r => id(r.$ref);
const op = (owner, name) => { const x=owner.operations.find(o=>o.op===name); if(!x)throw Error(`${owner.name}: missing ${name}`); seen.add(name); return x; };
const main = owner => { const x=owner.operations.find(o=>o.op!=="runtime-predicate"); if(!x)throw Error(`${owner.name}: no operation`); seen.add(x.op); return x; };
const intType = e => ({u8:"byte",u16:"ushort",u32:"uint",u64:"ulong",i64:"long"}[e] ?? (()=>{throw Error(`encoding ${e}`)})());
const lit = (v,e="u64") => `${v}${e==="u64"?"UL":e==="u32"?"U":e==="i64"?"L":""}`;
const typeOf = (r,n=false) => `${rn(r)}${n?"?":""}`;
const optional = f => Boolean(f.when)||f.required===false;
const fieldsDecl = fs => fs.map(f=>`${typeOf(f.type,optional(f))} ${id(f.name)}`).join(", ");
const cases = o => Object.entries(o.cases);

function declaration(t){const o=main(t),n=id(t.name); switch(o.op){
case"integer":return`internal sealed record ${n}(${intType(o.encoding)} Value);`;
case"enum":return`internal enum ${n} : ${intType(o.encoding)}\n{\n${o.values.map(v=>`    ${id(v.name)} = ${v.value}`).join(",\n")}\n}`;
case"length-prefixed":return`internal sealed record ${n}(${o.content==="text"?"string":"byte[]"}${o.zeroLengthMeaning?"?":""} Value);`;
case"struct":case"versioned-length-delimited":return`internal sealed record ${n}(${fieldsDecl(o.fields)});`;
case"vector":return`internal sealed record ${n}(IReadOnlyList<${typeOf(o.item)}> Items);`;
case"versioned-vector":{const r=o.layout.find(x=>x.kind==="repeat");return`internal sealed record ${n}(IReadOnlyList<${typeOf(r.item)}> ${id(r.name)});`;}
case"conditional-union":{const d=cases(o).map(([,v],i)=>`internal sealed record ${n}Case${i}(${fieldsDecl(v.fields)}) : ${n};`);if(o.otherwise.kind==="fields")d.push(`internal sealed record ${n}Otherwise(${fieldsDecl(o.otherwise.fields)}) : ${n};`);return`internal abstract record ${n};\n${d.join("\n")}`;}
case"tlv32":return`internal sealed record ${n}(${o.fields.map(f=>`${typeOf(f.type,true)} ${id(f.name)}`).join(", ")});`;
default:throw Error(`${t.name}: declaration ${o.op}`);}}

function expected(r,v){const t=types.get(r.$ref),o=main(t);if(o.op==="enum")return`${id(t.name)}.${id(v)}`;if(o.op==="integer")return`new ${id(t.name)}(${lit(v,o.encoding)})`;throw Error(`${t.name}: constant`);}
const scalar=(r,x)=>main(types.get(r.$ref)).op==="integer"?`${x}.Value`:x;
function external(r){const o=main(types.get(r.$ref));return o.op==="conditional-union"?o.discriminators.filter(d=>d.source.kind==="enclosingField"):[];}
function readCall(r,reader,env){return`Read${rn(r)}(${[reader,"context",...external(r).map(d=>env.get(d.source.name)??(()=>{throw Error(`${r.$ref}: missing ${d.source.name}`)})())].join(", ")})`;}
function writeCall(r,writer,value,env){return`Write${rn(r)}(${[writer,value,"context",...external(r).map(d=>env.get(d.source.name)??(()=>{throw Error(`${r.$ref}: missing ${d.source.name}`)})())].join(", ")})`;}
function cond(c,env,flags="flags"){return c.all.map(a=>{if(a.kind==="fieldPresent"){const field=env.get(`${a.operand.name}:field`),target=main(types.get(field.type.$ref)),value=env.get(a.operand.name);return target.op==="length-prefixed"&&target.zeroLengthMeaning?`${value}.Value is not null`:`${value} is not null`;}if(a.kind==="fieldEquals")return`${env.get(a.operand.name)} == ${expected(env.get(`${a.operand.name}:field`).type,a.value)}`;if(a.kind==="contextEquals"){const d=ir.semanticContexts.find(x=>x.name===a.operand.name);return`context.${id(a.operand.name)} == ${expected(d.valueType,a.value)}`;}if(a.kind==="allFlagsSet"||a.kind==="anyFlagsSet")return`(${a.operands.map(x=>`(${flags} & ${flagBits.get(x.name)}) != 0`).join(a.kind==="allFlagsSet"?" && ":" || ")})`;throw Error(`condition ${a.kind}`);}).join(" && ")||"true";}
function fieldChecks(f,x){const a=[];if(f.constant!==undefined)a.push(`if (${x} != ${expected(f.type,f.constant)}) throw Error("${f.name}: constant");`);const e=main(types.get(f.type.$ref)).encoding;if(f.minimum!==undefined)a.push(`if (${scalar(f.type,x)} < ${lit(f.minimum,e)}) throw Error("${f.name}: minimum");`);if(f.maximum!==undefined)a.push(`if (${scalar(f.type,x)} > ${lit(f.maximum,e)}) throw Error("${f.name}: maximum");`);for(const c of f.constraints??[]){seen.add(c.op);if(c.kind!=="contains-protocol-required-capability")throw Error(`field constraint ${c.kind}`);a.push(`if (!${x}.Items.Any(item => item.Value == ${JSON.stringify(ir.protocol.requiredCapability)})) throw Error("required capability");`);}return a;}
function readFields(fs,reader,base=new Map(),flags="0"){const env=new Map(base),a=[];for(const f of fs){seen.add(f.op);const n=id(f.name);env.set(f.name,n);env.set(`${f.name}:field`,f);if(f.when){a.push(`${typeOf(f.type,true)} ${n};`,`if (${cond(f.when,env,flags)})`,`{`,`    ${n} = ${readCall(f.type,reader,env)};`,...fieldChecks(f,n).map(x=>`    ${x}`),`}`,`else ${n} = null;`);}else a.push(`var ${n} = ${readCall(f.type,reader,env)};`,...fieldChecks(f,n));}return{lines:a,env,args:fs.map(f=>id(f.name)).join(", ")};}
function writeFields(fs,writer,owner,base=new Map(),flags="0"){const env=new Map(base),a=[];for(const f of fs){seen.add(f.op);const x=`${owner}.${id(f.name)}`;env.set(f.name,x);env.set(`${f.name}:field`,f);if(f.when)a.push(`if (${cond(f.when,env,flags)})`,`{`,`    if (${x} is null) throw Error("${f.name}: required");`,...fieldChecks(f,`${x}!`).map(y=>`    ${y}`),`    ${writeCall(f.type,writer,`${x}!`,env)};`,`}`,`else if (${x} is not null) throw Error("${f.name}: forbidden");`);else a.push(...fieldChecks(f,x),`${writeCall(f.type,writer,x,env)};`);}return{lines:a,env};}

function constraints(owner, value="value") {
  const o=main(owner), lines=[], helpers=[], ownerFields=new Map((o.fields??[]).map(field=>[field.name,field]));
  const equals=(name,expectedValue)=>`${value}.${id(name)} == ${expected(ownerFields.get(name).type,expectedValue)}`;
  (o.constraints??[]).forEach((constraint,index)=>{
    seen.add(constraint.op);
    if(constraint.kind==="not-both-zero") lines.push(`if (${constraint.fields.map(field=>`${value}.${id(field.name)}.Value == 0`).join(" && ")}) throw Error("not-both-zero");`);
    else if(constraint.kind==="field-less-than-or-equal") lines.push(`if (${value}.${id(constraint.left.name)}.Value > ${value}.${id(constraint.right.name)}.Value) throw Error("field order");`);
    else if(constraint.kind==="sorted"||constraint.kind==="unique") {
      if (constraint.comparison !== undefined && !new Set(["utf-8-bytes", "canonical-authority-key-bytes", "wire-value-then-utf-8-bytes", "unsigned-wire-value"]).has(constraint.comparison)) throw Error(`${owner.name}: comparison ${constraint.comparison}`);
      const property=o.op==="vector"?"Items":id(o.layout.find(x=>x.kind==="repeat").name), item=o.op==="vector"?o.item:o.layout.find(x=>x.kind==="repeat").item, key=`Key${id(owner.name)}${index}`, paths=constraint.fields?.map(x=>x.path)??(constraint.field?[constraint.field.path]:[""]), body=["var writer = new Writer();"];
      for(const [pathIndex,pathValue] of paths.entries()){let reference=item,expression="item";for(const part of pathValue?pathValue.split("."):[]){const field=main(types.get(reference.$ref)).fields.find(entry=>entry.name===part);reference=field.type;expression+=`.${id(part)}`;}body.push(constraint.comparison==="utf-8-bytes"||(constraint.comparison==="wire-value-then-utf-8-bytes"&&pathIndex===paths.length-1)?`writer.Bytes(Utf8.GetBytes(${expression}.Value!));`:`${writeCall(reference,"writer",expression,new Map())};`);}
      body.push("return writer.ToArray();");helpers.push(`    private static byte[] ${key}(${typeOf(item)} item, DecodeContext context)\n    {\n        ${body.join("\n        ")}\n    }`);lines.push(`var keys${index} = ${value}.${property}.Select(item => ${key}(item, context)).ToArray();`,constraint.kind==="sorted"?`if (keys${index}.Zip(keys${index}.Skip(1), (left, right) => CompareBytes(left, right) <= 0).Any(ok => !ok)) throw Error("unsorted vector");`:`if (keys${index}.Select(Convert.ToHexString).Distinct(StringComparer.Ordinal).Count() != keys${index}.Length) throw Error("duplicate vector item");`);
    } else if(constraint.kind==="terminal-success-shape") {
      const when=Object.entries(constraint.when).map(([name,expectedValue])=>equals(name,expectedValue)).join(" && "), required=Object.entries(constraint.requires).map(([name,expectedValue])=>equals(name,expectedValue)).join(" && ");lines.push(`if (${when} && !(${required})) throw Error("terminal success shape");`);
    } else if(constraint.kind==="terminal-failure-shape") {
      const [rawName,expectedValue]=Object.entries(constraint.when)[0], name=rawName.endsWith("Not")?rawName.slice(0,-3):rawName, required=Object.entries(constraint.requires).map(([fieldName,fieldValue])=>equals(fieldName,fieldValue)).join(" && ");lines.push(`if (!(${equals(name,expectedValue)}) && !(${required})) throw Error("terminal failure shape");`);
    } else if(constraint.kind==="existing-has-no-application-payload") {
      const [pathValue,discriminatorValue]=Object.entries(constraint.when)[0], [fieldName,discriminatorName]=pathValue.split("."), union=types.get(ownerFields.get(fieldName).type.$ref), caseIndex=cases(main(union)).findIndex(([signature])=>JSON.parse(signature)[discriminatorName]===discriminatorValue), required=Object.entries(constraint.requires).map(([name,expectedValue])=>equals(name,expectedValue)).join(" && ");if(caseIndex<0)throw Error(`${owner.name}: constraint case`);lines.push(`if (${value}.${id(fieldName)} is ${id(union.name)}Case${caseIndex} && !(${required})) throw Error("existing payload");`);
    } else throw Error(`constraint ${constraint.kind}`);
  });
  return {lines,helpers};
}
function predicates(owner,value){const a=[];for(const p of owner.operations.filter(x=>x.op==="runtime-predicate")){seen.add(p.op);if(p.reference.asset!=="service-wire-constants"||p.reference.name!=="valid-terminal-failure")throw Error(`predicate ${p.reference.name}`);if(main(owner).op==="conditional-union"){const i=cases(main(owner)).findIndex(([,c])=>c.fields.some(f=>f.name==="failureCode"));if(i<0)throw Error(`${owner.name}: predicate target`);a.push(`if (${value} is ${id(owner.name)}Case${i} terminal) ValidateTerminalFailure(terminal.TerminalResult, terminal.FailureCode);`);}else a.push(`ValidateTerminalFailure(${value}.TerminalResult, ${value}.FailureCode);`);}return a;}

function tlvPresence(operation, environment) {
  const lines = [];
  for (const rule of operation.presenceRules) {
    const test = cond(rule.when, environment);
    for (const name of rule.require ?? []) lines.push(`if (${test} && ${environment.get(name)} is null) throw Error("${name}: presence required");`);
    for (const name of rule.forbid ?? []) lines.push(`if (${test} && ${environment.get(name)} is not null) throw Error("${name}: presence forbidden");`);
  }
  return lines;
}

function assertSyntaxCoverage() {
  const fail = (owner, operation, detail) => { throw Error(`${owner}:${operation.op}: unsupported ${detail}`); };
  const inspectField = (owner, field) => {
    if (field.when && (field.whenFalse?.encoder !== "reject-present-value" || field.whenFalse?.decoder !== "consume-no-bytes")) fail(owner, field, "whenFalse");
    if (field.otherwise !== undefined && field.otherwise !== "forbidden") fail(owner, field, "otherwise");
  };
  for (const type of ir.types) {
    for (const operation of type.operations) {
      if (operation.op === "integer" || operation.op === "enum") {
        if (operation.byteOrder !== "big-endian") fail(type.name, operation, "byte order");
        if (operation.op === "enum" && operation.unknown !== "protocol-error") fail(type.name, operation, "enum unknown policy");
      } else if (operation.op === "length-prefixed") {
        if (!new Set(["bytes", "text"]).has(operation.content)) fail(type.name, operation, "content");
      } else if (operation.op === "text-validation") {
        if (operation.encoding !== "utf-8" || operation.malformed !== "protocol-error" || operation.nul !== "forbidden") fail(type.name, operation, "text policy");
      } else if (operation.op === "struct") {
        if (operation.order !== "sequential") fail(type.name, operation, "struct order");
        operation.fields.forEach(field => inspectField(type.name, field));
      } else if (operation.op === "versioned-length-delimited") {
        if (operation.reader.op !== "bounded-reader" || operation.reader.trailingBytes !== "forbidden") fail(type.name, operation, "bounded reader");
        operation.fields.forEach(field => inspectField(type.name, field));
      } else if (operation.op === "versioned-vector") {
        if (operation.order !== "sequential" || operation.trailingBytes !== "forbidden") fail(type.name, operation, "versioned vector policy");
      } else if (operation.op === "conditional-union") {
        if (operation.discriminators.some(discriminator => !new Set(["wire", "context", "enclosingField"]).has(discriminator.source.kind))) fail(type.name, operation, "discriminator source");
        for (const selected of Object.values(operation.cases)) selected.fields.forEach(field => inspectField(type.name, field));
        if (operation.otherwise.kind === "fields") operation.otherwise.fields.forEach(field => inspectField(type.name, field));
      } else if (operation.op === "tlv32") {
        if (operation.encodingOrder !== "ascending-field-id" || operation.duplicateField !== "protocol-error" || operation.unknownField !== "skip-after-length-validation") fail(type.name, operation, "TLV policy");
      } else if (operation.op !== "vector" && operation.op !== "runtime-predicate") fail(type.name, operation, "operation");
    }
  }
  for (const command of ir.commands) {
    for (const operation of command.operations) {
      if (operation.op === "command-header" && operation.byteOrder !== "big-endian") fail(command.name, operation, "byte order");
      else if (operation.op === "flags" && operation.unknown !== "protocol-error") fail(command.name, operation, "flag policy");
      else if (operation.op === "flag-constraint" && !new Set(["all-or-none", "implies"]).has(operation.kind)) fail(command.name, operation, "flag constraint");
      else if (operation.op === "metadata-flag-frame" && (operation.whenSet !== "frame-required" || operation.whenClear !== "frame-forbidden")) fail(command.name, operation, "metadata frame policy");
      else if (operation.op === "payload" && !new Set(["forbidden", "optional", "required"]).has(operation.policy)) fail(command.name, operation, "payload policy");
      else if (operation.op === "field") inspectField(command.name, operation);
      else if (!new Set(["command-header", "flags", "flag-constraint", "metadata-flag-frame", "payload", "runtime-predicate"]).has(operation.op)) fail(command.name, operation, "operation");
    }
  }
  for (const format of ir.durableFormats) {
    const header = op(format, "durable-header"), bounded = op(format, "bounded-reader"), checksum = op(format, "checksum");
    if (header.byteOrder !== "big-endian" || header.flagsComparison !== "exact") fail(format.name, header, "header policy");
    if (bounded.boundary !== "bodyLength" || bounded.trailingBytes !== "checksum-only") fail(format.name, bounded, "bounded reader");
    if (checksum.algorithm !== "crc32c-castagnoli" || checksum.encoding !== "u32-big-endian" || checksum.coverage !== "magic-through-body" || checksum.position !== "trailing" || checksum.mismatch !== "protocol-error") fail(format.name, checksum, "checksum policy");
  }
  const logical = op(ir.relocationLogicalStreamFormat, "logical-stream");
  if (logical.encoding !== "canonical-big-endian-field-stream-without-monolithic-provider-envelope") fail(ir.relocationLogicalStreamFormat.name, logical, "logical encoding");
}

function commandDecl(c){const fs=c.operations.filter(x=>x.op==="field"),p=op(c,"payload"),a=["byte Flags",...fs.map(f=>`${typeOf(f.type,!!f.when)} ${id(f.name)}`)];const m=c.operations.find(x=>x.op==="metadata-flag-frame");if(m)a.push(`${typeOf(m.frame,true)} Metadata`);if(p.policy!=="forbidden")a.push(`${typeOf(p.type,p.policy==="optional")} Payload`);return`internal sealed record ${id(c.name)}${c.id}(${a.join(",")});`;}
function flagChecks(c,x){const f=op(c,"flags"),allow=f.allowed.reduce((a,v)=>a|flagBits.get(v.name),0),req=f.required.reduce((a,v)=>a|flagBits.get(v.name),0),r=[`if((${x}&~${allow})!=0||(${x}&${req})!=${req})throw Error("flags");`];for(const q of c.operations.filter(v=>v.op==="flag-constraint")){seen.add(q.op);const bs=(q.flags??[]).map(v=>flagBits.get(v.name));r.push(q.kind==="all-or-none"?`var fc=${bs.map(b=>`((${x}&${b})!=0?1:0)`).join("+")};if(fc!=0&&fc!=${bs.length})throw Error("flag constraint");`:`if((${x}&${flagBits.get(q.if.name)})!=0&&!(${q.then.map(v=>`(${x}&${flagBits.get(v.name)})!=0`).join("&&")}))throw Error("flag implication");`);}return r;}
function commandMethods(c){const n=`${id(c.name)}${c.id}`,h=op(c,"command-header"),fs=c.operations.filter(x=>x.op==="field"),p=op(c,"payload"),m=c.operations.find(x=>x.op==="metadata-flag-frame"),rf=readFields(fs,"body",new Map(),"flags"),wf=writeFields(fs,"body","value",new Map(),"value.Flags"),ctor=["flags",...fs.map(f=>id(f.name))],r=["if(frames.Count==0)throw Error(\"frames\");","var body=new Reader(frames[0]);",...h.magic.map(v=>`if(body.U8()!=${v})throw Error("magic");`),`if(body.U8()!=${h.wireMajor}||body.U8()!=${h.commandId})throw Error("header");`,`var flags=body.U8();`,...flagChecks(c,"flags"),...rf.lines,"body.End(\"body\");","var index=1;"],w=[...flagChecks(c,"value.Flags"),"var body=new Writer();",...h.magic.map(v=>`body.U8(${v});`),`body.U8(${h.wireMajor});body.U8(${h.commandId});body.U8(value.Flags);`,...wf.lines,"var frames=new List<byte[]>{body.ToArray()};"];if(m){seen.add(m.op);const b=flagBits.get(m.flag.name);r.push(`${typeOf(m.frame,true)} metadata=null;`,`if((flags&${b})!=0){if(index>=frames.Count)throw Error("metadata required");var x=new Reader(frames[index++]);metadata=${readCall(m.frame,"x",new Map())};x.End("metadata");}`);ctor.push("metadata");w.push(`if((value.Flags&${b})!=0){if(value.Metadata is null)throw Error("metadata required");var x=new Writer();${writeCall(m.frame,"x","value.Metadata",new Map())};frames.Add(x.ToArray());}else if(value.Metadata is not null)throw Error("metadata forbidden");`);}if(p.policy!=="forbidden"){r.push(`${typeOf(p.type,true)} payload=null;`,p.policy==="required"?`if(index>=frames.Count)throw Error("payload required");`:"",`if(index<frames.Count){var x=new Reader(frames[index++]);payload=${readCall(p.type,"x",new Map())};x.End("payload");}`);ctor.push(`payload${p.policy==="required"?"!":""}`);w.push(p.policy==="required"?`if(value.Payload is null)throw Error("payload required");`:"",`if(value.Payload is not null){var x=new Writer();${writeCall(p.type,"x","value.Payload",new Map())};frames.Add(x.ToArray());}`);}r.push("if(index!=frames.Count)throw Error(\"extra frame\");",`var value=new ${n}(${ctor.join(",")});`,...predicates(c,"value"),"return value;");w.push(...predicates(c,"value"),"return frames.ToArray();");return`    internal static ${n} Decode${n}(IReadOnlyList<byte[]> frames,DecodeContext context)\n    {\n        ${r.filter(Boolean).join("\n        ")}\n    }\n    internal static ${n} Decode${n}(byte[] frame,DecodeContext context)=>Decode${n}(new[]{frame},context);\n    internal static byte[][] Encode${n}(${n} value,DecodeContext context)\n    {\n        ${w.filter(Boolean).join("\n        ")}\n    }`;}

function durable(f){const h=op(f,"durable-header"),br=op(f,"bounded-reader"),cs=op(f,"checksum"),l=op(f,"encoded-limit");seen.add(br.op);seen.add(cs.op);const n=id(f.name),b=rn(h.body);return`    internal static ${b} DecodeDurable${n}(byte[] bytes,DecodeContext context){if(bytes.LongLength>${l.maximumEncodedBytes}L)throw Error("limit");var r=new Reader(bytes);${h.magic.map(v=>`if(r.U8()!=${v})throw Error("magic");`).join("")}if(r.U8()!=${h.formatVersion})throw Error("version");if(Read${rn(h.flagsType)}(r,context).Value!=${h.flags})throw Error("flags");var x=r.Slice(checked((int)Read${rn(h.bodyLengthType)}(r,context).Value));var v=Read${b}(x,context);x.End("body");var sum=r.U32();r.End("durable");if(Crc32C(bytes.AsSpan(0,bytes.Length-4))!=sum)throw Error("checksum");return v;}\n    internal static byte[] EncodeDurable${n}(${b} value,DecodeContext context){var body=new Writer();Write${b}(body,value,context);var w=new Writer();${h.magic.map(v=>`w.U8(${v});`).join("")}w.U8(${h.formatVersion});Write${rn(h.flagsType)}(w,new ${rn(h.flagsType)}(${h.flags}),context);Write${rn(h.bodyLengthType)}(w,new ${rn(h.bodyLengthType)}(checked((${intType(main(types.get(h.bodyLengthType.$ref)).encoding)})body.Length)),context);w.Bytes(body.ToArray());var p=w.ToArray();w.U32(Crc32C(p));var r=w.ToArray();if(r.LongLength>${l.maximumEncodedBytes}L)throw Error("limit");return r;}`;}
const runtime=String.raw`    private sealed class Reader(byte[] b){private int p;internal int Remaining=>b.Length-p;private void N(int n){if(n<0||Remaining<n)throw new EndOfStreamException("truncated");}internal byte U8(){N(1);return b[p++];}internal ushort U16(){N(2);var v=BinaryPrimitives.ReadUInt16BigEndian(b.AsSpan(p));p+=2;return v;}internal uint U32(){N(4);var v=BinaryPrimitives.ReadUInt32BigEndian(b.AsSpan(p));p+=4;return v;}internal ulong U64(){N(8);var v=BinaryPrimitives.ReadUInt64BigEndian(b.AsSpan(p));p+=8;return v;}internal long I64(){N(8);var v=BinaryPrimitives.ReadInt64BigEndian(b.AsSpan(p));p+=8;return v;}internal byte[] Bytes(int n){N(n);var v=b.AsSpan(p,n).ToArray();p+=n;return v;}internal Reader Slice(int n)=>new(Bytes(n));internal void End(string n){if(Remaining!=0)throw Error(n+": trailing");}} private sealed class Writer{private readonly MemoryStream s=new();internal long Length=>s.Length;internal void U8(byte v)=>s.WriteByte(v);internal void U16(ushort v){Span<byte>x=stackalloc byte[2];BinaryPrimitives.WriteUInt16BigEndian(x,v);s.Write(x);}internal void U32(uint v){Span<byte>x=stackalloc byte[4];BinaryPrimitives.WriteUInt32BigEndian(x,v);s.Write(x);}internal void U64(ulong v){Span<byte>x=stackalloc byte[8];BinaryPrimitives.WriteUInt64BigEndian(x,v);s.Write(x);}internal void I64(long v){Span<byte>x=stackalloc byte[8];BinaryPrimitives.WriteInt64BigEndian(x,v);s.Write(x);}internal void Bytes(byte[]v)=>s.Write(v);internal byte[]ToArray()=>s.ToArray();} private static readonly UTF8Encoding Utf8=new(false,true);private static InvalidDataException Error(string m)=>new(m);private static int CompareBytes(byte[]a,byte[]b){var n=Math.Min(a.Length,b.Length);for(var i=0;i<n;i++){var c=a[i].CompareTo(b[i]);if(c!=0)return c;}return a.Length.CompareTo(b.Length);}private static uint Crc32C(ReadOnlySpan<byte>b){var c=uint.MaxValue;foreach(var v in b){c^=v;for(var i=0;i<8;i++)c=(c>>1)^(0x82f63b78u&(uint)-(int)(c&1));}return~c;}
    internal static void ValidateTerminalFailure(RequestTerminalResult result, FrameworkErrorCode failure){if(!ServiceWireConstants.ValidTerminalFailure((uint)result,(uint)failure))throw Error("terminal failure integrity");}`;
function render(){assertSyntaxCoverage();const ctx=`internal sealed record DecodeContext(${ir.semanticContexts.map(x=>`${x.valueType?typeOf(x.valueType,true):"bool?"} ${id(x.name)}`).join(",")}){internal static readonly DecodeContext Empty=new(${ir.semanticContexts.map(()=>"null").join(",")});}`;const lo=ir.relocationLogicalStreamFormat,lop=op(lo,"logical-stream"),ll=op(lo,"encoded-limit"),ln=id(lo.name),lb=rn(lop.body),logical=`    internal static ${lb} DecodeLogical${ln}(byte[] b,DecodeContext c){if(b.LongLength>${ll.maximumEncodedBytes}L)throw Error("limit");return Decode${lb}(b,c);}\n    internal static byte[] EncodeLogical${ln}(${lb} v,DecodeContext c){var b=Encode${lb}(v,c);if(b.LongLength>${ll.maximumEncodedBytes}L)throw Error("limit");return b;}`;const source=`// <auto-generated> DO NOT EDIT.\n#nullable enable\nusing System;\nusing System.Buffers.Binary;\nusing System.Collections.Generic;\nusing System.IO;\nusing System.Linq;\nusing System.Text;\nnamespace Systems.Zlink.Framework.Runtime.Protocol;\ninternal static class ServiceWireCodec\n{\n${[ctx,...ir.types.map(declaration),...ir.commands.map(commandDecl)].map(x=>x.split("\n").map(y=>`    ${y}`).join("\n")).join("\n\n")}\n${runtime}\n${ir.types.map(typeMethods).join("\n")}${ir.commands.map(commandMethods).join("\n")}${ir.durableFormats.map(durable).join("\n")}${logical}\n}\n`;const missing=ir.operationVocabulary.filter(x=>!seen.has(x));if(missing.length)throw Error(`unimplemented operations: ${missing.join(",")}`);return source;}
process.nextTick(()=>{try{const source=render();if(mode==="--write"){fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,source);}else if(!fs.existsSync(output)||fs.readFileSync(output,"utf8")!==source){console.error(`stale output: ${output}`);process.exitCode=1;}}catch(error){console.error(error.stack??String(error));process.exitCode=1;}});
function typeMethods(t){const o=main(t),n=id(t.name),ext=external({$ref:t.name}),params=ext.map(d=>`, ${typeOf(d.type)} enclosing${id(d.source.name)}`).join(""),args=ext.map(d=>`, enclosing${id(d.source.name)}`).join(""),c=constraints(t),pred=predicates(t,"value");let read=[],write=[];
if(o.op==="integer"){read=[`var raw = reader.${o.encoding.toUpperCase()}();`,`if (raw < ${lit(o.minimum,o.encoding)} || raw > ${lit(o.maximum,o.encoding)}) throw Error("${t.name}: range");`,`return new(raw);`];write=[`if (value.Value < ${lit(o.minimum,o.encoding)} || value.Value > ${lit(o.maximum,o.encoding)}) throw Error("${t.name}: range");`,`writer.${o.encoding.toUpperCase()}(value.Value);`];}
else if(o.op==="enum"){const vals=o.values.map(v=>`${lit(v.value,o.encoding)} => ${n}.${id(v.name)}`).join(", ");read=[`var raw = reader.${o.encoding.toUpperCase()}();`,`return raw switch { ${vals}, _ => throw Error("${t.name}: unknown enum") };`];write=[`if (!Enum.IsDefined(value)) throw Error("${t.name}: unknown enum");`,`writer.${o.encoding.toUpperCase()}((${intType(o.encoding)})value);`];}
else if(o.op==="length-prefixed") {
  const text=t.operations.find(x=>x.op==="text-validation");if(text)seen.add(text.op);const lr=o.lengthType;
  read=[`var length = checked((int)Read${rn(lr)}(reader, context).Value);`,`if ((ulong)length < ${lit(o.minimumBytes)} || (ulong)length > ${lit(o.maximumBytes)}) throw Error("${t.name}: length");`,`var bytes = reader.Bytes(length);`,o.zeroLengthMeaning?`if (length == 0) return new(null);`:"",o.content==="text"?`if (bytes.Contains((byte)0)) throw Error("${t.name}: NUL");\n        return new(Utf8.GetString(bytes));`:`return new(bytes);`].filter(Boolean);
  write=[`var bytes = ${o.content==="text"?"value.Value is null ? Array.Empty<byte>() : Utf8.GetBytes(value.Value)":"value.Value ?? Array.Empty<byte>()"};`,o.content==="text"?`if (bytes.Contains((byte)0)) throw Error("${t.name}: NUL");`:"",`if ((ulong)bytes.Length < ${lit(o.minimumBytes)} || (ulong)bytes.Length > ${lit(o.maximumBytes)}) throw Error("${t.name}: length");`,`Write${rn(lr)}(writer, new ${rn(lr)}(checked((${intType(main(types.get(lr.$ref)).encoding)})bytes.Length)), context);`,`writer.Bytes(bytes);`].filter(Boolean);
}
else if(o.op==="struct"||o.op==="versioned-length-delimited"){const target=o.op==="versioned-length-delimited"?"body":"reader";if(o.reader){seen.add(o.reader.op);read.push(`var version = ${readCall(o.version,"reader",new Map())};`,`if (version != ${expected(o.version,o.version.constant)}) throw Error("${t.name}: version");`,`var body = reader.Slice(checked((int)${readCall(o.length,"reader",new Map())}.Value));`);}const rf=readFields(o.fields,target);read.push(...rf.lines,`var value = new ${n}(${rf.args});`,...c.lines,...pred, ...(o.reader?[`body.End("${t.name}");`]:[]),"return value;");const wf=writeFields(o.fields,"body","value");write.push(...c.lines,...pred);if(o.op==="versioned-length-delimited"){write.push("var body = new Writer();",...wf.lines,`${writeCall(o.version,"writer",expected(o.version,o.version.constant),new Map())};`,`Write${rn(o.length)}(writer, new ${rn(o.length)}(checked((${intType(main(types.get(o.length.$ref)).encoding)})body.Length)), context);`,`writer.Bytes(body.ToArray());`);}else write.push(...writeFields(o.fields,"writer","value").lines);}
else if(o.op==="vector"||o.op==="versioned-vector"){let item,count,prop;if(o.op==="vector"){item=o.item;count=o.countType;prop="Items";}else{const v=o.layout[0],ct=o.layout[1],r=o.layout.find(x=>x.kind==="repeat");item=r.item;count={$ref:ct.$ref};prop=id(r.name);read.push(`var version = ${readCall(v,"reader",new Map())};`,`if (version != ${expected(v,v.constant)}) throw Error("${t.name}: version");`);}read.push(`var count = checked((int)${readCall(count,"reader",new Map())}.Value);`,...(o.maximumItems!==undefined?[`if (count > ${o.maximumItems}) throw Error("${t.name}: count");`]:[]),`var items = new ${typeOf(item)}[count];`,`for (var i = 0; i < count; ++i) items[i] = ${readCall(item,"reader",new Map())};`,`var value = new ${n}(items);`,...c.lines,"return value;");write.push(...c.lines,...(o.maximumItems!==undefined?[`if (value.${prop}.Count > ${o.maximumItems}) throw Error("${t.name}: count");`]:[]));if(o.op==="versioned-vector"){const v=o.layout[0];write.push(`${writeCall(v,"writer",expected(v,v.constant),new Map())};`);}write.push(`Write${rn(count)}(writer, new ${rn(count)}(checked((${intType(main(types.get(count.$ref)).encoding)})value.${prop}.Count)), context);`,`foreach (var item in value.${prop}) ${writeCall(item,"writer","item",new Map())};`);}
else if(o.op==="conditional-union") {
  const disc=o.discriminators;
  for(const d of disc){seen.add(d.op);if(d.source.kind==="wire")read.push(`var ${id(d.name)} = ${readCall(d.type,"reader",new Map())};`);else if(d.source.kind==="context")read.push(`var ${id(d.name)} = context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}");`);else read.push(`var ${id(d.name)} = enclosing${id(d.source.name)};`);}
  if(o.reader){seen.add(o.reader.op);read.push(`var selected = reader.Slice(checked((int)${readCall(o.bodyLengthType,"reader",new Map())}.Value));`);}else read.push("var selected = reader;");
  cases(o).forEach(([sig,v],i)=>{const s=JSON.parse(sig),test=disc.map(d=>`${id(d.name)} == ${expected(d.type,s[d.name])}`).join(" && "),rf=readFields(v.fields,"selected");read.push(`${i?"else ":""}if (${test})`,"{",...rf.lines.map(x=>`    ${x}`),...(o.reader?[`    selected.End("${t.name}");`]:[]),`    ${n} value = new ${n}Case${i}(${rf.args});`,...pred.map(x=>`    ${x}`),"    return value;","}");});
  if(o.otherwise.kind==="fields"){const rf=readFields(o.otherwise.fields,"selected");read.push(...rf.lines,...(o.reader?[`selected.End("${t.name}");`]:[]),`return new ${n}Otherwise(${rf.args});`);}else read.push(`throw Error("${t.name}: discriminator");`);
  write.push("switch (value)","{");
  cases(o).forEach(([sig,v],i)=>{const s=JSON.parse(sig);write.push(`case ${n}Case${i} item:`,"{");for(const d of disc){if(d.source.kind==="wire")write.push(`    ${writeCall(d.type,"writer",expected(d.type,s[d.name]),new Map())};`);else{const actual=d.source.kind==="context"?`(context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}"))`:`enclosing${id(d.source.name)}`;write.push(`    if (${actual} != ${expected(d.type,s[d.name])}) throw Error("${t.name}: discriminator");`);}}const wf=writeFields(v.fields,o.reader?"body":"writer","item");if(o.reader)write.push("    var body = new Writer();",...wf.lines.map(x=>`    ${x}`),`    Write${rn(o.bodyLengthType)}(writer, new ${rn(o.bodyLengthType)}(checked((${intType(main(types.get(o.bodyLengthType.$ref)).encoding)})body.Length)), context);`,"    writer.Bytes(body.ToArray());");else write.push(...wf.lines.map(x=>`    ${x}`));write.push("    break;","}");});
  if(o.otherwise.kind==="fields"){
    if(disc.some(d=>d.source.kind==="wire"))throw Error(`${t.name}: wire discriminator otherwise lacks a value`);
    write.push(`case ${n}Otherwise item:`,"{");
    const actuals=disc.map(d=>d.source.kind==="context"?`(context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}"))`:`enclosing${id(d.source.name)}`);
    for(const [sig] of cases(o)){const s=JSON.parse(sig);write.push(`    if (${disc.map((d,i)=>`${actuals[i]} == ${expected(d.type,s[d.name])}`).join(" && ")}) throw Error("${t.name}: otherwise matches case");`);}
    const wf=writeFields(o.otherwise.fields,o.reader?"body":"writer","item");if(o.reader)write.push("    var body = new Writer();",...wf.lines.map(x=>`    ${x}`),`    Write${rn(o.bodyLengthType)}(writer, new ${rn(o.bodyLengthType)}(checked((${intType(main(types.get(o.bodyLengthType.$ref)).encoding)})body.Length)), context);`,"    writer.Bytes(body.ToArray());");else write.push(...wf.lines.map(x=>`    ${x}`));write.push("    break;","}");
  }
  write.push(`default: throw Error("${t.name}: variant");`,"}",...pred);
}
else if(o.op==="tlv32") {
  seen.add(o.reader.op);
  if (o.encodingOrder !== "ascending-field-id" || o.duplicateField !== "protocol-error" || o.unknownField !== "skip-after-length-validation") throw Error(`${t.name}: unsupported TLV policy`);
  const ordered = [...o.fields].sort((left, right) => left.id - right.id);
  const readEnvironment = new Map(o.fields.flatMap(f => [[f.name, id(f.name)], [`${f.name}:field`, f]]));
  read.push(`var body = reader.Slice(checked((int)${readCall(o.totalLengthType,"reader",new Map())}.Value));`, ...o.fields.map(f=>`${typeOf(f.type,true)} ${id(f.name)} = null;`), "var previous = -1;", "while (body.Remaining > 0)", "{", `    var fieldId = checked((int)${readCall(o.fieldIdType,"body",new Map())}.Value);`, `    if (fieldId <= previous) throw Error("${t.name}: TLV order");`, "    previous = fieldId;", `    var item = body.Slice(checked((int)${readCall(o.fieldLengthType,"body",new Map())}.Value));`, "    switch (fieldId)", "    {");
  for (const f of ordered) read.push(`    case ${f.id}:`, `        ${id(f.name)} = ${readCall(f.type,"item",new Map())};`, ...fieldChecks(f, `${id(f.name)}!`).map(line => `        ${line}`), `        item.End("${f.name}");`, "        break;");
  read.push("    default:", "        item.Bytes(item.Remaining);", "        break;", "    }", "}");
  for (const f of o.fields.filter(x=>x.required)) read.push(`if (${id(f.name)} is null) throw Error("${f.name}: required");`);
  read.push(...tlvPresence(o, readEnvironment), `var value = new ${n}(${o.fields.map(f=>`${id(f.name)}${f.required?"!":""}`).join(", ")});`, "return value;");
  const writeEnvironment = new Map(o.fields.flatMap(f => [[f.name, `value.${id(f.name)}`], [`${f.name}:field`, f]]));
  write.push(...tlvPresence(o, writeEnvironment), "var body = new Writer();");
  for (const f of ordered) {
    const x=`value.${id(f.name)}`, v=main(types.get(f.type.$ref)).op==="enum"?`${x}.Value`:`${x}!`;
    if(f.required) write.push(`if (${x} is null) throw Error("${f.name}: required");`);
    write.push(`if (${x} is not null)`, "{", ...fieldChecks(f, v).map(line => `    ${line}`), "    var item = new Writer();", `    ${writeCall(f.type,"item",v,new Map())};`, `    Write${rn(o.fieldIdType)}(body, new ${rn(o.fieldIdType)}(${lit(f.id,main(types.get(o.fieldIdType.$ref)).encoding)}), context);`, `    Write${rn(o.fieldLengthType)}(body, new ${rn(o.fieldLengthType)}(checked((${intType(main(types.get(o.fieldLengthType.$ref)).encoding)})item.Length)), context);`, "    body.Bytes(item.ToArray());", "}");
  }
  write.push(`Write${rn(o.totalLengthType)}(writer, new ${rn(o.totalLengthType)}(checked((${intType(main(types.get(o.totalLengthType.$ref)).encoding)})body.Length)), context);`, "writer.Bytes(body.ToArray());");
}
else throw Error(`${t.name}: methods ${o.op}`);
const max=o.maximumEncodedBytes;
const limitCheck = max === undefined ? null : `if ((ulong)(encodedStart - reader.Remaining) > ${lit(max)}) throw Error("${t.name}: encoded limit");`;
const readBody=max===undefined?read:["var encodedStart = reader.Remaining;",...read.map(line=>line.replace(/return /g,`${limitCheck} return `))];
const writeBody=max===undefined?write:["var encodedStart = writer.Length;",...write,`if ((ulong)(writer.Length - encodedStart) > ${lit(max)}) throw Error("${t.name}: encoded limit");`];
return`    internal static ${n} Decode${n}(byte[] bytes, DecodeContext context${params}) { var reader = new Reader(bytes); var value = Read${n}(reader, context${args}); reader.End("${t.name}"); return value; }\n    internal static byte[] Encode${n}(${n} value, DecodeContext context${params}) { var writer = new Writer(); Write${n}(writer, value, context${args}); return writer.ToArray(); }\n    private static ${n} Read${n}(Reader reader, DecodeContext context${params})\n    {\n        ${readBody.join("\n        ")}\n    }\n    private static void Write${n}(Writer writer, ${n} value, DecodeContext context${params})\n    {\n        ${writeBody.join("\n        ")}\n    }\n${c.helpers.join("\n")}`;}

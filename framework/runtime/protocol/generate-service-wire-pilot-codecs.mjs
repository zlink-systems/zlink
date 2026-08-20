#!/usr/bin/env node
// Generates the deliberately small W-2 codec pilot.  The field order is read
// from the schema; do not duplicate it in a runtime.
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));
const schemaText = fs.readFileSync(path.join(root, "service-wire-v1.schema.json"), "utf8");
const schema = JSON.parse(schemaText);
const hash = crypto.createHash("sha256").update(schemaText).digest("hex");
const check = process.argv.includes("--check");
const actorJoin = schema.commands.find((command) => command.id === 28 && command.name === "actorJoin");
const relocation = schema.relocationLogicalStreamFormat;
const relocationFixture = JSON.parse(fs.readFileSync(
  path.join(root, "golden/relocation-envelope-v1.json"), "utf8",
));
if (!actorJoin || !relocation || relocation.body?.$ref !== "relocation-envelope-v1"
    || relocation.generatedObjectTree?.root?.$ref !== "relocation-envelope-v1"
    || ["applicationStates", "savedWork", "timerRegistrations", "pendingTimerTicks"].some(
      (name) => !relocation.generatedObjectTree[name]?.$ref,
    )
    || actorJoin.body.map((field) => field.$ref).join(",") !== "nonzero-u64,actor-route-fence,bool8,spot-route-fence") {
  throw new Error("service-wire pilot layouts changed; update generator before regenerating");
}

// W-2 mechanical command surface: bodies that use only primitives the pilot
// already supports (nonzero-u64, text8, actor-route-fence). Each is verified
// against the schema body layout below before any code is emitted, so a
// schema change fails generation instead of silently drifting.
const getCommand = (id, name) => {
  const found = schema.commands.find((c) => c.id === id && c.name === name);
  if (!found) throw new Error(`command ${name}(${id}) missing from schema`);
  return found;
};
const assertBody = (cmd, expected) => {
  const actual = cmd.body.map((f) => f.$ref).join(",");
  if (actual !== expected.join(",")) {
    throw new Error(`${cmd.name} body layout changed; update generator before regenerating`);
  }
};
const mechanicalCommands = [
  [5, "livenessProbe", ["nonzero-u64"]],
  [6, "livenessAck", ["nonzero-u64"]],
  [16, "nodeSend", []],
  [17, "nodeRequest", ["nonzero-u64"]],
  [18, "channelSend", ["text8"]],
  [19, "channelRequest", ["nonzero-u64", "text8"]],
  [23, "logicalMulticast", ["text8", "text8", "text8"]],
  [26, "actorLookup", ["nonzero-u64", "text8"]],
  [27, "actorDestroy", ["nonzero-u64", "actor-route-fence"]],
].map(([id, name, expected]) => {
  const cmd = getCommand(id, name);
  assertBody(cmd, expected);
  return cmd;
});
const pascal = (name) => name[0].toUpperCase() + name.slice(1);
const snake = (name) => name.replace(/[A-Z]/g, (c) => `_${c.toLowerCase()}`);
const M0 = schema.protocol.magic[0];
const M1 = schema.protocol.magic[1];
const WM = schema.protocol.wireMajor;

// -- Node/TypeScript ---------------------------------------------------
function nodeField(f) {
  switch (f.$ref) {
    case "nonzero-u64":
      return { ts: "bigint",
        enc: `if(!value.${f.name}) throw new RangeError("${f.name}"); u64(out,value.${f.name});`,
        dec: `const ${f.name}=read64(bytes,at); if(!${f.name}) throw new RangeError("${f.name}");` };
    case "text8":
      return { ts: "string", enc: `text8(out,value.${f.name});`, dec: `const ${f.name}=readText8(bytes,at);` };
    case "actor-route-fence": case "spot-route-fence":
      return { ts: "ServiceWireRouteFence", enc: `fence(out,value.${f.name});`, dec: `const ${f.name}=readFence(bytes,at);` };
    default: throw new Error(`unsupported node field ${f.$ref}`);
  }
}
function nodeCommandCode(cmd) {
  const suffix = `${pascal(cmd.name)}${cmd.id}`;
  const fields = cmd.body.map((f) => ({ ...f, ...nodeField(f) }));
  const hasFields = fields.length > 0;
  const iface = hasFields
    ? `export interface ${suffix} { ${fields.map((f) => `readonly ${f.name}: ${f.ts};`).join(" ")} }\n` : "";
  const param = hasFields ? `value: ${suffix}` : "";
  const returnType = hasFields ? suffix : "void";
  const returnExpr = hasFields ? `{ ${fields.map((f) => f.name).join(", ")} }` : "undefined";
  return `${iface}export function encode${suffix}(${param}): Uint8Array { const out=[${M0},${M1},${WM},${cmd.id},0]; ${fields.map((f) => f.enc).join(" ")} return Uint8Array.from(out); }
export function decode${suffix}(bytes: Uint8Array): ${returnType} { const at={value:0}; if(bytes.length<5 || bytes[at.value++]!==${M0} || bytes[at.value++]!==${M1} || bytes[at.value++]!==${WM} || bytes[at.value++]!==${cmd.id} || bytes[at.value++]!==0) throw new RangeError("${cmd.name} header"); ${fields.map((f) => f.dec).join(" ")} if(at.value!==bytes.length) throw new RangeError("${cmd.name} trailing"); return ${returnExpr}; }
`;
}
const nodeMechanical = mechanicalCommands.map(nodeCommandCode).join("");

// -- JVM/Java ------------------------------------------------------------
function javaField(f) {
  switch (f.$ref) {
    case "nonzero-u64":
      return { type: "long",
        enc: `if(v.${f.name}()==0)throw new IOException("${f.name}");u64(o,v.${f.name}());`,
        dec: `long ${f.name}=r64(i);if(${f.name}==0)throw new IOException("${f.name}");` };
    case "text8":
      return { type: "String", enc: `text8(o,v.${f.name}());`, dec: `String ${f.name}=text8(i);` };
    case "actor-route-fence": case "spot-route-fence":
      return { type: "Fence", enc: `fence(o,v.${f.name}());`, dec: `Fence ${f.name}=fence(i);` };
    default: throw new Error(`unsupported java field ${f.$ref}`);
  }
}
function javaCommandCode(cmd) {
  const suffix = `${pascal(cmd.name)}${cmd.id}`;
  const fields = cmd.body.map((f) => ({ ...f, ...javaField(f) }));
  const hasFields = fields.length > 0;
  const recordDecl = hasFields
    ? `  public record ${suffix}(${fields.map((f) => `${f.type} ${f.name}`).join(",")}) {}\n` : "";
  const param = hasFields ? `${suffix} v` : "";
  const decodeReturnType = hasFields ? suffix : "void";
  const trailingCheck = `if(i.available()!=0)throw new IOException("trailing");`;
  const decodeBody = hasFields
    ? `${fields.map((f) => f.dec).join("")}${trailingCheck}return new ${suffix}(${fields.map((f) => f.name).join(",")});`
    : `${trailingCheck}`;
  return `${recordDecl}  public static byte[] encode${suffix}(${param})throws IOException{var b=new ByteArrayOutputStream();var o=new DataOutputStream(b);o.write(new byte[]{${M0},${M1},${WM},${cmd.id},0});${fields.map((f) => f.enc).join("")}return b.toByteArray();}
  public static ${decodeReturnType} decode${suffix}(byte[] b)throws IOException{var i=new DataInputStream(new ByteArrayInputStream(b));if(i.readUnsignedByte()!=${M0}||i.readUnsignedByte()!=${M1}||i.readUnsignedByte()!=${WM}||i.readUnsignedByte()!=${cmd.id}||i.readUnsignedByte()!=0)throw new IOException("header");${decodeBody}}
`;
}
const javaMechanical = mechanicalCommands.map(javaCommandCode).join("");

// -- .NET/C# ---------------------------------------------------------
function dotnetField(f) {
  const prop = pascal(f.name);
  switch (f.$ref) {
    case "nonzero-u64":
      return { type: "ulong", prop,
        enc: `if(v.${prop}==0)throw new InvalidDataException();U64(w,v.${prop});`,
        dec: `var ${f.name}=R64(r);if(${f.name}==0)throw new InvalidDataException();` };
    case "text8":
      return { type: "string", prop, enc: `T(w,v.${prop});`, dec: `var ${f.name}=T(r);` };
    case "actor-route-fence": case "spot-route-fence":
      return { type: "Fence", prop, enc: `F(w,v.${prop});`, dec: `var ${f.name}=F(r);` };
    default: throw new Error(`unsupported dotnet field ${f.$ref}`);
  }
}
function dotnetCommandCode(cmd) {
  const suffix = `${pascal(cmd.name)}${cmd.id}`;
  const fields = cmd.body.map((f) => ({ ...f, ...dotnetField(f) }));
  const hasFields = fields.length > 0;
  const recordDecl = hasFields
    ? `public sealed record ${suffix}(${fields.map((f) => `${f.type} ${f.prop}`).join(",")}); ` : "";
  const param = hasFields ? `${suffix} v` : "";
  const decodeReturnType = hasFields ? suffix : "void";
  const trailingCheck = `if(r.BaseStream.Position!=r.BaseStream.Length)throw new InvalidDataException();`;
  const decodeBody = hasFields
    ? `${fields.map((f) => f.dec).join("")}${trailingCheck}return new(${fields.map((f) => f.name).join(",")});`
    : `${trailingCheck}`;
  return `${recordDecl}public static byte[] Encode${suffix}(${param}){using var m=new MemoryStream();using var w=new BinaryWriter(m);w.Write(new byte[]{${M0},${M1},${WM},${cmd.id},0});${fields.map((f) => f.enc).join("")}return m.ToArray();} public static ${decodeReturnType} Decode${suffix}(byte[] b){using var r=new BinaryReader(new MemoryStream(b));if(r.ReadByte()!=${M0}||r.ReadByte()!=${M1}||r.ReadByte()!=${WM}||r.ReadByte()!=${cmd.id}||r.ReadByte()!=0)throw new InvalidDataException();${decodeBody}} `;
}
const dotnetMechanical = mechanicalCommands.map(dotnetCommandCode).join("\n");

// -- C++ -------------------------------------------------------------
function cppField(f) {
  switch (f.$ref) {
    case "nonzero-u64":
      return { type: "std::uint64_t",
        enc: `if(!v.${f.name})throw std::invalid_argument("${f.name}");pilot_u64(o,v.${f.name});`,
        dec: `v.${f.name}=pilot_r64(b,at);if(!v.${f.name})throw std::invalid_argument("${f.name}");` };
    case "text8":
      return { type: "std::string", enc: `pilot_text8(o,v.${f.name});`, dec: `v.${f.name}=pilot_read_text8(b,at);` };
    case "actor-route-fence": case "spot-route-fence":
      return { type: "service_wire_pilot_fence", enc: `pilot_write_fence(o,v.${f.name});`, dec: `v.${f.name}=pilot_fence(b,at);` };
    default: throw new Error(`unsupported cpp field ${f.$ref}`);
  }
}
function cppCommandCode(cmd) {
  const base = `service_wire_pilot_${snake(cmd.name)}_${cmd.id}`;
  const fnBase = `${snake(cmd.name)}_${cmd.id}`;
  const fields = cmd.body.map((f) => ({ ...f, ...cppField(f) }));
  const hasFields = fields.length > 0;
  const structDecl = hasFields
    ? `struct ${base} { ${fields.map((f) => `${f.type} ${f.name}${f.type === "std::uint64_t" ? " = 0" : ""};`).join(" ")} }; ` : "";
  const encParam = hasFields ? `const ${base}&v` : "";
  const decRet = hasFields ? base : "void";
  const decBody = hasFields
    ? `${base} v{};${fields.map((f) => f.dec).join("")}if(at!=b.size())throw std::invalid_argument("${cmd.name} trailing");return v;`
    : `if(at!=b.size())throw std::invalid_argument("${cmd.name} trailing");`;
  return `${structDecl}inline std::vector<std::uint8_t> encode_${fnBase}(${encParam}){std::vector<std::uint8_t> o={${M0},${M1},${WM},${cmd.id},0};${fields.map((f) => f.enc).join("")}return o;} inline ${decRet} decode_${fnBase}(const std::vector<std::uint8_t>&b){std::size_t at=0;if(b.size()<5||b[at++]!=${M0}||b[at++]!=${M1}||b[at++]!=${WM}||b[at++]!=${cmd.id}||b[at++]!=0)throw std::invalid_argument("${cmd.name} header");${decBody}} `;
}
const cppMechanical = `inline void pilot_text8(std::vector<std::uint8_t>&o,const std::string&v){if(v.empty()||v.size()>255)throw std::invalid_argument("text8");o.push_back(static_cast<std::uint8_t>(v.size()));o.insert(o.end(),v.begin(),v.end());} inline std::string pilot_read_text8(const std::vector<std::uint8_t>&b,std::size_t&at){if(at>=b.size())throw std::invalid_argument("truncated text8");const auto n=b[at++];if(!n||at+n>b.size())throw std::invalid_argument("invalid text8");std::string s(reinterpret_cast<const char*>(b.data()+at),n);at+=n;return s;} inline void pilot_write_fence(std::vector<std::uint8_t>&o,const service_wire_pilot_fence&x){if(x.id.empty()||x.id.size()>255||x.target_node_rid.empty()||x.target_node_rid.size()>255||!x.generation||!x.target_node_generation||!x.expected_authority_owner_generation||!x.expected_owner_lease_generation)throw std::invalid_argument("fence");o.push_back(static_cast<std::uint8_t>(x.id.size()));o.insert(o.end(),x.id.begin(),x.id.end());pilot_u64(o,x.generation);o.push_back(static_cast<std::uint8_t>(x.target_node_rid.size()));o.insert(o.end(),x.target_node_rid.begin(),x.target_node_rid.end());pilot_u64(o,x.target_node_generation);pilot_u64(o,x.expected_authority_owner_generation);pilot_u64(o,x.expected_owner_lease_generation);} ${mechanicalCommands.map(cppCommandCode).join("")}`;

const banner = (comment) => `${comment} <auto-generated> DO NOT EDIT. schema-sha256: ${hash}\n`;
const node = `${banner("//")}export interface ServiceWireRouteFence { readonly id: string; readonly generation: bigint; readonly targetNodeRid: Uint8Array; readonly targetNodeGeneration: bigint; readonly expectedAuthorityOwnerGeneration: bigint; readonly expectedOwnerLeaseGeneration: bigint; }
export interface ActorJoin28 { readonly correlation: bigint; readonly actor: ServiceWireRouteFence; readonly entry: boolean; readonly targetSpot: ServiceWireRouteFence; }
const text = new TextEncoder(); const decodeText = new TextDecoder("utf-8", { fatal: true });
function u64(out: number[], value: bigint): void { if (value < 0n || value > 0xffffffffffffffffn) throw new RangeError("u64"); for (let i=7;i>=0;i--) out.push(Number((value >> BigInt(i*8)) & 255n)); }
function read64(bytes: Uint8Array, at: { value: number }): bigint { if (at.value + 8 > bytes.length) throw new RangeError("truncated u64"); let r=0n; for(let i=0;i<8;i++) r=(r<<8n)|BigInt(bytes[at.value++]); return r; }
function text8(out: number[], value: string): void { const b=text.encode(value); if(!b.length || b.length>255 || b.includes(0)) throw new RangeError("text8"); out.push(b.length,...b); }
function readText8(bytes: Uint8Array, at: { value: number }): string { const n=bytes[at.value++]; if(!n || at.value+n>bytes.length) throw new RangeError("truncated text8"); const b=bytes.slice(at.value,at.value+n); at.value+=n; return decodeText.decode(b); }
function rid(out: number[], value: Uint8Array): void { if(!value.length || value.length>255) throw new RangeError("rid"); out.push(value.length,...value); }
function readRid(bytes: Uint8Array, at: { value: number }): Uint8Array { const n=bytes[at.value++]; if(!n || at.value+n>bytes.length) throw new RangeError("truncated rid"); const r=bytes.slice(at.value,at.value+n); at.value+=n; return r; }
function fence(out: number[], value: ServiceWireRouteFence): void { text8(out,value.id); u64(out,value.generation); rid(out,value.targetNodeRid); u64(out,value.targetNodeGeneration); u64(out,value.expectedAuthorityOwnerGeneration); u64(out,value.expectedOwnerLeaseGeneration); }
function readFence(bytes: Uint8Array, at: { value: number }): ServiceWireRouteFence { return { id:readText8(bytes,at), generation:read64(bytes,at), targetNodeRid:readRid(bytes,at), targetNodeGeneration:read64(bytes,at), expectedAuthorityOwnerGeneration:read64(bytes,at), expectedOwnerLeaseGeneration:read64(bytes,at) }; }
export function encodeActorJoin28(value: ActorJoin28): Uint8Array { const out=[${schema.protocol.magic.join(",")},${schema.protocol.wireMajor},${actorJoin.id},0]; if(!value.correlation) throw new RangeError("correlation"); u64(out,value.correlation); fence(out,value.actor); out.push(value.entry?1:0); fence(out,value.targetSpot); return Uint8Array.from(out); }
export function decodeActorJoin28(bytes: Uint8Array): ActorJoin28 { const at={value:0}; if(bytes.length<5 || bytes[at.value++]!==${schema.protocol.magic[0]} || bytes[at.value++]!==${schema.protocol.magic[1]} || bytes[at.value++]!==${schema.protocol.wireMajor} || bytes[at.value++]!==${actorJoin.id} || bytes[at.value++]!==0) throw new RangeError("actorJoin header"); const correlation=read64(bytes,at); const actor=readFence(bytes,at); const entry=bytes[at.value++]; const targetSpot=readFence(bytes,at); if(!correlation || entry>1 || at.value!==bytes.length) throw new RangeError("actorJoin body"); return { correlation, actor, entry:entry===1, targetSpot }; }
export interface RelocationEnvelopeV1 { readonly relocationHigh: bigint; readonly relocationLow: bigint; readonly object: { readonly objectKind: "userSpot"; readonly spotId: string; readonly spotGeneration: bigint; readonly expectedAuthorityOwnerGeneration: bigint; }; readonly applicationVersion: bigint; readonly applicationStates: readonly { readonly participantId: bigint; readonly hasState: boolean; readonly payload: Uint8Array; }[]; readonly savedWork: readonly { readonly participantId: bigint; readonly order: bigint; readonly frozenRecord: Uint8Array; }[]; readonly timerRegistrations: readonly { readonly participantId: bigint; readonly name: string; readonly handlerType: string; readonly periodMilliseconds: bigint; readonly overrunPolicy: 1|2|3; readonly maxCatchUpTicks: bigint; readonly stopOnUnhandledException: boolean; readonly lastCompletedDeliveryIndex: bigint; readonly lastCompletedScheduledIndex: bigint; readonly nextScheduledAtUnixMilliseconds: bigint; }[]; readonly pendingTimerTicks: readonly { readonly participantId: bigint; readonly order: bigint; readonly timerName: string; readonly deliveryIndex: bigint; readonly scheduledIndex: bigint; readonly scheduledAtUnixMilliseconds: bigint; readonly skippedTicks: bigint; }[]; }
function u32(out:number[],v:number):void{if(!Number.isInteger(v)||v<0||v>0xffffffff)throw new RangeError("u32");out.push(v>>>24,(v>>>16)&255,(v>>>8)&255,v&255);}
function bytes64(out:number[],v:Uint8Array):void{u64(out,BigInt(v.length));out.push(...v);}
function body16(out:number[],write:(b:number[])=>void):void{const b:number[]=[];write(b);if(b.length>65535)throw new RangeError("body16");out.push(b.length>>>8,b.length&255,...b);}
function body64(out:number[],write:(b:number[])=>void):void{const b:number[]=[];write(b);u64(out,BigInt(b.length));out.push(...b);}
function sorted<T>(v:readonly T[],compare:(a:T,b:T)=>number,name:string):void{for(let i=1;i<v.length;i++)if(compare(v[i-1],v[i])>=0)throw new RangeError(name);}
export function encodeRelocationEnvelopeV1(value: RelocationEnvelopeV1): Uint8Array { const out:number[]=[];if(!value.relocationHigh&&!value.relocationLow)throw new RangeError("relocation");u64(out,value.relocationHigh);u64(out,value.relocationLow);if(value.object.objectKind!=="userSpot")throw new RangeError("objectKind");out.push(2);body16(out,b=>{text8(b,value.object.spotId);u64(b,value.object.spotGeneration);u64(b,value.object.expectedAuthorityOwnerGeneration);});u64(out,value.applicationVersion);sorted(value.applicationStates,(a,b)=>a.participantId<b.participantId?-1:a.participantId>b.participantId?1:0,"applicationStates");u32(out,value.applicationStates.length);for(const s of value.applicationStates){u64(out,s.participantId);out.push(s.hasState?1:0);body64(out,b=>{if(s.hasState)bytes64(b,s.payload);});}sorted(value.savedWork,(a,b)=>a.participantId===b.participantId?(a.order<b.order?-1:a.order>b.order?1:0):(a.participantId<b.participantId?-1:1),"savedWork");u32(out,value.savedWork.length);for(const w of value.savedWork){u64(out,w.participantId);u64(out,w.order);out.push(...w.frozenRecord);}sorted(value.timerRegistrations,(a,b)=>a.participantId===b.participantId?a.name.localeCompare(b.name):a.participantId<b.participantId?-1:1,"timerRegistrations");u32(out,value.timerRegistrations.length);for(const t of value.timerRegistrations){u64(out,t.participantId);text8(out,t.name);text8(out,t.handlerType);u64(out,t.periodMilliseconds);out.push(t.overrunPolicy);u64(out,t.maxCatchUpTicks);out.push(t.stopOnUnhandledException?1:0);u64(out,t.lastCompletedDeliveryIndex);u64(out,t.lastCompletedScheduledIndex);u64(out,t.nextScheduledAtUnixMilliseconds);}sorted(value.pendingTimerTicks,(a,b)=>a.participantId===b.participantId?(a.order<b.order?-1:a.order>b.order?1:0):(a.participantId<b.participantId?-1:1),"pendingTimerTicks");u32(out,value.pendingTimerTicks.length);for(const p of value.pendingTimerTicks){u64(out,p.participantId);u64(out,p.order);text8(out,p.timerName);u64(out,p.deliveryIndex);u64(out,p.scheduledIndex);u64(out,p.scheduledAtUnixMilliseconds);u64(out,p.skippedTicks);}if(out.length>${schema.bounds.find((x)=>x.name==="relocationLogicalBytes").value})throw new RangeError("relocation logical bytes");return Uint8Array.from(out); }
export function decodeRelocationEnvelopeV1(chunks: readonly Uint8Array[]): Uint8Array { const n=chunks.reduce((s,c)=>s+c.length,0); if(n>${schema.bounds.find((x)=>x.name==="relocationLogicalBytes").value}) throw new RangeError("relocation logical bytes"); const out=new Uint8Array(n); let at=0; for(const c of chunks){out.set(c,at);at+=c.length;} return out; }
`;

const java = `${banner("//")}package systems.zlink.framework.runtime.protocol;\nimport java.io.*; import java.util.*;\npublic final class ServiceWirePilotCodec {\n  public record Fence(String id,long generation,byte[] targetNodeRid,long targetNodeGeneration,long expectedAuthorityOwnerGeneration,long expectedOwnerLeaseGeneration) {}\n  public record ActorJoin28(long correlation,Fence actor,boolean entry,Fence targetSpot) {}\n  private static void u64(DataOutputStream o,long v)throws IOException{o.writeLong(v);} private static long r64(DataInputStream i)throws IOException{return i.readLong();}\n  private static void text8(DataOutputStream o,String s)throws IOException{byte[] b=s.getBytes(java.nio.charset.StandardCharsets.UTF_8);if(b.length==0||b.length>255)throw new IOException("text8");o.writeByte(b.length);o.write(b);}\n  private static String text8(DataInputStream i)throws IOException{int n=i.readUnsignedByte();byte[] b=i.readNBytes(n);if(n==0||b.length!=n)throw new EOFException();return new String(b,java.nio.charset.StandardCharsets.UTF_8);}\n  private static void fence(DataOutputStream o,Fence v)throws IOException{text8(o,v.id);u64(o,v.generation);o.writeByte(v.targetNodeRid.length);o.write(v.targetNodeRid);u64(o,v.targetNodeGeneration);u64(o,v.expectedAuthorityOwnerGeneration);u64(o,v.expectedOwnerLeaseGeneration);}\n  private static Fence fence(DataInputStream i)throws IOException{String id=text8(i);long g=r64(i);int n=i.readUnsignedByte();byte[] rid=i.readNBytes(n);if(n==0||rid.length!=n)throw new EOFException();return new Fence(id,g,rid,r64(i),r64(i),r64(i));}\n  public static byte[] encodeActorJoin28(ActorJoin28 v)throws IOException{if(v.correlation==0)throw new IOException("correlation");var b=new ByteArrayOutputStream();var o=new DataOutputStream(b);o.write(new byte[]{${schema.protocol.magic.join(",")},${schema.protocol.wireMajor},${actorJoin.id},0});u64(o,v.correlation);fence(o,v.actor);o.writeByte(v.entry?1:0);fence(o,v.targetSpot);return b.toByteArray();}\n  public static ActorJoin28 decodeActorJoin28(byte[] b)throws IOException{var i=new DataInputStream(new ByteArrayInputStream(b));if(i.readUnsignedByte()!=${schema.protocol.magic[0]}||i.readUnsignedByte()!=${schema.protocol.magic[1]}||i.readUnsignedByte()!=${schema.protocol.wireMajor}||i.readUnsignedByte()!=${actorJoin.id}||i.readUnsignedByte()!=0)throw new IOException("header");long c=r64(i);Fence a=fence(i);int e=i.readUnsignedByte();Fence s=fence(i);if(c==0||e>1||i.available()!=0)throw new IOException("body");return new ActorJoin28(c,a,e==1,s);}\n  public static byte[] encodeRelocationEnvelopeV1(byte[] logical){return logical.clone();} public static byte[] decodeRelocationEnvelopeV1(List<byte[]> chunks){var o=new ByteArrayOutputStream();for(var c:chunks)o.writeBytes(c);return o.toByteArray();}\n  private ServiceWirePilotCodec(){}\n}\n`;

const dotnet = `${banner("//")}using System; using System.IO; using System.Text;\nnamespace Systems.Zlink.Framework.Runtime.Protocol;\npublic static class ServiceWirePilotCodec { public sealed record Fence(string Id, ulong Generation, byte[] TargetNodeRid, ulong TargetNodeGeneration, ulong ExpectedAuthorityOwnerGeneration, ulong ExpectedOwnerLeaseGeneration); public sealed record ActorJoin28(ulong Correlation,Fence Actor,bool Entry,Fence TargetSpot); static void U64(BinaryWriter w,ulong v)=>w.Write(System.Buffers.Binary.BinaryPrimitives.ReverseEndianness(v)); static ulong R64(BinaryReader r)=>System.Buffers.Binary.BinaryPrimitives.ReverseEndianness(r.ReadUInt64()); static void T(BinaryWriter w,string s){var b=Encoding.UTF8.GetBytes(s);if(b.Length is 0 or >255)throw new InvalidDataException();w.Write((byte)b.Length);w.Write(b);} static string T(BinaryReader r){var n=r.ReadByte();return Encoding.UTF8.GetString(r.ReadBytes(n));} static void F(BinaryWriter w,Fence f){T(w,f.Id);U64(w,f.Generation);w.Write((byte)f.TargetNodeRid.Length);w.Write(f.TargetNodeRid);U64(w,f.TargetNodeGeneration);U64(w,f.ExpectedAuthorityOwnerGeneration);U64(w,f.ExpectedOwnerLeaseGeneration);} static Fence F(BinaryReader r){var id=T(r);var n=r.ReadByte();var rid=r.ReadBytes(n);if(n==0||rid.Length!=n)throw new EndOfStreamException();return new(id,R64(r),rid,R64(r),R64(r),R64(r));} public static byte[] EncodeActorJoin28(ActorJoin28 v){using var m=new MemoryStream();using var w=new BinaryWriter(m);w.Write(new byte[]{${schema.protocol.magic.join(",")},${schema.protocol.wireMajor},${actorJoin.id},0});U64(w,v.Correlation);F(w,v.Actor);w.Write(v.Entry?(byte)1:(byte)0);F(w,v.TargetSpot);return m.ToArray();} public static ActorJoin28 DecodeActorJoin28(byte[] b){using var r=new BinaryReader(new MemoryStream(b));if(r.ReadByte()!=${schema.protocol.magic[0]}||r.ReadByte()!=${schema.protocol.magic[1]}||r.ReadByte()!=${schema.protocol.wireMajor}||r.ReadByte()!=${actorJoin.id}||r.ReadByte()!=0)throw new InvalidDataException();var c=R64(r);var a=F(r);var e=r.ReadByte();var s=F(r);if(c==0||e>1||r.BaseStream.Position!=r.BaseStream.Length)throw new InvalidDataException();return new(c,a,e==1,s);} public static byte[] EncodeRelocationEnvelopeV1(byte[] logical)=>(byte[])logical.Clone(); public static byte[] DecodeRelocationEnvelopeV1(params byte[][] chunks){using var m=new MemoryStream();foreach(var c in chunks)m.Write(c);return m.ToArray();} }\n`;

const cpp = `${banner("//")}#pragma once\n#include <cstdint>\n#include <cstddef>\n#include <stdexcept>\n#include <vector>\n#include <string>\nnamespace zlink::framework::runtime::protocol { struct service_wire_pilot_fence { std::string id; std::uint64_t generation; std::vector<std::uint8_t> target_node_rid; std::uint64_t target_node_generation, expected_authority_owner_generation, expected_owner_lease_generation; }; struct service_wire_pilot_actor_join_28 { std::uint64_t correlation; service_wire_pilot_fence actor; bool entry; service_wire_pilot_fence target_spot; }; inline void pilot_u64(std::vector<std::uint8_t>&o,std::uint64_t v){for(int i=7;i>=0;--i)o.push_back(static_cast<std::uint8_t>(v>>(i*8)));} inline std::uint64_t pilot_r64(const std::vector<std::uint8_t>&b,std::size_t&at){if(at+8>b.size())throw std::invalid_argument("truncated u64");std::uint64_t v=0;for(int i=0;i<8;++i)v=(v<<8)|b[at++];return v;} inline service_wire_pilot_fence pilot_fence(const std::vector<std::uint8_t>&b,std::size_t&at){if(at>=b.size())throw std::invalid_argument("truncated id");const auto n=b[at++];if(!n||at+n>b.size())throw std::invalid_argument("invalid id");service_wire_pilot_fence f;f.id.assign(reinterpret_cast<const char*>(b.data()+at),n);at+=n;f.generation=pilot_r64(b,at);if(!f.generation||at>=b.size())throw std::invalid_argument("invalid generation");const auto r=b[at++];if(!r||at+r>b.size())throw std::invalid_argument("invalid rid");f.target_node_rid.assign(b.begin()+static_cast<std::ptrdiff_t>(at),b.begin()+static_cast<std::ptrdiff_t>(at+r));at+=r;f.target_node_generation=pilot_r64(b,at);f.expected_authority_owner_generation=pilot_r64(b,at);f.expected_owner_lease_generation=pilot_r64(b,at);if(!f.target_node_generation||!f.expected_authority_owner_generation||!f.expected_owner_lease_generation)throw std::invalid_argument("invalid fence");return f;} inline std::vector<std::uint8_t> encode_actor_join_28(const service_wire_pilot_actor_join_28&v){if(!v.correlation)throw std::invalid_argument("correlation");std::vector<std::uint8_t> o={${schema.protocol.magic.join(",")},${schema.protocol.wireMajor},${actorJoin.id},0};auto f=[&](const service_wire_pilot_fence&x){if(x.id.empty()||x.id.size()>255||x.target_node_rid.empty()||x.target_node_rid.size()>255||!x.generation||!x.target_node_generation||!x.expected_authority_owner_generation||!x.expected_owner_lease_generation)throw std::invalid_argument("fence");o.push_back(x.id.size());o.insert(o.end(),x.id.begin(),x.id.end());pilot_u64(o,x.generation);o.push_back(x.target_node_rid.size());o.insert(o.end(),x.target_node_rid.begin(),x.target_node_rid.end());pilot_u64(o,x.target_node_generation);pilot_u64(o,x.expected_authority_owner_generation);pilot_u64(o,x.expected_owner_lease_generation);};pilot_u64(o,v.correlation);f(v.actor);o.push_back(v.entry?1:0);f(v.target_spot);return o;} inline service_wire_pilot_actor_join_28 decode_actor_join_28(const std::vector<std::uint8_t>&b){std::size_t at=0;if(b.size()<5||b[at++]!=${schema.protocol.magic[0]}||b[at++]!=${schema.protocol.magic[1]}||b[at++]!=${schema.protocol.wireMajor}||b[at++]!=${actorJoin.id}||b[at++]!=0)throw std::invalid_argument("actorJoin header");service_wire_pilot_actor_join_28 v{};v.correlation=pilot_r64(b,at);if(!v.correlation)throw std::invalid_argument("correlation");v.actor=pilot_fence(b,at);if(at>=b.size()||b[at]>1)throw std::invalid_argument("entry");v.entry=b[at++]==1;v.target_spot=pilot_fence(b,at);if(at!=b.size())throw std::invalid_argument("actorJoin trailing");return v;} inline std::vector<std::uint8_t> encode_relocation_envelope_v1(std::vector<std::uint8_t> logical){return logical;} inline std::vector<std::uint8_t> decode_relocation_envelope_v1(const std::vector<std::vector<std::uint8_t>>&chunks){std::vector<std::uint8_t>o;for(const auto&c:chunks)o.insert(o.end(),c.begin(),c.end());return o;} }\n`;

const nodeOut = node + nodeMechanical;
const javaOut = java.replace(/\}\n$/, `${javaMechanical}}\n`);
const dotnetOut = dotnet.replace(/\}\n$/, `${dotnetMechanical}}\n`);
const cppOut = cpp.replace(/\}\n$/, `${cppMechanical}}\n`);

const outputs = new Map([["generated/node/service_wire_pilot_codec.generated.ts",nodeOut],["generated/jvm/ServiceWirePilotCodec.java",javaOut],["generated/dotnet/ServiceWirePilotCodec.g.cs",dotnetOut],["generated/cpp/service_wire_pilot_codec.hpp",cppOut],["generated/fixtures/relocation-envelope-v1-pilot.json", `${JSON.stringify({ schema: "service-wire-v1", format: relocationFixture.format, input: relocationFixture.decoded, hex: relocationFixture.logicalHex }, null, 2)}\n`]]);
let stale=false; for(const [relative,content] of outputs){const target=path.join(root,relative);if(check){if(!fs.existsSync(target)||fs.readFileSync(target,"utf8")!==content){console.error(`stale: ${relative}`);stale=true;}}else{fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,content);}} if(stale)process.exit(1); console.log(`${check?"verified":"generated"} service-wire pilot codecs; schema=${hash}`);

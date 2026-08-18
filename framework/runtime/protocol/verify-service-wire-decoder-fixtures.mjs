#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));
const schema = JSON.parse(fs.readFileSync(path.join(root, "service-wire-v1.schema.json"), "utf8"));
const fixtures = JSON.parse(fs.readFileSync(path.join(root, "golden/service-decoder-fixtures-v1.json"), "utf8"));
const replacedFixture = JSON.parse(fs.readFileSync(
  path.join(root, "golden/bound-session-replaced-v1.json"),
  "utf8",
));
const sessionBarrierFixture = JSON.parse(fs.readFileSync(
  path.join(root, "golden/session-relocation-barrier-v1.json"),
  "utf8",
));
const actorJoinReplyFixture = JSON.parse(fs.readFileSync(
  path.join(root, "golden/actor-join-reply-v1.json"),
  "utf8",
));
const commands = new Map(schema.commands.map((entry) => [entry.id, entry]));
const frameworkErrorType = schema.types.find((entry) => entry.name === "framework-error-code");
const frameworkErrors = new Map(frameworkErrorType.values.map((entry) => [entry.value, entry]));
const terminalFailureIntegrity = schema.semanticConstraints.find(
  (entry) => entry.kind === "terminal-failure-integrity",
);

function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}

function decode(bytes) {
  if (bytes.length < schema.protocol.headPrefixBytes) fail("truncated-head");
  if (bytes[0] !== schema.protocol.magic[0] || bytes[1] !== schema.protocol.magic[1]) fail("invalid-magic");
  if (bytes[2] !== schema.protocol.wireMajor) fail("invalid-major");
  const command = commands.get(bytes[3]);
  if (!command) fail("unknown-command");
  const flags = bytes[4];
  const allowed = command.allowedFlags.reduce((value, name) => {
    const flag = schema.flags.find((entry) => entry.name === name);
    return value | flag.bit;
  }, 0);
  if ((flags & ~allowed) !== 0) fail("forbidden-flag");
  if (command.name === "livenessProbe" || command.name === "livenessAck") {
    if (bytes.length < 13) fail("truncated-field");
    if (bytes.length > 13) fail("trailing-byte");
    let probeId = 0n;
    for (const byte of bytes.slice(5)) probeId = (probeId << 8n) | BigInt(byte);
    if (probeId === 0n) fail("invalid-field");
    return { command: command.name, probeId };
  }
  return { command: command.name };
}

function decodeFrameworkErrorCode(wireValue) {
  const entry = frameworkErrors.get(wireValue);
  if (!entry) fail("unknown-framework-error");
  if (entry.name === "none") return { name: entry.name, publicValue: null };
  if (terminalFailureIntegrity.publicMapping !== "wire-value-minus-one") {
    fail("unsupported-framework-error-mapping");
  }
  return { name: entry.name, publicValue: wireValue - 1 };
}

function decodeBoundSessionReplaced(bytes) {
  let offset = 0;
  const need = (count) => {
    if (offset + count > bytes.length) fail("truncated-field");
  };
  const byte = () => {
    need(1);
    return bytes[offset++];
  };
  const sized8 = () => {
    const length = byte();
    if (length === 0) fail("invalid-field");
    need(length);
    const value = Buffer.from(bytes.slice(offset, offset + length)).toString("utf8");
    offset += length;
    if (value.includes("\0")) fail("invalid-field");
    return value;
  };
  const nonzeroU64 = () => {
    need(8);
    let value = 0n;
    for (let index = 0; index < 8; index += 1) value = (value << 8n) | BigInt(bytes[offset++]);
    if (value === 0n || value > 0x7fff_ffff_ffff_ffffn) fail("invalid-field");
    return value;
  };
  need(5);
  if (byte() !== schema.protocol.magic[0] || byte() !== schema.protocol.magic[1]) fail("invalid-magic");
  if (byte() !== schema.protocol.wireMajor) fail("invalid-major");
  if (byte() !== replacedFixture.commandId) fail("unknown-command");
  if (byte() !== 0) fail("forbidden-flag");
  const result = {
    actorAuthority: {
      actorId: sized8(),
      objectGeneration: nonzeroU64().toString(),
      targetNodeRid: sized8(),
      targetNodeGeneration: nonzeroU64().toString(),
      expectedAuthorityOwnerGeneration: nonzeroU64().toString(),
      expectedOwnerLeaseGeneration: nonzeroU64().toString(),
    },
    retiredSession: {
      sessionOwnerNodeRid: sized8(),
      sessionOwnerNodeGeneration: nonzeroU64().toString(),
      sessionOwnerId: sized8(),
      sessionOwnerLeaseGeneration: nonzeroU64().toString(),
      sessionRid: sized8(),
      retiredBindingGeneration: nonzeroU64().toString(),
    },
  };
  if (offset !== bytes.length) fail("trailing-byte");
  return result;
}

function decodeSessionRelocationBarrier(hex) {
  const bytes = Buffer.from(hex, "hex");
  let offset = 0;
  const need = (count) => {
    if (offset + count > bytes.length) fail("truncated-field");
  };
  const byte = () => {
    need(1);
    return bytes[offset++];
  };
  const u16 = () => {
    need(2);
    const value = bytes.readUInt16BE(offset);
    offset += 2;
    return value;
  };
  const u64 = (allowZero = false) => {
    need(8);
    const value = bytes.readBigUInt64BE(offset);
    offset += 8;
    if (!allowZero && value === 0n) fail("invalid-field");
    return value.toString();
  };
  const text8 = () => {
    const length = byte();
    if (length === 0) fail("invalid-field");
    need(length);
    const value = bytes.subarray(offset, offset + length).toString("utf8");
    offset += length;
    if (value.includes("\0")) fail("invalid-field");
    return value;
  };
  const text16 = () => {
    const length = u16();
    if (length === 0) fail("invalid-field");
    need(length);
    const value = bytes.subarray(offset, offset + length).toString("utf8");
    offset += length;
    if (value.includes("\0")) fail("invalid-field");
    return value;
  };
  const actor = () => ({ actorId: text8(), objectGeneration: u64() });
  const actorFence = () => ({
    ...actor(),
    targetNodeRid: text8(),
    targetNodeGeneration: u64(),
    authorityOwnerGeneration: u64(),
    ownerLeaseGeneration: u64(),
  });
  const session = () => ({
    sessionOwnerNodeRid: text8(),
    sessionOwnerNodeGeneration: u64(),
    sessionOwnerId: text8(),
    sessionOwnerLeaseGeneration: u64(),
    sessionRid: text8(),
    bindingGeneration: u64(),
  });
  need(5);
  if (byte() !== schema.protocol.magic[0] || byte() !== schema.protocol.magic[1]) fail("invalid-magic");
  if (byte() !== schema.protocol.wireMajor) fail("invalid-major");
  const command = byte();
  if (byte() !== 0) fail("forbidden-flag");
  const relocation = { high: u64(true), low: u64(true) };
  if (relocation.high === "0" && relocation.low === "0") fail("invalid-field");
  const coordinator = {
    ownerId: text8(),
    leaseGeneration: u64(),
    nodeRid: text8(),
    nodeGeneration: u64(),
    expectedAuthorityStoreVersion: text16(),
  };
  let result;
  if (command === 42) {
    const senderRole = byte();
    if (senderRole !== 1) fail("invalid-field");
    result = { command, relocation, coordinator, senderRole: "source", actor: actorFence(), session: session() };
  } else if (command === 43) {
    // Command 43 returns only the exact seal-install result and carries no
    // Session-message sequence or high-water (internal-service-wire-protocol).
    result = {
      command,
      relocation,
      coordinator,
      actor: actorFence(),
      session: session(),
    };
  } else if (command === 44) {
    const senderRole = byte();
    const actorRef = actor();
    const sessionFence = session();
    const action = byte();
    const routeLength = u16();
    const routeEnd = offset + routeLength;
    if (routeEnd > bytes.length) fail("truncated-field");
    let route;
    if (action === 1 && senderRole === 2) {
      route = {
        // Command 44 carries no per-record ACK or numeric high-water
        // (internal-service-wire-protocol).
        action: "commit",
        previousAuthorityOwnerGeneration: u64(),
        targetAuthorityOwnerGeneration: u64(),
        targetNodeRid: text8(),
        targetNodeGeneration: u64(),
      };
      if (BigInt(route.targetAuthorityOwnerGeneration)
          <= BigInt(route.previousAuthorityOwnerGeneration)) fail("invalid-field");
    } else if (action === 2 && senderRole === 1) {
      route = { action: "abort", currentAuthorityOwnerGeneration: u64() };
    } else {
      fail("invalid-field");
    }
    if (offset !== routeEnd) fail("invalid-field");
    result = {
      command,
      relocation,
      coordinator,
      senderRole: senderRole === 2 ? "target" : "source",
      actor: actorRef,
      session: sessionFence,
      route,
    };
  } else {
    // Command 44 has no reply, and reserved command 45 is neither sent nor
    // accepted (internal-service-wire-protocol).
    fail("unknown-command");
  }
  if (offset !== bytes.length) fail("trailing-byte");
  return result;
}

for (const fixture of fixtures.canonical) {
  const decoded = decode(fixture.bytes);
  if (decoded.command !== fixture.name || decoded.probeId.toString() !== fixtures.probeId) {
    throw new Error(`canonical fixture mismatch: ${fixture.name}`);
  }
}
for (const fixture of fixtures.malformed) {
  try {
    decode(fixture.bytes);
    throw new Error(`malformed fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) throw error;
  }
}
const probe = decode(fixtures.canonical.find((entry) => entry.name === "livenessProbe").bytes);
const ack = decode(fixtures.canonical.find((entry) => entry.name === "livenessAck").bytes);
if (probe.probeId !== ack.probeId) throw new Error("livenessAck does not echo livenessProbe id");

if (fixtures.frameworkErrors.publicMapping !== terminalFailureIntegrity.publicMapping
    || JSON.stringify(fixtures.frameworkErrors.reservedWireValues)
      !== JSON.stringify(terminalFailureIntegrity.reservedWireValues)) {
  throw new Error("framework error mapping fixture does not match schema");
}
for (const fixture of fixtures.frameworkErrors.canonical) {
  const decoded = decodeFrameworkErrorCode(fixture.wireValue);
  if (decoded.name !== fixture.name || decoded.publicValue !== fixture.publicValue) {
    throw new Error(`framework error fixture mismatch: ${fixture.name}`);
  }
}
for (const fixture of fixtures.frameworkErrors.malformed) {
  try {
    decodeFrameworkErrorCode(fixture.wireValue);
    throw new Error(`malformed framework error fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) throw error;
  }
}
const relocationDataLost = fixtures.frameworkErrors.canonical.find(
  (entry) => entry.name === "relocationDataLost",
);
if (relocationDataLost?.wireValue !== 35 || relocationDataLost.publicValue !== 34) {
  throw new Error("RelocationDataLost must decode from wire 35 to public 34");
}

const replaced = decodeBoundSessionReplaced(replacedFixture.canonical.bytes);
if (JSON.stringify(replaced) !== JSON.stringify({
  actorAuthority: replacedFixture.canonical.actorAuthority,
  retiredSession: replacedFixture.canonical.retiredSession,
})) {
  throw new Error("boundSessionReplaced canonical fixture mismatch");
}
for (const fixture of replacedFixture.malformed) {
  try {
    decodeBoundSessionReplaced(fixture.bytes);
    throw new Error(`malformed boundSessionReplaced fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) throw error;
  }
}
if (replacedFixture.receiverFenceCases[0]?.result !== "apply"
    || replacedFixture.receiverFenceCases[1]?.result !== "ignore-stale") {
  throw new Error("boundSessionReplaced receiver lifecycle fixture is incomplete");
}

const barrierIdentity = sessionBarrierFixture.identity;
const barrierRecords = new Map(sessionBarrierFixture.canonical.map((fixture) => {
  const decoded = decodeSessionRelocationBarrier(fixture.hex);
  if (decoded.command !== fixture.command
      || decoded.relocation.high !== barrierIdentity.relocationHigh
      || decoded.relocation.low !== barrierIdentity.relocationLow
      || decoded.coordinator.ownerId !== barrierIdentity.coordinatorOwnerId
      || decoded.coordinator.leaseGeneration !== barrierIdentity.coordinatorLeaseGeneration
      || decoded.coordinator.nodeRid !== barrierIdentity.coordinatorNodeRid
      || decoded.coordinator.nodeGeneration !== barrierIdentity.coordinatorNodeGeneration
      || decoded.coordinator.expectedAuthorityStoreVersion
         !== barrierIdentity.expectedAuthorityStoreVersion
      || decoded.actor.actorId !== barrierIdentity.actorId
      || decoded.actor.objectGeneration !== barrierIdentity.objectGeneration
      || decoded.session.sessionOwnerNodeRid !== barrierIdentity.sessionOwnerNodeRid
      || decoded.session.sessionOwnerNodeGeneration
         !== barrierIdentity.sessionOwnerNodeGeneration
      || decoded.session.sessionOwnerId !== barrierIdentity.sessionOwnerId
      || decoded.session.sessionOwnerLeaseGeneration
         !== barrierIdentity.sessionOwnerLeaseGeneration
      || decoded.session.sessionRid !== barrierIdentity.sessionRid
      || decoded.session.bindingGeneration !== barrierIdentity.bindingGeneration) {
    throw new Error(`session relocation barrier identity mismatch: ${fixture.name}`);
  }
  for (const [name, value] of Object.entries(fixture.decoded)) {
    const actual = name in decoded ? decoded[name] : decoded.route?.[name] ?? decoded.actor?.[name];
    if (actual !== value) {
      throw new Error(`session relocation barrier field mismatch: ${fixture.name}.${name}`);
    }
  }
  return [fixture.name, decoded];
}));
if (barrierRecords.size !== 4
    || !barrierRecords.has("sessionRelocationSeal")
    || !barrierRecords.has("sessionRelocationSealed")
    || !barrierRecords.has("sessionRelocationRouteCommit")
    || !barrierRecords.has("sessionRelocationRouteAbort")) {
  throw new Error("session relocation barrier canonical fixture is incomplete");
}
const receiverCases = new Map(sessionBarrierFixture.receiverCases.map((entry) => [entry.name, entry]));
if (receiverCases.get("identicalSealDuplicate")?.result !== "idempotent"
    || receiverCases.get("conflictingSealDuplicate")?.result !== "protocolError"
    || receiverCases.get("identicalCommitDuplicate")?.result !== "alreadyApplied"
    || receiverCases.get("conflictingCommitDuplicate")?.result !== "protocolError"
    || receiverCases.get("matchingAbort")?.result !== "applied"
    || receiverCases.get("differentRelocationAbort")?.result !== "stale") {
  throw new Error("session relocation barrier receiver cases are incomplete");
}

// Independent, schema-derived decoder for golden/actor-join-reply-v1.json
// (type actor-join-reply-tail, service-wire-v1.schema.json). This shares no
// code with the node runtime's decodeStatefulReply/decodeReplyTail so that a
// canonical fixture agreed by both decoders is not a tautology of node's own
// implementation.
const actorJoinReplyTailType = schema.types.find((entry) => entry.name === "actor-join-reply-tail");
const nonzeroU64Type = schema.types.find((entry) => entry.name === "nonzero-u64");
const NONZERO_U64_MAX = BigInt(nonzeroU64Type.maximum);
// receiveChunkLimitBytes is declared as a plain u32 on actor-join-reply-tail
// (:2279-2281); the tighter bound below is not part of that field's own
// schema type. It is relocationStorageProfile.chunkDataMaximumBytes, the
// relocation state chunk-data ceiling that cpp/dotnet/java all reuse for
// this field (relocation_chunk_wire_limit_bytes / RelocationChunkBytesBound /
// CHUNK_DATA_BYTES_BOUND, each 67_108_864). It is enforced here, and by every
// implementation, as an application-level constraint layered on top of the
// wire type, not as a schema-declared bound on the field itself.
const RECEIVE_CHUNK_LIMIT_BYTES_MAX = schema.relocationStorageProfile.chunkDataMaximumBytes;
if (!actorJoinReplyTailType || actorJoinReplyTailType.bodyLengthCovers !== "selected-case") {
  throw new Error("actor-join-reply-tail schema shape changed; update the independent decoder");
}

function decodeActorJoinReply(hex) {
  const bytes = Buffer.from(hex, "hex");
  let offset = 0;
  const need = (count) => {
    if (offset + count > bytes.length) fail("truncated-field");
  };
  const byte = () => {
    need(1);
    return bytes[offset++];
  };
  const u16 = () => {
    need(2);
    const value = bytes.readUInt16BE(offset);
    offset += 2;
    return value;
  };
  const u32 = () => {
    need(4);
    const value = bytes.readUInt32BE(offset);
    offset += 4;
    return value;
  };
  const nonzeroU64 = () => {
    need(8);
    const value = bytes.readBigUInt64BE(offset);
    offset += 8;
    if (value === 0n || value > NONZERO_U64_MAX) fail("invalid-field");
    return value;
  };
  const text8 = () => {
    const length = byte();
    if (length === 0) fail("invalid-field");
    need(length);
    const value = bytes.subarray(offset, offset + length).toString("utf8");
    offset += length;
    if (value.includes("\0")) fail("invalid-field");
    return value;
  };
  const spotRef = () => ({ spotId: text8(), generation: nonzeroU64() });

  need(5);
  if (byte() !== schema.protocol.magic[0] || byte() !== schema.protocol.magic[1]) fail("invalid-magic");
  if (byte() !== schema.protocol.wireMajor) fail("invalid-major");
  const command = byte();
  if (command !== 20) fail("unknown-command");
  if (byte() !== 0) fail("forbidden-flag");
  const correlation = nonzeroU64();
  const terminalResult = u32();
  const failureCode = u32();
  const joinResultWire = u32();
  const bodyLength = u16();
  const bodyEnd = offset + bodyLength;
  let result;
  if (joinResultWire === 0) {
    const spot = spotRef();
    const membershipEpoch = nonzeroU64();
    const receiveChunkLimitBytes = u32();
    if (receiveChunkLimitBytes > RECEIVE_CHUNK_LIMIT_BYTES_MAX) fail("invalid-field");
    if (offset !== bodyEnd) fail("invalid-field");
    result = { joinResult: "accepted", spot, membershipEpoch, receiveChunkLimitBytes };
  } else if (joinResultWire === 1) {
    const hasSpot = byte();
    if (hasSpot !== 0 && hasSpot !== 1) fail("invalid-field");
    const optionalLength = u16();
    const optionalEnd = offset + optionalLength;
    const spot = hasSpot === 1 ? spotRef() : undefined;
    if (offset !== optionalEnd) fail("invalid-field");
    if (offset !== bodyEnd) fail("invalid-field");
    result = { joinResult: "rejected", ...(spot === undefined ? {} : { spot }) };
  } else {
    fail("invalid-field");
  }
  if (offset !== bytes.length) fail("trailing-byte");
  return { command, correlation, terminalResult, failureCode, ...result };
}

for (const fixture of actorJoinReplyFixture.canonical) {
  const decoded = decodeActorJoinReply(fixture.hex);
  const expected = fixture.decoded;
  const mismatch = decoded.command !== fixture.command
    || decoded.correlation.toString() !== fixture.correlation
    || decoded.terminalResult !== 0
    || decoded.failureCode !== 0
    || decoded.joinResult !== expected.joinResult
    || (expected.spot === undefined
      ? decoded.spot !== undefined
      : decoded.spot?.spotId !== expected.spot.spotId
        || decoded.spot?.generation.toString() !== expected.spot.generation)
    || (expected.membershipEpoch !== undefined
      && decoded.membershipEpoch?.toString() !== expected.membershipEpoch)
    || (expected.receiveChunkLimitBytes !== undefined
      && decoded.receiveChunkLimitBytes !== expected.receiveChunkLimitBytes);
  if (mismatch) throw new Error(`actor join reply canonical fixture mismatch: ${fixture.name}`);
}
for (const fixture of actorJoinReplyFixture.malformed) {
  try {
    decodeActorJoinReply(fixture.hex);
    throw new Error(`malformed actor join reply fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) {
      throw new Error(
        `actor join reply malformed fixture ${fixture.name}: expected error '${fixture.error}', got '${error.code ?? error.message}'`,
      );
    }
  }
}

console.log(
  `service wire decoder fixtures valid: canonical=${fixtures.canonical.length} `
    + `malformed=${fixtures.malformed.length} frameworkErrors=${fixtures.frameworkErrors.canonical.length} `
    + `frameworkErrorMalformed=${fixtures.frameworkErrors.malformed.length} probeEcho=pass `
    + `RelocationDataLost=wire35/public34 boundSessionReplaced=pass `
    + `sessionRelocationBarrier=${barrierRecords.size} `
    + `actorJoinReply=${actorJoinReplyFixture.canonical.length}/${actorJoinReplyFixture.malformed.length}`,
);

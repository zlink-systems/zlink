#!/usr/bin/env node

import fs from "node:fs";
import crypto from "node:crypto";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));
const schemaPath = path.join(root, "service-wire-v1.schema.json");
const check = process.argv.includes("--check");
const schema = JSON.parse(fs.readFileSync(schemaPath, "utf8"));

const snake = (value) => value.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toUpperCase();
const pascal = (value) => value[0].toUpperCase() + value.slice(1);
const commands = [...schema.commands].sort((a, b) => a.id - b.id);
const flags = [...schema.flags].sort((a, b) => a.bit - b.bit);
const numericBounds = schema.bounds.filter((entry) => Number.isSafeInteger(entry.value));
const frameworkErrors = [...schema.types.find((entry) => entry.name === "framework-error-code").values]
  .sort((a, b) => a.value - b.value);
const requestTerminalResults = [
  ...schema.types.find((entry) => entry.name === "request-terminal-result").values,
].sort((a, b) => a.value - b.value);
const terminalFailureIntegrity = schema.semanticConstraints.find(
  (entry) => entry.kind === "terminal-failure-integrity",
);
const frameworkErrorsByTerminal = new Map();
for (const [failure, terminal] of Object.entries(
  terminalFailureIntegrity.typedFrameworkFailure.exactResultByFailureCode,
)) {
  const failures = frameworkErrorsByTerminal.get(terminal) ?? [];
  failures.push(failure);
  frameworkErrorsByTerminal.set(terminal, failures);
}
const cppTypedTerminalCases = [...frameworkErrorsByTerminal.entries()]
  .map(([terminal, failures]) => `${failures
    .map((failure) => `        case framework_error_code::${failure}:`)
    .join("\n")}
            return terminal == static_cast<std::uint32_t>(request_terminal_result::${terminal});`)
  .join("\n");
const cppBoundaryTerminalCases = terminalFailureIntegrity.boundaryFailure.terminalResults
  .map((terminal) => `        case request_terminal_result::${terminal}:`)
  .join("\n");
const frameworkMultipartV1Profile = schema.frameworkMultipartV1Profile;
const terminalValueByName = new Map(
  requestTerminalResults.map((entry) => [entry.name, entry.value]),
);
const failureValueByName = new Map(frameworkErrors.map((entry) => [entry.name, entry.value]));
const boundaryTerminalValues = terminalFailureIntegrity.boundaryFailure.terminalResults
  .map((name) => terminalValueByName.get(name));
const typedTerminalGroups = [...frameworkErrorsByTerminal.entries()].map(
  ([terminal, failures]) => ({
    terminalValue: terminalValueByName.get(terminal),
    failures: failures.map((name) => ({ name, value: failureValueByName.get(name) })),
  }),
);
const dotnetBoundaryCases = boundaryTerminalValues
  .map((value) => `            case ${value}u:`)
  .join("\n");
const dotnetTypedCases = typedTerminalGroups
  .map((group) => `${group.failures
    .map((failure) => `            case ${failure.value}u: // ${failure.name}`)
    .join("\n")}
                return terminal == ${group.terminalValue}u;`)
  .join("\n");
const javaBoundaryCases = boundaryTerminalValues
  .map((value) => `            case ${value}:`)
  .join("\n");
const javaTypedCases = typedTerminalGroups
  .map((group) => `${group.failures
    .map((failure) => `            case ${failure.value}: // ${failure.name}`)
    .join("\n")}
                return terminal == ${group.terminalValue}L;`)
  .join("\n");
const nodeExactPairEntries = typedTerminalGroups
  .flatMap((group) => group.failures.map(
    (failure) => ({ ...failure, terminalValue: group.terminalValue }),
  ))
  .sort((a, b) => a.value - b.value);

function head(command, flagsValue = 0) {
  return [...schema.protocol.magic, schema.protocol.wireMajor, command, flagsValue];
}

function u64(value) {
  let current = BigInt(value);
  const bytes = Array(8).fill(0);
  for (let index = 7; index >= 0; --index) {
    bytes[index] = Number(current & 0xffn);
    current >>= 8n;
  }
  return bytes;
}

function u16(value) {
  const bytes = Buffer.alloc(2);
  bytes.writeUInt16BE(Number(value));
  return [...bytes];
}

function u32(value) {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32BE(Number(value));
  return [...bytes];
}

function bytes8(value) {
  const bytes = [...Buffer.from(value)];
  if (bytes.length < 1 || bytes.length > 255) {
    throw new Error("fixture bytes must be 1..255 bytes");
  }
  return [bytes.length, ...bytes];
}

function sized8(value) {
  const bytes = [...Buffer.from(value, "utf8")];
  if (bytes.length < 1 || bytes.length > 255 || bytes.includes(0)) {
    throw new Error("fixture text must be 1..255 UTF-8 bytes without NUL");
  }
  return [bytes.length, ...bytes];
}

function sized16(value) {
  const bytes = [...Buffer.from(value, "utf8")];
  if (bytes.length < 1 || bytes.length > 65535 || bytes.includes(0)) {
    throw new Error("fixture text must be 1..65535 UTF-8 bytes without NUL");
  }
  return [...u16(bytes.length), ...bytes];
}

function serviceCommand(commandId, body) {
  return [...head(commandId), ...body];
}

const commonTerminalOperation = {
  correlation: "1",
  operation: { high: "2", low: "3" },
  sourceNodeRid: "737263",
  sourceNodeGeneration: "4",
};

const oracleCommandBodies = new Map([
  ["userSpotCreate", [
    ["correlation", "nonzero-u64"], ["operation", "operation-id"],
    ["sourceNodeRid", "rid"], ["sourceNodeGeneration", "nonzero-u64"],
    ["spotId", "text8"], ["stableType", "text8"],
    ["reservation", "object-reservation-fence"], ["deadlineUnixMs", "nonzero-u64"],
  ]],
  ["userSpotClose", [
    ["correlation", "nonzero-u64"], ["operation", "operation-id"],
    ["sourceNodeRid", "rid"], ["sourceNodeGeneration", "nonzero-u64"],
    ["target", "user-spot-close-fence-v1"], ["deadlineUnixMs", "nonzero-u64"],
  ]],
  ["actorCreate", [
    ["correlation", "nonzero-u64"], ["operation", "operation-id"],
    ["sourceNodeRid", "rid"], ["sourceNodeGeneration", "nonzero-u64"],
    ["actorId", "text8"], ["stableType", "text8"],
    ["reservation", "object-reservation-fence"], ["deadlineUnixMs", "nonzero-u64"],
  ]],
]);
for (const [name, expectedBody] of oracleCommandBodies) {
  const command = commands.find((entry) => entry.name === name);
  const actualBody = command?.body?.map((field) => [field.name, field.$ref]);
  if (command?.payload !== "forbidden"
      || JSON.stringify(actualBody) !== JSON.stringify(expectedBody)) {
    throw new Error(`schema command body changed before oracle generation: ${name}`);
  }
}
const canonicalReservation = {
  reservationId: "reserve-1",
  expectedStoreVersion: "store-v1",
  objectGeneration: "5",
  authorityOwnerGeneration: "6",
  targetNodeRid: "647374",
  targetNodeGeneration: "7",
  targetOwnerId: "owner-a",
  targetOwnerLeaseGeneration: "8",
  pendingCapacityDelta: "9",
};

function reservationBytes(value) {
  return [
    ...sized8(value.reservationId),
    ...sized16(value.expectedStoreVersion),
    ...u64(value.objectGeneration),
    ...u64(value.authorityOwnerGeneration),
    ...bytes8(Buffer.from(value.targetNodeRid, "hex")),
    ...u64(value.targetNodeGeneration),
    ...sized8(value.targetOwnerId),
    ...u64(value.targetOwnerLeaseGeneration),
    ...u32(value.pendingCapacityDelta),
  ];
}

function terminalOperationPrefix(value) {
  return [
    ...u64(value.correlation),
    ...u64(value.operation.high),
    ...u64(value.operation.low),
    ...bytes8(Buffer.from(value.sourceNodeRid, "hex")),
    ...u64(value.sourceNodeGeneration),
  ];
}

function malformedHex(bytes, mutate) {
  const candidate = [...bytes];
  mutate(candidate);
  return Buffer.from(candidate).toString("hex");
}

const userSpotCreateDecoded = {
  ...commonTerminalOperation,
  spotId: "spot-a",
  stableType: "chat",
  reservation: canonicalReservation,
  deadlineUnixMs: "10",
};
const userSpotCreateBytes = serviceCommand(47, [
  ...terminalOperationPrefix(userSpotCreateDecoded),
  ...sized8(userSpotCreateDecoded.spotId),
  ...sized8(userSpotCreateDecoded.stableType),
  ...reservationBytes(userSpotCreateDecoded.reservation),
  ...u64(userSpotCreateDecoded.deadlineUnixMs),
]);
const userSpotCreateFixture = {
  format: "user-spot-create-v1",
  consumers: ["cpp", "dotnet", "jvm", "node"],
  commandId: 47,
  notes: {
    encoding: "complete service-wire command 47 frame; all integers are big-endian; body follows the schema field order; payload and trailing bytes are forbidden",
    offsetConvention: "inclusive zero-based byte offsets",
    offsets: [
      "0..1 magic=ZM", "2 wireMajor=1", "3 commandId=47", "4 flags=0",
      "5..12 correlation(nonzero-u64)", "13..20 operation.high(u64)",
      "21..28 operation.low(u64)", "29 sourceNodeRid.length(u8)=3",
      "30..32 sourceNodeRid bytes", "33..40 sourceNodeGeneration(nonzero-u64)",
      "41 spotId.length(u8)=6", "42..47 spotId UTF-8 bytes",
      "48 stableType.length(u8)=4", "49..52 stableType UTF-8 bytes",
      "53 reservationId.length(u8)=9", "54..62 reservationId UTF-8 bytes",
      "63..64 expectedStoreVersion.length(u16)=8", "65..72 expectedStoreVersion UTF-8 bytes",
      "73..80 objectGeneration(nonzero-u64)", "81..88 authorityOwnerGeneration(nonzero-u64)",
      "89 targetNodeRid.length(u8)=3", "90..92 targetNodeRid bytes",
      "93..100 targetNodeGeneration(nonzero-u64)", "101 targetOwnerId.length(u8)=7",
      "102..108 targetOwnerId UTF-8 bytes", "109..116 targetOwnerLeaseGeneration(nonzero-u64)",
      "117..120 pendingCapacityDelta(nonzero-u32)", "121..128 deadlineUnixMs(nonzero-u64)",
    ],
  },
  canonical: { name: "userSpotCreate", decoded: userSpotCreateDecoded, hex: Buffer.from(userSpotCreateBytes).toString("hex") },
  malformed: [
    { name: "zeroCorrelation", hex: malformedHex(userSpotCreateBytes, (bytes) => bytes.fill(0, 5, 13)), error: "invalid-field" },
    { name: "truncatedDeadline", hex: Buffer.from(userSpotCreateBytes.slice(0, -1)).toString("hex"), error: "truncated-field" },
    { name: "trailingByte", hex: Buffer.from([...userSpotCreateBytes, 0]).toString("hex"), error: "trailing-byte" },
  ],
};

const userSpotCloseDecoded = {
  ...commonTerminalOperation,
  target: {
    version: "1",
    spot: { spotId: "spot-a", objectGeneration: "5" },
    targetNodeRid: "647374",
    targetNodeGeneration: "7",
    expectedAuthorityOwnerGeneration: "6",
    expectedStoreVersion: "store-v1",
  },
  deadlineUnixMs: "10",
};
const userSpotCloseTargetBody = [
  ...sized8(userSpotCloseDecoded.target.spot.spotId),
  ...u64(userSpotCloseDecoded.target.spot.objectGeneration),
  ...bytes8(Buffer.from(userSpotCloseDecoded.target.targetNodeRid, "hex")),
  ...u64(userSpotCloseDecoded.target.targetNodeGeneration),
  ...u64(userSpotCloseDecoded.target.expectedAuthorityOwnerGeneration),
  ...sized16(userSpotCloseDecoded.target.expectedStoreVersion),
];
const userSpotCloseBytes = serviceCommand(48, [
  ...terminalOperationPrefix(userSpotCloseDecoded),
  1, ...u16(userSpotCloseTargetBody.length), ...userSpotCloseTargetBody,
  ...u64(userSpotCloseDecoded.deadlineUnixMs),
]);
const userSpotCloseFixture = {
  format: "user-spot-close-v1",
  consumers: ["cpp", "dotnet", "jvm", "node"],
  commandId: 48,
  notes: {
    encoding: "complete service-wire command 48 frame; all integers are big-endian; target is user-spot-close-fence-v1 version 1 with a u16 exact body length; payload and trailing bytes are forbidden",
    offsetConvention: "inclusive zero-based byte offsets",
    offsets: [
      "0..1 magic=ZM", "2 wireMajor=1", "3 commandId=48", "4 flags=0",
      "5..12 correlation(nonzero-u64)", "13..20 operation.high(u64)",
      "21..28 operation.low(u64)", "29 sourceNodeRid.length(u8)=3",
      "30..32 sourceNodeRid bytes", "33..40 sourceNodeGeneration(nonzero-u64)",
      "41 target.version(u8)=1", "42..43 target.bodyLength(u16)=45",
      "44 target.spot.spotId.length(u8)=6", "45..50 target.spot.spotId UTF-8 bytes",
      "51..58 target.spot.objectGeneration(nonzero-u64)", "59 target.targetNodeRid.length(u8)=3",
      "60..62 target.targetNodeRid bytes", "63..70 target.targetNodeGeneration(nonzero-u64)",
      "71..78 target.expectedAuthorityOwnerGeneration(nonzero-u64)",
      "79..80 target.expectedStoreVersion.length(u16)=8", "81..88 target.expectedStoreVersion UTF-8 bytes",
      "89..96 deadlineUnixMs(nonzero-u64)",
    ],
  },
  canonical: { name: "userSpotClose", decoded: userSpotCloseDecoded, hex: Buffer.from(userSpotCloseBytes).toString("hex") },
  malformed: [
    { name: "shortTargetBodyLength", hex: malformedHex(userSpotCloseBytes, (bytes) => { bytes[43] -= 1; }), error: "invalid-body-length" },
    { name: "zeroTargetNodeGeneration", hex: malformedHex(userSpotCloseBytes, (bytes) => bytes.fill(0, 63, 71)), error: "invalid-field" },
    { name: "trailingByte", hex: Buffer.from([...userSpotCloseBytes, 0]).toString("hex"), error: "trailing-byte" },
  ],
};

const actorCreateDecoded = {
  ...commonTerminalOperation,
  actorId: "actor-a",
  stableType: "player",
  reservation: canonicalReservation,
  deadlineUnixMs: "10",
};
const actorCreateBytes = serviceCommand(49, [
  ...terminalOperationPrefix(actorCreateDecoded),
  ...sized8(actorCreateDecoded.actorId),
  ...sized8(actorCreateDecoded.stableType),
  ...reservationBytes(actorCreateDecoded.reservation),
  ...u64(actorCreateDecoded.deadlineUnixMs),
]);
const actorCreateFixture = {
  format: "actor-create-v1",
  consumers: ["cpp", "dotnet", "jvm", "node"],
  commandId: 49,
  notes: {
    encoding: "complete service-wire command 49 frame; all integers are big-endian; body follows the schema field order; payload and trailing bytes are forbidden",
    offsetConvention: "inclusive zero-based byte offsets",
    offsets: [
      "0..1 magic=ZM", "2 wireMajor=1", "3 commandId=49", "4 flags=0",
      "5..12 correlation(nonzero-u64)", "13..20 operation.high(u64)",
      "21..28 operation.low(u64)", "29 sourceNodeRid.length(u8)=3",
      "30..32 sourceNodeRid bytes", "33..40 sourceNodeGeneration(nonzero-u64)",
      "41 actorId.length(u8)=7", "42..48 actorId UTF-8 bytes",
      "49 stableType.length(u8)=6", "50..55 stableType UTF-8 bytes",
      "56 reservationId.length(u8)=9", "57..65 reservationId UTF-8 bytes",
      "66..67 expectedStoreVersion.length(u16)=8", "68..75 expectedStoreVersion UTF-8 bytes",
      "76..83 objectGeneration(nonzero-u64)", "84..91 authorityOwnerGeneration(nonzero-u64)",
      "92 targetNodeRid.length(u8)=3", "93..95 targetNodeRid bytes",
      "96..103 targetNodeGeneration(nonzero-u64)", "104 targetOwnerId.length(u8)=7",
      "105..111 targetOwnerId UTF-8 bytes", "112..119 targetOwnerLeaseGeneration(nonzero-u64)",
      "120..123 pendingCapacityDelta(nonzero-u32)", "124..131 deadlineUnixMs(nonzero-u64)",
    ],
  },
  canonical: { name: "actorCreate", decoded: actorCreateDecoded, hex: Buffer.from(actorCreateBytes).toString("hex") },
  malformed: [
    { name: "zeroOperationId", hex: malformedHex(actorCreateBytes, (bytes) => bytes.fill(0, 13, 29)), error: "invalid-field" },
    { name: "truncatedDeadline", hex: Buffer.from(actorCreateBytes.slice(0, -1)).toString("hex"), error: "truncated-field" },
    { name: "trailingByte", hex: Buffer.from([...actorCreateBytes, 0]).toString("hex"), error: "trailing-byte" },
  ],
};

const zljrMetadata = {
  Request: {
    ActorId: "actor-a", ActorType: "sample.Actor",
    HandoffId: "00112233445566778899aabbccddeeff",
    BoundSessionNodeRid: null, BoundSessionRid: null,
    RelocationContentType: "application/vnd.zlink.actor-relocation.snapshot",
    RelocationReference: "pending", RelocationChecksumCrc32c: 0,
    RelocationAggregateId: "00112233-4455-6677-8899-aabbccddeeff",
    RelocationAggregateGeneration: 1,
    RelocationInventoryDigest: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
    RequestContentType: "application/json", Request: "", HandoffFrames: [],
    SourceSpotId: "source-spot", SourceNodeRid: "c3Jj", ActorGeneration: 7,
    ActorAuthorityOwnerGeneration: 11, BoundSessionBindingToken: null,
    BoundSessionBindingGeneration: 0, BoundSessionObjectGeneration: 0,
    BoundSessionAuthorityOwnerGeneration: 0, BoundSessionMeshName: null,
    BoundSessionTargetNodeGeneration: 0, BoundSessionOwnerLeaseGeneration: 0,
    BoundSessionOwnerNodeGeneration: 0, BoundSessionAcceptedHighWater: 0,
    BoundSessionSessionOwnerId: null, BoundSessionSessionOwnerLeaseGeneration: 0,
    ReservationToken: "00112233445566778899aabbccddeeff",
    ReservedPayloadBytes: 83951618, TargetNodeRid: "ZHN0", TargetNodeGeneration: 19,
    TargetSpotGeneration: 23, TargetAuthorityOwnerGeneration: 12,
    TargetSpotAuthorityOwnerGeneration: 29, RelocationCoordinatorOwnerId: "owner-a",
    RelocationCoordinatorLeaseGeneration: 17, RelocationCoordinatorNodeRid: "c3Jj",
    RelocationCoordinatorNodeGeneration: 13,
    RelocationCoordinatorExpectedAuthorityStoreVersion: "store-v1",
    ActorNodeGeneration: 13, ExpectedOwnerLeaseGeneration: 17,
  },
  TargetSpotId: "target-spot", TargetNodeRid: "ZHN0", TargetNodeGeneration: 19,
  TargetSpotGeneration: 23, TargetAuthorityOwnerGeneration: 12,
  OperationIdHigh: 31, OperationIdLow: 37, ReplyContentType: "application/json", Reply: "",
};
const zljrRequest = Buffer.from("{}", "utf8");
const zljrReply = Buffer.from("[]", "utf8");
const zljrMetadataBytes = Buffer.from(JSON.stringify(zljrMetadata), "utf8");
const zljrPayload = Buffer.from([
  ...u32(0x5a4c4a52), 1, ...u32(zljrMetadataBytes.length),
  ...u32(zljrRequest.length), ...u32(zljrReply.length),
  ...zljrMetadataBytes, ...zljrRequest, ...zljrReply,
]);
const zljrSourceBody = [
  ...sized8("737263"), ...u64(13), ...sized8("owner-a"), ...u64(17),
];
const zljrApplicationBody = [
  ...sized8("__zlink.actor.routed_join.recovery"),
  ...sized8("application/x-zlink-actor-routed-join-recovery-v1"),
  ...u32(zljrPayload.length), ...zljrPayload,
];
const zljrBytes = [
  1, 1, ...u16(zljrSourceBody.length), ...zljrSourceBody, 0,
  ...u64(0), ...u64(0), ...u32(0), ...u16(0),
  1, ...u32(zljrApplicationBody.length), ...zljrApplicationBody,
];
const zljrSha256 = crypto.createHash("sha256").update(Buffer.from(zljrBytes)).digest("hex");
if (zljrBytes.length !== 1958
    || zljrSha256 !== "0c8cd156c73c23e785dc63fb2979041cbae63511fa046edfd6bb76ce6adbe08a") {
  throw new Error("generated ZLJR vector diverges from the fixed cross-language Node vector");
}
const zljrFixture = {
  format: "zljr-v1",
  consumers: ["cpp", "dotnet", "jvm", "node"],
  notes: {
    encoding: "canonical frozen-record nodeSend carrying application-payload-envelope-v1; its payload is ZLJR magic, version 1, three big-endian u32 lengths, canonical JSON metadata, request bytes and reply bytes",
    provenance: "exact fixed Node byte vector pinned byte-for-byte by the cross-language Actor Join recovery codec tests",
    sha256: zljrSha256,
    offsetConvention: "inclusive zero-based byte offsets",
    offsets: [
      "0 recordKind=nodeSend(1)", "1 sourceKind=node(1)", "2..3 source.bodyLength(u16)=31",
      "4 sourceNodeRid.length(u8)=6", "5..10 sourceNodeRid storage-hex UTF-8 bytes",
      "11..18 sourceNodeGeneration(nonzero-u64)", "19 sourceOwnerId.length(u8)=7",
      "20..26 sourceOwnerId UTF-8 bytes", "27..34 sourceOwnerLeaseGeneration(nonzero-u64)",
      "35 hasMetadata(bool8)=false", "36..43 operationId.high(u64)=0", "44..51 operationId.low(u64)=0",
      "52..55 operationKind(u32)=none(0)", "56..57 replyRoute.bodyLength(u16)=0",
      "58 applicationPayload.version(u8)=1", "59..62 applicationPayload.bodyLength(u32)=1895",
      "63 packetName.length(u8)=34", "64..97 packetName UTF-8 bytes",
      "98 contentType.length(u8)=49", "99..147 contentType UTF-8 bytes",
      "148..151 payload.length(u32)=1806", "152..155 ZLJR magic=5a4c4a52", "156 ZLJR version=1",
      "157..160 metadata.length(u32)=1785", "161..164 request.length(u32)=2",
      "165..168 reply.length(u32)=2", "169..1953 metadata canonical JSON UTF-8 bytes",
      "1954..1955 request bytes", "1956..1957 reply bytes",
    ],
  },
  canonical: {
    name: "nodeFrozenActorJoinRecovery",
    decoded: {
      source: { nodeRid: "737263", nodeGeneration: "13", ownerId: "owner-a", ownerLeaseGeneration: "17" },
      packetName: "__zlink.actor.routed_join.recovery",
      contentType: "application/x-zlink-actor-routed-join-recovery-v1",
      metadata: zljrMetadata,
      requestHex: zljrRequest.toString("hex"),
      replyHex: zljrReply.toString("hex"),
    },
    hex: Buffer.from(zljrBytes).toString("hex"),
  },
  malformed: [
    { name: "wrongZljrMagic", hex: malformedHex(zljrBytes, (bytes) => { bytes[152] = 0; }), error: "invalid-zljr-header" },
    { name: "metadataLengthMismatch", hex: malformedHex(zljrBytes, (bytes) => { bytes[160] += 1; }), error: "invalid-zljr-length" },
    { name: "truncatedReply", hex: Buffer.from(zljrBytes.slice(0, -1)).toString("hex"), error: "truncated-field" },
    { name: "trailingByte", hex: Buffer.from([...zljrBytes, 0]).toString("hex"), error: "trailing-byte" },
  ],
};

const probeId = 0x0102030405060708n;
const probe = [...head(5), ...u64(probeId)];
const ack = [...head(6), ...u64(probeId)];
const fixtures = {
  schema: "zlink-service-wire-decoder-fixtures-v1",
  wireMajor: schema.protocol.wireMajor,
  probeId: probeId.toString(),
  canonical: [
    { name: "livenessProbe", commandId: 5, bytes: probe },
    { name: "livenessAck", commandId: 6, bytes: ack, echoesProbeIdFrom: "livenessProbe" },
  ],
  malformed: [
    { name: "wrongMagic", bytes: [0, schema.protocol.magic[1], ...probe.slice(2)], error: "invalid-magic" },
    { name: "unknownCommand", bytes: [...head(7), ...u64(probeId)], error: "unknown-command" },
    { name: "forbiddenFlag", bytes: [...head(5, 1), ...u64(probeId)], error: "forbidden-flag" },
    { name: "zeroProbeId", bytes: [...head(5), ...u64(0n)], error: "invalid-field" },
    { name: "truncatedProbeId", bytes: probe.slice(0, -1), error: "truncated-field" },
    { name: "trailingByte", bytes: [...probe, 0], error: "trailing-byte" },
  ],
  frameworkErrors: {
    publicMapping: terminalFailureIntegrity.publicMapping,
    canonical: frameworkErrors.map((entry) => ({
      name: entry.name,
      wireValue: entry.value,
      publicValue: entry.name === "none" ? null : entry.value - 1,
    })),
    reservedWireValues: terminalFailureIntegrity.reservedWireValues,
    malformed: [
      {
        name: "reservedPublicOnlyFirst",
        wireValue: terminalFailureIntegrity.reservedWireValues.first,
        error: "unknown-framework-error",
      },
      {
        name: "reservedPublicOnlyLast",
        wireValue: terminalFailureIntegrity.reservedWireValues.last,
        error: "unknown-framework-error",
      },
      {
        name: "unknownAfterRelocationDataLost",
        wireValue: frameworkErrors.at(-1).value + 1,
        error: "unknown-framework-error",
      },
    ],
  },
};

const replacedCanonical = [
  ...head(51),
  ...sized8("actor-a"),
  ...u64(1n),
  ...sized8("actor-owner"),
  ...u64(2n),
  ...u64(3n),
  ...u64(4n),
  ...sized8("session-owner"),
  ...u64(5n),
  ...sized8("session-runtime"),
  ...u64(6n),
  ...sized8("session-a"),
  ...u64(7n),
];
const replacedSessionOwnerGenerationOffset = 5
  + sized8("actor-a").length + 8
  + sized8("actor-owner").length + 8 + 8 + 8
  + sized8("session-owner").length;
const replacedRetiredGenerationOffset = replacedCanonical.length - 8;
const replacedFixture = {
  schema: "zlink-bound-session-replaced-v1",
  commandId: 51,
  canonical: {
    actorAuthority: {
      actorId: "actor-a",
      objectGeneration: "1",
      targetNodeRid: "actor-owner",
      targetNodeGeneration: "2",
      expectedAuthorityOwnerGeneration: "3",
      expectedOwnerLeaseGeneration: "4",
    },
    retiredSession: {
      sessionOwnerNodeRid: "session-owner",
      sessionOwnerNodeGeneration: "5",
      sessionOwnerId: "session-runtime",
      sessionOwnerLeaseGeneration: "6",
      sessionRid: "session-a",
      retiredBindingGeneration: "7",
    },
    bytes: replacedCanonical,
  },
  malformed: [
    {
      name: "zeroSessionOwnerNodeGeneration",
      bytes: replacedCanonical.map((byte, index) => (
        index >= replacedSessionOwnerGenerationOffset
          && index < replacedSessionOwnerGenerationOffset + 8 ? 0 : byte
      )),
      error: "invalid-field",
    },
    {
      name: "zeroRetiredBindingGeneration",
      bytes: replacedCanonical.map((byte, index) => (
        index >= replacedRetiredGenerationOffset ? 0 : byte
      )),
      error: "invalid-field",
    },
    { name: "truncatedRetiredBindingGeneration", bytes: replacedCanonical.slice(0, -1), error: "truncated-field" },
    { name: "trailingByte", bytes: [...replacedCanonical, 0], error: "trailing-byte" },
  ],
  receiverFenceCases: [
    { name: "exactCurrentLifecycle", sessionOwnerNodeGeneration: "5", result: "apply" },
    { name: "preRestartLifecycle", sessionOwnerNodeGeneration: "4", result: "ignore-stale" },
  ],
};

const cpp = `// Generated from service-wire-v1.schema.json. Do not edit.\n#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace zlink::framework::runtime::protocol {\ninline constexpr std::uint8_t magic[] = {${schema.protocol.magic.join(", ")}};\ninline constexpr std::uint8_t wire_major = ${schema.protocol.wireMajor};\ninline constexpr const char required_capability[] = "${schema.protocol.requiredCapability}";\ninline constexpr const char framework_multipart_packet_name[] = "${frameworkMultipartV1Profile.packetName}";\ninline constexpr const char framework_multipart_content_type[] = "${frameworkMultipartV1Profile.contentType}";\nenum class command : std::uint8_t {\n${commands.map((entry) => `    ${entry.name} = ${entry.id},`).join("\n")}\n};\nenum class flag : std::uint8_t {\n${flags.map((entry) => `    ${entry.name} = ${entry.bit},`).join("\n")}\n};\nenum class framework_error_code : std::uint32_t {\n${frameworkErrors.map((entry) => `    ${entry.name} = ${entry.value},`).join("\n")}\n};\nenum class request_terminal_result : std::uint32_t {\n${requestTerminalResults.map((entry) => `    ${entry.name} = ${entry.value},`).join("\n")}\n};\ninline constexpr bool valid_terminal_failure(\n  std::uint32_t terminal, framework_error_code failure) noexcept\n{\n    const auto result = static_cast<request_terminal_result>(terminal);\n    if (result == request_terminal_result::ok)\n        return failure == framework_error_code::none;\n    switch (result) {\n${cppBoundaryTerminalCases}\n            return failure == framework_error_code::none;\n        default:\n            break;\n    }\n    if (failure == framework_error_code::none)\n        return false;\n    switch (failure) {\n${cppTypedTerminalCases}\n        default:\n            return false;\n    }\n}\n${numericBounds.map((entry) => `inline constexpr std::uint64_t ${entry.name} = ${entry.value}ULL;`).join("\n")}\n} // namespace zlink::framework::runtime::protocol\n`;

const dotnet = `// Generated from service-wire-v1.schema.json. Do not edit.\nnamespace Systems.Zlink.Framework.Runtime.Protocol;\n\ninternal static class ServiceWireConstants\n{\n    internal const byte Magic0 = ${schema.protocol.magic[0]};\n    internal const byte Magic1 = ${schema.protocol.magic[1]};\n    internal const byte WireMajor = ${schema.protocol.wireMajor};\n    internal const string RequiredCapability = "${schema.protocol.requiredCapability}";\n    internal const string FrameworkMultipartPacketName = "${frameworkMultipartV1Profile.packetName}";\n    internal const string FrameworkMultipartContentType = "${frameworkMultipartV1Profile.contentType}";\n    internal enum Command : byte\n    {\n${commands.map((entry) => `        ${pascal(entry.name)} = ${entry.id},`).join("\n")}\n    }\n    [System.Flags]\n    internal enum Flag : byte\n    {\n        None = 0,\n${flags.map((entry) => `        ${pascal(entry.name)} = ${entry.bit},`).join("\n")}\n    }\n    internal enum FrameworkErrorCode : uint\n    {\n${frameworkErrors.map((entry) => `        ${pascal(entry.name)} = ${entry.value},`).join("\n")}\n    }\n    //  Schema terminal-failure-integrity: success is ok+none, boundary\n    //  terminals carry none, typed failures must match their exact terminal,\n    //  and an unknown failure code is a protocol error before dispatch.\n    internal static bool ValidTerminalFailure(uint terminal, uint failureCode)\n    {\n        if (terminal == ${terminalValueByName.get("ok")}u)\n            return failureCode == 0u;\n        switch (terminal)\n        {\n${dotnetBoundaryCases}\n                return failureCode == 0u;\n        }\n        if (failureCode == 0u)\n            return false;\n        switch (failureCode)\n        {\n${dotnetTypedCases}\n            default:\n                return false;\n        }\n    }\n}\n`;

const java = `// Generated from service-wire-v1.schema.json. Do not edit.\npackage systems.zlink.framework.runtime.protocol;\n\npublic final class ServiceWireConstants {\n    public static final int MAGIC_0 = ${schema.protocol.magic[0]};\n    public static final int MAGIC_1 = ${schema.protocol.magic[1]};\n    public static final int WIRE_MAJOR = ${schema.protocol.wireMajor};\n    public static final String REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}";\n    public static final String FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}";\n    public static final String FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}";\n${commands.map((entry) => `    public static final int COMMAND_${snake(entry.name)} = ${entry.id};`).join("\n")}\n${flags.map((entry) => `    public static final int FLAG_${snake(entry.name)} = ${entry.bit};`).join("\n")}\n${frameworkErrors.map((entry) => `    public static final long FRAMEWORK_ERROR_${snake(entry.name)} = ${entry.value}L;`).join("\n")}\n    //  Schema terminal-failure-integrity: success is ok+none, boundary\n    //  terminals carry none, typed failures must match their exact terminal,\n    //  and an unknown failure code is a protocol error before dispatch.\n    public static boolean validTerminalFailure(long terminal, long failureCode) {\n        if (terminal == ${terminalValueByName.get("ok")}L) {\n            return failureCode == 0L;\n        }\n        switch ((int) terminal) {\n${javaBoundaryCases}\n                return failureCode == 0L;\n            default:\n                break;\n        }\n        if (failureCode == 0L) {\n            return false;\n        }\n        switch ((int) failureCode) {\n${javaTypedCases}\n            default:\n                return false;\n        }\n    }\n    private ServiceWireConstants() {}\n}\n`;

const node = `// Generated from service-wire-v1.schema.json. Do not edit.\nexport const SERVICE_WIRE_MAGIC = [${schema.protocol.magic.join(", ")}] as const;\nexport const SERVICE_WIRE_MAJOR = ${schema.protocol.wireMajor} as const;\nexport const SERVICE_WIRE_REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}" as const;\nexport const SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}" as const;\nexport const SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}" as const;\nexport const ServiceWireCommand = {\n${commands.map((entry) => `  ${entry.name}: ${entry.id},`).join("\n")}\n} as const;\nexport const ServiceWireFlag = {\n${flags.map((entry) => `  ${entry.name}: ${entry.bit},`).join("\n")}\n} as const;\nexport const ServiceWireFrameworkErrorCode = {\n${frameworkErrors.map((entry) => `  ${entry.name}: ${entry.value},`).join("\n")}\n} as const;\nexport const ServiceWireBoundaryTerminalResults = [${boundaryTerminalValues.join(", ")}] as const;\nexport const ServiceWireExactTerminalByFailureCode = {\n${nodeExactPairEntries.map((entry) => `  ${entry.value}: ${entry.terminalValue}, // ${entry.name}`).join("\n")}\n} as const;\n// Schema terminal-failure-integrity: success is ok+none, boundary terminals\n// carry none, typed failures must match their exact terminal, and an unknown\n// failure code is a protocol error before dispatch.\nexport const isValidServiceWireTerminalFailure = (\n  terminal: number,\n  failureCode: number\n): boolean => {\n  if (terminal === ${terminalValueByName.get("ok")}) {\n    return failureCode === 0;\n  }\n  if ((ServiceWireBoundaryTerminalResults as readonly number[]).includes(terminal)) {\n    return failureCode === 0;\n  }\n  if (failureCode === 0) {\n    return false;\n  }\n  const expected = (ServiceWireExactTerminalByFailureCode as Record<number, number>)[failureCode];\n  return expected !== undefined && expected === terminal;\n};\n`;

const nodeJs = `"use strict";\nObject.defineProperty(exports, "__esModule", { value: true });\nexports.isValidServiceWireTerminalFailure = exports.ServiceWireExactTerminalByFailureCode = exports.ServiceWireBoundaryTerminalResults = exports.ServiceWireFrameworkErrorCode = exports.ServiceWireFlag = exports.ServiceWireCommand = exports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = exports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = exports.SERVICE_WIRE_REQUIRED_CAPABILITY = exports.SERVICE_WIRE_MAJOR = exports.SERVICE_WIRE_MAGIC = void 0;\n// Generated from service-wire-v1.schema.json. Do not edit.\nexports.SERVICE_WIRE_MAGIC = [${schema.protocol.magic.join(", ")}];\nexports.SERVICE_WIRE_MAJOR = ${schema.protocol.wireMajor};\nexports.SERVICE_WIRE_REQUIRED_CAPABILITY = "${schema.protocol.requiredCapability}";\nexports.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME = "${frameworkMultipartV1Profile.packetName}";\nexports.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE = "${frameworkMultipartV1Profile.contentType}";\nexports.ServiceWireCommand = {\n${commands.map((entry) => `    ${entry.name}: ${entry.id},`).join("\n")}\n};\nexports.ServiceWireFlag = {\n${flags.map((entry) => `    ${entry.name}: ${entry.bit},`).join("\n")}\n};\nexports.ServiceWireFrameworkErrorCode = {\n${frameworkErrors.map((entry) => `    ${entry.name}: ${entry.value},`).join("\n")}\n};\nexports.ServiceWireBoundaryTerminalResults = [${boundaryTerminalValues.join(", ")}];\nexports.ServiceWireExactTerminalByFailureCode = {\n${nodeExactPairEntries.map((entry) => `    ${entry.value}: ${entry.terminalValue},`).join("\n")}\n};\n// Schema terminal-failure-integrity: success is ok+none, boundary terminals\n// carry none, typed failures must match their exact terminal, and an unknown\n// failure code is a protocol error before dispatch.\nconst isValidServiceWireTerminalFailure = (terminal, failureCode) => {\n    if (terminal === ${terminalValueByName.get("ok")}) {\n        return failureCode === 0;\n    }\n    if (exports.ServiceWireBoundaryTerminalResults.includes(terminal)) {\n        return failureCode === 0;\n    }\n    if (failureCode === 0) {\n        return false;\n    }\n    const expected = exports.ServiceWireExactTerminalByFailureCode[failureCode];\n    return expected !== undefined && expected === terminal;\n};\nexports.isValidServiceWireTerminalFailure = isValidServiceWireTerminalFailure;\n`;

const nodeDeclarations = `export declare const SERVICE_WIRE_MAGIC: readonly [${schema.protocol.magic.join(", ")}];\nexport declare const SERVICE_WIRE_MAJOR: ${schema.protocol.wireMajor};\nexport declare const SERVICE_WIRE_REQUIRED_CAPABILITY: "${schema.protocol.requiredCapability}";\nexport declare const SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME: "${frameworkMultipartV1Profile.packetName}";\nexport declare const SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE: "${frameworkMultipartV1Profile.contentType}";\nexport declare const ServiceWireCommand: {\n${commands.map((entry) => `    readonly ${entry.name}: ${entry.id};`).join("\n")}\n};\nexport declare const ServiceWireFlag: {\n${flags.map((entry) => `    readonly ${entry.name}: ${entry.bit};`).join("\n")}\n};\nexport declare const ServiceWireFrameworkErrorCode: {\n${frameworkErrors.map((entry) => `    readonly ${entry.name}: ${entry.value};`).join("\n")}\n};\nexport declare const ServiceWireBoundaryTerminalResults: readonly [${boundaryTerminalValues.join(", ")}];\nexport declare const ServiceWireExactTerminalByFailureCode: {\n${nodeExactPairEntries.map((entry) => `    readonly ${entry.value}: ${entry.terminalValue};`).join("\n")}\n};\nexport declare const isValidServiceWireTerminalFailure: (terminal: number, failureCode: number) => boolean;\n`;

const outputs = new Map([
  [path.join(root, "generated/cpp/service_wire_constants.hpp"), cpp],
  [path.join(root, "generated/dotnet/ServiceWireConstants.g.cs"), dotnet],
  [path.join(root, "generated/jvm/ServiceWireConstants.java"), java],
  [path.join(root, "generated/node/service_wire_constants.ts"), node],
  [path.join(root, "generated/node/service_wire_constants.js"), nodeJs],
  [path.join(root, "generated/node/service_wire_constants.d.ts"), nodeDeclarations],
  [path.join(
    root,
    "../../languages/node/packages/framework/src/runtime/foundation/service-wire-constants.generated.ts",
  ), node],
  [path.join(root, "golden/service-decoder-fixtures-v1.json"), `${JSON.stringify(fixtures, null, 2)}\n`],
  [path.join(root, "golden/bound-session-replaced-v1.json"), `${JSON.stringify(replacedFixture, null, 2)}\n`],
  [path.join(root, "golden/user-spot-create-v1.json"), `${JSON.stringify(userSpotCreateFixture, null, 2)}\n`],
  [path.join(root, "golden/user-spot-close-v1.json"), `${JSON.stringify(userSpotCloseFixture, null, 2)}\n`],
  [path.join(root, "golden/actor-create-v1.json"), `${JSON.stringify(actorCreateFixture, null, 2)}\n`],
  [path.join(root, "golden/zljr-v1.json"), `${JSON.stringify(zljrFixture, null, 2)}\n`],
]);

let failed = false;
for (const [output, content] of outputs) {
  if (check) {
    if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== content) {
      console.error(`generated service wire asset is stale: ${path.relative(root, output)}`);
      failed = true;
    }
    continue;
  }
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, content);
}
if (failed) process.exit(1);
const fixtureCount = fixtures.canonical.length + fixtures.malformed.length
  + fixtures.frameworkErrors.canonical.length + fixtures.frameworkErrors.malformed.length
  + 4;
console.log(`${check ? "verified" : "generated"} service wire assets: commands=${commands.length} flags=${flags.length} fixtures=${fixtureCount}`);

import fs from "node:fs";

const root = "/home/hep7/project/zlink/framework/runtime/protocol";
const golden = JSON.parse(fs.readFileSync(`${root}/golden/relocation-envelope-v1.json`, "utf8"));
const logical = Buffer.from(golden.logicalHex, "hex");
const d = golden.decoded;

// ---- Compute the byte offset where the single savedWork frozenRecord blob begins,
// and its length, using pure arithmetic over fields whose encoding we know fully
// (everything the generated encoder itself computes deterministically), leaving
// only the opaque self-delimiting frozen-record bytes to be sliced out of the
// golden buffer verbatim (exactly how the real generated encoder treats it: it
// appends frozenRecord bytes with no length prefix of its own).

function utf8Len(s) { return Buffer.byteLength(s, "utf8"); }

let offset = 0;
offset += 8 + 8; // relocationHigh, relocationLow
offset += 1; // objectKind tag byte (userSpot = 2)
// body16: 2-byte length + content(text8 spotId + u64 + u64)
const spotIdBytes = utf8Len(d.object.spotIdUtf8Fixture);
const objectBody16Len = (1 + spotIdBytes) + 8 + 8;
offset += 2 + objectBody16Len;
offset += 8; // applicationVersion

offset += 4; // applicationStates u32 count
for (const s of d.applicationStates) {
  offset += 8; // participantId
  offset += 1; // hasState byte
  // body64: 8-byte length + content; content = bytes64(payload) only if hasState
  let bodyLen = 0;
  if (s.applicationState.hasState) {
    const payloadBytes = utf8Len(s.applicationState.payloadUtf8Fixture);
    bodyLen = 8 + payloadBytes; // bytes64 = u64 length + raw bytes
  }
  offset += 8 + bodyLen;
}

offset += 4; // savedWork u32 count
if (d.savedWork.length !== 1) throw new Error("harness assumes exactly one savedWork entry");
const sw = d.savedWork[0];
offset += 8; // participantId
offset += 8; // order
const frozenRecordStart = offset;

// ---- Compute the fixed-size tail (timerRegistrations + pendingTimerTicks) length
// so we can derive the frozenRecord length by subtraction.
function timerRegLen(t) {
  return 8 + (1 + utf8Len(t.name)) + (1 + utf8Len(t.handlerType)) + 8 + 1 + 8 + 1 + 8 + 8 + 8;
}
function pendingTickLen(p) {
  return 8 + 8 + (1 + utf8Len(p.timerName)) + 8 + 8 + 8 + 8;
}
let tailLen = 4; // timerRegistrations u32 count
for (const t of d.timerRegistrations) tailLen += timerRegLen(t);
tailLen += 4; // pendingTimerTicks u32 count
for (const p of d.pendingTimerTicks) tailLen += pendingTickLen(p);

const frozenRecordLen = logical.length - frozenRecordStart - tailLen;
if (frozenRecordLen < 0) throw new Error("negative frozenRecord length; offset math is wrong");
const frozenRecordBytes = logical.subarray(frozenRecordStart, frozenRecordStart + frozenRecordLen);

console.log("frozenRecordStart:", frozenRecordStart, "frozenRecordLen:", frozenRecordLen);
console.log("frozenRecord hex:", Buffer.from(frozenRecordBytes).toString("hex"));

// ---- Build the mapped input for the real generated encoder.
const overrunPolicyMap = { skipLateTicks: 1, deliverImmediately: 2, coalesce: 3 };

const input = {
  relocationHigh: BigInt(d.relocationHigh),
  relocationLow: BigInt(d.relocationLow),
  object: {
    objectKind: "userSpot",
    spotId: d.object.spotIdUtf8Fixture,
    spotGeneration: BigInt(d.object.spotGeneration),
    expectedAuthorityOwnerGeneration: BigInt(d.object.expectedAuthorityOwnerGeneration),
  },
  applicationVersion: BigInt(d.applicationVersion),
  applicationStates: d.applicationStates.map((s) => ({
    participantId: BigInt(s.participantId),
    hasState: s.applicationState.hasState,
    payload: s.applicationState.hasState
      ? Uint8Array.from(Buffer.from(s.applicationState.payloadUtf8Fixture, "utf8"))
      : new Uint8Array(0),
  })),
  savedWork: d.savedWork.map((w) => ({
    participantId: BigInt(w.participantId),
    order: BigInt(w.order),
    frozenRecord: Uint8Array.from(frozenRecordBytes),
  })),
  timerRegistrations: d.timerRegistrations.map((t) => ({
    participantId: BigInt(t.participantId),
    name: t.name,
    handlerType: t.handlerType,
    periodMilliseconds: BigInt(t.periodMilliseconds),
    overrunPolicy: overrunPolicyMap[t.overrunPolicy],
    maxCatchUpTicks: BigInt(t.maxCatchUpTicks),
    stopOnUnhandledException: t.stopOnUnhandledException,
    lastCompletedDeliveryIndex: BigInt(t.lastCompletedDeliveryIndex),
    lastCompletedScheduledIndex: BigInt(t.lastCompletedScheduledIndex),
    nextScheduledAtUnixMilliseconds: BigInt(t.nextScheduledAtUnixMilliseconds),
  })),
  pendingTimerTicks: d.pendingTimerTicks.map((p) => ({
    participantId: BigInt(p.participantId),
    order: BigInt(p.order),
    timerName: p.timerName,
    deliveryIndex: BigInt(p.deliveryIndex),
    scheduledIndex: BigInt(p.scheduledIndex),
    scheduledAtUnixMilliseconds: BigInt(p.scheduledAtUnixMilliseconds),
    skippedTicks: BigInt(p.skippedTicks),
  })),
};

const { encodeRelocationEnvelopeV1 } = await import(`${root}/generated/node/service_wire_pilot_codec.generated.ts`);
const encoded = encodeRelocationEnvelopeV1(input);
const encodedHex = Buffer.from(encoded).toString("hex");

console.log("encoded == golden.logicalHex:", encodedHex === golden.logicalHex);
if (encodedHex !== golden.logicalHex) {
  console.log("encoded:", encodedHex);
  console.log("golden :", golden.logicalHex);
}

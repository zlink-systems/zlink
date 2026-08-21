import { createHash } from 'node:crypto';
import type { ZLinkPlacementObjectKind, ZLinkAuthorityKey } from '../locations/internal-location-contracts';
import {
  decodeServiceWireFrozenRecordPrefix
} from './service-stateful-wire-codec';
import { decodeAuthorityKey } from '../locations/authority-key-codec';

export interface ServiceRelocationParticipant {
  /** Wire-local participant ordinal; identity is projected from Location Store. */
  readonly participantId?: bigint;
  /** Root projection carried by relocation-envelope-v1, never a participant identity vector. */
  readonly rootSpotId?: string;
  readonly rootSpotGeneration?: bigint;
  readonly rootOwnerGeneration?: bigint;
  readonly rootObjectKind?: ZLinkPlacementObjectKind;
  readonly key: string;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly applicationState: Uint8Array;
  readonly boundSessionState: Uint8Array;
  readonly queuedMessages: readonly ServiceRelocationQueuedMessage[];
  readonly timers: readonly ServiceRelocationTimer[];
}

export interface ServiceRelocationQueuedMessage {
  readonly sequence: bigint;
  readonly payload: Uint8Array;
}

export interface ServiceRelocationTimer {
  readonly timerId: string;
  readonly handlerType: string;
  readonly startedAtUnixMs: number;
  readonly dueAtUnixMs: number;
  readonly intervalMs: number;
  readonly deliveryIndex: bigint;
  readonly lastScheduledIndex: bigint;
  readonly overrunPolicy: string;
  readonly maxCatchUpTicks: number;
  readonly stopOnUnhandledException: boolean;
  readonly pendingTicks: readonly ServiceRelocationPendingTimerTick[];
}

export interface ServiceRelocationPendingTimerTick {
  readonly deliveryIndex: bigint;
  readonly scheduledIndex: bigint;
  readonly scheduledAtUnixMs: number;
  readonly skippedTicks: bigint;
}

export interface ServiceRelocationMembership {
  readonly actorKey: string;
  readonly spotKey: string;
  readonly spotObjectGeneration: bigint;
  readonly membershipEpoch: bigint;
}

export interface ServiceRelocationEnvelope {
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  /** Deployment ordinal carried by relocation-envelope-v1, distinct from staging identity. */
  readonly applicationVersion?: bigint;
  readonly participants: readonly ServiceRelocationParticipant[];
  readonly memberships: readonly ServiceRelocationMembership[];
}

export interface ServiceRelocationPublication {
  readonly reference: string;
  readonly checksumCrc32c: number;
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly inventoryDigest: string;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  /** True when the publication lives in the canonical ZLAU relocation slot. */
  readonly canonical?: boolean;
  readonly applicationVersion?: bigint;
}

export interface ServiceRelocationAuthorityCodec {
  publish(
    currentPayload: Uint8Array,
    publication: ServiceRelocationPublication
  ): Uint8Array;
  read(payload: Uint8Array): ServiceRelocationPublication | undefined;
  clear(currentPayload: Uint8Array, expectedReference: string): Uint8Array;
}

interface ServiceRelocationAuthorityEnvelope {
  readonly base: Buffer;
  readonly publication: ServiceRelocationPublication;
}

interface CanonicalAuthorityLayout {
  readonly flags: number;
  readonly body: Buffer;
  readonly slotStart: number;
  readonly slotEnd: number;
  readonly slot: Buffer;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
}

interface CanonicalRelocationSlot {
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly applicationVersion: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  readonly reference?: string;
  readonly checksumCrc32c?: number;
}

/** Deterministic Location authority wrapper for one immutable relocation root. */
export class ServiceRelocationAuthorityPayloadCodec
implements ServiceRelocationAuthorityCodec {
  publish(
    currentPayload: Uint8Array,
    publication: ServiceRelocationPublication
  ): Uint8Array {
    if (this.read(currentPayload) !== undefined) {
      throw new TypeError('Location authority already contains a relocation publication.');
    }
    const canonical = decodeCanonicalAuthorityLayout(currentPayload);
    if (canonical !== undefined) {
      return replaceCanonicalRelocationSlot(
        canonical,
        encodeCanonicalRelocationSlot(canonical, publication)
      );
    }
    return encodeAuthorityEnvelope(Buffer.from(currentPayload), publication);
  }

  read(payload: Uint8Array): ServiceRelocationPublication | undefined {
    return decodeCanonicalAuthorityPublication(payload)
      ?? decodeAuthorityEnvelope(payload)?.publication;
  }

  clear(currentPayload: Uint8Array, expectedReference: string): Uint8Array {
    const canonical = decodeCanonicalAuthorityLayout(currentPayload);
    const canonicalPublication = canonical === undefined
      ? undefined
      : decodeCanonicalRelocationSlot(canonical.slot);
    if (canonical !== undefined && canonicalPublication?.reference !== undefined) {
      if (canonicalPublication.reference !== requireText(
        expectedReference,
        'relocation reference'
      )) {
        throw new TypeError('Location authority relocation reference changed.');
      }
      return replaceCanonicalRelocationSlot(canonical, Buffer.alloc(0));
    }
    const current = decodeAuthorityEnvelope(currentPayload);
    if (
      current === undefined
      || current.publication.reference !== requireText(
        expectedReference,
        'relocation reference'
      )
    ) {
      throw new TypeError('Location authority relocation reference changed.');
    }
    return Buffer.from(current.base);
  }
}

/** Returns the application authority payload hidden by relocation metadata. */
export function serviceRelocationAuthorityApplicationPayload(
  payload: Uint8Array
): Buffer {
  const canonical = decodeCanonicalAuthorityLayout(payload);
  if (canonical !== undefined && canonical.slot.byteLength !== 0) {
    return replaceCanonicalRelocationSlot(canonical, Buffer.alloc(0));
  }
  return Buffer.from(decodeAuthorityEnvelope(payload)?.base ?? payload);
}

/** Reads the canonical relocation identity even before its root pointer exists. */
export function serviceRelocationAuthoritySlotIdentity(
  payload: Uint8Array
): Pick<
  ServiceRelocationPublication,
  'aggregateId' | 'aggregateGeneration' | 'applicationVersion'
> | undefined {
  const layout = decodeCanonicalAuthorityLayout(payload);
  const slot = layout === undefined || layout.slot.byteLength === 0
    ? undefined
    : decodeCanonicalRelocationSlot(layout.slot);
  return slot === undefined
    ? undefined
    : {
        aggregateId: slot.aggregateId,
        aggregateGeneration: slot.aggregateGeneration,
        applicationVersion: slot.applicationVersion
      };
}

/** Replaces the application payload without changing relocation metadata. */
export function replaceServiceRelocationAuthorityApplicationPayload(
  payload: Uint8Array,
  applicationPayload: Uint8Array
): Buffer {
  const canonical = decodeCanonicalAuthorityLayout(payload);
  if (canonical !== undefined && canonical.slot.byteLength !== 0) {
    const replacement = decodeCanonicalAuthorityLayout(applicationPayload);
    if (replacement === undefined || replacement.slot.byteLength !== 0) {
      throw new TypeError('Canonical authority application replacement is not steady ZLAU.');
    }
    return replaceCanonicalRelocationSlot(replacement, canonical.slot);
  }
  const current = decodeAuthorityEnvelope(payload);
  return current === undefined
    ? Buffer.from(applicationPayload)
    : encodeAuthorityEnvelope(Buffer.from(applicationPayload), current.publication);
}

/** Projects an embedded canonical relocation slot to the exact target-ready fence. */
export function projectServiceRelocationAuthorityTargetReady(
  payload: Uint8Array,
  target: {
    readonly targetAttemptGeneration: bigint;
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
    readonly ownerId: string;
    readonly ownerLeaseGeneration: bigint;
    readonly expectedStoreVersion: string;
  }
): Buffer {
  const layout = decodeCanonicalAuthorityLayout(payload);
  if (layout === undefined || layout.slot.byteLength === 0) {
    throw new TypeError('Canonical authority relocation slot is missing.');
  }
  const reader = new CanonicalReader(layout.slot);
  const relocationHigh = reader.u64();
  const relocationLow = reader.u64();
  reader.u64();
  const sourceNodeRid = reader.text8(false);
  const sourceNodeGeneration = reader.u64();
  const sourceOwnerId = reader.text8(false);
  const sourceOwnerLeaseGeneration = reader.u64();
  reader.text8(true); reader.u64(); reader.text8(true); reader.u64();
  reader.text8(false); reader.u64(); reader.text8(false); reader.u64();
  reader.u8();
  const applicationVersion = reader.i64();
  reader.u8();
  let aggregateGeneration = target.targetAttemptGeneration;
  let reference: string | undefined;
  let checksumCrc32c: number | undefined;
  if (!reader.done) {
    if (reader.u8() !== 1) {
      throw new TypeError('Canonical authority relocation extension is invalid.');
    }
    aggregateGeneration = reader.u64();
    if (!reader.done) reader.text8(true);
    if (!reader.done) {
      const pointerPresence = reader.u8();
      if (pointerPresence > 1) {
        throw new TypeError('Canonical authority relocation pointer is invalid.');
      }
      if (pointerPresence === 1) {
        reference = reader.text16();
        checksumCrc32c = reader.u32();
      }
    }
  }
  if (!reader.done || applicationVersion < 0n) {
    throw new TypeError('Canonical authority relocation slot is invalid.');
  }
  const slot = Buffer.concat([
    canonicalU64(relocationHigh), canonicalU64(relocationLow),
    canonicalU64(target.targetAttemptGeneration),
    canonicalText8(sourceNodeRid), canonicalU64(sourceNodeGeneration),
    canonicalText8(sourceOwnerId), canonicalU64(sourceOwnerLeaseGeneration),
    canonicalText8(target.nodeRid), canonicalU64(target.nodeGeneration),
    canonicalText8(target.ownerId), canonicalU64(target.ownerLeaseGeneration),
    canonicalText8(target.ownerId), canonicalU64(target.ownerLeaseGeneration),
    canonicalText8(target.nodeRid), canonicalU64(target.nodeGeneration),
    Buffer.of(5), canonicalI64(applicationVersion), Buffer.of(0),
    Buffer.of(1), canonicalU64(aggregateGeneration),
    canonicalText8(target.expectedStoreVersion),
    Buffer.of(reference === undefined ? 0 : 1),
    ...(reference === undefined || checksumCrc32c === undefined
      ? []
      : [canonicalText16(reference), canonicalU32(checksumCrc32c)])
  ]);
  return replaceCanonicalRelocationSlot(layout, slot);
}

function decodeCanonicalAuthorityPublication(
  payload: Uint8Array
): ServiceRelocationPublication | undefined {
  const layout = decodeCanonicalAuthorityLayout(payload);
  if (layout === undefined || layout.slot.byteLength === 0) return undefined;
  const slot = decodeCanonicalRelocationSlot(layout.slot);
  if (slot?.reference === undefined || slot.checksumCrc32c === undefined) return undefined;
  return {
    reference: slot.reference,
    checksumCrc32c: slot.checksumCrc32c,
    aggregateId: slot.aggregateId,
    aggregateGeneration: slot.aggregateGeneration,
    inventoryDigest: '0'.repeat(64),
    targetOwnerId: slot.targetOwnerId,
    targetOwnerLeaseGeneration: slot.targetOwnerLeaseGeneration,
    canonical: true,
    applicationVersion: slot.applicationVersion
  };
}

function decodeCanonicalAuthorityLayout(
  payload: Uint8Array
): CanonicalAuthorityLayout | undefined {
  try {
    const bytes = Buffer.from(payload);
    if (bytes.byteLength < 20 || bytes.byteLength > 1024 * 1024) return undefined;
    const reader = new CanonicalReader(bytes);
    reader.expect(Buffer.from('ZLAU'));
    if (reader.u8() !== 1) return undefined;
    const flags = reader.u16();
    const body = reader.take(reader.u32());
    const checksumOffset = reader.offset;
    if (reader.u32() !== crc32c(bytes.subarray(0, checksumOffset)) || !reader.done) {
      return undefined;
    }
    const bodyReader = new CanonicalReader(body);
    bodyReader.u8();
    bodyReader.u8();
    bodyReader.take(bodyReader.u16());
    const ownerId = bodyReader.text8(false);
    const ownerLeaseGeneration = bodyReader.u64();
    bodyReader.text8(false);
    const nodeRid = bodyReader.text8(false);
    const nodeGeneration = bodyReader.u64();
    const slotStart = bodyReader.offset;
    const presence = bodyReader.u8();
    if (presence > 1) return undefined;
    const slot = bodyReader.take(bodyReader.u32());
    const slotEnd = bodyReader.offset;
    if ((presence === 0) !== (slot.byteLength === 0)) return undefined;
    return {
      flags,
      body: Buffer.from(body),
      slotStart,
      slotEnd,
      slot: Buffer.from(slot),
      ownerId,
      ownerLeaseGeneration,
      nodeRid,
      nodeGeneration
    };
  } catch {
    return undefined;
  }
}

function decodeCanonicalRelocationSlot(
  payload: Uint8Array
): CanonicalRelocationSlot | undefined {
  try {
    const reader = new CanonicalReader(payload);
    const aggregateId = uuid(reader.u64(), reader.u64());
    if (aggregateId === undefined) return undefined;
    reader.u64();
    reader.text8(false);
    reader.u64();
    reader.text8(false);
    reader.u64();
    reader.text8(true);
    reader.u64();
    const targetOwnerId = reader.text8(true);
    const targetOwnerLeaseGeneration = reader.u64();
    reader.text8(false);
    reader.u64();
    reader.text8(false);
    reader.u64();
    const phase = reader.u8();
    const applicationVersion = reader.i64();
    if (phase < 1 || phase > 9 || applicationVersion < 0n) return undefined;
    reader.u8();
    let aggregateGeneration = 0n;
    let reference: string | undefined;
    let checksumCrc32c: number | undefined;
    if (!reader.done) {
      if (reader.u8() !== 1) return undefined;
      aggregateGeneration = reader.u64();
      if (!reader.done) reader.text8(true);
      if (!reader.done) {
        const pointerPresence = reader.u8();
        if (pointerPresence > 1) return undefined;
        if (pointerPresence === 1) {
          reference = reader.text16();
          checksumCrc32c = reader.u32();
        }
      }
    }
    if (!reader.done) return undefined;
    return {
      aggregateId,
      aggregateGeneration,
      applicationVersion,
      targetOwnerId,
      targetOwnerLeaseGeneration,
      ...(reference === undefined ? {} : { reference, checksumCrc32c })
    };
  } catch {
    return undefined;
  }
}

function encodeCanonicalRelocationSlot(
  layout: CanonicalAuthorityLayout,
  publication: ServiceRelocationPublication
): Buffer {
  validatePublication(publication);
  if (layout.slot.byteLength !== 0) {
    const current = decodeCanonicalRelocationSlot(layout.slot);
    if (current === undefined || current.reference !== undefined) {
      throw new TypeError('Canonical authority relocation slot is invalid or already published.');
    }
    if (current.aggregateId !== canonicalUuid(publication.aggregateId, 'aggregate id')) {
      throw new TypeError('Canonical authority relocation identity changed.');
    }
    const reader = new CanonicalReader(layout.slot);
    reader.u64(); reader.u64(); reader.u64();
    reader.text8(false); reader.u64(); reader.text8(false); reader.u64();
    reader.text8(true); reader.u64(); reader.text8(true); reader.u64();
    reader.text8(false); reader.u64(); reader.text8(false); reader.u64();
    reader.u8(); reader.i64(); reader.u8();
    let prefix: Buffer;
    let expectedStoreVersion = Buffer.of(0);
    if (reader.done) {
      prefix = Buffer.concat([
        layout.slot,
        Buffer.of(1),
        canonicalU64(publication.aggregateGeneration)
      ]);
    } else {
      if (reader.u8() !== 1) {
        throw new TypeError('Canonical authority relocation extension is invalid.');
      }
      reader.u64();
      prefix = Buffer.from(layout.slot.subarray(0, reader.offset));
      if (!reader.done) {
        const expectedStoreVersionStart = reader.offset;
        reader.text8(true);
        expectedStoreVersion = Buffer.from(
          layout.slot.subarray(expectedStoreVersionStart, reader.offset)
        );
      }
      if (!reader.done && (reader.u8() !== 0 || !reader.done)) {
        throw new TypeError('Canonical authority relocation pointer is invalid.');
      }
    }
    return Buffer.concat([
      prefix,
      expectedStoreVersion,
      Buffer.of(1),
      canonicalText16(publication.reference),
      canonicalU32(publication.checksumCrc32c)
    ]);
  }
  const id = relocationId(canonicalUuid(publication.aggregateId, 'aggregate id'));
  const nodeRid = requireText(layout.nodeRid, 'canonical authority node rid');
  const ownerId = requireText(layout.ownerId, 'canonical authority owner id');
  const applicationVersion = nonNegativeBigInt(
    publication.applicationVersion ?? 0n,
    'application version'
  );
  return Buffer.concat([
    canonicalU64(id.high), canonicalU64(id.low),
    canonicalU64(publication.aggregateGeneration),
    canonicalText8(nodeRid), canonicalU64(layout.nodeGeneration),
    canonicalText8(ownerId), canonicalU64(layout.ownerLeaseGeneration),
    canonicalText8(nodeRid), canonicalU64(layout.nodeGeneration),
    canonicalText8(publication.targetOwnerId),
    canonicalU64(publication.targetOwnerLeaseGeneration),
    canonicalText8(ownerId), canonicalU64(layout.ownerLeaseGeneration),
    canonicalText8(nodeRid), canonicalU64(layout.nodeGeneration),
    Buffer.of(9), canonicalI64(applicationVersion), Buffer.of(0),
    Buffer.of(1), canonicalU64(publication.aggregateGeneration),
    Buffer.of(0), Buffer.of(1),
    canonicalText16(publication.reference), canonicalU32(publication.checksumCrc32c)
  ]);
}

function replaceCanonicalRelocationSlot(
  layout: CanonicalAuthorityLayout,
  slot: Uint8Array
): Buffer {
  const encodedSlot = Buffer.from(slot);
  const body = Buffer.concat([
    layout.body.subarray(0, layout.slotStart),
    Buffer.of(encodedSlot.byteLength === 0 ? 0 : 1),
    canonicalU32(encodedSlot.byteLength),
    encodedSlot,
    layout.body.subarray(layout.slotEnd)
  ]);
  const envelope = Buffer.concat([
    Buffer.from('ZLAU'), Buffer.of(1), canonicalU16(layout.flags),
    canonicalU32(body.byteLength), body
  ]);
  const result = Buffer.concat([envelope, canonicalU32(crc32c(envelope))]);
  if (result.byteLength > 1024 * 1024) {
    throw new TypeError('Canonical authority payload exceeds 1 MiB.');
  }
  return result;
}

function validatePublication(publication: ServiceRelocationPublication): void {
  requireText(publication.reference, 'relocation reference');
  canonicalUuid(publication.aggregateId, 'aggregate id');
  const checksum = safeInteger(publication.checksumCrc32c, 'relocation checksum');
  if (checksum < 0 || checksum > 0xffff_ffff) {
    throw new TypeError('Relocation checksum must be an unsigned 32-bit integer.');
  }
  positiveBigInt(publication.aggregateGeneration, 'aggregate generation');
  requireText(publication.targetOwnerId, 'target owner id');
  positiveBigInt(publication.targetOwnerLeaseGeneration, 'target owner lease generation');
}

export function encodeServiceRelocationEnvelope(
  envelope: ServiceRelocationEnvelope,
  applicationVersion: bigint
): Buffer {
  const aggregate = relocationId(canonicalUuid(envelope.aggregateId, 'aggregate id'));
  const encodedApplicationVersion = nonNegativeBigInt(
    applicationVersion,
    'application version'
  );
  const participants = canonicalParticipants(envelope.participants);
  const root = participants.find(value => value.objectKind !== 'actor') ?? participants[0]!;
  if (envelope.participants.every(value => value.objectKind === 'actor')) {
    if (participants.length !== 1) {
      throw new TypeError('Standalone Actor relocation requires one participant.');
    }
  }
  const writer = new RelocationWriter();
  writer.u64(aggregate.high); writer.u64(aggregate.low);
  const rootKind = root.objectKind === 'actor' ? 1 : root.objectKind === 'user_spot' ? 2 : 3;
  writer.u8(rootKind);
  writer.body16(body => {
    if (rootKind === 3) body.text8(root.stableType);
    body.text8(authorityGlobalId(root.key)); body.u64(root.objectGeneration);
    if (rootKind !== 3) body.u64(root.authorityOwnerGeneration);
  });
  writer.u64(encodedApplicationVersion);
  writer.u32(participants.length);
  for (const [index, participant] of participants.entries()) {
    writer.u64(BigInt(index + 1)); writer.bool(true);
    writer.body64(body => { body.bytes64(participant.applicationState); });
  }
  const saved = participants.flatMap((participant, index) => participant.queuedMessages.map(message => ({
    participantId: BigInt(index + 1), order: positiveBigInt(message.sequence, 'queue sequence'), payload: Buffer.from(message.payload)
  }))).sort(compareParticipantOrder);
  if (saved.some((value, index) => index > 0
    && compareParticipantOrder(saved[index - 1]!, value) === 0)) {
    throw new TypeError('Relocation queue sequences must be unique per participant.');
  }
  writer.u32(saved.length);
  for (const value of saved) {
    const prefix = decodeServiceWireFrozenRecordPrefix(value.payload);
    if (prefix.length !== value.payload.byteLength || prefix.record.recordKind > 11) {
      throw new TypeError('Saved work must be one canonical relocation frozen record.');
    }
    writer.u64(value.participantId); writer.u64(value.order); writer.raw(value.payload);
  }
  const timers = participants.flatMap((participant, index) => participant.timers.map(timer => ({
    participantId: BigInt(index + 1), timer
  }))).sort((a, b) => a.participantId === b.participantId
    ? Buffer.compare(Buffer.from(a.timer.timerId), Buffer.from(b.timer.timerId))
    : a.participantId < b.participantId ? -1 : 1);
  if (timers.some((value, index) => index > 0
    && timers[index - 1]!.participantId === value.participantId
    && timers[index - 1]!.timer.timerId === value.timer.timerId)) {
    throw new TypeError('Relocation timer ids must be unique per participant.');
  }
  writer.u32(timers.length);
  for (const { participantId, timer } of timers) {
    writer.u64(participantId); writer.text8(timer.timerId); writer.text8(timer.handlerType);
    writer.u64(BigInt(positiveInteger(timer.intervalMs, 'timer interval')));
    writer.u8(timerPolicy(timer.overrunPolicy));
    writer.u64(BigInt(positiveInteger(timer.maxCatchUpTicks, 'timer catch-up limit')));
    writer.bool(requireBoolean(timer.stopOnUnhandledException, 'timer stop-on-error flag'));
    writer.u64(nonNegativeBigInt(timer.deliveryIndex, 'timer delivery index'));
    writer.u64(nonNegativeBigInt(timer.lastScheduledIndex, 'timer scheduled index'));
    writer.u64(nonNegativeUnixMilliseconds(timer.dueAtUnixMs, 'timer due time'));
  }
  const nextOrder = new Map<bigint, bigint>();
  for (const value of saved) {
    const current = nextOrder.get(value.participantId) ?? 0n;
    if (value.order > current) nextOrder.set(value.participantId, value.order);
  }
  const pending = timers.flatMap(({ participantId, timer }) => timer.pendingTicks.map(tick => {
    const order = (nextOrder.get(participantId) ?? 0n) + 1n;
    nextOrder.set(participantId, order);
    return { participantId, order, timerName: timer.timerId, tick };
  })).sort(compareParticipantOrder);
  writer.u32(pending.length);
  for (const value of pending) {
    writer.u64(value.participantId); writer.u64(value.order); writer.text8(value.timerName);
    writer.u64(nonNegativeBigInt(value.tick.deliveryIndex, 'pending timer delivery index'));
    writer.u64(nonNegativeBigInt(value.tick.scheduledIndex, 'pending timer scheduled index'));
    writer.u64(nonNegativeUnixMilliseconds(value.tick.scheduledAtUnixMs, 'pending timer scheduled time'));
    writer.u64(nonNegativeBigInt(value.tick.skippedTicks, 'pending timer skipped ticks'));
  }
  return writer.finish();
}

export function decodeServiceRelocationEnvelope(
  payload: Uint8Array,
  aggregateGeneration: bigint
): ServiceRelocationEnvelope {
  const reader = new RelocationReader(payload);
  const aggregateId = uuid(reader.u64(), reader.u64());
  if (aggregateId === undefined) throw new TypeError('Invalid relocation identity.');
  const rootKind = reader.u8();
  if (rootKind < 1 || rootKind > 3) throw new TypeError('relocation-envelope-v1 root object kind is invalid.');
  const root = reader.body16();
  if (rootKind === 3) root.text8();
  const spotId = root.text8(); const spotGeneration = root.nonZeroU64();
  const ownerGeneration = rootKind === 3 ? undefined : root.nonZeroU64(); root.end();
  const applicationVersion = reader.u64();
  positiveBigInt(aggregateGeneration, 'aggregate generation');
  const count = reader.count();
  const participants = Array.from({ length: count }, (_, index) => {
    const participantId = reader.nonZeroU64(); if (participantId !== BigInt(index + 1)) throw new TypeError('Application state participant order is invalid.');
    const hasState = reader.bool(); const body = reader.body64(); const applicationState = hasState ? body.bytes64() : Buffer.alloc(0); body.end();
    return wireParticipant(participantId, applicationState);
  });
  const byId = new Map(participants.map(value => [value.participantId!, value]));
  for (const _ of reader.vector()) {
    const participant = byId.get(reader.nonZeroU64()); const sequence = reader.nonZeroU64();
    if (participant === undefined) throw new TypeError('Saved work participant is unknown.');
    const start = reader.position; const prefix = decodeServiceWireFrozenRecordPrefix(reader.remainingBytes());
    if (prefix.record.recordKind > 11) throw new TypeError('Saved work record kind is invalid.');
    reader.skip(prefix.length); participant.queuedMessages.push({ sequence, payload: reader.copy(start, reader.position) });
  }
  const timerNames = new Map<bigint, Set<string>>();
  for (const _ of reader.vector()) {
    const participant = byId.get(reader.nonZeroU64()); if (participant === undefined) throw new TypeError('Timer participant is unknown.');
    const timerId = reader.text8(); const names = timerNames.get(participant.participantId!) ?? new Set<string>(); if (names.has(timerId)) throw new TypeError('Timer name is duplicated.'); names.add(timerId); timerNames.set(participant.participantId!, names);
    const handlerType = reader.text8();
    const intervalMs = Number(reader.nonZeroU64());
    const overrunPolicy = timerPolicyName(reader.u8());
    const maxCatchUpTicks = Number(reader.nonZeroU64());
    const stopOnUnhandledException = reader.bool();
    const deliveryIndex = reader.u64();
    const lastScheduledIndex = reader.u64();
    const dueAtUnixMs = unixMilliseconds(reader.u64());
    participant.timers.push({ timerId, handlerType, startedAtUnixMs: 0, dueAtUnixMs, intervalMs, overrunPolicy, maxCatchUpTicks, stopOnUnhandledException, deliveryIndex, lastScheduledIndex, pendingTicks: [] });
  }
  for (const _ of reader.vector()) {
    const participant = byId.get(reader.nonZeroU64()); const order = reader.nonZeroU64(); const timerName = reader.text8();
    const timer = participant?.timers.find(value => value.timerId === timerName);
    if (timer === undefined || order === 0n) throw new TypeError('Pending timer tick is invalid.');
    (timer.pendingTicks as ServiceRelocationPendingTimerTick[]).push({ deliveryIndex: reader.nonZeroU64(), scheduledIndex: reader.nonZeroU64(), scheduledAtUnixMs: unixMilliseconds(reader.u64()), skippedTicks: reader.u64() });
  }
  reader.end();
  // Root identity is an envelope projection, not an entry in the participant
  // vector.  Keep it once as local decode metadata until Location Store maps
  // ordinal states back to authoritative identities.
  const rootParticipant = participants[0]!;
  const rootObjectKind: ZLinkPlacementObjectKind = rootKind === 1 ? 'actor' : rootKind === 2 ? 'user_spot' : 'instance_spot';
  return { aggregateId, aggregateGeneration, applicationVersion, participants: participants.map(value => ({ ...value, ...(value === rootParticipant ? { rootSpotId: spotId, rootSpotGeneration: spotGeneration, rootOwnerGeneration: ownerGeneration, rootObjectKind } : {}) } as ServiceRelocationParticipant)), memberships: [] };
}

function canonicalParticipants(values: readonly ServiceRelocationParticipant[]): readonly ServiceRelocationParticipant[] {
  if (values.length === 0) throw new TypeError('Relocation participant count is outside its bound.');
  const participants = [...values].sort((a, b) => Buffer.compare(Buffer.from(a.key), Buffer.from(b.key)));
  if (new Set(participants.map(value => value.key)).size !== participants.length) {
    throw new TypeError('Relocation participants must have unique keys.');
  }
  return participants;
}

function authorityGlobalId(key: string): string {
  try {
    return decodeAuthorityKey({ value: key } as ZLinkAuthorityKey).globalId;
  } catch {
    const marker = key.indexOf(':');
    return marker < 0 ? requireText(key, 'participant key') : requireText(key.slice(marker + 1), 'participant key');
  }
}

function relocationId(value: string): { readonly high: bigint; readonly low: bigint } {
  const hex = value.replaceAll('-', '');
  return { high: BigInt(`0x${hex.slice(0, 16)}`), low: BigInt(`0x${hex.slice(16)}`) };
}

function uuid(high: bigint, low: bigint): string | undefined {
  if (high === 0n && low === 0n) return undefined;
  const hex = high.toString(16).padStart(16, '0') + low.toString(16).padStart(16, '0');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function compareParticipantOrder<T extends { readonly participantId: bigint; readonly order: bigint }>(a: T, b: T): number {
  return a.participantId === b.participantId
    ? a.order < b.order ? -1 : a.order > b.order ? 1 : 0
    : a.participantId < b.participantId ? -1 : 1;
}

function timerPolicy(value: string): 1 | 2 | 3 {
  if (value === 'skipLateTicks') return 1;
  if (value === 'catchUpBounded') return 2;
  if (value === 'delayNextTick') return 3;
  throw new TypeError('Timer overrun policy is invalid.');
}

function timerPolicyName(value: number): string {
  if (value === 1) return 'skipLateTicks';
  if (value === 2) return 'catchUpBounded';
  if (value === 3) return 'delayNextTick';
  throw new TypeError('Timer overrun policy is invalid.');
}

function nonNegativeUnixMilliseconds(value: number, label: string): bigint {
  const parsed = safeInteger(value, label);
  if (parsed < 0) throw new TypeError(`${label} must not be negative.`);
  return BigInt(parsed);
}

function unixMilliseconds(value: bigint): number {
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new TypeError('Unix milliseconds exceed Node safe integer range.');
  return Number(value);
}

function wireParticipant(participantId: bigint, applicationState: Buffer): ServiceRelocationParticipant & {
  queuedMessages: ServiceRelocationQueuedMessage[];
  timers: ServiceRelocationTimer[];
} {
  return {
    participantId,
    key: `wire:${participantId}`,
    objectKind: 'actor',
    stableType: 'wire',
    objectGeneration: 1n,
    authorityOwnerGeneration: 1n,
    applicationState,
    boundSessionState: Buffer.alloc(0),
    queuedMessages: [],
    timers: []
  };
}

class RelocationWriter {
  private readonly values: number[] = [];
  u8(value: number): void { if (!Number.isInteger(value) || value < 0 || value > 255) throw new TypeError('Invalid u8.'); this.values.push(value); }
  bool(value: boolean): void { this.u8(value ? 1 : 0); }
  u32(value: number): void { if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) throw new TypeError('Invalid u32.'); this.values.push(value >>> 24, (value >>> 16) & 255, (value >>> 8) & 255, value & 255); }
  u64(value: bigint): void { if (value < 0n || value > 0xffff_ffff_ffff_ffffn) throw new TypeError('Invalid u64.'); for (let i = 7; i >= 0; --i) this.u8(Number((value >> BigInt(i * 8)) & 255n)); }
  raw(value: Uint8Array): void { this.values.push(...value); }
  text8(value: string): void { const bytes = Buffer.from(requireText(value, 'text8'), 'utf8'); if (bytes.byteLength > 255) throw new TypeError('text8 exceeds 255 bytes.'); this.u8(bytes.byteLength); this.raw(bytes); }
  bytes64(value: Uint8Array): void { this.u64(BigInt(value.byteLength)); this.raw(value); }
  body16(write: (body: RelocationWriter) => void): void { const body = new RelocationWriter(); write(body); const bytes = body.finish(); if (bytes.byteLength > 0xffff) throw new TypeError('body16 exceeds 65535 bytes.'); this.u8(bytes.byteLength >>> 8); this.u8(bytes.byteLength & 255); this.raw(bytes); }
  body64(write: (body: RelocationWriter) => void): void { const body = new RelocationWriter(); write(body); const bytes = body.finish(); this.u64(BigInt(bytes.byteLength)); this.raw(bytes); }
  finish(): Buffer { return Buffer.from(this.values); }
}

class RelocationReader {
  private offset = 0;
  private readonly bytes: Buffer;
  constructor(value: Uint8Array) { this.bytes = Buffer.from(value.buffer, value.byteOffset, value.byteLength); }
  get position(): number { return this.offset; }
  u8(): number { this.need(1); return this.bytes[this.offset++]!; }
  bool(): boolean { const value = this.u8(); if (value > 1) throw new TypeError('Invalid boolean.'); return value === 1; }
  u32(): number { this.need(4); const value = this.bytes.readUInt32BE(this.offset); this.offset += 4; return value; }
  u64(): bigint { this.need(8); const value = this.bytes.readBigUInt64BE(this.offset); this.offset += 8; return value; }
  nonZeroU64(): bigint { const value = this.u64(); if (value === 0n) throw new TypeError('Expected non-zero u64.'); return value; }
  text8(): string { const length = this.u8(); if (length === 0) throw new TypeError('Expected non-empty text8.'); return this.text(length); }
  bytes64(): Buffer { const length = this.u64(); if (length > BigInt(Number.MAX_SAFE_INTEGER)) throw new TypeError('bytes64 exceeds safe length.'); return this.take(Number(length)); }
  body16(): RelocationReader { return new RelocationReader(this.take(this.u8() * 256 + this.u8())); }
  body64(): RelocationReader { const length = this.u64(); if (length > BigInt(Number.MAX_SAFE_INTEGER)) throw new TypeError('body64 exceeds safe length.'); return new RelocationReader(this.take(Number(length))); }
  count(): number { const value = this.u32(); if (value > 65_536 || value > this.bytes.byteLength - this.offset) throw new TypeError('Relocation vector count exceeds remaining bytes.'); return value; }
  *vector(): Iterable<undefined> { for (let index = 0, count = this.count(); index < count; ++index) yield undefined; }
  remainingBytes(): Buffer { return this.bytes.subarray(this.offset); }
  skip(length: number): void { this.need(length); this.offset += length; }
  copy(start: number, end: number): Buffer { return Buffer.from(this.bytes.subarray(start, end)); }
  end(): void { if (this.offset !== this.bytes.byteLength) throw new TypeError('Trailing relocation envelope bytes.'); }
  private take(length: number): Buffer { this.need(length); const value = this.bytes.subarray(this.offset, this.offset + length); this.offset += length; return value; }
  private text(length: number): string { const bytes = this.take(length); const value = new TextDecoder('utf-8', { fatal: true }).decode(bytes); if (value.includes('\0')) throw new TypeError('text8 contains NUL.'); return value; }
  private need(length: number): void { if (length < 0 || this.offset + length > this.bytes.byteLength) throw new TypeError('Truncated relocation envelope.'); }
}

function objectKind(value: unknown): ZLinkPlacementObjectKind {
  if (value !== 'actor' && value !== 'user_spot' && value !== 'instance_spot') {
    throw new TypeError('Relocation object kind is invalid.');
  }
  return value;
}

export function inventoryDigest(
  participants: readonly ServiceRelocationParticipant[],
  memberships: readonly ServiceRelocationMembership[] = []
): string {
  const identities = participants.map(participant => ({
    key: requireText(participant.key, 'participant key'),
    objectKind: objectKind(participant.objectKind),
    stableType: requireText(participant.stableType, 'participant stable type'),
    objectGeneration: positiveBigInt(
      participant.objectGeneration,
      'object generation'
    ).toString(),
    authorityOwnerGeneration: positiveBigInt(
      participant.authorityOwnerGeneration,
      'authority owner generation'
    ).toString()
  })).sort((left, right) => left.key.localeCompare(right.key));
  if (new Set(identities.map(({ key }) => key)).size !== identities.length) {
    throw new TypeError('Relocation participants must have unique keys.');
  }
  const canonicalMemberships = encodeMemberships(memberships, participants);
  return createHash('sha256').update(JSON.stringify({
    participants: identities,
    memberships: canonicalMemberships
  }), 'utf8').digest('hex');
}

function encodeAuthorityEnvelope(
  base: Uint8Array,
  publication: ServiceRelocationPublication
): Buffer {
  const checksum = safeInteger(publication.checksumCrc32c, 'relocation checksum');
  if (checksum < 0 || checksum > 0xffff_ffff) {
    throw new TypeError('Relocation checksum must be an unsigned 32-bit integer.');
  }
  if (!/^[a-f0-9]{64}$/u.test(publication.inventoryDigest)) {
    throw new TypeError('Relocation inventory digest must be lowercase SHA-256.');
  }
  const aggregateGeneration = positiveBigInt(
    publication.aggregateGeneration,
    'aggregate generation'
  );
  const targetOwnerLeaseGeneration = positiveBigInt(
    publication.targetOwnerLeaseGeneration,
    'target owner lease generation'
  );
  if (
    aggregateGeneration > 0x7fff_ffff_ffff_ffffn
    || targetOwnerLeaseGeneration > 0x7fff_ffff_ffff_ffffn
  ) {
    throw new TypeError('Relocation publication generations must fit signed 64-bit storage.');
  }
  const payload = Buffer.concat([
    // BinaryWriter.Write(0x5a4c4152u) is little-endian in the .NET reference codec.
    Buffer.from([0x52, 0x41, 0x4c, 0x5a]),
    u16le(1),
    text16le(requireText(publication.reference, 'relocation reference')),
    u32le(checksum),
    dotnetGuidBytes(canonicalUuid(publication.aggregateId, 'aggregate id')),
    u64le(aggregateGeneration),
    bytes32le(Buffer.from(publication.inventoryDigest, 'hex')),
    text16le(requireText(publication.targetOwnerId, 'target owner id')),
    i64le(targetOwnerLeaseGeneration),
    bytes32le(base)
  ]);
  if (payload.byteLength > 1024 * 1024) {
    throw new TypeError('Location authority relocation payload exceeds 1 MiB.');
  }
  return payload;
}

function decodeAuthorityEnvelope(
  payload: Uint8Array
): ServiceRelocationAuthorityEnvelope | undefined {
  try {
    const reader = new DotnetBinaryReader(payload);
    reader.expect(Buffer.from([0x52, 0x41, 0x4c, 0x5a]));
    if (reader.u16() !== 1) return undefined;
    const reference = requireText(reader.text16(), 'relocation reference');
    const checksumCrc32c = reader.u32();
    const aggregateId = canonicalUuid(
      canonicalUuidFromDotnetBytes(reader.take(16)),
      'aggregate id'
    );
    const aggregateGeneration = reader.u64();
    const inventoryDigestBytes = reader.bytes32();
    const targetOwnerId = requireText(reader.text16(), 'target owner id');
    const targetOwnerLeaseGeneration = reader.i64();
    const base = reader.bytes32();
    if (
      !reader.done
      || aggregateGeneration === 0n
      || aggregateGeneration > 0x7fff_ffff_ffff_ffffn
      || inventoryDigestBytes.byteLength !== 32
      || targetOwnerLeaseGeneration <= 0n
    ) {
      return undefined;
    }
    return {
      base,
      publication: {
        reference,
        checksumCrc32c,
        aggregateId,
        aggregateGeneration,
        inventoryDigest: inventoryDigestBytes.toString('hex'),
        targetOwnerId,
        targetOwnerLeaseGeneration
      }
    };
  } catch {
    return undefined;
  }
}

function encodeMemberships(
  memberships: readonly ServiceRelocationMembership[],
  participants: readonly ServiceRelocationParticipant[]
) {
  validateMemberships(memberships, participants);
  return memberships.map(membership => ({
    actorKey: requireText(membership.actorKey, 'membership actor key'),
    spotKey: requireText(membership.spotKey, 'membership spot key'),
    spotObjectGeneration: positiveBigInt(
      membership.spotObjectGeneration,
      'membership Spot generation'
    ).toString(),
    membershipEpoch: positiveBigInt(
      membership.membershipEpoch,
      'membership epoch'
    ).toString()
  })).sort((left, right) => left.actorKey.localeCompare(right.actorKey));
}

function validateMemberships(
  memberships: readonly ServiceRelocationMembership[],
  participants: readonly ServiceRelocationParticipant[]
): void {
  const byKey = new Map(participants.map(participant => [participant.key, participant]));
  const actorKeys = new Set<string>();
  for (const membership of memberships) {
    const actorKey = requireText(membership.actorKey, 'membership actor key');
    if (actorKeys.has(actorKey)) {
      throw new TypeError('Relocation membership Actor keys must be unique.');
    }
    actorKeys.add(actorKey);
    const actor = byKey.get(actorKey);
    if (actor?.objectKind !== 'actor') {
      throw new TypeError('Relocation membership must reference an Actor participant.');
    }
    const spot = byKey.get(requireText(membership.spotKey, 'membership spot key'));
    if (
      spot !== undefined
      && (
        spot.objectKind !== 'user_spot'
        || spot.objectGeneration !== positiveBigInt(
          membership.spotObjectGeneration,
          'membership Spot generation'
        )
      )
    ) {
      throw new TypeError('Relocation membership Spot fence does not match its participant.');
    }
    positiveBigInt(membership.membershipEpoch, 'membership epoch');
  }
}


const CRC32C_TABLE = Uint32Array.from({ length: 256 }, (_, value) => {
  let crc = value;
  for (let bit = 0; bit < 8; bit++) {
    crc = (crc >>> 1) ^ ((crc & 1) === 0 ? 0 : 0x82f6_3b78);
  }
  return crc >>> 0;
});

export function crc32c(payload: Uint8Array): number {
  let crc = 0xffff_ffff;
  for (const byte of payload) {
    crc = CRC32C_TABLE[(crc ^ byte) & 0xff]! ^ (crc >>> 8);
  }
  return (crc ^ 0xffff_ffff) >>> 0;
}

function requireText(value: unknown, label: string): string {
  if (typeof value !== 'string' || value.length === 0 || value.includes('\0')) {
    throw new TypeError(`${label} must be non-empty text without NUL.`);
  }
  return value;
}

function canonicalUuid(value: unknown, label: string): string {
  const text = requireText(value, label);
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/u.test(text)
    || /^0{8}-0{4}-0{4}-0{4}-0{12}$/u.test(text)) {
    throw new TypeError(`${label} must be a lowercase non-zero 128-bit identity.`);
  }
  return text;
}

function text16le(value: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < 1 || bytes.byteLength > 0xffff) {
    throw new TypeError('Relocation text must contain 1..65535 UTF-8 bytes.');
  }
  return Buffer.concat([u16le(bytes.byteLength), bytes]);
}

function bytes32le(value: Uint8Array): Buffer {
  const bytes = Buffer.from(value);
  if (bytes.byteLength > 1024 * 1024) {
    throw new TypeError('Relocation byte field exceeds 1 MiB.');
  }
  const length = Buffer.alloc(4);
  length.writeInt32LE(bytes.byteLength);
  return Buffer.concat([length, bytes]);
}

function u16le(value: number): Buffer {
  const result = Buffer.alloc(2);
  result.writeUInt16LE(value);
  return result;
}

function u32le(value: number): Buffer {
  const result = Buffer.alloc(4);
  result.writeUInt32LE(value);
  return result;
}

function u64le(value: bigint): Buffer {
  const result = Buffer.alloc(8);
  result.writeBigUInt64LE(value);
  return result;
}

function i64le(value: bigint): Buffer {
  const result = Buffer.alloc(8);
  result.writeBigInt64LE(value);
  return result;
}

function dotnetGuidBytes(value: string): Buffer {
  const canonical = Buffer.from(value.replaceAll('-', ''), 'hex');
  return Buffer.from([
    canonical[3]!, canonical[2]!, canonical[1]!, canonical[0]!,
    canonical[5]!, canonical[4]!,
    canonical[7]!, canonical[6]!,
    ...canonical.subarray(8)
  ]);
}

function canonicalUuidFromDotnetBytes(value: Uint8Array): string {
  const bytes = Buffer.from(value);
  if (bytes.byteLength !== 16) throw new TypeError('Relocation aggregate id is invalid.');
  const canonical = Buffer.from([
    bytes[3]!, bytes[2]!, bytes[1]!, bytes[0]!,
    bytes[5]!, bytes[4]!,
    bytes[7]!, bytes[6]!,
    ...bytes.subarray(8)
  ]).toString('hex');
  return `${canonical.slice(0, 8)}-${canonical.slice(8, 12)}-${canonical.slice(12, 16)}`
    + `-${canonical.slice(16, 20)}-${canonical.slice(20)}`;
}

class CanonicalReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    if (!this.take(expected.byteLength).equals(Buffer.from(expected))) {
      throw new TypeError('Canonical authority magic is invalid.');
    }
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  u16(): number {
    return this.take(2).readUInt16BE(0);
  }

  u32(): number {
    return this.take(4).readUInt32BE(0);
  }

  u64(): bigint {
    return this.take(8).readBigUInt64BE(0);
  }

  i64(): bigint {
    return this.take(8).readBigInt64BE(0);
  }

  text8(optional: boolean): string {
    const length = this.u8();
    if (length === 0) {
      if (optional) return '';
      throw new TypeError('Canonical authority text is empty.');
    }
    return canonicalText(this.take(length));
  }

  text16(): string {
    const length = this.u16();
    if (length === 0) throw new TypeError('Canonical authority text is empty.');
    return canonicalText(this.take(length));
  }

  take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new TypeError('Canonical authority payload is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
}

function canonicalText(value: Uint8Array): string {
  const bytes = Buffer.from(value);
  if (bytes.includes(0)) throw new TypeError('Canonical authority text contains NUL.');
  return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
}

function canonicalText8(value: string): Buffer {
  const bytes = Buffer.from(requireText(value, 'canonical authority text'), 'utf8');
  if (bytes.byteLength > 0xff) throw new TypeError('Canonical authority text8 is too long.');
  return Buffer.concat([Buffer.of(bytes.byteLength), bytes]);
}

function canonicalText16(value: string): Buffer {
  const bytes = Buffer.from(requireText(value, 'canonical authority text'), 'utf8');
  if (bytes.byteLength > 4096) throw new TypeError('Canonical authority text16 is too long.');
  return Buffer.concat([canonicalU16(bytes.byteLength), bytes]);
}

function canonicalU16(value: number): Buffer {
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function canonicalU32(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new TypeError('Canonical authority u32 is invalid.');
  }
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function canonicalU64(value: bigint): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new TypeError('Canonical authority u64 is invalid.');
  }
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function canonicalI64(value: bigint): Buffer {
  if (value < 0n || value > 0x7fff_ffff_ffff_ffffn) {
    throw new TypeError('Canonical authority i64 is invalid.');
  }
  const result = Buffer.alloc(8);
  result.writeBigInt64BE(value);
  return result;
}

class DotnetBinaryReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
    if (this.bytes.byteLength > 1024 * 1024) {
      throw new TypeError('Location authority relocation payload exceeds 1 MiB.');
    }
  }

  get done(): boolean {
    return this.offset === this.bytes.byteLength;
  }

  expect(expected: Uint8Array): void {
    if (!this.take(expected.byteLength).equals(Buffer.from(expected))) {
      throw new TypeError('Location authority relocation magic is invalid.');
    }
  }

  u16(): number {
    return this.take(2).readUInt16LE(0);
  }

  u32(): number {
    return this.take(4).readUInt32LE(0);
  }

  u64(): bigint {
    return this.take(8).readBigUInt64LE(0);
  }

  i64(): bigint {
    return this.take(8).readBigInt64LE(0);
  }

  text16(): string {
    const length = this.u16();
    if (length === 0) throw new TypeError('Location authority relocation text is empty.');
    return this.take(length).toString('utf8');
  }

  bytes32(): Buffer {
    const length = this.take(4).readInt32LE(0);
    if (length < 0 || length > 1024 * 1024) {
      throw new TypeError('Location authority relocation byte field is invalid.');
    }
    return Buffer.from(this.take(length));
  }

  take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new TypeError('Location authority relocation payload is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
}

function positiveBigInt(value: unknown, label: string): bigint {
  let parsed: bigint;
  try {
    parsed = typeof value === 'bigint' ? value : BigInt(requireText(value, label));
  } catch {
    throw new TypeError(`${label} must be a positive integer.`);
  }
  if (parsed <= 0n) throw new TypeError(`${label} must be a positive integer.`);
  return parsed;
}

function nonNegativeBigInt(value: unknown, label: string): bigint {
  let parsed: bigint;
  try {
    parsed = typeof value === 'bigint' ? value : BigInt(requireText(value, label));
  } catch {
    throw new TypeError(`${label} must be a non-negative integer.`);
  }
  if (parsed < 0n) throw new TypeError(`${label} must be a non-negative integer.`);
  return parsed;
}

function safeInteger(value: unknown, label: string): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
    throw new TypeError(`${label} must be a safe integer.`);
  }
  return value;
}

function positiveInteger(value: unknown, label: string): number {
  const parsed = safeInteger(value, label);
  if (parsed <= 0) throw new TypeError(`${label} must be positive.`);
  return parsed;
}

function requireBoolean(value: unknown, label: string): boolean {
  if (typeof value !== 'boolean') throw new TypeError(`${label} must be boolean.`);
  return value;
}

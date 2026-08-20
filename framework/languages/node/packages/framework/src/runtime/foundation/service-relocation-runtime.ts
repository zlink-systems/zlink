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
    return encodeAuthorityEnvelope(Buffer.from(currentPayload), publication);
  }

  read(payload: Uint8Array): ServiceRelocationPublication | undefined {
    return decodeAuthorityEnvelope(payload)?.publication;
  }

  clear(currentPayload: Uint8Array, expectedReference: string): Uint8Array {
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
  return Buffer.from(decodeAuthorityEnvelope(payload)?.base ?? payload);
}

/** Replaces the application payload without changing relocation metadata. */
export function replaceServiceRelocationAuthorityApplicationPayload(
  payload: Uint8Array,
  applicationPayload: Uint8Array
): Buffer {
  const current = decodeAuthorityEnvelope(payload);
  return current === undefined
    ? Buffer.from(applicationPayload)
    : encodeAuthorityEnvelope(Buffer.from(applicationPayload), current.publication);
}

export function encodeServiceRelocationEnvelope(envelope: ServiceRelocationEnvelope): Buffer {
  const aggregate = relocationId(canonicalUuid(envelope.aggregateId, 'aggregate id'));
  const applicationVersion = positiveBigInt(envelope.aggregateGeneration, 'aggregate generation');
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
  writer.u64(applicationVersion);
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

export function decodeServiceRelocationEnvelope(payload: Uint8Array): ServiceRelocationEnvelope {
  const reader = new RelocationReader(payload);
  const aggregateId = uuid(reader.u64(), reader.u64());
  if (aggregateId === undefined) throw new TypeError('Invalid relocation identity.');
  const rootKind = reader.u8();
  if (rootKind < 1 || rootKind > 3) throw new TypeError('relocation-envelope-v1 root object kind is invalid.');
  const root = reader.body16();
  if (rootKind === 3) root.text8();
  const spotId = root.text8(); const spotGeneration = root.nonZeroU64();
  const ownerGeneration = rootKind === 3 ? undefined : root.nonZeroU64(); root.end();
  const aggregateGeneration = reader.nonZeroU64();
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
  return { aggregateId, aggregateGeneration, participants: participants.map(value => ({ ...value, ...(value === rootParticipant ? { rootSpotId: spotId, rootSpotGeneration: spotGeneration, rootOwnerGeneration: ownerGeneration, rootObjectKind } : {}) } as ServiceRelocationParticipant)), memberships: [] };
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
  const encoded = {
    magic: 'ZLAR',
    version: 2,
    base: Buffer.from(base).toString('base64'),
    publication: encodePublication(publication)
  };
  const payload = Buffer.from(JSON.stringify(encoded), 'utf8');
  if (payload.byteLength > 1024 * 1024) {
    throw new TypeError('Location authority relocation payload exceeds 1 MiB.');
  }
  return payload;
}

function decodeAuthorityEnvelope(
  payload: Uint8Array
): ServiceRelocationAuthorityEnvelope | undefined {
  try {
    const decoded = record(
      JSON.parse(Buffer.from(payload).toString('utf8')),
      'authority payload'
    );
    requireExactKeys(decoded, ['base', 'magic', 'publication', 'version'], 'authority payload');
    if (decoded.magic !== 'ZLAR' || decoded.version !== 2) return undefined;
    const publication = record(decoded.publication, 'authority publication');
    requireExactKeys(publication, [
      'aggregateGeneration',
      'aggregateId',
      'checksumCrc32c',
      'inventoryDigest',
      'reference',
      'targetOwnerId',
      'targetOwnerLeaseGeneration'
    ], 'authority publication');
    const checksum = safeInteger(publication.checksumCrc32c, 'relocation checksum');
    if (checksum < 0 || checksum > 0xffff_ffff) return undefined;
    if (
      typeof publication.inventoryDigest !== 'string'
      || !/^[a-f0-9]{64}$/u.test(publication.inventoryDigest)
    ) {
      return undefined;
    }
    return {
      base: base64(decoded.base, 'authority application payload'),
      publication: {
        reference: requireText(publication.reference, 'relocation reference'),
        checksumCrc32c: checksum,
        aggregateId: canonicalUuid(publication.aggregateId, 'aggregate id'),
        aggregateGeneration: positiveBigInt(
          publication.aggregateGeneration,
          'aggregate generation'
        ),
        inventoryDigest: publication.inventoryDigest,
        targetOwnerId: requireText(publication.targetOwnerId, 'target owner id'),
        targetOwnerLeaseGeneration: positiveBigInt(
          publication.targetOwnerLeaseGeneration,
          'target owner lease generation'
        )
      }
    };
  } catch {
    return undefined;
  }
}

function encodePublication(publication: ServiceRelocationPublication) {
  const checksum = safeInteger(publication.checksumCrc32c, 'relocation checksum');
  if (checksum < 0 || checksum > 0xffff_ffff) {
    throw new TypeError('Relocation checksum must be an unsigned 32-bit integer.');
  }
  if (!/^[a-f0-9]{64}$/u.test(publication.inventoryDigest)) {
    throw new TypeError('Relocation inventory digest must be lowercase SHA-256.');
  }
  return {
    reference: requireText(publication.reference, 'relocation reference'),
    checksumCrc32c: checksum,
    aggregateId: canonicalUuid(publication.aggregateId, 'aggregate id'),
    aggregateGeneration: positiveBigInt(
      publication.aggregateGeneration,
      'aggregate generation'
    ).toString(),
    inventoryDigest: publication.inventoryDigest,
    targetOwnerId: requireText(publication.targetOwnerId, 'target owner id'),
    targetOwnerLeaseGeneration: positiveBigInt(
      publication.targetOwnerLeaseGeneration,
      'target owner lease generation'
    ).toString()
  };
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

function record(value: unknown, label: string): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError(`Invalid relocation ${label}.`);
  }
  return value as Record<string, unknown>;
}

function requireExactKeys(
  value: Record<string, unknown>,
  expected: readonly string[],
  label: string
): void {
  const actual = Object.keys(value).sort();
  if (
    actual.length !== expected.length
    || actual.some((key, index) => key !== expected[index])
  ) {
    throw new TypeError(`Invalid relocation ${label} fields.`);
  }
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

function base64(value: unknown, label: string): Buffer {
  if (typeof value !== 'string' || value.length % 4 !== 0) {
    throw new TypeError(`${label} must be canonical base64.`);
  }
  const padding = value.endsWith('==') ? 2 : value.endsWith('=') ? 1 : 0;
  const contentLength = value.length - padding;
  for (let index = 0; index < contentLength; index++) {
    const code = value.charCodeAt(index);
    if (!((code >= 0x41 && code <= 0x5a)
      || (code >= 0x61 && code <= 0x7a)
      || (code >= 0x30 && code <= 0x39)
      || code === 0x2b
      || code === 0x2f)) {
      throw new TypeError(`${label} must be canonical base64.`);
    }
  }
  for (let index = contentLength; index < value.length; index++) {
    if (value.charCodeAt(index) !== 0x3d) {
      throw new TypeError(`${label} must be canonical base64.`);
    }
  }
  const bytes = Buffer.from(value, 'base64');
  if (bytes.toString('base64') !== value) throw new TypeError(`${label} must be canonical base64.`);
  return bytes;
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

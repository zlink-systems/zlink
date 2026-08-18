import { createHash } from 'node:crypto';
import type { ZLinkPlacementObjectKind } from '../../contracts/Locations';

export interface ServiceRelocationParticipant {
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
  readonly startedAtUnixMs: number;
  readonly dueAtUnixMs: number;
  readonly intervalMs: number;
  readonly deliveryIndex: bigint;
  readonly lastScheduledIndex: bigint;
  readonly overrunPolicy: string;
  readonly maxCatchUpTicks: number;
  readonly stopOnUnhandledException: boolean;
  readonly pendingTicks: number;
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
  if (envelope.participants.length < 1) {
    throw new TypeError('Relocation participant count is outside its bound.');
  }
  const aggregateId = canonicalUuid(envelope.aggregateId, 'aggregate id');
  const aggregateGeneration = positiveBigInt(
    envelope.aggregateGeneration,
    'aggregate generation'
  );
  const participants = [...envelope.participants]
    .map(participant => encodeParticipant(participant))
    .sort((left, right) => left.key.localeCompare(right.key));
  if (new Set(participants.map(({ key }) => key)).size !== participants.length) {
    throw new TypeError('Relocation participants must have unique keys.');
  }
  const memberships = encodeMemberships(envelope.memberships, envelope.participants);
  return Buffer.from(JSON.stringify({
    version: 3,
    aggregateId,
    aggregateGeneration: aggregateGeneration.toString(),
    inventoryDigest: inventoryDigest(envelope.participants, envelope.memberships),
    memberships,
    participants
  }), 'utf8');
}

export function decodeServiceRelocationEnvelope(payload: Uint8Array): ServiceRelocationEnvelope {
  const parsed = JSON.parse(Buffer.from(payload).toString('utf8')) as {
    readonly version?: unknown;
    readonly aggregateId?: unknown;
    readonly aggregateGeneration?: unknown;
    readonly inventoryDigest?: unknown;
    readonly memberships?: unknown;
    readonly participants?: unknown;
  };
  requireExactKeys(parsed, [
    'aggregateGeneration',
    'aggregateId',
    'inventoryDigest',
    'memberships',
    'participants',
    'version'
  ], 'envelope');
  if (
    parsed.version !== 3
    || typeof parsed.inventoryDigest !== 'string'
    || !Array.isArray(parsed.participants)
    || !Array.isArray(parsed.memberships)
    || parsed.participants.length < 1
  ) {
    throw new TypeError('Invalid relocation envelope.');
  }
  const envelope: ServiceRelocationEnvelope = {
    aggregateId: canonicalUuid(parsed.aggregateId, 'aggregate id'),
    aggregateGeneration: positiveBigInt(
      parsed.aggregateGeneration,
      'aggregate generation'
    ),
    participants: parsed.participants.map((value: unknown) => {
      const item = record(value, 'participant');
      requireExactKeys(item, [
        'applicationState',
        'authorityOwnerGeneration',
        'boundSessionState',
        'key',
        'objectGeneration',
        'objectKind',
        'queuedMessages',
        'stableType',
        'timers'
      ], 'participant');
      if (
        !Array.isArray(item.queuedMessages)
        || !Array.isArray(item.timers)
      ) {
        throw new TypeError('Invalid relocation participant work inventory.');
      }
      return {
        key: requireText(item.key, 'participant key'),
        objectKind: objectKind(item.objectKind),
        stableType: requireText(item.stableType, 'participant stable type'),
        objectGeneration: positiveBigInt(item.objectGeneration, 'object generation'),
        authorityOwnerGeneration: positiveBigInt(
          item.authorityOwnerGeneration,
          'authority owner generation'
        ),
        applicationState: base64(item.applicationState, 'application state'),
        boundSessionState: base64(item.boundSessionState, 'bound Session state'),
        queuedMessages: item.queuedMessages.map(decodeQueuedMessage),
        timers: item.timers.map(decodeTimer)
      };
    }),
    memberships: parsed.memberships.map(decodeMembership)
  };
  validateMemberships(envelope.memberships, envelope.participants);
  const canonical = encodeServiceRelocationEnvelope(envelope);
  const canonicalParsed = JSON.parse(canonical.toString('utf8')) as { readonly inventoryDigest: string };
  if (canonicalParsed.inventoryDigest !== parsed.inventoryDigest) {
    throw new TypeError('Relocation inventory digest mismatch.');
  }
  return envelope;
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

function decodeMembership(value: unknown): ServiceRelocationMembership {
  const item = record(value, 'membership');
  requireExactKeys(item, [
    'actorKey',
    'membershipEpoch',
    'spotKey',
    'spotObjectGeneration'
  ], 'membership');
  return {
    actorKey: requireText(item.actorKey, 'membership actor key'),
    spotKey: requireText(item.spotKey, 'membership spot key'),
    spotObjectGeneration: positiveBigInt(
      item.spotObjectGeneration,
      'membership Spot generation'
    ),
    membershipEpoch: positiveBigInt(item.membershipEpoch, 'membership epoch')
  };
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

function encodeParticipant(participant: ServiceRelocationParticipant) {
  const queuedMessages = [...participant.queuedMessages]
    .map(message => ({
      sequence: positiveBigInt(message.sequence, 'queue sequence').toString(),
      payload: Buffer.from(message.payload).toString('base64')
    }))
    .sort((left, right) => {
      const a = BigInt(left.sequence);
      const b = BigInt(right.sequence);
      return a < b ? -1 : a > b ? 1 : 0;
    });
  if (new Set(queuedMessages.map(({ sequence }) => sequence)).size !== queuedMessages.length) {
    throw new TypeError('Relocation queue sequences must be unique per participant.');
  }
  const timers = [...participant.timers]
    .map(timer => ({
      timerId: requireText(timer.timerId, 'timer id'),
      startedAtUnixMs: safeInteger(timer.startedAtUnixMs, 'timer start time'),
      dueAtUnixMs: safeInteger(timer.dueAtUnixMs, 'timer due time'),
      intervalMs: positiveInteger(timer.intervalMs, 'timer interval'),
      deliveryIndex: nonNegativeBigInt(timer.deliveryIndex, 'timer delivery index').toString(),
      lastScheduledIndex: nonNegativeBigInt(
        timer.lastScheduledIndex,
        'timer scheduled index'
      ).toString(),
      overrunPolicy: requireText(timer.overrunPolicy, 'timer overrun policy'),
      maxCatchUpTicks: positiveInteger(timer.maxCatchUpTicks, 'timer catch-up limit'),
      stopOnUnhandledException: requireBoolean(
        timer.stopOnUnhandledException,
        'timer stop-on-error flag'
      ),
      pendingTicks: nonNegativeInteger(timer.pendingTicks, 'pending timer ticks')
    }))
    .sort((left, right) => left.timerId.localeCompare(right.timerId));
  if (new Set(timers.map(({ timerId }) => timerId)).size !== timers.length) {
    throw new TypeError('Relocation timer ids must be unique per participant.');
  }
  return {
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
    ).toString(),
    applicationState: Buffer.from(participant.applicationState).toString('base64'),
    boundSessionState: Buffer.from(participant.boundSessionState).toString('base64'),
    queuedMessages,
    timers
  };
}

function decodeQueuedMessage(value: unknown): ServiceRelocationQueuedMessage {
  const item = record(value, 'queued message');
  requireExactKeys(item, ['payload', 'sequence'], 'queued message');
  return {
    sequence: positiveBigInt(item.sequence, 'queue sequence'),
    payload: base64(item.payload, 'queued payload')
  };
}

function decodeTimer(value: unknown): ServiceRelocationTimer {
  const item = record(value, 'timer');
  requireExactKeys(item, [
    'deliveryIndex',
    'dueAtUnixMs',
    'intervalMs',
    'lastScheduledIndex',
    'maxCatchUpTicks',
    'overrunPolicy',
    'pendingTicks',
    'startedAtUnixMs',
    'stopOnUnhandledException',
    'timerId'
  ], 'timer');
  return {
    timerId: requireText(item.timerId, 'timer id'),
    startedAtUnixMs: safeInteger(item.startedAtUnixMs, 'timer start time'),
    dueAtUnixMs: safeInteger(item.dueAtUnixMs, 'timer due time'),
    intervalMs: positiveInteger(item.intervalMs, 'timer interval'),
    deliveryIndex: nonNegativeBigInt(item.deliveryIndex, 'timer delivery index'),
    lastScheduledIndex: nonNegativeBigInt(
      item.lastScheduledIndex,
      'timer scheduled index'
    ),
    overrunPolicy: requireText(item.overrunPolicy, 'timer overrun policy'),
    maxCatchUpTicks: positiveInteger(item.maxCatchUpTicks, 'timer catch-up limit'),
    stopOnUnhandledException: requireBoolean(
      item.stopOnUnhandledException,
      'timer stop-on-error flag'
    ),
    pendingTicks: nonNegativeInteger(item.pendingTicks, 'pending timer ticks')
  };
}

function objectKind(value: unknown): ZLinkPlacementObjectKind {
  if (value !== 'actor' && value !== 'user_spot' && value !== 'instance_spot') {
    throw new TypeError('Relocation object kind is invalid.');
  }
  return value;
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

function nonNegativeInteger(value: unknown, label: string): number {
  const parsed = safeInteger(value, label);
  if (parsed < 0) throw new TypeError(`${label} must not be negative.`);
  return parsed;
}

function requireBoolean(value: unknown, label: string): boolean {
  if (typeof value !== 'boolean') throw new TypeError(`${label} must be boolean.`);
  return value;
}

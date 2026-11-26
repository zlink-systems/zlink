import { AUTHORITY_ENVELOPE_MAX_BYTES } from '../../contracts/Configuration/InternalDefaults';
import { decodeAuthorityKey } from '../locations/authority-key-codec';
import type {
  ZLinkAuthorityKey,
  ZLinkPlacementObjectKind
} from '../locations/internal-location-contracts';
import { enumWireRelocationPhase } from '../protocol/service_wire_codec.generated';
import {
  decodeRelocationEnvelopeV1 as decodeGeneratedRelocationEnvelopeV1,
  encodeRelocationEnvelopeV1 as encodeGeneratedRelocationEnvelopeV1,
  type RelocationEnvelopeV1 as GeneratedRelocationEnvelopeV1,
  type RelocationObjectIdentity as GeneratedRelocationObjectIdentity
} from '../protocol/service_wire_pilot_codec.generated';
import { SHA256_DIGEST_BYTES } from './actor-join-recovery-codec';
const MAX_RELOCATION_GENERATION = 0x7fff_ffff_ffff_ffffn;

const RELOCATION_PREPARING_PHASE = enumWireRelocationPhase('preparing');
const RELOCATION_LAST_PHASE = enumWireRelocationPhase('aborted');

export interface ServiceRelocationParticipant {
  /** Wire-local participant ordinal; identity is projected from Location Store. */
  readonly participantId?: bigint;
  /** Root projection carried by relocation-envelope-v1, never a participant identity vector. */
  readonly rootSpotId?: string;
  readonly rootSpotGeneration?: bigint;
  readonly rootOwnerGeneration?: bigint;
  readonly rootObjectKind?: ZLinkPlacementObjectKind;
  readonly rootStableType?: string;
  readonly key: string;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  /** Preserves relocation-envelope-v1's absent-state projection on decode. */
  readonly applicationStatePresent?: boolean;
  readonly applicationState: Uint8Array;
  readonly boundSessionState: Uint8Array;
  readonly sessionBinding?: GeneratedRelocationEnvelopeV1['sessionBindings'][number];
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
  /** Wire order retained on decode; local capture assigns it when omitted. */
  readonly order?: bigint;
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
  /** Fields decoded from the legacy ZLAU relocation slot. */
  readonly targetAttemptGeneration?: bigint;
  readonly sourceNodeRid?: string;
  readonly sourceNodeGeneration?: bigint;
  readonly sourceOwnerId?: string;
  readonly sourceOwnerLeaseGeneration?: bigint;
  readonly targetNodeRid?: string;
  readonly targetNodeGeneration?: bigint;
  readonly coordinatorOwnerId?: string;
  readonly coordinatorLeaseGeneration?: bigint;
  readonly coordinatorNodeRid?: string;
  readonly coordinatorNodeGeneration?: bigint;
  readonly coordinatorExpectedStoreVersion?: string;
  readonly canonicalPhase?: number;
  readonly canonicalSourceCleanupState?: number;
  /** True when the publication lives in the canonical ZLAU relocation slot. */
  readonly canonical?: boolean;
  readonly applicationVersion?: bigint;
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
}

export interface CanonicalRelocationSlot {
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly targetAttemptGeneration: bigint;
  readonly reference: string;
  readonly checksumCrc32c: number;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly sourceOwnerId: string;
  readonly sourceOwnerLeaseGeneration: bigint;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly applicationVersion: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  readonly coordinatorOwnerId: string;
  readonly coordinatorLeaseGeneration: bigint;
  readonly coordinatorNodeRid: string;
  readonly coordinatorNodeGeneration: bigint;
  readonly coordinatorExpectedStoreVersion: string;
  readonly phase: number;
  readonly sourceCleanupState: number;
}

/** Deterministic Location authority wrapper for one immutable relocation root. */
export class ServiceRelocationAuthorityPayloadCodec {
  read(payload: Uint8Array): ServiceRelocationPublication | undefined {
    const canonical = decodeCanonicalAuthorityLayout(payload);
    return canonical === undefined
      ? decodeAuthorityEnvelope(payload)?.publication
      : decodeCanonicalAuthorityPublication(canonical);
  }

  clear(currentPayload: Uint8Array, expectedReference: string): Uint8Array {
    const canonical = decodeCanonicalAuthorityLayout(currentPayload);
    const canonicalPublication =
      canonical === undefined ? undefined : decodeCanonicalRelocationSlot(canonical.slot);
    if (canonical !== undefined && canonicalPublication?.reference !== undefined) {
      if (
        canonicalPublication.reference !== requireText(expectedReference, 'relocation reference')
      ) {
        throw new TypeError('Location authority relocation reference changed.');
      }
      return clearCanonicalRelocationSlot(canonical);
    }
    const current = decodeAuthorityEnvelope(currentPayload);
    if (
      current === undefined ||
      current.publication.reference !== requireText(expectedReference, 'relocation reference')
    ) {
      throw new TypeError('Location authority relocation reference changed.');
    }
    return Buffer.from(current.base);
  }
}

/** Returns the application authority payload hidden by relocation metadata. */
export function serviceRelocationAuthorityApplicationPayload(payload: Uint8Array): Buffer {
  const canonical = decodeCanonicalAuthorityLayout(payload);
  if (canonical !== undefined) {
    if (canonical.slot.byteLength === 0) return Buffer.from(payload);
    if (decodeCanonicalRelocationSlot(canonical.slot) === undefined) {
      throw new TypeError('Canonical authority relocation slot is invalid.');
    }
    return clearCanonicalRelocationSlot(canonical);
  }
  return Buffer.from(decodeAuthorityEnvelope(payload)?.base ?? payload);
}

/** Reads the canonical relocation identity even before its root pointer exists. */
export function serviceRelocationAuthoritySlotIdentity(
  payload: Uint8Array
):
  | Pick<ServiceRelocationPublication, 'aggregateId' | 'aggregateGeneration' | 'applicationVersion'>
  | undefined {
  const layout = decodeCanonicalAuthorityLayout(payload);
  const slot =
    layout === undefined || layout.slot.byteLength === 0
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

function decodeCanonicalAuthorityPublication(
  layout: CanonicalAuthorityLayout
): ServiceRelocationPublication | undefined {
  if (layout.slot.byteLength === 0) return undefined;
  const slot = decodeCanonicalRelocationSlot(layout.slot);
  if (slot === undefined) return undefined;
  return {
    reference: slot.reference,
    checksumCrc32c: slot.checksumCrc32c,
    aggregateId: slot.aggregateId,
    aggregateGeneration: slot.aggregateGeneration,
    inventoryDigest: '0'.repeat(64),
    targetOwnerId: slot.targetOwnerId,
    targetOwnerLeaseGeneration: slot.targetOwnerLeaseGeneration,
    targetAttemptGeneration: slot.targetAttemptGeneration,
    sourceNodeRid: slot.sourceNodeRid,
    sourceNodeGeneration: slot.sourceNodeGeneration,
    sourceOwnerId: slot.sourceOwnerId,
    sourceOwnerLeaseGeneration: slot.sourceOwnerLeaseGeneration,
    targetNodeRid: slot.targetNodeRid,
    targetNodeGeneration: slot.targetNodeGeneration,
    coordinatorOwnerId: slot.coordinatorOwnerId,
    coordinatorLeaseGeneration: slot.coordinatorLeaseGeneration,
    coordinatorNodeRid: slot.coordinatorNodeRid,
    coordinatorNodeGeneration: slot.coordinatorNodeGeneration,
    coordinatorExpectedStoreVersion: slot.coordinatorExpectedStoreVersion,
    canonicalPhase: slot.phase,
    canonicalSourceCleanupState: slot.sourceCleanupState,
    canonical: true,
    applicationVersion: slot.applicationVersion
  };
}

function decodeCanonicalAuthorityLayout(payload: Uint8Array): CanonicalAuthorityLayout | undefined {
  try {
    const bytes = Buffer.from(payload);
    if (bytes.byteLength < 20 || bytes.byteLength > AUTHORITY_ENVELOPE_MAX_BYTES) return undefined;
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
    bodyReader.text8(false);
    bodyReader.u64();
    bodyReader.text8(false);
    bodyReader.text8(false);
    bodyReader.u64();
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
      slot: Buffer.from(slot)
    };
  } catch {
    return undefined;
  }
}

export function decodeCanonicalRelocationSlot(
  payload: Uint8Array,
  rootAggregateGeneration?: bigint
): CanonicalRelocationSlot | undefined {
  try {
    const reader = new CanonicalReader(payload);
    const aggregateId = serviceRelocationIdFromWire({ high: reader.u64(), low: reader.u64() });
    if (aggregateId === undefined) return undefined;
    const aggregateGeneration = reader.u64();
    const targetAttemptGeneration = reader.u64();
    const reference = reader.text16();
    const checksumCrc32c = reader.u32();
    const sourceNodeRid = reader.text8(false);
    const sourceNodeGeneration = reader.u64();
    const sourceOwnerId = reader.text8(false);
    const sourceOwnerLeaseGeneration = reader.u64();
    const targetNodeRid = reader.text8(true);
    const targetNodeGeneration = reader.u64();
    const targetOwnerId = reader.text8(true);
    const targetOwnerLeaseGeneration = reader.u64();
    const coordinatorOwnerId = reader.text8(false);
    const coordinatorLeaseGeneration = reader.u64();
    const coordinatorNodeRid = reader.text8(false);
    const coordinatorNodeGeneration = reader.u64();
    const coordinatorExpectedStoreVersion = reader.text8(true);
    const phase = reader.u8();
    const applicationVersion = reader.i64();
    const sourceCleanupState = reader.u8();
    if (
      !reader.done ||
      aggregateGeneration > 0x7fff_ffff_ffff_fffen ||
      (aggregateGeneration === 0n && phase !== RELOCATION_PREPARING_PHASE) ||
      (rootAggregateGeneration !== undefined && aggregateGeneration !== rootAggregateGeneration) ||
      targetAttemptGeneration > MAX_RELOCATION_GENERATION ||
      sourceNodeGeneration === 0n ||
      sourceNodeGeneration > MAX_RELOCATION_GENERATION ||
      sourceOwnerLeaseGeneration === 0n ||
      sourceOwnerLeaseGeneration > MAX_RELOCATION_GENERATION ||
      targetNodeGeneration > MAX_RELOCATION_GENERATION ||
      targetOwnerLeaseGeneration > MAX_RELOCATION_GENERATION ||
      coordinatorLeaseGeneration === 0n ||
      coordinatorLeaseGeneration > MAX_RELOCATION_GENERATION ||
      coordinatorNodeGeneration === 0n ||
      coordinatorNodeGeneration > MAX_RELOCATION_GENERATION ||
      phase < RELOCATION_PREPARING_PHASE ||
      phase > RELOCATION_LAST_PHASE ||
      applicationVersion < 0n ||
      sourceCleanupState > 2
    )
      return undefined;
    return {
      aggregateId,
      aggregateGeneration,
      targetAttemptGeneration,
      reference,
      checksumCrc32c,
      sourceNodeRid,
      sourceNodeGeneration,
      sourceOwnerId,
      sourceOwnerLeaseGeneration,
      targetNodeRid,
      targetNodeGeneration,
      applicationVersion,
      targetOwnerId,
      targetOwnerLeaseGeneration,
      coordinatorOwnerId,
      coordinatorLeaseGeneration,
      coordinatorNodeRid,
      coordinatorNodeGeneration,
      coordinatorExpectedStoreVersion,
      phase,
      sourceCleanupState
    };
  } catch {
    return undefined;
  }
}

function clearCanonicalRelocationSlot(layout: CanonicalAuthorityLayout): Buffer {
  const body = Buffer.concat([
    layout.body.subarray(0, layout.slotStart),
    Buffer.alloc(5),
    layout.body.subarray(layout.slotEnd)
  ]);
  const envelope = Buffer.concat([
    Buffer.from('ZLAU'),
    Buffer.of(1),
    canonicalU16(layout.flags),
    canonicalU32(body.byteLength),
    body
  ]);
  const result = Buffer.concat([envelope, canonicalU32(crc32c(envelope))]);
  if (result.byteLength > AUTHORITY_ENVELOPE_MAX_BYTES) {
    throw new TypeError(
      `Canonical authority payload exceeds ${AUTHORITY_ENVELOPE_MAX_BYTES / (1024 * 1024)} MiB.`
    );
  }
  return result;
}

export function encodeServiceRelocationEnvelope(
  envelope: ServiceRelocationEnvelope,
  applicationVersion: bigint
): Buffer {
  const aggregate = relocationId(canonicalUuid(envelope.aggregateId, 'aggregate id'));
  const encodedApplicationVersion = nonNegativeBigInt(applicationVersion, 'application version');
  const participants = canonicalParticipants(envelope.participants);
  const root =
    participants.find((value) => value.rootSpotId !== undefined) ??
    participants.find((value) => value.objectKind !== 'actor') ??
    participants[0]!;
  const rootObjectKind = root.rootObjectKind ?? root.objectKind;
  if (rootObjectKind === 'actor' && participants.length !== 1) {
    throw new TypeError('Standalone Actor relocation requires one participant.');
  }
  const applicationStates = participants.map((participant, index) => {
    const hasState = participant.applicationStatePresent !== false;
    if (!hasState && participant.applicationState.byteLength !== 0) {
      throw new TypeError('Absent relocation application state must have an empty payload.');
    }
    return {
      participantId: BigInt(index + 1),
      hasState,
      payload: Buffer.from(participant.applicationState)
    };
  });
  const saved = participants
    .flatMap((participant, index) =>
      participant.queuedMessages.map((message) => ({
        participantId: BigInt(index + 1),
        order: positiveBigInt(message.sequence, 'queue sequence'),
        frozenRecord: Buffer.from(message.payload)
      }))
    )
    .sort(compareParticipantOrder);
  if (
    saved.some(
      (value, index) => index > 0 && compareParticipantOrder(saved[index - 1]!, value) === 0
    )
  ) {
    throw new TypeError('Relocation queue sequences must be unique per participant.');
  }
  const timers = participants
    .flatMap((participant, index) =>
      participant.timers.map((timer) => ({
        participantId: BigInt(index + 1),
        timer
      }))
    )
    .sort((a, b) =>
      a.participantId === b.participantId
        ? Buffer.compare(Buffer.from(a.timer.timerId), Buffer.from(b.timer.timerId))
        : a.participantId < b.participantId
          ? -1
          : 1
    );
  if (
    timers.some(
      (value, index) =>
        index > 0 &&
        timers[index - 1]!.participantId === value.participantId &&
        timers[index - 1]!.timer.timerId === value.timer.timerId
    )
  ) {
    throw new TypeError('Relocation timer ids must be unique per participant.');
  }
  const nextOrder = new Map<bigint, bigint>();
  for (const value of saved) {
    const current = nextOrder.get(value.participantId) ?? 0n;
    if (value.order > current) nextOrder.set(value.participantId, value.order);
  }
  const pending = timers
    .flatMap(({ participantId, timer }) =>
      timer.pendingTicks.map((tick) => {
        const current = nextOrder.get(participantId) ?? 0n;
        const order =
          tick.order === undefined
            ? current + 1n
            : positiveBigInt(tick.order, 'pending timer order');
        if (order > current) nextOrder.set(participantId, order);
        return { participantId, order, timerName: timer.timerId, tick };
      })
    )
    .sort(compareParticipantOrder);
  return Buffer.from(
    encodeGeneratedRelocationEnvelopeV1({
      relocationHigh: aggregate.high,
      relocationLow: aggregate.low,
      object: toGeneratedRelocationObject(root, rootObjectKind),
      applicationVersion: encodedApplicationVersion,
      applicationStates,
      savedWork: saved,
      timerRegistrations: timers.map(({ participantId, timer }) => ({
        participantId,
        name: timer.timerId,
        handlerType: timer.handlerType,
        periodMilliseconds: BigInt(positiveInteger(timer.intervalMs, 'timer interval')),
        overrunPolicy: timerPolicy(timer.overrunPolicy),
        maxCatchUpTicks: BigInt(positiveInteger(timer.maxCatchUpTicks, 'timer catch-up limit')),
        stopOnUnhandledException: requireBoolean(
          timer.stopOnUnhandledException,
          'timer stop-on-error flag'
        ),
        lastCompletedDeliveryIndex: nonNegativeBigInt(timer.deliveryIndex, 'timer delivery index'),
        lastCompletedScheduledIndex: nonNegativeBigInt(
          timer.lastScheduledIndex,
          'timer scheduled index'
        ),
        nextScheduledAtUnixMilliseconds: nonNegativeUnixMilliseconds(
          timer.dueAtUnixMs,
          'timer due time'
        )
      })),
      sessionBindings: participants.flatMap((participant, index) =>
        participant.sessionBinding === undefined
          ? []
          : [{ ...participant.sessionBinding, participantId: BigInt(index + 1) }]
      ),
      pendingTimerTicks: pending.map((value) => ({
        participantId: value.participantId,
        order: value.order,
        timerName: value.timerName,
        deliveryIndex: nonNegativeBigInt(value.tick.deliveryIndex, 'pending timer delivery index'),
        scheduledIndex: nonNegativeBigInt(
          value.tick.scheduledIndex,
          'pending timer scheduled index'
        ),
        scheduledAtUnixMilliseconds: nonNegativeUnixMilliseconds(
          value.tick.scheduledAtUnixMs,
          'pending timer scheduled time'
        ),
        skippedTicks: nonNegativeBigInt(value.tick.skippedTicks, 'pending timer skipped ticks')
      }))
    })
  );
}

export function decodeServiceRelocationEnvelope(
  payload: Uint8Array,
  aggregateGeneration: bigint
): ServiceRelocationEnvelope {
  let generated: GeneratedRelocationEnvelopeV1;
  try {
    generated = decodeGeneratedRelocationEnvelopeV1([payload]);
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : '';
    if (message === 'object kind') {
      throw new TypeError('relocation-envelope-v1 root object kind is invalid.', { cause });
    }
    if (message === 'relocation envelope trailing') {
      throw new TypeError('Trailing relocation envelope bytes.', { cause });
    }
    throw new TypeError('Invalid relocation envelope.', { cause });
  }
  const aggregateId = serviceRelocationIdFromWire({
    high: generated.relocationHigh,
    low: generated.relocationLow
  });
  if (aggregateId === undefined) throw new TypeError('Invalid relocation identity.');
  positiveBigInt(aggregateGeneration, 'aggregate generation');
  const bindings = new Map(
    generated.sessionBindings.map((binding) => [binding.participantId, binding])
  );
  for (const id of bindings.keys()) {
    if (id > BigInt(generated.applicationStates.length))
      throw new TypeError('Session binding participant is outside its inventory.');
  }
  const participants = generated.applicationStates.map((state, index) => {
    if (state.participantId !== BigInt(index + 1)) {
      throw new TypeError('Application state participant order is invalid.');
    }
    const binding = bindings.get(state.participantId);
    return {
      ...wireParticipant(state.participantId, state.hasState, Buffer.from(state.payload)),
      ...(binding === undefined ? {} : { sessionBinding: binding })
    };
  });
  const byId = new Map(participants.map((value) => [value.participantId!, value]));
  for (const work of generated.savedWork) {
    byId.get(work.participantId)!.queuedMessages.push({
      sequence: work.order,
      payload: Buffer.from(work.frozenRecord)
    });
  }
  for (const timer of generated.timerRegistrations) {
    const dueAtUnixMs = unixMilliseconds(timer.nextScheduledAtUnixMilliseconds);
    const intervalMs = unixMilliseconds(timer.periodMilliseconds);
    byId.get(timer.participantId)!.timers.push({
      timerId: timer.name,
      handlerType: timer.handlerType,
      // The wire anchors the nominal schedule by its next due time: the next
      // nominal tick after the completed ScheduledIndex (Spot timer §1).
      startedAtUnixMs: dueAtUnixMs - Number(timer.lastCompletedScheduledIndex + 1n) * intervalMs,
      dueAtUnixMs,
      intervalMs,
      overrunPolicy: timerPolicyName(timer.overrunPolicy),
      maxCatchUpTicks: unixMilliseconds(timer.maxCatchUpTicks),
      stopOnUnhandledException: timer.stopOnUnhandledException,
      deliveryIndex: timer.lastCompletedDeliveryIndex,
      lastScheduledIndex: timer.lastCompletedScheduledIndex,
      pendingTicks: []
    });
  }
  for (const tick of generated.pendingTimerTicks) {
    const timer = byId
      .get(tick.participantId)!
      .timers.find((value) => value.timerId === tick.timerName)!;
    (timer.pendingTicks as ServiceRelocationPendingTimerTick[]).push({
      order: tick.order,
      deliveryIndex: tick.deliveryIndex,
      scheduledIndex: tick.scheduledIndex,
      scheduledAtUnixMs: unixMilliseconds(tick.scheduledAtUnixMilliseconds),
      skippedTicks: tick.skippedTicks
    });
  }
  const rootParticipant = participants[0]!;
  const rootProjection = fromGeneratedRelocationObject(generated.object);
  return {
    aggregateId,
    aggregateGeneration,
    applicationVersion: generated.applicationVersion,
    participants: participants.map((value) =>
      value === rootParticipant ? { ...value, ...rootProjection } : value
    ),
    memberships: []
  };
}

function toGeneratedRelocationObject(
  root: ServiceRelocationParticipant,
  objectKind: ZLinkPlacementObjectKind
): GeneratedRelocationObjectIdentity {
  const objectId = root.rootSpotId ?? authorityGlobalId(root.key);
  const objectGeneration = root.rootSpotGeneration ?? root.objectGeneration;
  if (objectKind === 'actor') {
    return {
      objectKind: 'actor',
      actorId: objectId,
      objectGeneration,
      expectedAuthorityOwnerGeneration: root.rootOwnerGeneration ?? root.authorityOwnerGeneration
    };
  }
  if (objectKind === 'user_spot') {
    return {
      objectKind: 'userSpot',
      spotId: objectId,
      objectGeneration,
      expectedAuthorityOwnerGeneration: root.rootOwnerGeneration ?? root.authorityOwnerGeneration
    };
  }
  return {
    objectKind: 'instanceSpot',
    instanceType: root.rootStableType ?? root.stableType,
    spotId: objectId,
    objectGeneration
  };
}

function fromGeneratedRelocationObject(
  object: GeneratedRelocationObjectIdentity
): Pick<
  ServiceRelocationParticipant,
  'rootSpotId' | 'rootSpotGeneration' | 'rootOwnerGeneration' | 'rootObjectKind' | 'rootStableType'
> {
  if (object.objectKind === 'actor') {
    return {
      rootSpotId: object.actorId,
      rootSpotGeneration: object.objectGeneration,
      rootOwnerGeneration: object.expectedAuthorityOwnerGeneration,
      rootObjectKind: 'actor'
    };
  }
  if (object.objectKind === 'userSpot') {
    return {
      rootSpotId: object.spotId,
      rootSpotGeneration: object.objectGeneration,
      rootOwnerGeneration: object.expectedAuthorityOwnerGeneration,
      rootObjectKind: 'user_spot'
    };
  }
  return {
    rootSpotId: object.spotId,
    rootSpotGeneration: object.objectGeneration,
    rootObjectKind: 'instance_spot',
    rootStableType: object.instanceType
  };
}

function canonicalParticipants(
  values: readonly ServiceRelocationParticipant[]
): readonly ServiceRelocationParticipant[] {
  if (values.length === 0)
    throw new TypeError('Relocation participant count is outside its bound.');
  const participants = [...values].sort((a, b) =>
    Buffer.compare(Buffer.from(a.key), Buffer.from(b.key))
  );
  if (new Set(participants.map((value) => value.key)).size !== participants.length) {
    throw new TypeError('Relocation participants must have unique keys.');
  }
  return participants;
}

function authorityGlobalId(key: string): string {
  try {
    return decodeAuthorityKey({ value: key } as ZLinkAuthorityKey).globalId;
  } catch {
    const marker = key.indexOf(':');
    return marker < 0
      ? requireText(key, 'participant key')
      : requireText(key.slice(marker + 1), 'participant key');
  }
}

function relocationId(value: string): { readonly high: bigint; readonly low: bigint } {
  const hex = value.replaceAll('-', '');
  return { high: BigInt(`0x${hex.slice(0, 16)}`), low: BigInt(`0x${hex.slice(16)}`) };
}

export function serviceRelocationIdFromWire(value: {
  readonly high: bigint;
  readonly low: bigint;
}): string | undefined {
  if (value.high === 0n && value.low === 0n) return undefined;
  const hex = value.high.toString(16).padStart(16, '0') + value.low.toString(16).padStart(16, '0');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function compareParticipantOrder<
  T extends { readonly participantId: bigint; readonly order: bigint }
>(a: T, b: T): number {
  return a.participantId === b.participantId
    ? a.order < b.order
      ? -1
      : a.order > b.order
        ? 1
        : 0
    : a.participantId < b.participantId
      ? -1
      : 1;
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
  if (value > BigInt(Number.MAX_SAFE_INTEGER))
    throw new TypeError('Unix milliseconds exceed Node safe integer range.');
  return Number(value);
}

function wireParticipant(
  participantId: bigint,
  applicationStatePresent: boolean,
  applicationState: Buffer
): ServiceRelocationParticipant & {
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
    applicationStatePresent,
    applicationState,
    boundSessionState: Buffer.alloc(0),
    queuedMessages: [],
    timers: []
  };
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
      !reader.done ||
      aggregateGeneration === 0n ||
      aggregateGeneration > MAX_RELOCATION_GENERATION ||
      inventoryDigestBytes.byteLength !== SHA256_DIGEST_BYTES ||
      targetOwnerLeaseGeneration <= 0n
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
  if (
    !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/u.test(text) ||
    /^0{8}-0{4}-0{4}-0{4}-0{12}$/u.test(text)
  ) {
    throw new TypeError(`${label} must be a lowercase non-zero 128-bit identity.`);
  }
  return text;
}

function canonicalUuidFromDotnetBytes(value: Uint8Array): string {
  const bytes = Buffer.from(value);
  if (bytes.byteLength !== 16) throw new TypeError('Relocation aggregate id is invalid.');
  const canonical = Buffer.from([
    bytes[3]!,
    bytes[2]!,
    bytes[1]!,
    bytes[0]!,
    bytes[5]!,
    bytes[4]!,
    bytes[7]!,
    bytes[6]!,
    ...bytes.subarray(8)
  ]).toString('hex');
  return (
    `${canonical.slice(0, 8)}-${canonical.slice(8, 12)}-${canonical.slice(12, 16)}` +
    `-${canonical.slice(16, 20)}-${canonical.slice(20)}`
  );
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

class DotnetBinaryReader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(payload: Uint8Array) {
    this.bytes = Buffer.from(payload);
    if (this.bytes.byteLength > AUTHORITY_ENVELOPE_MAX_BYTES) {
      throw new TypeError(
        `Location authority relocation payload exceeds ${AUTHORITY_ENVELOPE_MAX_BYTES / (1024 * 1024)} MiB.`
      );
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
    if (length < 0 || length > AUTHORITY_ENVELOPE_MAX_BYTES) {
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

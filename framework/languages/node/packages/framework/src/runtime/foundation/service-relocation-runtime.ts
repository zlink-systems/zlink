import { createHash } from 'node:crypto';
import type {
  ZLinkAuthorityCompareExchangeResult,
  ZLinkAuthorityKey,
  ZLinkAuthorityMutation,
  ZLinkAuthorityReadResult,
  ZLinkAuthoritySnapshot,
  ZLinkAuthorityStoreVersion,
  ZLinkLocationOwnerToken,
  ZLinkPlacementObjectKind,
  ZLinkRelocationCapacityFence
} from '../../contracts/Locations';

const RELOCATION_RETENTION_MS = 24 * 60 * 60 * 1_000;
const MAX_RELOCATION_ITEMS_PER_PARTICIPANT = 65_536;

type Awaitable<T> = T | Promise<T>;

export interface ServiceAuthorityProvider {
  readAuthority(
    key: ZLinkAuthorityKey,
    signal?: AbortSignal
  ): Awaitable<ZLinkAuthorityReadResult>;
  compareExchangeAuthority(
    key: ZLinkAuthorityKey,
    expectedStoreVersion: ZLinkAuthorityStoreVersion,
    mutation: ZLinkAuthorityMutation,
    signal?: AbortSignal
  ): Awaitable<ZLinkAuthorityCompareExchangeResult>;
}

export interface ServiceRelocationStored {
  readonly reference: string;
  readonly checksumCrc32c: number;
  readonly expiresAtMs: number;
  readonly storeNowMs: number;
}

export interface ServiceRelocationStorePort {
  put(
    payload: Uint8Array,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ServiceRelocationStored>;
  get(
    reference: string,
    signal?: AbortSignal
  ): Promise<{ readonly kind: 'found'; readonly payload: Uint8Array } | { readonly kind: 'missing' }>;
  delete(reference: string, signal?: AbortSignal): Promise<'deleted' | 'missing'>;
}

export interface ServiceRelocationParticipant {
  readonly key: string;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly applicationState: Uint8Array;
  readonly acceptedJournal: Uint8Array;
  readonly replayCursor: bigint;
  readonly terminalReplies: Uint8Array;
  readonly pendingRelayCount: number;
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
  readonly sourceCleanup: 'pending' | 'completed';
  readonly participants: readonly ServiceRelocationParticipant[];
  readonly memberships: readonly ServiceRelocationMembership[];
}

export interface ServiceRelocationParticipantProgress {
  readonly replayCursor: bigint;
  readonly terminalReplies: Uint8Array;
  readonly pendingRelayCount: number;
}

export type ServiceRelocationSuccessorProgress = ReadonlyMap<
  string,
  ServiceRelocationParticipantProgress
>;

export interface ServiceRelocationPublication {
  readonly phase: 'sourceCleanupPending' | 'sourceCleanupCompleted';
  readonly reference: string;
  readonly checksumCrc32c: number;
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly inventoryDigest: string;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
}

export interface ServiceRelocationAuthorityCodec {
  prepare(currentPayload: Uint8Array): Uint8Array;
  readPreparing(payload: Uint8Array): Uint8Array | undefined;
  publish(
    currentPayload: Uint8Array,
    publication: ServiceRelocationPublication
  ): Uint8Array;
  read(payload: Uint8Array): ServiceRelocationPublication | undefined;
  replace(
    currentPayload: Uint8Array,
    expected: ServiceRelocationPublication,
    next: ServiceRelocationPublication
  ): Uint8Array;
  clear(currentPayload: Uint8Array, expectedReference: string): Uint8Array;
}

interface ServiceRelocationAuthorityEnvelope {
  readonly base: Buffer;
  readonly publication: ServiceRelocationPublication;
}

/** Deterministic Location authority wrapper for one immutable relocation root. */
export class ServiceRelocationAuthorityPayloadCodec
implements ServiceRelocationAuthorityCodec {
  prepare(currentPayload: Uint8Array): Uint8Array {
    if (this.read(currentPayload) !== undefined || this.readPreparing(currentPayload) !== undefined) {
      throw new TypeError('Location authority already contains relocation state.');
    }
    return encodePreparingAuthorityEnvelope(currentPayload);
  }

  readPreparing(payload: Uint8Array): Uint8Array | undefined {
    return decodePreparingAuthorityEnvelope(payload);
  }

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

  replace(
    currentPayload: Uint8Array,
    expected: ServiceRelocationPublication,
    next: ServiceRelocationPublication
  ): Uint8Array {
    const current = decodeAuthorityEnvelope(currentPayload);
    if (current === undefined || !samePublication(current.publication, expected)) {
      throw new TypeError('Location authority relocation publication changed.');
    }
    return encodeAuthorityEnvelope(current.base, next);
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

export interface ServicePublishedRelocation {
  readonly authority: ZLinkAuthoritySnapshot;
  readonly publication: ServiceRelocationPublication;
}

export class ServiceRelocationDataLostError extends Error {
  constructor(readonly reference: string, message: string) {
    super(message);
    this.name = 'ServiceRelocationDataLostError';
  }
}

/**
 * Stores immutable relocation input first and publishes it with one Location
 * authority CAS. The two providers never need a shared transaction.
 */
export class ServiceDurableRelocationRuntime {
  constructor(
    private readonly authority: ServiceAuthorityProvider,
    private readonly store: ServiceRelocationStorePort,
    private readonly codec: ServiceRelocationAuthorityCodec
  ) {}

  async captureAndPublish(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal,
    beforePublish?: (publication: ServiceRelocationPublication) => Promise<void>
  ): Promise<ServicePublishedRelocation> {
    signal?.throwIfAborted();
    if (envelope.sourceCleanup !== 'pending') {
      throw new TypeError('Initial relocation source cleanup must be pending.');
    }
    const preparingPayload = this.codec.prepare(expected.payload);
    let preparing: ZLinkAuthorityCompareExchangeResult;
    try {
      preparing = await this.authority.compareExchangeAuthority(
        key,
        expected.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: preparingPayload
        },
        signal
      );
    } catch (error) {
      const current = await this.authority.readAuthority(key, signal);
      if (
        current.kind !== 'snapshot'
        || !Buffer.from(current.payload).equals(preparingPayload)
        || current.objectGeneration !== expected.objectGeneration
        || current.authorityOwnerGeneration !== expected.authorityOwnerGeneration
      ) {
        throw error;
      }
      const { kind: _kind, ...stored } = current;
      preparing = { kind: 'stored', ...stored };
    }
    if (
      preparing.kind !== 'stored'
      || preparing.objectGeneration !== expected.objectGeneration
      || preparing.authorityOwnerGeneration !== expected.authorityOwnerGeneration
    ) {
      throw new Error('Location authority rejected relocation Preparing publication.');
    }
    const prepared = storedSnapshot(preparing);
    const encoded = encodeServiceRelocationEnvelope(envelope);
    const checksumCrc32c = crc32c(encoded);
    let stored: Awaited<ReturnType<ServiceRelocationStorePort['put']>>;
    try {
      stored = await this.store.put(encoded, RELOCATION_RETENTION_MS, signal);
    } catch (error) {
      await this.restorePreparing(key, prepared, expected.payload, signal);
      throw error;
    }
    if (
      stored.reference.length === 0
      || stored.checksumCrc32c !== checksumCrc32c
      || stored.expiresAtMs <= stored.storeNowMs
    ) {
      await this.store.delete(stored.reference, signal);
      await this.restorePreparing(key, prepared, expected.payload, signal);
      throw new Error('Relocation Store returned an invalid immutable payload receipt.');
    }
    const publication: ServiceRelocationPublication = {
      phase: 'sourceCleanupPending',
      reference: stored.reference,
      checksumCrc32c,
      aggregateId: canonicalUuid(envelope.aggregateId, 'aggregate id'),
      aggregateGeneration: positiveBigInt(
        envelope.aggregateGeneration,
        'aggregate generation'
      ),
      inventoryDigest: inventoryDigest(envelope.participants, envelope.memberships),
      targetOwnerId: requireText(targetOwner.ownerId, 'target owner id'),
      targetOwnerLeaseGeneration: positiveBigInt(
        targetOwner.leaseGeneration,
        'target owner lease generation'
      )
    };
    let result: ZLinkAuthorityCompareExchangeResult;
    try {
      await beforePublish?.(publication);
      result = await this.authority.compareExchangeAuthority(
        key,
        prepared.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.publish(expected.payload, publication)
        },
        signal
      );
    } catch (error) {
      const reconciled = await this.reconcilePublication(
        key,
        prepared,
        publication,
        signal
      );
      if (reconciled.kind === 'published') {
        return { authority: reconciled.authority, publication };
      }
      if (reconciled.kind === 'notCommitted') {
        await this.store.delete(stored.reference, signal);
        await this.restorePreparing(key, prepared, expected.payload, signal);
      }
      throw error;
    }
    if (
      result.kind !== 'stored'
      || result.objectGeneration !== expected.objectGeneration
      || result.authorityOwnerGeneration !== expected.authorityOwnerGeneration
    ) {
      await this.store.delete(stored.reference, signal);
      await this.restorePreparing(key, prepared, expected.payload, signal);
      throw new Error('Location authority rejected relocation publication.');
    }
    return { authority: storedSnapshot(result), publication };
  }

  private async restorePreparing(
    key: ZLinkAuthorityKey,
    prepared: ZLinkAuthoritySnapshot,
    steadyPayload: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    await this.authority.compareExchangeAuthority(
      key,
      prepared.storeVersion,
      {
        kind: 'restore',
        payload: steadyPayload,
        expectedOwner: {
          ownerId: prepared.ownerId,
          leaseGeneration: prepared.ownerLeaseGeneration
        }
      },
      signal
    );
  }

  readPublication(authority: ZLinkAuthoritySnapshot): ServiceRelocationPublication {
    const publication = this.codec.read(authority.payload);
    if (publication === undefined) {
      throw new Error('Location authority has no published relocation reference.');
    }
    return publication;
  }

  private async reconcilePublication(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    publication: ServiceRelocationPublication,
    signal?: AbortSignal
  ): Promise<
    | { readonly kind: 'published'; readonly authority: ZLinkAuthoritySnapshot }
    | { readonly kind: 'notCommitted' }
    | { readonly kind: 'unknown' }
  > {
    let current: ZLinkAuthorityReadResult;
    try {
      current = await this.authority.readAuthority(key, signal);
    } catch {
      return { kind: 'unknown' };
    }
    if (current.kind !== 'snapshot') {
      return { kind: 'unknown' };
    }
    const observed = this.codec.read(current.payload);
    if (samePublication(observed, publication)) {
      return { kind: 'published', authority: current };
    }
    return current.storeVersion.value === expected.storeVersion.value
      ? { kind: 'notCommitted' }
      : { kind: 'unknown' };
  }

  async restore(
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ServiceRelocationEnvelope> {
    signal?.throwIfAborted();
    const publication = this.codec.read(authority.payload);
    if (publication === undefined) {
      throw new Error('Location authority has no published relocation reference.');
    }
    const read = await this.store.get(publication.reference, signal);
    if (read.kind === 'missing') {
      throw new ServiceRelocationDataLostError(
        publication.reference,
        'Location authority references missing relocation data.'
      );
    }
    if (crc32c(read.payload) !== publication.checksumCrc32c) {
      throw new ServiceRelocationDataLostError(
        publication.reference,
        'Published relocation checksum does not match the immutable payload.'
      );
    }
    const envelope = decodeServiceRelocationEnvelope(read.payload);
    if (inventoryDigest(envelope.participants, envelope.memberships) !== publication.inventoryDigest) {
      throw new ServiceRelocationDataLostError(
        publication.reference,
        'Published relocation inventory does not match Location authority.'
      );
    }
    return envelope;
  }

  async commitOwner(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    targetOwner: ZLinkLocationOwnerToken,
    relocationCapacityFence?: ZLinkRelocationCapacityFence,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    signal?.throwIfAborted();
    const publication = this.codec.read(expected.payload);
    if (publication === undefined) {
      throw new Error('Location authority has no published relocation reference.');
    }
    if (
      publication.targetOwnerId !== targetOwner.ownerId
      || publication.targetOwnerLeaseGeneration !== targetOwner.leaseGeneration
    ) {
      throw new Error('Relocation target owner does not match the published owner fence.');
    }
    const result = await this.authority.compareExchangeAuthority(
      key,
      expected.storeVersion,
      {
        kind: 'put',
        generationTransition: 'newOwner',
        targetOwner,
        ...(relocationCapacityFence === undefined ? {} : { relocationCapacityFence }),
        payload: expected.payload
      },
      signal
    );
    if (
      result.kind !== 'stored'
      || result.objectGeneration !== expected.objectGeneration
      || result.authorityOwnerGeneration <= expected.authorityOwnerGeneration
      || result.ownerId !== targetOwner.ownerId
      || result.ownerLeaseGeneration !== targetOwner.leaseGeneration
    ) {
      throw new Error('Location authority rejected relocation owner commit.');
    }
    return storedSnapshot(result);
  }

  /**
   * Replaces the pending immutable root with a completed root. The old root is
   * deleted only after Location authority exposes the completed reference.
   */
  async completeSourceCleanup(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    progress?: ServiceRelocationSuccessorProgress,
    signal?: AbortSignal,
    options?: { readonly retainPreviousRoot?: boolean }
  ): Promise<ZLinkAuthoritySnapshot> {
    signal?.throwIfAborted();
    const pending = this.codec.read(expected.payload);
    if (pending === undefined) {
      throw new Error('Location authority has no published relocation reference.');
    }
    if (pending.phase === 'sourceCleanupCompleted') return expected;
    validatePublicationOwner(expected, pending);

    const retried = await this.readCompletedSourceCleanup(key, pending, signal);
    if (retried !== undefined) {
      if (options?.retainPreviousRoot !== true) {
        await this.store.delete(pending.reference, signal);
      }
      return retried;
    }

    const restored = await this.restore(expected, signal);
    if (
      restored.sourceCleanup !== 'pending'
      || restored.aggregateId !== pending.aggregateId
      || restored.aggregateGeneration !== pending.aggregateGeneration
    ) {
      throw new ServiceRelocationDataLostError(
        pending.reference,
        'Published relocation root is not the exact source-cleanup pending aggregate.'
      );
    }
    const completedEnvelope: ServiceRelocationEnvelope = {
      ...restored,
      aggregateGeneration: pending.aggregateGeneration + 1n,
      sourceCleanup: 'completed',
      participants: restored.participants.map(participant => {
        const completed = progress?.get(participant.key);
        return completed === undefined
          ? participant
          : {
              ...participant,
              replayCursor: completed.replayCursor,
              terminalReplies: Buffer.from(completed.terminalReplies),
              pendingRelayCount: completed.pendingRelayCount
            };
      })
    };
    const encoded = encodeServiceRelocationEnvelope(completedEnvelope);
    const checksumCrc32c = crc32c(encoded);
    const stored = await this.store.put(encoded, RELOCATION_RETENTION_MS, signal);
    if (
      stored.reference.length === 0
      || stored.checksumCrc32c !== checksumCrc32c
      || stored.expiresAtMs <= stored.storeNowMs
    ) {
      await this.store.delete(stored.reference, signal);
      throw new Error('Relocation Store returned an invalid completed payload receipt.');
    }
    const completed: ServiceRelocationPublication = {
      ...pending,
      phase: 'sourceCleanupCompleted',
      reference: stored.reference,
      checksumCrc32c,
      aggregateGeneration: completedEnvelope.aggregateGeneration
    };

    let authority: ZLinkAuthoritySnapshot | undefined;
    let result: ZLinkAuthorityCompareExchangeResult | undefined;
    try {
      result = await this.authority.compareExchangeAuthority(
        key,
        expected.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.replace(expected.payload, pending, completed)
        },
        signal
      );
    } catch (error) {
      const reconciled = await this.reconcilePublication(
        key,
        expected,
        completed,
        signal
      );
      if (reconciled.kind !== 'published') {
        await this.store.delete(stored.reference, signal);
        throw error;
      }
      authority = reconciled.authority;
    }
    if (result !== undefined) {
      if (
        result.kind === 'stored'
        && result.objectGeneration === expected.objectGeneration
        && result.authorityOwnerGeneration === expected.authorityOwnerGeneration
        && result.ownerId === pending.targetOwnerId
        && result.ownerLeaseGeneration === pending.targetOwnerLeaseGeneration
      ) {
        authority = storedSnapshot(result);
      } else {
        const reconciled = await this.reconcilePublication(
          key,
          expected,
          completed,
          signal
        );
        if (reconciled.kind !== 'published') {
          await this.store.delete(stored.reference, signal);
          throw new Error('Location authority rejected source-cleanup completion.');
        }
        authority = reconciled.authority;
      }
    }

    if (authority === undefined) {
      await this.store.delete(stored.reference, signal);
      throw new Error('Source-cleanup completion has no authority result.');
    }
    if (options?.retainPreviousRoot !== true) {
      await this.store.delete(pending.reference, signal);
    }
    return authority;
  }

  deleteRetainedRoot(reference: string, signal?: AbortSignal): Promise<'deleted' | 'missing'> {
    return this.store.delete(reference, signal);
  }

  async advanceCompletedProgress(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    progress: ServiceRelocationSuccessorProgress,
    signal?: AbortSignal,
    options?: { readonly retainPreviousRoot?: boolean }
  ): Promise<ZLinkAuthoritySnapshot> {
    signal?.throwIfAborted();
    const currentPublication = this.codec.read(expected.payload);
    if (currentPublication?.phase !== 'sourceCleanupCompleted') {
      throw new Error('Completed relocation progress requires a completed authority root.');
    }
    const restored = await this.restore(expected, signal);
    if (restored.sourceCleanup !== 'completed') {
      throw new ServiceRelocationDataLostError(
        currentPublication.reference,
        'Completed relocation authority references a pending root.'
      );
    }
    const nextEnvelope: ServiceRelocationEnvelope = {
      ...restored,
      aggregateGeneration: restored.aggregateGeneration + 1n,
      participants: restored.participants.map(participant => {
        const next = progress.get(participant.key);
        return next === undefined ? participant : {
          ...participant,
          replayCursor: next.replayCursor,
          terminalReplies: Buffer.from(next.terminalReplies),
          pendingRelayCount: next.pendingRelayCount
        };
      })
    };
    const encoded = encodeServiceRelocationEnvelope(nextEnvelope);
    const checksumCrc32c = crc32c(encoded);
    const stored = await this.store.put(encoded, RELOCATION_RETENTION_MS, signal);
    if (stored.reference.length === 0 || stored.checksumCrc32c !== checksumCrc32c
      || stored.expiresAtMs <= stored.storeNowMs) {
      await this.store.delete(stored.reference, signal);
      throw new Error('Relocation Store returned an invalid delivery payload receipt.');
    }
    const nextPublication: ServiceRelocationPublication = {
      ...currentPublication,
      reference: stored.reference,
      checksumCrc32c,
      aggregateGeneration: nextEnvelope.aggregateGeneration
    };
    let authority: ZLinkAuthoritySnapshot | undefined;
    try {
      const result = await this.authority.compareExchangeAuthority(
        key,
        expected.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.replace(expected.payload, currentPublication, nextPublication)
        },
        signal
      );
      if (result.kind === 'stored'
        && result.objectGeneration === expected.objectGeneration
        && result.authorityOwnerGeneration === expected.authorityOwnerGeneration) {
        authority = storedSnapshot(result);
      }
    } catch (error) {
      const reconciled = await this.reconcilePublication(
        key, expected, nextPublication, signal
      );
      if (reconciled.kind !== 'published') {
        await this.store.delete(stored.reference, signal);
        throw error;
      }
      authority = reconciled.authority;
    }
    if (authority === undefined) {
      const reconciled = await this.reconcilePublication(
        key, expected, nextPublication, signal
      );
      if (reconciled.kind !== 'published') {
        await this.store.delete(stored.reference, signal);
        throw new Error('Location authority rejected terminal delivery progress.');
      }
      authority = reconciled.authority;
    }
    if (options?.retainPreviousRoot !== true) {
      await this.store.delete(currentPublication.reference, signal);
    }
    return authority;
  }

  private async readCompletedSourceCleanup(
    key: ZLinkAuthorityKey,
    pending: ServiceRelocationPublication,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot | undefined> {
    const current = await this.authority.readAuthority(key, signal);
    if (current.kind !== 'snapshot') return undefined;
    const publication = this.codec.read(current.payload);
    if (
      publication?.phase !== 'sourceCleanupCompleted'
      || publication.aggregateId !== pending.aggregateId
      || publication.aggregateGeneration !== pending.aggregateGeneration + 1n
      || publication.inventoryDigest !== pending.inventoryDigest
      || publication.targetOwnerId !== pending.targetOwnerId
      || publication.targetOwnerLeaseGeneration
        !== pending.targetOwnerLeaseGeneration
    ) {
      return undefined;
    }
    validatePublicationOwner(current, publication);
    const envelope = await this.restore(current, signal);
    if (
      envelope.sourceCleanup !== 'completed'
      || envelope.aggregateId !== publication.aggregateId
      || envelope.aggregateGeneration !== publication.aggregateGeneration
    ) {
      throw new ServiceRelocationDataLostError(
        publication.reference,
        'Completed relocation root does not match its authority phase.'
      );
    }
    return current;
  }

  async release(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    signal?.throwIfAborted();
    const publication = this.codec.read(expected.payload);
    if (publication === undefined) return expected;
    const result = await this.authority.compareExchangeAuthority(
      key,
      expected.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: this.codec.clear(expected.payload, publication.reference)
      },
      signal
    );
    if (result.kind !== 'stored') {
      throw new Error('Location authority rejected relocation release.');
    }
    await this.store.delete(publication.reference, signal);
    return storedSnapshot(result);
  }
}

function storedSnapshot(
  result: Extract<ZLinkAuthorityCompareExchangeResult, { readonly kind: 'stored' }>
): ZLinkAuthoritySnapshot {
  const { kind: _kind, ...snapshot } = result;
  return { kind: 'snapshot', ...snapshot };
}

export function encodeServiceRelocationEnvelope(envelope: ServiceRelocationEnvelope): Buffer {
  if (
    envelope.participants.length < 1
  ) {
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
    version: 2,
    aggregateId,
    aggregateGeneration: aggregateGeneration.toString(),
    inventoryDigest: inventoryDigest(envelope.participants, envelope.memberships),
    memberships,
    sourceCleanup: envelope.sourceCleanup,
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
    readonly sourceCleanup?: unknown;
    readonly participants?: unknown;
  };
  requireExactKeys(parsed, [
    'aggregateGeneration',
    'aggregateId',
    'inventoryDigest',
    'memberships',
    'participants',
    'sourceCleanup',
    'version'
  ], 'envelope');
  if (
    parsed.version !== 2
    || typeof parsed.inventoryDigest !== 'string'
    || (parsed.sourceCleanup !== 'pending' && parsed.sourceCleanup !== 'completed')
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
    sourceCleanup: parsed.sourceCleanup,
    participants: parsed.participants.map((value: unknown) => {
      const item = record(value, 'participant');
      requireExactKeys(item, [
        'acceptedJournal',
        'applicationState',
        'authorityOwnerGeneration',
        'key',
        'objectGeneration',
        'objectKind',
        'pendingRelayCount',
        'queuedMessages',
        'replayCursor',
        'stableType',
        'terminalReplies',
        'timers'
      ], 'participant');
      if (
        !Array.isArray(item.queuedMessages)
        || !Array.isArray(item.timers)
        || item.queuedMessages.length > MAX_RELOCATION_ITEMS_PER_PARTICIPANT
        || item.timers.length > MAX_RELOCATION_ITEMS_PER_PARTICIPANT
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
        acceptedJournal: base64(item.acceptedJournal, 'accepted journal'),
        replayCursor: nonNegativeBigInt(item.replayCursor, 'replay cursor'),
        terminalReplies: base64(item.terminalReplies, 'terminal replies'),
        pendingRelayCount: nonNegativeInteger(
          item.pendingRelayCount,
          'pending relay count'
        ),
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
    version: 1,
    base: Buffer.from(base).toString('base64'),
    publication: encodePublication(publication)
  };
  const payload = Buffer.from(JSON.stringify(encoded), 'utf8');
  if (payload.byteLength > 1024 * 1024) {
    throw new TypeError('Location authority relocation payload exceeds 1 MiB.');
  }
  return payload;
}

function encodePreparingAuthorityEnvelope(base: Uint8Array): Buffer {
  const payload = Buffer.from(JSON.stringify({
    magic: 'ZLAP',
    version: 1,
    base: Buffer.from(base).toString('base64')
  }), 'utf8');
  if (payload.byteLength > 1024 * 1024) {
    throw new TypeError('Location authority Preparing payload exceeds 1 MiB.');
  }
  return payload;
}

export function decodePreparingAuthorityEnvelope(payload: Uint8Array): Buffer | undefined {
  try {
    const decoded = record(
      JSON.parse(Buffer.from(payload).toString('utf8')),
      'Preparing authority payload'
    );
    requireExactKeys(decoded, ['base', 'magic', 'version'], 'Preparing authority payload');
    if (decoded.magic !== 'ZLAP' || decoded.version !== 1) return undefined;
    return base64(decoded.base, 'Preparing authority application payload');
  } catch {
    return undefined;
  }
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
    if (decoded.magic !== 'ZLAR' || decoded.version !== 1) return undefined;
    const publication = record(decoded.publication, 'authority publication');
    requireExactKeys(publication, [
      'aggregateGeneration',
      'aggregateId',
      'checksumCrc32c',
      'inventoryDigest',
      'phase',
      'reference',
      'targetOwnerId',
      'targetOwnerLeaseGeneration'
    ], 'authority publication');
    if (
      publication.phase !== 'sourceCleanupPending'
      && publication.phase !== 'sourceCleanupCompleted'
    ) {
      return undefined;
    }
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
        phase: publication.phase,
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
    phase: publication.phase,
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
  if (memberships.length > MAX_RELOCATION_ITEMS_PER_PARTICIPANT) {
    throw new TypeError('Relocation membership inventory exceeds its bound.');
  }
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
  if (
    participant.queuedMessages.length > MAX_RELOCATION_ITEMS_PER_PARTICIPANT
    || participant.timers.length > MAX_RELOCATION_ITEMS_PER_PARTICIPANT
  ) {
    throw new TypeError('Relocation participant work inventory exceeds its bound.');
  }
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
    acceptedJournal: Buffer.from(participant.acceptedJournal).toString('base64'),
    replayCursor: nonNegativeBigInt(participant.replayCursor, 'replay cursor').toString(),
    terminalReplies: Buffer.from(participant.terminalReplies).toString('base64'),
    pendingRelayCount: nonNegativeInteger(
      participant.pendingRelayCount,
      'pending relay count'
    ),
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

export function crc32c(payload: Uint8Array): number {
  let crc = 0xffff_ffff;
  for (const byte of payload) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ ((crc & 1) === 0 ? 0 : 0x82f6_3b78);
    }
  }
  return (crc ^ 0xffff_ffff) >>> 0;
}

function samePublication(
  left: ServiceRelocationPublication | undefined,
  right: ServiceRelocationPublication
): boolean {
  return left?.phase === right.phase
    && left.reference === right.reference
    && left.checksumCrc32c === right.checksumCrc32c
    && left.aggregateId === right.aggregateId
    && left.aggregateGeneration === right.aggregateGeneration
    && left.inventoryDigest === right.inventoryDigest
    && left.targetOwnerId === right.targetOwnerId
    && left.targetOwnerLeaseGeneration === right.targetOwnerLeaseGeneration;
}

function validatePublicationOwner(
  authority: ZLinkAuthoritySnapshot,
  publication: ServiceRelocationPublication
): void {
  if (
    authority.ownerId !== publication.targetOwnerId
    || authority.ownerLeaseGeneration !== publication.targetOwnerLeaseGeneration
  ) {
    throw new ServiceRelocationDataLostError(
      publication.reference,
      'Relocation authority owner fence does not match its published target.'
    );
  }
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
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u.test(text)) {
    throw new TypeError(`${label} must be a lowercase canonical UUID.`);
  }
  return text;
}

function base64(value: unknown, label: string): Buffer {
  if (typeof value !== 'string' || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    throw new TypeError(`${label} must be canonical base64.`);
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

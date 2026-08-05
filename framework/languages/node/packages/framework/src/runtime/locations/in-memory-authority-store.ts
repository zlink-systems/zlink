import { createHash, randomUUID, timingSafeEqual } from 'node:crypto';
import type {
  ZLinkAggregateAbortResult,
  ZLinkAggregateCommitResult,
  ZLinkAggregateFence,
  ZLinkAggregatePrepareRequest,
  ZLinkAggregatePrepareResult,
  ZLinkAuthorityCompareExchangeResult,
  ZLinkAuthorityKey,
  ZLinkAuthorityMutation,
  ZLinkAuthorityReadResult,
  ZLinkAuthorityScanCursor,
  ZLinkAuthorityScanResult,
  ZLinkAuthoritySnapshot,
  ZLinkAuthorityStoreVersion,
  ZLinkCapacityVector,
  ZLinkLocationOwnerToken,
  ZLinkMeshNodeDescriptorKey,
  ZLinkObjectAbortRequest,
  ZLinkObjectAbortResult,
  ZLinkObjectCommitRequest,
  ZLinkObjectCommitResult,
  ZLinkObjectCreationCompleteRequest,
  ZLinkObjectCreationCompleteResult,
  ZLinkObjectReserveRequest,
  ZLinkObjectReserveResult,
  ZLinkCreationOperationIdentity,
  ZLinkCreationTerminalPublication,
  ZLinkCreationTerminalReadResult,
  ZLinkCreationTerminalRecord,
  ZLinkPlacementAllocation,
  ZLinkRelocationCapacityAbortResult,
  ZLinkRelocationCapacityFence,
  ZLinkRelocationCapacityReservationRequest,
  ZLinkRelocationCapacityReserveResult
} from './internal-location-contracts';
import { encodeAuthorityKey } from './authority-key-codec';

const MAX_GENERATION = 0x7fff_ffff_ffff_ffffn;
const MAX_PAYLOAD_BYTES = 1024 * 1024;
const CREATION_TERMINAL_RETENTION_MS = 5 * 60 * 1000;
const MAX_U64 = 0xffff_ffff_ffff_ffffn;

export interface ZLinkInMemoryAuthorityValidation {
  isTargetLive(
    descriptor: ZLinkMeshNodeDescriptorKey,
    lifecycleGeneration: bigint,
    owner: ZLinkLocationOwnerToken
  ): boolean;
  placementCapacityAvailable?(
    descriptor: ZLinkMeshNodeDescriptorKey,
    requested: ZLinkCapacityVector,
    currentReserved: ZLinkCapacityVector,
    currentActive: ZLinkCapacityVector
  ): boolean;
  identityClaimed?(authorityKey: string): boolean;
}

interface AuthorityRow {
  snapshot: StoredSnapshot;
  creation?: {
    readonly reservationId: string;
    readonly target: CreationTarget;
    terminal?: 'committed' | 'rejected' | 'failed' | 'aborted';
  };
}

type StoredSnapshot = Omit<ZLinkAuthoritySnapshot, 'kind' | 'storeNow'>;

interface CreationTarget {
  readonly descriptor: ZLinkMeshNodeDescriptorKey;
  readonly lifecycleGeneration: bigint;
  readonly owner: ZLinkLocationOwnerToken;
}

interface CapacityReservation {
  readonly request: ZLinkRelocationCapacityReservationRequest;
  readonly fence: ZLinkRelocationCapacityFence;
  state: 'reserved' | 'prepared' | 'committed' | 'aborted';
  aggregate?: string;
}

interface AggregateRecord {
  readonly request: ZLinkAggregatePrepareRequest;
  readonly fence: ZLinkAggregateFence;
  readonly capacityReservationId?: string;
  state: 'prepared' | 'committed' | 'aborted';
}

/**
 * In-memory reference provider for the provider-owned authority state machine.
 * Opaque payload bytes are copied but never parsed.
 */
export class ZLinkInMemoryAuthorityStore {
  private readonly rows = new Map<string, AuthorityRow>();
  private readonly creationTerminals =
    new Map<string, 'committed' | 'rejected' | 'failed' | 'aborted'>();
  private readonly operationTerminals = new Map<string, ZLinkCreationTerminalRecord>();
  private readonly capacityReservations = new Map<string, CapacityReservation>();
  private readonly aggregates = new Map<string, AggregateRecord>();
  private readonly activeCapacity = new Map<string, number>();
  private readonly pendingCapacity = new Map<string, number>();
  private storeVersion = 0n;
  private objectGeneration = 0n;
  private ownerGeneration = 0n;
  private scanRevision = 0n;

  constructor(
    private readonly validation: ZLinkInMemoryAuthorityValidation,
    private readonly now: () => Date = () => new Date()
  ) {}

  async readAuthority(
    key: ZLinkAuthorityKey,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityReadResult> {
    signal?.throwIfAborted();
    const value = requireText(key.value, 'authority key');
    return this.read(value);
  }

  async compareExchangeAuthority(
    key: ZLinkAuthorityKey,
    expectedStoreVersion: ZLinkAuthorityStoreVersion,
    mutation: ZLinkAuthorityMutation,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityCompareExchangeResult> {
    signal?.throwIfAborted();
    const keyValue = requireText(key.value, 'authority key');
    validateAuthorityMutation(mutation);
    const row = this.rows.get(keyValue);
    if (
      row === undefined
      || row.snapshot.allocation.state !== 'active'
      || row.snapshot.storeVersion.value !== expectedStoreVersion.value
    ) {
      return { kind: 'conflict', current: this.read(keyValue) };
    }

    if (mutation.kind === 'delete') {
      if (!this.isOwnerLive(row.snapshot)) {
        return { kind: 'conflict', current: this.read(keyValue) };
      }
      const nextVersion = this.tryNextStoreVersion();
      if (nextVersion === undefined) return { kind: 'generationExhausted' };
      this.adjustCapacity(this.activeCapacity, row.snapshot.allocation, -1);
      this.rows.delete(keyValue);
      this.scanRevision++;
      return { kind: 'deleted', storeVersion: version(nextVersion), storeNow: this.now() };
    }

    validatePayload(mutation.payload);
    if (mutation.kind === 'rebindOwnerLease') {
      const currentOwner = {
        ownerId: row.snapshot.ownerId,
        leaseGeneration: row.snapshot.ownerLeaseGeneration
      };
      if (
        !sameOwner(mutation.expectedOwner, currentOwner)
        || mutation.targetOwner.ownerId !== currentOwner.ownerId
        || mutation.targetOwner.leaseGeneration === currentOwner.leaseGeneration
        || !this.validation.isTargetLive(
          row.snapshot.allocation.descriptor,
          row.snapshot.allocation.descriptorLifecycleGeneration,
          mutation.targetOwner
        )
      ) {
        return { kind: 'conflict', current: this.read(keyValue) };
      }
      const nextVersion = this.tryNextStoreVersion();
      if (nextVersion === undefined) return { kind: 'generationExhausted' };
      row.snapshot = {
        ...row.snapshot,
        ownerLeaseGeneration: mutation.targetOwner.leaseGeneration,
        storeVersion: version(nextVersion),
        payload: Buffer.from(mutation.payload)
      };
      this.scanRevision++;
      return this.stored(row.snapshot);
    }
    if (mutation.kind === 'restore') {
      if (!sameOwner(mutation.expectedOwner, {
        ownerId: row.snapshot.ownerId,
        leaseGeneration: row.snapshot.ownerLeaseGeneration
      })) {
        return { kind: 'conflict', current: this.read(keyValue) };
      }
      const nextVersion = this.tryNextStoreVersion();
      if (nextVersion === undefined) return { kind: 'generationExhausted' };
      row.snapshot = {
        ...row.snapshot,
        storeVersion: version(nextVersion),
        payload: Buffer.from(mutation.payload)
      };
      this.scanRevision++;
      return this.stored(row.snapshot);
    }
    if (mutation.generationTransition === 'preserve') {
      if (!this.isOwnerLive(row.snapshot)) {
        return { kind: 'conflict', current: this.read(keyValue) };
      }
      const nextVersion = this.tryNextStoreVersion();
      if (nextVersion === undefined) return { kind: 'generationExhausted' };
      row.snapshot = {
        ...row.snapshot,
        storeVersion: version(nextVersion),
        payload: Buffer.from(mutation.payload)
      };
      this.scanRevision++;
      return this.stored(row.snapshot);
    }

    const targetOwner = mutation.targetOwner!;
    const reservation = this.capacityReservations.get(mutation.relocationCapacityFence!.value);
    if (
      reservation === undefined
      || reservation.state !== 'reserved'
      || !sameFenceAuthority(reservation.request, keyValue, expectedStoreVersion, row.snapshot)
      || !sameOwner(reservation.request.targetOwner, targetOwner)
      || !this.targetLive(reservation.request)
    ) {
      return { kind: 'conflict', current: this.read(keyValue) };
    }
    if (this.storeVersion >= MAX_GENERATION || this.ownerGeneration >= MAX_GENERATION) {
      return { kind: 'generationExhausted' };
    }
    const nextVersion = ++this.storeVersion;
    const nextOwnerGeneration = ++this.ownerGeneration;
    this.moveCapacity(row.snapshot.allocation, reservation.request);
    row.snapshot = {
      storeVersion: version(nextVersion),
      payload: Buffer.from(mutation.payload),
      objectGeneration: row.snapshot.objectGeneration,
      authorityOwnerGeneration: nextOwnerGeneration,
      ownerId: targetOwner.ownerId,
      ownerLeaseGeneration: targetOwner.leaseGeneration,
      allocation: targetAllocation(reservation.request)
    };
    reservation.state = 'committed';
    this.scanRevision++;
    return this.stored(row.snapshot);
  }

  async listAuthorities(
    prefix: string,
    cursor: ZLinkAuthorityScanCursor | undefined,
    limit: number,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityScanResult> {
    signal?.throwIfAborted();
    if (!Number.isInteger(limit) || limit < 1 || limit > 1000) {
      throw new RangeError('Authority scan limit must be in 1..1000.');
    }
    const decoded = cursor === undefined
      ? { revision: this.scanRevision, offset: 0, prefix }
      : decodeCursor(cursor.encoded);
    if (decoded.revision !== this.scanRevision || decoded.prefix !== prefix) {
      return { kind: 'scanExpired' };
    }
    const rows = [...this.rows.entries()]
      .filter(([key]) => key.startsWith(prefix))
      .sort(([left], [right]) => left.localeCompare(right));
    const selected = rows.slice(decoded.offset, decoded.offset + limit);
    const nextOffset = decoded.offset + selected.length;
    return {
      kind: 'page',
      items: selected.map(([key, row]) => ({
        key: authorityKey(key),
        snapshot: this.snapshot(row.snapshot)
      })),
      nextCursor: nextOffset < rows.length
        ? scanCursor(this.scanRevision, nextOffset, prefix)
        : undefined
    };
  }

  async removeAllByOwner(
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<bigint> {
    signal?.throwIfAborted();
    let removed = 0;
    for (const [key, row] of [...this.rows.entries()]) {
      if (
        row.snapshot.ownerId !== owner.ownerId
        || row.snapshot.ownerLeaseGeneration !== owner.leaseGeneration
      ) continue;
      if (row.snapshot.allocation.state === 'active') {
        this.adjustCapacity(this.activeCapacity, row.snapshot.allocation, -1);
      } else {
        this.adjustCapacity(this.pendingCapacity, row.snapshot.allocation, -1);
      }
      this.rows.delete(key);
      removed += 1;
    }
    if (removed > 0) this.scanRevision++;
    return BigInt(removed);
  }

  async reserve(
    request: ZLinkObjectReserveRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectReserveResult> {
    signal?.throwIfAborted();
    validateReserve(request);
    const key = creationKey(request.key);
    if (this.validation.identityClaimed?.(key) === true) {
      return { kind: 'conflict', current: { kind: 'missing', storeNow: this.now() } };
    }
    const current = this.rows.get(key);
    if (current !== undefined) {
      if (
        current.snapshot.allocation.objectKind !== request.key.kind
        || current.snapshot.allocation.stableType !== request.intent.stableType
      ) {
        return { kind: 'typeMismatch', current: this.snapshot(current.snapshot) };
      }
      if (current.snapshot.allocation.state === 'active') {
        return { kind: 'alreadyExists', current: this.snapshot(current.snapshot) };
      }
      return { kind: 'conflict', current: this.read(key) };
    }
    const target = creationTarget(request);
    if (!this.validation.isTargetLive(target.descriptor, target.lifecycleGeneration, target.owner)) {
      return { kind: 'conflict', current: this.read(key) };
    }
    const allocation: ZLinkPlacementAllocation = {
      state: 'reserved',
      objectKind: request.key.kind,
      stableType: request.intent.stableType,
      descriptor: target.descriptor,
      descriptorLifecycleGeneration: target.lifecycleGeneration,
      capacity: cloneCapacity(request.capacity)
    };
    if (!this.hasPendingCapacity(allocation)) {
      return { kind: 'placementCapacityExhausted' };
    }
    if (
      this.storeVersion >= MAX_GENERATION
      || this.objectGeneration >= MAX_GENERATION
      || this.ownerGeneration >= MAX_GENERATION
    ) {
      return { kind: 'generationExhausted' };
    }
    const reservationId = randomUUID();
    const snapshot: StoredSnapshot = {
      storeVersion: version(++this.storeVersion),
      payload: Buffer.from(request.creatingPayload),
      objectGeneration: ++this.objectGeneration,
      authorityOwnerGeneration: ++this.ownerGeneration,
      ownerId: target.owner.ownerId,
      ownerLeaseGeneration: target.owner.leaseGeneration,
      allocation,
      pendingCreation: {
        reservationId,
        requestContentReference: request.intent.requestContentReference,
        requestSha256: Buffer.from(request.intent.requestSha256),
        requestEncodedSize: request.intent.requestEncodedSize
      }
    };
    this.rows.set(key, {
      snapshot,
      creation: {
        reservationId,
        target
      }
    });
    this.adjustCapacity(this.pendingCapacity, allocation, 1);
    this.scanRevision++;
    return { kind: 'reserved', reservationId, creating: this.snapshot(snapshot) };
  }

  async commit(
    request: ZLinkObjectCommitRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectCommitResult> {
    signal?.throwIfAborted();
    validatePayload(request.readyPayload);
    if (request.key.kind === 'actor') {
      throw new TypeError('Actor creation must use completeCreation.');
    }
    const terminal = this.creationTerminals.get(request.reservationId);
    const key = creationKey(request.key);
    const row = this.rows.get(key);
    if (terminal === 'committed' && row?.snapshot.allocation.state === 'active') {
      return { kind: 'alreadyCommitted', ready: this.snapshot(row.snapshot) };
    }
    if (
      row === undefined
      || row.snapshot.allocation.state !== 'reserved'
      || row.creation?.reservationId !== request.reservationId
      || row.snapshot.storeVersion.value !== request.expectedStoreVersion
      || !sameCreationTarget(row.creation.target, request.target)
      || !this.validation.isTargetLive(
        row.creation.target.descriptor,
        row.creation.target.lifecycleGeneration,
        row.creation.target.owner
      )
    ) {
      return { kind: 'stale' };
    }
    const nextVersion = this.tryNextStoreVersion();
    if (nextVersion === undefined) return { kind: 'generationExhausted' };
    const pending = row.snapshot.allocation;
    this.adjustCapacity(this.pendingCapacity, pending, -1);
    const active = { ...pending, state: 'active' as const };
    this.adjustCapacity(this.activeCapacity, active, 1);
    const { pendingCreation: _, ...creating } = row.snapshot;
    row.snapshot = {
      ...creating,
      storeVersion: version(nextVersion),
      payload: Buffer.from(request.readyPayload),
      allocation: active
    };
    row.creation.terminal = 'committed';
    this.creationTerminals.set(request.reservationId, 'committed');
    this.scanRevision++;
    return { kind: 'committed', ready: this.snapshot(row.snapshot) };
  }

  async completeCreation(
    request: ZLinkObjectCreationCompleteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectCreationCompleteResult> {
    signal?.throwIfAborted();
    if (request.key.kind !== 'actor') {
      throw new TypeError('completeCreation is reserved for Actor creation.');
    }
    if (request.completion.kind === 'created') {
      validatePayload(request.completion.readyPayload);
    }
    const terminalRecord = validateTerminalForMutation(
      request.completion.terminal,
      request.completion.kind,
      request.reservationId,
      request.key.kind,
      this.now()
    )!;
    const existingTerminal = this.operationTerminals.get(
      creationOperationKey(terminalRecord.operation)
    );
    if (existingTerminal !== undefined
      && existingTerminal.expiresAt.getTime() > this.now().getTime()) {
      return {
        kind: 'alreadyCompleted',
        terminal: copyTerminalRecord(existingTerminal)
      };
    }
    const reservationTerminal = this.creationTerminals.get(request.reservationId);
    if (reservationTerminal === 'committed'
      || reservationTerminal === 'rejected'
      || reservationTerminal === 'failed') {
      const retained = this.operationTerminals.get(
        creationOperationKey(terminalRecord.operation)
      );
      return retained === undefined
        ? { kind: 'stale' }
        : { kind: 'alreadyCompleted', terminal: copyTerminalRecord(retained) };
    }
    const key = creationKey(request.key);
    const row = this.rows.get(key);
    if (
      row === undefined
      || row.snapshot.allocation.state !== 'reserved'
      || row.creation?.reservationId !== request.reservationId
      || row.snapshot.storeVersion.value !== request.expectedStoreVersion
      || !sameCreationTarget(row.creation.target, request.target)
      || !this.validation.isTargetLive(
        row.creation.target.descriptor,
        row.creation.target.lifecycleGeneration,
        row.creation.target.owner
      )
    ) {
      return { kind: 'stale' };
    }
    if (!this.creationTerminalAvailable(terminalRecord)) {
      return { kind: 'stale' };
    }
    const nextVersion = this.tryNextStoreVersion();
    if (nextVersion === undefined) return { kind: 'generationExhausted' };
    const pending = row.snapshot.allocation;
    this.adjustCapacity(this.pendingCapacity, pending, -1);
    let ready: ZLinkAuthoritySnapshot | undefined;
    if (request.completion.kind === 'created') {
      const active = { ...pending, state: 'active' as const };
      this.adjustCapacity(this.activeCapacity, active, 1);
      const { pendingCreation: _, ...creating } = row.snapshot;
      row.snapshot = {
        ...creating,
        storeVersion: version(nextVersion),
        payload: Buffer.from(request.completion.readyPayload),
        allocation: active
      };
      row.creation.terminal = 'committed';
      this.creationTerminals.set(request.reservationId, 'committed');
      ready = this.snapshot(row.snapshot);
    } else {
      this.rows.delete(key);
      row.creation.terminal = request.completion.kind;
      this.creationTerminals.set(request.reservationId, request.completion.kind);
    }
    this.operationTerminals.set(
      creationOperationKey(terminalRecord.operation),
      terminalRecord
    );
    this.scanRevision++;
    const terminal = copyTerminalRecord(terminalRecord);
    return request.completion.kind === 'created'
      ? { kind: 'created', ready: ready!, terminal }
      : { kind: request.completion.kind, terminal };
  }

  async readCreationTerminal(
    operation: ZLinkCreationOperationIdentity,
    signal?: AbortSignal
  ): Promise<ZLinkCreationTerminalReadResult> {
    signal?.throwIfAborted();
    validateCreationOperation(operation);
    const key = creationOperationKey(operation);
    const record = this.operationTerminals.get(key);
    if (record === undefined) return { kind: 'missing', storeNow: this.now() };
    if (record.expiresAt.getTime() <= this.now().getTime()) {
      this.operationTerminals.delete(key);
      return { kind: 'missing', storeNow: this.now() };
    }
    return { kind: 'found', record: copyTerminalRecord(record) };
  }

  private creationTerminalAvailable(record: ZLinkCreationTerminalRecord): boolean {
    const key = creationOperationKey(record.operation);
    const existing = this.operationTerminals.get(key);
    if (existing === undefined) return true;
    if (existing.expiresAt.getTime() <= this.now().getTime()) {
      this.operationTerminals.delete(key);
      return true;
    }
    return false;
  }

  async abort(
    request: ZLinkObjectAbortRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectAbortResult> {
    signal?.throwIfAborted();
    const terminal = this.creationTerminals.get(request.reservationId);
    if (terminal === 'aborted') return { kind: 'alreadyAborted' };
    const key = creationKey(request.key);
    const row = this.rows.get(key);
    if (
      row === undefined
      || row.snapshot.allocation.state !== 'reserved'
      || row.creation?.reservationId !== request.reservationId
      || row.snapshot.storeVersion.value !== request.expectedStoreVersion
      || !sameCreationTarget(row.creation.target, request.target)
    ) {
      return { kind: 'stale' };
    }
    this.adjustCapacity(this.pendingCapacity, row.snapshot.allocation, -1);
    this.rows.delete(key);
    this.creationTerminals.set(request.reservationId, 'aborted');
    this.scanRevision++;
    return { kind: 'aborted' };
  }

  async reserveRelocationCapacity(
    request: ZLinkRelocationCapacityReservationRequest,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityReserveResult> {
    signal?.throwIfAborted();
    validateRelocationReservation(request);
    const existing = this.capacityReservations.get(request.reservationId);
    if (existing !== undefined) {
      return sameRelocationRequest(existing.request, request)
        ? { kind: 'alreadyReserved', fence: existing.fence }
        : { kind: 'conflict', current: this.read(request.authorityKey.value) };
    }
    const row = this.rows.get(request.authorityKey.value);
    if (
      row === undefined
      || row.snapshot.allocation.state !== 'active'
      || !sameFenceAuthority(request, request.authorityKey.value, request.expectedStoreVersion, row.snapshot)
    ) {
      return { kind: 'conflict', current: this.read(request.authorityKey.value) };
    }
    if (!this.targetLive(request)) return { kind: 'targetUnavailable' };
    const target = targetAllocation(request);
    if (!this.hasPendingCapacity(target)) return { kind: 'placementCapacityExhausted' };
    const fence = capacityFence(request.reservationId);
    this.capacityReservations.set(request.reservationId, {
      request: cloneRelocationRequest(request),
      fence,
      state: 'reserved'
    });
    this.adjustCapacity(this.pendingCapacity, target, 1);
    return { kind: 'reserved', fence };
  }

  async abortRelocationCapacity(
    fence: ZLinkRelocationCapacityFence,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityAbortResult> {
    signal?.throwIfAborted();
    const reservation = this.capacityReservations.get(fence.value);
    if (reservation === undefined) return 'stale';
    if (reservation.state === 'aborted') return 'alreadyAborted';
    if (reservation.state === 'committed') return 'alreadyCommitted';
    if (reservation.state === 'prepared') return 'stale';
    reservation.state = 'aborted';
    this.adjustCapacity(
      this.pendingCapacity,
      targetAllocation(reservation.request),
      -1
    );
    return 'aborted';
  }

  async prepareAggregate(
    request: ZLinkAggregatePrepareRequest,
    signal?: AbortSignal
  ): Promise<ZLinkAggregatePrepareResult> {
    signal?.throwIfAborted();
    validateAggregateRequest(request);
    const aggregateKey = aggregateRecordKey(request.aggregateId.value, request.aggregateGeneration);
    const existing = this.aggregates.get(aggregateKey);
    if (existing !== undefined) {
      if (existing.state === 'prepared' && sameAggregateRequest(existing.request, request)) {
        return { kind: 'alreadyPrepared', fence: existing.fence };
      }
      return existing.state === 'committed'
        ? { kind: 'stale' }
        : { kind: 'conflict' };
    }
    if (!this.validation.isTargetLive(
      request.targetDescriptor,
      request.targetDescriptorLifecycleGeneration,
      request.targetOwner
    )) {
      return { kind: 'conflict' };
    }
    const target = aggregateTargetAllocation(request);
    const capacityReservation = this.capacityReservations.get(request.aggregateId.value);
    const adoptsCapacityReservation = capacityReservation !== undefined
      && capacityReservation.state === 'reserved'
      && sameOwner(capacityReservation.request.targetOwner, request.targetOwner)
      && sameDescriptor(
        capacityReservation.request.targetDescriptor,
        request.targetDescriptor
      )
      && capacityReservation.request.targetNodeLifecycleGeneration
        === request.targetDescriptorLifecycleGeneration
      && sameCapacity(capacityReservation.request.capacity, request.capacity);
    if (!adoptsCapacityReservation && !this.hasPendingCapacity(target)) {
      return { kind: 'conflict' };
    }
    let sourceCapacity: ZLinkCapacityVector = { actors: 0, spots: 0 };
    for (const participant of request.participants) {
      const row = this.rows.get(participant.authorityKey.value);
      if (
        row === undefined
        || row.snapshot.allocation.state !== 'active'
        || row.snapshot.storeVersion.value !== participant.expectedStoreVersion.value
      ) {
        return { kind: 'conflict' };
      }
      if (participant.ownerTransition === 'preserve') {
        if (!this.isOwnerLive(row.snapshot)) return { kind: 'conflict' };
        continue;
      }
      sourceCapacity = addCapacity(sourceCapacity, row.snapshot.allocation.capacity);
    }
    if (!sameCapacity(sourceCapacity, request.capacity)) return { kind: 'conflict' };
    const fence: ZLinkAggregateFence = {
      aggregateId: request.aggregateId,
      aggregateGeneration: request.aggregateGeneration
    };
    if (adoptsCapacityReservation) {
      capacityReservation.state = 'prepared';
      capacityReservation.aggregate = aggregateKey;
    } else {
      this.adjustCapacity(this.pendingCapacity, target, 1);
    }
    this.aggregates.set(aggregateKey, {
      request: cloneAggregateRequest(request),
      fence,
      ...(adoptsCapacityReservation
        ? { capacityReservationId: request.aggregateId.value }
        : {}),
      state: 'prepared'
    });
    return { kind: 'prepared', fence };
  }

  async commitAggregate(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<ZLinkAggregateCommitResult> {
    signal?.throwIfAborted();
    const key = aggregateRecordKey(fence.aggregateId.value, fence.aggregateGeneration);
    const aggregate = this.aggregates.get(key);
    if (aggregate === undefined) return { kind: 'stale' };
    if (aggregate.state === 'committed') return { kind: 'alreadyCommitted' };
    if (aggregate.state !== 'prepared') return { kind: 'stale' };
    let versionsNeeded = 0n;
    let ownersNeeded = 0n;
    const rows: Array<{
      readonly participant: ZLinkAggregatePrepareRequest['participants'][number];
      readonly row: AuthorityRow;
      readonly changesOwner: boolean;
    }> = [];
    for (const participant of aggregate.request.participants) {
      const row = this.rows.get(participant.authorityKey.value);
      if (
        row === undefined
        || row.snapshot.allocation.state !== 'active'
        || row.snapshot.storeVersion.value !== participant.expectedStoreVersion.value
      ) {
        return { kind: 'stale' };
      }
      if (participant.ownerTransition === 'newOwner') {
        if (!this.validation.isTargetLive(
          aggregate.request.targetDescriptor,
          aggregate.request.targetDescriptorLifecycleGeneration,
          aggregate.request.targetOwner
        )) {
          return { kind: 'stale' };
        }
        ownersNeeded++;
      } else if (!this.isOwnerLive(row.snapshot)) {
        return { kind: 'stale' };
      }
      versionsNeeded++;
      rows.push({ participant, row, changesOwner: participant.ownerTransition === 'newOwner' });
    }
    if (
      this.storeVersion + versionsNeeded > MAX_GENERATION
      || this.ownerGeneration + ownersNeeded > MAX_GENERATION
    ) {
      return { kind: 'generationExhausted' };
    }
    for (const entry of rows) {
      const nextVersion = ++this.storeVersion;
      if (!entry.changesOwner) {
        entry.row.snapshot = {
          ...entry.row.snapshot,
          storeVersion: version(nextVersion),
          payload: Buffer.from(entry.participant.authorityPayload)
        };
      } else {
        const owner = aggregate.request.targetOwner;
        this.adjustCapacity(this.activeCapacity, entry.row.snapshot.allocation, -1);
        entry.row.snapshot = {
          storeVersion: version(nextVersion),
          payload: Buffer.from(entry.participant.authorityPayload),
          objectGeneration: entry.row.snapshot.objectGeneration,
          authorityOwnerGeneration: ++this.ownerGeneration,
          ownerId: owner.ownerId,
          ownerLeaseGeneration: owner.leaseGeneration,
          allocation: {
            ...entry.row.snapshot.allocation,
            descriptor: { ...aggregate.request.targetDescriptor },
            descriptorLifecycleGeneration:
              aggregate.request.targetDescriptorLifecycleGeneration,
            capacity: cloneCapacity(entry.row.snapshot.allocation.capacity)
          }
        };
      }
    }
    const aggregateTarget = aggregateTargetAllocation(aggregate.request);
    this.adjustCapacity(this.pendingCapacity, aggregateTarget, -1);
    this.adjustCapacity(this.activeCapacity, aggregateTarget, 1);
    aggregate.state = 'committed';
    if (aggregate.capacityReservationId !== undefined) {
      const capacityReservation =
        this.capacityReservations.get(aggregate.capacityReservationId);
      if (capacityReservation !== undefined) capacityReservation.state = 'committed';
    }
    this.scanRevision++;
    return { kind: 'committed' };
  }

  async abortAggregate(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<ZLinkAggregateAbortResult> {
    signal?.throwIfAborted();
    const key = aggregateRecordKey(fence.aggregateId.value, fence.aggregateGeneration);
    const aggregate = this.aggregates.get(key);
    if (aggregate === undefined) return { kind: 'stale' };
    if (aggregate.state === 'aborted') return { kind: 'alreadyAborted' };
    if (aggregate.state !== 'prepared') return { kind: 'stale' };
    this.adjustCapacity(
      this.pendingCapacity,
      aggregateTargetAllocation(aggregate.request),
      -1
    );
    aggregate.state = 'aborted';
    if (aggregate.capacityReservationId !== undefined) {
      const capacityReservation =
        this.capacityReservations.get(aggregate.capacityReservationId);
      if (capacityReservation !== undefined) capacityReservation.state = 'aborted';
    }
    return { kind: 'aborted' };
  }

  private read(key: string): ZLinkAuthorityReadResult {
    const row = this.rows.get(key);
    return row === undefined
      ? { kind: 'missing', storeNow: this.now() }
      : this.snapshot(row.snapshot);
  }

  private snapshot(snapshot: StoredSnapshot): ZLinkAuthoritySnapshot {
    return {
      kind: 'snapshot',
      ...snapshot,
      payload: Buffer.from(snapshot.payload),
      allocation: cloneAllocation(snapshot.allocation),
      ...(snapshot.pendingCreation === undefined
        ? {}
        : {
            pendingCreation: {
              ...snapshot.pendingCreation,
              requestSha256: Buffer.from(snapshot.pendingCreation.requestSha256)
            }
          }),
      storeNow: this.now()
    };
  }

  private stored(snapshot: StoredSnapshot): ZLinkAuthorityCompareExchangeResult {
    const value = this.snapshot(snapshot);
    const { kind: _, ...stored } = value;
    return { kind: 'stored', ...stored };
  }

  private isOwnerLive(snapshot: StoredSnapshot): boolean {
    return this.validation.isTargetLive(
      snapshot.allocation.descriptor,
      snapshot.allocation.descriptorLifecycleGeneration,
      { ownerId: snapshot.ownerId, leaseGeneration: snapshot.ownerLeaseGeneration }
    );
  }

  private targetLive(request: ZLinkRelocationCapacityReservationRequest): boolean {
    return this.validation.isTargetLive(
      request.targetDescriptor,
      request.targetNodeLifecycleGeneration,
      request.targetOwner
    );
  }

  private hasPendingCapacity(allocation: ZLinkPlacementAllocation): boolean {
    return this.validation.placementCapacityAvailable?.(
      allocation.descriptor,
      allocation.capacity,
      this.capacityVectorUsage(this.pendingCapacity, allocation),
      this.capacityVectorUsage(this.activeCapacity, allocation)
    ) ?? true;
  }

  private moveCapacity(
    source: ZLinkPlacementAllocation,
    request: ZLinkRelocationCapacityReservationRequest
  ): void {
    this.adjustCapacity(this.activeCapacity, source, -1);
    const target = targetAllocation(request);
    this.adjustCapacity(this.pendingCapacity, target, -1);
    this.adjustCapacity(this.activeCapacity, target, 1);
  }

  private adjustCapacity(
    values: Map<string, number>,
    allocation: ZLinkPlacementAllocation,
    multiplier: 1 | -1
  ): void {
    for (const entry of capacityEntries(
      allocation.descriptor,
      allocation.descriptorLifecycleGeneration,
      allocation.capacity
    )) {
      this.adjust(values, entry.key, entry.count * multiplier);
    }
  }

  private capacityVectorUsage(
    values: Map<string, number>,
    allocation: ZLinkPlacementAllocation
  ): ZLinkCapacityVector {
    const descriptor = allocation.descriptor;
    const lifecycle = allocation.descriptorLifecycleGeneration;
    const spotType = allocation.capacity.spotType;
    return {
      actors: values.get(capacityKey(descriptor, lifecycle, 'actor', '')) ?? 0,
      spots: values.get(capacityKey(descriptor, lifecycle, 'spot', '')) ?? 0,
      ...(spotType === undefined ? {} : {
        spotType: {
          ...spotType,
          count: values.get(capacityKey(
            descriptor,
            lifecycle,
            spotType.objectKind,
            spotType.stableType
          )) ?? 0
        }
      })
    };
  }

  private adjust(values: Map<string, number>, key: string, delta: number): void {
    const next = (values.get(key) ?? 0) + delta;
    if (next < 0) throw new Error('In-memory placement capacity counter underflow.');
    if (next === 0) values.delete(key);
    else values.set(key, next);
  }

  capacityUsage(
    descriptor: ZLinkMeshNodeDescriptorKey,
    lifecycleGeneration: bigint,
    objectKind: string,
    stableType: string
  ): { readonly active: number; readonly reserved: number } {
    const key = objectKind === 'actor'
      ? capacityKey(descriptor, lifecycleGeneration, 'actor', '')
      : objectKind === 'user_spot' || objectKind === 'instance_spot'
        ? capacityKey(descriptor, lifecycleGeneration, objectKind, stableType)
        : capacityKey(descriptor, lifecycleGeneration, 'spot', '');
    return {
      active: this.activeCapacity.get(key) ?? 0,
      reserved: this.pendingCapacity.get(key) ?? 0
    };
  }

  private tryNextStoreVersion(): bigint | undefined {
    return this.storeVersion >= MAX_GENERATION ? undefined : ++this.storeVersion;
  }
}

function validateAuthorityMutation(mutation: ZLinkAuthorityMutation): void {
  if (mutation.kind === 'delete' || mutation.kind === 'restore') return;
  if (mutation.kind === 'rebindOwnerLease') {
    if (
      mutation.expectedOwner.ownerId !== mutation.targetOwner.ownerId
      || mutation.expectedOwner.leaseGeneration === mutation.targetOwner.leaseGeneration
      || mutation.expectedOwner.leaseGeneration <= 0n
      || mutation.targetOwner.leaseGeneration <= 0n
    ) {
      throw new TypeError('Authority lease rebind must retain the owner and change the lease generation.');
    }
    return;
  }
  const hasOwner = mutation.targetOwner !== undefined;
  const hasFence = mutation.relocationCapacityFence !== undefined;
  if (mutation.generationTransition === 'preserve' ? (hasOwner || hasFence) : (!hasOwner || !hasFence)) {
    throw new TypeError('Authority owner and relocation fence do not match the generation transition.');
  }
}

function validateReserve(request: ZLinkObjectReserveRequest): void {
  requireText(request.key.globalId, 'object global ID');
  requireText(request.intent.stableType, 'stable type');
  requireText(request.intent.requestContentReference, 'creation content reference');
  validatePayload(request.creatingPayload);
  validateCapacityVector(request.capacity);
  if (
    request.intent.requestSha256.byteLength !== 32
    || request.intent.requestEncodedSize < 0n
    || request.intent.requestEncodedSize > BigInt(MAX_PAYLOAD_BYTES)
  ) {
    throw new TypeError('Object creation content receipt is invalid.');
  }
}

function validateCreationOperation(operation: ZLinkCreationOperationIdentity): void {
  const sourceRid = String(operation.sourceNodeRid);
  const sourceRidBytes = Buffer.byteLength(sourceRid, 'utf8');
  if (sourceRidBytes < 1 || sourceRidBytes > 255 || sourceRid.includes('\0')) {
    throw new TypeError('Creation terminal source node RID must contain 1..255 UTF-8 bytes without NUL.');
  }
  if (
    operation.sourceNodeGeneration < 1n
    || operation.sourceNodeGeneration > MAX_GENERATION
    || operation.operationId.high < 0n
    || operation.operationId.high > MAX_U64
    || operation.operationId.low < 0n
    || operation.operationId.low > MAX_U64
    || (operation.operationId.high === 0n && operation.operationId.low === 0n)
  ) {
    throw new RangeError('Creation terminal source generation and operation ID are invalid.');
  }
}

function validateTerminalForMutation(
  publication: ZLinkCreationTerminalPublication | undefined,
  state: ZLinkCreationTerminalRecord['state'],
  reservationId: string,
  objectKind: ZLinkPlacementAllocation['objectKind'],
  storeNow: Date
): ZLinkCreationTerminalRecord | undefined {
  if (publication === undefined) return undefined;
  validateCreationOperation(publication.operation);
  if (
    publication.terminalEnvelope.byteLength > MAX_PAYLOAD_BYTES
    || publication.terminalEnvelopeSha256.byteLength !== 32
  ) {
    throw new RangeError('Creation terminal envelope must not exceed 1 MiB and requires a SHA-256 digest.');
  }
  const actualSha = createHash('sha256').update(publication.terminalEnvelope).digest();
  if (!timingSafeEqual(actualSha, Buffer.from(publication.terminalEnvelopeSha256))) {
    throw new TypeError('Creation terminal envelope SHA-256 does not match its bytes.');
  }
  const deadlineMs = publication.operationDeadline.getTime();
  const expiresAtMs = deadlineMs + CREATION_TERMINAL_RETENTION_MS;
  if (
    !Number.isSafeInteger(deadlineMs)
    || !Number.isSafeInteger(expiresAtMs)
    || expiresAtMs <= storeNow.getTime()
  ) {
    throw new RangeError('Creation terminal expiry must be the live operation deadline plus five minutes.');
  }
  return {
    state,
    operation: copyCreationOperation(publication.operation),
    reservationId: requireText(reservationId, 'creation reservation ID'),
    objectKind,
    terminalEnvelope: Buffer.from(publication.terminalEnvelope),
    terminalEnvelopeSha256: Buffer.from(publication.terminalEnvelopeSha256),
    expiresAt: new Date(expiresAtMs),
    storeNow: new Date(storeNow.getTime())
  };
}

function creationOperationKey(operation: ZLinkCreationOperationIdentity): string {
  return [
    String(operation.sourceNodeRid),
    operation.sourceNodeGeneration.toString(),
    operation.operationId.high.toString(16).padStart(16, '0'),
    operation.operationId.low.toString(16).padStart(16, '0')
  ].join('\0');
}

function copyCreationOperation(
  operation: ZLinkCreationOperationIdentity
): ZLinkCreationOperationIdentity {
  return {
    sourceNodeRid: operation.sourceNodeRid,
    sourceNodeGeneration: operation.sourceNodeGeneration,
    operationId: {
      high: operation.operationId.high,
      low: operation.operationId.low
    }
  };
}

function copyTerminalRecord(record: ZLinkCreationTerminalRecord): ZLinkCreationTerminalRecord {
  return {
    ...record,
    operation: copyCreationOperation(record.operation),
    terminalEnvelope: Buffer.from(record.terminalEnvelope),
    terminalEnvelopeSha256: Buffer.from(record.terminalEnvelopeSha256),
    expiresAt: new Date(record.expiresAt),
    storeNow: new Date(record.storeNow)
  };
}

function validateRelocationReservation(request: ZLinkRelocationCapacityReservationRequest): void {
  requireText(request.reservationId, 'relocation reservation ID');
  requireText(request.stableType, 'stable type');
  validateCapacityVector(request.capacity);
}

function validateAggregateRequest(request: ZLinkAggregatePrepareRequest): void {
  requireText(request.aggregateId.value, 'aggregate ID');
  if (request.aggregateGeneration < 1n || request.participants.length < 1) {
    throw new RangeError('Aggregate generation and participant count are invalid.');
  }
  if (request.inventoryDigest.byteLength !== 32) {
    throw new TypeError('Aggregate inventory digest must contain 32 bytes.');
  }
  validateCapacityVector(request.capacity);
  const keys = request.participants.map(participant => participant.authorityKey.value);
  const sorted = [...keys].sort();
  if (new Set(keys).size !== keys.length || keys.some((key, index) => key !== sorted[index])) {
    throw new TypeError('Aggregate participants must be unique and canonically sorted.');
  }
}

function validatePayload(payload: Uint8Array): void {
  if (payload.byteLength > MAX_PAYLOAD_BYTES) {
    throw new RangeError('Authority payload exceeds 1 MiB.');
  }
}

function validateCapacityVector(value: ZLinkCapacityVector): void {
  const counts = [value.actors, value.spots, value.spotType?.count ?? 0];
  if (counts.some(count => !Number.isInteger(count) || count < 0 || count > 0x7fff_ffff)
    || counts.every(count => count === 0)
    || value.spotType !== undefined && value.spotType.count === 0) {
    throw new RangeError('Placement capacity vector is invalid.');
  }
}

function sameFenceAuthority(
  request: ZLinkRelocationCapacityReservationRequest,
  key: string,
  expectedVersion: ZLinkAuthorityStoreVersion,
  current: StoredSnapshot
): boolean {
  return request.authorityKey.value === key
    && request.expectedStoreVersion.value === expectedVersion.value
    && current.storeVersion.value === expectedVersion.value
    && current.allocation.state === 'active'
    && current.allocation.objectKind === request.objectKind
    && current.allocation.stableType === request.stableType
    && sameDescriptor(current.allocation.descriptor, request.sourceDescriptor)
    && current.allocation.descriptorLifecycleGeneration === request.sourceNodeLifecycleGeneration
    && current.ownerId === request.sourceOwner.ownerId
    && current.ownerLeaseGeneration === request.sourceOwner.leaseGeneration;
}

function targetAllocation(
  request: ZLinkRelocationCapacityReservationRequest
): ZLinkPlacementAllocation {
  return {
    state: 'active',
    objectKind: request.objectKind,
    stableType: request.stableType,
    descriptor: { ...request.targetDescriptor },
    descriptorLifecycleGeneration: request.targetNodeLifecycleGeneration,
    capacity: cloneCapacity(request.capacity)
  };
}

function aggregateTargetAllocation(
  request: ZLinkAggregatePrepareRequest
): ZLinkPlacementAllocation {
  return {
    state: 'active',
    objectKind: request.capacity.spotType?.objectKind ?? 'actor',
    stableType: request.capacity.spotType?.stableType ?? '',
    descriptor: { ...request.targetDescriptor },
    descriptorLifecycleGeneration: request.targetDescriptorLifecycleGeneration,
    capacity: cloneCapacity(request.capacity)
  };
}

function addCapacity(
  left: ZLinkCapacityVector,
  right: ZLinkCapacityVector
): ZLinkCapacityVector {
  let spotType = left.spotType;
  if (right.spotType !== undefined) {
    if (spotType !== undefined
      && (spotType.objectKind !== right.spotType.objectKind
        || spotType.stableType !== right.spotType.stableType)) {
      throw new TypeError('Aggregate capacity contains more than one Spot type.');
    }
    spotType = {
      ...right.spotType,
      count: (spotType?.count ?? 0) + right.spotType.count
    };
  }
  return {
    actors: left.actors + right.actors,
    spots: left.spots + right.spots,
    ...(spotType === undefined ? {} : { spotType })
  };
}

function cloneAllocation(allocation: ZLinkPlacementAllocation): ZLinkPlacementAllocation {
  return {
    ...allocation,
    descriptor: { ...allocation.descriptor },
    capacity: cloneCapacity(allocation.capacity)
  };
}

function creationTarget(request: ZLinkObjectReserveRequest): CreationTarget {
  return {
    descriptor: { meshName: request.target.meshName, rid: request.target.nodeRid },
    lifecycleGeneration: request.target.nodeLifecycleGeneration,
    owner: { ...request.target.owner }
  };
}

function sameCreationTarget(target: CreationTarget, actual: ZLinkObjectCommitRequest['target']): boolean {
  return target.descriptor.meshName === actual.meshName
    && sameRid(target.descriptor.rid, actual.nodeRid)
    && target.lifecycleGeneration === actual.nodeLifecycleGeneration
    && sameOwner(target.owner, actual.owner);
}

function sameOwner(left: ZLinkLocationOwnerToken, right: ZLinkLocationOwnerToken): boolean {
  return left.ownerId === right.ownerId && left.leaseGeneration === right.leaseGeneration;
}

function sameDescriptor(left: ZLinkMeshNodeDescriptorKey, right: ZLinkMeshNodeDescriptorKey): boolean {
  return left.meshName === right.meshName && sameRid(left.rid, right.rid);
}

function sameRid(left: unknown, right: unknown): boolean {
  return String(left) === String(right);
}

function capacityEntries(
  descriptor: ZLinkMeshNodeDescriptorKey,
  lifecycle: bigint,
  capacity: ZLinkCapacityVector
): readonly { readonly key: string; readonly count: number }[] {
  return [
    ...(capacity.actors === 0 ? [] : [{
      key: capacityKey(descriptor, lifecycle, 'actor', ''),
      count: capacity.actors
    }]),
    ...(capacity.spots === 0 ? [] : [{
      key: capacityKey(descriptor, lifecycle, 'spot', ''),
      count: capacity.spots
    }]),
    ...(capacity.spotType === undefined ? [] : [{
      key: capacityKey(
        descriptor,
        lifecycle,
        capacity.spotType.objectKind,
        capacity.spotType.stableType
      ),
      count: capacity.spotType.count
    }])
  ];
}

function cloneCapacity(value: ZLinkCapacityVector): ZLinkCapacityVector {
  return {
    actors: value.actors,
    spots: value.spots,
    ...(value.spotType === undefined ? {} : { spotType: { ...value.spotType } })
  };
}

function sameCapacity(left: ZLinkCapacityVector, right: ZLinkCapacityVector): boolean {
  return left.actors === right.actors
    && left.spots === right.spots
    && left.spotType?.objectKind === right.spotType?.objectKind
    && left.spotType?.stableType === right.spotType?.stableType
    && left.spotType?.count === right.spotType?.count;
}

function capacityKey(
  descriptor: ZLinkMeshNodeDescriptorKey,
  lifecycle: bigint,
  kind: string,
  stableType: string
): string {
  return `${descriptor.meshName}\0${String(descriptor.rid)}\0${lifecycle}\0${kind}\0${stableType}`;
}

function creationKey(key: ZLinkObjectReserveRequest['key']): string {
  return encodeAuthorityKey(key.kind, key.globalId).value;
}

function authorityKey(value: string): ZLinkAuthorityKey {
  return { value } as ZLinkAuthorityKey;
}

function version(value: bigint): ZLinkAuthorityStoreVersion {
  return { value: value.toString() } as ZLinkAuthorityStoreVersion;
}

function capacityFence(value: string): ZLinkRelocationCapacityFence {
  return { value } as ZLinkRelocationCapacityFence;
}

function scanCursor(revision: bigint, offset: number, prefix: string): ZLinkAuthorityScanCursor {
  const encoded = Buffer.from(JSON.stringify({
    revision: revision.toString(),
    offset,
    prefix
  })).toString('base64url');
  const factory = requireScanCursorFactory();
  return factory(encoded);
}

function requireScanCursorFactory(): (encoded: string) => ZLinkAuthorityScanCursor {
  // Imported as a type to keep the provider contract free of runtime cycles.
  return (encoded) => ({ encoded } as ZLinkAuthorityScanCursor);
}

function decodeCursor(encoded: string): { revision: bigint; offset: number; prefix: string } {
  try {
    const value = JSON.parse(Buffer.from(encoded, 'base64url').toString('utf8')) as {
      readonly revision: string;
      readonly offset: number;
      readonly prefix: string;
    };
    if (!Number.isInteger(value.offset) || value.offset < 0 || typeof value.prefix !== 'string') {
      throw new Error();
    }
    return { revision: BigInt(value.revision), offset: value.offset, prefix: value.prefix };
  } catch {
    throw new TypeError('Authority scan cursor is invalid.');
  }
}

function sameRelocationRequest(
  left: ZLinkRelocationCapacityReservationRequest,
  right: ZLinkRelocationCapacityReservationRequest
): boolean {
  return JSON.stringify(relocationComparable(left)) === JSON.stringify(relocationComparable(right));
}

function relocationComparable(request: ZLinkRelocationCapacityReservationRequest): unknown {
  return {
    ...request,
    expectedStoreVersion: request.expectedStoreVersion.value,
    sourceNodeLifecycleGeneration: request.sourceNodeLifecycleGeneration.toString(),
    targetNodeLifecycleGeneration: request.targetNodeLifecycleGeneration.toString(),
    sourceOwner: { ...request.sourceOwner, leaseGeneration: request.sourceOwner.leaseGeneration.toString() },
    targetOwner: { ...request.targetOwner, leaseGeneration: request.targetOwner.leaseGeneration.toString() },
    sourceDescriptor: {
      meshName: request.sourceDescriptor.meshName,
      rid: String(request.sourceDescriptor.rid)
    },
    targetDescriptor: {
      meshName: request.targetDescriptor.meshName,
      rid: String(request.targetDescriptor.rid)
    }
  };
}

function cloneRelocationRequest(
  request: ZLinkRelocationCapacityReservationRequest
): ZLinkRelocationCapacityReservationRequest {
  return {
    ...request,
    authorityKey: { ...request.authorityKey },
    expectedStoreVersion: { ...request.expectedStoreVersion },
    sourceOwner: { ...request.sourceOwner },
    targetOwner: { ...request.targetOwner },
    sourceDescriptor: { ...request.sourceDescriptor },
    targetDescriptor: { ...request.targetDescriptor },
    capacity: cloneCapacity(request.capacity)
  };
}

function aggregateRecordKey(id: string, generation: bigint): string {
  return `${id}\0${generation}`;
}

function sameAggregateRequest(
  left: ZLinkAggregatePrepareRequest,
  right: ZLinkAggregatePrepareRequest
): boolean {
  if (
    left.aggregateId.value !== right.aggregateId.value
    || left.aggregateGeneration !== right.aggregateGeneration
    || !sameOwner(left.targetOwner, right.targetOwner)
    || !Buffer.from(left.inventoryDigest).equals(Buffer.from(right.inventoryDigest))
    || left.participants.length !== right.participants.length
    || !sameDescriptor(left.targetDescriptor, right.targetDescriptor)
    || left.targetDescriptorLifecycleGeneration
      !== right.targetDescriptorLifecycleGeneration
    || !sameCapacity(left.capacity, right.capacity)
  ) {
    return false;
  }
  return left.participants.every((participant, index) => {
    const other = right.participants[index]!;
    return participant.authorityKey.value === other.authorityKey.value
      && participant.expectedStoreVersion.value === other.expectedStoreVersion.value
      && participant.ownerTransition === other.ownerTransition
      && Buffer.from(participant.authorityPayload).equals(Buffer.from(other.authorityPayload))
      && Buffer.from(participant.membershipMutation).equals(Buffer.from(other.membershipMutation));
  });
}

function cloneAggregateRequest(request: ZLinkAggregatePrepareRequest): ZLinkAggregatePrepareRequest {
  return {
    ...request,
    aggregateId: { ...request.aggregateId },
    targetOwner: { ...request.targetOwner },
    targetDescriptor: { ...request.targetDescriptor },
    capacity: cloneCapacity(request.capacity),
    inventoryDigest: Buffer.from(request.inventoryDigest),
    participants: request.participants.map(participant => ({
      ...participant,
      authorityKey: { ...participant.authorityKey },
      expectedStoreVersion: { ...participant.expectedStoreVersion },
      authorityPayload: Buffer.from(participant.authorityPayload),
      membershipMutation: Buffer.from(participant.membershipMutation)
    }))
  };
}

function requireText(value: string, name: string): string {
  if (value.length === 0 || value.includes('\0')) {
    throw new TypeError(`${name} must be non-empty text without NUL.`);
  }
  return value;
}

import { createHash, randomUUID, timingSafeEqual } from 'node:crypto';
import type {
  ZLinkLocationStore,
  ZLinkLocationPage,
  ZLinkClientServerServerDescriptor,
  ZLinkClientServerServerDescriptorKey,
  ZLinkFanoutPublisherDescriptor,
  ZLinkFanoutPublisherDescriptorKey,
  ZLinkMeshNodeDescriptor,
  ZLinkMeshNodeDescriptorKey,
  ZLinkPageRequest,
  ZLinkStoreCondition,
  ZLinkStoreKey,
  ZLinkStoreReadResult,
  ZLinkStoreScanCursor,
  ZLinkStoreScanRequest,
  ZLinkStoreScanResult,
  ZLinkStoreWriteRequest,
  ZLinkStoreWriteResult,
  ZLinkStoreVersion
} from '../../contracts';
import type {
  ZLinkAggregateAbortResult,
  ZLinkAggregateCommitResult,
  ZLinkAggregateFence,
  ZLinkAggregateId,
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
  ZLinkCreationOperationIdentity,
  ZLinkCreationTerminalReadResult,
  ZLinkCreationTerminalRecord,
  ZLinkLocationOwnerToken,
  ZLinkLocationWriteResult,
  ZLinkLocationWriteStatus,
  ZLinkObjectCommitRequest,
  ZLinkObjectCommitResult,
  ZLinkObjectAbortRequest,
  ZLinkObjectAbortResult,
  ZLinkObjectCreationCompleteRequest,
  ZLinkObjectCreationCompleteResult,
  ZLinkObjectReserveRequest,
  ZLinkObjectReserveResult,
  ZLinkOwnerLeaseClaimResult,
  ZLinkOwnerLeaseReadResult,
  ZLinkOwnerLeaseReleaseResult,
  ZLinkOwnerLeaseRenewResult,
  ZLinkPlacementAllocation,
  ZLinkRelocationCapacityAbortResult,
  ZLinkRelocationCapacityFence,
  ZLinkRelocationCapacityReservationRequest,
  ZLinkRelocationCapacityReserveResult
} from '../../contracts/Locations';
import { ZLinkLocationWriteIntent } from '../../contracts/Locations';
import type {
  ZLinkActorLocation,
  ZLinkActorLocationFilter,
  ZLinkActorLocationKey,
  ZLinkRouteLocation,
  ZLinkRouteLocationFilter,
  ZLinkRouteLocationKey,
  ZLinkSpotLocation,
  ZLinkSpotLocationFilter,
  ZLinkSpotLocationKey
} from './internal-location-contracts';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole
} from '../../contracts';
import { ZLinkLocationWriteStatus as WriteStatus } from '../../contracts/Locations';
import { ZLinkInMemoryLocationStore } from './in-memory-location-store';
import { storeKey } from './in-memory-provider-location-store';
import { decodeAuthorityKey, encodeAuthorityKey } from './authority-key-codec';
import { ZLinkAggregateInventoryStore } from './aggregate-inventory-store';

const PREFIX = 'zlink:v11:';
const OWNER_COUNTER_KEY = storeKey(`${PREFIX}owner-counter`);
const MAX_GENERATION = 0x7fff_ffff_ffff_ffffn;
const MAX_U64 = 0xffff_ffff_ffff_ffffn;
const MAX_CREATION_TERMINAL_BYTES = 1024 * 1024;
const CREATION_TERMINAL_RETENTION_MS = 5 * 60 * 1000;

type StoredAuthoritySnapshot = Omit<
  ZLinkAuthoritySnapshot,
  'kind' | 'storeVersion' | 'storeNow'
>;

interface AuthorityRecord {
  readonly snapshot: StoredAuthoritySnapshot;
  readonly reservationId?: string;
  readonly terminal?: 'committed' | 'rejected' | 'failed' | 'aborted';
  readonly aggregate?: AggregateParticipantFenceRecord;
  readonly visibleStoreVersion?: string;
}

interface CapacityRecord {
  readonly active: CapacityUsage;
  readonly pending: CapacityUsage;
}

interface CapacityUsage {
  readonly actors: number;
  readonly spots: number;
  readonly spotTypes: Readonly<Record<string, number>>;
}

interface RelocationCapacityRecord {
  readonly request: ZLinkRelocationCapacityReservationRequest;
  readonly state: 'reserved' | 'prepared' | 'committed' | 'aborted';
}

interface AggregateParticipantFenceRecord {
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly index: number;
  readonly expectedStoreVersion: string;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly targetAuthorityOwnerGeneration: bigint;
  readonly authorityPayloadSha256: string;
  readonly membershipMutationSha256: string;
}

interface AggregateRecord {
  readonly state: 'staging' | 'prepared' | 'committed' | 'aborted';
  readonly requestFingerprint: string;
  readonly participantCount: number;
  readonly inventoryDigest: Uint8Array;
  readonly targetDescriptor: ZLinkMeshNodeDescriptorKey;
  readonly targetDescriptorLifecycleGeneration: bigint;
  readonly capacity: ZLinkCapacityVector;
  readonly targetOwner: ZLinkLocationOwnerToken;
  readonly capacityReservationId?: string;
}

type NewOwnerAuthorityMutation = Omit<
  Extract<ZLinkAuthorityMutation, { readonly kind: 'put' }>,
  'generationTransition'
> & { readonly generationTransition: 'newOwner' };

interface OwnerRecord {
  readonly ownerId: string;
  readonly leaseGeneration: string;
}

interface MeshRecord {
  readonly generation: string;
  readonly descriptor: ZLinkMeshNodeDescriptor;
}

interface DescriptorRecord<T> {
  readonly generation: string;
  readonly descriptor: T;
}

interface OwnerCleanupCandidate {
  readonly key: ZLinkStoreKey;
  readonly scanVersion: ZLinkStoreVersion;
}

interface OwnerCleanupRecord {
  readonly descriptor: OwnedStoreRecord;
}

const MAX_OWNER_CLEANUP_BATCH_ROWS = 2_047;

/**
 * Maps framework domain records to the minimal provider SPI.
 *
 * Owner lease and MeshNode descriptor publication are persisted through the
 * provider. Remaining domain families still use the inherited framework-owned
 * repository while their record codecs are migrated.
 */
export class ZLinkLocationStoreRepository extends ZLinkInMemoryLocationStore {
  private readonly provider: ZLinkLocationStore;
  private readonly aggregateInventory: ZLinkAggregateInventoryStore;

  constructor(
    provider: ZLinkLocationStore,
    private readonly nowProvider: () => Date = () => new Date()
  ) {
    super(nowProvider);
    this.provider = new AmbiguousWriteReconcilingLocationStore(provider);
    this.aggregateInventory = new ZLinkAggregateInventoryStore(this.provider);
  }

  override async readAuthority(
    key: ZLinkAuthorityKey,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityReadResult> {
    const result = await this.provider.read(authorityKey(key.value), signal);
    if (result.kind === 'missing') {
      return { kind: 'missing', storeNow: result.storeNow };
    }
    return await this.projectAuthority(
      decodeJson<AuthorityRecord>(result.value.bytes),
      result.value.version,
      result.value.storeNow,
      signal
    );
  }

  override async listAuthorities(
    prefix: string,
    cursor: ZLinkAuthorityScanCursor | undefined,
    limit: number,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityScanResult> {
    if (!Number.isInteger(limit) || limit < 1 || limit > 1_000) {
      throw new RangeError('Authority scan limit must be in 1..1000.');
    }
    const scan = await this.provider.scan({
      prefix: `${PREFIX}authority:${encodeURIComponent(prefix)}`,
      cursor: cursor === undefined
        ? undefined
        : ({ value: cursor.encoded } as ZLinkStoreScanCursor),
      limit
    }, signal);
    if (scan.kind === 'expired') return { kind: 'scanExpired' };
    const items = [];
    for (const item of scan.value.items) {
      const encodedKey = decodeURIComponent(
        item.key.value.slice(`${PREFIX}authority:`.length)
      );
      const snapshot = await this.projectAuthority(
        decodeJson<AuthorityRecord>(item.value.bytes),
        item.value.version,
        item.value.storeNow,
        signal
      );
      items.push({ key: authorityContractKey(encodedKey), snapshot });
    }
    return {
      kind: 'page',
      items,
      nextCursor: scan.value.nextCursor === undefined
        ? undefined
        : ({ encoded: scan.value.nextCursor.value } as ZLinkAuthorityScanCursor)
    };
  }

  override async compareExchangeAuthority(
    key: ZLinkAuthorityKey,
    expectedStoreVersion: ZLinkAuthorityStoreVersion,
    mutation: ZLinkAuthorityMutation,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityCompareExchangeResult> {
    if (mutation.kind !== 'delete') {
      validatePayloadSize(mutation.payload, 'Authority payload');
    }
    const rowKey = authorityKey(requireText(key.value, 'authority key'));
    for (;;) {
      signal?.throwIfAborted();
      const current = await this.provider.read(rowKey, signal);
      if (current.kind === 'missing') {
        return { kind: 'conflict', current: { kind: 'missing', storeNow: current.storeNow } };
      }
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      const snapshot = await this.projectAuthority(
        record,
        current.value.version,
        current.value.storeNow,
        signal
      );
      if (record.aggregate !== undefined) {
        return { kind: 'conflict', current: snapshot };
      }
      if (
        record.snapshot.allocation.state !== 'active'
        || (record.visibleStoreVersion ?? current.value.version.value)
          !== expectedStoreVersion.value
      ) {
        return { kind: 'conflict', current: snapshot };
      }
      if (mutation.kind === 'rebindOwnerLease') {
        if (
          mutation.expectedOwner.ownerId !== record.snapshot.ownerId
          || mutation.expectedOwner.leaseGeneration !== record.snapshot.ownerLeaseGeneration
          || mutation.targetOwner.ownerId !== record.snapshot.ownerId
          || mutation.targetOwner.leaseGeneration === record.snapshot.ownerLeaseGeneration
        ) {
          return { kind: 'conflict', current: snapshot };
        }
        const descriptorKey = meshKey(
          record.snapshot.allocation.descriptor.meshName,
          String(record.snapshot.allocation.descriptor.rid)
        );
        const [descriptorRead, targetLeaseRead] = await Promise.all([
          this.provider.read(descriptorKey, signal),
          this.provider.read(ownerKey(mutation.targetOwner.ownerId), signal)
        ]);
        if (liveTargetDescriptor(
          descriptorRead,
          targetLeaseRead,
          {
            meshName: record.snapshot.allocation.descriptor.meshName,
            nodeRid: record.snapshot.allocation.descriptor.rid,
            nodeLifecycleGeneration: record.snapshot.allocation.descriptorLifecycleGeneration,
            owner: mutation.targetOwner
          }
        ) === undefined) {
          return { kind: 'conflict', current: snapshot };
        }
        const nextRecord: AuthorityRecord = {
          ...record,
          visibleStoreVersion: undefined,
          snapshot: {
            ...record.snapshot,
            ownerLeaseGeneration: mutation.targetOwner.leaseGeneration,
            payload: Buffer.from(mutation.payload)
          }
        };
        const result = await this.provider.write({
          conditions: [
            { kind: 'version', key: rowKey, expected: current.value.version },
            versionCondition(descriptorKey, descriptorRead),
            versionCondition(ownerKey(mutation.targetOwner.ownerId), targetLeaseRead)
          ],
          mutations: [{ kind: 'put', key: rowKey, bytes: encodeJson(nextRecord) }]
        }, signal);
        if (result.kind === 'conflict') continue;
        const storeVersion = result.putVersions.find(entry =>
          entry.key.value === rowKey.value)?.version;
        if (storeVersion === undefined) {
          throw new Error('Authority lease rebind did not return a row version.');
        }
        const stored = authoritySnapshot(nextRecord.snapshot, storeVersion, result.storeNow);
        const { kind: _kind, ...withoutKind } = stored;
        return { kind: 'stored', ...withoutKind };
      }
      if (mutation.kind === 'put' && mutation.generationTransition === 'newOwner') {
        return await this.commitRelocationAuthority(
          rowKey,
          current,
          record,
          snapshot,
          mutation as NewOwnerAuthorityMutation,
          signal
        );
      }
      if (
        mutation.kind === 'restore'
        && (
          mutation.expectedOwner.ownerId !== record.snapshot.ownerId
          || mutation.expectedOwner.leaseGeneration
            !== record.snapshot.ownerLeaseGeneration
        )
      ) {
        return { kind: 'conflict', current: snapshot };
      }
      const leaseKey = ownerKey(record.snapshot.ownerId);
      const lease = await this.provider.read(leaseKey, signal);
      if (
        mutation.kind !== 'restore'
        && !sameLiveOwner(lease, record.snapshot)
      ) {
        return { kind: 'conflict', current: snapshot };
      }
      const capacityRowKey = capacityKey(
        record.snapshot.allocation.descriptor.meshName,
        String(record.snapshot.allocation.descriptor.rid)
      );
      const capacityRead = mutation.kind === 'delete'
        ? await this.provider.read(capacityRowKey, signal)
        : undefined;
      const nextRecord: AuthorityRecord | undefined = mutation.kind === 'delete'
        ? undefined
        : {
            ...record,
            visibleStoreVersion: undefined,
            snapshot: {
              ...record.snapshot,
              payload: Buffer.from(mutation.payload)
            }
          };
      const result = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: current.value.version },
          ...(mutation.kind === 'restore'
            ? []
            : [versionCondition(leaseKey, lease)]),
          ...(capacityRead === undefined
            ? []
            : [conditionFor(capacityRowKey, capacityRead)])
        ],
        mutations: mutation.kind === 'delete'
          ? [
              { kind: 'delete', key: rowKey },
              {
                kind: 'put',
                key: capacityRowKey,
                bytes: encodeJson({
                  active: subtractCapacity(
                    capacityRead?.kind === 'found'
                      ? decodeJson<CapacityRecord>(capacityRead.value.bytes).active
                      : emptyCapacityRecord().active,
                    record.snapshot.allocation.capacity
                  ),
                  pending: capacityRead?.kind === 'found'
                    ? decodeJson<CapacityRecord>(capacityRead.value.bytes).pending
                    : emptyCapacityRecord().pending
                } satisfies CapacityRecord)
              }
            ]
          : [{ kind: 'put', key: rowKey, bytes: encodeJson(nextRecord!) }]
      }, signal);
      if (result.kind === 'conflict') continue;
      const storeVersion = result.putVersions.find(entry =>
        entry.key.value === rowKey.value)?.version;
      if (mutation.kind === 'delete') {
        return {
          kind: 'deleted',
          storeVersion: {
            value: (storeVersion ?? current.value.version).value
          } as ZLinkAuthorityStoreVersion,
          storeNow: result.storeNow
        };
      }
      if (storeVersion === undefined) {
        throw new Error('Authority compare-exchange did not return a row version.');
      }
      const stored = authoritySnapshot(nextRecord!.snapshot, storeVersion, result.storeNow);
      const { kind: _kind, ...withoutKind } = stored;
      return { kind: 'stored', ...withoutKind };
    }
  }

  override async reserveRelocationCapacity(
    request: ZLinkRelocationCapacityReservationRequest,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityReserveResult> {
    requireText(request.reservationId, 'relocation reservation ID');
    requireText(request.stableType, 'stable type');
    const reservationKey = relocationCapacityKey(request.reservationId);
    const rowKey = authorityKey(request.authorityKey.value);
    const descriptorKey = meshKey(
      request.targetDescriptor.meshName,
      String(request.targetDescriptor.rid)
    );
    const leaseKey = ownerKey(request.targetOwner.ownerId);
    const capacityRowKey = capacityKey(
      request.targetDescriptor.meshName,
      String(request.targetDescriptor.rid)
    );
    for (;;) {
      signal?.throwIfAborted();
      const [existing, authority, descriptorRead, leaseRead, capacityRead] =
        await Promise.all([
          this.provider.read(reservationKey, signal),
          this.provider.read(rowKey, signal),
          this.provider.read(descriptorKey, signal),
          this.provider.read(leaseKey, signal),
          this.provider.read(capacityRowKey, signal)
        ]);
      if (existing.kind === 'found') {
        const stored = decodeJson<RelocationCapacityRecord>(existing.value.bytes);
        if (!sameRelocationRequest(stored.request, request)) {
          return {
            kind: 'conflict',
            current: authority.kind === 'missing'
              ? { kind: 'missing', storeNow: authority.storeNow }
              : authoritySnapshot(
                  decodeJson<AuthorityRecord>(authority.value.bytes).snapshot,
                  authority.value.version,
                  authority.value.storeNow
                )
          };
        }
        return stored.state === 'reserved'
          ? { kind: 'alreadyReserved', fence: relocationCapacityFence(request.reservationId) }
          : {
              kind: 'conflict',
              current: authority.kind === 'missing'
                ? { kind: 'missing', storeNow: authority.storeNow }
                : authoritySnapshot(
                    decodeJson<AuthorityRecord>(authority.value.bytes).snapshot,
                    authority.value.version,
                    authority.value.storeNow
                  )
            };
      }
      if (authority.kind === 'missing') {
        return { kind: 'conflict', current: { kind: 'missing', storeNow: authority.storeNow } };
      }
      const authorityRecord = decodeJson<AuthorityRecord>(authority.value.bytes);
      const current = authoritySnapshot(
        authorityRecord.snapshot,
        authority.value.version,
        authority.value.storeNow
      );
      if (!sameRelocationAuthority(request, authorityRecord.snapshot, authority.value.version)) {
        return { kind: 'conflict', current };
      }
      const descriptor = liveTargetDescriptor(descriptorRead, leaseRead, {
        meshName: request.targetDescriptor.meshName,
        nodeRid: request.targetDescriptor.rid,
        nodeLifecycleGeneration: request.targetNodeLifecycleGeneration,
        owner: request.targetOwner
      });
      if (descriptor === undefined) return { kind: 'targetUnavailable' };
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      if (!capacityAvailable(descriptor, request.capacity, capacity)) {
        return { kind: 'placementCapacityExhausted' };
      }
      const result = await this.provider.write({
        conditions: [
          { kind: 'missing', key: reservationKey },
          { kind: 'version', key: rowKey, expected: authority.value.version },
          versionCondition(descriptorKey, descriptorRead),
          versionCondition(leaseKey, leaseRead),
          conditionFor(capacityRowKey, capacityRead)
        ],
        mutations: [
          {
            kind: 'put',
            key: reservationKey,
            bytes: encodeJson({
              request,
              state: 'reserved'
            } satisfies RelocationCapacityRecord)
          },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: capacity.active,
              pending: addCapacity(capacity.pending, request.capacity)
            } satisfies CapacityRecord)
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      return {
        kind: 'reserved',
        fence: relocationCapacityFence(request.reservationId)
      };
    }
  }

  private async commitRelocationAuthority(
    rowKey: ZLinkStoreKey,
    current: Extract<ZLinkStoreReadResult, { readonly kind: 'found' }>,
    record: AuthorityRecord,
    snapshot: ZLinkAuthoritySnapshot,
    mutation: NewOwnerAuthorityMutation,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityCompareExchangeResult> {
    const targetOwner = mutation.targetOwner;
    const fence = mutation.relocationCapacityFence;
    if (targetOwner === undefined || fence === undefined) {
      throw new TypeError('A new owner authority mutation requires its target owner and capacity fence.');
    }
    const reservationKey = relocationCapacityKey(fence.value);
    const reservationRead = await this.provider.read(reservationKey, signal);
    if (reservationRead.kind === 'missing') {
      return { kind: 'conflict', current: snapshot };
    }
    const reservation = decodeJson<RelocationCapacityRecord>(reservationRead.value.bytes);
    const request = reservation.request;
    if (
      reservation.state !== 'reserved'
      || request.reservationId !== fence.value
      || authorityKey(request.authorityKey.value).value !== rowKey.value
      || request.targetOwner.ownerId !== targetOwner.ownerId
      || request.targetOwner.leaseGeneration !== targetOwner.leaseGeneration
      || !sameRelocationAuthority(request, record.snapshot, current.value.version, false)
    ) {
      return { kind: 'conflict', current: snapshot };
    }
    if (record.snapshot.authorityOwnerGeneration >= MAX_GENERATION) {
      return { kind: 'generationExhausted' };
    }
    const targetDescriptorKey = meshKey(
      request.targetDescriptor.meshName,
      String(request.targetDescriptor.rid)
    );
    const targetLeaseKey = ownerKey(targetOwner.ownerId);
    const sourceCapacityKey = capacityKey(
      record.snapshot.allocation.descriptor.meshName,
      String(record.snapshot.allocation.descriptor.rid)
    );
    const targetCapacityKey = capacityKey(
      request.targetDescriptor.meshName,
      String(request.targetDescriptor.rid)
    );
    const [targetDescriptorRead, targetLeaseRead, sourceCapacityRead, targetCapacityRead] =
      await Promise.all([
        this.provider.read(targetDescriptorKey, signal),
        this.provider.read(targetLeaseKey, signal),
        this.provider.read(sourceCapacityKey, signal),
        this.provider.read(targetCapacityKey, signal)
      ]);
    if (liveTargetDescriptor(targetDescriptorRead, targetLeaseRead, {
      meshName: request.targetDescriptor.meshName,
      nodeRid: request.targetDescriptor.rid,
      nodeLifecycleGeneration: request.targetNodeLifecycleGeneration,
      owner: targetOwner
    }) === undefined) {
      return { kind: 'conflict', current: snapshot };
    }
    if (sourceCapacityRead.kind === 'missing' || targetCapacityRead.kind === 'missing') {
      return { kind: 'conflict', current: snapshot };
    }
    const sourceCapacity = decodeJson<CapacityRecord>(sourceCapacityRead.value.bytes);
    const targetCapacity = decodeJson<CapacityRecord>(targetCapacityRead.value.bytes);
    const nextRecord: AuthorityRecord = {
      ...record,
      visibleStoreVersion: undefined,
      snapshot: {
        ...record.snapshot,
        payload: Buffer.from(mutation.payload),
        authorityOwnerGeneration: record.snapshot.authorityOwnerGeneration + 1n,
        ownerId: targetOwner.ownerId,
        ownerLeaseGeneration: targetOwner.leaseGeneration,
        allocation: relocationTargetAllocation(request)
      }
    };
    const sameCapacityRow = sourceCapacityKey.value === targetCapacityKey.value;
    const nextTargetCapacity: CapacityRecord = sameCapacityRow
      ? {
          active: targetCapacity.active,
          pending: subtractCapacity(targetCapacity.pending, request.capacity)
        }
      : {
          active: addCapacity(targetCapacity.active, request.capacity),
          pending: subtractCapacity(targetCapacity.pending, request.capacity)
        };
    const result = await this.provider.write({
      conditions: [
        { kind: 'version', key: rowKey, expected: current.value.version },
        { kind: 'version', key: reservationKey, expected: reservationRead.value.version },
        versionCondition(targetDescriptorKey, targetDescriptorRead),
        versionCondition(targetLeaseKey, targetLeaseRead),
        { kind: 'version', key: sourceCapacityKey, expected: sourceCapacityRead.value.version },
        ...(sameCapacityRow
          ? []
          : [{ kind: 'version' as const, key: targetCapacityKey, expected: targetCapacityRead.value.version }])
      ],
      mutations: [
        { kind: 'put', key: rowKey, bytes: encodeJson(nextRecord) },
        {
          kind: 'put',
          key: reservationKey,
          bytes: encodeJson({ ...reservation, state: 'committed' } satisfies RelocationCapacityRecord)
        },
        ...(sameCapacityRow
          ? [{
              kind: 'put' as const,
              key: targetCapacityKey,
              bytes: encodeJson(nextTargetCapacity)
            }]
          : [
              {
                kind: 'put' as const,
                key: sourceCapacityKey,
                bytes: encodeJson({
                  active: subtractCapacity(sourceCapacity.active, request.capacity),
                  pending: sourceCapacity.pending
                } satisfies CapacityRecord)
              },
              {
                kind: 'put' as const,
                key: targetCapacityKey,
                bytes: encodeJson(nextTargetCapacity)
              }
            ])
      ]
    }, signal);
    if (result.kind === 'conflict') {
      const latest = await this.readAuthority(request.authorityKey, signal);
      return { kind: 'conflict', current: latest };
    }
    const storeVersion = result.putVersions.find(entry =>
      entry.key.value === rowKey.value)?.version;
    if (storeVersion === undefined) {
      throw new Error('Relocation authority commit did not return an authority row version.');
    }
    const stored = authoritySnapshot(nextRecord.snapshot, storeVersion, result.storeNow);
    const { kind: _kind, ...withoutKind } = stored;
    return { kind: 'stored', ...withoutKind };
  }

  override async abortRelocationCapacity(
    fence: ZLinkRelocationCapacityFence,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityAbortResult> {
    const rowKey = relocationCapacityKey(requireText(fence.value, 'relocation capacity fence'));
    for (;;) {
      const current = await this.provider.read(rowKey, signal);
      if (current.kind === 'missing') return 'stale';
      const record = decodeJson<RelocationCapacityRecord>(current.value.bytes);
      if (record.state === 'aborted') return 'alreadyAborted';
      if (record.state === 'committed') return 'alreadyCommitted';
      if (record.state === 'prepared') return 'stale';
      const capacityRowKey = capacityKey(
        record.request.targetDescriptor.meshName,
        String(record.request.targetDescriptor.rid)
      );
      const capacityRead = await this.provider.read(capacityRowKey, signal);
      if (capacityRead.kind === 'missing') return 'stale';
      const capacity = decodeJson<CapacityRecord>(capacityRead.value.bytes);
      const result = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: current.value.version },
          { kind: 'version', key: capacityRowKey, expected: capacityRead.value.version }
        ],
        mutations: [
          {
            kind: 'put',
            key: rowKey,
            bytes: encodeJson({ ...record, state: 'aborted' } satisfies RelocationCapacityRecord)
          },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: capacity.active,
              pending: subtractCapacity(capacity.pending, record.request.capacity)
            } satisfies CapacityRecord)
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      return 'aborted';
    }
  }

  override async prepareAggregate(
    request: ZLinkAggregatePrepareRequest,
    signal?: AbortSignal
  ): Promise<ZLinkAggregatePrepareResult> {
    validateProviderAggregateRequest(request);
    const fence = aggregateFence(request.aggregateId, request.aggregateGeneration);
    const rowKey = aggregateKey(fence);
    const fingerprint = sha256Hex(encodeJson(request));
    let aggregateRead = await this.provider.read(rowKey, signal);
    if (aggregateRead.kind === 'found') {
      const existing = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
      const reconciled = reconcileAggregatePrepare(existing, fingerprint, fence);
      if (reconciled !== undefined) {
        if (reconciled.kind === 'alreadyPrepared') {
          await this.aggregateInventory.read(fence, request.inventoryDigest, signal);
        }
        return reconciled;
      }
    } else {
      const staging = aggregateRecord(request, fingerprint, 'staging');
      const claimed = await this.provider.write({
        conditions: [{ kind: 'missing', key: rowKey }],
        mutations: [{ kind: 'put', key: rowKey, bytes: encodeJson(staging) }]
      }, signal);
      if (claimed.kind === 'conflict') {
        aggregateRead = await this.provider.read(rowKey, signal);
        if (aggregateRead.kind === 'missing') return { kind: 'conflict' };
        const existing = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
        const reconciled = reconcileAggregatePrepare(existing, fingerprint, fence);
        if (reconciled !== undefined) return reconciled;
      } else {
        const version = claimed.putVersions.find(value =>
          value.key.value === rowKey.value)?.version;
        if (version === undefined) {
          throw new Error('Aggregate staging did not return a Store version.');
        }
        aggregateRead = {
          kind: 'found',
          value: {
            bytes: encodeJson(staging),
            version,
            storeNow: claimed.storeNow
          }
        };
      }
    }
    const aggregate = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
    if (
      aggregate.state !== 'staging'
      || aggregate.requestFingerprint !== fingerprint
    ) {
      return { kind: 'conflict' };
    }
    try {
      await this.aggregateInventory.store(request, signal);
      const entries = await this.aggregateInventory.read(
        fence,
        request.inventoryDigest,
        signal
      );
      if (entries.length !== request.participants.length) {
        throw new Error('Aggregate inventory count changed after staging.');
      }
      await parallelForEach(request.participants, 64, async (participant, index) => {
        const entry = entries[index]!;
        if (
          entry.authorityKey !== participant.authorityKey.value
          || entry.expectedStoreVersion !== participant.expectedStoreVersion.value
          || entry.ownerTransition !== participant.ownerTransition
        ) {
          throw new Error('Aggregate inventory differs from its participant request.');
        }
        await this.putImmutable(
          aggregateParticipantPayloadKey(fence, index),
          participant.authorityPayload,
          signal
        );
        await this.putImmutable(
          aggregateParticipantMembershipKey(fence, index),
          participant.membershipMutation,
          signal
        );
      });

      const targetDescriptorKey = meshKey(
        request.targetDescriptor.meshName,
        String(request.targetDescriptor.rid)
      );
      const targetLeaseKey = ownerKey(request.targetOwner.ownerId);
      const targetCapacityKey = capacityKey(
        request.targetDescriptor.meshName,
        String(request.targetDescriptor.rid)
      );
      const capacityReservationKey = relocationCapacityKey(
        request.aggregateId.value
      );
      const [
        descriptorRead,
        leaseRead,
        targetCapacityRead,
        capacityReservationRead
      ] = await Promise.all([
        this.provider.read(targetDescriptorKey, signal),
        this.provider.read(targetLeaseKey, signal),
        this.provider.read(targetCapacityKey, signal),
        this.provider.read(capacityReservationKey, signal)
      ]);
      const descriptor = liveTargetDescriptor(
        descriptorRead,
        leaseRead,
        aggregateTarget(request)
      );
      const targetCapacity = targetCapacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(targetCapacityRead.value.bytes);
      const capacityReservation = capacityReservationRead.kind === 'found'
        ? decodeJson<RelocationCapacityRecord>(capacityReservationRead.value.bytes)
        : undefined;
      const capacityReservationVersion = capacityReservationRead.kind === 'found'
        ? capacityReservationRead.value.version
        : undefined;
      const adoptsCapacityReservation = capacityReservation !== undefined
        && capacityReservationVersion !== undefined
        && capacityReservation.state === 'reserved'
        && sameAggregateCapacityReservation(capacityReservation.request, request);
      if (
        descriptor === undefined
        || !adoptsCapacityReservation
          && !capacityAvailable(descriptor, request.capacity, targetCapacity)
      ) {
        await this.abortAggregateStaging(fence, signal);
        return { kind: 'conflict' };
      }

      let sourceCapacity: ZLinkCapacityVector = { actors: 0, spots: 0 };
      const installed: Array<{
        readonly key: ZLinkStoreKey;
        readonly expectedStoreVersion: string;
      }> = [];
      for (let index = 0; index < request.participants.length; index++) {
        const participant = request.participants[index]!;
        const entry = entries[index]!;
        const authorityRowKey = authorityKey(participant.authorityKey.value);
        const current = await this.provider.read(authorityRowKey, signal);
        if (current.kind === 'missing') {
          await this.abortAggregateStaging(fence, signal);
          return { kind: 'conflict' };
        }
        const record = decodeJson<AuthorityRecord>(current.value.bytes);
        if (sameAggregateMarker(record.aggregate, fence, index, participant)) {
          installed.push({
            key: authorityRowKey,
            expectedStoreVersion: participant.expectedStoreVersion.value
          });
          if (participant.ownerTransition === 'newOwner') {
            sourceCapacity = addCapacityVector(
              sourceCapacity,
              record.snapshot.allocation.capacity
            );
          }
          continue;
        }
        if (
          record.aggregate !== undefined
          || record.snapshot.allocation.state !== 'active'
          || (record.visibleStoreVersion ?? current.value.version.value)
            !== participant.expectedStoreVersion.value
        ) {
          await this.clearAggregateMarkers(fence, installed, signal);
          await this.abortAggregateStaging(fence, signal);
          return { kind: 'conflict' };
        }
        if (participant.ownerTransition === 'preserve') {
          const lease = await this.provider.read(ownerKey(record.snapshot.ownerId), signal);
          if (!sameLiveOwner(lease, record.snapshot)) {
            await this.clearAggregateMarkers(fence, installed, signal);
            await this.abortAggregateStaging(fence, signal);
            return { kind: 'conflict' };
          }
        } else {
          sourceCapacity = addCapacityVector(
            sourceCapacity,
            record.snapshot.allocation.capacity
          );
        }
        if (record.snapshot.authorityOwnerGeneration >= MAX_GENERATION) {
          await this.clearAggregateMarkers(fence, installed, signal);
          await this.abortAggregateStaging(fence, signal);
          return { kind: 'generationExhausted' };
        }
        const marker: AggregateParticipantFenceRecord = {
          aggregateId: fence.aggregateId.value,
          aggregateGeneration: fence.aggregateGeneration,
          index,
          expectedStoreVersion: participant.expectedStoreVersion.value,
          ownerTransition: participant.ownerTransition,
          targetAuthorityOwnerGeneration:
            participant.ownerTransition === 'newOwner'
              ? record.snapshot.authorityOwnerGeneration + 1n
              : record.snapshot.authorityOwnerGeneration,
          authorityPayloadSha256: entry.authorityPayloadSha256,
          membershipMutationSha256: entry.membershipMutationSha256
        };
        const stored = await this.provider.write({
          conditions: [{
            kind: 'version',
            key: authorityRowKey,
            expected: current.value.version
          }],
          mutations: [{
            kind: 'put',
            key: authorityRowKey,
            bytes: encodeJson({
              ...record,
              aggregate: marker,
              visibleStoreVersion:
                record.visibleStoreVersion ?? current.value.version.value
            } satisfies AuthorityRecord)
          }]
        }, signal);
        if (stored.kind === 'conflict') {
          const raced = await this.provider.read(authorityRowKey, signal);
          if (raced.kind === 'found') {
            const racedRecord = decodeJson<AuthorityRecord>(raced.value.bytes);
            if (sameAggregateMarker(
              racedRecord.aggregate,
              fence,
              index,
              participant
            )) {
              installed.push({
                key: authorityRowKey,
                expectedStoreVersion: participant.expectedStoreVersion.value
              });
              continue;
            }
          }
          await this.clearAggregateMarkers(fence, installed, signal);
          await this.abortAggregateStaging(fence, signal);
          return { kind: 'conflict' };
        }
        installed.push({
          key: authorityRowKey,
          expectedStoreVersion: participant.expectedStoreVersion.value
        });
      }
      if (!sameCapacityVector(sourceCapacity, request.capacity)) {
        await this.clearAggregateMarkers(fence, installed, signal);
        await this.abortAggregateStaging(fence, signal);
        return { kind: 'conflict' };
      }

      aggregateRead = await this.provider.read(rowKey, signal);
      if (aggregateRead.kind === 'missing') return { kind: 'conflict' };
      const latest = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
      if (latest.state === 'prepared' && latest.requestFingerprint === fingerprint) {
        return { kind: 'alreadyPrepared', fence };
      }
      if (latest.state !== 'staging' || latest.requestFingerprint !== fingerprint) {
        return { kind: 'conflict' };
      }
      const reserved = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: aggregateRead.value.version },
          versionCondition(targetDescriptorKey, descriptorRead),
          versionCondition(targetLeaseKey, leaseRead),
          conditionFor(targetCapacityKey, targetCapacityRead),
          ...(adoptsCapacityReservation
            ? [{
                kind: 'version' as const,
                key: capacityReservationKey,
                expected: capacityReservationVersion!
              }]
            : [])
        ],
        mutations: [
          {
            kind: 'put',
            key: rowKey,
            bytes: encodeJson({
              ...latest,
              state: 'prepared',
              ...(adoptsCapacityReservation
                ? { capacityReservationId: request.aggregateId.value }
                : {})
            } satisfies AggregateRecord)
          },
          {
            kind: 'put',
            key: targetCapacityKey,
            bytes: encodeJson({
              active: targetCapacity.active,
              pending: adoptsCapacityReservation
                ? targetCapacity.pending
                : addCapacity(targetCapacity.pending, request.capacity)
            } satisfies CapacityRecord)
          },
          ...(adoptsCapacityReservation
            ? [{
                kind: 'put' as const,
                key: capacityReservationKey,
                bytes: encodeJson({
                  ...capacityReservation,
                  state: 'prepared'
                } satisfies RelocationCapacityRecord)
              }]
            : [])
        ]
      }, signal);
      if (reserved.kind === 'conflict') {
        const raced = await this.provider.read(rowKey, signal);
        if (raced.kind === 'found') {
          const record = decodeJson<AggregateRecord>(raced.value.bytes);
          if (record.state === 'prepared' && record.requestFingerprint === fingerprint) {
            return { kind: 'alreadyPrepared', fence };
          }
        }
        await this.clearAggregateMarkers(fence, installed, signal);
        return { kind: 'conflict' };
      }
      return { kind: 'prepared', fence };
    } catch (error) {
      await this.abortAggregateStaging(fence, signal);
      throw error;
    }
  }

  override async commitAggregate(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<ZLinkAggregateCommitResult> {
    validateAggregateFence(fence);
    const rowKey = aggregateKey(fence);
    let aggregateRead = await this.provider.read(rowKey, signal);
    if (aggregateRead.kind === 'missing') return { kind: 'stale' };
    let aggregate = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
    if (aggregate.state === 'aborted' || aggregate.state === 'staging') {
      return { kind: 'stale' };
    }
    if (aggregate.state === 'committed') {
      await this.normalizeCommittedAggregate(fence, aggregate, signal);
      return { kind: 'alreadyCommitted' };
    }
    const entries = await this.aggregateInventory.read(
      fence,
      Buffer.from(aggregate.inventoryDigest),
      signal
    );
    if (entries.length !== aggregate.participantCount) {
      throw new Error('Aggregate inventory count differs from its authority record.');
    }
    const authorityRows = [];
    for (let index = 0; index < entries.length; index++) {
      const entry = entries[index]!;
      const rowKeyForParticipant = authorityKey(entry.authorityKey);
      const current = await this.provider.read(rowKeyForParticipant, signal);
      if (current.kind === 'missing') return { kind: 'stale' };
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (!sameAggregateMarkerEntry(record.aggregate, fence, index, entry)) {
        return { kind: 'stale' };
      }
      const [payload, membership] = await Promise.all([
        requireProviderBytes(
          this.provider,
          aggregateParticipantPayloadKey(fence, index),
          signal
        ),
        requireProviderBytes(
          this.provider,
          aggregateParticipantMembershipKey(fence, index),
          signal
        )
      ]);
      if (
        sha256Hex(payload) !== entry.authorityPayloadSha256
        || sha256Hex(membership) !== entry.membershipMutationSha256
      ) {
        throw new Error('Aggregate participant staging checksum does not match inventory.');
      }
      authorityRows.push({ key: rowKeyForParticipant, current, record, entry });
    }

    const targetDescriptorKey = meshKey(
      aggregate.targetDescriptor.meshName,
      String(aggregate.targetDescriptor.rid)
    );
    const targetLeaseKey = ownerKey(aggregate.targetOwner.ownerId);
    const targetCapacityKey = capacityKey(
      aggregate.targetDescriptor.meshName,
      String(aggregate.targetDescriptor.rid)
    );
    const [descriptorRead, leaseRead] = await Promise.all([
      this.provider.read(targetDescriptorKey, signal),
      this.provider.read(targetLeaseKey, signal)
    ]);
    if (
      liveTargetDescriptor(
        descriptorRead,
        leaseRead,
        aggregateTargetFromRecord(aggregate)
      ) === undefined
    ) {
      return { kind: 'stale' };
    }

    const capacityDeltas = new Map<string, ZLinkCapacityVector>();
    for (const row of authorityRows) {
      if (row.entry.ownerTransition !== 'newOwner') continue;
      const key = capacityKey(
        row.record.snapshot.allocation.descriptor.meshName,
        String(row.record.snapshot.allocation.descriptor.rid)
      ).value;
      capacityDeltas.set(
        key,
        addCapacityVector(
          capacityDeltas.get(key) ?? { actors: 0, spots: 0 },
          row.record.snapshot.allocation.capacity
        )
      );
    }
    const capacityKeys = new Map<string, ZLinkStoreKey>();
    capacityKeys.set(targetCapacityKey.value, targetCapacityKey);
    for (const value of capacityDeltas.keys()) capacityKeys.set(value, storeKey(value));
    const capacityReads = new Map<string, Extract<ZLinkStoreReadResult, { kind: 'found' }>>();
    for (const [value, key] of capacityKeys) {
      const read = await this.provider.read(key, signal);
      if (read.kind === 'missing') return { kind: 'stale' };
      capacityReads.set(value, read);
    }
    const capacityMutations = [];
    for (const [value, key] of capacityKeys) {
      const read = capacityReads.get(value)!;
      let capacity = decodeJson<CapacityRecord>(read.value.bytes);
      const sourceDelta = capacityDeltas.get(value);
      if (sourceDelta !== undefined) {
        capacity = {
          active: subtractCapacity(capacity.active, sourceDelta),
          pending: capacity.pending
        };
      }
      if (value === targetCapacityKey.value) {
        capacity = {
          active: addCapacity(capacity.active, aggregate.capacity),
          pending: subtractCapacity(capacity.pending, aggregate.capacity)
        };
      }
      capacityMutations.push({
        kind: 'put' as const,
        key,
        bytes: encodeJson(capacity)
      });
    }
    const capacityReservationKey = aggregate.capacityReservationId === undefined
      ? undefined
      : relocationCapacityKey(aggregate.capacityReservationId);
    const capacityReservationRead = capacityReservationKey === undefined
      ? undefined
      : await this.provider.read(capacityReservationKey, signal);
    const capacityReservation = capacityReservationRead?.kind === 'found'
      ? decodeJson<RelocationCapacityRecord>(capacityReservationRead.value.bytes)
      : undefined;
    if (capacityReservationKey !== undefined
      && (capacityReservation === undefined
        || capacityReservation.state !== 'prepared')) {
      return { kind: 'stale' };
    }
    const published = await this.provider.write({
      conditions: [
        { kind: 'version', key: rowKey, expected: aggregateRead.value.version },
        versionCondition(targetDescriptorKey, descriptorRead),
        versionCondition(targetLeaseKey, leaseRead),
        ...(capacityReservationKey !== undefined
          && capacityReservationRead?.kind === 'found'
          ? [{
              kind: 'version' as const,
              key: capacityReservationKey,
              expected: capacityReservationRead.value.version
            }]
          : []),
        ...[...capacityReads.entries()].map(([value, read]) => ({
          kind: 'version' as const,
          key: capacityKeys.get(value)!,
          expected: read.value.version
        }))
      ],
      mutations: [
        {
          kind: 'put',
          key: rowKey,
          bytes: encodeJson({ ...aggregate, state: 'committed' } satisfies AggregateRecord)
        },
        ...(capacityReservationKey !== undefined && capacityReservation !== undefined
          ? [{
              kind: 'put' as const,
              key: capacityReservationKey,
              bytes: encodeJson({
                ...capacityReservation,
                state: 'committed'
              } satisfies RelocationCapacityRecord)
            }]
          : []),
        ...capacityMutations
      ]
    }, signal);
    if (published.kind === 'conflict') {
      aggregateRead = await this.provider.read(rowKey, signal);
      if (aggregateRead.kind === 'missing') return { kind: 'stale' };
      aggregate = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
      if (aggregate.state !== 'committed') return { kind: 'stale' };
    } else {
      aggregate = { ...aggregate, state: 'committed' };
    }
    await this.normalizeCommittedAggregate(fence, aggregate, signal);
    return { kind: 'committed' };
  }

  override async abortAggregate(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<ZLinkAggregateAbortResult> {
    validateAggregateFence(fence);
    const rowKey = aggregateKey(fence);
    for (;;) {
      const current = await this.provider.read(rowKey, signal);
      if (current.kind === 'missing') return { kind: 'stale' };
      const aggregate = decodeJson<AggregateRecord>(current.value.bytes);
      if (aggregate.state === 'aborted') {
        await this.clearAggregateMarkersByScan(fence, signal);
        return { kind: 'alreadyAborted' };
      }
      if (aggregate.state === 'committed') return { kind: 'stale' };
      const conditions: ZLinkStoreCondition[] = [{
        kind: 'version',
        key: rowKey,
        expected: current.value.version
      }];
      const mutations: ZLinkStoreWriteRequest['mutations'][number][] = [{
        kind: 'put',
        key: rowKey,
        bytes: encodeJson({ ...aggregate, state: 'aborted' } satisfies AggregateRecord)
      }];
      if (aggregate.capacityReservationId !== undefined) {
        const capacityReservationKey = relocationCapacityKey(
          aggregate.capacityReservationId
        );
        const capacityReservationRead = await this.provider.read(
          capacityReservationKey,
          signal
        );
        if (capacityReservationRead.kind === 'missing') return { kind: 'stale' };
        const capacityReservation = decodeJson<RelocationCapacityRecord>(
          capacityReservationRead.value.bytes
        );
        if (capacityReservation.state !== 'prepared') return { kind: 'stale' };
        conditions.push({
          kind: 'version',
          key: capacityReservationKey,
          expected: capacityReservationRead.value.version
        });
        mutations.push({
          kind: 'put',
          key: capacityReservationKey,
          bytes: encodeJson({
            ...capacityReservation,
            state: 'aborted'
          } satisfies RelocationCapacityRecord)
        });
      }
      if (aggregate.state === 'prepared') {
        const targetCapacityKey = capacityKey(
          aggregate.targetDescriptor.meshName,
          String(aggregate.targetDescriptor.rid)
        );
        const targetCapacityRead = await this.provider.read(targetCapacityKey, signal);
        if (targetCapacityRead.kind === 'missing') return { kind: 'stale' };
        const capacity = decodeJson<CapacityRecord>(targetCapacityRead.value.bytes);
        conditions.push({
          kind: 'version',
          key: targetCapacityKey,
          expected: targetCapacityRead.value.version
        });
        mutations.push({
          kind: 'put',
          key: targetCapacityKey,
          bytes: encodeJson({
            active: capacity.active,
            pending: subtractCapacity(capacity.pending, aggregate.capacity)
          } satisfies CapacityRecord)
        });
      }
      const result = await this.provider.write({ conditions, mutations }, signal);
      if (result.kind === 'conflict') continue;
      await this.clearAggregateMarkersByScan(fence, signal);
      return { kind: 'aborted' };
    }
  }

  override async reserve(
    request: ZLinkObjectReserveRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectReserveResult> {
    const encodedAuthorityKey = encodeAuthorityKey(
      request.key.kind,
      request.key.globalId
    );
    for (;;) {
      signal?.throwIfAborted();
      const rowKey = authorityKey(encodedAuthorityKey.value);
      const descriptorKey = meshKey(request.target.meshName, String(request.target.nodeRid));
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(
        request.target.meshName,
        String(request.target.nodeRid)
      );
      const generationRowKey = authorityGenerationKey(encodedAuthorityKey.value);
      const [current, descriptorRead, leaseRead, capacityRead, generationRead] =
        await Promise.all([
          this.provider.read(rowKey, signal),
          this.provider.read(descriptorKey, signal),
          this.provider.read(leaseKey, signal),
          this.provider.read(capacityRowKey, signal),
          this.provider.read(generationRowKey, signal)
        ]);
      if (current.kind === 'found') {
        const record = decodeJson<AuthorityRecord>(current.value.bytes);
        const snapshot = authoritySnapshot(
          record.snapshot,
          current.value.version,
          current.value.storeNow
        );
        if (
          snapshot.allocation.objectKind !== request.key.kind
          || snapshot.allocation.stableType !== request.intent.stableType
        ) {
          return { kind: 'typeMismatch', current: snapshot };
        }
        return snapshot.allocation.state === 'active'
          ? { kind: 'alreadyExists', current: snapshot }
          : { kind: 'conflict', current: snapshot };
      }
      const descriptor = liveTargetDescriptor(
        descriptorRead,
        leaseRead,
        request.target
      );
      if (descriptor === undefined) {
        return { kind: 'conflict', current: { kind: 'missing', storeNow: current.storeNow } };
      }
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      if (!capacityAvailable(descriptor, request.capacity, capacity)) {
        return { kind: 'placementCapacityExhausted' };
      }
      const generation = generationRead.kind === 'missing'
        ? 1n
        : BigInt(decodeText(generationRead.value.bytes)) + 1n;
      if (generation > MAX_GENERATION) return { kind: 'generationExhausted' };
      const reservationId = randomUUID();
      const allocation: ZLinkPlacementAllocation = {
        state: 'reserved',
        objectKind: request.key.kind,
        stableType: request.intent.stableType,
        descriptor: {
          meshName: request.target.meshName,
          rid: request.target.nodeRid
        },
        descriptorLifecycleGeneration: request.target.nodeLifecycleGeneration,
        capacity: cloneCapacity(request.capacity)
      };
      const record: AuthorityRecord = {
        reservationId,
        snapshot: {
          payload: Buffer.from(request.creatingPayload),
          objectGeneration: generation,
          authorityOwnerGeneration: 1n,
          ownerId: request.target.owner.ownerId,
          ownerLeaseGeneration: request.target.owner.leaseGeneration,
          allocation,
          pendingCreation: {
            reservationId,
            requestContentReference: request.intent.requestContentReference,
            requestSha256: Buffer.from(request.intent.requestSha256),
            requestEncodedSize: request.intent.requestEncodedSize
          }
        }
      };
      const result = await this.provider.write({
        conditions: [
          { kind: 'missing', key: rowKey },
          versionCondition(descriptorKey, descriptorRead),
          versionCondition(leaseKey, leaseRead),
          conditionFor(capacityRowKey, capacityRead),
          conditionFor(generationRowKey, generationRead)
        ],
        mutations: [
          { kind: 'put', key: rowKey, bytes: encodeJson(record) },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: capacity.active,
              pending: addCapacity(capacity.pending, request.capacity)
            } satisfies CapacityRecord)
          },
          { kind: 'put', key: generationRowKey, bytes: encodeText(String(generation)) }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      const version = result.putVersions.find(entry =>
        entry.key.value === rowKey.value)?.version;
      if (version === undefined) throw new Error('Authority reserve did not return a row version.');
      return {
        kind: 'reserved',
        reservationId,
        creating: authoritySnapshot(record.snapshot, version, result.storeNow)
      };
    }
  }

  override async commit(
    request: ZLinkObjectCommitRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectCommitResult> {
    const key = encodeAuthorityKey(request.key.kind, request.key.globalId);
    const rowKey = authorityKey(key.value);
    for (;;) {
      signal?.throwIfAborted();
      const current = await this.provider.read(rowKey, signal);
      if (current.kind === 'missing') return { kind: 'stale' };
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (
        record.terminal === 'committed'
        && record.reservationId === request.reservationId
        && record.snapshot.allocation.state === 'active'
      ) {
        return {
          kind: 'alreadyCommitted',
          ready: authoritySnapshot(record.snapshot, current.value.version, current.value.storeNow)
        };
      }
      if (
        record.reservationId !== request.reservationId
        || current.value.version.value !== request.expectedStoreVersion
        || record.snapshot.allocation.state !== 'reserved'
        || !sameCreationTarget(record.snapshot, request.target)
      ) {
        return { kind: 'stale' };
      }
      const descriptorKey = meshKey(request.target.meshName, String(request.target.nodeRid));
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(request.target.meshName, String(request.target.nodeRid));
      const [descriptorRead, leaseRead, capacityRead] = await Promise.all([
        this.provider.read(descriptorKey, signal),
        this.provider.read(leaseKey, signal),
        this.provider.read(capacityRowKey, signal)
      ]);
      if (liveTargetDescriptor(descriptorRead, leaseRead, request.target) === undefined) {
        return { kind: 'stale' };
      }
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      const ready: AuthorityRecord = {
        reservationId: request.reservationId,
        terminal: 'committed',
        snapshot: {
          ...record.snapshot,
          payload: Buffer.from(request.readyPayload),
          allocation: { ...record.snapshot.allocation, state: 'active' },
          pendingCreation: undefined
        }
      };
      const result = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: current.value.version },
          versionCondition(descriptorKey, descriptorRead),
          versionCondition(leaseKey, leaseRead),
          conditionFor(capacityRowKey, capacityRead)
        ],
        mutations: [
          { kind: 'put', key: rowKey, bytes: encodeJson(ready) },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: addCapacity(capacity.active, record.snapshot.allocation.capacity),
              pending: subtractCapacity(capacity.pending, record.snapshot.allocation.capacity)
            } satisfies CapacityRecord)
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      const version = result.putVersions.find(entry =>
        entry.key.value === rowKey.value)?.version;
      if (version === undefined) throw new Error('Authority commit did not return a row version.');
      return {
        kind: 'committed',
        ready: authoritySnapshot(ready.snapshot, version, result.storeNow)
      };
    }
  }

  override async abort(
    request: ZLinkObjectAbortRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectAbortResult> {
    const key = encodeAuthorityKey(request.key.kind, request.key.globalId);
    const rowKey = authorityKey(key.value);
    for (;;) {
      signal?.throwIfAborted();
      const current = await this.provider.read(rowKey, signal);
      if (current.kind === 'missing') return { kind: 'stale' };
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (
        record.reservationId !== request.reservationId
        || current.value.version.value !== request.expectedStoreVersion
        || record.snapshot.allocation.state !== 'reserved'
        || !sameCreationTarget(record.snapshot, request.target)
      ) {
        return { kind: 'stale' };
      }
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(
        request.target.meshName,
        String(request.target.nodeRid)
      );
      const [leaseRead, capacityRead] = await Promise.all([
        this.provider.read(leaseKey, signal),
        this.provider.read(capacityRowKey, signal)
      ]);
      if (!sameLiveOwner(leaseRead, record.snapshot)) return { kind: 'stale' };
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      const result = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: current.value.version },
          versionCondition(leaseKey, leaseRead),
          conditionFor(capacityRowKey, capacityRead)
        ],
        mutations: [
          { kind: 'delete', key: rowKey },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: capacity.active,
              pending: subtractCapacity(
                capacity.pending,
                record.snapshot.allocation.capacity
              )
            } satisfies CapacityRecord)
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      return { kind: 'aborted' };
    }
  }

  override async completeCreation(
    request: ZLinkObjectCreationCompleteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectCreationCompleteResult> {
    if (request.key.kind !== 'actor') {
      throw new TypeError('completeCreation is reserved for Actor creation.');
    }
    const terminal = createTerminalRecord(
      request,
      this.nowProvider()
    );
    const rowKey = authorityKey(
      encodeAuthorityKey(request.key.kind, request.key.globalId).value
    );
    const terminalRowKey = creationTerminalKey(terminal.operation);
    for (;;) {
      signal?.throwIfAborted();
      const [current, existingTerminal] = await Promise.all([
        this.provider.read(rowKey, signal),
        this.provider.read(terminalRowKey, signal)
      ]);
      if (existingTerminal.kind === 'found') {
        return {
          kind: 'alreadyCompleted',
          terminal: reviveCreationTerminal(
            decodeJson<ZLinkCreationTerminalRecord>(existingTerminal.value.bytes)
          )
        };
      }
      if (current.kind === 'missing') return { kind: 'stale' };
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (
        record.reservationId !== request.reservationId
        || current.value.version.value !== request.expectedStoreVersion
        || record.snapshot.allocation.state !== 'reserved'
        || !sameCreationTarget(record.snapshot, request.target)
      ) {
        return { kind: 'stale' };
      }
      const descriptorKey = meshKey(request.target.meshName, String(request.target.nodeRid));
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(request.target.meshName, String(request.target.nodeRid));
      const [descriptorRead, leaseRead, capacityRead] = await Promise.all([
        this.provider.read(descriptorKey, signal),
        this.provider.read(leaseKey, signal),
        this.provider.read(capacityRowKey, signal)
      ]);
      if (liveTargetDescriptor(descriptorRead, leaseRead, request.target) === undefined) {
        return { kind: 'stale' };
      }
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      const terminalAtStore = {
        ...terminal,
        storeNow: current.value.storeNow
      };
      const retentionMs = terminalAtStore.expiresAt.getTime()
        - current.value.storeNow.getTime();
      if (retentionMs <= 0) {
        throw new RangeError(
          'Creation terminal expiry must be the live operation deadline plus five minutes.'
        );
      }
      const mutations = request.completion.kind === 'created'
        ? [
            {
              kind: 'put' as const,
              key: rowKey,
              bytes: encodeJson({
                reservationId: request.reservationId,
                terminal: 'committed',
                snapshot: {
                  ...record.snapshot,
                  payload: Buffer.from(request.completion.readyPayload),
                  allocation: { ...record.snapshot.allocation, state: 'active' },
                  pendingCreation: undefined
                }
              } satisfies AuthorityRecord)
            },
            {
              kind: 'put' as const,
              key: capacityRowKey,
              bytes: encodeJson({
                active: addCapacity(capacity.active, record.snapshot.allocation.capacity),
                pending: subtractCapacity(capacity.pending, record.snapshot.allocation.capacity)
              } satisfies CapacityRecord)
            }
          ]
        : [
            { kind: 'delete' as const, key: rowKey },
            {
              kind: 'put' as const,
              key: capacityRowKey,
              bytes: encodeJson({
                active: capacity.active,
                pending: subtractCapacity(capacity.pending, record.snapshot.allocation.capacity)
              } satisfies CapacityRecord)
            }
          ];
      const result = await this.provider.write({
        conditions: [
          { kind: 'version', key: rowKey, expected: current.value.version },
          { kind: 'missing', key: terminalRowKey },
          versionCondition(descriptorKey, descriptorRead),
          versionCondition(leaseKey, leaseRead),
          conditionFor(capacityRowKey, capacityRead)
        ],
        mutations: [
          ...mutations,
          {
            kind: 'put',
            key: terminalRowKey,
            bytes: encodeJson(terminalAtStore),
            retentionMs
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      const publishedTerminal = {
        ...terminalAtStore,
        storeNow: result.storeNow
      };
      if (request.completion.kind !== 'created') {
        return { kind: request.completion.kind, terminal: publishedTerminal };
      }
      const version = result.putVersions.find(entry =>
        entry.key.value === rowKey.value)?.version;
      if (version === undefined) {
        throw new Error('Actor creation completion did not return an authority row version.');
      }
      const readyRecord = decodeJson<AuthorityRecord>(
        encodeJson({
          reservationId: request.reservationId,
          terminal: 'committed',
          snapshot: {
            ...record.snapshot,
            payload: Buffer.from(request.completion.readyPayload),
            allocation: { ...record.snapshot.allocation, state: 'active' },
            pendingCreation: undefined
          }
        } satisfies AuthorityRecord)
      );
      return {
        kind: 'created',
        ready: authoritySnapshot(readyRecord.snapshot, version, result.storeNow),
        terminal: publishedTerminal
      };
    }
  }

  override async readCreationTerminal(
    operation: ZLinkCreationOperationIdentity,
    signal?: AbortSignal
  ): Promise<ZLinkCreationTerminalReadResult> {
    validateCreationOperation(operation);
    const result = await this.provider.read(creationTerminalKey(operation), signal);
    return result.kind === 'missing'
      ? { kind: 'missing', storeNow: result.storeNow }
      : {
          kind: 'found',
          record: reviveCreationTerminal(
            decodeJson<ZLinkCreationTerminalRecord>(result.value.bytes)
          )
        };
  }

  override async claimOwnerLease(
    ownerId: string,
    leaseTtlMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseClaimResult> {
    requireOwnerInput(ownerId, leaseTtlMs);
    for (;;) {
      const [owner, counter] = await Promise.all([
        this.provider.read(ownerKey(ownerId), signal),
        this.provider.read(OWNER_COUNTER_KEY, signal)
      ]);
      if (owner.kind === 'found') return { kind: 'conflict' };
      const generation = counter.kind === 'missing'
        ? 1n
        : BigInt(decodeText(counter.value.bytes));
      if (generation > MAX_GENERATION) return { kind: 'generationExhausted' };
      const token = { ownerId, leaseGeneration: generation };
      const conditions: ZLinkStoreCondition[] = [
        { kind: 'missing', key: ownerKey(ownerId) },
        counter.kind === 'missing'
          ? { kind: 'missing', key: OWNER_COUNTER_KEY }
          : { kind: 'version', key: OWNER_COUNTER_KEY, expected: counter.value.version }
      ];
      const result = await this.provider.write({
        conditions,
        mutations: [
          {
            kind: 'put',
            key: ownerKey(ownerId),
            bytes: encodeJson<OwnerRecord>({
              ownerId,
              leaseGeneration: generation.toString()
            }),
            retentionMs: leaseTtlMs
          },
          {
            kind: 'put',
            key: OWNER_COUNTER_KEY,
            bytes: encodeText((generation + 1n).toString())
          }
        ]
      }, signal);
      if (result.kind === 'conflict') continue;
      return {
        kind: 'claimed',
        token,
        leaseExpiresAt: new Date(result.storeNow.getTime() + leaseTtlMs),
        storeNow: result.storeNow
      };
    }
  }

  override async readOwnerLease(
    ownerId: string,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseReadResult> {
    const result = await this.provider.read(ownerKey(ownerId), signal);
    if (result.kind === 'missing') return { kind: 'missing' };
    const record = decodeJson<OwnerRecord>(result.value.bytes);
    if (record.ownerId !== ownerId || result.value.expiresAt === undefined) {
      throw new Error('Location Store owner lease record is invalid.');
    }
    return {
      kind: 'found',
      token: { ownerId, leaseGeneration: BigInt(record.leaseGeneration) },
      leaseExpiresAt: result.value.expiresAt,
      storeNow: result.value.storeNow
    };
  }

  override async renewOwnerLease(
    token: ZLinkLocationOwnerToken,
    leaseTtlMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseRenewResult> {
    requireOwnerInput(token.ownerId, leaseTtlMs);
    const key = ownerKey(token.ownerId);
    const current = await this.provider.read(key, signal);
    if (current.kind === 'missing') return { kind: 'stale' };
    const record = decodeJson<OwnerRecord>(current.value.bytes);
    if (BigInt(record.leaseGeneration) !== token.leaseGeneration) return { kind: 'stale' };
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key, expected: current.value.version }],
      mutations: [{
        kind: 'put',
        key,
        bytes: current.value.bytes,
        retentionMs: leaseTtlMs
      }]
    }, signal);
    if (result.kind === 'conflict') return { kind: 'stale' };
    return {
      kind: 'renewed',
      leaseExpiresAt: new Date(result.storeNow.getTime() + leaseTtlMs),
      storeNow: result.storeNow
    };
  }

  override async releaseOwnerLease(
    token: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseReleaseResult> {
    const key = ownerKey(token.ownerId);
    const current = await this.provider.read(key, signal);
    if (current.kind === 'missing') return 'stale';
    const record = decodeJson<OwnerRecord>(current.value.bytes);
    if (BigInt(record.leaseGeneration) !== token.leaseGeneration) return 'stale';
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key, expected: current.value.version }],
      mutations: [{ kind: 'delete', key }]
    }, signal);
    return result.kind === 'applied' ? 'released' : 'stale';
  }

  override async updateMeshNode(
    descriptor: ZLinkMeshNodeDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const leaseKey = ownerKey(descriptor.ownerId);
    const rowKey = meshKey(descriptor.meshName, String(descriptor.rid));
    const [lease, current] = await Promise.all([
      this.provider.read(leaseKey, signal),
      this.provider.read(rowKey, signal)
    ]);
    if (lease.kind === 'missing') return rejected(lease.storeNow);
    if (liveOwnerLeaseGeneration(lease) !== descriptor.leaseGeneration) {
      return rejected(lease.value.storeNow);
    }

    let generation = 1n;
    let rowCondition: ZLinkStoreCondition = { kind: 'missing', key: rowKey };
    if (current.kind === 'found') {
      const record = decodeJson<MeshRecord>(current.value.bytes);
      const stored = reviveMeshDescriptor(record.descriptor);
      generation = BigInt(record.generation);
      if (sameMeshDescriptor(stored, descriptor)) {
        return { status: WriteStatus.Stored, generation, updatedAt: current.value.storeNow };
      }
      const currentLease = await this.provider.read(ownerKey(stored.ownerId), signal);
      const currentLeaseGeneration = liveOwnerLeaseGeneration(currentLease);
      const takeover = canTakeOverStoredLocation(
        intent,
        stored.ownerId,
        stored.leaseGeneration,
        descriptor.ownerId,
        descriptor.leaseGeneration,
        currentLeaseGeneration
      );
      const renew = intent === 2
        && stored.ownerId === descriptor.ownerId
        && stored.leaseGeneration === descriptor.leaseGeneration
        && stored.lifecycleGeneration === descriptor.lifecycleGeneration
        && descriptor.descriptorRevision > stored.descriptorRevision;
      if (!takeover && !renew) {
        return {
          status: WriteStatus.IgnoredStale,
          generation,
          updatedAt: current.value.storeNow
        };
      }
      if (takeover) generation += 1n;
      rowCondition = { kind: 'version', key: rowKey, expected: current.value.version };
    } else if (intent === 2) {
      return rejected(lease.value.storeNow);
    }

    const result = await this.provider.write({
      conditions: [
        { kind: 'version', key: leaseKey, expected: lease.value.version },
        rowCondition
      ],
      mutations: [{
        kind: 'put',
        key: rowKey,
        bytes: encodeJson<MeshRecord>({
          generation: generation.toString(),
          descriptor: persistMeshDescriptor(descriptor)
        })
      }]
    }, signal);
    if (result.kind === 'conflict') return rejected(result.storeNow);
    return { status: WriteStatus.Stored, generation, updatedAt: result.storeNow };
  }

  override async removeMeshNode(
    key: ZLinkMeshNodeDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    const rowKey = meshKey(key.meshName, String(key.rid));
    const current = await this.provider.read(rowKey, signal);
    if (current.kind === 'missing') return WriteStatus.IgnoredStale;
    const descriptor = reviveMeshDescriptor(decodeJson<MeshRecord>(current.value.bytes).descriptor);
    if (descriptor.ownerId !== owner.ownerId
      || descriptor.leaseGeneration !== owner.leaseGeneration) {
      return WriteStatus.IgnoredStale;
    }
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key: rowKey, expected: current.value.version }],
      mutations: [{ kind: 'delete', key: rowKey }]
    }, signal);
    return result.kind === 'applied' ? WriteStatus.Stored : WriteStatus.IgnoredStale;
  }

  override async listMeshNodes(
    meshName: string,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> {
    const result = await this.provider.scan({
      prefix: meshPrefix(meshName),
      cursor: page.continuationToken === undefined
        ? undefined
        : ({ value: page.continuationToken } as ZLinkStoreScanCursor),
      limit: page.pageSize ?? 100
    }, signal);
    if (result.kind === 'expired') {
      throw new Error('Location Store scan snapshot expired.');
    }
    return {
      items: result.value.items.map(item =>
        reviveMeshDescriptor(decodeJson<MeshRecord>(item.value.bytes).descriptor)),
      continuationToken: result.value.nextCursor?.value
    };
  }

  override async updateClientServer(
    descriptor: ZLinkClientServerServerDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    validateClientServerDescriptor(descriptor);
    const normalized = reviveClientServerDescriptor({
      ...descriptor,
      serverRid: String(descriptor.serverRid)
    });
    return this.updateDescriptor(
      clientServerKey(normalized.channelName, String(normalized.serverRid)),
      normalized,
      intent,
      reviveClientServerDescriptor,
      sameClientServerDescriptor,
      canRenewClientServer,
      signal
    );
  }

  override async removeClientServer(
    key: ZLinkClientServerServerDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    return this.removeDescriptor(
      clientServerKey(key.channelName, String(key.serverRid)),
      owner,
      reviveClientServerDescriptor,
      signal
    );
  }

  override async listClientServers(
    channelName: string,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> {
    return this.listDescriptors(
      clientServerPrefix(channelName),
      page,
      reviveClientServerDescriptor,
      signal
    );
  }

  override async updateFanoutPublisher(
    descriptor: ZLinkFanoutPublisherDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    validateFanoutPublisherDescriptor(descriptor);
    const normalized = reviveFanoutDescriptor({
      ...descriptor,
      publisherRid: String(descriptor.publisherRid)
    });
    return this.updateDescriptor(
      fanoutKey(normalized.channelName, String(normalized.publisherRid)),
      normalized,
      intent,
      reviveFanoutDescriptor,
      sameFanoutDescriptor,
      canRenewFanout,
      signal
    );
  }

  override async removeFanoutPublisher(
    key: ZLinkFanoutPublisherDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    return this.removeDescriptor(
      fanoutKey(key.channelName, String(key.publisherRid)),
      owner,
      reviveFanoutDescriptor,
      signal
    );
  }

  override async listFanoutPublishers(
    channelName: string,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> {
    return this.listDescriptors(
      fanoutPrefix(channelName),
      page,
      reviveFanoutDescriptor,
      signal
    );
  }

  override async updateSpot(
    location: ZLinkSpotLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    return this.updateOwnedLocation(
      spotKey(location.meshName, String(location.spotId)),
      normalizeSpot(location),
      intent,
      signal
    );
  }

  override async removeSpot(
    key: ZLinkSpotLocationKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    return this.removeOwnedLocation(
      spotKey(key.meshName, String(key.spotId)),
      owner,
      signal
    );
  }

  override async resolveSpot(
    key: ZLinkSpotLocationKey,
    signal?: AbortSignal
  ): Promise<ZLinkSpotLocation | undefined> {
    const value = await this.readOwnedLocation<ZLinkSpotLocation>(
      spotKey(key.meshName, String(key.spotId)),
      signal
    );
    return value === undefined ? undefined : normalizeSpot(value);
  }

  override async listSpots(
    filter: ZLinkSpotLocationFilter,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkSpotLocation>> {
    return this.listOwnedLocations(
      `${PREFIX}spot:`,
      page,
      normalizeSpot,
      value => matchesSpot(value, filter),
      signal
    );
  }

  override async updateActor(
    location: ZLinkActorLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    return this.updateOwnedLocation(
      actorKey(location.meshName, location.actorId),
      normalizeActor(location),
      intent,
      signal
    );
  }

  override async removeActor(
    key: ZLinkActorLocationKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    return this.removeOwnedLocation(actorKey(key.meshName, key.actorId), owner, signal);
  }

  override async resolveActor(
    key: ZLinkActorLocationKey,
    signal?: AbortSignal
  ): Promise<ZLinkActorLocation | undefined> {
    const value = await this.readOwnedLocation<ZLinkActorLocation>(
      actorKey(key.meshName, key.actorId),
      signal
    );
    return value === undefined ? undefined : normalizeActor(value);
  }

  override async listActors(
    filter: ZLinkActorLocationFilter,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkActorLocation>> {
    return this.listOwnedLocations(
      `${PREFIX}actor:`,
      page,
      normalizeActor,
      value => matchesActor(value, filter),
      signal
    );
  }

  override async updateRoute(
    location: ZLinkRouteLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const rowKey = routeKey(String(location.routeKind), location.routeKey);
    const normalized = normalizeRoute(location);
    const current = await this.provider.read(rowKey, signal);
    let generation = 1n;
    let condition: ZLinkStoreCondition = { kind: 'missing', key: rowKey };
    if (current.kind === 'found') {
      const record = decodeJson<DescriptorRecord<ZLinkRouteLocation>>(current.value.bytes);
      const stored = normalizeRoute(record.descriptor);
      generation = BigInt(record.generation);
      if (intent === 1) {
        const ownerLease = await this.provider.read(ownerKey(stored.ownerId), signal);
        if (ownerLease.kind === 'found') {
          return {
            status: WriteStatus.RejectedConflict,
            generation,
            updatedAt: current.value.storeNow
          };
        }
      } else if (intent === 2
        && (stored.ownerId !== normalized.ownerId
          || normalized.generation !== 0n && stored.generation !== normalized.generation)) {
        return {
          status: WriteStatus.IgnoredStale,
          generation,
          updatedAt: current.value.storeNow
        };
      }
      if (intent !== 2) generation += 1n;
      condition = { kind: 'version', key: rowKey, expected: current.value.version };
    } else if (intent === 2) {
      return rejected(new Date());
    }
    const stored = { ...normalized, generation };
    const result = await this.provider.write({
      conditions: [condition],
      mutations: [{
        kind: 'put',
        key: rowKey,
        bytes: encodeJson<DescriptorRecord<ZLinkRouteLocation>>({
          generation: generation.toString(),
          descriptor: stored
        })
      }]
    }, signal);
    return result.kind === 'applied'
      ? { status: WriteStatus.Stored, generation, updatedAt: result.storeNow }
      : rejected(result.storeNow);
  }

  override async removeRoute(
    key: ZLinkRouteLocationKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const rowKey = routeKey(String(key.routeKind), key.routeKey);
    const current = await this.provider.read(rowKey, signal);
    if (current.kind === 'missing') return rejected(current.storeNow);
    const record = decodeJson<DescriptorRecord<ZLinkRouteLocation>>(current.value.bytes);
    if (record.descriptor.ownerId !== owner.ownerId
      || record.descriptor.generation !== owner.leaseGeneration) {
      return {
        status: WriteStatus.IgnoredStale,
        generation: BigInt(record.generation),
        updatedAt: current.value.storeNow
      };
    }
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key: rowKey, expected: current.value.version }],
      mutations: [{ kind: 'delete', key: rowKey }]
    }, signal);
    return result.kind === 'applied'
      ? { status: WriteStatus.Stored, generation: BigInt(record.generation), updatedAt: result.storeNow }
      : rejected(result.storeNow);
  }

  override async resolveRoute(
    key: ZLinkRouteLocationKey,
    signal?: AbortSignal
  ): Promise<ZLinkRouteLocation | undefined> {
    const value = await this.readOwnedLocation<ZLinkRouteLocation>(
      routeKey(String(key.routeKind), key.routeKey),
      signal
    );
    return value === undefined ? undefined : normalizeRoute(value);
  }

  override async listRoutes(
    filter: ZLinkRouteLocationFilter,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkRouteLocation>> {
    return this.listOwnedLocations(
      `${PREFIX}route:`,
      page,
      normalizeRoute,
      value => matchesRoute(value, filter),
      signal
    );
  }

  override async removeAllByOwner(
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<bigint> {
    const leaseKey = ownerKey(owner.ownerId);
    const lease = await this.provider.read(leaseKey, signal);
    if (liveOwnerLeaseGeneration(lease) !== owner.leaseGeneration) return 0n;
    let removed = await this.removeOwnedAuthorities(owner, signal);
    const candidates: OwnerCleanupCandidate[] = [];
    for (const prefix of [
      `${PREFIX}mesh:`,
      `${PREFIX}client-server:`,
      `${PREFIX}fanout:`,
      `${PREFIX}spot:`,
      `${PREFIX}actor:`,
      `${PREFIX}route:`
    ]) {
      for (;;) {
        const prefixCandidates: OwnerCleanupCandidate[] = [];
        let cursor: ZLinkStoreScanCursor | undefined;
        let expired = false;
        do {
          const result = await this.provider.scan({ prefix, cursor, limit: 1_000 }, signal);
          if (result.kind === 'expired') {
            expired = true;
            break;
          }
          for (const item of result.value.items) {
            prefixCandidates.push({ key: item.key, scanVersion: item.value.version });
          }
          cursor = result.value.nextCursor;
        } while (cursor !== undefined);
        if (expired) {
          // Snapshot scan expiry invalidates every page from this prefix.
          // Restart from its first page instead of silently leaving rows
          // behind after a partial scan.
          continue;
        }
        candidates.push(...prefixCandidates);
        break;
      }
    }
    for (let offset = 0; offset < candidates.length; offset += MAX_OWNER_CLEANUP_BATCH_ROWS) {
      removed += await this.removeOwnerCleanupBatch(
        owner,
        leaseKey,
        candidates.slice(offset, offset + MAX_OWNER_CLEANUP_BATCH_ROWS),
        signal
      );
    }
    return removed + await super.removeAllByOwner(owner);
  }

  private async removeOwnedAuthorities(
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<bigint> {
    const candidates: OwnerCleanupCandidate[] = [];
    for (;;) {
      const pageCandidates: OwnerCleanupCandidate[] = [];
      let cursor: ZLinkStoreScanCursor | undefined;
      let expired = false;
      do {
        const result = await this.provider.scan({
          prefix: `${PREFIX}authority:`,
          cursor,
          limit: 1_000
        }, signal);
        if (result.kind === 'expired') {
          expired = true;
          break;
        }
        for (const item of result.value.items) {
          pageCandidates.push({ key: item.key, scanVersion: item.value.version });
        }
        cursor = result.value.nextCursor;
      } while (cursor !== undefined);
      if (expired) {
        candidates.length = 0;
        continue;
      }
      candidates.push(...pageCandidates);
      break;
    }

    let removed = 0n;
    for (const candidate of candidates) {
      signal?.throwIfAborted();
      const current = await this.provider.read(candidate.key, signal);
      if (current.kind === 'missing'
        || current.value.version.value !== candidate.scanVersion.value) continue;
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (
        record.snapshot.ownerId !== owner.ownerId
        || record.snapshot.ownerLeaseGeneration !== owner.leaseGeneration
        || record.aggregate !== undefined
      ) continue;
      const encodedAuthority = decodeURIComponent(
        candidate.key.value.slice(`${PREFIX}authority:`.length)
      );
      const identity = decodeAuthorityKey({ value: encodedAuthority } as ZLinkAuthorityKey);
      if (record.snapshot.allocation.state === 'active') {
        const result = await this.compareExchangeAuthority(
          { value: encodedAuthority } as ZLinkAuthorityKey,
          current.value.version as unknown as ZLinkAuthorityStoreVersion,
          { kind: 'delete' },
          signal
        );
        if (result.kind === 'deleted') removed += 1n;
        continue;
      }
      if (record.reservationId === undefined) continue;
      const aborted = await this.abort({
        key: identity,
        reservationId: record.reservationId,
        expectedStoreVersion: current.value.version.value,
        target: {
          meshName: record.snapshot.allocation.descriptor.meshName,
          nodeRid: record.snapshot.allocation.descriptor.rid,
          nodeLifecycleGeneration:
            record.snapshot.allocation.descriptorLifecycleGeneration,
          owner: {
            ownerId: record.snapshot.ownerId,
            leaseGeneration: record.snapshot.ownerLeaseGeneration
          }
        }
      }, signal);
      if (aborted.kind === 'aborted') removed += 1n;
    }
    return removed;
  }

  private async removeOwnerCleanupBatch(
    owner: ZLinkLocationOwnerToken,
    leaseKey: ZLinkStoreKey,
    candidates: readonly OwnerCleanupCandidate[],
    signal?: AbortSignal
  ): Promise<bigint> {
    if (candidates.length === 0) return 0n;

    const currentRows = await Promise.all(candidates.map(candidate =>
      this.provider.read(candidate.key, signal)));
    const unchangedOwnedRows: OwnerCleanupCandidate[] = [];
    for (let index = 0; index < candidates.length; index += 1) {
      const candidate = candidates[index]!;
      const current = currentRows[index]!;
      if (current.kind === 'missing'
        || current.value.version.value !== candidate.scanVersion.value) {
        continue;
      }
      const record = decodeJson<OwnerCleanupRecord>(current.value.bytes);
      const descriptor = record.descriptor;
      if (descriptor.ownerId !== owner.ownerId
        || descriptorOwnerLeaseGeneration(descriptor) !== owner.leaseGeneration) {
        continue;
      }
      unchangedOwnedRows.push({
        key: candidate.key,
        scanVersion: current.value.version
      });
    }
    if (unchangedOwnedRows.length === 0) return 0n;

    const currentLease = await this.provider.read(leaseKey, signal);
    if (currentLease.kind === 'missing'
      || liveOwnerLeaseGeneration(currentLease) !== owner.leaseGeneration) {
      return 0n;
    }
    const result = await this.provider.write({
      conditions: [
        { kind: 'version', key: leaseKey, expected: currentLease.value.version },
        ...unchangedOwnedRows.map(row => ({
          kind: 'version' as const,
          key: row.key,
          expected: row.scanVersion
        }))
      ],
      mutations: unchangedOwnedRows.map(row => ({
        kind: 'delete' as const,
        key: row.key
      }))
    }, signal);
    if (result.kind === 'applied') return BigInt(unchangedOwnedRows.length);

    // A row may have changed while the page was being prepared. Split the
    // bounded batch so unaffected rows can still be removed atomically, while
    // the changed row is discarded by the next version re-read. A changed
    // owner lease causes every recursive branch to stop at the fence above.
    if (unchangedOwnedRows.length === 1) return 0n;
    const midpoint = Math.ceil(unchangedOwnedRows.length / 2);
    const left = await this.removeOwnerCleanupBatch(
      owner,
      leaseKey,
      unchangedOwnedRows.slice(0, midpoint),
      signal
    );
    const right = await this.removeOwnerCleanupBatch(
      owner,
      leaseKey,
      unchangedOwnedRows.slice(midpoint),
      signal
    );
    return left + right;
  }

  private async updateDescriptor<T extends OwnedDescriptor>(
    rowKey: ZLinkStoreKey,
    descriptor: T,
    intent: ZLinkLocationWriteIntent,
    revive: (descriptor: T) => T,
    same: (left: T, right: T) => boolean,
    canRenew: (current: T, next: T) => boolean,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const leaseKey = ownerKey(descriptor.ownerId);
    const [lease, current] = await Promise.all([
      this.provider.read(leaseKey, signal),
      this.provider.read(rowKey, signal)
    ]);
    if (lease.kind === 'missing') return rejected(lease.storeNow);
    if (liveOwnerLeaseGeneration(lease) !== descriptor.leaseGeneration) {
      return rejected(lease.value.storeNow);
    }

    let generation = 1n;
    let rowCondition: ZLinkStoreCondition = { kind: 'missing', key: rowKey };
    if (current.kind === 'found') {
      const record = decodeJson<DescriptorRecord<T>>(current.value.bytes);
      const stored = revive(record.descriptor);
      generation = BigInt(record.generation);
      if (same(stored, descriptor)) {
        return { status: WriteStatus.Stored, generation, updatedAt: current.value.storeNow };
      }
      const currentLease = await this.provider.read(ownerKey(stored.ownerId), signal);
      const currentLeaseGeneration = liveOwnerLeaseGeneration(currentLease);
      const takeover = canTakeOverStoredLocation(
        intent,
        stored.ownerId,
        stored.leaseGeneration,
        descriptor.ownerId,
        descriptor.leaseGeneration,
        currentLeaseGeneration
      );
      const renew = intent === 2 && canRenew(stored, descriptor);
      if (!takeover && !renew) {
        return {
          status: WriteStatus.IgnoredStale,
          generation,
          updatedAt: current.value.storeNow
        };
      }
      if (takeover) generation += 1n;
      rowCondition = { kind: 'version', key: rowKey, expected: current.value.version };
    } else if (intent === 2) {
      return rejected(lease.value.storeNow);
    }

    const result = await this.provider.write({
      conditions: [
        { kind: 'version', key: leaseKey, expected: lease.value.version },
        rowCondition
      ],
      mutations: [{
        kind: 'put',
        key: rowKey,
        bytes: encodeJson<DescriptorRecord<T>>({
          generation: generation.toString(),
          descriptor
        })
      }]
    }, signal);
    if (result.kind === 'conflict') return rejected(result.storeNow);
    return { status: WriteStatus.Stored, generation, updatedAt: result.storeNow };
  }

  private async removeDescriptor<T extends OwnedDescriptor>(
    rowKey: ZLinkStoreKey,
    owner: ZLinkLocationOwnerToken,
    revive: (descriptor: T) => T,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    const current = await this.provider.read(rowKey, signal);
    if (current.kind === 'missing') return WriteStatus.IgnoredStale;
    const descriptor = revive(
      decodeJson<DescriptorRecord<T>>(current.value.bytes).descriptor);
    if (descriptor.ownerId !== owner.ownerId
      || descriptor.leaseGeneration !== owner.leaseGeneration) {
      return WriteStatus.IgnoredStale;
    }
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key: rowKey, expected: current.value.version }],
      mutations: [{ kind: 'delete', key: rowKey }]
    }, signal);
    return result.kind === 'applied' ? WriteStatus.Stored : WriteStatus.IgnoredStale;
  }

  private async listDescriptors<T extends OwnedDescriptor>(
    prefix: string,
    page: ZLinkPageRequest,
    revive: (descriptor: T) => T,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<T>> {
    const result = await this.provider.scan({
      prefix,
      cursor: page.continuationToken === undefined
        ? undefined
        : ({ value: page.continuationToken } as ZLinkStoreScanCursor),
      limit: page.pageSize ?? 100
    }, signal);
    if (result.kind === 'expired') {
      throw new Error('Location Store scan snapshot expired.');
    }
    return {
      items: result.value.items.map(item =>
        revive(decodeJson<DescriptorRecord<T>>(item.value.bytes).descriptor)),
      continuationToken: result.value.nextCursor?.value
    };
  }

  private async updateOwnedLocation<T extends LeaseOwnedLocation>(
    rowKey: ZLinkStoreKey,
    location: T,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const leaseKey = ownerKey(location.ownerId);
    const [lease, current] = await Promise.all([
      this.provider.read(leaseKey, signal),
      this.provider.read(rowKey, signal)
    ]);
    if (lease.kind === 'missing') return rejected(lease.storeNow);
    if (liveOwnerLeaseGeneration(lease) !== locationOwnerGeneration(location)) {
      return rejected(lease.value.storeNow);
    }

    let generation = 1n;
    let rowCondition: ZLinkStoreCondition = { kind: 'missing', key: rowKey };
    if (current.kind === 'found') {
      const record = decodeJson<DescriptorRecord<T>>(current.value.bytes);
      generation = BigInt(record.generation);
      const stored = record.descriptor;
      if (intent === 2) {
        if (stored.ownerId !== location.ownerId
          || locationOwnerGeneration(stored) !== locationOwnerGeneration(location)) {
          return {
            status: WriteStatus.IgnoredStale,
            generation,
            updatedAt: current.value.storeNow
          };
        }
      } else {
        const currentLease = await this.provider.read(ownerKey(stored.ownerId), signal);
        const currentLeaseGeneration = liveOwnerLeaseGeneration(currentLease);
        const takeover = canTakeOverStoredLocation(
          intent,
          stored.ownerId,
          locationOwnerGeneration(stored),
          location.ownerId,
          locationOwnerGeneration(location),
          currentLeaseGeneration
        );
        if (!takeover) {
          return {
            status: WriteStatus.RejectedConflict,
            generation,
            updatedAt: current.value.storeNow
          };
        }
        generation += 1n;
      }
      rowCondition = { kind: 'version', key: rowKey, expected: current.value.version };
    } else if (intent === 2) {
      return rejected(lease.value.storeNow);
    }

    const result = await this.provider.write({
      conditions: [
        { kind: 'version', key: leaseKey, expected: lease.value.version },
        rowCondition
      ],
      mutations: [{
        kind: 'put',
        key: rowKey,
        bytes: encodeJson<DescriptorRecord<T>>({
          generation: generation.toString(),
          descriptor: location
        })
      }]
    }, signal);
    return result.kind === 'applied'
      ? { status: WriteStatus.Stored, generation, updatedAt: result.storeNow }
      : rejected(result.storeNow);
  }

  private async removeOwnedLocation(
    rowKey: ZLinkStoreKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    const current = await this.provider.read(rowKey, signal);
    if (current.kind === 'missing') return WriteStatus.IgnoredStale;
    const record = decodeJson<DescriptorRecord<LeaseOwnedLocation>>(current.value.bytes);
    const descriptor = record.descriptor as unknown as {
      readonly ownerId: string;
      readonly leaseGeneration?: bigint;
    };
    const leaseGeneration = descriptor.leaseGeneration ?? BigInt(record.generation);
    if (descriptor.ownerId !== owner.ownerId
      || leaseGeneration !== owner.leaseGeneration) {
      return WriteStatus.IgnoredStale;
    }
    const result = await this.provider.write({
      conditions: [{ kind: 'version', key: rowKey, expected: current.value.version }],
      mutations: [{ kind: 'delete', key: rowKey }]
    }, signal);
    return result.kind === 'applied' ? WriteStatus.Stored : WriteStatus.IgnoredStale;
  }

  private async readOwnedLocation<T extends StoredLocation>(
    rowKey: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<T | undefined> {
    const result = await this.provider.read(rowKey, signal);
    return result.kind === 'missing'
      ? undefined
      : decodeJson<DescriptorRecord<T>>(result.value.bytes).descriptor;
  }

  private async listOwnedLocations<T extends StoredLocation>(
    prefix: string,
    page: ZLinkPageRequest,
    revive: (value: T) => T,
    matches: (value: T) => boolean,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<T>> {
    const requested = page.pageSize ?? 100;
    let cursor = page.continuationToken === undefined
      ? undefined
      : ({ value: page.continuationToken } as ZLinkStoreScanCursor);
    const items: T[] = [];
    do {
      const result = await this.provider.scan({
        prefix,
        cursor,
        limit: requested
      }, signal);
      if (result.kind === 'expired') {
        throw new Error('Location Store scan snapshot expired.');
      }
      for (const item of result.value.items) {
        const value = revive(
          decodeJson<DescriptorRecord<T>>(item.value.bytes).descriptor);
        if (matches(value)) items.push(value);
        if (items.length === requested) {
          return {
            items,
            continuationToken: result.value.nextCursor?.value
          };
        }
      }
      cursor = result.value.nextCursor;
    } while (cursor !== undefined);
    return { items };
  }

  private async projectAuthority(
    record: AuthorityRecord,
    version: ZLinkStoreVersion,
    storeNow: Date,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const marker = record.aggregate;
    if (marker === undefined) {
      return authoritySnapshot(
        record.snapshot,
        record.visibleStoreVersion === undefined
          ? version
          : ({ value: record.visibleStoreVersion } as ZLinkStoreVersion),
        storeNow
      );
    }
    const aggregateRead = await this.provider.read(
      aggregateKey(aggregateFence(
        aggregateId(marker.aggregateId),
        marker.aggregateGeneration
      )),
      signal
    );
    if (aggregateRead.kind === 'missing') {
      throw new Error('Aggregate participant references a missing authority record.');
    }
    const aggregate = decodeJson<AggregateRecord>(aggregateRead.value.bytes);
    if (aggregate.state !== 'committed') {
      return authoritySnapshot(
        record.snapshot,
        { value: marker.expectedStoreVersion } as ZLinkStoreVersion,
        storeNow
      );
    }
    const fence = aggregateFence(
      aggregateId(marker.aggregateId),
      marker.aggregateGeneration
    );
    const payload = await requireProviderBytes(
      this.provider,
      aggregateParticipantPayloadKey(fence, marker.index),
      signal
    );
    if (sha256Hex(payload) !== marker.authorityPayloadSha256) {
      throw new Error('Committed aggregate participant payload checksum is invalid.');
    }
    const projected: StoredAuthoritySnapshot = marker.ownerTransition === 'newOwner'
      ? {
          payload: Buffer.from(payload),
          objectGeneration: record.snapshot.objectGeneration,
          authorityOwnerGeneration: marker.targetAuthorityOwnerGeneration,
          ownerId: aggregate.targetOwner.ownerId,
          ownerLeaseGeneration: aggregate.targetOwner.leaseGeneration,
          allocation: {
            ...record.snapshot.allocation,
            descriptor: { ...aggregate.targetDescriptor },
            descriptorLifecycleGeneration:
              aggregate.targetDescriptorLifecycleGeneration,
            capacity: cloneCapacity(record.snapshot.allocation.capacity)
          }
        }
      : {
          ...record.snapshot,
          payload: Buffer.from(payload)
        };
    return authoritySnapshot(projected, version, storeNow);
  }

  private async normalizeCommittedAggregate(
    fence: ZLinkAggregateFence,
    aggregate: AggregateRecord,
    signal?: AbortSignal
  ): Promise<void> {
    const entries = await this.aggregateInventory.read(
      fence,
      Buffer.from(aggregate.inventoryDigest),
      signal
    );
    await parallelForEach(entries, 64, async (entry, index) => {
      const rowKey = authorityKey(entry.authorityKey);
      for (;;) {
        const current = await this.provider.read(rowKey, signal);
        if (current.kind === 'missing') {
          throw new Error('Committed aggregate participant authority is missing.');
        }
        const record = decodeJson<AuthorityRecord>(current.value.bytes);
        if (record.aggregate === undefined) {
          if (
            !Buffer.from(record.snapshot.payload).equals(
              Buffer.from(await requireProviderBytes(
                this.provider,
                aggregateParticipantPayloadKey(fence, index),
                signal
              ))
            )
          ) {
            throw new Error('Normalized aggregate participant payload changed.');
          }
          return;
        }
        if (!sameAggregateMarkerEntry(record.aggregate, fence, index, entry)) {
          throw new Error('Committed aggregate participant fence changed.');
        }
        const payload = await requireProviderBytes(
          this.provider,
          aggregateParticipantPayloadKey(fence, index),
          signal
        );
        if (sha256Hex(payload) !== entry.authorityPayloadSha256) {
          throw new Error('Committed aggregate participant payload checksum is invalid.');
        }
        const snapshot: StoredAuthoritySnapshot =
          record.aggregate.ownerTransition === 'newOwner'
            ? {
                payload: Buffer.from(payload),
                objectGeneration: record.snapshot.objectGeneration,
                authorityOwnerGeneration:
                  record.aggregate.targetAuthorityOwnerGeneration,
                ownerId: aggregate.targetOwner.ownerId,
                ownerLeaseGeneration: aggregate.targetOwner.leaseGeneration,
                allocation: {
                  ...record.snapshot.allocation,
                  descriptor: { ...aggregate.targetDescriptor },
                  descriptorLifecycleGeneration:
                    aggregate.targetDescriptorLifecycleGeneration,
                  capacity: cloneCapacity(record.snapshot.allocation.capacity)
                }
              }
            : {
                ...record.snapshot,
                payload: Buffer.from(payload)
              };
        const normalized: AuthorityRecord = {
          ...record,
          snapshot,
          aggregate: undefined,
          visibleStoreVersion: undefined
        };
        const result = await this.provider.write({
          conditions: [{
            kind: 'version',
            key: rowKey,
            expected: current.value.version
          }],
          mutations: [{
            kind: 'put',
            key: rowKey,
            bytes: encodeJson(normalized)
          }]
        }, signal);
        if (result.kind === 'applied') return;
      }
    });
  }

  private async putImmutable(
    key: ZLinkStoreKey,
    bytes: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    if (bytes.byteLength > 1024 * 1024) {
      throw new RangeError('Aggregate participant staging value exceeds 1 MiB.');
    }
    const result = await this.provider.write({
      conditions: [{ kind: 'missing', key }],
      mutations: [{ kind: 'put', key, bytes: Buffer.from(bytes) }]
    }, signal);
    if (result.kind === 'applied') return;
    const current = await this.provider.read(key, signal);
    if (
      current.kind !== 'found'
      || !Buffer.from(current.value.bytes).equals(Buffer.from(bytes))
    ) {
      throw new Error(`Immutable aggregate participant value changed: ${key.value}.`);
    }
  }

  private async abortAggregateStaging(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<void> {
    const key = aggregateKey(fence);
    for (;;) {
      const current = await this.provider.read(key, signal);
      if (current.kind === 'missing') return;
      const record = decodeJson<AggregateRecord>(current.value.bytes);
      if (record.state === 'aborted') {
        await this.clearAggregateMarkersByScan(fence, signal);
        return;
      }
      if (record.state !== 'staging') return;
      const result = await this.provider.write({
        conditions: [{ kind: 'version', key, expected: current.value.version }],
        mutations: [{
          kind: 'put',
          key,
          bytes: encodeJson({ ...record, state: 'aborted' } satisfies AggregateRecord)
        }]
      }, signal);
      if (result.kind === 'applied') {
        await this.clearAggregateMarkersByScan(fence, signal);
        return;
      }
    }
  }

  private async clearAggregateMarkers(
    fence: ZLinkAggregateFence,
    installed: readonly {
      readonly key: ZLinkStoreKey;
      readonly expectedStoreVersion: string;
    }[],
    signal?: AbortSignal
  ): Promise<void> {
    await parallelForEach(installed, 64, async value => {
      await this.clearAggregateMarker(fence, value.key, signal);
    });
  }

  private async clearAggregateMarkersByScan(
    fence: ZLinkAggregateFence,
    signal?: AbortSignal
  ): Promise<void> {
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const result = await this.provider.scan({
        prefix: `${PREFIX}authority:`,
        cursor,
        limit: 1_000
      }, signal);
      if (result.kind === 'expired') {
        cursor = undefined;
        continue;
      }
      const matching = result.value.items.filter(item => {
        const record = decodeJson<AuthorityRecord>(item.value.bytes);
        return record.aggregate?.aggregateId === fence.aggregateId.value
          && record.aggregate.aggregateGeneration === fence.aggregateGeneration;
      });
      await parallelForEach(matching, 64, async item => {
        await this.clearAggregateMarker(fence, item.key, signal);
      });
      if (result.value.nextCursor === undefined) return;
      cursor = result.value.nextCursor;
    }
  }

  private async clearAggregateMarker(
    fence: ZLinkAggregateFence,
    key: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<void> {
    for (;;) {
      const current = await this.provider.read(key, signal);
      if (current.kind === 'missing') return;
      const record = decodeJson<AuthorityRecord>(current.value.bytes);
      if (
        record.aggregate === undefined
        || record.aggregate.aggregateId !== fence.aggregateId.value
        || record.aggregate.aggregateGeneration !== fence.aggregateGeneration
      ) {
        return;
      }
      const result = await this.provider.write({
        conditions: [{ kind: 'version', key, expected: current.value.version }],
        mutations: [{
          kind: 'put',
          key,
          bytes: encodeJson({ ...record, aggregate: undefined } satisfies AuthorityRecord)
        }]
      }, signal);
      if (result.kind === 'applied') return;
    }
  }
}

const AMBIGUOUS_WRITE_RECONCILIATION_TIMEOUT_MS = 5_000;

/**
 * Keeps provider failures private while preserving the opaque Store contract.
 *
 * A write response can be lost after the provider applied the atomic batch.
 * The Framework confirms every mutation by an independent exact read. A put
 * must retain the requested bytes and a version different from the conditioned
 * version; a delete must remain absent. Any mismatch is a normal conflict, so
 * the domain repository can reread its authoritative record and classify the
 * operation without exposing a provider-specific retry API.
 */
class AmbiguousWriteReconcilingLocationStore implements ZLinkLocationStore {
  constructor(private readonly inner: ZLinkLocationStore) {}

  read(key: ZLinkStoreKey, signal?: AbortSignal): Promise<ZLinkStoreReadResult> {
    return this.inner.read(key, signal);
  }

  scan(
    request: ZLinkStoreScanRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreScanResult> {
    return this.inner.scan(request, signal);
  }

  async write(
    request: ZLinkStoreWriteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreWriteResult> {
    try {
      return await this.inner.write(request, signal);
    } catch (failure) {
      try {
        return await this.reconcile(request);
      } catch {
        throw failure;
      }
    }
  }

  private async reconcile(
    request: ZLinkStoreWriteRequest
  ): Promise<ZLinkStoreWriteResult> {
    if (request.mutations.length === 0) {
      throw new Error('Cannot reconcile an opaque Store write without mutations.');
    }
    const reconciliationSignal = AbortSignal.timeout(
      AMBIGUOUS_WRITE_RECONCILIATION_TIMEOUT_MS
    );
    const reads = await Promise.all(request.mutations.map(
      mutation => this.inner.read(mutation.key, reconciliationSignal)
    ));
    const putVersions: Array<{
      readonly key: ZLinkStoreKey;
      readonly version: ZLinkStoreVersion;
    }> = [];
    let storeNow = new Date(0);

    for (let index = 0; index < request.mutations.length; index += 1) {
      const mutation = request.mutations[index];
      const read = reads[index];
      storeNow = read.kind === 'found'
        ? latestDate(storeNow, read.value.storeNow)
        : latestDate(storeNow, read.storeNow);

      if (mutation.kind === 'delete') {
        if (read.kind !== 'missing') return { kind: 'conflict', storeNow };
        continue;
      }
      if (read.kind !== 'found'
        || !Buffer.from(read.value.bytes).equals(Buffer.from(mutation.bytes))
        || !versionAdvanced(request.conditions, mutation.key, read.value.version)) {
        return { kind: 'conflict', storeNow };
      }
      putVersions.push({ key: mutation.key, version: read.value.version });
    }

    return { kind: 'applied', putVersions, storeNow };
  }
}

function versionAdvanced(
  conditions: readonly ZLinkStoreCondition[],
  key: ZLinkStoreKey,
  current: ZLinkStoreVersion
): boolean {
  const condition = conditions.find(candidate => candidate.key.value === key.value);
  return condition?.kind !== 'version' || condition.expected.value !== current.value;
}

function latestDate(left: Date, right: Date): Date {
  return left.getTime() >= right.getTime() ? left : right;
}

type OwnedDescriptor =
  | ZLinkMeshNodeDescriptor
  | ZLinkClientServerServerDescriptor
  | ZLinkFanoutPublisherDescriptor;

type LeaseOwnedLocation = ZLinkSpotLocation | ZLinkActorLocation;
type StoredLocation = LeaseOwnedLocation | ZLinkRouteLocation;
type OwnedStoreRecord = OwnedDescriptor | StoredLocation;

function validateProviderAggregateRequest(request: ZLinkAggregatePrepareRequest): void {
  requireText(request.aggregateId.value, 'aggregate ID');
  if (request.aggregateGeneration < 1n || request.participants.length < 1) {
    throw new RangeError('Aggregate generation and participant count are invalid.');
  }
  if (request.inventoryDigest.byteLength !== 32) {
    throw new TypeError('Aggregate inventory digest must contain 32 bytes.');
  }
  const keys = request.participants.map(value => value.authorityKey.value);
  if (
    new Set(keys).size !== keys.length
    || keys.some((key, index) => index > 0 && keys[index - 1]!.localeCompare(key) >= 0)
  ) {
    throw new TypeError('Aggregate participants must be unique and canonically sorted.');
  }
  for (const participant of request.participants) {
    requireText(participant.authorityKey.value, 'aggregate authority key');
    requireText(participant.expectedStoreVersion.value, 'aggregate expected Store version');
    if (
      participant.authorityPayload.byteLength > 1024 * 1024
      || participant.membershipMutation.byteLength > 1024 * 1024
    ) {
      throw new RangeError('Aggregate participant value exceeds 1 MiB.');
    }
  }
}

function validateAggregateFence(fence: ZLinkAggregateFence): void {
  requireText(fence.aggregateId.value, 'aggregate ID');
  if (fence.aggregateGeneration < 1n) {
    throw new RangeError('Aggregate generation must be positive.');
  }
}

function aggregateRecord(
  request: ZLinkAggregatePrepareRequest,
  requestFingerprint: string,
  state: AggregateRecord['state']
): AggregateRecord {
  return {
    state,
    requestFingerprint,
    participantCount: request.participants.length,
    inventoryDigest: Buffer.from(request.inventoryDigest),
    targetDescriptor: { ...request.targetDescriptor },
    targetDescriptorLifecycleGeneration:
      request.targetDescriptorLifecycleGeneration,
    capacity: cloneCapacity(request.capacity),
    targetOwner: { ...request.targetOwner }
  };
}

function reconcileAggregatePrepare(
  record: AggregateRecord,
  fingerprint: string,
  fence: ZLinkAggregateFence
): ZLinkAggregatePrepareResult | undefined {
  if (record.requestFingerprint !== fingerprint) return { kind: 'conflict' };
  if (record.state === 'prepared') return { kind: 'alreadyPrepared', fence };
  if (record.state === 'committed') return { kind: 'stale' };
  if (record.state === 'aborted') return { kind: 'conflict' };
  return undefined;
}

function sameAggregateMarker(
  marker: AggregateParticipantFenceRecord | undefined,
  fence: ZLinkAggregateFence,
  index: number,
  participant: ZLinkAggregatePrepareRequest['participants'][number]
): boolean {
  return marker !== undefined
    && marker.aggregateId === fence.aggregateId.value
    && marker.aggregateGeneration === fence.aggregateGeneration
    && marker.index === index
    && marker.expectedStoreVersion === participant.expectedStoreVersion.value
    && marker.ownerTransition === participant.ownerTransition;
}

function sameAggregateMarkerEntry(
  marker: AggregateParticipantFenceRecord | undefined,
  fence: ZLinkAggregateFence,
  index: number,
  entry: {
    readonly expectedStoreVersion: string;
    readonly ownerTransition: 'preserve' | 'newOwner';
    readonly authorityPayloadSha256: string;
    readonly membershipMutationSha256: string;
  }
): boolean {
  return marker !== undefined
    && marker.aggregateId === fence.aggregateId.value
    && marker.aggregateGeneration === fence.aggregateGeneration
    && marker.index === index
    && marker.expectedStoreVersion === entry.expectedStoreVersion
    && marker.ownerTransition === entry.ownerTransition
    && marker.authorityPayloadSha256 === entry.authorityPayloadSha256
    && marker.membershipMutationSha256 === entry.membershipMutationSha256;
}

function aggregateTarget(
  request: ZLinkAggregatePrepareRequest
): ZLinkObjectCommitRequest['target'] {
  return {
    meshName: request.targetDescriptor.meshName,
    nodeRid: request.targetDescriptor.rid,
    nodeLifecycleGeneration: request.targetDescriptorLifecycleGeneration,
    owner: { ...request.targetOwner }
  };
}

function aggregateTargetFromRecord(
  record: AggregateRecord
): ZLinkObjectCommitRequest['target'] {
  return {
    meshName: record.targetDescriptor.meshName,
    nodeRid: record.targetDescriptor.rid,
    nodeLifecycleGeneration: record.targetDescriptorLifecycleGeneration,
    owner: { ...record.targetOwner }
  };
}

function addCapacityVector(
  left: ZLinkCapacityVector,
  right: ZLinkCapacityVector
): ZLinkCapacityVector {
  let spotType = left.spotType;
  if (right.spotType !== undefined) {
    if (
      spotType !== undefined
      && (
        spotType.objectKind !== right.spotType.objectKind
        || spotType.stableType !== right.spotType.stableType
      )
    ) {
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

async function requireProviderBytes(
  provider: ZLinkLocationStore,
  key: ZLinkStoreKey,
  signal?: AbortSignal
): Promise<Uint8Array> {
  const read = await provider.read(key, signal);
  if (read.kind === 'missing') {
    throw new Error(`Aggregate participant value is missing: ${key.value}.`);
  }
  return read.value.bytes;
}

async function parallelForEach<T>(
  values: readonly T[],
  concurrency: number,
  operation: (value: T, index: number) => Promise<void>
): Promise<void> {
  let next = 0;
  await Promise.all(Array.from(
    { length: Math.min(concurrency, values.length) },
    async () => {
      while (next < values.length) {
        const index = next++;
        await operation(values[index]!, index);
      }
    }
  ));
}

function sha256Hex(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

function ownerKey(ownerId: string) {
  return storeKey(`${PREFIX}owner:${encodeURIComponent(ownerId)}`);
}

function authorityKey(value: string) {
  return storeKey(`${PREFIX}authority:${encodeURIComponent(value)}`);
}

function authorityContractKey(value: string): ZLinkAuthorityKey {
  return { value } as ZLinkAuthorityKey;
}

function aggregateId(value: string): ZLinkAggregateId {
  return { value } as ZLinkAggregateId;
}

function aggregateFence(
  id: ZLinkAggregateId,
  generation: bigint
): ZLinkAggregateFence {
  return { aggregateId: id, aggregateGeneration: generation };
}

function aggregateKey(fence: ZLinkAggregateFence): ZLinkStoreKey {
  return storeKey(
    `${PREFIX}aggregate:${encodeURIComponent(fence.aggregateId.value)}:${fence.aggregateGeneration}`
  );
}

function aggregateParticipantPayloadKey(
  fence: ZLinkAggregateFence,
  index: number
): ZLinkStoreKey {
  return storeKey(
    `${PREFIX}aggregate-participant:${encodeURIComponent(fence.aggregateId.value)}:`
      + `${fence.aggregateGeneration}:${index}:authority`
  );
}

function aggregateParticipantMembershipKey(
  fence: ZLinkAggregateFence,
  index: number
): ZLinkStoreKey {
  return storeKey(
    `${PREFIX}aggregate-participant:${encodeURIComponent(fence.aggregateId.value)}:`
      + `${fence.aggregateGeneration}:${index}:membership`
  );
}

function authorityGenerationKey(value: string) {
  return storeKey(`${PREFIX}authority-generation:${encodeURIComponent(value)}`);
}

function capacityKey(meshName: string, nodeRid: string) {
  return storeKey(
    `${PREFIX}capacity:${encodeURIComponent(meshName)}:${encodeURIComponent(nodeRid)}`
  );
}

function relocationCapacityKey(reservationId: string) {
  return storeKey(`${PREFIX}relocation-capacity:${encodeURIComponent(reservationId)}`);
}

function creationTerminalKey(operation: ZLinkCreationOperationIdentity) {
  validateCreationOperation(operation);
  return storeKey(`${PREFIX}creation-terminal:${[
    String(operation.sourceNodeRid),
    operation.sourceNodeGeneration.toString(),
    operation.operationId.high.toString(16).padStart(16, '0'),
    operation.operationId.low.toString(16).padStart(16, '0')
  ].map(encodeURIComponent).join(':')}`);
}

function meshPrefix(meshName: string): string {
  return `${PREFIX}mesh:${encodeURIComponent(meshName)}:`;
}

function meshKey(meshName: string, nodeRid: string) {
  return storeKey(`${meshPrefix(meshName)}${encodeURIComponent(nodeRid)}`);
}

function clientServerPrefix(channelName: string): string {
  return `${PREFIX}client-server:${encodeURIComponent(channelName)}:`;
}

function clientServerKey(channelName: string, serverRid: string) {
  return storeKey(`${clientServerPrefix(channelName)}${encodeURIComponent(serverRid)}`);
}

function fanoutPrefix(channelName: string): string {
  return `${PREFIX}fanout:${encodeURIComponent(channelName)}:`;
}

function fanoutKey(channelName: string, publisherRid: string) {
  return storeKey(`${fanoutPrefix(channelName)}${encodeURIComponent(publisherRid)}`);
}

function spotKey(meshName: string, spotId: string) {
  return storeKey(
    `${PREFIX}spot:${encodeURIComponent(meshName)}:${encodeURIComponent(spotId)}`);
}

function actorKey(meshName: string, actorId: string) {
  return storeKey(
    `${PREFIX}actor:${encodeURIComponent(meshName)}:${encodeURIComponent(actorId)}`);
}

function routeKey(routeKind: string, value: string) {
  return storeKey(
    `${PREFIX}route:${encodeURIComponent(routeKind)}:${encodeURIComponent(value)}`);
}

function encodeText(value: string): Uint8Array {
  return Buffer.from(value, 'utf8');
}

function decodeText(value: Uint8Array): string {
  return Buffer.from(value).toString('utf8');
}

function encodeJson<T>(value: T): Uint8Array {
  return Buffer.from(JSON.stringify(value, (_key, candidate) => {
    if (typeof candidate === 'bigint') return { $bigint: candidate.toString() };
    if (candidate instanceof Uint8Array) {
      return { $bytes: Buffer.from(candidate).toString('base64') };
    }
    return candidate;
  }), 'utf8');
}

function decodeJson<T>(value: Uint8Array): T {
  return JSON.parse(decodeText(value), (_key, candidate) => {
    if (candidate !== null
      && typeof candidate === 'object'
      && Object.keys(candidate).length === 1
      && typeof candidate.$bigint === 'string') {
      return BigInt(candidate.$bigint);
    }
    if (candidate !== null
      && typeof candidate === 'object'
      && Object.keys(candidate).length === 1
      && typeof candidate.$bytes === 'string') {
      return Buffer.from(candidate.$bytes, 'base64');
    }
    return candidate;
  }) as T;
}

function createTerminalRecord(
  request: ZLinkObjectCreationCompleteRequest,
  now: Date
): ZLinkCreationTerminalRecord {
  const publication = request.completion.terminal;
  validateCreationOperation(publication.operation);
  if (
    publication.terminalEnvelope.byteLength > MAX_CREATION_TERMINAL_BYTES
    || publication.terminalEnvelopeSha256.byteLength !== 32
  ) {
    throw new RangeError(
      'Creation terminal envelope must not exceed 1 MiB and requires a SHA-256 digest.'
    );
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
    || expiresAtMs <= now.getTime()
  ) {
    throw new RangeError(
      'Creation terminal expiry must be the live operation deadline plus five minutes.'
    );
  }
  if (request.completion.kind === 'created') {
    validatePayloadSize(request.completion.readyPayload, 'Actor ready payload');
  }
  return {
    state: request.completion.kind,
    operation: {
      sourceNodeRid: publication.operation.sourceNodeRid,
      sourceNodeGeneration: publication.operation.sourceNodeGeneration,
      operationId: { ...publication.operation.operationId }
    },
    reservationId: requireText(request.reservationId, 'creation reservation ID'),
    objectKind: request.key.kind,
    terminalEnvelope: Buffer.from(publication.terminalEnvelope),
    terminalEnvelopeSha256: Buffer.from(publication.terminalEnvelopeSha256),
    expiresAt: new Date(expiresAtMs),
    storeNow: new Date(now)
  };
}

function reviveCreationTerminal(
  record: ZLinkCreationTerminalRecord
): ZLinkCreationTerminalRecord {
  return {
    ...record,
    operation: {
      ...record.operation,
      sourceNodeRid: String(record.operation.sourceNodeRid),
      operationId: { ...record.operation.operationId }
    },
    terminalEnvelope: Buffer.from(record.terminalEnvelope),
    terminalEnvelopeSha256: Buffer.from(record.terminalEnvelopeSha256),
    expiresAt: reviveDate(record.expiresAt),
    storeNow: reviveDate(record.storeNow)
  };
}

function validateCreationOperation(operation: ZLinkCreationOperationIdentity): void {
  const sourceRid = String(operation.sourceNodeRid);
  const sourceRidBytes = Buffer.byteLength(sourceRid, 'utf8');
  if (sourceRidBytes < 1 || sourceRidBytes > 255 || sourceRid.includes('\0')) {
    throw new TypeError(
      'Creation terminal source node RID must contain 1..255 UTF-8 bytes without NUL.'
    );
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

function validatePayloadSize(value: Uint8Array, name: string): void {
  if (value.byteLength > MAX_CREATION_TERMINAL_BYTES) {
    throw new RangeError(`${name} must not exceed 1 MiB.`);
  }
}

function requireText(value: string, name: string): string {
  if (value.length === 0 || value.includes('\0')) {
    throw new TypeError(`${name} must be non-empty text without NUL.`);
  }
  return value;
}

function authoritySnapshot(
  snapshot: StoredAuthoritySnapshot,
  storeVersion: ZLinkStoreVersion,
  storeNow: Date
): ZLinkAuthoritySnapshot {
  return {
    kind: 'snapshot',
    ...snapshot,
    payload: Buffer.from(snapshot.payload),
    pendingCreation: snapshot.pendingCreation === undefined
      ? undefined
      : {
          ...snapshot.pendingCreation,
          requestSha256: Buffer.from(snapshot.pendingCreation.requestSha256)
        },
    storeVersion: { value: storeVersion.value } as ZLinkAuthorityStoreVersion,
    storeNow
  };
}

function liveTargetDescriptor(
  descriptorRead: ZLinkStoreReadResult,
  leaseRead: ZLinkStoreReadResult,
  target: {
    readonly meshName: string;
    readonly nodeRid: unknown;
    readonly nodeLifecycleGeneration: bigint;
    readonly owner: ZLinkLocationOwnerToken;
  }
): ZLinkMeshNodeDescriptor | undefined {
  if (descriptorRead.kind === 'missing' || leaseRead.kind === 'missing') return undefined;
  const descriptor = reviveMeshDescriptor(
    decodeJson<MeshRecord>(descriptorRead.value.bytes).descriptor
  );
  const lease = decodeJson<OwnerRecord>(leaseRead.value.bytes);
  return descriptor.meshName === target.meshName
    && String(descriptor.rid) === String(target.nodeRid)
    && descriptor.lifecycleGeneration === target.nodeLifecycleGeneration
    && descriptor.ownerId === target.owner.ownerId
    && descriptor.leaseGeneration === target.owner.leaseGeneration
    && descriptor.objectRole === ZLinkObjectRole.Server
    && descriptor.state === ZLinkFrameworkRuntimeState.Serving
    && lease.ownerId === target.owner.ownerId
    && BigInt(lease.leaseGeneration) === target.owner.leaseGeneration
    && leaseRead.value.expiresAt !== undefined
    && leaseRead.value.expiresAt.getTime() > leaseRead.value.storeNow.getTime()
      ? descriptor
      : undefined;
}

function conditionFor(
  key: ZLinkStoreKey,
  read: ZLinkStoreReadResult
): ZLinkStoreCondition {
  return read.kind === 'missing'
    ? { kind: 'missing', key }
    : { kind: 'version', key, expected: read.value.version };
}

function versionCondition(
  key: ZLinkStoreKey,
  read: ZLinkStoreReadResult
): ZLinkStoreCondition {
  if (read.kind === 'missing') {
    throw new Error(`Required provider row '${key}' is missing.`);
  }
  return { kind: 'version', key, expected: read.value.version };
}

function emptyCapacityRecord(): CapacityRecord {
  return {
    active: { actors: 0, spots: 0, spotTypes: {} },
    pending: { actors: 0, spots: 0, spotTypes: {} }
  };
}

function capacityAvailable(
  descriptor: ZLinkMeshNodeDescriptor,
  requested: ZLinkCapacityVector,
  capacity: CapacityRecord
): boolean {
  const spotType = requested.spotType;
  const typeCapacity = spotType === undefined
    ? undefined
    : descriptor.populationCapacity.spotTypes.find(candidate =>
        candidate.objectKind === spotType.objectKind
        && candidate.stableType === spotType.stableType);
  const typeKey = spotType === undefined ? undefined : capacityTypeKey(spotType);
  return capacity.active.actors + capacity.pending.actors + requested.actors
      <= descriptor.populationCapacity.actors.limit
    && capacity.active.spots + capacity.pending.spots + requested.spots
      <= descriptor.populationCapacity.spots.limit
    && (spotType === undefined
      || typeCapacity !== undefined
        && (typeCapacity.limit === 0
          || (capacity.active.spotTypes[typeKey!] ?? 0)
            + (capacity.pending.spotTypes[typeKey!] ?? 0)
            + spotType.count <= typeCapacity.limit));
}

function addCapacity(usage: CapacityUsage, delta: ZLinkCapacityVector): CapacityUsage {
  return adjustCapacity(usage, delta, 1);
}

function subtractCapacity(usage: CapacityUsage, delta: ZLinkCapacityVector): CapacityUsage {
  return adjustCapacity(usage, delta, -1);
}

function adjustCapacity(
  usage: CapacityUsage,
  delta: ZLinkCapacityVector,
  direction: 1 | -1
): CapacityUsage {
  const spotTypes = { ...usage.spotTypes };
  if (delta.spotType !== undefined) {
    const key = capacityTypeKey(delta.spotType);
    const next = (spotTypes[key] ?? 0) + direction * delta.spotType.count;
    if (next < 0) throw new Error('Provider capacity counter underflow.');
    if (next === 0) delete spotTypes[key];
    else spotTypes[key] = next;
  }
  const actors = usage.actors + direction * delta.actors;
  const spots = usage.spots + direction * delta.spots;
  if (actors < 0 || spots < 0) throw new Error('Provider capacity counter underflow.');
  return { actors, spots, spotTypes };
}

function capacityTypeKey(
  value: NonNullable<ZLinkCapacityVector['spotType']>
): string {
  return `${value.objectKind}\0${value.stableType}`;
}

function cloneCapacity(value: ZLinkCapacityVector): ZLinkCapacityVector {
  return {
    actors: value.actors,
    spots: value.spots,
    spotType: value.spotType === undefined ? undefined : { ...value.spotType }
  };
}

function relocationCapacityFence(value: string): ZLinkRelocationCapacityFence {
  return { value } as ZLinkRelocationCapacityFence;
}

function relocationTargetAllocation(
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

function sameRelocationAuthority(
  request: ZLinkRelocationCapacityReservationRequest,
  current: StoredAuthoritySnapshot,
  version: ZLinkStoreVersion,
  requireStoreVersion = true
): boolean {
  return (!requireStoreVersion || request.expectedStoreVersion.value === version.value)
    && current.allocation.state === 'active'
    && current.allocation.objectKind === request.objectKind
    && current.allocation.stableType === request.stableType
    && current.allocation.descriptor.meshName === request.sourceDescriptor.meshName
    && String(current.allocation.descriptor.rid) === String(request.sourceDescriptor.rid)
    && current.allocation.descriptorLifecycleGeneration === request.sourceNodeLifecycleGeneration
    && current.ownerId === request.sourceOwner.ownerId
    && current.ownerLeaseGeneration === request.sourceOwner.leaseGeneration;
}

function sameRelocationRequest(
  left: ZLinkRelocationCapacityReservationRequest,
  right: ZLinkRelocationCapacityReservationRequest
): boolean {
  return Buffer.from(encodeJson(left)).equals(Buffer.from(encodeJson(right)));
}

function sameAggregateCapacityReservation(
  reservation: ZLinkRelocationCapacityReservationRequest,
  aggregate: ZLinkAggregatePrepareRequest
): boolean {
  return reservation.reservationId === aggregate.aggregateId.value
    && reservation.targetOwner.ownerId === aggregate.targetOwner.ownerId
    && reservation.targetOwner.leaseGeneration
      === aggregate.targetOwner.leaseGeneration
    && reservation.targetDescriptor.meshName
      === aggregate.targetDescriptor.meshName
    && String(reservation.targetDescriptor.rid)
      === String(aggregate.targetDescriptor.rid)
    && reservation.targetNodeLifecycleGeneration
      === aggregate.targetDescriptorLifecycleGeneration
    && sameCapacityVector(reservation.capacity, aggregate.capacity);
}

function sameCapacityVector(left: ZLinkCapacityVector, right: ZLinkCapacityVector): boolean {
  return left.actors === right.actors
    && left.spots === right.spots
    && left.spotType?.objectKind === right.spotType?.objectKind
    && left.spotType?.stableType === right.spotType?.stableType
    && left.spotType?.count === right.spotType?.count;
}

function sameCreationTarget(
  snapshot: StoredAuthoritySnapshot,
  target: ZLinkObjectCommitRequest['target']
): boolean {
  return snapshot.allocation.descriptor.meshName === target.meshName
    && String(snapshot.allocation.descriptor.rid) === String(target.nodeRid)
    && snapshot.allocation.descriptorLifecycleGeneration
      === target.nodeLifecycleGeneration
    && snapshot.ownerId === target.owner.ownerId
    && snapshot.ownerLeaseGeneration === target.owner.leaseGeneration;
}

function sameLiveOwner(
  lease: ZLinkStoreReadResult,
  snapshot: StoredAuthoritySnapshot
): boolean {
  if (liveOwnerLeaseGeneration(lease) !== snapshot.ownerLeaseGeneration) return false;
  const owner = decodeJson<OwnerRecord>((lease as Extract<ZLinkStoreReadResult, { kind: 'found' }>).value.bytes);
  return owner.ownerId === snapshot.ownerId;
}

function liveOwnerLeaseGeneration(
  lease: ZLinkStoreReadResult
): bigint | undefined {
  if (lease.kind === 'missing'
    || lease.value.expiresAt === undefined
    || lease.value.expiresAt.getTime() <= lease.value.storeNow.getTime()) {
    return undefined;
  }
  return BigInt(decodeJson<OwnerRecord>(lease.value.bytes).leaseGeneration);
}

function canTakeOverStoredLocation(
  intent: ZLinkLocationWriteIntent,
  currentOwnerId: string,
  currentLeaseGeneration: bigint,
  nextOwnerId: string,
  nextLeaseGeneration: bigint,
  liveLeaseGeneration: bigint | undefined
): boolean {
  if (intent === ZLinkLocationWriteIntent.Takeover) {
    // Takeover is an explicit handoff operation. The provider CAS still
    // fences concurrent stale writes while the next owner lease is checked.
    return true;
  }
  if (intent !== ZLinkLocationWriteIntent.NewClaim) {
    return false;
  }
  if (liveLeaseGeneration === undefined) return true;
  return currentOwnerId === nextOwnerId
    && currentLeaseGeneration !== nextLeaseGeneration
    && liveLeaseGeneration === nextLeaseGeneration;
}

function persistMeshDescriptor(descriptor: ZLinkMeshNodeDescriptor): ZLinkMeshNodeDescriptor {
  return { ...descriptor, rid: String(descriptor.rid), updatedAt: descriptor.updatedAt.toISOString() as never };
}

function reviveMeshDescriptor(descriptor: ZLinkMeshNodeDescriptor): ZLinkMeshNodeDescriptor {
  return {
    ...descriptor,
    rid: String(descriptor.rid),
    updatedAt: descriptor.updatedAt instanceof Date
      ? descriptor.updatedAt
      : new Date(descriptor.updatedAt)
  };
}

function reviveClientServerDescriptor(
  descriptor: ZLinkClientServerServerDescriptor
): ZLinkClientServerServerDescriptor {
  return {
    ...descriptor,
    serverRid: String(descriptor.serverRid),
    updatedAt: reviveDate(descriptor.updatedAt)
  };
}

function reviveFanoutDescriptor(
  descriptor: ZLinkFanoutPublisherDescriptor
): ZLinkFanoutPublisherDescriptor {
  return {
    ...descriptor,
    publisherRid: String(descriptor.publisherRid),
    updatedAt: reviveDate(descriptor.updatedAt)
  };
}

function reviveDate(value: Date): Date {
  return value instanceof Date ? value : new Date(value);
}

function normalizeSpot(value: ZLinkSpotLocation): ZLinkSpotLocation {
  return {
    ...value,
    spotId: String(value.spotId),
    ownerNodeRid: String(value.ownerNodeRid),
    updatedAt: reviveDate(value.updatedAt)
  };
}

function normalizeActor(value: ZLinkActorLocation): ZLinkActorLocation {
  return {
    ...value,
    actorRef: {
      ...value.actorRef,
      nodeRid: String(value.actorRef.nodeRid)
    },
    ownerNodeRid: String(value.ownerNodeRid),
    spotId: String(value.spotId),
    updatedAt: reviveDate(value.updatedAt)
  };
}

function normalizeRoute(value: ZLinkRouteLocation): ZLinkRouteLocation {
  return {
    ...value,
    ownerNodeRid: String(value.ownerNodeRid),
    value: Buffer.from(value.value),
    updatedAt: reviveDate(value.updatedAt)
  };
}

function matchesSpot(value: ZLinkSpotLocation, filter: ZLinkSpotLocationFilter): boolean {
  return (filter.meshName === undefined || value.meshName === filter.meshName)
    && (filter.spotType === undefined || value.spotType === filter.spotType)
    && (filter.nodeRid === undefined || String(value.ownerNodeRid) === String(filter.nodeRid))
    && (filter.spotKind === undefined || value.spotKind === filter.spotKind);
}

function matchesActor(value: ZLinkActorLocation, filter: ZLinkActorLocationFilter): boolean {
  return (filter.actorType === undefined || value.actorType === filter.actorType)
    && (filter.nodeRid === undefined || String(value.ownerNodeRid) === String(filter.nodeRid))
    && (filter.spotId === undefined || String(value.spotId) === String(filter.spotId))
    && (filter.locationKind === undefined || value.spotKind === filter.locationKind);
}

function matchesRoute(value: ZLinkRouteLocation, filter: ZLinkRouteLocationFilter): boolean {
  return (filter.routeKind === undefined || value.routeKind === filter.routeKind)
    && (filter.ownerNodeRid === undefined
      || String(value.ownerNodeRid) === String(filter.ownerNodeRid))
    && (filter.ownerId === undefined || value.ownerId === filter.ownerId);
}

function locationOwnerGeneration(value: LeaseOwnedLocation): bigint {
  return value.leaseGeneration;
}

function descriptorOwnerLeaseGeneration(value: OwnedStoreRecord): bigint | undefined {
  if ('leaseGeneration' in value) {
    return typeof value.leaseGeneration === 'bigint'
      ? value.leaseGeneration
      : BigInt(value.leaseGeneration);
  }
  if ('generation' in value) {
    return typeof value.generation === 'bigint'
      ? value.generation
      : BigInt(value.generation);
  }
  return undefined;
}

function sameMeshDescriptor(
  left: ZLinkMeshNodeDescriptor,
  right: ZLinkMeshNodeDescriptor
): boolean {
  return Buffer.from(encodeJson(persistMeshDescriptor(left))).equals(
    Buffer.from(encodeJson(persistMeshDescriptor(right))));
}

function sameClientServerDescriptor(
  left: ZLinkClientServerServerDescriptor,
  right: ZLinkClientServerServerDescriptor
): boolean {
  return descriptorFingerprint(left, ['updatedAt']) === descriptorFingerprint(right, ['updatedAt']);
}

function sameFanoutDescriptor(
  left: ZLinkFanoutPublisherDescriptor,
  right: ZLinkFanoutPublisherDescriptor
): boolean {
  return descriptorFingerprint(left, ['updatedAt']) === descriptorFingerprint(right, ['updatedAt']);
}

function canRenewClientServer(
  current: ZLinkClientServerServerDescriptor,
  next: ZLinkClientServerServerDescriptor
): boolean {
  return sameOwnerGeneration(current, next)
    && next.descriptorRevision > current.descriptorRevision
    && descriptorFingerprint(current, ['descriptorRevision', 'weight', 'state', 'updatedAt'])
      === descriptorFingerprint(next, ['descriptorRevision', 'weight', 'state', 'updatedAt']);
}

function canRenewFanout(
  current: ZLinkFanoutPublisherDescriptor,
  next: ZLinkFanoutPublisherDescriptor
): boolean {
  return sameOwnerGeneration(current, next)
    && next.descriptorRevision > current.descriptorRevision
    && descriptorFingerprint(current, ['descriptorRevision', 'state', 'updatedAt'])
      === descriptorFingerprint(next, ['descriptorRevision', 'state', 'updatedAt']);
}

function sameOwnerGeneration(left: OwnedDescriptor, right: OwnedDescriptor): boolean {
  return left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration
    && left.lifecycleGeneration === right.lifecycleGeneration;
}

function descriptorFingerprint(
  descriptor: OwnedDescriptor,
  omitted: readonly string[]
): string {
  const copy = { ...descriptor } as Record<string, unknown>;
  for (const key of omitted) delete copy[key];
  return Buffer.from(encodeJson(copy)).toString('base64');
}

function validateClientServerDescriptor(descriptor: ZLinkClientServerServerDescriptor): void {
  validateDescriptorIdentity([
    descriptor.channelName,
    String(descriptor.serverRid),
    descriptor.endpoint,
    descriptor.securityIdentity,
    descriptor.ownerId
  ], 'ClientServer');
  validateDescriptorGenerations(descriptor, 'ClientServer');
  if (!Number.isInteger(descriptor.weight) || descriptor.weight < 0 || descriptor.weight > 10_000) {
    throw new RangeError('ClientServer descriptor weight must be an integer in 0..10000.');
  }
}

function validateFanoutPublisherDescriptor(descriptor: ZLinkFanoutPublisherDescriptor): void {
  validateDescriptorIdentity([
    descriptor.channelName,
    String(descriptor.publisherRid),
    descriptor.endpoint,
    descriptor.securityIdentity,
    descriptor.ownerId
  ], 'Fanout publisher');
  validateDescriptorGenerations(descriptor, 'Fanout publisher');
}

function validateDescriptorIdentity(values: readonly string[], kind: string): void {
  if (values.some(value => Buffer.byteLength(value, 'utf8') < 1 || value.includes('\0'))) {
    throw new TypeError(`${kind} descriptor identity and endpoint are required.`);
  }
}

function validateDescriptorGenerations(descriptor: OwnedDescriptor, kind: string): void {
  if (descriptor.lifecycleGeneration < 1n || descriptor.lifecycleGeneration > MAX_GENERATION
    || descriptor.descriptorRevision < 1n || descriptor.descriptorRevision > MAX_GENERATION
    || descriptor.leaseGeneration < 1n || descriptor.leaseGeneration > MAX_GENERATION) {
    throw new RangeError(`${kind} descriptor generations are invalid.`);
  }
}

function rejected(updatedAt: Date): ZLinkLocationWriteResult {
  return {
    status: WriteStatus.RejectedConflict,
    generation: 0n,
    updatedAt
  };
}

function requireOwnerInput(ownerId: string, leaseTtlMs: number): void {
  if (Buffer.byteLength(ownerId, 'utf8') < 1 || ownerId.includes('\0')) {
    throw new TypeError('Owner ID must be non-empty UTF-8 without NUL.');
  }
  if (!Number.isSafeInteger(leaseTtlMs) || leaseTtlMs < 1) {
    throw new RangeError('Owner lease TTL must be a positive safe integer.');
  }
}

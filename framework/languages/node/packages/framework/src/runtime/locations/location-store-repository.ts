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
  ZLinkPlacementAllocation
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
import type { RoutingId } from '../../contracts/Common/CoreTypes';
import { decodeRoutingId, encodeRoutingIdStorageHex } from '../routing-id';

const PREFIX = 'zlink:v11:';
const OWNER_COUNTER_KEY = storeKey(`${PREFIX}owner-counter`);
// These counters are store-wide fences.  They deliberately are not derived
// from an authority identity: deleting and recreating an authority must never
// reuse an incarnation or owner-transition generation.
const OBJECT_COUNTER_KEY = storeKey(`${PREFIX}object-counter`);
const AUTHORITY_OWNER_COUNTER_KEY = storeKey(`${PREFIX}authority-owner-counter`);
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
}

interface OwnerRecord {
  readonly recordVersion: 1;
  readonly ownerId: string;
  readonly leaseGeneration: string;
}

interface DescriptorRecord<T> {
  readonly generation: string;
  readonly descriptor: T;
}

interface CanonicalDescriptorRecord<T> {
  readonly recordVersion: 1;
  /** Legacy Node-private row generation.  Canonical v1 records omit this. */
  readonly generation?: string;
  readonly ownerId: string;
  readonly leaseGeneration: string;
  readonly descriptorRevision: string;
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
      decodeAuthorityRecord(result.value.bytes),
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
      prefix: AUTHORITY_PREIMAGE_PREFIX,
      cursor: cursor === undefined
        ? undefined
        : ({ value: cursor.encoded } as ZLinkStoreScanCursor),
      limit
    }, signal);
    if (scan.kind === 'expired') return { kind: 'scanExpired' };
    const items = [];
    for (const item of scan.value.items) {
      const encodedKey = authorityContractValueFromPreimage(item.key.value);
      if (!encodedKey.startsWith(prefix)) continue;
      const snapshot = await this.projectAuthority(
        decodeAuthorityRecord(item.value.bytes),
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
      const record = decodeAuthorityRecord(current.value.bytes);
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
          record.snapshot.allocation.descriptor.rid
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
          mutations: [{ kind: 'put', key: rowKey, bytes: encodeAuthorityRecord(nextRecord) }]
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
          : [{ kind: 'put', key: rowKey, bytes: encodeAuthorityRecord(nextRecord!) }]
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
        request.targetDescriptor.rid
      );
      const targetLeaseKey = ownerKey(request.targetOwner.ownerId);
      const targetCapacityKey = capacityKey(
        request.targetDescriptor.meshName,
        request.targetDescriptor.rid
      );
      const [
        descriptorRead,
        leaseRead,
        targetCapacityRead
      ] = await Promise.all([
        this.provider.read(targetDescriptorKey, signal),
        this.provider.read(targetLeaseKey, signal),
        this.provider.read(targetCapacityKey, signal)
      ]);
      const descriptor = liveTargetDescriptor(
        descriptorRead,
        leaseRead,
        aggregateTarget(request)
      );
      const targetCapacity = targetCapacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(targetCapacityRead.value.bytes);
      if (
        descriptor === undefined
        || !capacityAvailable(descriptor, request.capacity, targetCapacity)
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
        const record = decodeAuthorityRecord(current.value.bytes);
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
        const targetAuthorityOwnerGeneration = participant.ownerTransition === 'newOwner'
          ? record.snapshot.authorityOwnerGeneration + 1n
          : record.snapshot.authorityOwnerGeneration;
        const marker: AggregateParticipantFenceRecord = {
            aggregateId: fence.aggregateId.value,
            aggregateGeneration: fence.aggregateGeneration,
            index,
            expectedStoreVersion: participant.expectedStoreVersion.value,
            ownerTransition: participant.ownerTransition,
            targetAuthorityOwnerGeneration,
            authorityPayloadSha256: entry.authorityPayloadSha256,
            membershipMutationSha256: entry.membershipMutationSha256
          };
        const stored = await this.provider.write({
          conditions: [{ kind: 'version', key: authorityRowKey, expected: current.value.version }],
          mutations: [{
            kind: 'put', key: authorityRowKey, bytes: encodeAuthorityRecord({
              ...record,
              aggregate: marker,
              visibleStoreVersion:
                record.visibleStoreVersion ?? current.value.version.value
            } satisfies AuthorityRecord)
          }]
        }, signal);
        if (stored.kind === 'conflict') {
          const raced = await this.provider.read(authorityRowKey, signal);
          if (raced.kind === 'found' && sameAggregateMarker(
            decodeAuthorityRecord(raced.value.bytes).aggregate, fence, index, participant
          )) {
            installed.push({ key: authorityRowKey, expectedStoreVersion: participant.expectedStoreVersion.value });
            continue;
          }
          // A peer can be in the middle of publishing this same aggregate.
          // Do not abort its shared staging row merely because this attempt
          // lost a marker CAS; re-enter and adopt its terminal state.
          const aggregateRaced = await this.provider.read(rowKey, signal);
          if (aggregateRaced.kind === 'found') {
            const aggregateRecord = decodeJson<AggregateRecord>(aggregateRaced.value.bytes);
            if (aggregateRecord.requestFingerprint === fingerprint
              && (aggregateRecord.state === 'staging' || aggregateRecord.state === 'prepared')) {
              return await this.prepareAggregate(request, signal);
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
          conditionFor(targetCapacityKey, targetCapacityRead)
        ],
        mutations: [
          {
            kind: 'put',
            key: rowKey,
            bytes: encodeJson({ ...latest, state: 'prepared' } satisfies AggregateRecord)
          },
          {
            kind: 'put',
            key: targetCapacityKey,
            bytes: encodeJson({
              active: targetCapacity.active,
              pending: addCapacity(targetCapacity.pending, request.capacity)
            } satisfies CapacityRecord)
          }
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
      const record = decodeAuthorityRecord(current.value.bytes);
      if (!sameAggregateMarkerEntry(record.aggregate, fence, index, entry)) {
        // A competing committer may already have published the aggregate and
        // normalized this participant before this reader reached it.  The
        // committed aggregate is the terminal outcome to adopt, not a stale
        // failure caused by observing the post-normalization row.
        const latest = await this.provider.read(rowKey, signal);
        if (latest.kind === 'found') {
          const latestAggregate = decodeJson<AggregateRecord>(latest.value.bytes);
          if (latestAggregate.state === 'committed') {
            await this.normalizeCommittedAggregate(fence, latestAggregate, signal);
            return { kind: 'alreadyCommitted' };
          }
        }
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
      aggregate.targetDescriptor.rid
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
    const published = await this.provider.write({
      conditions: [
        { kind: 'version', key: rowKey, expected: aggregateRead.value.version },
        versionCondition(targetDescriptorKey, descriptorRead),
        versionCondition(targetLeaseKey, leaseRead),
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
      const descriptorKey = meshKey(request.target.meshName, request.target.nodeRid);
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(
        request.target.meshName,
        String(request.target.nodeRid)
      );
      const [
        current,
        descriptorRead,
        leaseRead,
        capacityRead,
        objectGenerationRead,
        authorityOwnerGenerationRead
      ] =
        await Promise.all([
          this.provider.read(rowKey, signal),
          this.provider.read(descriptorKey, signal),
          this.provider.read(leaseKey, signal),
          this.provider.read(capacityRowKey, signal),
          this.provider.read(OBJECT_COUNTER_KEY, signal),
          this.provider.read(AUTHORITY_OWNER_COUNTER_KEY, signal)
        ]);
      if (current.kind === 'found') {
        const record = decodeAuthorityRecord(current.value.bytes);
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
      const generation = objectGenerationRead.kind === 'missing'
        ? 1n
        : BigInt(decodeText(objectGenerationRead.value.bytes));
      const authorityOwnerGeneration = authorityOwnerGenerationRead.kind === 'missing'
        ? 1n
        : BigInt(decodeText(authorityOwnerGenerationRead.value.bytes));
      // Counters retain the next value to issue.  Issuing MAX_GENERATION
      // would require persisting MAX_GENERATION + 1, which is outside the
      // contract's valid stored range, so it fails before this batch mutates.
      if (generation >= MAX_GENERATION || authorityOwnerGeneration >= MAX_GENERATION) {
        return { kind: 'generationExhausted' };
      }
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
          authorityOwnerGeneration,
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
          conditionFor(OBJECT_COUNTER_KEY, objectGenerationRead),
          conditionFor(AUTHORITY_OWNER_COUNTER_KEY, authorityOwnerGenerationRead)
        ],
        mutations: [
          { kind: 'put', key: rowKey, bytes: encodeAuthorityRecord(record) },
          {
            kind: 'put',
            key: capacityRowKey,
            bytes: encodeJson({
              active: capacity.active,
              pending: addCapacity(capacity.pending, request.capacity)
            } satisfies CapacityRecord)
          },
          {
            kind: 'put',
            key: OBJECT_COUNTER_KEY,
            bytes: encodeText((generation + 1n).toString())
          },
          {
            kind: 'put',
            key: AUTHORITY_OWNER_COUNTER_KEY,
            bytes: encodeText((authorityOwnerGeneration + 1n).toString())
          }
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
      const record = decodeAuthorityRecord(current.value.bytes);
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
      const descriptorKey = meshKey(request.target.meshName, request.target.nodeRid);
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(request.target.meshName, request.target.nodeRid);
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
          { kind: 'put', key: rowKey, bytes: encodeAuthorityRecord(ready) },
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
      const record = decodeAuthorityRecord(current.value.bytes);
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
      const record = decodeAuthorityRecord(current.value.bytes);
      if (
        record.reservationId !== request.reservationId
        || current.value.version.value !== request.expectedStoreVersion
        || record.snapshot.allocation.state !== 'reserved'
        || !sameCreationTarget(record.snapshot, request.target)
      ) {
        return { kind: 'stale' };
      }
      const descriptorKey = meshKey(request.target.meshName, request.target.nodeRid);
      const leaseKey = ownerKey(request.target.owner.ownerId);
      const capacityRowKey = capacityKey(request.target.meshName, request.target.nodeRid);
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
              bytes: encodeAuthorityRecord({
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
      const readyRecord = decodeAuthorityRecord(
        encodeAuthorityRecord({
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
      // Counters retain the next value to issue. Issuing MAX_GENERATION would
      // require persisting MAX_GENERATION + 1, outside the stored range.
      if (generation >= MAX_GENERATION) return { kind: 'generationExhausted' };
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
            bytes: encodeOwnerRecord(ownerId, generation),
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
    const record = decodeOwnerRecord(result.value.bytes);
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
    const record = decodeOwnerRecord(current.value.bytes);
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
    const record = decodeOwnerRecord(current.value.bytes);
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
    const rowKey = meshKey(descriptor.meshName, descriptor.rid);
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
      const record = decodeCanonicalDescriptorRecord<ZLinkMeshNodeDescriptor>(current.value.bytes);
      const stored = reviveMeshDescriptor(record.descriptor);
      generation = descriptorStoreGeneration(record, current.value.version.value);
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
        bytes: encodeCanonicalDescriptorRecord(generation, persistMeshDescriptor(descriptor))
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
    const rowKey = meshKey(key.meshName, key.rid);
    const current = await this.provider.read(rowKey, signal);
    if (current.kind === 'missing') return WriteStatus.IgnoredStale;
    const descriptor = reviveMeshDescriptor(decodeCanonicalDescriptorRecord<ZLinkMeshNodeDescriptor>(current.value.bytes).descriptor);
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
    const items = await Promise.all(result.value.items.map(async item => {
      const descriptor = reviveMeshDescriptor(
        decodeCanonicalDescriptorRecord<ZLinkMeshNodeDescriptor>(item.value.bytes).descriptor
      );
      const capacityRead = await this.provider.read(
        capacityKey(descriptor.meshName, String(descriptor.rid)),
        signal
      );
      const capacity = capacityRead.kind === 'missing'
        ? emptyCapacityRecord()
        : decodeJson<CapacityRecord>(capacityRead.value.bytes);
      return {
        ...descriptor,
        populationCapacity: {
          actors: {
            ...descriptor.populationCapacity.actors,
            active: capacity.active.actors,
            reserved: capacity.pending.actors
          },
          spots: {
            ...descriptor.populationCapacity.spots,
            active: capacity.active.spots,
            reserved: capacity.pending.spots
          },
          spotTypes: descriptor.populationCapacity.spotTypes.map(value => {
            const key = capacityTypeKey({
              objectKind: value.objectKind,
              stableType: value.stableType,
              count: 0
            });
            return {
              ...value,
              active: capacity.active.spotTypes[key] ?? 0,
              reserved: capacity.pending.spotTypes[key] ?? 0
            };
          })
        }
      };
    }));
    return {
      items,
      continuationToken: result.value.nextCursor?.value
    };
  }

  override async updateClientServer(
    descriptor: ZLinkClientServerServerDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    validateClientServerDescriptor(descriptor);
    const normalized = reviveClientServerDescriptor(descriptor);
    return this.updateDescriptor(
      clientServerKey(normalized.channelName, normalized.serverRid),
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
      clientServerKey(key.channelName, key.serverRid),
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
    const normalized = reviveFanoutDescriptor(descriptor);
    return this.updateDescriptor(
      fanoutKey(normalized.channelName, normalized.publisherRid),
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
      fanoutKey(key.channelName, key.publisherRid),
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
      'mesh-node\0',
      'client-server\0',
      'fanout-publisher\0',
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
          prefix: AUTHORITY_PREIMAGE_PREFIX,
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
      const record = decodeAuthorityRecord(current.value.bytes);
      if (
        record.snapshot.ownerId !== owner.ownerId
        || record.snapshot.ownerLeaseGeneration !== owner.leaseGeneration
        || record.aggregate !== undefined
      ) continue;
      const encodedAuthority = authorityContractValueFromPreimage(candidate.key.value);
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
      const record = decodeCanonicalDescriptorRecord<T>(current.value.bytes);
      const stored = revive(record.descriptor);
      generation = descriptorStoreGeneration(record, current.value.version.value);
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
        bytes: encodeCanonicalDescriptorRecord(generation, descriptor)
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
      decodeCanonicalDescriptorRecord<T>(current.value.bytes).descriptor);
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
        revive(decodeCanonicalDescriptorRecord<T>(item.value.bytes).descriptor)),
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
        const record = decodeAuthorityRecord(current.value.bytes);
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
            bytes: encodeAuthorityRecord(normalized)
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
        prefix: AUTHORITY_PREIMAGE_PREFIX,
        cursor,
        limit: 1_000
      }, signal);
      if (result.kind === 'expired') {
        cursor = undefined;
        continue;
      }
      const matching = result.value.items.filter(item => {
        const record = decodeAuthorityRecord(item.value.bytes);
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
      const record = decodeAuthorityRecord(current.value.bytes);
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
          bytes: encodeAuthorityRecord({ ...record, aggregate: undefined } satisfies AuthorityRecord)
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

// Logical key preimages (21-location-runtime.md#2.4). Values are joined with
// literal NUL bytes -- the same scheme the opaque record store hashes with
// SHA-256 to derive the Redis key -- so a runtime in one language can read a
// record another language wrote. Not used for framework-internal-only rows
// (aggregate/capacity/creation-terminal/spot/actor/route), which stay on the
// legacy PREFIX scheme since they aren't part of the public opaque contract.
function ownerKey(ownerId: string) {
  return storeKey(`owner-lease\0${requireNoNul(ownerId, 'OwnerId')}`);
}

const AUTHORITY_PREIMAGE_PREFIX = 'authority\0';

function authorityKey(value: string) {
  return storeKey(authorityPreimage(value));
}

function authorityPreimage(value: string): string {
  const decoded = decodeAuthorityKey({ value } as ZLinkAuthorityKey);
  // §2.3: Entry|User|Instance Spot kinds share one segment ("spot") -- one
  // Id has exactly one authority row regardless of spot kind.
  const segment = decoded.kind === 'actor' ? 'actor' : 'spot';
  return `${AUTHORITY_PREIMAGE_PREFIX}${segment}\0${requireNoNul(decoded.globalId, 'Authority Id')}`;
}

// Reverses authorityPreimage(): recovers the framework-internal zla1-encoded
// ZLinkAuthorityKey contract value from a scanned opaque record's logical
// key. Used by the authority list/cleanup scans below, which see raw
// preimages (authority\0actor\0{id} | authority\0spot\0{id}) and must hand
// callers back the same encodeAuthorityKey() representation authorityKey()
// was given.
function authorityContractValueFromPreimage(logicalKey: string): string {
  if (!logicalKey.startsWith(AUTHORITY_PREIMAGE_PREFIX)) {
    throw new Error('Authority scan returned a non-authority logical key.');
  }
  const rest = logicalKey.slice(AUTHORITY_PREIMAGE_PREFIX.length);
  const separator = rest.indexOf('\0');
  if (separator < 0) throw new Error('Authority logical key preimage is malformed.');
  const segment = rest.slice(0, separator);
  const globalId = rest.slice(separator + 1);
  return encodeAuthorityKey(segment === 'actor' ? 'actor' : 'user_spot', globalId).value;
}

function requireNoNul(value: string, name: string): string {
  if (value.includes('\0')) {
    throw new TypeError(`${name} must not contain a NUL byte.`);
  }
  return value;
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

function capacityKey(meshName: string, nodeRid: string) {
  return storeKey(
    `${PREFIX}capacity:${encodeURIComponent(meshName)}:${encodeURIComponent(nodeRid)}`
  );
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
  return `mesh-node\0${requireNoNul(meshName, 'MeshName')}\0`;
}

function meshKey(meshName: string, nodeRid: RoutingId) {
  return storeKey(`${meshPrefix(meshName)}${routingIdHexSegment(nodeRid)}`);
}

function clientServerPrefix(channelName: string): string {
  return `client-server\0${requireNoNul(channelName, 'ChannelName')}\0`;
}

function clientServerKey(channelName: string, serverRid: RoutingId) {
  return storeKey(`${clientServerPrefix(channelName)}${routingIdHexSegment(serverRid)}`);
}

// {hex(RoutingId)}: lowercase hex of the RoutingId's raw bytes
// (21-location-runtime.md#2.4).
function routingIdHexSegment(rid: RoutingId): string {
  return encodeRoutingIdStorageHex(rid).toLowerCase();
}

function fanoutPrefix(channelName: string): string {
  return `fanout-publisher\0${requireNoNul(channelName, 'ChannelName')}\0`;
}

function fanoutKey(channelName: string, publisherRid: RoutingId) {
  return storeKey(`${fanoutPrefix(channelName)}${routingIdHexSegment(publisherRid)}`);
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

function requireRecordVersion(value: unknown, kind: string): void {
  if (value === null || typeof value !== 'object'
    || (value as { recordVersion?: unknown }).recordVersion !== 1) {
    throw new Error(`Location Store ${kind} record has an unrecognized recordVersion.`);
  }
}

function canonicalize(value: unknown): unknown {
  if (typeof value === 'bigint') return value.toString();
  if (value instanceof Uint8Array) return Buffer.from(value).toString('base64');
  if (value instanceof Date) return value.toISOString();
  if (Array.isArray(value)) return value.map(canonicalize);
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, canonicalize(item)]));
  }
  return value;
}

function reviveCanonical(value: unknown, key = ''): unknown {
  const generationKeys = new Set([
    'generation', 'leaseGeneration', 'ownerLeaseGeneration', 'objectGeneration',
    'authorityOwnerGeneration', 'aggregateGeneration', 'lifecycleGeneration', 'descriptorRevision',
    'nodeGeneration', 'applicationVersion', 'requestEncodedSize'
  ]);
  if (typeof value === 'string' && generationKeys.has(key)) return BigInt(value);
  if (Array.isArray(value)) return value.map(item => reviveCanonical(item));
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([name, item]) => [name, reviveCanonical(item, name)]));
  }
  return value;
}

function decodeOwnerRecord(bytes: Uint8Array): OwnerRecord {
  const record = decodeJson<OwnerRecord>(bytes);
  requireRecordVersion(record, 'owner lease');
  return record;
}

function encodeOwnerRecord(ownerId: string, leaseGeneration: bigint): Uint8Array {
  return encodeJson<OwnerRecord>({ recordVersion: 1, ownerId, leaseGeneration: leaseGeneration.toString() });
}

function encodeCanonicalDescriptorRecord<T extends OwnedDescriptor>(
  generation: bigint,
  descriptor: T
): Uint8Array {
  // `generation` is an old Node-local row counter.  The opaque record's
  // cmsgpack version is the interoperable row version, so it is deliberately
  // not serialized (21-location-runtime §2.4).
  void generation;
  return Buffer.from(JSON.stringify({
    recordVersion: 1,
    ownerId: descriptor.ownerId,
    leaseGeneration: descriptor.leaseGeneration.toString(),
    descriptorRevision: descriptor.descriptorRevision.toString(),
    descriptor: canonicalDescriptor(descriptor)
  } satisfies CanonicalDescriptorRecord<unknown>), 'utf8');
}

function decodeCanonicalDescriptorRecord<T extends OwnedDescriptor>(bytes: Uint8Array): CanonicalDescriptorRecord<T> {
  const record = JSON.parse(decodeText(bytes)) as CanonicalDescriptorRecord<unknown>;
  requireRecordVersion(record, 'descriptor');
  return reviveCanonical(record) as CanonicalDescriptorRecord<T>;
}

function descriptorStoreGeneration(
  record: CanonicalDescriptorRecord<unknown>,
  storeVersion: string
): bigint {
  return record.generation === undefined ? BigInt(storeVersion) : BigInt(record.generation);
}

function canonicalDescriptor(descriptor: OwnedDescriptor): Record<string, unknown> {
  if ('rid' in descriptor) return canonicalMeshDescriptor(descriptor);
  if ('serverRid' in descriptor) return {
    channelName: descriptor.channelName,
    serverRoutingIdHex: encodeRoutingIdStorageHex(descriptor.serverRid),
    lifecycleGeneration: descriptor.lifecycleGeneration.toString(),
    descriptorRevision: descriptor.descriptorRevision.toString(),
    endpoint: descriptor.endpoint,
    weight: descriptor.weight,
    state: canonicalRuntimeState(descriptor.state),
    securityIdentity: descriptor.securityIdentity,
    ownerId: descriptor.ownerId,
    leaseGeneration: descriptor.leaseGeneration.toString(),
    updatedAtEpochMs: descriptor.updatedAt.getTime().toString()
  };
  const publisher = descriptor as ZLinkFanoutPublisherDescriptor;
  return {
    channelName: publisher.channelName,
    publisherRoutingIdHex: encodeRoutingIdStorageHex(publisher.publisherRid),
    lifecycleGeneration: publisher.lifecycleGeneration.toString(),
    descriptorRevision: publisher.descriptorRevision.toString(),
    endpoint: publisher.endpoint,
    state: canonicalRuntimeState(publisher.state),
    securityIdentity: publisher.securityIdentity,
    ownerId: publisher.ownerId,
    leaseGeneration: publisher.leaseGeneration.toString(),
    updatedAtEpochMs: publisher.updatedAt.getTime().toString()
  };
}

function canonicalMeshDescriptor(descriptor: ZLinkMeshNodeDescriptor): Record<string, unknown> {
  return {
    meshName: descriptor.meshName,
    routingIdHex: encodeRoutingIdStorageHex(descriptor.rid),
    lifecycleGeneration: descriptor.lifecycleGeneration.toString(),
    descriptorRevision: descriptor.descriptorRevision.toString(),
    endpoint: descriptor.endpoint,
    entrySpotId: descriptor.entrySpotId ?? null,
    channelWeights: descriptor.channelWeights,
    applicationVersion: descriptor.applicationVersion.toString(),
    objectCapabilities: descriptor.objectCapabilities.map(capability => ({
      ...capability,
      objectKind: canonicalObjectKind(capability.objectKind),
      policy: capability.policy === 'disabled' ? 'disabled' : capability.policy
    })),
    objectRole: descriptor.objectRole,
    placementWeight: descriptor.placementWeight,
    capacity: {
      actors: descriptor.populationCapacity.actors,
      spots: descriptor.populationCapacity.spots,
      spotTypes: descriptor.populationCapacity.spotTypes.map(capacity => ({
        ...capacity,
        objectKind: canonicalObjectKind(capacity.objectKind)
      }))
    },
    activationConcurrency: descriptor.activationConcurrency,
    maintenanceWave: descriptor.maintenanceWave ?? null,
    state: canonicalRuntimeState(descriptor.state),
    securityIdentity: descriptor.securityIdentity,
    ownerId: descriptor.ownerId,
    leaseGeneration: descriptor.leaseGeneration.toString(),
    updatedAtEpochMs: descriptor.updatedAt.getTime().toString()
  };
}

function reviveCanonicalCapacity(value: Record<string, unknown>): ZLinkMeshNodeDescriptor['populationCapacity'] {
  return {
    actors: value.actors as ZLinkMeshNodeDescriptor['populationCapacity']['actors'],
    spots: value.spots as ZLinkMeshNodeDescriptor['populationCapacity']['spots'],
    spotTypes: (value.spotTypes as readonly Record<string, unknown>[]).map(capacity => ({
      ...capacity,
      objectKind: runtimeSpotObjectKind(String(capacity.objectKind))
    })) as unknown as ZLinkMeshNodeDescriptor['populationCapacity']['spotTypes']
  };
}

function reviveCanonicalCapabilities(value: unknown): ZLinkMeshNodeDescriptor['objectCapabilities'] {
  return (value as readonly Record<string, unknown>[]).map(capability => ({
    ...capability,
    objectKind: runtimeObjectKind(String(capability.objectKind))
  })) as unknown as ZLinkMeshNodeDescriptor['objectCapabilities'];
}

function canonicalObjectKind(kind: 'actor' | 'user_spot' | 'instance_spot'): string {
  return kind === 'user_spot' ? 'userSpot' : kind === 'instance_spot' ? 'instanceSpot' : 'actor';
}

function runtimeObjectKind(kind: string): 'actor' | 'user_spot' | 'instance_spot' {
  if (kind === 'actor') return 'actor';
  if (kind === 'userSpot') return 'user_spot';
  if (kind === 'instanceSpot') return 'instance_spot';
  throw new TypeError(`Location Store descriptor has invalid objectKind '${kind}'.`);
}

function runtimeSpotObjectKind(kind: string): 'user_spot' | 'instance_spot' {
  const result = runtimeObjectKind(kind);
  if (result === 'actor') throw new TypeError('Location Store spot capacity cannot have actor objectKind.');
  return result;
}

function canonicalRuntimeState(state: ZLinkFrameworkRuntimeState): string {
  return ['preparing', 'serving', 'relocating', 'relocated', 'draining', 'stopped', 'error'][state]
    ?? (() => { throw new TypeError(`Location Store descriptor has invalid state '${state}'.`); })();
}

function runtimeState(state: string): ZLinkFrameworkRuntimeState {
  const index = ['preparing', 'serving', 'relocating', 'relocated', 'draining', 'stopped', 'error'].indexOf(state);
  if (index < 0) throw new TypeError(`Location Store descriptor has invalid state '${state}'.`);
  return index as ZLinkFrameworkRuntimeState;
}

function encodeAuthorityRecord(record: AuthorityRecord): Uint8Array {
  const snapshot = record.snapshot;
  const allocation = snapshot.allocation;
  const envelope: Record<string, unknown> = {
    recordVersion: 1,
    payload: Buffer.from(snapshot.payload).toString('base64'),
    objectGeneration: snapshot.objectGeneration.toString(),
    authorityOwnerGeneration: snapshot.authorityOwnerGeneration.toString(),
    ownerId: snapshot.ownerId,
    ownerLeaseGeneration: snapshot.ownerLeaseGeneration.toString(),
    allocation: {
      state: allocation.state,
      objectKind: canonicalPlacementObjectKind(allocation.objectKind),
      stableType: allocation.stableType,
      descriptor: {
        meshName: allocation.descriptor.meshName,
        routingIdHex: encodeRoutingIdStorageHex(allocation.descriptor.rid)
      },
      descriptorLifecycleGeneration: allocation.descriptorLifecycleGeneration.toString(),
      capacity: canonicalAuthorityCapacity(allocation.capacity)
    },
    pendingCreation: snapshot.pendingCreation === undefined ? null : {
      reservationId: snapshot.pendingCreation.reservationId,
      requestContentReference: snapshot.pendingCreation.requestContentReference,
      requestSha256: Buffer.from(snapshot.pendingCreation.requestSha256).toString('hex'),
      requestEncodedSize: Number(snapshot.pendingCreation.requestEncodedSize)
    }
  };
  // These fields gate Node-only creation/aggregate recovery.  They are absent
  // from normal authority values (including the golden vector), while the
  // interoperable envelope above remains byte-canonical.
  if (record.reservationId !== undefined) envelope.reservationId = record.reservationId;
  if (record.terminal !== undefined) envelope.terminal = record.terminal;
  if (record.aggregate !== undefined) envelope.aggregate = canonicalize(record.aggregate);
  if (record.visibleStoreVersion !== undefined) envelope.visibleStoreVersion = record.visibleStoreVersion;
  return Buffer.from(JSON.stringify(envelope), 'utf8');
}

function decodeAuthorityRecord(bytes: Uint8Array): AuthorityRecord {
  const value = JSON.parse(decodeText(bytes)) as Record<string, unknown>;
  requireRecordVersion(value, 'authority');
  const allocation = value.allocation as Record<string, unknown>;
  const descriptor = allocation.descriptor as Record<string, unknown>;
  const capacity = allocation.capacity as Record<string, unknown>;
  const pending = value.pendingCreation as Record<string, unknown> | null;
  const objectKind = runtimePlacementObjectKind(String(allocation.objectKind));
  return {
    reservationId: typeof value.reservationId === 'string' ? value.reservationId : undefined,
    terminal: value.terminal as AuthorityRecord['terminal'],
    aggregate: value.aggregate === undefined
      ? undefined
      : reviveCanonical(value.aggregate) as AggregateParticipantFenceRecord,
    visibleStoreVersion: typeof value.visibleStoreVersion === 'string'
      ? value.visibleStoreVersion : undefined,
    snapshot: {
      payload: Buffer.from(String(value.payload), 'base64'),
      objectGeneration: BigInt(String(value.objectGeneration)),
      authorityOwnerGeneration: BigInt(String(value.authorityOwnerGeneration)),
      ownerId: String(value.ownerId),
      ownerLeaseGeneration: BigInt(String(value.ownerLeaseGeneration)),
      allocation: {
        state: allocation.state as ZLinkPlacementAllocation['state'],
        objectKind: objectKind as ZLinkPlacementAllocation['objectKind'],
        stableType: String(allocation.stableType),
        descriptor: {
          meshName: String(descriptor.meshName),
          rid: decodeRoutingId(
            String(descriptor.routingIdHex),
            descriptor.routingIdHex
          )
        },
        descriptorLifecycleGeneration: BigInt(String(allocation.descriptorLifecycleGeneration)),
        capacity: {
          actors: Number(capacity.actors),
          spots: Number(capacity.spots),
          spotType: capacity.spotType === null
            ? undefined
            : decodeAuthoritySpotType(capacity.spotType)
        }
      },
      pendingCreation: pending === null ? undefined : {
        reservationId: String(pending.reservationId),
        requestContentReference: String(pending.requestContentReference),
        requestSha256: Buffer.from(String(pending.requestSha256), 'hex'),
        requestEncodedSize: BigInt(String(pending.requestEncodedSize))
      }
    }
  };
}

function canonicalPlacementObjectKind(kind: ZLinkPlacementAllocation['objectKind']): string {
  switch (kind) {
    case 'actor': return 'actor';
    case 'user_spot': return 'userSpot';
    case 'instance_spot': return 'instanceSpot';
  }
}

function runtimePlacementObjectKind(kind: string): ZLinkPlacementAllocation['objectKind'] {
  switch (kind) {
    case 'actor': return 'actor';
    case 'userSpot': return 'user_spot';
    case 'instanceSpot': return 'instance_spot';
    default: throw new Error(`Location Store authority record has an invalid allocation objectKind '${kind}'.`);
  }
}

function canonicalAuthorityCapacity(capacity: ZLinkCapacityVector): Record<string, unknown> {
  return {
    actors: capacity.actors,
    spots: capacity.spots,
    spotType: capacity.spotType === undefined
      ? null
      : {
          objectKind: canonicalPlacementObjectKind(capacity.spotType.objectKind),
          stableType: capacity.spotType.stableType,
          count: capacity.spotType.count
        }
  };
}

function decodeAuthoritySpotType(value: unknown): ZLinkCapacityVector['spotType'] {
  const spotType = value as Record<string, unknown>;
  return {
    objectKind: runtimeSpotPlacementObjectKind(String(spotType.objectKind)),
    stableType: String(spotType.stableType),
    count: Number(spotType.count)
  };
}

function runtimeSpotPlacementObjectKind(
  kind: string
): Exclude<ZLinkPlacementAllocation['objectKind'], 'actor'> {
  const objectKind = runtimePlacementObjectKind(kind);
  if (objectKind === 'actor') {
    throw new Error('Location Store authority capacity spotType cannot be an actor.');
  }
  return objectKind;
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
    decodeCanonicalDescriptorRecord<ZLinkMeshNodeDescriptor>(descriptorRead.value.bytes).descriptor
  );
  const lease = decodeOwnerRecord(leaseRead.value.bytes);
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
  return (descriptor.populationCapacity.actors.limit === 0
      || capacity.active.actors + capacity.pending.actors + requested.actors
        <= descriptor.populationCapacity.actors.limit)
    && (descriptor.populationCapacity.spots.limit === 0
      || capacity.active.spots + capacity.pending.spots + requested.spots
        <= descriptor.populationCapacity.spots.limit)
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
  const owner = decodeOwnerRecord((lease as Extract<ZLinkStoreReadResult, { kind: 'found' }>).value.bytes);
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
  return BigInt(decodeOwnerRecord(lease.value.bytes).leaseGeneration);
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
  return descriptor;
}

function reviveMeshDescriptor(descriptor: ZLinkMeshNodeDescriptor): ZLinkMeshNodeDescriptor {
  const value = descriptor as unknown as Record<string, unknown>;
  if ('routingIdHex' in value) {
    const capacity = value.capacity as Record<string, unknown>;
    return {
      meshName: String(value.meshName),
      rid: decodeRoutingId(String(value.routingIdHex), value.routingIdHex),
      lifecycleGeneration: BigInt(String(value.lifecycleGeneration)),
      descriptorRevision: BigInt(String(value.descriptorRevision)),
      endpoint: String(value.endpoint),
      objectRole: value.objectRole as ZLinkObjectRole,
      entrySpotId: value.entrySpotId === null ? undefined : value.entrySpotId as string | undefined,
      placementWeight: Number(value.placementWeight),
      populationCapacity: reviveCanonicalCapacity(capacity),
      activationConcurrency: value.activationConcurrency as ZLinkMeshNodeDescriptor['activationConcurrency'],
      channelWeights: value.channelWeights as Readonly<Record<string, number>>,
      applicationVersion: BigInt(String(value.applicationVersion)),
      spotTypes: [],
      objectCapabilities: reviveCanonicalCapabilities(value.objectCapabilities),
      maintenanceWave: value.maintenanceWave === null ? undefined : value.maintenanceWave as string | undefined,
      state: runtimeState(String(value.state)),
      securityIdentity: String(value.securityIdentity),
      ownerId: String(value.ownerId),
      leaseGeneration: BigInt(String(value.leaseGeneration)),
      updatedAt: new Date(Number(value.updatedAtEpochMs))
    };
  }
  return {
    ...descriptor,
    rid: descriptor.rid,
    updatedAt: descriptor.updatedAt instanceof Date
      ? descriptor.updatedAt
      : new Date(descriptor.updatedAt)
  };
}

function reviveClientServerDescriptor(
  descriptor: ZLinkClientServerServerDescriptor
): ZLinkClientServerServerDescriptor {
  const value = descriptor as unknown as Record<string, unknown>;
  if ('serverRoutingIdHex' in value) {
    return {
      channelName: String(value.channelName),
      serverRid: decodeRoutingId(String(value.serverRoutingIdHex), value.serverRoutingIdHex),
      lifecycleGeneration: BigInt(String(value.lifecycleGeneration)),
      descriptorRevision: BigInt(String(value.descriptorRevision)),
      endpoint: String(value.endpoint),
      weight: Number(value.weight),
      state: runtimeState(String(value.state)),
      securityIdentity: String(value.securityIdentity),
      ownerId: String(value.ownerId),
      leaseGeneration: BigInt(String(value.leaseGeneration)),
      updatedAt: new Date(Number(value.updatedAtEpochMs))
    };
  }
  return {
    ...descriptor,
    serverRid: descriptor.serverRid,
    updatedAt: reviveDate(descriptor.updatedAt)
  };
}

function reviveFanoutDescriptor(
  descriptor: ZLinkFanoutPublisherDescriptor
): ZLinkFanoutPublisherDescriptor {
  const value = descriptor as unknown as Record<string, unknown>;
  if ('publisherRoutingIdHex' in value) {
    return {
      channelName: String(value.channelName),
      publisherRid: decodeRoutingId(String(value.publisherRoutingIdHex), value.publisherRoutingIdHex),
      lifecycleGeneration: BigInt(String(value.lifecycleGeneration)),
      descriptorRevision: BigInt(String(value.descriptorRevision)),
      endpoint: String(value.endpoint),
      state: runtimeState(String(value.state)),
      securityIdentity: String(value.securityIdentity),
      ownerId: String(value.ownerId),
      leaseGeneration: BigInt(String(value.leaseGeneration)),
      updatedAt: new Date(Number(value.updatedAtEpochMs))
    };
  }
  return {
    ...descriptor,
    publisherRid: descriptor.publisherRid,
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
  // Descriptor rows are canonicalized through routing-id hex.  Comparing the
  // in-memory shape here would distinguish a binding RoutingId instance from
  // the equivalent ID revived from a canonical row, incorrectly rejecting a
  // legitimate renew as stale.
  const canonical = canonicalDescriptor(descriptor);
  for (const key of omitted) {
    delete canonical[key === 'updatedAt' ? 'updatedAtEpochMs' : key];
  }
  return Buffer.from(encodeJson(canonical)).toString('base64');
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

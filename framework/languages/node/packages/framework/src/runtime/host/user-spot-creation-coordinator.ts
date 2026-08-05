import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createHash } from 'node:crypto';
import type {
  RoutingId,
  SpotRef,
  ZLinkSpotCreateResult
} from '../../contracts';
import type {
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkObjectReserveResult
} from '../../contracts/Locations';
import type { ZLinkAuthorityStore, ZLinkObjectCreationStore } from '../locations/internal-store-contracts';
import {
  ZLinkFrameworkException,
  ZLinkSpotCreateState
} from '../../contracts';
import {
  decodeServiceReadySpotAuthority,
  encodeServiceUserSpotAuthorityPayload
} from '../foundation/service-authority-payload-codec';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import type { ZLinkLocalSpotCreateResult } from '../spots/spot-manager-internal-contracts';
import type {
  ServiceUserSpotOperationResult
} from '../foundation/service-stateful-runtime';
import type {
  ServiceUserSpotCloseRecord,
  ServiceUserSpotCreateRecord,
  ServiceDirectSpotRouteFence
} from '../foundation/service-stateful-wire-codec';

export interface ZLinkUserSpotCreationCoordinatorOptions {
  readonly store: ZLinkObjectCreationStore & ZLinkAuthorityStore;
  readonly target: (request: Pick<
    ZLinkUserSpotCreationRequest,
    'meshName' | 'stableType'
  >, signal?: AbortSignal, excludedNodeRids?: ReadonlySet<string>) => Promise<{
    readonly meshName: string;
    readonly nodeRid: RoutingId;
    readonly nodeGeneration: bigint;
    readonly owner: ZLinkLocationOwnerToken;
    readonly isLocal: boolean;
  } | undefined>;
  readonly pollIntervalMs?: number;
  readonly cleanupTimeoutMs?: number;
  readonly remoteCreate?: (
    meshName: string,
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ) => Promise<ServiceUserSpotOperationResult>;
  readonly decodeRemoteReply?: (payload: Uint8Array) => unknown;
  /** Records the committed Ready fence on the owning MeshNode before publication. */
  readonly publishReadyRoute?: (meshName: string, route: ServiceDirectSpotRouteFence) => void;
  /** Removes the committed Ready fence after the authority is deleted. */
  readonly forgetReadyRoute?: (meshName: string, route: ServiceDirectSpotRouteFence) => void;
}

export interface ZLinkUserSpotCreationRequest {
  readonly meshName?: string;
  readonly spotId: RoutingId;
  readonly stableType: string;
  readonly requestPayload: Uint8Array;
  readonly timeoutMs: number;
  readonly signal?: AbortSignal;
  readonly generatedIdentity?: boolean;
}

export interface ZLinkUserSpotCreationResult {
  readonly result: ZLinkSpotCreateResult;
  readonly spot: SpotRef;
}

export interface ZLinkUserSpotCloseTarget {
  readonly spot: SpotRef;
  readonly snapshot: ZLinkAuthoritySnapshot;
}

/**
 * Makes generic Location Store reservation the only visibility barrier for a
 * User Spot. Only the reservation winner invokes application lifecycle code.
 */
export class ZLinkUserSpotCreationCoordinator {
  private readonly localCreations = new Map<string, Promise<ZLinkUserSpotCreationResult>>();
  private readonly remoteCreations = new Map<string, {
    readonly fingerprint: string;
    readonly result: Promise<ZLinkUserSpotCreationResult>;
  }>();

  constructor(private readonly options: ZLinkUserSpotCreationCoordinatorOptions) {}

  async getOrCreate(
    request: ZLinkUserSpotCreationRequest,
    materialize: (target: {
      readonly meshName: string;
      readonly nodeRid: RoutingId;
      readonly isLocal: boolean;
    }, authority: ZLinkAuthoritySnapshot, signal: AbortSignal) => Promise<ZLinkLocalSpotCreateResult>,
    discard?: (signal?: AbortSignal) => Promise<void>
  ): Promise<ZLinkUserSpotCreationResult> {
    const deadline = createDeadline(request.timeoutMs, request.signal);
    const deadlineUnixMs = Date.now() + request.timeoutMs;
    const signal = deadline.signal;
    signal.throwIfAborted();
    const checksum = crc32c(request.requestPayload);
    const sha256 = createHash('sha256').update(request.requestPayload).digest();
    const contentReference = encodeLocationCreationContent(request.requestPayload, checksum);
    const excludedNodeRids = new Set<string>();
    let target: Awaited<ReturnType<ZLinkUserSpotCreationCoordinatorOptions['target']>>;
    let authorityTarget;
    let reserved: ZLinkObjectReserveResult;
    for (;;) {
      try {
        target = await this.options.target(request, signal, excludedNodeRids);
      } catch (error) {
        deadline.close();
        if (error instanceof ZLinkFrameworkException) throw error;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          'User Spot placement provider failed.',
          false,
          error
        );
      }
      if (target === undefined) {
        deadline.close();
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
          'No eligible User Spot placement target is ready.',
          true
        );
      }
      if (excludedNodeRids.has(String(target.nodeRid))) {
        deadline.close();
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
          'No additional User Spot placement target is ready.',
          true
        );
      }
      const owner = target.owner;
      authorityTarget = {
        meshName: target.meshName,
        nodeRid: target.nodeRid,
        nodeLifecycleGeneration: target.nodeGeneration,
        owner
      };
      try {
        reserved = await this.options.store.reserve({
          key: { kind: 'user_spot', globalId: String(request.spotId) },
          intent: {
            stableType: request.stableType,
            requestContentReference: contentReference,
            requestSha256: sha256,
            requestEncodedSize: BigInt(request.requestPayload.byteLength)
          },
          target: authorityTarget,
          creatingPayload: encodeServiceUserSpotAuthorityPayload({
            state: 'creating',
            stableType: request.stableType,
            spotId: String(request.spotId),
            ownerId: owner.ownerId,
            ownerLeaseGeneration: owner.leaseGeneration,
            ownerMeshName: target.meshName,
            ownerNodeRid: String(target.nodeRid),
            ownerNodeGeneration: target.nodeGeneration
          }),
          capacity: {
            actors: 0,
            spots: 1,
            spotType: {
              objectKind: 'user_spot',
              stableType: request.stableType,
              count: 1
            }
          }
        }, signal);
      } catch (error) {
        deadline.close();
        if (error instanceof ZLinkFrameworkException) throw error;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          'User Spot reservation Store operation failed.',
          false,
          error
        );
      }
      if (reserved.kind !== 'placementCapacityExhausted') break;
      excludedNodeRids.add(String(target.nodeRid));
    }
    if (
      reserved.kind === 'alreadyExists'
      || (
        reserved.kind === 'conflict'
        && reserved.current.kind === 'snapshot'
        && reserved.current.allocation.objectKind === 'user_spot'
        && reserved.current.allocation.stableType === request.stableType
      )
    ) {
      const current = reserved.kind === 'alreadyExists'
        ? reserved.current
        : reserved.current;
      if (
        current.kind === 'snapshot'
        && current.allocation.state === 'active'
      ) {
        if (request.generatedIdentity === true) {
          deadline.close();
          throw spotIdConflict(request.spotId);
        }
        const spot = spotRef(current, request);
        deadline.close();
        return {
          result: { spot, state: ZLinkSpotCreateState.Existing },
          spot
        };
      }
      if (
        current.kind !== 'snapshot'
        || !sameCreationTarget(current, authorityTarget)
      ) {
        deadline.close();
        if (request.generatedIdentity === true) {
          throw spotIdConflict(request.spotId);
        }
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          'User Spot creation requires a remote generation-fenced create operation.'
        );
      }
      if (current.allocation.state === 'reserved' && !target.isLocal) {
        try {
          return await this.remoteCreate(
            request,
            current,
            target.nodeRid,
            target.nodeGeneration,
            deadlineUnixMs
          );
        } finally {
          deadline.close();
        }
      }
      if (current.allocation.state === 'reserved' && target.isLocal) {
        const pending = current.pendingCreation;
        const admitted = pending === undefined
          ? undefined
          : this.localCreations.get(pending.reservationId);
        if (admitted === undefined) {
          try {
            const spot = await this.awaitReady(request, signal);
            return {
              result: { spot, state: ZLinkSpotCreateState.Existing },
              spot
            };
          } finally {
            deadline.close();
          }
        }
        try {
          const result = await admitted;
          return result.result.state === ZLinkSpotCreateState.Created
            ? {
                result: {
                  ...result.result,
                  state: ZLinkSpotCreateState.Existing
                },
                spot: result.spot
              }
            : result;
        } finally {
          deadline.close();
        }
      }
      try {
        const spot = await this.awaitReady(request, signal);
        return {
          result: { spot, state: ZLinkSpotCreateState.Existing },
          spot
        };
      } finally {
        deadline.close();
      }
    }
    if (reserved.kind === 'typeMismatch') {
      deadline.close();
      if (request.generatedIdentity === true) {
        throw spotIdConflict(request.spotId);
      }
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
        `User Spot '${String(request.spotId)}' has another stable type.`
      );
    }
    if (reserved.kind !== 'reserved') {
      deadline.close();
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        `User Spot reservation failed: ${reserved.kind}.`
      );
    }

    try {
      return target.isLocal
        ? await this.handleLocalCreate(
            localCreationRecord(
              request,
              reserved.creating,
              target.nodeRid,
              target.nodeGeneration,
              deadlineUnixMs
            ),
            (_payload, authority, localSignal) =>
              materialize(target, authority, localSignal),
            discard,
            signal
          )
        : await this.remoteCreate(
            request,
            reserved.creating,
            target.nodeRid,
            target.nodeGeneration,
            deadlineUnixMs
          );
    } finally {
      deadline.close();
    }
  }

  async close(
    spot: SpotRef,
    closeOwner: (current: SpotRef, snapshot: ZLinkAuthoritySnapshot) => Promise<boolean>,
    signal?: AbortSignal,
    ownerPresent?: (current: SpotRef) => boolean,
    ownerCanClose?: (current: SpotRef) => boolean
  ): Promise<boolean> {
    const resolved = await this.resolveCloseTarget(spot, signal);
    if (resolved === undefined) return false;
    const { spot: currentRef, snapshot: current } = resolved;
    const key = encodeAuthorityKey('user_spot', String(spot.spotId));
    if (ownerPresent?.(currentRef) === false) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `User Spot '${String(spot.spotId)}' is missing from its authority owner.`,
        true
      );
    }
    if (ownerCanClose?.(currentRef) === false) return false;
    const closing = await this.beginClosing(key, current, String(spot.spotId), signal);
    let closed: boolean;
    try {
      closed = await closeOwner(currentRef, closing);
    } catch (error) {
      await this.restoreReady(key, closing, String(spot.spotId), signal);
      throw error;
    }
    if (!closed) {
      await this.restoreReady(key, closing, String(spot.spotId), signal);
      return false;
    }
    const deleted = await this.options.store.compareExchangeAuthority(
      key,
      closing.storeVersion,
      { kind: 'delete' },
      signal
    );
    if (deleted.kind === 'deleted') {
      this.forgetReadyRoute(currentRef.spotId, current);
      return true;
    }
    if (deleted.kind === 'conflict') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `User Spot '${String(spot.spotId)}' authority changed while closing.`,
        true
      );
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
      `User Spot '${String(spot.spotId)}' generation cannot be closed.`
    );
  }

  async resolveCloseTarget(
    spot: SpotRef,
    signal?: AbortSignal
  ): Promise<ZLinkUserSpotCloseTarget | undefined> {
    const key = encodeAuthorityKey('user_spot', String(spot.spotId));
    const current = await this.options.store.readAuthority(key, signal);
    if (current.kind === 'missing') return undefined;
    if (current.objectGeneration !== spot.objectGeneration) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
        `User Spot '${String(spot.spotId)}' generation is stale.`
      );
    }
    const currentRef = spotRef(current, {
      meshName: current.allocation.descriptor.meshName,
      spotId: spot.spotId,
      stableType: current.allocation.stableType,
      requestPayload: Buffer.alloc(0),
      timeoutMs: 1
    });
    if (
      String(current.allocation.descriptor.rid) !== String(spot.nodeRid)
      || current.allocation.descriptor.meshName !== spot.meshName
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
        `User Spot '${String(spot.spotId)}' owner route is stale.`
      );
    }
    return { spot: currentRef, snapshot: current };
  }

  async handleRemoteCreate(
    record: ServiceUserSpotCreateRecord,
    materialize: (
      requestPayload: Uint8Array,
      authority: ZLinkAuthoritySnapshot,
      signal: AbortSignal
    ) => Promise<ZLinkLocalSpotCreateResult>,
    discard?: (signal?: AbortSignal) => Promise<void>,
    signal?: AbortSignal
  ): Promise<ZLinkUserSpotCreationResult> {
    const key = `${record.spotId}\0${record.reservation.reservationId}`;
    const fingerprint = remoteCreationFingerprint(record);
    let admitted = this.remoteCreations.get(key);
    if (admitted !== undefined && admitted.fingerprint !== fingerprint) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Remote User Spot reservation was reused with different immutable content.'
      );
    }
    if (admitted === undefined) {
      const result = this.materializeRemoteCreate(record, materialize, discard, signal);
      admitted = { fingerprint, result };
      this.remoteCreations.set(key, admitted);
      void result.finally(() => {
        if (this.remoteCreations.get(key)?.result === result) {
          this.remoteCreations.delete(key);
        }
      }).catch(() => undefined);
    }
    return await admitted.result;
  }

  private async handleLocalCreate(
    record: ServiceUserSpotCreateRecord,
    materialize: (
      requestPayload: Uint8Array,
      authority: ZLinkAuthoritySnapshot,
      signal: AbortSignal
    ) => Promise<ZLinkLocalSpotCreateResult>,
    discard?: (signal?: AbortSignal) => Promise<void>,
    signal?: AbortSignal
  ): Promise<ZLinkUserSpotCreationResult> {
    const key = record.reservation.reservationId;
    let admitted = this.localCreations.get(key);
    if (admitted === undefined) {
      admitted = this.materializeRemoteCreate(record, materialize, discard, signal);
      this.localCreations.set(key, admitted);
      void admitted.finally(() => {
        if (this.localCreations.get(key) === admitted) {
          this.localCreations.delete(key);
        }
      }).catch(() => undefined);
    }
    return await admitted;
  }

  private async materializeRemoteCreate(
    record: ServiceUserSpotCreateRecord,
    materialize: (
      requestPayload: Uint8Array,
      authority: ZLinkAuthoritySnapshot,
      signal: AbortSignal
    ) => Promise<ZLinkLocalSpotCreateResult>,
    discard?: (signal?: AbortSignal) => Promise<void>,
    signal?: AbortSignal
  ): Promise<ZLinkUserSpotCreationResult> {
    const key = encodeAuthorityKey('user_spot', record.spotId);
    const current = await this.options.store.readAuthority(key, signal);
    const exactIdentity =
      current.kind !== 'snapshot'
        ? false
        : current.allocation.objectKind === 'user_spot'
          && current.allocation.stableType === record.stableType
          && current.objectGeneration === record.reservation.objectGeneration
          && current.authorityOwnerGeneration === record.reservation.authorityOwnerGeneration
          && String(current.allocation.descriptor.rid) === record.reservation.targetNodeRid
          && current.allocation.descriptorLifecycleGeneration
            === record.reservation.targetNodeGeneration
          && current.ownerId === record.reservation.targetOwnerId
          && current.ownerLeaseGeneration === record.reservation.targetOwnerLeaseGeneration
          && current.allocation.capacity.actors === 0
          && current.allocation.capacity.spots === record.reservation.pendingCapacityDelta
          && current.allocation.capacity.spotType?.objectKind === 'user_spot'
          && current.allocation.capacity.spotType.stableType === record.stableType
          && current.allocation.capacity.spotType.count
            === record.reservation.pendingCapacityDelta;
    if (!exactIdentity || current.kind !== 'snapshot') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        'Remote User Spot reservation fence is stale.'
      );
    }
    const spot: SpotRef = {
      spotId: record.spotId as RoutingId,
      objectGeneration: current.objectGeneration,
      meshName: current.allocation.descriptor.meshName,
      nodeRid: current.allocation.descriptor.rid
    };
    if (current.allocation.state === 'active') {
      // A target restart loses the process-local command terminal, but Ready
      // authority proves the exact reservation generation committed. Return
      // Existing without re-running application lifecycle. Created/Rejected
      // application replies require a future durable operation-terminal store.
      return {
        result: { spot, state: ZLinkSpotCreateState.Existing },
        spot
      };
    }
    if (current.storeVersion.value !== record.reservation.expectedStoreVersion) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        'Remote User Spot Pending StoreVersion is stale.'
      );
    }
    const pending = current.pendingCreation;
    if (
      pending === undefined
      || pending.reservationId !== record.reservation.reservationId
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Remote User Spot Pending creation projection is missing.'
      );
    }
    const requestPayload = decodeLocationCreationContent(
      pending.requestContentReference
    );
    if (
      BigInt(requestPayload.byteLength) !== pending.requestEncodedSize
      || !createHash('sha256').update(requestPayload).digest()
        .equals(Buffer.from(pending.requestSha256))
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Remote User Spot Pending creation content failed integrity validation.'
      );
    }
    let local: ZLinkLocalSpotCreateResult | undefined;
    try {
      local = await materialize(
        requestPayload,
        current,
        signal ?? new AbortController().signal
      );
      if (local.state === ZLinkSpotCreateState.Rejected) {
        local.publication?.abort();
        await this.options.store.abort({
          key: { kind: 'user_spot', globalId: record.spotId },
          reservationId: pending.reservationId,
          expectedStoreVersion: current.storeVersion.value,
          target: {
            meshName: current.allocation.descriptor.meshName,
            nodeRid: current.allocation.descriptor.rid,
            nodeLifecycleGeneration: current.allocation.descriptorLifecycleGeneration,
            owner: {
              ownerId: current.ownerId,
              leaseGeneration: current.ownerLeaseGeneration
            }
          }
        }, signal);
        await discard?.(signal);
        return {
          result: { spot, state: local.state, reply: local.reply },
          spot
        };
      }
      let committed;
      try {
        committed = await this.options.store.commit({
          key: { kind: 'user_spot', globalId: record.spotId },
          reservationId: pending.reservationId,
          expectedStoreVersion: current.storeVersion.value,
          target: {
            meshName: current.allocation.descriptor.meshName,
            nodeRid: current.allocation.descriptor.rid,
            nodeLifecycleGeneration: current.allocation.descriptorLifecycleGeneration,
            owner: {
              ownerId: current.ownerId,
              leaseGeneration: current.ownerLeaseGeneration
            }
          },
          readyPayload: encodeServiceUserSpotAuthorityPayload({
            state: 'ready',
            stableType: record.stableType,
            spotId: record.spotId,
            ownerId: current.ownerId,
            ownerLeaseGeneration: current.ownerLeaseGeneration,
            ownerMeshName: current.allocation.descriptor.meshName,
            ownerNodeRid: String(current.allocation.descriptor.rid),
            ownerNodeGeneration: current.allocation.descriptorLifecycleGeneration
          })
        }, signal);
      } catch (error) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          'User Spot Ready commit Store operation failed.',
          false,
          error
        );
      }
      if (committed.kind !== 'committed' && committed.kind !== 'alreadyCommitted') {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestFailed,
          `Remote User Spot Ready commit failed: ${committed.kind}.`
        );
      }
      const readySpot: SpotRef = {
        ...spot,
        objectGeneration: committed.ready.objectGeneration
      };
      this.options.publishReadyRoute?.(
        committed.ready.allocation.descriptor.meshName,
        readySpotRoute(record.spotId, committed.ready)
      );
      local.publication?.publish();
      return {
        result: { spot: readySpot, state: local.state, reply: local.reply },
        spot: readySpot
      };
    } catch (error) {
      local?.publication?.abort();
      const cleanupDeadline = createDeadline(this.options.cleanupTimeoutMs ?? 1_000);
      const cleanupSignal = cleanupDeadline.signal;
      const cleanup = await Promise.allSettled([
        waitForAbort(this.options.store.abort({
          key: { kind: 'user_spot', globalId: record.spotId },
          reservationId: pending.reservationId,
          expectedStoreVersion: current.storeVersion.value,
          target: {
            meshName: current.allocation.descriptor.meshName,
            nodeRid: current.allocation.descriptor.rid,
            nodeLifecycleGeneration: current.allocation.descriptorLifecycleGeneration,
            owner: {
              ownerId: current.ownerId,
              leaseGeneration: current.ownerLeaseGeneration
            }
          }
        }, cleanupSignal), cleanupSignal),
        waitForAbort(discard?.(cleanupSignal) ?? Promise.resolve(), cleanupSignal)
      ]);
      cleanupDeadline.close();
      const cleanupErrors = cleanup
        .filter((item): item is PromiseRejectedResult => item.status === 'rejected')
        .map(item => item.reason);
      if (cleanupErrors.length > 0) {
        throw new AggregateError(
          [error, ...cleanupErrors],
          'Remote User Spot creation rollback failed.'
        );
      }
      throw error;
    }
  }

  async handleRemoteClose(
    record: ServiceUserSpotCloseRecord,
    closeOwner: (spot: SpotRef, signal?: AbortSignal) => Promise<boolean>,
    signal?: AbortSignal
  ): Promise<boolean> {
    const key = encodeAuthorityKey('user_spot', record.target.spotId);
    const current = await this.options.store.readAuthority(key, signal);
    if (current.kind === 'missing') return false;
    if (
      current.objectGeneration !== record.target.objectGeneration
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
        `User Spot '${record.target.spotId}' generation is stale.`
      );
    }
    if (
      current.authorityOwnerGeneration !== record.target.authorityOwnerGeneration
      || current.storeVersion.value !== record.target.expectedStoreVersion
      || String(current.allocation.descriptor.rid) !== record.target.targetNodeRid
      || current.allocation.descriptorLifecycleGeneration
        !== record.target.targetNodeGeneration
      || current.allocation.objectKind !== 'user_spot'
      || current.allocation.state !== 'active'
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `User Spot '${record.target.spotId}' close authority is moving.`,
        true
      );
    }
    const spot: SpotRef = {
      spotId: record.target.spotId as RoutingId,
      objectGeneration: current.objectGeneration,
      meshName: current.allocation.descriptor.meshName,
      nodeRid: current.allocation.descriptor.rid
    };
    const closing = await this.beginClosing(key, current, record.target.spotId, signal);
    let closed: boolean;
    try {
      closed = await closeOwner(spot, signal);
    } catch (error) {
      await this.restoreReady(key, closing, record.target.spotId, signal);
      throw error;
    }
    if (!closed) {
      await this.restoreReady(key, closing, record.target.spotId, signal);
      return false;
    }
    const deleted = await this.options.store.compareExchangeAuthority(
      key,
      closing.storeVersion,
      { kind: 'delete' },
      signal
    );
    if (deleted.kind === 'deleted') {
      this.forgetReadyRoute(record.target.spotId as RoutingId, current);
      return true;
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotMoving,
      `User Spot '${record.target.spotId}' authority changed while closing.`,
      true
    );
  }

  forgetReadyRoute(spotId: RoutingId, snapshot: ZLinkAuthoritySnapshot): void {
    this.options.forgetReadyRoute?.(
      snapshot.allocation.descriptor.meshName,
      readySpotRoute(spotId, snapshot)
    );
  }

  private async beginClosing(
    key: ReturnType<typeof encodeAuthorityKey>,
    current: ZLinkAuthoritySnapshot,
    spotId: string,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const ready = decodeServiceReadySpotAuthority(current.payload);
    if (
      ready?.kind !== 'user_spot'
      || ready.spotId !== spotId
      || ready.stableType !== current.allocation.stableType
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `User Spot '${spotId}' is not in Framework-owned Ready state.`,
        true
      );
    }
    const closing = await this.options.store.compareExchangeAuthority(
      key,
      current.storeVersion,
      {
        kind: 'put',
        payload: userSpotAuthorityPayload(current, spotId, 'closing'),
        generationTransition: 'preserve'
      },
      signal
    );
    if (closing.kind === 'stored') {
      const { kind: _kind, ...snapshot } = closing;
      return { kind: 'snapshot', ...snapshot };
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotMoving,
      `User Spot authority changed while entering Closing.`,
      true
    );
  }

  private async restoreReady(
    key: ReturnType<typeof encodeAuthorityKey>,
    closing: ZLinkAuthoritySnapshot,
    spotId: string,
    signal?: AbortSignal
  ): Promise<void> {
    const restored = await this.options.store.compareExchangeAuthority(
      key,
      closing.storeVersion,
      {
        kind: 'put',
        payload: userSpotAuthorityPayload(closing, spotId, 'ready'),
        generationTransition: 'preserve'
      },
      signal
    );
    if (restored.kind !== 'stored') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        'User Spot Ready authority could not be restored after close was declined.',
        true
      );
    }
  }

  private async awaitReady(
    request: ZLinkUserSpotCreationRequest,
    signal: AbortSignal
  ): Promise<SpotRef> {
    const key = encodeAuthorityKey('user_spot', String(request.spotId));
    for (;;) {
      signal.throwIfAborted();
      const current = await this.options.store.readAuthority(key, signal);
      if (current.kind === 'snapshot' && current.allocation.state === 'active') {
        return spotRef(current, request);
      }
      await wait(this.options.pollIntervalMs ?? 10, signal);
    }
  }

  private async remoteCreate(
    request: ZLinkUserSpotCreationRequest,
    snapshot: ZLinkAuthoritySnapshot,
    targetNodeRid: RoutingId,
    targetNodeGeneration: bigint,
    deadlineUnixMs: number
  ): Promise<ZLinkUserSpotCreationResult> {
    const remote = this.options.remoteCreate;
    const pending = snapshot.pendingCreation;
    if (remote === undefined || pending === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Remote User Spot creation transport is not configured.'
      );
    }
    const timeoutMs = deadlineUnixMs - Date.now();
    if (timeoutMs <= 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        'Remote User Spot creation exhausted its end-to-end deadline.',
        true
      );
    }
    const result = await remote(
      snapshot.allocation.descriptor.meshName,
      String(targetNodeRid),
      {
        sourceNodeRid: '',
        sourceNodeGeneration: 1n,
        spotId: String(request.spotId),
        stableType: request.stableType,
        reservation: {
          reservationId: pending.reservationId,
          expectedStoreVersion: snapshot.storeVersion.value,
          objectGeneration: snapshot.objectGeneration,
          authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
          targetNodeRid: String(targetNodeRid),
          targetNodeGeneration,
          targetOwnerId: snapshot.ownerId,
          targetOwnerLeaseGeneration: snapshot.ownerLeaseGeneration,
          pendingCapacityDelta: snapshot.allocation.capacity.spots
        },
        deadlineUnixMs: BigInt(deadlineUnixMs)
      },
      timeoutMs
    );
    if (
      result.terminalResult !== 0
      || result.tail?.kind !== 'userSpotCreate'
    ) {
      throw remoteUserSpotFailure(
        result.terminalResult,
        result.failureCode,
        'Remote User Spot creation failed.'
      );
    }
    const spot: SpotRef = {
      spotId: request.spotId,
      objectGeneration: result.tail.objectGeneration,
      meshName: snapshot.allocation.descriptor.meshName,
      nodeRid: targetNodeRid
    };
    const state = result.tail.createResult === 'existing'
      ? ZLinkSpotCreateState.Existing
      : result.tail.createResult === 'created'
        ? ZLinkSpotCreateState.Created
        : ZLinkSpotCreateState.Rejected;
    return {
      result: {
        spot,
        state,
        ...(result.payload === undefined
          ? {}
          : {
              reply: this.options.decodeRemoteReply?.(result.payload.payload)
                ?? Buffer.from(result.payload.payload)
            })
      },
      spot
    };
  }

}

function spotIdConflict(spotId: RoutingId): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.SpotIdConflict,
    `Generated User Spot ID '${String(spotId)}' is already active.`
  );
}

function readySpotRoute(
  spotId: RoutingId,
  snapshot: ZLinkAuthoritySnapshot
): ServiceDirectSpotRouteFence {
  return {
    spot: {
      spotId: String(spotId),
      generation: snapshot.objectGeneration
    },
    targetNodeRid: String(snapshot.allocation.descriptor.rid),
    targetNodeGeneration: snapshot.allocation.descriptorLifecycleGeneration,
    authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
    ownerLeaseGeneration: snapshot.ownerLeaseGeneration,
    storeVersion: snapshot.storeVersion.value
  };
}

function spotRef(
  snapshot: ZLinkAuthoritySnapshot,
  request: ZLinkUserSpotCreationRequest,
  allowPending = false
): SpotRef {
  const decoded = decodeServiceReadySpotAuthority(snapshot.payload);
  if (
    (
      allowPending
        ? (
            snapshot.allocation.objectKind !== 'user_spot'
            || snapshot.allocation.stableType !== request.stableType
          )
        : (
            decoded?.kind !== 'user_spot'
            || decoded.stableType !== request.stableType
            || decoded.spotId !== String(request.spotId)
          )
    )
  ) {
    throw new Error('User Spot Ready authority does not match the requested identity.');
  }
  return {
    spotId: request.spotId,
    objectGeneration: snapshot.objectGeneration,
    meshName: snapshot.allocation.descriptor.meshName,
    nodeRid: snapshot.allocation.descriptor.rid
  };
}

function localCreationRecord(
  request: ZLinkUserSpotCreationRequest,
  snapshot: ZLinkAuthoritySnapshot,
  targetNodeRid: RoutingId,
  targetNodeGeneration: bigint,
  deadlineUnixMs: number
): ServiceUserSpotCreateRecord {
  const pending = snapshot.pendingCreation;
  if (pending === undefined) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestFailed,
      'Local User Spot Pending creation projection is missing.'
    );
  }
  return {
    kind: 'userSpotCreate',
    correlation: 1n,
    operation: {
      high: snapshot.objectGeneration,
      low: snapshot.authorityOwnerGeneration
    },
    sourceNodeRid: String(targetNodeRid),
    sourceNodeGeneration: targetNodeGeneration,
    spotId: String(request.spotId),
    stableType: request.stableType,
    reservation: {
      reservationId: pending.reservationId,
      expectedStoreVersion: snapshot.storeVersion.value,
      objectGeneration: snapshot.objectGeneration,
      authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
      targetNodeRid: String(targetNodeRid),
      targetNodeGeneration,
      targetOwnerId: snapshot.ownerId,
      targetOwnerLeaseGeneration: snapshot.ownerLeaseGeneration,
      pendingCapacityDelta: snapshot.allocation.capacity.spots
    },
    deadlineUnixMs: BigInt(deadlineUnixMs)
  };
}

export function encodeLocationCreationContent(
  payload: Uint8Array,
  checksum = crc32c(payload)
): string {
  return `inline-v1:${checksum.toString(16).padStart(8, '0')}:${Buffer.from(payload).toString('base64url')}`;
}

export function decodeLocationCreationContent(
  reference: string,
  expectedSha256?: Uint8Array,
  expectedEncodedSize?: bigint
): Buffer {
  const match = /^inline-v1:([0-9a-f]{8}):([A-Za-z0-9_-]+)$/.exec(reference);
  if (match === null) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestFailed,
      'User Spot creation content reference is invalid.'
    );
  }
  const payload = Buffer.from(match[2]!, 'base64url');
  if (crc32c(payload) !== Number.parseInt(match[1]!, 16)) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestFailed,
      'User Spot creation content checksum does not match.'
    );
  }
  if (
    expectedEncodedSize !== undefined
    && BigInt(payload.byteLength) !== expectedEncodedSize
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestFailed,
      'Creation content encoded size does not match its Pending reservation.'
    );
  }
  if (
    expectedSha256 !== undefined
    && !createHash('sha256').update(payload).digest().equals(Buffer.from(expectedSha256))
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestFailed,
      'Creation content SHA-256 does not match its Pending reservation.'
    );
  }
  return payload;
}

function sameCreationTarget(
  snapshot: ZLinkAuthoritySnapshot,
  target: Parameters<ZLinkObjectCreationStore['reserve']>[0]['target']
): boolean {
  return snapshot.allocation.descriptor.meshName === target.meshName
    && String(snapshot.allocation.descriptor.rid) === String(target.nodeRid)
    && snapshot.allocation.descriptorLifecycleGeneration === target.nodeLifecycleGeneration
    && snapshot.ownerId === target.owner.ownerId
    && snapshot.ownerLeaseGeneration === target.owner.leaseGeneration;
}

function remoteCreationFingerprint(record: ServiceUserSpotCreateRecord): string {
  return JSON.stringify(
    {
      spotId: record.spotId,
      stableType: record.stableType,
      reservation: record.reservation
    },
    (_key, value: unknown) => typeof value === 'bigint' ? `${value}n` : value
  );
}

function userSpotAuthorityPayload(
  snapshot: ZLinkAuthoritySnapshot,
  spotId: string,
  state: 'ready' | 'closing'
): Buffer {
  return encodeServiceUserSpotAuthorityPayload({
    state,
    stableType: snapshot.allocation.stableType,
    spotId,
    ownerId: snapshot.ownerId,
    ownerLeaseGeneration: snapshot.ownerLeaseGeneration,
    ownerMeshName: snapshot.allocation.descriptor.meshName,
    ownerNodeRid: String(snapshot.allocation.descriptor.rid),
    ownerNodeGeneration: snapshot.allocation.descriptorLifecycleGeneration
  });
}

function crc32c(payload: Uint8Array): number {
  let crc = 0xffff_ffff;
  for (const value of payload) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (0x82f6_3b78 & -(crc & 1));
    }
  }
  return (~crc) >>> 0;
}

function remoteUserSpotFailure(
  terminalResult: number,
  failureCode: number,
  message: string
): ZLinkFrameworkException {
  const detail = `${message} terminalResult=${terminalResult}, failureCode=${failureCode}.`;
  if (failureCode === 33) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
      detail
    );
  }
  if (failureCode === 34) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotMoving,
      detail,
      true
    );
  }
  if (terminalResult === 101) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      detail,
      true
    );
  }
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RequestFailed,
    detail
  );
}

function createDeadline(timeoutMs: number, parent?: AbortSignal): {
  readonly signal: AbortSignal;
  close(): void;
} {
  const controller = new AbortController();
  const timeout = setTimeout(
    () => controller.abort(createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      'User Spot creation exceeded its end-to-end deadline.',
      true
    )),
    timeoutMs
  );
  const abort = () => controller.abort(parent?.reason);
  parent?.addEventListener('abort', abort, { once: true });
  return {
    signal: controller.signal,
    close: () => {
      clearTimeout(timeout);
      parent?.removeEventListener('abort', abort);
    }
  };
}

function wait(delayMs: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const done = () => {
      signal?.removeEventListener('abort', abort);
      resolve();
    };
    const timer = setTimeout(done, delayMs);
    const abort = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };
    signal?.addEventListener('abort', abort, { once: true });
  });
}

function waitForAbort<T>(operation: Promise<T>, signal: AbortSignal): Promise<T> {
  if (signal.aborted) return Promise.reject(signal.reason);
  return new Promise<T>((resolve, reject) => {
    const abort = () => reject(signal.reason);
    signal.addEventListener('abort', abort, { once: true });
    operation.then(
      value => {
        signal.removeEventListener('abort', abort);
        resolve(value);
      },
      error => {
        signal.removeEventListener('abort', abort);
        reject(error);
      }
    );
  });
}

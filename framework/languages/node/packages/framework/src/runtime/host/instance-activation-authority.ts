import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createHash } from 'node:crypto';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkBlobReference,
  ZLinkRelocationStore
} from '../../contracts/Locations';
import type { ZLinkAuthorityStore, ZLinkObjectCreationStore } from '../locations/internal-store-contracts';
import {
  ZLinkFrameworkException
} from '../../contracts';
import {
  decodeServiceInstanceAuthorityPayload,
  decodeServiceReadySpotAuthority,
  encodeServiceInstanceAuthorityPayload
} from '../foundation/service-authority-payload-codec';
import type {
  ServiceAsyncInstanceActivationAuthority,
  ServiceInstanceActivationReservation,
  ServiceInstanceAuthorityRead,
  ServiceInstanceAuthorityReserve,
  ServicePendingInstanceActivation
} from '../foundation/service-stateful-runtime';
import type { ServiceSpotState } from '../foundation/service-stateful-registry';
import type {
  ServiceInstanceActivationTarget,
  ServiceInstanceRouteFence
} from '../foundation/service-stateful-wire-codec';
import { routingIdsEqual } from '../routing-id';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  crc32c
} from '../foundation/service-relocation-runtime';
import {
  putNewRelocationBlob,
  relocationBlobReference
} from '../locations/relocation-blob';

import {
  encodeInstanceActivationRecoveryEnvelope,
  type ServiceInstanceActivationRecoveryEnvelope
} from '../foundation/service-instance-activation-recovery-codec';

const CREATION_REQUEST_RETENTION_MS = 24 * 60 * 60 * 1_000;
const MAX_CREATION_REQUEST_BYTES = 1024 * 1024;
const ACTIVATION_JOIN_POLL_INTERVAL_MS = 10;

interface PendingReservation {
  readonly reservationId: string;
  readonly creating: ZLinkAuthoritySnapshot;
  readonly owner: ZLinkLocationOwnerToken;
  readonly requestReference: ZLinkBlobReference;
  readonly requestSha256: Buffer;
  readonly requestEncodedSize: number;
}

export interface ZLinkInstanceActivationAuthorityOptions {
  readonly store: ZLinkObjectCreationStore & ZLinkAuthorityStore;
  readonly relocationStore?: ZLinkRelocationStore;
  readonly meshName: string;
  readonly owner: () => ZLinkLocationOwnerToken | undefined;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly onReady?: (
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence
  ) => void;
}

export class ZLinkInstanceActivationAuthority
implements ServiceAsyncInstanceActivationAuthority {
  private readonly pending = new Map<string, PendingReservation>();

  constructor(private readonly options: ZLinkInstanceActivationAuthorityOptions) {}

  async read(target: ServiceInstanceActivationTarget): Promise<ServiceInstanceAuthorityRead> {
    const current = await this.options.store.readAuthority(authorityKey(target.targetSpotId));
    return current.kind === 'snapshot'
      ? readyRead(current, target)
      : { kind: 'missing' };
  }

  async reserve(
    activation: Omit<ServiceInstanceActivationRecoveryEnvelope, 'targetMeshName'>
  ): Promise<ServiceInstanceAuthorityReserve> {
    const target = activation.target;
    const owner = this.requireOwner();
    const requestBytes = encodeInstanceActivationRecoveryEnvelope({
      ...activation,
      targetMeshName: this.options.meshName
    });
    if (requestBytes.byteLength > MAX_CREATION_REQUEST_BYTES) {
      throw new RangeError('Encoded Instance activation request exceeds 1 MiB.');
    }
    const relocationStore = this.options.relocationStore;
    if (relocationStore === undefined) {
      throw new Error('Instance activation requires a Relocation Store.');
    }
    const requestHash = createHash('sha256').update(requestBytes).digest();
    const stored = await putNewRelocationBlob(
      relocationStore,
      requestBytes,
      CREATION_REQUEST_RETENTION_MS
    );
    const storedRead = await relocationStore.read(stored.reference);
    if (
      stored.reference.value.length === 0
      || stored.expiresAt.getTime() <= stored.storeNow.getTime()
      || storedRead.kind !== 'found'
      || crc32c(storedRead.bytes) !== crc32c(requestBytes)
      || !Buffer.from(storedRead.bytes).equals(requestBytes)
    ) {
      await this.deleteOrphan(stored.reference);
      throw new Error('Relocation Store returned an invalid creation request receipt.');
    }
    let reserved: Awaited<ReturnType<ZLinkObjectCreationStore['reserve']>>;
    try {
      for (;;) {
        reserved = await this.options.store.reserve({
          key: { kind: 'instance_spot', globalId: target.targetSpotId },
          intent: {
            stableType: target.stableType,
            requestContentReference: stored.reference.value,
            requestSha256: requestHash,
            requestEncodedSize: BigInt(requestBytes.byteLength)
          },
          target: {
            meshName: this.options.meshName,
            nodeRid: target.targetNodeRid,
            nodeLifecycleGeneration: target.targetNodeGeneration,
            owner
          },
          creatingPayload: encodeServiceInstanceAuthorityPayload({
            state: 'coldActivating',
            stableType: target.stableType,
            spotId: target.targetSpotId,
            ownerId: owner.ownerId,
            ownerLeaseGeneration: owner.leaseGeneration,
            ownerMeshName: this.options.meshName,
            ownerNodeRid: target.targetNodeRid,
            ownerNodeGeneration: target.targetNodeGeneration
          }),
          capacity: {
            actors: 0,
            spots: 1,
            spotType: {
              objectKind: 'instance_spot',
              stableType: target.stableType,
              count: 1
            }
          }
        });
        if (reserved.kind === 'conflict' || reserved.kind === 'alreadyExists') {
          const existingState = reserved.current.kind === 'snapshot'
            ? decodeServiceInstanceAuthorityPayload(reserved.current.payload)?.state
            : undefined;
          if (existingState === 'closing') {
            await this.awaitClosingRelease(target, activation.deadlineUnixMs);
            continue;
          }
        }
        if (reserved.kind !== 'alreadyExists') break;
        this.options.metrics?.recordInstanceSpotClaimConflict(
          this.options.meshName,
          target.stableType,
          'already_exists'
        );
        await this.deleteOrphan(stored.reference);
        return readyExisting(reserved.current, target);
      }
    } catch (error) {
      await this.deleteOrphan(stored.reference);
      throw error;
    }
    if (
      reserved.kind === 'conflict'
      && reserved.current.kind === 'snapshot'
      && reserved.current.allocation.objectKind === 'instance_spot'
      && reserved.current.allocation.stableType === target.stableType
      && reserved.current.allocation.state === 'reserved'
    ) {
      this.options.metrics?.recordInstanceSpotClaimConflict(
        this.options.meshName,
        target.stableType,
        'activation_in_progress'
      );
      await this.deleteOrphan(stored.reference);
      return await this.awaitConcurrentActivation(target, activation.deadlineUnixMs);
    }
    if (reserved.kind !== 'reserved') {
      this.options.metrics?.recordInstanceSpotClaimConflict(
        this.options.meshName,
        target.stableType,
        reserved.kind
      );
      await this.deleteOrphan(stored.reference);
      throw new Error(`Instance activation reservation failed: ${reserved.kind}.`);
    }
    const reservation: ServiceInstanceActivationReservation = {
      attempt: reserved.creating.objectGeneration,
      authorityOwnerGeneration: reserved.creating.authorityOwnerGeneration,
      token: reserved.reservationId
    };
    this.pending.set(reservation.token, {
      reservationId: reserved.reservationId,
      creating: reserved.creating,
      owner,
      requestReference: stored.reference,
      requestSha256: requestHash,
      requestEncodedSize: requestBytes.byteLength
    });
    return { kind: 'reserved', reservation };
  }

  async resume(
    target: ServiceInstanceActivationTarget,
    pending: ServicePendingInstanceActivation
  ): Promise<ServiceInstanceActivationReservation> {
    const current = await this.options.store.readAuthority(authorityKey(target.targetSpotId));
    const projection = current.kind === 'snapshot' ? current.pendingCreation : undefined;
    if (
      current.kind !== 'snapshot'
      || current.allocation.state !== 'reserved'
      || current.allocation.objectKind !== 'instance_spot'
      || current.allocation.stableType !== target.stableType
      || current.storeVersion.value !== pending.storeVersion
      || current.objectGeneration !== pending.objectGeneration
      || current.authorityOwnerGeneration !== pending.authorityOwnerGeneration
      || current.ownerId !== pending.ownerId
      || current.ownerLeaseGeneration !== pending.ownerLeaseGeneration
      || current.allocation.descriptor.meshName !== pending.meshName
      || !routingIdsEqual(current.allocation.descriptor.rid, pending.nodeRid)
      || current.allocation.descriptorLifecycleGeneration !== pending.nodeGeneration
      || projection?.reservationId !== pending.reservationId
      || projection.requestContentReference !== pending.requestReference
      || projection.requestEncodedSize !== pending.requestEncodedSize
      || !Buffer.from(projection.requestSha256).equals(Buffer.from(pending.requestSha256))
    ) {
      throw new Error('Pending Instance activation recovery fence is stale.');
    }
    const reservation = {
      attempt: current.objectGeneration,
      authorityOwnerGeneration: current.authorityOwnerGeneration,
      token: projection.reservationId
    };
    this.pending.set(reservation.token, {
      reservationId: projection.reservationId,
      creating: current,
      owner: {
        ownerId: current.ownerId,
        leaseGeneration: current.ownerLeaseGeneration
      },
      requestReference: relocationBlobReference(
        projection.requestContentReference
      ),
      requestSha256: Buffer.from(projection.requestSha256),
      requestEncodedSize: Number(projection.requestEncodedSize)
    });
    return reservation;
  }

  async commit(
    target: ServiceInstanceActivationTarget,
    reservation: ServiceInstanceActivationReservation,
    spot: ServiceSpotState
  ): Promise<{ readonly kind: 'committed' | 'lost'; readonly route: ServiceInstanceRouteFence }> {
    const pending = this.requirePending(reservation);
    requireCommitIdentity(
      target,
      reservation,
      pending.creating,
      spot,
      this.options.meshName
    );
    let result;
    try {
      result = await this.options.store.commit({
        key: { kind: 'instance_spot', globalId: target.targetSpotId },
        reservationId: pending.reservationId,
        expectedStoreVersion: pending.creating.storeVersion.value,
        target: {
          meshName: this.options.meshName,
          nodeRid: target.targetNodeRid,
          nodeLifecycleGeneration: target.targetNodeGeneration,
          owner: pending.owner
        },
        readyPayload: encodeServiceInstanceAuthorityPayload({
          state: 'ready',
          stableType: spot.stableType,
          spotId: spot.ref.spotId,
          ownerId: pending.owner.ownerId,
          ownerLeaseGeneration: pending.owner.leaseGeneration,
          ownerMeshName: this.options.meshName,
          ownerNodeRid: target.targetNodeRid,
          ownerNodeGeneration: target.targetNodeGeneration,
          activationRecovery: {
            reference: pending.requestReference.value,
            sha256: pending.requestSha256,
            encodedSize: pending.requestEncodedSize,
            inboxSequence: 1n,
            replayCursor: 0n
          }
        })
      });
    } catch (error) {
      if (error instanceof ZLinkFrameworkException) throw error;
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Instance Spot Ready commit Store operation failed.',
        false,
        error
      );
    }
    if (result.kind === 'committed' || result.kind === 'alreadyCommitted') {
      this.pending.delete(reservation.token);
      const route = routeFromSnapshot(result.ready);
      this.options.onReady?.(target, route);
      return { kind: 'committed', route };
    }
    const current = await this.options.store.readAuthority(authorityKey(target.targetSpotId));
    if (current.kind === 'snapshot') {
      const ready = readyRead(current, target);
      if (ready.kind === 'ready') {
        this.pending.delete(reservation.token);
        return { kind: 'lost', route: ready.route };
      }
    }
    throw new Error(`Instance activation commit failed: ${result.kind}.`);
  }

  async complete(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence
  ): Promise<ServiceInstanceRouteFence> {
    const key = authorityKey(target.targetSpotId);
    const current = await this.options.store.readAuthority(key);
    if (current.kind !== 'snapshot') {
      throw new Error('Instance activation authority disappeared before inbox release.');
    }
    const decoded = decodeServiceReadySpotAuthority(current.payload);
    if (
      decoded?.kind !== 'instance_spot'
      || decoded.spotId !== target.targetSpotId
      || decoded.stableType !== target.stableType
      || current.allocation.state !== 'active'
      || current.objectGeneration !== route.objectGeneration
      || current.authorityOwnerGeneration !== route.authorityOwnerGeneration
      || current.ownerId !== route.ownerId
      || current.ownerLeaseGeneration !== route.leaseGeneration
      || current.allocation.descriptorLifecycleGeneration !== route.targetNodeGeneration
      || !routingIdsEqual(current.allocation.descriptor.rid, route.targetNodeRid)
    ) {
      throw new Error('Instance activation authority changed before inbox release.');
    }
    const recovery = decoded.activationRecovery;
    if (recovery === undefined) return routeFromSnapshot(current);
    let completed = current;
    if (recovery.replayCursor < recovery.inboxSequence) {
      const cursorStored = await this.options.store.compareExchangeAuthority(
        key,
        current.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: encodeServiceInstanceAuthorityPayload({
            state: 'ready',
            stableType: decoded.stableType,
            spotId: decoded.spotId,
            ownerId: decoded.ownerId,
            ownerLeaseGeneration: decoded.ownerLeaseGeneration,
            ownerMeshName: decoded.ownerMeshName,
            ownerNodeRid: decoded.ownerNodeRid,
            ownerNodeGeneration: decoded.ownerNodeGeneration,
            activationRecovery: {
              ...recovery,
              replayCursor: recovery.inboxSequence
            }
          })
        }
      );
      if (cursorStored.kind !== 'stored') {
        throw new Error(
          `Instance activation terminal completion record failed: ${cursorStored.kind}.`
        );
      }
      const { kind: _, ...snapshot } = cursorStored;
      completed = { kind: 'snapshot', ...snapshot };
    }
    const completedPayload = decodeServiceReadySpotAuthority(completed.payload);
    const completedRecovery = completedPayload?.activationRecovery;
    if (
      completedPayload?.kind !== 'instance_spot'
      || completedRecovery === undefined
      || completedRecovery.replayCursor !== completedRecovery.inboxSequence
    ) {
      throw new Error('Instance activation terminal completion was not durably recorded.');
    }
    const stored = await this.options.store.compareExchangeAuthority(
      key,
      completed.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: encodeServiceInstanceAuthorityPayload({
          state: 'ready',
          stableType: completedPayload.stableType,
          spotId: completedPayload.spotId,
          ownerId: completedPayload.ownerId,
          ownerLeaseGeneration: completedPayload.ownerLeaseGeneration,
          ownerMeshName: completedPayload.ownerMeshName,
          ownerNodeRid: completedPayload.ownerNodeRid,
          ownerNodeGeneration: completedPayload.ownerNodeGeneration
        })
      }
    );
    if (stored.kind !== 'stored') {
      throw new Error(`Instance activation inbox release failed: ${stored.kind}.`);
    }
    await this.deleteOrphan(relocationBlobReference(recovery.reference));
    const { kind: _, ...snapshot } = stored;
    return routeFromSnapshot({ kind: 'snapshot', ...snapshot });
  }

  async abort(
    target: ServiceInstanceActivationTarget,
    reservation: ServiceInstanceActivationReservation
  ): Promise<void> {
    const pending = this.pending.get(reservation.token);
    if (pending === undefined) return;
    const result = await this.options.store.abort({
      key: { kind: 'instance_spot', globalId: target.targetSpotId },
      reservationId: pending.reservationId,
      expectedStoreVersion: pending.creating.storeVersion.value,
      target: {
        meshName: this.options.meshName,
        nodeRid: target.targetNodeRid,
        nodeLifecycleGeneration: target.targetNodeGeneration,
        owner: pending.owner
      }
    });
    if (result.kind === 'aborted' || result.kind === 'alreadyAborted') {
      this.pending.delete(reservation.token);
      await this.deleteOrphan(pending.requestReference);
    } else {
      throw new Error(`Instance activation abort failed: ${result.kind}.`);
    }
  }

  private requireOwner(): ZLinkLocationOwnerToken {
    const owner = this.options.owner();
    if (owner === undefined) throw new Error('Location owner lease is not ready.');
    return owner;
  }

  private async deleteOrphan(reference: ZLinkBlobReference): Promise<void> {
    try {
      await this.options.relocationStore?.delete(reference);
    } catch {
      // No authority publishes this root. Retention owns cleanup when
      // best-effort deletion fails.
    }
  }

  private async awaitConcurrentActivation(
    target: ServiceInstanceActivationTarget,
    deadlineUnixMs: bigint
  ): Promise<Extract<ServiceInstanceAuthorityRead, { readonly kind: 'ready' }>> {
    const key = authorityKey(target.targetSpotId);
    while (BigInt(Date.now()) <= deadlineUnixMs) {
      const current = await this.options.store.readAuthority(key);
      if (current.kind === 'snapshot') {
        if (
          current.allocation.objectKind !== 'instance_spot'
          || current.allocation.stableType !== target.stableType
        ) {
          throw new Error('Concurrent Instance activation resolved to a different Spot type.');
        }
        if (current.allocation.state === 'active') {
          return readyExisting(current, target);
        }
      } else {
        throw new Error('Concurrent Instance activation ended before the Ready barrier.');
      }
      await waitForActivationJoin(deadlineUnixMs);
    }
    throw new Error(
      `Concurrent Instance activation for '${target.targetSpotId}' did not reach Ready before deadline.`
    );
  }

  private async awaitClosingRelease(
    target: ServiceInstanceActivationTarget,
    deadlineUnixMs: bigint
  ): Promise<void> {
    const key = authorityKey(target.targetSpotId);
    while (BigInt(Date.now()) <= deadlineUnixMs) {
      const current = await this.options.store.readAuthority(key);
      if (current.kind !== 'snapshot') return;
      const decoded = decodeServiceInstanceAuthorityPayload(current.payload);
      if (
        current.allocation.objectKind !== 'instance_spot'
        || current.allocation.stableType !== target.stableType
        || decoded?.kind !== 'instance_spot'
        || decoded.spotId !== target.targetSpotId
      ) {
        throw new Error('Instance authority changed to a different Spot while closing.');
      }
      if (decoded.state !== 'closing') return;
      await waitForActivationJoin(deadlineUnixMs);
    }
    throw new Error(
      `Instance Spot '${target.targetSpotId}' close did not release authority before deadline.`
    );
  }

  private requirePending(reservation: ServiceInstanceActivationReservation): PendingReservation {
    const pending = this.pending.get(reservation.token);
    if (
      pending === undefined
      || pending.creating.objectGeneration !== reservation.attempt
    ) {
      throw new Error('Instance activation reservation is stale.');
    }
    return pending;
  }
}

function readyRead(
  snapshot: ZLinkAuthoritySnapshot,
  target: ServiceInstanceActivationTarget
): ServiceInstanceAuthorityRead {
  const decoded = decodeServiceReadySpotAuthority(snapshot.payload);
  const creating = decodeServiceInstanceAuthorityPayload(snapshot.payload);
  if (
    creating?.state === 'coldActivating'
    && snapshot.allocation.objectKind === 'instance_spot'
    && snapshot.allocation.state === 'reserved'
    && creating.stableType === target.stableType
    && creating.spotId === target.targetSpotId
    && creating.ownerId === snapshot.ownerId
    && creating.ownerLeaseGeneration === snapshot.ownerLeaseGeneration
    && creating.ownerMeshName === snapshot.allocation.descriptor.meshName
    && creating.ownerNodeGeneration === snapshot.allocation.descriptorLifecycleGeneration
    && routingIdsEqual(creating.ownerNodeRid, snapshot.allocation.descriptor.rid)
    && routingIdsEqual(target.targetNodeRid, snapshot.allocation.descriptor.rid)
    && target.targetNodeGeneration === snapshot.allocation.descriptorLifecycleGeneration
  ) {
    return {
      kind: 'creating',
      objectGeneration: snapshot.objectGeneration,
      authorityOwnerGeneration: snapshot.authorityOwnerGeneration
    };
  }
  if (
    decoded?.kind !== 'instance_spot'
    || decoded.stableType !== target.stableType
    || decoded.spotId !== target.targetSpotId
    || snapshot.allocation.objectKind !== 'instance_spot'
    || snapshot.allocation.state !== 'active'
    || decoded.ownerId !== snapshot.ownerId
    || decoded.ownerLeaseGeneration !== snapshot.ownerLeaseGeneration
    || decoded.ownerMeshName !== snapshot.allocation.descriptor.meshName
    || decoded.ownerNodeGeneration !== snapshot.allocation.descriptorLifecycleGeneration
    || !routingIdsEqual(decoded.ownerNodeRid, snapshot.allocation.descriptor.rid)
    || !routingIdsEqual(target.targetNodeRid, snapshot.allocation.descriptor.rid)
    || target.targetNodeGeneration !== snapshot.allocation.descriptorLifecycleGeneration
  ) {
    return { kind: 'missing' };
  }
  return { kind: 'ready', route: routeFromSnapshot(snapshot) };
}

function readyExisting(
  snapshot: ZLinkAuthoritySnapshot,
  target: ServiceInstanceActivationTarget
): Extract<ServiceInstanceAuthorityRead, { readonly kind: 'ready' }> {
  const decoded = decodeServiceReadySpotAuthority(snapshot.payload);
  if (
    decoded?.kind !== 'instance_spot'
    || decoded.stableType !== target.stableType
    || decoded.spotId !== target.targetSpotId
    || snapshot.allocation.objectKind !== 'instance_spot'
    || snapshot.allocation.state !== 'active'
    || decoded.ownerId !== snapshot.ownerId
    || decoded.ownerLeaseGeneration !== snapshot.ownerLeaseGeneration
    || decoded.ownerMeshName !== snapshot.allocation.descriptor.meshName
    || decoded.ownerNodeGeneration !== snapshot.allocation.descriptorLifecycleGeneration
    || !routingIdsEqual(decoded.ownerNodeRid, snapshot.allocation.descriptor.rid)
  ) {
    throw new Error('Existing Instance authority is not a matching Ready allocation.');
  }
  return { kind: 'ready', route: routeFromSnapshot(snapshot) };
}

async function waitForActivationJoin(deadlineUnixMs: bigint): Promise<void> {
  const remaining = Number(deadlineUnixMs - BigInt(Date.now()));
  if (remaining <= 0) return;
  await new Promise<void>((resolve) => {
    setTimeout(resolve, Math.min(ACTIVATION_JOIN_POLL_INTERVAL_MS, remaining));
  });
}

function requireCommitIdentity(
  target: ServiceInstanceActivationTarget,
  reservation: ServiceInstanceActivationReservation,
  creating: ZLinkAuthoritySnapshot,
  spot: ServiceSpotState,
  meshName: string
): void {
  if (
    creating.allocation.state !== 'reserved'
    || creating.allocation.objectKind !== 'instance_spot'
    || creating.allocation.stableType !== target.stableType
    || creating.objectGeneration !== reservation.attempt
    || !routingIdsEqual(creating.allocation.descriptor.rid, target.targetNodeRid)
    || creating.allocation.descriptor.meshName !== meshName
    || creating.allocation.descriptorLifecycleGeneration !== target.targetNodeGeneration
    || spot.kind !== 'instance'
    || spot.stableType !== target.stableType
    || spot.ref.spotId !== target.targetSpotId
    || spot.ref.generation !== reservation.attempt
    || spot.authorityOwnerGeneration !== creating.authorityOwnerGeneration
  ) {
    throw new Error('Instance activation Ready identity does not match its exact reservation.');
  }
}

function routeFromSnapshot(snapshot: ZLinkAuthoritySnapshot): ServiceInstanceRouteFence {
  const decoded = decodeServiceReadySpotAuthority(snapshot.payload);
  if (decoded?.kind !== 'instance_spot') {
    throw new Error('Instance authority is not Ready.');
  }
  return {
    targetNodeRid: String(snapshot.allocation.descriptor.rid),
    targetNodeGeneration: snapshot.allocation.descriptorLifecycleGeneration,
    targetSpotId: decoded.spotId,
    objectGeneration: snapshot.objectGeneration,
    ownerId: snapshot.ownerId,
    authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
    leaseGeneration: snapshot.ownerLeaseGeneration,
    storeVersion: snapshot.storeVersion.value
  };
}

function authorityKey(spotId: string): ZLinkAuthorityKey {
  return encodeAuthorityKey('instance_spot', spotId);
}

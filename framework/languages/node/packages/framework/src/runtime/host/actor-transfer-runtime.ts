import { createHash, randomUUID } from 'node:crypto';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId,
  ZLinkBlobReference,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkRelocationStore,
  ZLinkSpot
} from '../../contracts';
import type {
  ZLinkAggregateId,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkRelocationCapacityFence
} from '../../contracts/Locations';
import type {
  ZLinkAuthorityStore,
  ZLinkObjectCreationStore,
  ZLinkOwnerLeaseStore,
  ZLinkRelocationCapacityStore
} from '../locations/internal-store-contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import { putNewRelocationBlob, relocationBlobReference } from '../locations/relocation-blob';
import { crc32c } from '../foundation/service-relocation-runtime';
import { ZLinkSpotKind } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef, ZLinkBackendMeshNode } from '../backend';
import {
  ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
  toFrameworkActorRef,
  mergeRemoteBoundSessionTarget,
  preferredRemoteBoundSessionTarget,
  type ZLinkActorHandoffCoordinator,
  type ZLinkActorHandoffDispatch,
  type ZLinkActorRoutedJoinTransport,
  type ZLinkActorTransferRegistry,
  ZLinkDeferredJoinAcceptedJournal,
  rewriteActorAuthorityRoute,
  ZLinkBoundSessionAcceptedJournal,
  type ZLinkBoundSessionAcceptedJournalRoot,
  type ZLinkDeferredJoinAcceptedRoot,
  type ZLinkDeferredJoinRecoveryInput,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { ZLinkActorRuntimeState } from '../actors/actor-runtime-state';
import { ZLinkActorRetryDelay } from '../actors/actor-retry-delay';
import { encodeRoutingIdStorageHex } from '../routing-id';
import { encodeRemoteActorPacketTarget } from '../actors/actor-packet-relay-wire';
import {
  decodeRemoteBoundSessionOwnershipAck,
  decodeRemoteBoundSessionSealAck,
  encodeRemoteBoundSessionOwnershipPayload,
  encodeRemoteBoundSessionSealPayload,
  ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET
} from '../actors/bound-session-wire';
import type { ZLinkLocationLifecycle } from '../locations';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import type { DefaultZLinkSpotManager } from '../spots';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type {
  ZLinkActorHandoffPacket,
  ZLinkActorHandoffResult,
  ZLinkActorHandoffTerminalAcceptance,
  ZLinkActorHandoffTerminalAck
} from '../actors/actor-handoff';
import type { ZLinkNativeActorJoinSnapshot } from '../spots/spot-runtime-ports';
import {
  ownerFence,
  type ZLinkActorMessageFollowOwnerFence
} from '../actors/actor-message-follow-context';

export interface ZLinkActorTransferRuntimeActorManager {
  getState(actorId: string): ZLinkActorRuntimeState | undefined;
  getOrCreateActor(actorId: string, actorType: string, signal?: AbortSignal): Promise<ZLinkActor>;
  getOrCreateWithNativeRef(
    actorId: string,
    actorType: string,
    actorRef: ZLinkBackendActorRef,
    actorCreateRequest?: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkActor>;
  materializeTransferredActor(
    actorId: string,
    actorType: string,
    adapterKey: string | undefined,
    transferState: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<{ readonly actor: ZLinkActor; readonly actorRef: ActorRef }>;
  prepareRelocationActor(
    actorId: string,
    actorType: string,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    signal?: AbortSignal,
    transfer?: {
      readonly adapterKey: string;
      readonly state: ZLinkMessage;
    }
  ): Promise<ZLinkActor>;
  publishRelocationActor(actorId: string): void;
  rollbackTransferredActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void>;
  completeCoreRelocationSource(actorId: string): Promise<void>;
}

export interface ZLinkActorTransferRuntimeOptions {
  readonly routeTransport: ZLinkActorRoutedJoinTransport;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly actorManager: () => ZLinkActorTransferRuntimeActorManager | undefined;
  readonly primaryMeshNode: () => ZLinkBackendMeshNode;
  readonly notifyEntrySpotActorLeft: (actor: ZLinkActor, signal?: AbortSignal) => Promise<void>;
  readonly restoreEntrySpotActorJoined: (actor: ZLinkActor, signal?: AbortSignal) => Promise<void>;
  readonly locationLifecycle: () => ZLinkLocationLifecycle | undefined;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly actorTransferRegistry: ZLinkActorTransferRegistry;
  readonly authorityStore: () => (
    ZLinkAuthorityStore
    & ZLinkObjectCreationStore
    & ZLinkOwnerLeaseStore
    & ZLinkRelocationCapacityStore
  ) | undefined;
  readonly relocationStore: () => ZLinkRelocationStore | undefined;
  readonly clearRemoteActorPacketTarget: (actorId: string) => void;
  readonly reportPostCommitError?: (error: unknown) => void;
  readonly onSourceDepartureCompleted?: (actorId: string) => void;
  readonly shutdownSignal?: () => AbortSignal | undefined;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
}

export class ZLinkActorTransferRuntime {
  private readonly sourceDepartureTasks = new Map<string, Promise<void>>();
  private readonly coreSourceLeaves = new Map<string, {
    readonly promise: Promise<void>;
    readonly resolve: () => void;
    readonly reject: (error: unknown) => void;
  }>();

  constructor(private readonly options: ZLinkActorTransferRuntimeOptions) {}

  beginDeferredActorHandoff(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: string
  ): void {
    const actorId = actor.context.actorId;
    this.options.actorHandoff.beginProvisional(
      actorId,
      operationId,
      state.nativeActorRef?.generation ?? 0n,
      state.nativeActorRef === undefined ? undefined : String(state.nativeActorRef.nodeRid),
      state.locationGeneration ?? 1n,
      state.ownerLeaseGeneration
    );
  }

  promoteDeferredActorHandoff(
    actor: ZLinkActor,
    _state: ZLinkActorRuntimeState,
    operationId: string
  ): void {
    this.options.actorHandoff.promoteProvisional(actor.context.actorId, operationId);
  }

  async completeDeferredActorHandoff(
    actor: ZLinkActor,
    target: ZLinkSpotRouteTarget,
    _targetActorRef: ActorRef,
    operationId: string
  ): Promise<void> {
    const actorId = actor.context.actorId;
    if (!this.options.actorHandoff.isProvisional(actorId)) return;
    const manager = this.options.spotManager();
    if (manager === undefined) {
      throw new Error(`Actor '${actorId}' same-node Join has no local Spot manager.`);
    }
    const dispatch: ZLinkActorHandoffDispatch = (
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    ) => manager.dispatchRoutedActorPacket(
      target.spotId,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    );
    await this.options.actorHandoff.releaseDeferred(actorId, operationId, dispatch);
  }

  async cancelDeferredActorHandoff(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: string
  ): Promise<void> {
    await this.options.actorHandoff.releaseDeferred(
      actor.context.actorId,
      operationId,
      this.sourceHandoffReplay(actor, state)
    );
  }

  async prepareDeferredJoinAccepted(
    actorId: string,
    operationId: ZLinkActorJoinOperationId,
    actorRef: ActorRef,
    rawReply: Uint8Array,
    signal?: AbortSignal,
    recovery?: ZLinkDeferredJoinRecoveryInput
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return await this.requireDeferredJoinJournal().prepare(
      actorId,
      operationId,
      actorRef,
      rawReply,
      signal,
      recovery
    );
  }

  async recoverDeferredJoinAccepted(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot | undefined> {
    const authority = this.options.authorityStore();
    const relocation = this.options.relocationStore();
    if (authority === undefined || relocation === undefined) return undefined;
    return await new ZLinkDeferredJoinAcceptedJournal(authority, relocation)
      .recover(actorId, signal);
  }

  async discardDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    await this.requireDeferredJoinJournal().discardPrepared(root, signal);
  }

  async markDeferredJoinRecoveryMessageReplayed(
    root: ZLinkDeferredJoinAcceptedRoot,
    nextReplayCursor: number,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return await this.requireDeferredJoinJournal().markRecoveryMessageReplayed(
      root,
      nextReplayCursor,
      signal
    );
  }

  markDeferredJoinAcceptedCommitted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return this.requireDeferredJoinJournal().markCommitted(root, actorRef, signal);
  }

  async readDeferredJoinRecoveryPayload(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<Buffer> {
    return await this.requireDeferredJoinJournal().readRecoveryPayload(root, signal);
  }

  async takeOverDeferredJoinRecoveryAuthority(
    root: ZLinkDeferredJoinAcceptedRoot,
    targetActorRef: ActorRef,
    target: {
      readonly meshName: string;
      readonly nodeRid: RoutingId;
      readonly nodeGeneration: bigint;
      readonly owner: ZLinkLocationOwnerToken;
      readonly spotId: string;
      readonly spotGeneration: bigint;
      readonly membershipEpoch: bigint;
      readonly spotAuthority: ZLinkAuthoritySnapshot;
      readonly spotAuthorityPayload: Uint8Array;
    },
    signal?: AbortSignal
  ): Promise<{
    readonly root: ZLinkDeferredJoinAcceptedRoot;
    readonly actorAuthority: ZLinkAuthoritySnapshot;
    readonly spotAuthority: ZLinkAuthoritySnapshot;
  } | undefined> {
    const authority = this.options.authorityStore();
    if (authority === undefined) {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery authority store is unavailable.`
      );
    }
    const key = encodeAuthorityKey('actor', root.actor.actorId);
    const [current, currentSpot] = await Promise.all([
      authority.readAuthority(key, signal),
      authority.readAuthority(encodeAuthorityKey('user_spot', target.spotId), signal)
    ]);
    if (current.kind !== 'snapshot' || currentSpot.kind !== 'snapshot') {
      throw new Error(`Actor '${root.actor.actorId}' recovery authority is missing.`);
    }
    const published = await this.requireDeferredJoinJournal().recover(
      root.actor.actorId,
      signal
    );
    if (
      published === undefined
      || published.operationId.high !== root.operationId.high
      || published.operationId.low !== root.operationId.low
      || published.actor.objectGeneration !== root.actor.objectGeneration
    ) {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery authority references another operation.`
      );
    }
    const actorIsLocal =
      String(current.allocation.descriptor.rid) === String(target.nodeRid)
      && current.allocation.descriptorLifecycleGeneration === target.nodeGeneration
      && current.ownerId === target.owner.ownerId
      && current.ownerLeaseGeneration === target.owner.leaseGeneration;
    const spotIsLocal =
      String(currentSpot.allocation.descriptor.rid) === String(target.nodeRid)
      && currentSpot.allocation.descriptorLifecycleGeneration === target.nodeGeneration
      && currentSpot.ownerId === target.owner.ownerId
      && currentSpot.ownerLeaseGeneration === target.owner.leaseGeneration;
    if (actorIsLocal && spotIsLocal) {
      return {
        root: published,
        actorAuthority: current,
        spotAuthority: currentSpot
      };
    }

    for (const candidate of [
      ...(actorIsLocal ? [] : [current]),
      ...(spotIsLocal ? [] : [currentSpot])
    ]) {
      const sourceLease = await authority.readOwnerLease(candidate.ownerId, signal);
      if (
        sourceLease.kind === 'found'
        && sourceLease.token.leaseGeneration === candidate.ownerLeaseGeneration
        && sourceLease.leaseExpiresAt.getTime() > sourceLease.storeNow.getTime()
      ) {
        return undefined;
      }
    }

    const membershipMutation = Buffer.from(JSON.stringify({
      actorId: root.actor.actorId,
      actorGeneration: root.actor.objectGeneration.toString(),
      spotId: target.spotId,
      spotGeneration: target.spotGeneration.toString(),
      membershipEpoch: target.membershipEpoch.toString()
    }), 'utf8');
    const aggregateId = {
      value: `deferred-join:${root.operationId.high.toString(16)}:${root.operationId.low.toString(16)}`
    } as ZLinkAggregateId;
    const capacity = {
      actors: actorIsLocal ? 0 : current.allocation.capacity.actors,
      spots: spotIsLocal ? 0 : currentSpot.allocation.capacity.spots,
      ...(spotIsLocal || currentSpot.allocation.capacity.spotType === undefined
        ? {}
        : { spotType: currentSpot.allocation.capacity.spotType })
    };
    const prepared = await authority.prepareAggregate({
      aggregateId,
      aggregateGeneration: target.nodeGeneration,
      participants: [
        {
          authorityKey: key,
          expectedStoreVersion: current.storeVersion,
          ownerTransition: actorIsLocal ? 'preserve' : 'newOwner',
          authorityPayload: rewriteActorAuthorityRoute(
            current.payload,
            targetActorRef,
            target.spotId,
            target.spotGeneration,
            target.nodeGeneration,
            target.owner
          ),
          membershipMutation
        },
        {
          authorityKey: encodeAuthorityKey('user_spot', target.spotId),
          expectedStoreVersion: currentSpot.storeVersion,
          ownerTransition: spotIsLocal ? 'preserve' : 'newOwner',
          authorityPayload: spotIsLocal
            ? currentSpot.payload
            : target.spotAuthorityPayload,
          membershipMutation
        }
      ],
      inventoryDigest: createHash('sha256').update(membershipMutation).digest(),
      targetDescriptor: {
        meshName: target.meshName,
        rid: target.nodeRid
      },
      targetDescriptorLifecycleGeneration: target.nodeGeneration,
      capacity,
      targetOwner: target.owner
    }, signal);
    if (prepared.kind === 'conflict' || prepared.kind === 'stale') {
      // Startup publishes the local descriptor as Preparing before it opens
      // application admission. Aggregate placement accepts only Serving
      // targets, so the periodic authority reconciliation retries after the
      // runtime publishes Serving. A concurrent replacement can produce the
      // same outcomes and is handled by that same retry.
      return undefined;
    }
    if (prepared.kind === 'generationExhausted') {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery aggregate prepare failed with '${prepared.kind}'.`
      );
    }
    let committed = false;
    try {
      const result = await authority.commitAggregate(prepared.fence, signal);
      if (result.kind !== 'committed' && result.kind !== 'alreadyCommitted') {
        throw new Error(
          `Actor '${root.actor.actorId}' recovery authority takeover was rejected.`
        );
      }
      committed = true;
      const [actorAuthority, spotAuthority, recovered] = await Promise.all([
        authority.readAuthority(key, signal),
        authority.readAuthority(encodeAuthorityKey('user_spot', target.spotId), signal),
        this.requireDeferredJoinJournal().recover(root.actor.actorId, signal)
      ]);
      if (
        actorAuthority.kind !== 'snapshot'
        || spotAuthority.kind !== 'snapshot'
        || recovered === undefined
        || String(actorAuthority.allocation.descriptor.rid) !== String(target.nodeRid)
        || actorAuthority.allocation.descriptorLifecycleGeneration !== target.nodeGeneration
        || String(spotAuthority.allocation.descriptor.rid) !== String(target.nodeRid)
        || spotAuthority.allocation.descriptorLifecycleGeneration !== target.nodeGeneration
      ) {
        throw new Error(
          `Actor '${root.actor.actorId}' recovery aggregate did not converge.`
        );
      }
      return { root: recovered, actorAuthority, spotAuthority };
    } finally {
      if (!committed) {
        await authority.abortAggregate(prepared.fence, signal)
          .catch(() => undefined);
      }
    }
  }

  async commitAndDeliverDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor: ZLinkActor,
    actorRef: ActorRef,
    submitMailbox: <T>(operation: () => Promise<T>) => Promise<T>,
    signal?: AbortSignal,
    retainRecoveryRoot = false
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    const targetActorRef = this.options.actorManager()
      ?.getState(actor.context.actorId)
      ?.nativeActorRef;
    const currentActorRef = targetActorRef === undefined
      ? actorRef
      : toFrameworkActorRef(targetActorRef, actor.context.meshName);
    let current = root.cursor === 'prepared'
      ? await this.requireDeferredJoinJournal().markCommitted(root, currentActorRef, signal)
      : root;
    let lastError: unknown;
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        current = await this.requireDeferredJoinJournal().deliver(
          current,
          actor,
          currentActorRef,
          submitMailbox,
          signal,
          retainRecoveryRoot
        );
        return current;
      } catch (error) {
        lastError = error;
        if (attempt < 2) {
          await new Promise<void>((resolve, reject) => {
            const timer = setTimeout(resolve, 10 << attempt);
            timer.unref();
            signal?.addEventListener('abort', () => {
              clearTimeout(timer);
              reject(signal.reason);
            }, { once: true });
          });
          current = await this.requireDeferredJoinJournal().recover(
            currentActorRef.actorId,
            signal
          ) ?? current;
        }
      }
    }
    throw lastError;
  }

  releaseDeferredJoinRecovery(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    return this.requireDeferredJoinJournal().releaseRecovery(root, signal);
  }

  private requireDeferredJoinJournal(): ZLinkDeferredJoinAcceptedJournal {
    const authority = this.options.authorityStore();
    const relocation = this.options.relocationStore();
    if (authority === undefined || relocation === undefined) {
      throw new Error('Cross-node deferred Actor Join requires Location and Relocation Stores.');
    }
    return new ZLinkDeferredJoinAcceptedJournal(authority, relocation);
  }

  private async prepareSourceActorLeave(
    actor: ZLinkActor,
    sourceSpotId: RoutingId | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    if (sourceSpotId !== undefined) {
      const manager = this.options.spotManager();
      if (manager !== undefined) {
        await manager.prepareActorLeaveForTransfer(sourceSpotId, actor, signal);
        return;
      }
    }
    await this.options.notifyEntrySpotActorLeft(actor, signal);
  }

  private async restoreSourceActor(
    actor: ZLinkActor,
    sourceSpotId: RoutingId | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    if (sourceSpotId !== undefined) {
      const manager = this.options.spotManager();
      if (manager !== undefined) {
        await manager.restoreActorAfterFailedTransfer(sourceSpotId, actor, signal);
        return;
      }
    }
    await this.options.restoreEntrySpotActorJoined(actor, signal);
  }

  private async beginSourceActorMove(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    deferredOperationId?: string
  ): Promise<void> {
    if (deferredOperationId !== undefined && this.options.actorHandoff.isProvisional(actor.context.actorId)) {
      this.promoteDeferredActorHandoff(actor, state, deferredOperationId);
    }
    state.beginMove();
    if (!this.options.actorHandoff.isActive(actor.context.actorId)) {
      this.options.actorHandoff.begin(
        actor.context.actorId,
        state.nativeActorRef?.generation ?? 0n,
        state.nativeActorRef === undefined ? undefined : String(state.nativeActorRef.nodeRid),
        state.locationGeneration ?? 1n,
        state.ownerLeaseGeneration
      );
    }
    try {
      if (state.spotId !== undefined) {
        await this.options.spotManager()?.beginActorTransfer(state.spotId, actor.context.actorId);
      }
    } catch (error) {
      if (deferredOperationId === undefined) {
        this.options.actorHandoff.cancel(actor.context.actorId);
      } else {
        await this.options.actorHandoff.releaseDeferred(
          actor.context.actorId,
          deferredOperationId,
          this.sourceHandoffReplay(actor, state)
        )
          .catch(() => undefined);
      }
      state.endMove();
      throw error;
    }
  }

  private async cancelSourceActorMove(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    deferredOperationId?: string
  ): Promise<void> {
    try {
      if (state.spotId !== undefined) {
        await this.options.spotManager()?.cancelActorTransfer(state.spotId, actor.context.actorId);
      }
    } finally {
      if (deferredOperationId === undefined) {
        this.options.actorHandoff.cancel(actor.context.actorId);
      } else {
        await this.options.actorHandoff.releaseDeferred(
          actor.context.actorId,
          deferredOperationId,
          this.sourceHandoffReplay(actor, state)
        );
      }
      state.endMove();
    }
  }

  private sourceHandoffReplay(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState
  ): ZLinkActorHandoffDispatch | undefined {
    const sourceSpotId = state.spotId;
    const manager = this.options.spotManager();
    if (sourceSpotId === undefined || manager === undefined) {
      return undefined;
    }
    return (
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    ) => manager.dispatchRoutedActorPacket(
      sourceSpotId,
      actor.context.actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    );
  }

  async prepareSource(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    signal?: AbortSignal,
    lifecycleAuthority: 'framework' | 'core' = 'framework',
    deferredOperationId?: string
  ) {
    await this.beginSourceActorMove(actor, state, deferredOperationId);
    const relocationMetric = state.meshName === undefined
      ? undefined
      : this.options.metrics?.startRelocation(
          state.meshName,
          'actor',
          this.options.actorTransferRegistry.policy(state.actorType)
        );
    const sourceSpotId = state.spotId;
    let sourceLeaveStarted = false;
    let sealId: string | undefined;
    let acceptedRoot: ZLinkBoundSessionAcceptedJournalRoot | undefined;
    try {
      if (state.remoteBoundSessionTarget !== undefined) {
        sealId = randomUUID();
        const sealedTarget = await this.sealBoundSessionRoute(actor, state, sealId, signal);
        state.setRemoteBoundSessionTarget(sealedTarget);
      }
      const transfer = await this.options.actorTransferRegistry.transferOut(
        actor,
        state.actorType,
        signal
      );
      const transferBytes = transfer.state.toEncodedPayload().toBytes();
      let transferStateReference: ZLinkBlobReference | undefined;
      let transferStateChecksumCrc32c: number | undefined;
      if (transfer.adapterKey !== undefined) {
        const relocationStore = this.options.relocationStore();
        if (relocationStore === undefined) {
          throw new Error('Cross-node Actor relocation requires a Relocation Store.');
        }
        transferStateReference = (await putNewRelocationBlob(
          relocationStore,
          transferBytes,
          24 * 60 * 60 * 1_000,
          signal
        )).reference;
        transferStateChecksumCrc32c = crc32c(transferBytes);
      }
      relocationMetric?.recordBytes(transfer.state.toEncodedPayload().toBytes().byteLength);
      if (lifecycleAuthority === 'framework') {
        sourceLeaveStarted = true;
        await this.prepareSourceActorLeave(actor, sourceSpotId, signal);
      }
      const sourceLeaveCompletion = lifecycleAuthority === 'core'
        ? this.beginCoreSourceLeave(actor.context.actorId)
        : undefined;
      const handoffBacklog = lifecycleAuthority === 'core'
        ? this.options.actorHandoff.snapshotCoreBacklog(actor.context.actorId)
        : this.options.actorHandoff.snapshot(actor.context.actorId);
      if (sealId !== undefined) {
        acceptedRoot = await this.prepareBoundSessionAcceptedJournal(actor, state, sealId, handoffBacklog, signal);
        state.setRemoteBoundSessionTarget({
          ...state.remoteBoundSessionTarget!,
          relocationSealId: sealId,
          acceptedJournalReference: acceptedRoot.reference.value,
          acceptedJournalChecksumCrc32c: acceptedRoot.checksumCrc32c
        });
      }
      let phase: 'prepared' | 'committed' | 'rolledBack' = 'prepared';
      let authorityReservation: {
        readonly snapshot: ZLinkAuthoritySnapshot;
        readonly fence: ZLinkRelocationCapacityFence;
      } | undefined;
      let committedTargetOwnerFence:
        ZLinkActorMessageFollowOwnerFence | undefined;
      return {
        ...transfer,
        stateReference: transferStateReference?.value,
        stateChecksumCrc32c: transferStateChecksumCrc32c,
        handoffBacklog,
        sourceLeaveCompletion,
        reserveTarget: async (target: ZLinkSpotRouteTarget, reserveSignal?: AbortSignal) => {
          if (authorityReservation !== undefined) return;
          const authority = this.options.authorityStore();
          const actorType = state.actorType;
          if (
            authority === undefined
            || actorType === undefined
            || target.targetOwnerId === undefined
            || target.ownerLeaseGeneration === undefined
            || target.targetNodeGeneration === undefined
          ) {
            throw new Error(
              `Actor '${actor.context.actorId}' relocation target has no complete authority fence.`
            );
          }
          const key = encodeAuthorityKey('actor', actor.context.actorId);
          const snapshot = await authority.readAuthority(key, reserveSignal);
          if (snapshot.kind !== 'snapshot') {
            throw new Error(`Actor '${actor.context.actorId}' authority is missing before relocation.`);
          }
          const reservationId = randomUUID();
          const reserved = await authority.reserveRelocationCapacity({
            reservationId,
            authorityKey: key,
            expectedStoreVersion: snapshot.storeVersion,
            objectKind: 'actor',
            stableType: actorType,
            sourceDescriptor: snapshot.allocation.descriptor,
            sourceNodeLifecycleGeneration: snapshot.allocation.descriptorLifecycleGeneration,
            sourceOwner: {
              ownerId: snapshot.ownerId,
              leaseGeneration: snapshot.ownerLeaseGeneration
            },
            targetDescriptor: {
              meshName: snapshot.allocation.descriptor.meshName,
              rid: target.targetNodeRid
            },
            targetNodeLifecycleGeneration: target.targetNodeGeneration,
            targetOwner: {
              ownerId: target.targetOwnerId,
              leaseGeneration: target.ownerLeaseGeneration
            },
            capacity: snapshot.allocation.capacity
          }, reserveSignal);
          if (reserved.kind !== 'reserved' && reserved.kind !== 'alreadyReserved') {
            throw new Error(
              `Actor '${actor.context.actorId}' relocation capacity reservation failed with '${reserved.kind}'.`
            );
          }
          authorityReservation = { snapshot, fence: reserved.fence };
        },
        commitAuthority: async (
          target: ZLinkSpotRouteTarget,
          targetActorRef: ActorRef,
          commitSignal?: AbortSignal
        ) => {
          await (authorityReservation === undefined
            ? (async () => {
                throw new Error(`Actor '${actor.context.actorId}' relocation capacity was not reserved.`);
              })()
            : Promise.resolve());
          const reservation = authorityReservation!;
          const authority = this.options.authorityStore()!;
          const latest = await authority.readAuthority(
            encodeAuthorityKey('actor', actor.context.actorId),
            commitSignal
          );
          if (latest.kind !== 'snapshot') {
            throw new Error(`Actor '${actor.context.actorId}' authority disappeared before relocation commit.`);
          }
          const result = await authority.compareExchangeAuthority(
            encodeAuthorityKey('actor', actor.context.actorId),
            latest.storeVersion,
            {
              kind: 'put',
              generationTransition: 'newOwner',
              targetOwner: {
                ownerId: target.targetOwnerId!,
                leaseGeneration: target.ownerLeaseGeneration!
              },
              relocationCapacityFence: reservation.fence,
              payload: rewriteActorAuthorityRoute(
                latest.payload,
                targetActorRef,
                String(target.spotId),
                target.targetSpotGeneration,
                target.targetNodeGeneration,
                {
                  ownerId: target.targetOwnerId!,
                  leaseGeneration: target.ownerLeaseGeneration!
                }
              )
            },
            commitSignal
          );
          if (
            result.kind !== 'stored'
            || String(result.allocation.descriptor.rid) !== String(target.targetNodeRid)
            || result.objectGeneration !== targetActorRef.objectGeneration
          ) {
            const detail = result.kind === 'stored'
              ? `stored node=${String(result.allocation.descriptor.rid)} generation=${result.objectGeneration}`
              : result.kind;
            throw new Error(
              `Actor '${actor.context.actorId}' relocation authority commit was rejected (${detail}; `
              + `expected node=${String(target.targetNodeRid)} generation=${targetActorRef.objectGeneration}).`
            );
          }
          committedTargetOwnerFence = ownerFence({
            ownerId: result.ownerId,
            ownerLeaseGeneration: result.ownerLeaseGeneration,
            nodeRid: String(result.allocation.descriptor.rid),
            nodeGeneration: result.allocation.descriptorLifecycleGeneration,
            authorityOwnerGeneration: result.authorityOwnerGeneration
          });
        },
        commit: (
          target: Parameters<ZLinkActorHandoffCoordinator['complete']>[1],
          targetActorRef: ActorRef,
          results: Parameters<ZLinkActorHandoffCoordinator['complete']>[3],
          releaseLocation = true
        ) => {
          if (phase !== 'prepared') return;
          phase = 'committed';
          relocationMetric?.complete('completed');
          try {
            this.options.actorHandoff.complete(
              actor.context.actorId,
              target,
              targetActorRef,
              results,
              committedTargetOwnerFence
            );
          } catch (error) {
            // The target has already committed. Local Message Follow setup is now
            // post-commit work and must not turn the accepted transfer into a
            // source rollback that can no longer undo the target.
            this.options.reportPostCommitError?.(error);
          } finally {
            const releaseSourceLocation = lifecycleAuthority === 'core' && releaseLocation;
            if (releaseSourceLocation && sourceLeaveCompletion !== undefined) {
              // Core sends the source leave control after the authority
              // transition. Keep the source shell available for that
              // callback, then release the old location and registry entry.
              void sourceLeaveCompletion.then(
                () => this.scheduleSourceDeparture(actor, sourceSpotId, true),
                error => {
                  this.options.reportPostCommitError?.(error);
                  this.scheduleSourceDeparture(actor, sourceSpotId, true);
                }
              );
            } else {
              this.scheduleSourceDeparture(actor, sourceSpotId, releaseSourceLocation);
            }
          }
        },
        rollback: async () => {
          if (phase !== 'prepared') return;
          phase = 'rolledBack';
          relocationMetric?.complete('aborted');
          this.coreSourceLeaves.delete(actor.context.actorId);
          if (sealId !== undefined) {
            await this.abortBoundSessionRouteSeal(actor, state, sealId);
          }
          if (acceptedRoot !== undefined) {
            await this.boundSessionAcceptedJournal()?.delete(acceptedRoot);
          }
          if (authorityReservation !== undefined) {
            await this.options.authorityStore()?.abortRelocationCapacity(
              authorityReservation.fence,
              signal
            );
          }
          if (transferStateReference !== undefined) {
            await this.options.relocationStore()?.delete(transferStateReference, signal);
          }
          await this.cancelSourceActorMove(actor, state, deferredOperationId);
          await this.restoreSourceActor(actor, sourceSpotId);
        }
      };
    } catch (error) {
      relocationMetric?.complete('failed');
      try {
        if (sealId !== undefined) {
          await this.abortBoundSessionRouteSeal(actor, state, sealId);
        }
        if (acceptedRoot !== undefined) {
          await this.boundSessionAcceptedJournal()?.delete(acceptedRoot);
        }
        await this.cancelSourceActorMove(actor, state, deferredOperationId);
        if (sourceLeaveStarted) await this.restoreSourceActor(actor, sourceSpotId);
      } catch (rollbackError) {
        throw new AggregateError([error, rollbackError], 'Actor source leave and rollback both failed.');
      }
      throw error;
    }
  }

  async readPreparedTransferState(
    reference: string,
    checksumCrc32c: number,
    signal?: AbortSignal
  ): Promise<Buffer> {
    const store = this.options.relocationStore();
    if (store === undefined) {
      throw new Error('Cross-node Actor relocation requires a Relocation Store.');
    }
    const read = await store.read(relocationBlobReference(reference), signal);
    if (read.kind !== 'found' || crc32c(read.bytes) !== checksumCrc32c) {
      throw new Error('Actor relocation state reference is missing or corrupt.');
    }
    return Buffer.from(read.bytes);
  }

  async prepareMaintenanceSession(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    signal?: AbortSignal,
    manageMembership = true
  ): Promise<{
    readonly target?: ZLinkRemoteBoundSessionTarget;
    readonly handoffBacklog: readonly import('../actors').ZLinkActorHandoffPacket[];
    setReplayResults(results: readonly import('../actors').ZLinkActorHandoffResult[]): void;
    commit(target: ZLinkSpotRouteTarget, targetActorRef: ActorRef): Promise<void>;
    rollback(): Promise<void>;
  }> {
    if (manageMembership) {
      await this.beginSourceActorMove(actor, state);
    } else {
      state.beginMove();
      this.options.actorHandoff.begin(
        actor.context.actorId,
        state.nativeActorRef?.generation ?? 0n,
        state.nativeActorRef === undefined ? undefined : String(state.nativeActorRef.nodeRid),
        state.locationGeneration ?? 1n,
        state.ownerLeaseGeneration
      );
    }
    let acceptedRoot: ZLinkBoundSessionAcceptedJournalRoot | undefined;
    let sealId: string | undefined;
    try {
      if (state.remoteBoundSessionTarget !== undefined) {
        sealId = randomUUID();
        state.setRemoteBoundSessionTarget(
          await this.sealBoundSessionRoute(actor, state, sealId, signal)
        );
      }
      const handoffBacklog = this.options.actorHandoff.snapshot(actor.context.actorId);
      if (sealId !== undefined) {
        acceptedRoot = await this.prepareBoundSessionAcceptedJournal(
          actor,
          state,
          sealId,
          handoffBacklog,
          signal
        );
        state.setRemoteBoundSessionTarget({
          ...state.remoteBoundSessionTarget!,
          relocationSealId: sealId,
          acceptedJournalReference: acceptedRoot.reference.value,
          acceptedJournalChecksumCrc32c: acceptedRoot.checksumCrc32c
        });
      }
      let terminal: 'prepared' | 'committed' | 'rolledBack' = 'prepared';
      let replayResults: readonly import('../actors').ZLinkActorHandoffResult[] = [];
      return {
        target: state.remoteBoundSessionTarget,
        handoffBacklog,
        setReplayResults: results => {
          if (terminal === 'prepared') replayResults = [...results];
        },
        commit: async (target, targetActorRef) => {
          if (terminal === 'rolledBack') return;
          if (terminal === 'prepared') {
            this.options.actorHandoff.complete(
              actor.context.actorId,
              target,
              targetActorRef,
              replayResults
            );
            if (manageMembership && state.spotId !== undefined) {
              await this.options.spotManager()
                ?.commitActorLeaveAfterTransfer(state.spotId, actor.context.actorId);
            }
            state.endMove();
            terminal = 'committed';
          }
          if (acceptedRoot !== undefined) {
            await this.boundSessionAcceptedJournal()?.delete(acceptedRoot);
            acceptedRoot = undefined;
          }
        },
        rollback: async () => {
          if (terminal !== 'prepared') return;
          terminal = 'rolledBack';
          if (sealId !== undefined) {
            await this.abortBoundSessionRouteSeal(actor, state, sealId);
          }
          if (acceptedRoot !== undefined) {
            await this.boundSessionAcceptedJournal()?.delete(acceptedRoot);
          }
          if (manageMembership) {
            await this.cancelSourceActorMove(actor, state);
          } else {
            this.options.actorHandoff.cancel(actor.context.actorId);
            state.endMove();
          }
        }
      };
    } catch (error) {
      if (sealId !== undefined) {
        await this.abortBoundSessionRouteSeal(actor, state, sealId).catch(() => undefined);
      }
      if (acceptedRoot !== undefined) {
        await this.boundSessionAcceptedJournal()?.delete(acceptedRoot).catch(() => undefined);
      }
      if (manageMembership) {
        await this.cancelSourceActorMove(actor, state).catch(() => undefined);
      } else {
        this.options.actorHandoff.cancel(actor.context.actorId);
        state.endMove();
      }
      throw error;
    }
  }

  async notifyCoreSourceLeave(actor: ZLinkActor, callback: () => Promise<void>): Promise<void> {
    const pending = this.coreSourceLeaves.get(actor.context.actorId);
    try {
      await callback();
      pending?.resolve();
    } catch (error) {
      pending?.reject(error);
      throw error;
    } finally {
      this.coreSourceLeaves.delete(actor.context.actorId);
    }
  }

  relayMaintenanceTerminal(
    actorId: string,
    packet: ZLinkActorHandoffPacket,
    result: ZLinkActorHandoffResult,
    sourceNodeRid: string,
    targetAuthorityOwnerGeneration?: bigint
  ): ZLinkActorHandoffTerminalAck {
    return this.options.actorHandoff.acceptRelocatedTerminal(
      actorId,
      packet,
      result,
      sourceNodeRid,
      targetAuthorityOwnerGeneration
    );
  }

  relayCanonicalMaintenanceTerminal(
    operationId: string,
    replyRouteId: string,
    result: ZLinkActorHandoffResult,
    sourceNodeRid: string,
    targetAuthorityOwnerGeneration?: bigint
  ): ZLinkActorHandoffTerminalAcceptance {
    return this.options.actorHandoff.acceptRelocatedTerminalRelay(
      operationId,
      replyRouteId,
      undefined,
      result,
      sourceNodeRid,
      targetAuthorityOwnerGeneration
    );
  }

  private async sealBoundSessionRoute(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    sealId: string,
    signal?: AbortSignal
  ): Promise<ZLinkRemoteBoundSessionTarget> {
    const target = state.remoteBoundSessionTarget;
    const actorRef = state.nativeActorRef;
    if (
      target === undefined || actorRef === undefined ||
      target.bindingGeneration === undefined || target.bindingGeneration <= 0n ||
      state.locationGeneration === undefined || state.locationGeneration < 0n ||
      state.ownerLeaseGeneration === undefined || state.ownerLeaseGeneration <= 0n
    ) {
      throw new Error(`Actor '${actor.context.actorId}' Session route cannot be sealed without its exact source fence.`);
    }
    const request = {
      actorId: actor.context.actorId,
      actorGeneration: actorRef.generation.toString(),
      actorOwnershipGeneration: state.locationGeneration.toString(),
      bindingGeneration: target.bindingGeneration.toString(),
      ownerLeaseGeneration: state.ownerLeaseGeneration.toString(),
      sealId
    };
    const ack = decodeRemoteBoundSessionSealAck(await this.options.routeTransport.requestToSpot(
      {
        routerChannelId: target.routerChannelId,
        targetNodeRid: target.targetNodeRid,
        spotId: target.spotId,
        spotKind: ZLinkSpotKind.Entry
      },
      encodeRemoteBoundSessionSealPayload(request),
      { packetName: ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET, signal }
    ));
    if (ack.actorId !== request.actorId || ack.sealId !== sealId || BigInt(ack.acceptedHighWater) < 0n) {
      throw new Error(`Actor '${actor.context.actorId}' Session route seal ACK does not match command 42.`);
    }
    return {
      ...target,
      previousAuthorityOwnerGeneration: state.locationGeneration,
      previousOwnerLeaseGeneration: state.ownerLeaseGeneration,
      acceptedHighWater: BigInt(ack.acceptedHighWater),
      relocationSealId: sealId
    };
  }

  private async abortBoundSessionRouteSeal(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    sealId: string
  ): Promise<void> {
    const target = preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    );
    const actorRef = state.nativeActorRef;
    if (target === undefined || actorRef === undefined || target.bindingGeneration === undefined ||
      target.previousAuthorityOwnerGeneration === undefined || target.previousOwnerLeaseGeneration === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' Session route seal cannot be released without its exact fence.`);
    }
    const ack = decodeRemoteBoundSessionSealAck(await this.options.routeTransport.requestToSpot(
      {
        routerChannelId: target.routerChannelId,
        targetNodeRid: target.targetNodeRid,
        spotId: target.spotId,
        spotKind: ZLinkSpotKind.Entry
      },
      encodeRemoteBoundSessionSealPayload({
        actorId: actor.context.actorId,
        actorGeneration: actorRef.generation.toString(),
        actorOwnershipGeneration: target.previousAuthorityOwnerGeneration.toString(),
        bindingGeneration: target.bindingGeneration.toString(),
        ownerLeaseGeneration: target.previousOwnerLeaseGeneration.toString(),
        sealId
      }, true),
      { packetName: ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET }
    ));
    if (ack.actorId !== actor.context.actorId || ack.sealId !== sealId) {
      throw new Error(`Actor '${actor.context.actorId}' Session route seal abort ACK does not match.`);
    }
  }

  private async prepareBoundSessionAcceptedJournal(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    sealId: string,
    backlog: readonly import('../actors').ZLinkActorHandoffPacket[],
    signal?: AbortSignal
  ): Promise<ZLinkBoundSessionAcceptedJournalRoot> {
    const actorRef = state.nativeActorRef;
    const highWater = preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    )?.acceptedHighWater;
    const journal = this.boundSessionAcceptedJournal();
    if (actorRef === undefined || highWater === undefined || journal === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' accepted Session journal cannot be prepared.`);
    }
    return await journal.prepare(
      actor.context.actorId,
      actorRef.generation,
      sealId,
      highWater,
      backlog,
      signal
    );
  }

  private boundSessionAcceptedJournal(): ZLinkBoundSessionAcceptedJournal | undefined {
    const store = this.options.relocationStore();
    return store === undefined ? undefined : new ZLinkBoundSessionAcceptedJournal(store);
  }

  private beginCoreSourceLeave(actorId: string): Promise<void> {
    let resolve!: () => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<void>((accept, fail) => {
      resolve = accept;
      reject = fail;
    });
    void promise.catch(() => {});
    this.coreSourceLeaves.set(actorId, { promise, resolve, reject });
    return promise;
  }

  private scheduleSourceDeparture(
    actor: ZLinkActor,
    sourceSpotId: RoutingId | undefined,
    releaseLocation: boolean
  ): void {
    if (this.sourceDepartureTasks.has(actor.context.actorId)) return;
    const task = this.finishSourceDeparture(actor, sourceSpotId, releaseLocation)
      .finally(() => this.sourceDepartureTasks.delete(actor.context.actorId));
    this.sourceDepartureTasks.set(actor.context.actorId, task);
  }

  private async finishSourceDeparture(
    actor: ZLinkActor,
    sourceSpotId: RoutingId | undefined,
    releaseLocation: boolean
  ): Promise<void> {
    const retry = new ZLinkActorRetryDelay();
    while (this.options.shutdownSignal?.()?.aborted !== true) {
      try {
        if (sourceSpotId !== undefined) {
          await this.options.spotManager()?.commitActorLeaveAfterTransfer(sourceSpotId, actor.context.actorId);
        }
        if (releaseLocation) {
          const state = this.options.actorManager()?.getState(actor.context.actorId);
          if (state?.actorType !== undefined && state.ownsLocation) {
            await this.options.locationLifecycle()?.releaseActor(
              state.actorType,
              actor.context.actorId
            );
            state.markLocationReleased();
          }
          // Message Follow owns the bounded stale route after the native leave.
          // The source Actor shell must therefore be removed from the process
          // registry so a later relocation can materialize the same Actor ID on
          // this node without colliding with its previous incarnation.
          await this.options.actorManager()?.completeCoreRelocationSource(actor.context.actorId);
        }
        this.options.onSourceDepartureCompleted?.(actor.context.actorId);
        return;
      } catch (error) {
        this.options.reportPostCommitError?.(error);
        if (!await retry.wait(this.options.shutdownSignal?.())) return;
      }
    }
  }

  async getOrCreateRoutedActor(
    actorId: string,
    actorType: string,
    actorRef?: ActorRef,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    actorCreateRequest?: Message,
    signal?: AbortSignal
  ): Promise<{ readonly actor: ZLinkActor; readonly actorRef: ZLinkBackendActorRef }> {
    const actorManager = this.requireActorManager('Routed actor join requires ZLINK_ACTOR_MANAGER.');
    const actor = actorRef === undefined
      ? await actorManager.getOrCreateActor(actorId, actorType, signal)
      : await actorManager.getOrCreateWithNativeRef(
          actorId,
          actorType,
          actorRef as unknown as ZLinkBackendActorRef,
          actorCreateRequest === undefined
            ? undefined
            : wrapFrameworkPayloadMessage(actorCreateRequest, this.options.messageSerializers),
          signal
        );
    const state = actorManager.getState(actorId);
    if (state === undefined) {
      throw new Error(`Actor '${actorId}' state was not created.`);
    }
    if (actorRef !== undefined) {
      state.setNativeActorRef(actorRef as unknown as ZLinkBackendActorRef);
      state.setRemoteBoundSessionTarget(remoteBoundSessionTarget);
      return { actor, actorRef: actorRef as unknown as ZLinkBackendActorRef };
    }
    return { actor, actorRef: state.ensureNativeActorRef(this.options.primaryMeshNode()) };
  }

  async materializeRoutedActor(
    actorId: string,
    actorType: string,
    adapterKey: string | undefined,
    transferState: Message,
    actorEntryNodeRid: RoutingId | undefined,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    signal?: AbortSignal
  ): Promise<{ readonly actor: ZLinkActor; readonly actorRef: ZLinkBackendActorRef }> {
    const actorManager = this.requireActorManager('Routed actor transfer requires ZLINK_ACTOR_MANAGER.');
    const materialized = await actorManager.materializeTransferredActor(
      actorId,
      actorType,
      adapterKey,
      wrapFrameworkPayloadMessage(transferState, this.options.messageSerializers),
      signal
    );
    const state = actorManager.getState(actorId);
    if (state === undefined) {
      throw new Error(`Actor '${actorId}' transfer state was not created.`);
    }
    if (actorEntryNodeRid !== undefined) state.setEntryNodeRid(actorEntryNodeRid);
    state.setBoundSessionTransferTarget(remoteBoundSessionTarget);
    if (remoteBoundSessionTarget !== undefined) {
      state.setRemoteBoundSessionTarget(
        mergeRemoteBoundSessionTarget(
          remoteBoundSessionTarget,
          state.remoteBoundSessionTarget
        )
      );
    }
    if (remoteBoundSessionTarget?.bindingGeneration !== undefined) {
      state.setBoundSessionBindingGeneration(remoteBoundSessionTarget.bindingGeneration);
    }
    return {
      actor: materialized.actor,
      actorRef: materialized.actorRef as unknown as ZLinkBackendActorRef
    };
  }

  rememberRoutedActorTransferTarget(
    actorId: string,
    target: ZLinkRemoteBoundSessionTarget | undefined
  ): void {
    if (target === undefined) return;
    const state = this.options.actorManager()?.getState(actorId);
    if (state === undefined) {
      return;
    }
    state.setBoundSessionTransferTarget(target);
    state.setRemoteBoundSessionTarget(
      mergeRemoteBoundSessionTarget(target, state.remoteBoundSessionTarget)
    );
    if (target.bindingGeneration !== undefined) {
      state.setBoundSessionBindingGeneration(target.bindingGeneration);
    }
  }

  async prepareRecoveryRoutedActor(
    actorId: string,
    actorType: string,
    actorRef: ActorRef,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    adapterKey: string | undefined,
    transferState: Message,
    actorEntryNodeRid: RoutingId | undefined,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    signal?: AbortSignal
  ): Promise<ZLinkActor> {
    const actorManager = this.requireActorManager(
      'Routed Actor recovery requires ZLINK_ACTOR_MANAGER.'
    );
    const actor = await actorManager.prepareRelocationActor(
      actorId,
      actorType,
      actorRef.objectGeneration,
      authorityOwnerGeneration,
      spotId,
      spotGeneration,
      membershipEpoch,
      signal,
      adapterKey === undefined
        ? undefined
        : {
            adapterKey,
            state: wrapFrameworkPayloadMessage(
              transferState,
              this.options.messageSerializers
            )
          }
    );
    const state = actorManager.getState(actorId);
    if (state === undefined) {
      throw new Error(`Actor '${actorId}' recovery state was not created.`);
    }
    if (actorEntryNodeRid !== undefined) state.setEntryNodeRid(actorEntryNodeRid);
    state.setBoundSessionTransferTarget(remoteBoundSessionTarget);
    if (remoteBoundSessionTarget !== undefined) {
      state.setRemoteBoundSessionTarget(
        mergeRemoteBoundSessionTarget(
          remoteBoundSessionTarget,
          state.remoteBoundSessionTarget
        )
      );
    }
    if (remoteBoundSessionTarget?.bindingGeneration !== undefined) {
      state.setBoundSessionBindingGeneration(remoteBoundSessionTarget.bindingGeneration);
    }
    return actor;
  }

  commitRoutedActor(actor: ZLinkActor, spotId: RoutingId, spot: ZLinkSpot): void {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    state?.setJoinedSpot(spotId, spot);
    const actorRef = state?.nativeActorRef;
    const binding = state?.boundSessionTransferTarget;
    if (
      actorRef !== undefined
      && binding?.sessionNodeRid !== undefined
      && binding.sessionRid !== undefined
      && binding.bindingGeneration !== undefined
    ) {
      this.options.primaryMeshNode().restoreActorSessionBinding?.(
        actorRef,
        binding.sessionNodeRid,
        binding.sessionRid,
        binding.bindingGeneration
      );
    }
  }

  bindRoutedActorRef(actor: ZLinkActor, actorRef: ActorRef): void {
    const node = this.options.primaryMeshNode();
    const localActorRef = node.actorLookup(actor.context.actorId).actor;
    const targetActorRef = {
      actorId: actorRef.actorId,
      nodeRid: node.status().routingId,
      generation: localActorRef.generation > 0n
        ? localActorRef.generation
        : actorRef.objectGeneration
    };
    this.options.actorManager()?.getState(actor.context.actorId)?.setNativeActorRef(targetActorRef);
  }

  publishRecoveryRoutedActor(actor: ZLinkActor): void {
    this.options.actorManager()?.publishRelocationActor(actor.context.actorId);
  }

  currentRoutedActorRef(actor: ZLinkActor): ActorRef | undefined {
    const native = this.options.actorManager()
      ?.getState(actor.context.actorId)
      ?.nativeActorRef;
    return native === undefined
      ? undefined
      : toFrameworkActorRef(native, actor.context.meshName);
  }

  adoptRoutedActorAuthority(
    actor: ZLinkActor,
    authority: ZLinkAuthoritySnapshot,
    spotId: RoutingId,
    spot: ZLinkSpot,
    membershipEpoch: bigint
  ): void {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    if (state === undefined) {
      throw new Error(
        `Actor '${actor.context.actorId}' recovery state is unavailable.`
      );
    }
    state.setLocationGeneration(authority.authorityOwnerGeneration);
    state.setOwnerLeaseGeneration(authority.ownerLeaseGeneration);
    state.setJoinedSpot(spotId, spot, membershipEpoch);
    state.markLocationOwned();
  }

  async claimRoutedActorLocation(
    actor: ZLinkActor,
    spotId: RoutingId,
    spotMeshName: string,
    joinedLocation?: {
      readonly spotGeneration: bigint;
      readonly membershipEpoch: bigint;
    }
  ): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const actorType = state?.actorType;
    const lifecycle = this.options.locationLifecycle();
    if (state === undefined || actorType === undefined || lifecycle === undefined) {
      return;
    }
    const node = this.options.primaryMeshNode();
    const location = node.actorLookup(actor.context.actorId);
    const spotGeneration = joinedLocation?.spotGeneration ?? location.spotGeneration;
    const membershipEpoch = joinedLocation?.membershipEpoch ?? location.membershipEpoch;
    if (spotGeneration <= 0n || membershipEpoch <= 0n) {
      throw new Error(`Actor '${actor.context.actorId}' committed target location has invalid lifecycle generations.`);
    }
    // RoutingId is an opaque byte value, so the committed target is compared by
    // value. Reference equality would reject a matching SPOT read back from the
    // Core lookup.
    if (
      joinedLocation === undefined
      && (
        location.spotId === null
        || location.spotId === undefined
        || encodeRoutingIdStorageHex(location.spotId as RoutingId)
          !== encodeRoutingIdStorageHex(spotId)
      )
    ) {
      throw new Error(`Actor '${actor.context.actorId}' Core location does not match the committed target SPOT.`);
    }
    const deadline = Date.now() + 5_000;
    const ownerNodeGeneration = node.status().lifecycleGeneration;
    if (ownerNodeGeneration <= 0n) {
      throw new Error(`Actor '${actor.context.actorId}' owner MeshNode has no valid lifecycle generation.`);
    }
    const authority = await this.options.authorityStore()?.readAuthority(
      encodeAuthorityKey('actor', actor.context.actorId)
    );
    if (
      authority?.kind === 'snapshot'
      && authority.allocation.state === 'active'
      && String(authority.allocation.descriptor.rid) === String(node.status().routingId)
      && authority.allocation.descriptorLifecycleGeneration === ownerNodeGeneration
    ) {
      state.setLocationGeneration(authority.authorityOwnerGeneration);
      state.setOwnerLeaseGeneration(authority.ownerLeaseGeneration);
      state.setJoinedSpot(spotId, state.spot, membershipEpoch, spotGeneration);
      return;
    }
    let claim;
    for (;;) {
      claim = await lifecycle.takeoverActorJoinedSpot(
        actorType,
        actor.context.actorId,
        toFrameworkActorRef(
          state.nativeActorRef ?? location.actor as never,
          spotMeshName
        ),
        spotMeshName,
        spotId,
        spotGeneration,
        membershipEpoch,
        ownerNodeGeneration,
        async () => state.clearAfterDestroy()
      );
      if (claim.status !== 'conflict' || Date.now() >= deadline) {
        break;
      }
      await new Promise<void>((resolve) => setTimeout(resolve, 10));
    }
    if (claim.status === 'conflict') {
      throw new Error(`Actor '${actor.context.actorId}' target location takeover was rejected.`);
    }
    if (claim.generation !== undefined) {
      state.setLocationGeneration(claim.generation);
    }
    if (claim.claimed !== undefined) {
      state.setOwnerLeaseGeneration(claim.claimed.leaseGeneration);
    }
    state.setJoinedSpot(spotId, state.spot, membershipEpoch, spotGeneration);
    state.markLocationOwned();
  }

  async claimNativeActorLocation(
    actor: ZLinkActor,
    spotId: RoutingId,
    spotMeshName: string
  ): Promise<ZLinkNativeActorJoinSnapshot> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const previousLocation = this.options.locationLifecycle()?.actorLocationSnapshot(actor.context.actorId);
    const snapshot = {
      spotId: state?.spotId,
      spot: state?.spot,
      locationSpotId: previousLocation?.spotId,
      spotMeshName: previousLocation?.meshName,
      actorRef: previousLocation?.actorRef,
      spotGeneration: previousLocation?.spotGeneration,
      membershipEpoch: previousLocation?.membershipEpoch,
      ownerNodeGeneration: previousLocation?.ownerNodeGeneration
    };
    await this.claimRoutedActorLocation(actor, spotId, spotMeshName);
    return snapshot;
  }

  async publishRoutedActorOwnership(actor: ZLinkActor): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const actorRef = state?.nativeActorRef;
    const generation = state?.locationGeneration;
    if (state === undefined || actorRef === undefined || generation === undefined) return;
    const boundSessionTarget = preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    );
    // A regular actor created with a remote Session has a routing target, but
    // it has not entered an actor relocation. Command 44 is a relocation
    // ownership update and requires the accepted-journal fence created by the
    // relocation protocol. Core already owns the initial binding, so there is
    // no ownership update to publish for this ordinary join.
    if (boundSessionTarget !== undefined && !hasAcceptedJournalFence(boundSessionTarget)) {
      return;
    }
    await this.verifyBoundSessionAcceptedJournal(actor, state);
    await this.publishBoundSessionOwnership(
      actor.context.actorId,
      actorRef,
      generation,
      boundSessionTarget,
      state.ownerLeaseGeneration,
      state.spotId === undefined || boundSessionTarget === undefined ? undefined : {
        routerChannelId: boundSessionTarget.routerChannelId,
        targetNodeRid: actorRef.nodeRid,
        spotId: state.spotId,
        spotKind: ZLinkSpotKind.User
      }
    );
  }

  async openRoutedActorSession(actor: ZLinkActor): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const target = state === undefined
      ? undefined
      : preferredRemoteBoundSessionTarget(
          state.remoteBoundSessionTarget,
          state.boundSessionTransferTarget
        );
    const sealId = target?.relocationSealId;
    if (state === undefined || target === undefined || sealId === undefined) return;
    const retry = new ZLinkActorRetryDelay();
    let lastError: unknown;
    let immediateRetry = true;
    while (this.options.shutdownSignal?.()?.aborted !== true) {
      try {
        await this.abortBoundSessionRouteSeal(actor, state, sealId);
        return;
      } catch (error) {
        lastError = error;
        this.options.reportPostCommitError?.(error);
        if (this.options.shutdownSignal?.()?.aborted === true) break;
        if (immediateRetry) {
          immediateRetry = false;
          continue;
        }
        if (!await retry.wait(this.options.shutdownSignal?.())) break;
      }
    }
    throw new Error(
      `Actor '${actor.context.actorId}' Session route remained sealed while the runtime stopped.`,
      { cause: lastError }
    );
  }

  clearRoutedActor(actor: ZLinkActor): void {
    this.options.actorManager()?.getState(actor.context.actorId)?.clearJoinedSpot();
    this.options.clearRemoteActorPacketTarget(actor.context.actorId);
  }

  async rollbackNativeActorJoin(
    actor: ZLinkActor,
    snapshot: ZLinkNativeActorJoinSnapshot
  ): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const actorType = state?.actorType;
    const lifecycle = this.options.locationLifecycle();
    if (snapshot.spotId === undefined) state?.clearJoinedSpot();
    else state?.setJoinedSpot(snapshot.spotId, snapshot.spot);
    if (state?.ownsLocation !== true || actorType === undefined || lifecycle === undefined) return;
    if (snapshot.spotId === undefined) {
      if (
        snapshot.locationSpotId === undefined
        || snapshot.spotGeneration === undefined
        || snapshot.membershipEpoch === undefined
        || snapshot.ownerNodeGeneration === undefined
      ) {
        throw new Error(`Actor '${actor.context.actorId}' cannot restore its Entry SPOT location without its exact generation fields.`);
      }
      await lifecycle.notifyActorLeftSpot(
        actorType,
        actor.context.actorId,
        snapshot.locationSpotId,
        snapshot.spotGeneration,
        snapshot.membershipEpoch,
        snapshot.ownerNodeGeneration
      );
      return;
    }
    if (snapshot.actorRef === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' cannot restore its previous SPOT location without a native ref.`);
    }
    if (
      snapshot.spotMeshName === undefined
      || snapshot.spotGeneration === undefined
      || snapshot.membershipEpoch === undefined
      || snapshot.ownerNodeGeneration === undefined
    ) {
      throw new Error(`Actor '${actor.context.actorId}' cannot restore its previous SPOT location without its exact generation fields.`);
    }
    const restored = await lifecycle.takeoverActorJoinedSpot(
      actorType,
      actor.context.actorId,
      snapshot.actorRef,
      snapshot.spotMeshName,
      snapshot.spotId,
      snapshot.spotGeneration,
      snapshot.membershipEpoch,
      snapshot.ownerNodeGeneration,
      async () => state.clearAfterDestroy()
    );
    if (restored.status === 'conflict') {
      throw new Error(`Actor '${actor.context.actorId}' previous SPOT location could not be restored.`);
    }
    if (restored.generation !== undefined) state.setLocationGeneration(restored.generation);
    if (restored.claimed !== undefined) state.setOwnerLeaseGeneration(restored.claimed.leaseGeneration);
  }

  async rollbackRoutedActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void> {
    const manager = this.options.actorManager();
    const state = manager?.getState(actor.context.actorId);
    const actorType = state?.actorType;
    const lifecycle = this.options.locationLifecycle();
    let locationError: unknown;
    if (state?.ownsLocation === true && actorType !== undefined && lifecycle !== undefined) {
      try {
        await lifecycle.releaseActor(actorType, actor.context.actorId);
        state.markLocationReleased();
      } catch (error) {
        locationError = error;
        void lifecycle.releaseActorEventually(actorType, actor.context.actorId);
      }
    }
    try {
      await manager?.rollbackTransferredActor(actor, signal);
    } catch (rollbackError) {
      if (locationError !== undefined) {
        throw new AggregateError(
          [locationError, rollbackError],
          `Actor '${actor.context.actorId}' target transfer rollback failed.`
        );
      }
      throw rollbackError;
    }
    if (locationError !== undefined) throw locationError;
  }

  actorEntryNodeRid(actor: ZLinkActor): RoutingId | undefined {
    return this.options.actorManager()?.getState(actor.context.actorId)?.entryNodeRid;
  }

  private requireActorManager(message: string): ZLinkActorTransferRuntimeActorManager {
    const actorManager = this.options.actorManager();
    if (actorManager === undefined) {
      throw new Error(message);
    }
    return actorManager;
  }

  private async publishBoundSessionOwnership(
    actorId: string,
    actorRef: ZLinkBackendActorRef,
    ownershipGeneration: bigint,
    target: ZLinkRemoteBoundSessionTarget | undefined,
    targetOwnerLeaseGeneration: bigint | undefined,
    actorPacketTarget: ZLinkRemoteActorPacketTarget | undefined
  ): Promise<void> {
    if (target === undefined) {
      return;
    }
    if (
      target.bindingGeneration === undefined ||
      target.previousAuthorityOwnerGeneration === undefined ||
      target.previousOwnerLeaseGeneration === undefined ||
      target.acceptedHighWater === undefined ||
      target.relocationSealId === undefined ||
      target.acceptedJournalReference === undefined ||
      target.acceptedJournalChecksumCrc32c === undefined ||
      targetOwnerLeaseGeneration === undefined ||
      target.bindingGeneration <= 0n ||
      target.previousAuthorityOwnerGeneration < 0n ||
      ownershipGeneration <= target.previousAuthorityOwnerGeneration ||
      target.previousOwnerLeaseGeneration <= 0n ||
      targetOwnerLeaseGeneration <= 0n ||
      target.acceptedHighWater < 0n
    ) {
      throw new Error(`Actor '${actorId}' bound-session ownership fence is incomplete.`);
    }
    const actorGeneration = actorRef.generation.toString();
    const actorOwnershipGeneration = ownershipGeneration.toString();
    const bindingGeneration = target.bindingGeneration.toString();
    const targetOwnerLease = targetOwnerLeaseGeneration.toString();
    const acceptedHighWater = target.acceptedHighWater.toString();
    const sealId = target.relocationSealId;
    const payload = encodeRemoteBoundSessionOwnershipPayload({
      actorId,
      meshName: actorPacketTarget?.routerChannelId ?? target.routerChannelId,
      actorNodeRid: String(actorRef.nodeRid),
      actorNodeRidHex: (actorRef.nodeRid as { toHex?: () => string }).toHex?.(),
      actorGeneration,
      previousActorOwnershipGeneration: target.previousAuthorityOwnerGeneration.toString(),
      actorOwnershipGeneration,
      bindingGeneration,
      previousOwnerLeaseGeneration: target.previousOwnerLeaseGeneration.toString(),
      targetOwnerLeaseGeneration: targetOwnerLease,
      acceptedHighWater,
      sealId,
      acceptedJournalReference: target.acceptedJournalReference,
      acceptedJournalChecksumCrc32c: target.acceptedJournalChecksumCrc32c,
      actorPacketTarget: encodeRemoteActorPacketTarget(actorPacketTarget)
    });
    const retry = new ZLinkActorRetryDelay();
    let lastError: unknown;
    let immediateRetry = true;
    while (this.options.shutdownSignal?.()?.aborted !== true) {
      try {
        const ack = decodeRemoteBoundSessionOwnershipAck(await this.options.routeTransport.requestToSpot(
          {
            routerChannelId: target.routerChannelId,
            targetNodeRid: target.targetNodeRid,
            spotId: target.spotId,
            spotKind: ZLinkSpotKind.Entry
          },
          payload,
          {
            packetName: ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
            signal: this.options.shutdownSignal?.()
          }
        ));
        if (
          ack.actorId !== actorId ||
          ack.actorGeneration !== actorGeneration ||
          ack.actorOwnershipGeneration !== actorOwnershipGeneration ||
          ack.bindingGeneration !== bindingGeneration ||
          ack.targetOwnerLeaseGeneration !== targetOwnerLease ||
          ack.acceptedHighWater !== acceptedHighWater ||
          ack.sealId !== sealId
        ) {
          throw new Error(`Actor '${actorId}' bound-session ownership ACK does not match the request.`);
        }
        return;
      } catch (error) {
        lastError = error;
        this.options.reportPostCommitError?.(error);
        if (this.options.shutdownSignal?.()?.aborted === true) break;
        if (immediateRetry) {
          immediateRetry = false;
          continue;
        }
        if (!await retry.wait(this.options.shutdownSignal?.())) break;
      }
    }
    throw new Error(
      `Actor '${actorId}' bound-session ownership update stopped before command 45 ACK on route ` +
      `'${target.routerChannelId}' to '${String(target.targetNodeRid)}'.`,
      { cause: lastError }
    );
  }

  private async verifyBoundSessionAcceptedJournal(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState
  ): Promise<void> {
    const target = preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    );
    if (target === undefined) return;
    const actorRef = state.nativeActorRef;
    const journal = this.boundSessionAcceptedJournal();
    if (
      actorRef === undefined || journal === undefined ||
      target.relocationSealId === undefined ||
      target.acceptedHighWater === undefined ||
      target.acceptedJournalReference === undefined ||
      target.acceptedJournalChecksumCrc32c === undefined
    ) {
      throw new Error(`Actor '${actor.context.actorId}' accepted Session journal fence is incomplete.`);
    }
    await journal.verify({
      actorId: actor.context.actorId,
      actorGeneration: actorRef.generation,
      sealId: target.relocationSealId,
      acceptedHighWater: target.acceptedHighWater,
      reference: { value: target.acceptedJournalReference } as import('../../contracts').ZLinkBlobReference,
      checksumCrc32c: target.acceptedJournalChecksumCrc32c
    });
  }
}

function hasAcceptedJournalFence(
  target: import('../actors/actor-runtime-state').ZLinkRemoteBoundSessionTarget
): boolean {
  return target.acceptedHighWater !== undefined
    || target.relocationSealId !== undefined
    || target.acceptedJournalReference !== undefined
    || target.acceptedJournalChecksumCrc32c !== undefined;
}

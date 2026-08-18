import { createHash, randomUUID } from 'node:crypto';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkRelocationStore,
  ZLinkSpot
} from '../../contracts';
import type {
  ZLinkAggregateId,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import type {
  ZLinkAuthorityStore,
  ZLinkObjectCreationStore,
  ZLinkOwnerLeaseStore
} from '../locations/internal-store-contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef, ZLinkBackendMeshNode } from '../backend';
import {
  toFrameworkActorRef,
  mergeRemoteBoundSessionTarget,
  preferredRemoteBoundSessionTarget,
  type ZLinkActorHandoffCoordinator,
  type ZLinkActorHandoffDispatch,
  type ZLinkActorRoutedJoinTransport,
  type ZLinkActorTransferRegistry,
  ZLinkDeferredJoinAcceptedJournal,
  rewriteActorAuthorityRoute,
  type ZLinkDeferredJoinAcceptedRoot,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { ZLinkActorRuntimeState } from '../actors/actor-runtime-state';
import { ZLinkActorRetryDelay } from '../actors/actor-retry-delay';
import { encodeRoutingIdStorageHex, routingIdsEqual } from '../routing-id';
import type { ZLinkLocationLifecycle } from '../locations';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import type { DefaultZLinkSpotManager } from '../spots';
import type {
  ZLinkSpotRouteResolver,
  ZLinkSpotRouteTarget
} from '../spots/spot-routing-internal';
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
import type {
  ServiceSessionRelocationRoute,
  ServiceSessionRelocationSeal,
  ServiceSessionRelocationSealed,
  ServiceWireOperationId
} from '../foundation/service-stateful-wire-codec';

interface ZLinkSessionRelocationWirePort {
  requestSessionRelocationSeal(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceSessionRelocationSeal,
    signal?: AbortSignal
  ): Promise<ServiceSessionRelocationSealed>;
  sendSessionRelocationRoute(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceSessionRelocationRoute,
    signal?: AbortSignal
  ): Promise<void>;
}

type ZLinkCommittedActorAuthority = Pick<
  ZLinkAuthoritySnapshot,
  | 'objectGeneration'
  | 'authorityOwnerGeneration'
  | 'ownerId'
  | 'ownerLeaseGeneration'
  | 'allocation'
>;

/** Converts only committed actor authority evidence into a Message Follow fence. */
export function committedActorOwnerFence(
  actorId: string,
  targetActorRef: ActorRef,
  authority: ZLinkCommittedActorAuthority
): ZLinkActorMessageFollowOwnerFence {
  if (targetActorRef.actorId !== actorId
    || targetActorRef.objectGeneration <= 0n
    || authority.objectGeneration !== targetActorRef.objectGeneration
    || authority.allocation.state !== 'active'
    || authority.allocation.objectKind !== 'actor'
    || authority.allocation.descriptor.meshName !== targetActorRef.meshName
    || !routingIdsEqual(authority.allocation.descriptor.rid, targetActorRef.nodeRid)
    || authority.ownerId.length === 0
    || authority.ownerLeaseGeneration <= 0n
    || authority.allocation.descriptorLifecycleGeneration <= 0n
    || authority.authorityOwnerGeneration <= 0n) {
    throw new Error(
      `Actor '${actorId}' handoff target does not match the committed authority snapshot.`
    );
  }
  return ownerFence({
    ownerId: authority.ownerId,
    ownerLeaseGeneration: authority.ownerLeaseGeneration,
    nodeRid: String(authority.allocation.descriptor.rid),
    nodeRidHex: encodeRoutingIdStorageHex(authority.allocation.descriptor.rid),
    nodeGeneration: authority.allocation.descriptorLifecycleGeneration,
    authorityOwnerGeneration: authority.authorityOwnerGeneration
  });
}

function requireSourceObjectGeneration(
  actorId: string,
  state: ZLinkActorRuntimeState
): bigint {
  const generation = state.nativeActorRef?.generation;
  if (generation === undefined || generation <= 0n) {
    throw new Error(`Actor '${actorId}' handoff requires a positive source ObjectGeneration.`);
  }
  return generation;
}

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
  /** Resolves the authoritative Ready route for a target User/Instance Spot. */
  readonly spotRouteResolver?: () => ZLinkSpotRouteResolver | undefined;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly actorTransferRegistry: ZLinkActorTransferRegistry;
  readonly authorityStore: () => (
    ZLinkAuthorityStore
    & ZLinkObjectCreationStore
    & ZLinkOwnerLeaseStore
  ) | undefined;
  readonly currentOwner: () => ZLinkLocationOwnerToken | undefined;
  readonly relocationStore: () => ZLinkRelocationStore | undefined;
  /** Live admitted peers of a mesh; used to stop retries against a gone session owner. */
  readonly liveDescriptors?: (
    meshName: string,
    signal?: AbortSignal
  ) => Promise<readonly import('../../contracts').ZLinkMeshNodeDescriptor[]>;
  /** Service-wire command 42/43/44 bridge, installed after the host runtime is assembled. */
  readonly sessionRelocationWire?: () => ZLinkSessionRelocationWirePort | undefined;
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
    readonly submitted: Promise<void>;
    readonly resolveSubmitted: () => void;
    readonly rejectSubmitted: (error: unknown) => void;
    notifySubmitted?: () => Promise<void>;
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
      requireSourceObjectGeneration(actorId, state)
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
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return await this.requireDeferredJoinJournal().prepare(
      actorId,
      operationId,
      actorRef,
      rawReply,
      signal
    );
  }

  async discardDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    await this.requireDeferredJoinJournal().discardPrepared(root, signal);
  }

  markDeferredJoinAcceptedCommitted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return this.requireDeferredJoinJournal().markCommitted(root, actorRef, signal);
  }

  async commitAndDeliverDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor: ZLinkActor,
    actorRef: ActorRef,
    submitMailbox: <T>(operation: () => Promise<T>) => Promise<T>,
    signal?: AbortSignal
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
          signal
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
        requireSourceObjectGeneration(actor.context.actorId, state)
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
        await this.releaseCanceledHandoff(actor, state);
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

  private async releaseCanceledHandoff(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState
  ): Promise<void> {
    // The seal-era ingress hold returns to the source queue on a precommit
    // abort. Rejection stays reserved for a source Actor whose dispatch is
    // gone, because held packets then have no queue left to return to.
    const replay = this.sourceHandoffReplay(actor, state);
    if (replay === undefined) {
      this.options.actorHandoff.cancel(actor.context.actorId);
      return;
    }
    await this.options.actorHandoff.releaseCanceled(actor.context.actorId, replay);
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
    deferredOperationId?: string,
    relocation?: ServiceWireOperationId
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
    try {
      if (state.remoteBoundSessionTarget !== undefined) {
        sealId = randomUUID();
        const sealedTarget = await this.sealBoundSessionRoute(
          actor,
          state,
          sealId,
          signal,
          relocation
        );
        state.setRemoteBoundSessionTarget(sealedTarget);
        this.options.actorHandoff.sealConnectionBoundIngress(actor.context.actorId);
      }
      const transfer = await this.options.actorTransferRegistry.transferOut(
        actor,
        state.actorType,
        signal
      );
      // The captured state is handed off inline with the join request; the
      // Relocation Store no longer carries relocation state payloads
      // (spec 28 §2 — direct transfer is the only handoff original).
      relocationMetric?.recordBytes(transfer.state.toEncodedPayload().toBytes().byteLength);
      if (lifecycleAuthority === 'framework') {
        sourceLeaveStarted = true;
        await this.prepareSourceActorLeave(actor, sourceSpotId, signal);
      }
      const coreSourceLeave = lifecycleAuthority === 'core'
        ? this.beginCoreSourceLeave(actor.context.actorId)
        : undefined;
      const sourceLeaveCompletion = coreSourceLeave?.completion;
      const handoffBacklog = lifecycleAuthority === 'core'
        ? this.options.actorHandoff.snapshotCoreBacklog(actor.context.actorId)
        : this.options.actorHandoff.snapshot(actor.context.actorId);
      let phase: 'prepared' | 'committed' | 'rolledBack' = 'prepared';
      let committedTargetOwnerFence:
        ZLinkActorMessageFollowOwnerFence | undefined;
      return {
        ...transfer,
        handoffBacklog,
        sourceLeaveCompletion,
        sourceLeaveSubmitted: coreSourceLeave?.submitted,
        onSourceLeaveSubmitted: (notify: () => Promise<void>) => {
          const pending = this.coreSourceLeaves.get(actor.context.actorId);
          if (pending === undefined) {
            throw new Error(
              `Actor '${actor.context.actorId}' has no pending Core source leave.`
            );
          }
          pending.notifySubmitted = notify;
        },
        observeTargetAuthority: async (
          target: ZLinkSpotRouteTarget,
          targetActorRef: ActorRef,
          observeSignal?: AbortSignal
        ) => {
          const authority = this.options.authorityStore();
          if (authority === undefined
            || target.targetOwnerId === undefined
            || target.ownerLeaseGeneration === undefined
            || target.targetNodeGeneration === undefined) {
            throw new Error(
              `Actor '${actor.context.actorId}' relocation target has no complete authority fence.`
            );
          }
          const committed = await authority.readAuthority(
            encodeAuthorityKey('actor', actor.context.actorId),
            observeSignal
          );
          if (
            committed.kind !== 'snapshot'
            || committed.ownerId !== target.targetOwnerId
            || committed.ownerLeaseGeneration !== target.ownerLeaseGeneration
            || committed.allocation.descriptorLifecycleGeneration !== target.targetNodeGeneration
            || !routingIdsEqual(committed.allocation.descriptor.rid, target.targetNodeRid)
            || committed.objectGeneration !== targetActorRef.objectGeneration
          ) {
            throw new Error(
              `Actor '${actor.context.actorId}' relocation target authority was not committed exactly.`
            );
          }
          committedTargetOwnerFence = committedActorOwnerFence(
            actor.context.actorId,
            targetActorRef,
            committed
          );
        },
        commit: (
          target: Parameters<ZLinkActorHandoffCoordinator['complete']>[1],
          targetActorRef: ActorRef,
          results: Parameters<ZLinkActorHandoffCoordinator['complete']>[3],
          releaseLocation = true
        ) => {
          if (phase !== 'prepared') return;
          if (committedTargetOwnerFence === undefined) {
            throw new Error(
              `Actor '${actor.context.actorId}' handoff has no committed target authority fence.`
            );
          }
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
          await this.cancelSourceActorMove(actor, state, deferredOperationId);
          await this.restoreSourceActor(actor, sourceSpotId);
          if (sealId !== undefined) {
            this.beginBoundSessionSealAbort(actor, state, sealId);
          }
        }
      };
    } catch (error) {
      relocationMetric?.complete('failed');
      try {
        await this.cancelSourceActorMove(actor, state, deferredOperationId);
        if (sourceLeaveStarted) await this.restoreSourceActor(actor, sourceSpotId);
        if (sealId !== undefined) {
          this.beginBoundSessionSealAbort(actor, state, sealId);
        }
      } catch (rollbackError) {
        throw new AggregateError([error, rollbackError], 'Actor source leave and rollback both failed.');
      }
      throw error;
    }
  }

  async prepareMaintenanceSession(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    signal?: AbortSignal,
    manageMembership = true,
    relocation?: ServiceWireOperationId
  ): Promise<{
    readonly target?: ZLinkRemoteBoundSessionTarget;
    readonly handoffBacklog: readonly import('../actors').ZLinkActorHandoffPacket[];
    takeRelocationRelay(): readonly import('../actors').ZLinkActorHandoffPacket[];
    setReplayResults(results: readonly import('../actors').ZLinkActorHandoffResult[]): void;
    commit(
      target: ZLinkSpotRouteTarget,
      targetActorRef: ActorRef,
      targetOwnerFence: ZLinkActorMessageFollowOwnerFence
    ): Promise<void>;
    rollback(): Promise<void>;
  }> {
    if (manageMembership) {
      await this.beginSourceActorMove(actor, state);
    } else {
      state.beginMove();
      this.options.actorHandoff.begin(
        actor.context.actorId,
        requireSourceObjectGeneration(actor.context.actorId, state)
      );
    }
    let sealId: string | undefined;
    try {
      if (state.remoteBoundSessionTarget !== undefined) {
        sealId = randomUUID();
        state.setRemoteBoundSessionTarget(
          await this.sealBoundSessionRoute(actor, state, sealId, signal, relocation)
        );
        this.options.actorHandoff.sealConnectionBoundIngress(actor.context.actorId);
      }
      const handoffBacklog = this.options.actorHandoff.snapshot(actor.context.actorId);
      let terminal: 'prepared' | 'committed' | 'rolledBack' = 'prepared';
      let replayResults: readonly import('../actors').ZLinkActorHandoffResult[] = [];
      return {
        target: state.remoteBoundSessionTarget,
        handoffBacklog,
        takeRelocationRelay: () =>
          this.options.actorHandoff.takeRelocationRelay(actor.context.actorId),
        setReplayResults: results => {
          if (terminal === 'prepared') replayResults = [...results];
        },
        commit: async (target, targetActorRef, targetOwnerFence) => {
          if (terminal === 'rolledBack') return;
          if (terminal === 'prepared') {
            this.options.actorHandoff.complete(
              actor.context.actorId,
              target,
              targetActorRef,
              replayResults,
              targetOwnerFence
            );
            if (manageMembership && state.spotId !== undefined) {
              await this.options.spotManager()
                ?.commitActorLeaveAfterTransfer(state.spotId, actor.context.actorId);
            }
            state.endMove();
            terminal = 'committed';
          }
        },
        rollback: async () => {
          if (terminal !== 'prepared') return;
          terminal = 'rolledBack';
          if (manageMembership) {
            await this.cancelSourceActorMove(actor, state);
          } else {
            try {
              await this.releaseCanceledHandoff(actor, state);
            } finally {
              state.endMove();
            }
          }
          if (sealId !== undefined) {
            this.beginBoundSessionSealAbort(actor, state, sealId);
          }
        }
      };
    } catch (error) {
      let sourceRestored = false;
      if (manageMembership) {
        try {
          await this.cancelSourceActorMove(actor, state);
          sourceRestored = true;
        } catch {}
      } else {
        try {
          await this.releaseCanceledHandoff(actor, state);
          sourceRestored = true;
        } catch {
          // The Session owner keeps the seal until timeout when the source
          // queue could not be restored.
        } finally {
          state.endMove();
        }
      }
      if (sourceRestored && sealId !== undefined) {
        this.beginBoundSessionSealAbort(actor, state, sealId);
      }
      throw error;
    }
  }

  async notifyCoreSourceLeave(actor: ZLinkActor, callback: () => Promise<void>): Promise<void> {
    const pending = this.coreSourceLeaves.get(actor.context.actorId);
    let callbackResult: Promise<void>;
    try {
      callbackResult = Promise.resolve(callback());
    } catch (error) {
      callbackResult = Promise.reject(error);
    }
    // Submission, not the callback result, releases the target Join. Observe
    // a fast rejection while the submission ACK is in flight without making
    // that rejection an unhandled promise.
    void callbackResult.catch(() => {});
    try {
      if (pending !== undefined) {
        if (pending.notifySubmitted === undefined) {
          throw new Error(
            `Actor '${actor.context.actorId}' has no Core source leave submission notifier.`
          );
        }
        await pending.notifySubmitted();
        pending.resolveSubmitted();
      }
      await callbackResult;
      pending?.resolve();
    } catch (error) {
      pending?.rejectSubmitted(error);
      pending?.reject(error);
      throw error;
    } finally {
      this.coreSourceLeaves.delete(actor.context.actorId);
    }
  }

  async completeRelocationSourceLeave(
    actor: ZLinkActor,
    sourceSpotId: RoutingId | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    await this.prepareSourceActorLeave(actor, sourceSpotId, signal);
  }

  relayMaintenanceTerminal(
    actorId: string,
    packet: ZLinkActorHandoffPacket,
    result: ZLinkActorHandoffResult,
    sourceNodeRid: RoutingId,
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
    sourceNodeRid: RoutingId,
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
    signal?: AbortSignal,
    relocation?: ServiceWireOperationId
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
    const serviceWire = this.options.sessionRelocationWire?.();
    const authorityStore = this.options.authorityStore();
    const descriptors = this.options.liveDescriptors;
    if (
      relocation === undefined
      || serviceWire === undefined
      || authorityStore === undefined
      || descriptors === undefined
      || target.sessionRid === undefined
    ) {
      throw new Error(
        `Actor '${actor.context.actorId}' Session relocation requires the command 42/43 service-wire path.`
      );
    }
    const [authority, live] = await Promise.all([
        authorityStore.readAuthority(
          encodeAuthorityKey('actor', actor.context.actorId),
          signal
        ),
        descriptors(target.routerChannelId, signal)
      ]);
    const local = this.options.primaryMeshNode().status();
    const sessionOwner = live.find(value =>
        String(value.rid) === String(target.targetNodeRid)
      );
    if (
        authority.kind !== 'snapshot'
        || sessionOwner === undefined
        || String(local.routingId) !== String(actorRef.nodeRid)
        || authority.objectGeneration !== actorRef.generation
        || authority.authorityOwnerGeneration !== state.locationGeneration
        || authority.ownerLeaseGeneration !== state.ownerLeaseGeneration
        || String(authority.allocation.descriptor.rid) !== String(actorRef.nodeRid)
        || authority.allocation.descriptorLifecycleGeneration !== local.lifecycleGeneration
        || (target.sessionNodeRid !== undefined
          && String(target.sessionNodeRid) !== String(target.targetNodeRid))
    ) {
      throw new Error(
        `Actor '${actor.context.actorId}' Session route service-wire source fence is stale.`
      );
    }
    const coordinator = {
        ownerId: authority.ownerId,
        leaseGeneration: authority.ownerLeaseGeneration,
        nodeRid: String(local.routingId),
        nodeGeneration: local.lifecycleGeneration,
        expectedAuthorityStoreVersion: authority.storeVersion.value
      };
    const session = {
        sessionOwnerNodeRid: String(target.targetNodeRid),
        sessionOwnerNodeGeneration: sessionOwner.lifecycleGeneration,
        sessionOwnerId: sessionOwner.ownerId,
        sessionOwnerLeaseGeneration: sessionOwner.leaseGeneration,
        sessionRid: String(target.sessionRid),
        bindingGeneration: target.bindingGeneration
      };
    const request: ServiceSessionRelocationSeal = {
        relocation,
        coordinator,
        senderRole: 'source',
        actor: {
          actor: {
            actorId: actor.context.actorId,
            generation: actorRef.generation,
            nodeRid: String(actorRef.nodeRid)
          },
          targetNodeGeneration: local.lifecycleGeneration,
          authorityOwnerGeneration: authority.authorityOwnerGeneration,
          ownerLeaseGeneration: authority.ownerLeaseGeneration
        },
        session
      };
    await serviceWire.requestSessionRelocationSeal(
        target.routerChannelId,
        target.targetNodeRid,
        request,
        signal
      );
    return {
      ...target,
      previousAuthorityOwnerGeneration: authority.authorityOwnerGeneration,
      previousOwnerLeaseGeneration: authority.ownerLeaseGeneration,
      relocationSealId: sealId,
      serviceWireRelocation: { relocation, coordinator, session }
    };
  }

  private beginBoundSessionSealAbort(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    sealId: string
  ): void {
    const target = preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    );
    void this.abortBoundSessionRouteSeal(actor, state, sealId, target)
      .catch(error => this.options.reportPostCommitError?.(error));
  }

  private async abortBoundSessionRouteSeal(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    sealId: string,
    targetOverride?: ZLinkRemoteBoundSessionTarget
  ): Promise<void> {
    const target = targetOverride ?? preferredRemoteBoundSessionTarget(
      state.remoteBoundSessionTarget,
      state.boundSessionTransferTarget
    );
    const actorRef = state.nativeActorRef;
    if (target === undefined || actorRef === undefined || target.bindingGeneration === undefined ||
      target.previousAuthorityOwnerGeneration === undefined || target.previousOwnerLeaseGeneration === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' Session route seal cannot be released without its exact fence.`);
    }
    const serviceFence = target.serviceWireRelocation;
    const serviceWire = this.options.sessionRelocationWire?.();
    if (serviceFence === undefined || serviceWire === undefined) {
      throw new Error(
        `Actor '${actor.context.actorId}' Session route service-wire release is unavailable.`
      );
    }
    if (target.relocationSealId !== sealId) {
      throw new Error(
        `Actor '${actor.context.actorId}' Session route seal release changed its seal identity.`
      );
    }
    await serviceWire.sendSessionRelocationRoute(
        target.routerChannelId,
        target.targetNodeRid,
        {
          relocation: serviceFence.relocation,
          coordinator: serviceFence.coordinator,
          senderRole: 'source',
          actor: {
            actorId: actor.context.actorId,
            generation: actorRef.generation,
            nodeRid: String(actorRef.nodeRid)
          },
          session: serviceFence.session,
          route: {
            action: 'abort',
            currentAuthorityOwnerGeneration: target.previousAuthorityOwnerGeneration
          }
        },
        this.options.shutdownSignal?.()
    );
  }

  private beginCoreSourceLeave(actorId: string): {
    readonly completion: Promise<void>;
    readonly submitted: Promise<void>;
  } {
    let resolve!: () => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<void>((accept, fail) => {
      resolve = accept;
      reject = fail;
    });
    let resolveSubmitted!: () => void;
    let rejectSubmitted!: (error: unknown) => void;
    const submitted = new Promise<void>((accept, fail) => {
      resolveSubmitted = accept;
      rejectSubmitted = fail;
    });
    void promise.catch(() => {});
    void submitted.catch(() => {});
    this.coreSourceLeaves.set(actorId, {
      promise,
      resolve,
      reject,
      submitted,
      resolveSubmitted,
      rejectSubmitted
    });
    return { completion: promise, submitted };
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

  async commitRoutedActorAuthority(
    actor: ZLinkActor,
    sourceActorRef: ActorRef,
    transferId: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    deadlineAtMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const store = this.options.authorityStore();
    const owner = this.options.currentOwner();
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    const actorType = state?.actorType;
    const local = this.options.primaryMeshNode().status();
    const targetActorRef = state?.nativeActorRef === undefined
      ? undefined
      : toFrameworkActorRef(state.nativeActorRef, actor.context.meshName);
    if (
      store === undefined
      || owner === undefined
      || state === undefined
      || actorType === undefined
      || targetActorRef === undefined
      || local.lifecycleGeneration <= 0n
      || spotGeneration <= 0n
      || membershipEpoch <= 0n
      || !Number.isSafeInteger(deadlineAtMs)
    ) {
      throw new Error(
        `Actor '${actor.context.actorId}' target authority commit has an incomplete materialization fence.`
      );
    }
    const key = encodeAuthorityKey('actor', actor.context.actorId);
    const expected = await store.readAuthority(key, signal);
    if (expected.kind !== 'snapshot') {
      throw new Error(`Actor '${actor.context.actorId}' source authority is missing at the target.`);
    }
    const exactTarget = (value: ZLinkAuthoritySnapshot): boolean =>
      value.allocation.state === 'active'
      && value.allocation.objectKind === 'actor'
      && value.allocation.stableType === actorType
      && value.objectGeneration === targetActorRef.objectGeneration
      && value.ownerId === owner.ownerId
      && value.ownerLeaseGeneration === owner.leaseGeneration
      && value.allocation.descriptor.meshName === actor.context.meshName
      && routingIdsEqual(value.allocation.descriptor.rid, local.routingId)
      && value.allocation.descriptorLifecycleGeneration === local.lifecycleGeneration;
    const adopt = (value: ZLinkAuthoritySnapshot): ZLinkAuthoritySnapshot => {
      state.setLocationGeneration(value.authorityOwnerGeneration);
      state.setOwnerLeaseGeneration(value.ownerLeaseGeneration);
      state.setJoinedSpot(spotId, state.spot, membershipEpoch, spotGeneration);
      state.markLocationOwned();
      return value;
    };
    if (exactTarget(expected)) return adopt(expected);
    if (
      expected.allocation.state !== 'active'
      || expected.allocation.objectKind !== 'actor'
      || expected.allocation.stableType !== actorType
      || expected.objectGeneration !== sourceActorRef.objectGeneration
      || expected.allocation.descriptor.meshName !== sourceActorRef.meshName
      || !routingIdsEqual(expected.allocation.descriptor.rid, sourceActorRef.nodeRid)
    ) {
      throw new Error(
        `Actor '${actor.context.actorId}' relocation source authority is stale at the target.`
      );
    }
    const membershipMutation = Buffer.from(JSON.stringify({
      actorId: actor.context.actorId,
      actorGeneration: targetActorRef.objectGeneration.toString(),
      spotId: String(spotId),
      spotGeneration: spotGeneration.toString(),
      membershipEpoch: membershipEpoch.toString()
    }), 'utf8');
    const request = {
      aggregateId: { value: transferId } as ZLinkAggregateId,
      aggregateGeneration: local.lifecycleGeneration,
      participants: [{
        authorityKey: key,
        expectedStoreVersion: expected.storeVersion,
        ownerTransition: 'newOwner' as const,
        authorityPayload: rewriteActorAuthorityRoute(
          expected.payload,
          targetActorRef,
          String(spotId),
          spotGeneration,
          local.lifecycleGeneration,
          owner
        ),
        membershipMutation
      }],
      inventoryDigest: createHash('sha256').update(membershipMutation).digest(),
      targetDescriptor: {
        meshName: actor.context.meshName,
        rid: local.routingId
      },
      targetDescriptorLifecycleGeneration: local.lifecycleGeneration,
      capacity: expected.allocation.capacity,
      targetOwner: owner
    };
    let prepared: Awaited<ReturnType<typeof store.prepareAggregate>> | undefined;
    let firstError: unknown;
    for (;;) {
      signal?.throwIfAborted();
      try {
        prepared = await store.prepareAggregate(request, signal);
      } catch (error) {
        firstError ??= error;
      }
      if (prepared?.kind === 'prepared' || prepared?.kind === 'alreadyPrepared') break;
      const observed = await this.readActorAuthorityForRelocation(store, key, signal);
      if (observed !== undefined && exactTarget(observed)) return adopt(observed);
      if (prepared !== undefined || observed !== undefined && !sameSourceActorAuthority(observed, expected)) {
        throw new Error(
          `Actor '${actor.context.actorId}' target authority prepare was rejected.`,
          { cause: firstError }
        );
      }
      await waitForActorAuthorityRetry(deadlineAtMs, signal, firstError);
    }
    for (;;) {
      signal?.throwIfAborted();
      let committed: Awaited<ReturnType<typeof store.commitAggregate>> | undefined;
      try {
        committed = await store.commitAggregate(prepared.fence, signal);
      } catch (error) {
        firstError ??= error;
      }
      const observed = await this.readActorAuthorityForRelocation(store, key, signal);
      if (observed !== undefined && exactTarget(observed)) return adopt(observed);
      if (
        committed?.kind === 'stale'
        || committed?.kind === 'generationExhausted'
        || observed !== undefined && !sameSourceActorAuthority(observed, expected)
      ) {
        throw new Error(
          `Actor '${actor.context.actorId}' target authority commit did not converge.`,
          { cause: firstError }
        );
      }
      await waitForActorAuthorityRetry(deadlineAtMs, signal, firstError);
    }
  }

  private async readActorAuthorityForRelocation(
    store: ZLinkAuthorityStore,
    key: ReturnType<typeof encodeAuthorityKey>,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot | undefined> {
    try {
      const current = await store.readAuthority(key, signal);
      return current.kind === 'snapshot' ? current : undefined;
    } catch {
      return undefined;
    }
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
      await this.installNativeActorPacketTarget(state, spotId, spotGeneration, node);
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
    await this.installNativeActorPacketTarget(state, spotId, spotGeneration, node);
  }

  private async installNativeActorPacketTarget(
    state: ZLinkActorRuntimeState,
    spotId: RoutingId,
    spotGeneration: bigint,
    node: ZLinkBackendMeshNode
  ): Promise<void> {
    const target = await this.options.spotRouteResolver?.()?.resolve(spotId);
    if (target === undefined || target.spotKind === 'entry') return;
    if (
      !routingIdsEqual(target.spotId, spotId)
      || !routingIdsEqual(target.targetNodeRid, node.status().routingId)
      || target.targetSpotGeneration !== spotGeneration
    ) {
      throw new Error('Committed native Actor target does not match its Ready Spot route.');
    }
    state.setRemoteActorPacketTarget(target);
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
    if (boundSessionTarget?.serviceWireRelocation === undefined) return;
    await this.publishBoundSessionOwnership(
      actor.context.actorId,
      actorRef,
      generation,
      boundSessionTarget,
      state.ownerLeaseGeneration
    );
  }

  async openRoutedActorSession(_actor: ZLinkActor): Promise<void> {}

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
    targetOwnerLeaseGeneration: bigint | undefined
  ): Promise<void> {
    if (target === undefined) {
      return;
    }
    const serviceFence = target.serviceWireRelocation;
    const serviceWire = serviceFence === undefined
      ? undefined
      : this.options.sessionRelocationWire?.();
    if (serviceFence === undefined) {
      throw new Error(`Actor '${actorId}' command 44 has no service-wire relocation fence.`);
    }
    {
      if (
        target.bindingGeneration === undefined
        || target.previousAuthorityOwnerGeneration === undefined
        || target.bindingGeneration <= 0n
        || target.previousAuthorityOwnerGeneration < 0n
        || ownershipGeneration <= target.previousAuthorityOwnerGeneration
      ) {
        throw new Error(`Actor '${actorId}' command 44 Session route fence is incomplete.`);
      }
      if (serviceWire === undefined) {
        throw new Error(`Actor '${actorId}' command 44 service-wire bridge is unavailable.`);
      }
      const authority = await this.options.authorityStore()?.readAuthority(
        encodeAuthorityKey('actor', actorId),
        this.options.shutdownSignal?.()
      );
      if (
        authority?.kind !== 'snapshot'
        || authority.objectGeneration !== actorRef.generation
        || authority.authorityOwnerGeneration !== ownershipGeneration
        || authority.ownerLeaseGeneration !== targetOwnerLeaseGeneration
        || String(authority.allocation.descriptor.rid) !== String(actorRef.nodeRid)
      ) {
        throw new Error(`Actor '${actorId}' command 44 target authority fence is stale.`);
      }
      await serviceWire.sendSessionRelocationRoute(
        target.routerChannelId,
        target.targetNodeRid,
        {
          relocation: serviceFence.relocation,
          coordinator: serviceFence.coordinator,
          senderRole: 'target',
          actor: {
            actorId,
            generation: actorRef.generation,
            nodeRid: String(actorRef.nodeRid)
          },
          session: serviceFence.session,
          route: {
            action: 'commit',
            previousAuthorityOwnerGeneration: target.previousAuthorityOwnerGeneration,
            targetAuthorityOwnerGeneration: ownershipGeneration,
            targetNodeRid: String(actorRef.nodeRid),
            targetNodeGeneration: authority.allocation.descriptorLifecycleGeneration
          }
        },
        this.options.shutdownSignal?.()
      );
      return;
    }
  }
}

function sameSourceActorAuthority(
  current: ZLinkAuthoritySnapshot,
  expected: ZLinkAuthoritySnapshot
): boolean {
  return current.storeVersion.value === expected.storeVersion.value
    && current.objectGeneration === expected.objectGeneration
    && current.authorityOwnerGeneration === expected.authorityOwnerGeneration
    && current.ownerId === expected.ownerId
    && current.ownerLeaseGeneration === expected.ownerLeaseGeneration
    && current.allocation.state === expected.allocation.state
    && current.allocation.objectKind === expected.allocation.objectKind
    && current.allocation.stableType === expected.allocation.stableType
    && current.allocation.descriptor.meshName === expected.allocation.descriptor.meshName
    && routingIdsEqual(
      current.allocation.descriptor.rid,
      expected.allocation.descriptor.rid
    )
    && current.allocation.descriptorLifecycleGeneration
      === expected.allocation.descriptorLifecycleGeneration;
}

async function waitForActorAuthorityRetry(
  deadlineAtMs: number,
  signal: AbortSignal | undefined,
  cause: unknown
): Promise<void> {
  signal?.throwIfAborted();
  const remaining = deadlineAtMs - Date.now();
  if (remaining <= 0) {
    throw new Error('Actor target authority deadline expired.', { cause });
  }
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(resolve, Math.min(10, remaining));
    timer.unref();
    const abort = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };
    signal?.addEventListener('abort', abort, { once: true });
    timer.refresh();
  });
}

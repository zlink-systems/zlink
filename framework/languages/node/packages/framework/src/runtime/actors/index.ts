import { encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  type ActorRef,
  type RoutingId,
  type SpotRef,
  type ZLinkActor,
  type ZLinkActorCreateCall,
  type ZLinkActorCreateResult,
  type ZLinkActorGetOrCreateCall,
  type ZLinkActorManager,
  ZLinkEncodedPayload,
  ZLinkFrameworkException,
  ZLinkMessage
} from '../../contracts';
import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import { awaitWithAbort, throwIfAborted } from '../abort';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode,
  ZLinkBackendSpotNode
} from '../backend/contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import type { ServiceActorRouteFence } from '../foundation/service-stateful-wire-codec';
import { routingIdsEqual } from '../routing-id';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { ZLinkConfigurationException } from '../configuration';
import { METRIC_NAMES } from '../diagnostics/runtime-metrics';
import {
  captureZLinkSpotSerialTurn,
  requireZLinkYieldTurn,
  type ZLinkSpotSerialTurn
} from '../execution';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  wireReplyFailureException
} from '../framework-errors-internal';
import { disposeLifecycleHandlers } from '../handlers/handler-instance-scope';
import { currentZLinkActorExecution } from './actor-execution-context';
import { encodeFrameworkPayloadMessage } from '../messaging/payload-codec';
import { ZLinkActorCreationCoordinator, type ZLinkActorCreateRequest } from './actor-creation';
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';
import {
  startActorTeardown,
  ZLinkActorRuntimeState,
  toFrameworkActorRef,
  type ZLinkActorCreationAttemptResult
} from './actor-runtime-state';
import { ZLinkTransferredActorRollbackCoordinator } from './transferred-actor-rollback';
const ACTOR_CREATION_RETRY_INTERVAL_MS = 5;

export { DefaultZLinkActorClient, type ZLinkActorClientOptions } from './actor-client';
export { DefaultZLinkActorContext, ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME } from './actor-context';
export { ZLinkActorSerialExecutor } from './actor-mailbox';
export {
  DEFAULT_MESSAGE_FOLLOW_DURATION_MS,
  ZLinkActorHandoffCoordinator,
  decodeHandoffPacket,
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffDispatch,
  type ZLinkActorHandoffPrefixAdmission,
  type ZLinkActorHandoffPrefixQueue,
  type ZLinkActorHandoffPrefixRecord,
  type ZLinkActorHandoffPreparedReplayAdmission,
  type ZLinkActorHandoffReplayAdmission,
  type ZLinkActorHandoffReplayPreparation,
  type ZLinkActorHandoffResult
} from './actor-handoff';
export {
  actorMessageFollowContext,
  attachActorMessageFollowContext,
  decodeActorMessageFollowContext,
  type ZLinkActorMessageFollowContext,
  type ZLinkActorMessageFollowOwnerFence
} from './actor-message-follow-context';
export {
  ZLinkActorRuntimeState,
  toFrameworkActorRef,
  toFrameworkRoutingId,
  type ZLinkActorCreationOperation,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from './actor-runtime-state';
export {
  createDeferredJoinCompletion,
  type ZLinkDeferredJoinCompletion
} from './deferred-join-completion';
export {
  decodeActorAuthorityIdentity,
  decodeRelocatingActorAuthorityIdentity,
  encodeActorAuthorityIdentity,
  isActorAuthorityPayload,
  rewriteActorAuthorityRoute,
  rewriteActorAuthorityOwner,
  publishInitialActorAuthority,
  type ZLinkActorAuthorityIdentity
} from './actor-authority-publication';
export {
  ZLinkActorPacketKind,
  ZLinkSpotActorDispatcher,
  ZLinkSpotActorHandlerRegistryRuntime,
  type ZLinkActorPacketDescriptor,
  type ZLinkSpotActorDispatcherOptions
} from './spot-actor-dispatch';
export {
  ZLinkActorNativeJoinCoordinator,
  type ZLinkActorNativeJoinCoordinatorOptions
} from './actor-remote-joiner';
export type { ZLinkActorRoutedJoinTransport } from './actor-routed-join-transport';
export type {
  ZLinkActorBoundSessionFactory,
  ZLinkActorJoinCoordinator,
  ZLinkActorManagerOptions
} from './actor-runtime-contracts';
export {
  ZLinkActorTransferRegistry,
  type ZLinkActorTransferPayloadState
} from './actor-transfer-registry';
export {
  ZLINK_REMOTE_ACTOR_JOIN_PACKET,
  ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL,
  decodeRemoteActorSourceLeaveTerminal
} from './actor-remote-wire';
export {
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET
} from './actor-packet-relay-wire';
export {
  ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_FENCE_NACK_CODE,
  ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
  encodeRemoteBoundSessionNack,
  isRemoteBoundSessionFenceError,
  ZLinkRemoteBoundSessionFenceError
} from './bound-session-wire';
export {
  actorJoinIdentity,
  createActorJoinRequest,
  createActorMembership,
  ZLINK_ACTOR_LIFECYCLE_SNAPSHOT,
  type ZLinkActorLifecycleSnapshotSource
} from './actor-lifecycle-snapshot';

export class DefaultZLinkActorManager implements ZLinkActorManager {
  private readonly states = new Map<string, ZLinkActorRuntimeState>();
  private readonly actorMeshNames = new Map<string, string>();
  private readonly relocationStaged = new Set<string>();
  private readonly creation: ZLinkActorCreationCoordinator;
  private readonly transferredActorRollback: ZLinkTransferredActorRollbackCoordinator;

  constructor(
    private readonly options: ZLinkActorManagerOptions,
    private readonly detachedTaskRunner: import('../spots/spot-actor-join-dispatch').ZLinkDetachedTaskRunner
  ) {
    if ((detachedTaskRunner as unknown) === undefined) {
      throw new Error('Actor manager requires a detached task runner.');
    }
    this.creation = new ZLinkActorCreationCoordinator(options);
    this.transferredActorRollback = new ZLinkTransferredActorRollbackCoordinator(
      this.states,
      options,
      (actorId, state) => this.finalizeDestroyedActor(actorId, state)
    );
  }

  create(actorId: string, actorType: string): ZLinkActorCreateCall {
    return new ZLinkActorCreateCallRuntime((options, signal) =>
      this.submitCreate(actorId, actorType, true, options, signal)
    );
  }

  async find(actorId: string, signal?: AbortSignal): Promise<ActorRef | undefined> {
    throwIfAborted(signal);
    const state = this.publishedState(actorId);
    if (state?.isDestroying === true) return undefined;
    if (state?.actor !== undefined && state.nativeActorRef !== undefined) {
      return this.actorRefForState(state);
    }
    return await this.options.actorRefResolver?.resolveActorRef(actorId, signal);
  }

  getOrCreate(actorId: string, actorType: string): ZLinkActorGetOrCreateCall {
    return new ZLinkActorGetOrCreateCallRuntime((options, signal) =>
      this.submitCreate(actorId, actorType, false, options, signal)
    );
  }

  async findSpot(actorId: string, signal?: AbortSignal): Promise<SpotRef | undefined> {
    throwIfAborted(signal);
    const state = this.states.get(actorId);
    if (
      state?.actor === undefined ||
      state.spotId === undefined ||
      state.nativeActorRef === undefined
    )
      return undefined;
    const spotId = state.spotId;
    const actorRef = this.actorRefForState(state);
    return {
      spotId: spotId,
      objectGeneration: state.spotGeneration ?? 0n,
      meshName: this.actorMeshNames.get(actorId) ?? '',
      nodeRid: actorRef.nodeRid
    };
  }

  async destroy(actor: ActorRef, signal?: AbortSignal): Promise<boolean> {
    throwIfAborted(signal);
    const state = this.publishedState(actor.actorId);
    if (state?.actor === undefined) {
      return (await this.options.remoteDestroy?.(actor, signal)) ?? false;
    }
    const current = this.actorRefForState(state);
    if (current.objectGeneration !== actor.objectGeneration) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorGenerationStale,
        `Actor '${actor.actorId}' generation is stale.`
      );
    }
    const node = this.options.nativeActorNodeProvider?.() ?? this.options.nativeActorNode;
    const completions = this.options.nativeActorCompletionTableProvider?.();
    if (node === undefined || completions === undefined) return false;
    await this.destroyLocal(
      state,
      state.entryNodeRid ?? current.nodeRid,
      node,
      completions,
      signal
    );
    return true;
  }

  async destroyOwnedActor(
    fence: ServiceActorRouteFence,
    node: ZLinkBackendMeshNode,
    completions: ZLinkMeshCompletionTable
  ): Promise<void> {
    const { actor } = fence;
    const state = this.publishedState(actor.actorId);
    const current = state?.nativeActorRef;
    if (
      state === undefined ||
      !state.ownsLocation ||
      current === undefined ||
      current.generation !== actor.generation ||
      !routingIdsEqual(current.nodeRid, actor.nodeRid) ||
      state.locationGeneration !== fence.authorityOwnerGeneration ||
      this.options.locationLifecycle?.currentOwnerToken?.leaseGeneration !==
        fence.ownerLeaseGeneration
    ) {
      return;
    }
    await this.destroyLocal(state, current.nodeRid, node, completions);
  }

  private async destroyLocal(
    state: ZLinkActorRuntimeState,
    entryNodeRid: RoutingId,
    node: ZLinkBackendMeshNode,
    completions: ZLinkMeshCompletionTable,
    signal?: AbortSignal
  ): Promise<void> {
    await this.destroyState(state, entryNodeRid, signal, async (nativeRef) => {
      const completion = await completions.submit(() => node.destroyActor(nativeRef, 0));
      try {
        if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
          throw wireReplyFailureException(
            completion.terminalResult,
            completion.failureErrno,
            `Actor '${state.actorId}' destroy failed with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
          );
        }
      } finally {
        closeMeshCompletion(completion);
      }
    });
  }

  async materializeTransferredActor(
    actorId: string,
    actorType: string,
    adapterKey: string | undefined,
    transferState: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<{ readonly actor: ZLinkActor; readonly actorRef: ActorRef }> {
    throwIfAborted(signal);
    const state = this.getOrCreateState(actorId);
    this.rememberActorMeshFromType(actorId, actorType);
    if (state.actor !== undefined) {
      await disposeLifecycleHandlers(state.actor, this.detachedTaskRunner);
    }
    state.prepareForRemoteReentry();
    const operation = state.getOrStartCreation(actorType, false, () =>
      this.creation.materializeTransferredActor(
        actorId,
        actorType,
        state,
        adapterKey === undefined
          ? undefined
          : (actor) => {
              const registry = this.options.actorTransferRegistry;
              if (registry === undefined) {
                throw new ZLinkConfigurationException('Actor transfer registry is not configured.');
              }
              return registry.restore(adapterKey, actor, transferState, signal);
            }
      )
    );
    try {
      const result = await operation.task;
      if (result.status !== 'created') {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorCreateRejected,
          `Transferred Actor '${actorId}' materialization was rejected.`
        );
      }
      const actor = result.actor;
      return { actor, actorRef: this.actorRefForState(state) };
    } catch (error) {
      state.clearFailedCreation(operation.task);
      throw error;
    }
  }

  async rollbackTransferredActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void> {
    await this.transferredActorRollback.rollback(actor, signal);
  }

  private async submitCreate(
    actorId: string,
    actorType: string,
    failIfExists: boolean,
    options: ZLinkActorCreateCallOptions,
    signal?: AbortSignal
  ): Promise<ZLinkActorCreateResult> {
    const meshName = options.meshName ?? this.options.actorMeshNameProvider?.(actorType);
    if (meshName === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
        `Actor type '${actorType}' has no configured object-role RouteMesh.`
      );
    }
    requireActorMeshName(meshName);
    this.ensureActorTypeBelongsToMesh(meshName, actorType);
    if (this.options.placementCreate !== undefined) {
      return await this.options.placementCreate(actorId, actorType, failIfExists, options, signal);
    }
    const localState = this.states.get(actorId);
    if (localState?.actorType !== undefined && localState.actorType !== actorType) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorTypeMismatch,
        `Actor '${actorId}' is registered as type '${localState.actorType}', not '${actorType}'.`
      );
    }
    const existing = await this.find(actorId, signal);
    if (existing !== undefined) {
      if (failIfExists) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
          `Actor '${actorId}' already exists.`
        );
      }
      return { status: 'existing', actor: existing };
    }
    this.rememberActorMesh(actorId, meshName);
    const result = await this.createOrGet(
      actorId,
      actorType,
      failIfExists,
      options.request,
      signal,
      true,
      options.timeoutMs
    );
    if (result.status === 'rejected') {
      return result.reply === undefined
        ? { status: 'rejected' }
        : { status: 'rejected', reply: result.reply };
    }
    if (result.status === 'failed') {
      throw actorApplicationFailure(actorId, result.error);
    }
    const actorRef = result.actorRef ?? this.actorRefForState(this.states.get(actorId)!);
    return result.reply === undefined
      ? { status: result.status, actor: actorRef }
      : { status: result.status, actor: actorRef, reply: result.reply };
  }

  private ensureActorTypeBelongsToMesh(meshName: string, actorType: string): void {
    const registeredMesh = this.options.actorMeshNameProvider?.(actorType);
    if (registeredMesh !== undefined && registeredMesh !== meshName) {
      throw new ZLinkConfigurationException(
        `Actor type '${actorType}' belongs to RouteMesh '${registeredMesh}', not '${meshName}'.`
      );
    }
  }

  private rememberActorMesh(actorId: string, meshName: string): void {
    const existing = this.actorMeshNames.get(actorId);
    if (existing !== undefined && existing !== meshName) {
      throw new ZLinkConfigurationException(
        `Actor '${actorId}' belongs to RouteMesh '${existing}', not '${meshName}'.`
      );
    }
    this.actorMeshNames.set(actorId, meshName);
    this.states.get(actorId)?.rememberMeshName(meshName);
  }

  private rememberActorMeshFromType(actorId: string, actorType: string): void {
    const meshName = this.options.actorMeshNameProvider?.(actorType);
    if (meshName !== undefined) {
      this.rememberActorMesh(actorId, meshName);
    }
  }

  async findActor(actorId: string, signal?: AbortSignal): Promise<ZLinkActor | undefined> {
    throwIfAborted(signal);
    return this.resolveLocalActor(actorId);
  }

  /** @internal Uses the relocation owner's publication boundary for local dispatch. */
  resolveLocalActor(actorId: string): ZLinkActor | undefined {
    return this.publishedState(actorId)?.actor;
  }

  requireRelocationActorFactory(actorType: string): void {
    if (!this.options.actorFactories.has(actorType)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor factory '${actorType}' is not registered.`
      );
    }
  }

  async prepareRelocationActor(
    actorId: string,
    actorType: string,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId | undefined,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    signal?: AbortSignal,
    transfer?: {
      readonly adapterKey: string;
      readonly state: ZLinkMessage;
    },
    stageIngress?: () => (reason?: unknown) => void
  ): Promise<ZLinkActor> {
    if (this.states.has(actorId) || this.relocationStaged.has(actorId)) {
      throw new Error(`Actor '${actorId}' already exists on the relocation target.`);
    }
    const node = this.options.nativeActorNodeProvider?.() ?? this.options.nativeActorNode;
    const restore = node?.restoreActorAuthority;
    if (node === undefined || restore === undefined) {
      throw new Error('Actor relocation requires native authority restore support.');
    }
    this.relocationStaged.add(actorId);
    let nativeRef: ZLinkBackendActorRef | undefined;
    let cancelIngress: ((reason?: unknown) => void) | undefined;
    try {
      nativeRef = restore.call(
        node,
        actorId,
        actorType,
        objectGeneration,
        authorityOwnerGeneration,
        spotId === undefined ? undefined : String(spotId),
        spotGeneration,
        membershipEpoch
      );
      this.rememberActorMeshFromType(actorId, actorType);
      const state = this.getOrCreateState(actorId);
      state.setNativeActorRef(nativeRef);
      state.setLocationGeneration(authorityOwnerGeneration);
      cancelIngress = stageIngress?.();
      const operation = state.getOrStartCreation(actorType, false, () =>
        this.creation.materializeTransferredActor(actorId, actorType, state, undefined)
      );
      const result = await operation.task;
      if (result.status !== 'created') {
        throw new Error(`Actor '${actorId}' relocation materialization was rejected.`);
      }
      const actor = result.actor;
      if (transfer !== undefined) {
        const registry = this.options.actorTransferRegistry;
        if (registry === undefined) {
          throw new ZLinkConfigurationException('Actor transfer registry is not configured.');
        }
        await registry.restore(transfer.adapterKey, actor, transfer.state, signal);
      }
      if (spotId !== undefined)
        state.setJoinedSpot(spotId, undefined, membershipEpoch, spotGeneration);
      return actor;
    } catch (error) {
      cancelIngress?.(error);
      if (nativeRef !== undefined) node.discardRelocatedActor?.(nativeRef);
      this.relocationStaged.delete(actorId);
      const failedState = this.states.get(actorId);
      await this.finalizeActorState(failedState);
      throw error;
    }
  }

  publishRelocationActor(actorId: string): void {
    if (!this.states.has(actorId)) {
      throw new Error(`Actor '${actorId}' relocation staging is missing.`);
    }
    this.relocationStaged.delete(actorId);
  }

  async abortRelocationActor(actorId: string): Promise<void> {
    await this.releaseLocalActor(actorId);
  }

  async completeRelocationSource(
    actorId: string,
    sourceActorRef?: ZLinkBackendActorRef
  ): Promise<void> {
    await this.releaseLocalActor(actorId, sourceActorRef);
  }

  /** Commit removes current ownership; a Join profile may still own its leave callback. */
  detachRelocationSource(state: ZLinkActorRuntimeState): void {
    if (this.detachActorStateRegistration(state)) {
      this.discardNativeRelocationSource(state.nativeActorRef);
    }
  }

  async releaseSpotActorMembers(meshName: string, spotId?: RoutingId): Promise<void> {
    const results = await Promise.allSettled(
      [...this.states.values()]
        .filter((state) => state.meshName === meshName && state.spotId === spotId)
        .map((state) => this.releaseLocalActor(state.actorId))
    );
    const errors = results
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Actor member cleanup failed.');
  }

  private async releaseLocalActor(
    actorId: string,
    exactNativeActorRef?: ZLinkBackendActorRef
  ): Promise<void> {
    const state = this.states.get(actorId);
    if (state === undefined) return;
    const failures: unknown[] = [];
    try {
      this.discardNativeRelocationSource(exactNativeActorRef ?? state.nativeActorRef);
    } catch (error) {
      failures.push(error);
    }
    try {
      await this.completeCoreRelocationSource(actorId, state);
    } catch (error) {
      failures.push(error);
    }
    if (failures.length > 0) {
      throw failures.length === 1
        ? failures[0]
        : new AggregateError(failures, 'Actor relocation cleanup failed.');
    }
  }

  async completeCoreRelocationSource(
    actorId: string,
    state = this.states.get(actorId)
  ): Promise<void> {
    await this.finalizeActorState(state);
  }

  private discardNativeRelocationSource(nativeActorRef: ZLinkBackendActorRef | undefined): void {
    if (nativeActorRef === undefined) return;
    const node = this.options.nativeActorNodeProvider?.() ?? this.options.nativeActorNode;
    node?.discardRelocatedActor?.(nativeActorRef);
  }

  private trackExecution(state: ZLinkActorRuntimeState): void {
    const actor = state.actor;
    if (actor === undefined) return;
    this.options.locationLifecycle?.bindActorExecution(state.actorId, actor);
    this.options.locationLifecycle?.trackExecution(
      actor,
      () => state.isSteadyReady && !this.relocationStaged.has(state.actorId),
      (error) => {
        if (!state.isSteadyReady) return;
        const failures: unknown[] = [];
        for (const cleanup of [
          () => this.options.discardActorActivation?.(actor, error),
          () => this.discardNativeRelocationSource(state.nativeActorRef)
        ]) {
          try {
            cleanup();
          } catch (error) {
            failures.push(error);
          }
        }
        const cleanup = (async () => {
          try {
            await this.options.actorDestroyedCleanup?.(state.actorId);
          } catch (error) {
            failures.push(error);
          }
        })();
        state.clearAfterDestroy();
        if (this.detachActorStateRegistration(state))
          this.options.metrics?.change(METRIC_NAMES.ActorCount, -1);
        const disposal = (async () => {
          await cleanup;
          try {
            await disposeLifecycleHandlers(actor, this.detachedTaskRunner);
          } catch (error) {
            failures.push(error);
          }
          if (failures.length > 0)
            throw new AggregateError(failures, 'Actor local disposal failed.');
        })();
        this.detachedTaskRunner.runDetached('Discard Actor execution', () => disposal);
        return disposal;
      }
    );
  }

  private async disposeActorHandlers(state: ZLinkActorRuntimeState): Promise<void> {
    if (state.actor !== undefined) {
      this.options.locationLifecycle?.forgetExecution(state.actor);
      await disposeLifecycleHandlers(state.actor, this.detachedTaskRunner);
    }
  }

  private async finalizeActorState(state: ZLinkActorRuntimeState | undefined): Promise<void> {
    if (state === undefined) return;
    try {
      await this.disposeActorHandlers(state);
    } finally {
      state.clearAfterDestroy();
      this.detachActorStateRegistration(state);
    }
  }

  private detachActorStateRegistration(state: ZLinkActorRuntimeState): boolean {
    if (this.states.get(state.actorId) !== state) return false;
    this.states.delete(state.actorId);
    this.actorMeshNames.delete(state.actorId);
    this.relocationStaged.delete(state.actorId);
    return true;
  }

  private async finalizeDestroyedActor(
    actorId: string,
    state: ZLinkActorRuntimeState
  ): Promise<void> {
    if (this.states.get(actorId) !== state) return;
    try {
      await this.options.actorDestroyedCleanup?.(actorId);
    } finally {
      await this.finalizeActorState(state);
      this.options.metrics?.change(METRIC_NAMES.ActorCount, -1);
    }
  }

  private destroyState(
    state: ZLinkActorRuntimeState,
    entryNodeRid: RoutingId,
    signal: AbortSignal | undefined,
    destroyNative: (actorRef: ZLinkBackendActorRef) => Promise<void>
  ): Promise<void> {
    if (state.hasPendingMembershipTransition) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorMoving,
        `Actor '${state.actorId}' has a pending membership transition.`
      );
    }
    const self = currentZLinkActorExecution()?.actorId === state.actorId;
    const operation = state.getOrStartDestroy(entryNodeRid, (actorRef) => {
      if (state.isJoined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
          `Actor '${state.actorId}' must leave its current SPOT before destroy.`
        );
      }
      const current = state.nativeActorRef === undefined ? undefined : this.actorRefForState(state);
      const publishClosing = async () => {
        if (current !== undefined && state.ownsLocation) {
          await this.options.locationLifecycle?.beginActorClosing(current);
        }
      };
      const teardown = async () => {
        if (actorRef !== undefined) await destroyNative(actorRef);
        try {
          await this.options.actorDestroyedCleanup?.(state.actorId);
        } finally {
          await this.disposeActorHandlers(state);
        }
        if (state.actorType !== undefined && state.ownsLocation) {
          await this.options.locationLifecycle?.releaseActor(
            state.actorType,
            state.actorId,
            current
          );
        }
        state.clearAfterDestroy();
        if (this.detachActorStateRegistration(state)) {
          this.options.metrics?.change(METRIC_NAMES.ActorCount, -1);
        }
      };
      const actor = state.actor;
      const operation =
        actor === undefined || this.options.terminateActorActivation === undefined
          ? startActorTeardown(publishClosing, teardown)
          : this.options.terminateActorActivation(actor, publishClosing, teardown);
      if (self) {
        this.detachedTaskRunner.runDetached('Actor terminal cleanup', () => operation.terminal);
      }
      return operation;
    });
    return awaitWithAbort(self ? operation.accepted : operation.terminal, signal);
  }

  async getOrCreateActor(
    actorId: string,
    actorType: string,
    signal?: AbortSignal
  ): Promise<ZLinkActor> {
    this.rememberActorMeshFromType(actorId, actorType);
    const result = await this.createOrGet(actorId, actorType, false, undefined, signal);
    return requireCreatedActor(result, actorId);
  }

  async getOrCreateWithNativeRef(
    actorId: string,
    actorType: string,
    actorRef: ZLinkBackendActorRef,
    createRequest?: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkActor> {
    this.rememberActorMeshFromType(actorId, actorType);
    const state = this.getOrCreateState(actorId);
    state.setNativeActorRef(actorRef);
    const result = await this.createOrGet(actorId, actorType, false, createRequest, signal, false);
    return requireCreatedActor(result, actorId);
  }

  async getOrCreateActorResult(
    actorId: string,
    actorType: string,
    createRequest?: unknown,
    signal?: AbortSignal
  ): Promise<ZLinkActorLocalCreateResult<ActorRef | undefined>> {
    this.rememberActorMeshFromType(actorId, actorType);
    return await this.createOrGet(actorId, actorType, false, createRequest, signal);
  }

  async createReservedActorResult(
    actorId: string,
    actorType: string,
    createRequest: unknown,
    signal?: AbortSignal,
    nativeActorRef?: ZLinkBackendActorRef
  ): Promise<ZLinkActorLocalCreateResult> {
    this.rememberActorMeshFromType(actorId, actorType);
    const result = await this.createOrGet(
      actorId,
      actorType,
      false,
      createRequest,
      signal,
      false,
      DEFAULT_REQUEST_TIMEOUT_MS,
      nativeActorRef
    );
    if (result.status === 'failed' || result.status === 'rejected') return result;
    return {
      ...result,
      actorRef: this.actorRefForState(this.states.get(actorId)!)
    };
  }

  adoptCreatedAuthority(
    actorId: string,
    authority: import('../locations/internal-location-contracts').ZLinkAuthoritySnapshot
  ): void {
    const state = this.states.get(actorId);
    if (state === undefined) {
      throw new Error(`Actor '${actorId}' is not materialized.`);
    }
    this.options.locationLifecycle?.confirmAuthority(
      encodeAuthorityKey('actor', actorId),
      authority
    );
    state.setLocationGeneration(authority.authorityOwnerGeneration);
    state.markLocationOwned();
  }

  getState(actorId: string): ZLinkActorRuntimeState | undefined {
    return this.states.get(actorId);
  }

  /** @internal Returns state only when the Actor is published. */
  publishedState(actorId: string): ZLinkActorRuntimeState | undefined {
    return this.relocationStaged.has(actorId) ? undefined : this.states.get(actorId);
  }

  snapshotStates(): readonly ZLinkActorRuntimeState[] {
    return [...this.states.values()];
  }

  activeActorCount(meshName: string): number {
    return [...this.states.values()].filter(
      (state) => state.meshName === meshName && state.actor !== undefined
    ).length;
  }

  async destroyActor(
    node: ZLinkBackendSpotNode,
    entryNodeRid: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const state = this.states.get(actor.context.actorId);
    if (state === undefined || state.actor === undefined || state.actor !== actor) {
      return;
    }
    await this.destroyState(state, entryNodeRid, signal, async (actorRef) => {
      await node.destroyActor(actorRef, 0);
    });
  }

  private async createOrGet(
    actorId: string,
    actorType: string,
    failIfExists: boolean,
    request: unknown,
    signal?: AbortSignal,
    claimLocation = true,
    timeoutMs: number = DEFAULT_REQUEST_TIMEOUT_MS,
    nativeActorRef?: ZLinkBackendActorRef
  ): Promise<ZLinkActorLocalCreateResult<ActorRef | undefined>> {
    const deadline = performance.now() + timeoutMs;
    const requestedMeshName = this.actorMeshNames.get(actorId);
    for (;;) {
      throwIfAborted(signal);
      if (requestedMeshName !== undefined && !this.actorMeshNames.has(actorId)) {
        this.actorMeshNames.set(actorId, requestedMeshName);
      }
      const existingState = this.states.get(actorId);
      if (existingState?.isDestroying === true) {
        if (failIfExists) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
            `Actor '${actorId}' already exists.`
          );
        }
        if (performance.now() >= deadline) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor '${actorId}' destroy did not finish before the creation deadline.`,
            true
          );
        }
        await waitForCreationRetry(signal);
        continue;
      }
      if (existingState?.hasActorOrCreation !== true) {
        this.options.admission?.requireActorCreate(
          actorId,
          this.options.actorMeshNameProvider?.(actorType)
        );
      }
      const state = existingState ?? this.getOrCreateState(actorId);
      if (nativeActorRef !== undefined) {
        state.setNativeActorRef(nativeActorRef);
      }
      const createRequest = this.createRequestMessage(request);
      if (request !== undefined && createRequest.nativeRequest !== undefined) {
        state.setCreateRequestPayload(createRequest.nativeRequest.data());
      }
      const operation = state.getOrStartCreation(actorType, failIfExists, async () => {
        const meshName = state.meshName ?? this.options.actorMeshNameProvider?.(actorType);
        const release =
          meshName === undefined
            ? undefined
            : await this.options.activationAdmission?.acquire(meshName, signal);
        try {
          return await this.creation.createActor(
            actorId,
            actorType,
            state,
            createRequest,
            claimLocation,
            signal
          );
        } finally {
          release?.();
        }
      });
      if (operation.created) {
        void operation.task.then(
          (result) => {
            if (result.status !== 'created') {
              this.discardFailedState(actorId, state, operation.task);
            }
          },
          () => this.discardFailedState(actorId, state, operation.task)
        );
      }

      try {
        const result = await operation.task;
        if (result.status === 'created') {
          const actorRef =
            state.nativeActorRef === undefined ? undefined : this.actorRefForState(state);
          return {
            status: operation.created ? 'created' : 'existing',
            actor: result.actor,
            actorRef,
            reply: operation.created ? result.reply : undefined
          };
        }
        if (operation.created) {
          return result.status === 'failed'
            ? { status: 'failed', error: result.error }
            : { status: 'rejected', reply: result.reply };
        }
      } catch (error) {
        if (operation.created) {
          if (error instanceof ZLinkFrameworkException) {
            throw error;
          }
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
            `Actor '${actorId}' creation failed: ${error instanceof Error ? error.message : String(error)}`,
            false,
            error
          );
        }
      } finally {
        createRequest.nativeRequest?.close();
      }

      if (performance.now() >= deadline) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Actor '${actorId}' creation did not reach a terminal state before its deadline.`,
          true
        );
      }
      await waitForCreationRetry(signal);
    }
  }

  private discardFailedState(
    actorId: string,
    state: ZLinkActorRuntimeState,
    task: Promise<ZLinkActorCreationAttemptResult>
  ): void {
    if (!state.clearFailedCreation(task)) return;
    if (this.states.get(actorId) === state) {
      this.states.delete(actorId);
      this.actorMeshNames.delete(actorId);
      this.options.metrics?.change(METRIC_NAMES.ActorCount, -1);
    }
  }

  private createRequestMessage(request: unknown): ZLinkActorCreateRequest {
    if (request === undefined) {
      const empty = RuntimeMessage.from(Buffer.alloc(0));
      return {
        nativeRequest: empty,
        callbackRequest: ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(empty.data()))
      };
    }
    const nativeRequest = encodeFrameworkPayloadMessage(request, this.options.messageSerializers);
    return {
      nativeRequest,
      callbackRequest: ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(nativeRequest.data()))
    };
  }

  private getOrCreateState(actorId: string): ZLinkActorRuntimeState {
    const existing = this.states.get(actorId);
    if (existing !== undefined) {
      return existing;
    }

    const state = new ZLinkActorRuntimeState(
      actorId,
      () => this.trackExecution(state),
      () => {
        if (state.actor !== undefined)
          this.options.locationLifecycle?.discardExecution(state.actor);
      },
      this.options.projectBoundSession
    );
    const remembered = this.actorMeshNames.get(actorId);
    if (remembered !== undefined) {
      state.rememberMeshName(remembered);
    }
    this.states.set(actorId, state);
    this.options.metrics?.change(METRIC_NAMES.ActorCount, 1);
    return state;
  }

  private actorRefForState(state: ZLinkActorRuntimeState): ActorRef {
    return toFrameworkActorRef(state.currentActorRef(), state.meshName ?? '');
  }
}

interface ZLinkActorCreateCallOptions {
  readonly meshName?: string;
  readonly request?: unknown;
  readonly timeoutMs: number;
}

type ZLinkActorCreateSubmit = (
  options: ZLinkActorCreateCallOptions,
  signal?: AbortSignal
) => Promise<ZLinkActorCreateResult>;

class ZLinkActorCreateCallRuntime implements ZLinkActorCreateCall {
  private meshNameValue: string | undefined;
  private requestValue: unknown;
  private requestConfigured = false;
  private timeoutMsValue: number = DEFAULT_REQUEST_TIMEOUT_MS;
  private timeoutConfigured = false;
  private submitted = false;
  private readonly turn: ZLinkSpotSerialTurn | undefined = captureZLinkSpotSerialTurn();

  constructor(private readonly execute: ZLinkActorCreateSubmit) {}

  inMesh(meshName: string): this {
    this.requireMutable();
    if (this.meshNameValue !== undefined) this.throwDuplicateOption('inMesh');
    requireActorMeshName(meshName);
    this.meshNameValue = meshName;
    return this;
  }

  request(request: unknown): this {
    this.requireMutable();
    if (this.requestConfigured) this.throwDuplicateOption('request');
    this.requestConfigured = true;
    this.requestValue = request;
    return this;
  }

  timeout(timeoutMs: number): this {
    this.requireMutable();
    if (this.timeoutConfigured) this.throwDuplicateOption('timeout');
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        'Actor create timeout must be a positive finite number.'
      );
    }
    this.timeoutConfigured = true;
    this.timeoutMsValue = timeoutMs;
    return this;
  }

  submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult> {
    return this.executeOnce(signal);
  }

  yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult> {
    const turn = requireZLinkYieldTurn(this.turn);
    const pending = this.executeOnce(signal);
    return turn.yieldFrameworkPromise(pending);
  }

  private executeOnce(signal?: AbortSignal): Promise<ZLinkActorCreateResult> {
    this.requireMutable();
    this.submitted = true;
    return this.execute(
      {
        ...(this.meshNameValue === undefined ? {} : { meshName: this.meshNameValue }),
        ...(this.requestConfigured ? { request: this.requestValue } : {}),
        timeoutMs: this.timeoutMsValue
      },
      signal
    );
  }

  private requireMutable(): void {
    if (!this.submitted) return;
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
      'Actor create call was already submitted.'
    );
  }

  private throwDuplicateOption(option: string): never {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.InvalidOperation,
      `Actor create option '${option}' was already configured.`
    );
  }
}

class ZLinkActorGetOrCreateCallRuntime
  extends ZLinkActorCreateCallRuntime
  implements ZLinkActorGetOrCreateCall {}

export type ZLinkActorLocalCreateResult<TActorRef extends ActorRef | undefined = ActorRef> =
  | {
      readonly status: 'existing' | 'created';
      readonly actor: ZLinkActor;
      readonly actorRef: TActorRef;
      readonly reply?: unknown;
    }
  | {
      readonly status: 'rejected';
      readonly reply?: unknown;
    }
  | {
      readonly status: 'failed';
      readonly error: unknown;
    };

function requireCreatedActor(
  result: ZLinkActorLocalCreateResult<ActorRef | undefined>,
  actorId: string
): ZLinkActor {
  if (result.status === 'existing' || result.status === 'created') return result.actor;
  if (result.status === 'failed') {
    throw actorApplicationFailure(actorId, result.error);
  }
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorCreateRejected,
    `Actor '${actorId}' create request was rejected.`
  );
}

function actorApplicationFailure(actorId: string, error: unknown): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
    `Actor '${actorId}' creation failed: ${error instanceof Error ? error.message : String(error)}`,
    false,
    error
  );
}

async function waitForCreationRetry(signal?: AbortSignal): Promise<void> {
  throwIfAborted(signal);
  await new Promise<void>((resolve, reject) => {
    const finish = (error?: unknown) => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', onAbort);
      if (error === undefined) resolve();
      else reject(error);
    };
    const timer = setTimeout(() => finish(), ACTOR_CREATION_RETRY_INTERVAL_MS);
    const onAbort = () => {
      finish(signal?.reason ?? new Error('Actor creation was aborted.'));
    };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

function requireActorMeshName(meshName: string): void {
  if (meshName.length === 0 || meshName !== meshName.trim()) {
    throw new ZLinkConfigurationException('Actor RouteMesh name must not be empty or padded.');
  }
}

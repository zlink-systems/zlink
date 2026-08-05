import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  SpotRef,
  ZLinkActor,
  ZLinkActorCreateCall,
  ZLinkActorCreateResult,
  ZLinkActorGetOrCreateCall,
  ZLinkActorManager,
} from '../../contracts';
import {
  ZLinkEncodedPayload,
  ZLinkFrameworkException
} from '../../contracts';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { ZLinkMessage } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import { throwIfAborted } from '../abort';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendSpotNode
} from '../backend/contracts';
import { closeMeshCompletion } from '../backend/mesh-completion-table';
import {
  captureZLinkSpotSerialTurn,
  requireZLinkYieldTurn,
  type ZLinkSpotSerialTurn
} from '../execution';
import { disposeLifecycleHandlers } from '../handlers/handler-instance-scope';

export {
  DefaultZLinkActorClient,
  forwardEncodedActorPacket,
  type ZLinkActorClientOptions
} from './actor-client';

import {
  encodeFrameworkPayloadMessage
} from '../messaging/payload-codec';
export {
  DefaultZLinkActorContext,
  ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME
} from './actor-context';
import {
  ZLinkActorCreationCoordinator,
  type ZLinkActorCreateRequest
} from './actor-creation';
export {
  ZLinkActorDispatchMailbox,
  ZLinkActorDispatchMailboxSet
} from './actor-mailbox';
export {
  DEFAULT_MESSAGE_FOLLOW_DURATION_MS,
  ZLinkActorHandoffCoordinator,
  decodeHandoffPacket,
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffDispatch,
  type ZLinkActorHandoffResult,
  type ZLinkActorHandoffTarget
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
  mergeRemoteBoundSessionTarget,
  preferredRemoteBoundSessionTarget,
  toFrameworkActorRef,
  toFrameworkRoutingId,
  type ZLinkActorCreationOperation,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from './actor-runtime-state';
export {
  ZLinkDeferredJoinAcceptedJournal,
  type ZLinkDeferredJoinAcceptedRoot,
  type ZLinkDeferredJoinDeliveryCursor,
  type ZLinkDeferredJoinRecoveryInput,
  type ZLinkDeferredJoinRecoveryManifest
} from './deferred-join-accepted-journal';
export {
  decodeActorAuthorityIdentity,
  encodeActorAuthorityIdentity,
  rewriteActorAuthorityRoute,
  rewriteActorAuthorityOwner,
  publishInitialActorAuthority,
  type ZLinkActorAuthorityIdentity
} from './actor-authority-publication';
import {
  ZLinkActorRuntimeState,
  toFrameworkActorRef,
  type ZLinkActorCreationAttemptResult
} from './actor-runtime-state';
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
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';
import { ZLinkTransferredActorRollbackCoordinator } from './transferred-actor-rollback';
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
  ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET
} from './bound-session-wire';
export {
  ZLinkBoundSessionAcceptedJournal,
  type ZLinkBoundSessionAcceptedJournalRoot
} from './bound-session-accepted-journal';
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

  constructor(private readonly options: ZLinkActorManagerOptions) {
    this.creation = new ZLinkActorCreationCoordinator(options);
    this.transferredActorRollback = new ZLinkTransferredActorRollbackCoordinator(this.states, options);
  }

  create(actorId: string, actorType: string): ZLinkActorCreateCall {
    return new ZLinkActorCreateCallRuntime(
      (options, signal) => this.submitCreate(actorId, actorType, true, options, signal)
    );
  }

  async find(actorId: string, signal?: AbortSignal): Promise<ActorRef | undefined> {
    throwIfAborted(signal);
    if (this.relocationStaged.has(actorId)) return undefined;
    const state = this.states.get(actorId);
    if (state?.actor !== undefined) {
      return this.actorRefForState(state);
    }
    return await this.options.actorRefResolver?.resolveActorRef(actorId, signal);
  }

  getOrCreate(actorId: string, actorType: string): ZLinkActorGetOrCreateCall {
    return new ZLinkActorGetOrCreateCallRuntime(
      (options, signal) => this.submitCreate(actorId, actorType, false, options, signal)
    );
  }

  async findSpot(actorId: string, signal?: AbortSignal): Promise<SpotRef | undefined> {
    throwIfAborted(signal);
    const state = this.states.get(actorId);
    if (state?.actor === undefined || state.spotId === undefined) return undefined;
    const spotId = state.spotId;
    const actorRef = this.actorRefForState(state);
    return {
      spotId: spotId,
      objectGeneration: state.locationGeneration ?? 0n,
      meshName: this.actorMeshNames.get(actorId) ?? '',
      nodeRid: actorRef.nodeRid
    };
  }

  async destroy(actor: ActorRef, signal?: AbortSignal): Promise<boolean> {
    throwIfAborted(signal);
    const state = this.states.get(actor.actorId);
    if (state?.actor === undefined) return false;
    const current = this.actorRefForState(state);
    if (current.objectGeneration !== actor.objectGeneration) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorGenerationStale,
        `Actor '${actor.actorId}' generation is stale.`
      );
    }
    if (state.isMoving) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorMoving,
        `Actor '${actor.actorId}' is relocating.`
      );
    }
    const actorInstance = state.actor;
    const node = this.options.nativeActorNodeProvider?.() ?? this.options.nativeActorNode;
    const completions = this.options.nativeActorCompletionTableProvider?.();
    const entryNodeRid = state.entryNodeRid ?? current.nodeRid;
    if (node === undefined || completions === undefined) return false;
    const destroyTask = state.getOrStartDestroy(entryNodeRid, async (nativeRef) => {
      if (nativeRef !== undefined) {
        const operationId = node.destroyActor(nativeRef, 0);
        const completion = await completions.wait(operationId, signal);
        try {
          if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
            throw createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
              `Actor '${actor.actorId}' destroy failed with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
            );
          }
        } finally {
          closeMeshCompletion(completion);
        }
        state.markNativeActorDestroyed(nativeRef);
      }
      if (state.actorType !== undefined && state.ownsLocation) {
        await this.options.locationLifecycle?.releaseActor(state.actorType, actor.actorId, current);
      }
      this.options.actorDestroyedCleanup?.(actor.actorId);
      await disposeLifecycleHandlers(actorInstance);
      state.clearAfterDestroy();
      if (this.states.get(actor.actorId) === state) {
        this.states.delete(actor.actorId);
        this.actorMeshNames.delete(actor.actorId);
      }
      this.options.metrics?.change('zlink.actor.count', -1);
    });
    try {
      await destroyTask;
    } catch (error) {
      state.clearFailedDestroy(destroyTask);
      throw error;
    }
    return true;
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
      await disposeLifecycleHandlers(state.actor);
    }
    state.prepareForRemoteReentry();
    const operation = state.getOrStartCreation(
      actorType,
      false,
      () => this.creation.materializeTransferredActor(
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
      return await this.options.placementCreate(
        actorId,
        actorType,
        failIfExists,
        options,
        signal
      );
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
    return result.reply === undefined
      ? { status: result.status, actor: result.actorRef }
      : { status: result.status, actor: result.actorRef, reply: result.reply };
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
    if (this.relocationStaged.has(actorId)) return undefined;
    return this.states.get(actorId)?.actor;
  }

  async prepareRelocationActor(
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
    try {
      nativeRef = restore.call(
        node,
        actorId,
        actorType,
        objectGeneration,
        authorityOwnerGeneration,
        String(spotId),
        spotGeneration,
        membershipEpoch
      );
      const actor = await this.getOrCreateWithNativeRef(
        actorId,
        actorType,
        nativeRef,
        undefined,
        signal
      );
      if (transfer !== undefined) {
        const registry = this.options.actorTransferRegistry;
        if (registry === undefined) {
          throw new ZLinkConfigurationException('Actor transfer registry is not configured.');
        }
        await registry.restore(
          transfer.adapterKey,
          actor,
          transfer.state,
          signal
        );
      }
      const state = this.states.get(actorId)!;
      state.setLocationGeneration(authorityOwnerGeneration);
      state.setJoinedSpot(spotId, undefined, membershipEpoch);
      return actor;
    } catch (error) {
      if (nativeRef !== undefined) node.discardRelocatedActor?.(nativeRef);
      this.relocationStaged.delete(actorId);
      const failedState = this.states.get(actorId);
      if (failedState?.actor !== undefined) {
        await disposeLifecycleHandlers(failedState.actor);
      }
      failedState?.clearAfterDestroy();
      this.states.delete(actorId);
      this.actorMeshNames.delete(actorId);
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
    const state = this.states.get(actorId);
    const node = this.options.nativeActorNodeProvider?.() ?? this.options.nativeActorNode;
    if (state?.nativeActorRef !== undefined) {
      node?.discardRelocatedActor?.(state.nativeActorRef);
    }
    if (state?.actor !== undefined) {
      await disposeLifecycleHandlers(state.actor);
    }
    state?.clearAfterDestroy();
    this.states.delete(actorId);
    this.actorMeshNames.delete(actorId);
    this.relocationStaged.delete(actorId);
  }

  async completeRelocationSource(actorId: string): Promise<void> {
    await this.abortRelocationActor(actorId);
  }

  async completeCoreRelocationSource(actorId: string): Promise<void> {
    const state = this.states.get(actorId);
    if (state?.actor !== undefined) {
      await disposeLifecycleHandlers(state.actor);
    }
    state?.clearAfterDestroy();
    this.states.delete(actorId);
    this.actorMeshNames.delete(actorId);
    this.relocationStaged.delete(actorId);
  }

  async getOrCreateActor(actorId: string, actorType: string, signal?: AbortSignal): Promise<ZLinkActor> {
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
  ): Promise<ZLinkActorLocalCreateResult> {
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
    return await this.createOrGet(
      actorId,
      actorType,
      false,
      createRequest,
      signal,
      false,
      30_000,
      nativeActorRef
    );
  }

  adoptCreatedAuthority(
    actorId: string,
    authorityOwnerGeneration: bigint,
    ownerLeaseGeneration: bigint
  ): void {
    const state = this.states.get(actorId);
    if (state === undefined) {
      throw new Error(`Actor '${actorId}' is not materialized.`);
    }
    state.setLocationGeneration(authorityOwnerGeneration);
    state.setOwnerLeaseGeneration(ownerLeaseGeneration);
    state.markLocationOwned();
  }

  getState(actorId: string): ZLinkActorRuntimeState | undefined {
    return this.states.get(actorId);
  }

  snapshotStates(): readonly ZLinkActorRuntimeState[] {
    return [...this.states.values()];
  }

  activeActorCount(meshName: string): number {
    return [...this.states.values()].filter((state) =>
      state.meshName === meshName && state.actor !== undefined
    ).length;
  }

  async destroyActor(
    node: ZLinkBackendSpotNode,
    entryNodeRid: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    const destroySignal = signal;
    throwIfAborted(destroySignal);
    const state = this.states.get(actor.context.actorId);
    if (state === undefined || state.actor === undefined || state.actor !== actor) {
      return;
    }
    if (state.isJoined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${actor.context.actorId}' must leave its current SPOT before destroy.`
      );
    }
    const destroyTask = state.getOrStartDestroy(entryNodeRid, async (actorRef) => {
      const destroyedActorRef = actorRef === undefined
        ? undefined
        : toFrameworkActorRef(actorRef, state.meshName ?? '');
      if (actorRef !== undefined) {
        await node.destroyActor(actorRef, 0, destroySignal);
        state.markNativeActorDestroyed(actorRef);
      }
      if (state.actorType !== undefined && state.ownsLocation) {
        await this.options.locationLifecycle?.releaseActor(
          state.actorType,
          actor.context.actorId,
          destroyedActorRef
        );
      }
      this.options.actorDestroyedCleanup?.(actor.context.actorId);
      await disposeLifecycleHandlers(actor);
      state.clearAfterDestroy();
      if (this.states.get(actor.context.actorId) === state) {
        this.states.delete(actor.context.actorId);
        this.actorMeshNames.delete(actor.context.actorId);
      }
      this.options.metrics?.change('zlink.actor.count', -1);
    });

    try {
      await destroyTask;
    } catch (error) {
      state.clearFailedDestroy(destroyTask);
      throw error;
    }
  }

  private async createOrGet(
    actorId: string,
    actorType: string,
    failIfExists: boolean,
    request: unknown,
    signal?: AbortSignal,
    claimLocation = true,
    timeoutMs = 30_000,
    nativeActorRef?: ZLinkBackendActorRef
  ): Promise<ZLinkActorLocalCreateResult> {
    const deadline = Date.now() + timeoutMs;
    const requestedMeshName = this.actorMeshNames.get(actorId);
    for (;;) {
      throwIfAborted(signal);
      if (requestedMeshName !== undefined && !this.actorMeshNames.has(actorId)) {
        this.actorMeshNames.set(actorId, requestedMeshName);
      }
      const existingState = this.states.get(actorId);
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
      const operation = state.getOrStartCreation(
        actorType,
        failIfExists,
        () => this.creation.createActor(actorId, actorType, state, createRequest, claimLocation, signal)
      );
      if (operation.created) {
        void operation.task.then(
          (result) => {
            if (result.status === 'rejected') {
              this.discardFailedState(actorId, state, operation.task);
            }
          },
          () => this.discardFailedState(actorId, state, operation.task)
        );
      }

      try {
        const result = await operation.task;
        if (result.status === 'created') {
          const actorRef = this.actorRefForState(state);
          return {
            status: operation.created ? 'created' : 'existing',
            actor: result.actor,
            actorRef,
            reply: operation.created ? result.reply : undefined
          };
        }
        if (operation.created) {
          return { status: 'rejected', reply: result.reply };
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

      if (Date.now() >= deadline) {
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
      this.options.metrics?.change('zlink.actor.count', -1);
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

    const state = new ZLinkActorRuntimeState(actorId);
    const remembered = this.actorMeshNames.get(actorId);
    if (remembered !== undefined) {
      state.rememberMeshName(remembered);
    }
    this.states.set(actorId, state);
    this.options.metrics?.change('zlink.actor.count', 1);
    return state;
  }

  private actorRefForState(state: ZLinkActorRuntimeState): ActorRef {
    const nativeActorRef = state.nativeActorRef;
    if (nativeActorRef !== undefined) {
      return toFrameworkActorRef(nativeActorRef, state.meshName ?? '');
    }
    return {
      actorId: state.actorId,
      objectGeneration: 1n,
      meshName: state.meshName ?? '',
      nodeRid: this.options.actorCreatedNodeRidProvider?.()
        ?? 'local'
    };
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
  private timeoutMsValue = 30_000;
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
    return turn.yieldPromise(pending);
  }

  private executeOnce(signal?: AbortSignal): Promise<ZLinkActorCreateResult> {
    this.requireMutable();
    this.submitted = true;
    return this.execute({
      ...(this.meshNameValue === undefined ? {} : { meshName: this.meshNameValue }),
      ...(this.requestConfigured ? { request: this.requestValue } : {}),
      timeoutMs: this.timeoutMsValue
    }, signal);
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
      ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
      `Actor create option '${option}' was already configured.`
    );
  }
}

class ZLinkActorGetOrCreateCallRuntime
  extends ZLinkActorCreateCallRuntime
  implements ZLinkActorGetOrCreateCall {}

export type ZLinkActorLocalCreateResult =
  | {
      readonly status: 'existing' | 'created';
      readonly actor: ZLinkActor;
      readonly actorRef: ActorRef;
      readonly reply?: unknown;
    }
  | {
      readonly status: 'rejected';
      readonly reply?: unknown;
    };

function requireCreatedActor(result: ZLinkActorLocalCreateResult, actorId: string): ZLinkActor {
  if (result.status !== 'rejected') return result.actor;
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorCreateRejected,
    `Actor '${actorId}' create request was rejected.`
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
    const timer = setTimeout(() => finish(), 5);
    const onAbort = () => {
      finish(signal?.reason ?? new Error('Actor creation was aborted.'));
    };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

function requireActorMeshName(meshName: string): void {
  if (meshName.length === 0 || meshName !== meshName.trim()) {
    throw new ZLinkConfigurationException(
      'Actor RouteMesh name must not be empty or padded.'
    );
  }
}

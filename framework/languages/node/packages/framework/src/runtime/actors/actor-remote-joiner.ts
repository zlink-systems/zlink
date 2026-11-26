import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type { ActorRef, RoutingId, ZLinkActor, ZLinkMessageSerializer } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMeshNode, ZLinkMeshCompletionTable } from '../backend';
import type { ZLinkLocationLifecycle } from '../locations';
import { throwIfAborted } from '../abort';
import type { ZLinkSpotRouteResolver } from '../spots/spot-routing-internal';
import type {
  ZLinkActorJoinCoordinator,
  ZLinkActorJoinRuntimeResult
} from './actor-runtime-contracts';
import type { ZLinkActorSourceTransfer } from './actor-source-transfer';
import { ZLinkActorRuntimeState } from './actor-runtime-state';
import { lookupNativeActorRef } from './actor-native-lookup';
import { ZLinkLocalNativeActorJoin } from './actor-local-native-join';
import { ZLinkPostCommitActorBinder } from './post-commit-actor-binder';
import type { ZLinkActorJoinRelocation } from './actor-join-relocation';

export interface ZLinkActorNativeJoinCoordinatorOptions extends Pick<
  import('./actor-local-native-join').ZLinkLocalNativeActorJoinOptions,
  'localSpotJoin' | 'localEntryJoin' | 'localSourceLeave' | 'reportSourceLeaveError'
> {
  readonly messageFlow?: () =>
    import('../diagnostics/message-flow').ZLinkMessageFlowTracer | undefined;
  readonly node: ZLinkBackendMeshNode | (() => ZLinkBackendMeshNode);
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly spotRouteResolver?: ZLinkSpotRouteResolver;
  readonly locationLifecycle?: ZLinkLocationLifecycle;
  readonly remoteActorBinder?: (
    actorRef: ActorRef,
    signal?: AbortSignal,
    force?: boolean
  ) => Promise<void>;
  readonly postCommitErrorReporter?: (error: unknown) => void;
  readonly sourceTransfer?: ZLinkActorSourceTransfer;
  readonly actorJoinRelocation?: ZLinkActorJoinRelocation;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly shutdownSignal?: AbortSignal;
  readonly actorTransferTimeoutMs?: number;
  readonly entrySpotTargetSelector?: (
    meshName: string | undefined,
    stableType: string | undefined,
    signal?: AbortSignal
  ) => Promise<ZLinkSpotRouteTarget | undefined>;
}

/** Selects local native join or remote two-phase transfer for each destination. */
export class ZLinkActorNativeJoinCoordinator implements ZLinkActorJoinCoordinator {
  private readonly localJoin: ZLinkLocalNativeActorJoin;

  messageFlow(): import('../diagnostics/message-flow').ZLinkMessageFlowTracer | undefined {
    return this.options.messageFlow?.();
  }

  constructor(private readonly options: ZLinkActorNativeJoinCoordinatorOptions) {
    this.localJoin = new ZLinkLocalNativeActorJoin({
      locationLifecycle: options.locationLifecycle,
      invalidateSpotRouteIfMatches: (used) =>
        options.spotRouteResolver?.invalidateIfMatches?.(used),
      localSpotJoin: options.localSpotJoin,
      localEntryJoin: options.localEntryJoin,
      localSourceLeave: options.localSourceLeave,
      reportSourceLeaveError: options.reportSourceLeaveError,
      postCommitBinder:
        options.remoteActorBinder === undefined
          ? undefined
          : new ZLinkPostCommitActorBinder({
              bind: (actorRef, force) => options.remoteActorBinder!(actorRef, undefined, force),
              reportError: options.postCommitErrorReporter,
              signal: options.shutdownSignal
            }),
      completionTableProvider: options.completionTableProvider,
      actorJoinRelocation: options.actorJoinRelocation,
      messageSerializers: options.messageSerializers
    });
  }

  beginDeferredJoin(actor: ZLinkActor, state: ZLinkActorRuntimeState): void {
    this.options.sourceTransfer?.beginDeferredActorHandoff?.(actor, state);
  }

  async abortDeferredJoin(actor: ZLinkActor, state: ZLinkActorRuntimeState): Promise<void> {
    await this.options.sourceTransfer?.cancelDeferredActorHandoff?.(actor, state);
  }

  async joinSpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    spotId: RoutingId,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId,
    deadlineAtMs?: number
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    throwIfAborted(signal);
    const node = this.node();
    const actorRef =
      state.nativeActorRef ??
      lookupNativeActorRef(node, actor.context.actorId) ??
      node.createActor(actor.context.actorId);
    state.setNativeActorRef(actorRef as never);
    let target: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined;
    target = await this.options.spotRouteResolver?.resolve(spotId, signal);
    installResolvedSpotRoute(node, target);
    const result = await this.localJoin.joinSpot(
      node,
      actor,
      state,
      actorRef,
      spotId,
      target,
      request,
      timeoutMs ?? this.options.actorTransferTimeoutMs,
      signal,
      completionOperationId,
      deadlineAtMs
    );
    return this.withDeferredJoinFinalizer(actor, state, target, result, completionOperationId);
  }

  async joinEntrySpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    _nodeRid: RoutingId | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId,
    deadlineAtMs?: number
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    throwIfAborted(signal);
    const node = this.node();
    const actorRef =
      state.nativeActorRef ??
      lookupNativeActorRef(node, actor.context.actorId) ??
      node.createActor(actor.context.actorId);
    state.setNativeActorRef(actorRef as never);
    const meshName = state.meshName ?? actor.context.meshName;
    const target = await this.options.entrySpotTargetSelector?.(meshName, state.actorType, signal);
    if (target === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
        'No eligible Entry Spot target is available.',
        true
      );
    }
    const result = await this.localJoin.joinEntrySpot(
      node,
      actor,
      state,
      actorRef,
      target.targetNodeRid,
      target,
      request,
      timeoutMs ?? this.options.actorTransferTimeoutMs,
      signal,
      completionOperationId,
      deadlineAtMs
    );
    return this.withDeferredJoinFinalizer(actor, state, target, result, completionOperationId);
  }

  private withDeferredJoinFinalizer(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    target: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined,
    result: ZLinkActorJoinRuntimeResult<Message>,
    operationId: import('../../contracts').ZLinkActorJoinOperationId | undefined
  ): ZLinkActorJoinRuntimeResult<Message> {
    if (
      result.sourceCleanup !== undefined ||
      operationId === undefined ||
      this.options.sourceTransfer === undefined
    )
      return result;
    return {
      ...result,
      finalizeDeferredJoin: async () => {
        if (result.accepted && result.actor !== undefined && target !== undefined) {
          await this.options.sourceTransfer!.completeDeferredActorHandoff?.(
            actor,
            target,
            result.actor
          );
        } else {
          await this.options.sourceTransfer!.cancelDeferredActorHandoff?.(actor, state);
        }
      }
    };
  }

  private node(): ZLinkBackendMeshNode {
    return typeof this.options.node === 'function' ? this.options.node() : this.options.node;
  }
}

function installResolvedSpotRoute(
  node: ZLinkBackendMeshNode,
  target: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined
): void {
  if (
    target?.targetSpotGeneration === undefined ||
    target.targetNodeGeneration === undefined ||
    target.authorityOwnerGeneration === undefined ||
    target.ownerLeaseGeneration === undefined ||
    target.authorityStoreVersion === undefined
  ) {
    return;
  }
  node.rememberSpotRoute?.({
    spot: {
      spotId: String(target.spotId),
      generation: target.targetSpotGeneration
    },
    targetNodeRid: String(target.targetNodeRid),
    targetNodeGeneration: target.targetNodeGeneration,
    authorityOwnerGeneration: target.authorityOwnerGeneration,
    ownerLeaseGeneration: target.ownerLeaseGeneration,
    storeVersion: target.authorityStoreVersion
  });
}

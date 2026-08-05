import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkMessageSerializer
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMeshNode, ZLinkMeshCompletionTable } from '../backend';
import type { ZLinkLocationLifecycle } from '../locations';
import { throwIfAborted } from '../abort';
import type {
  ZLinkSpotRouteResolver
} from '../spots/spot-routing-internal';
import type { ZLinkActorJoinCoordinator, ZLinkActorJoinRuntimeResult } from './actor-runtime-contracts';
import type { ZLinkActorRoutedJoinTransport } from './actor-routed-join-transport';
import type { ZLinkStoreLocationResolvers } from '../locations';
import type { ZLinkActorSourceTransfer } from './actor-source-transfer';
import { ZLinkActorRuntimeState, toFrameworkRoutingId } from './actor-runtime-state';
import { lookupNativeActorRef } from './actor-native-lookup';
import { ZLinkPostCommitActorLocation } from './post-commit-actor-location';
import { ZLinkLocalNativeActorJoin } from './actor-local-native-join';
import { ZLinkPostCommitActorBinder } from './post-commit-actor-binder';
import { routingIdsEqual } from '../routing-id';

export interface ZLinkActorNativeJoinCoordinatorOptions {
  readonly node: ZLinkBackendMeshNode | (() => ZLinkBackendMeshNode);
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly spotRouteResolver?: ZLinkSpotRouteResolver;
  readonly routedTransport?: ZLinkActorRoutedJoinTransport;
  readonly locationLifecycle?: ZLinkLocationLifecycle;
  readonly remoteActorBinder?: (actorRef: ActorRef, signal?: AbortSignal, force?: boolean) => Promise<void>;
  readonly postCommitErrorReporter?: (error: unknown) => void;
  readonly sourceTransfer?: ZLinkActorSourceTransfer;
  readonly actorLocationResolver?: () => ZLinkStoreLocationResolvers | undefined;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly shutdownSignal?: AbortSignal;
  readonly actorTransferTimeoutMs?: number;
  readonly entrySpotIdProvider?: (meshName: string | undefined) => string | undefined;
}

/** Selects local native join or remote two-phase transfer for each destination. */
export class ZLinkActorNativeJoinCoordinator implements ZLinkActorJoinCoordinator {
  private readonly localJoin: ZLinkLocalNativeActorJoin;

  constructor(private readonly options: ZLinkActorNativeJoinCoordinatorOptions) {
    const postCommitLocation = options.locationLifecycle === undefined
      ? undefined
      : new ZLinkPostCommitActorLocation({
          lifecycle: options.locationLifecycle,
          reportError: options.postCommitErrorReporter,
          signal: options.shutdownSignal
        });
    this.localJoin = new ZLinkLocalNativeActorJoin({
      postCommitLocation,
      postCommitBinder: options.remoteActorBinder === undefined
        ? undefined
        : new ZLinkPostCommitActorBinder({
            bind: (actorRef, force) => options.remoteActorBinder!(actorRef, undefined, force),
            reportError: options.postCommitErrorReporter,
            signal: options.shutdownSignal
          }),
      completionTableProvider: options.completionTableProvider,
      sourceTransfer: options.sourceTransfer,
      messageSerializers: options.messageSerializers,
      postCommitErrorReporter: options.postCommitErrorReporter,
      entrySpotIdProvider: options.entrySpotIdProvider,
      remoteActivationWaiter: async (actorId, targetNodeRid, timeoutMs, signal) => {
        const deadline = Date.now() + Math.min(timeoutMs ?? 10_000, 10_000);
        for (;;) {
          throwIfAborted(signal);
          const resolver = options.actorLocationResolver?.();
          if (resolver === undefined) {
            return undefined;
          }
          const actorRef = await resolver.resolveActorRef(actorId, signal);
          if (actorRef !== undefined && routingIdsEqual(actorRef.nodeRid, targetNodeRid)) {
            return actorRef;
          }
          if (Date.now() >= deadline) {
            throw createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
              `Actor '${actorId}' target activation was not published before the join deadline.`
            );
          }
          await new Promise<void>((resolve) => setTimeout(resolve, 10));
        }
      }
    });
  }

  beginDeferredJoin(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: import('../../contracts').ZLinkActorJoinOperationId
  ): void {
    this.options.sourceTransfer?.beginDeferredActorHandoff?.(
      actor,
      state,
      deferredOperationKey(operationId)
    );
  }

  async abortDeferredJoin(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<void> {
    await this.options.sourceTransfer?.cancelDeferredActorHandoff?.(
      actor,
      state,
      deferredOperationKey(operationId)
    );
  }

  async joinSpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    spotId: RoutingId,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    throwIfAborted(signal);
    const node = this.node();
    const actorRef = state.nativeActorRef ?? lookupNativeActorRef(node, actor.context.actorId) ?? node.createActor(actor.context.actorId);
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
      completionOperationId
    );
    return this.withDeferredJoinFinalizer(
      actor,
      state,
      target,
      result,
      completionOperationId
    );
  }

  async joinEntrySpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    nodeRid: RoutingId | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    throwIfAborted(signal);
    const node = this.node();
    const actorRef = state.nativeActorRef ?? lookupNativeActorRef(node, actor.context.actorId) ?? node.createActor(actor.context.actorId);
    state.setNativeActorRef(actorRef as never);
    const meshName = state.meshName ?? actor.context.meshName;
    const entrySpotId = this.options.entrySpotIdProvider?.(meshName);
    let resolvedTarget: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined;
    resolvedTarget = entrySpotId === undefined
      ? undefined
      : await this.options.spotRouteResolver?.resolve(entrySpotId, signal);
    // The caller may carry the Entry node from the previous membership.
    // Entry placement can change while the Actor is in a User Spot, so the
    // current authority route is the source of truth whenever it is available.
    const selectedNodeRid = resolvedTarget?.targetNodeRid
      ?? nodeRid
      ?? state.entryNodeRid
      ?? toFrameworkRoutingId(node.status().routingId);
    const target = resolvedTarget;
    const result = await this.localJoin.joinEntrySpot(
      node,
      actor,
      state,
      actorRef,
      selectedNodeRid,
      target,
      request,
      timeoutMs ?? this.options.actorTransferTimeoutMs,
      signal,
      completionOperationId
    );
    return this.withDeferredJoinFinalizer(
      actor,
      state,
      target,
      result,
      completionOperationId
    );
  }

  private withDeferredJoinFinalizer(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    target: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined,
    result: ZLinkActorJoinRuntimeResult<Message>,
    operationId: import('../../contracts').ZLinkActorJoinOperationId | undefined
  ): ZLinkActorJoinRuntimeResult<Message> {
    if (operationId === undefined || this.options.sourceTransfer === undefined) return result;
    const key = deferredOperationKey(operationId);
    return {
      ...result,
      finalizeDeferredJoin: async () => {
        if (result.accepted && result.actor !== undefined && target !== undefined) {
          await this.options.sourceTransfer!.completeDeferredActorHandoff?.(
            actor,
            target,
            result.actor,
            key
          );
        } else {
          await this.options.sourceTransfer!.cancelDeferredActorHandoff?.(actor, state, key);
        }
      }
    };
  }

  private node(): ZLinkBackendMeshNode {
    return typeof this.options.node === 'function'
      ? this.options.node()
      : this.options.node;
  }
}

function deferredOperationKey(
  operationId: import('../../contracts').ZLinkActorJoinOperationId
): string {
  return `${operationId.high.toString(16)}:${operationId.low.toString(16)}`;
}

function installResolvedSpotRoute(
  node: ZLinkBackendMeshNode,
  target: Awaited<ReturnType<ZLinkSpotRouteResolver['resolve']>> | undefined
): void {
  if (
    target?.targetSpotGeneration === undefined
    || target.targetNodeGeneration === undefined
    || target.authorityOwnerGeneration === undefined
    || target.ownerLeaseGeneration === undefined
    || target.authorityStoreVersion === undefined
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

import type {
  ActorRef,
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkActorCreateResponse,
  ZLinkChannelClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkFanoutClient,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotPublisherClient
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import { ZLinkSpotCloseReason } from '../../contracts';
import type {
  ZLinkEntrySpotActorRequestHandlerRegistration,
  ZLinkEntrySpotActorSendHandlerRegistration,
  ZLinkEntrySpotPacketHandlerRegistration,
  ZLinkEntrySpotSubscriptionHandlerRegistration,
  ZLinkEntrySpotTimerHandlerRegistration
} from '../../contracts/Configuration/RegistrationTypes';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import { throwIfAborted } from '../abort';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import {
  ZLinkActorDispatchMailboxSet,
  ZLinkSpotActorHandlerRegistryRuntime
} from '../actors';
import type {
  ZLinkBackendActorRecvInfo,
  ZLinkBackendReceived,
  ZLinkBackendSpot,
  ZLinkBackendSpotNode,
  ZLinkBackendTopicMessage
} from '../backend/contracts';
import type { ZLinkDispatchErrorReporter } from '../channels';
import { ZLinkConfigurationException } from '../configuration';
import { ZLinkWorkerRuntime } from '../workers';
import { disposeLifecycleHandlers } from '../handlers/handler-instance-scope';
import { ZLinkSpotActorPacketDispatch } from './spot-actor-packet-dispatch';
import {
  ZLinkSpotActorJoinDispatch,
  type ZLinkDetachedTaskRunner
} from './spot-actor-join-dispatch';
import {
  applyEntrySpotHandlerRegistrations,
  DefaultZLinkSpotHandlerRegistry
} from './spot-handler-registry';
import {
  DefaultZLinkSpotOutbound,
  type ZLinkSpotRoutedTransport
} from './spot-outbound';
import { createProviderInstance } from './spot-provider';
import { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import { invokeSpotClosing } from './spot-closing';
import {
  addEntrySpotTimerRegistrations,
  ZLinkSpotTimerRegistry
} from './spot-timer';
import { createEntrySpotContext } from './spot-context';
import { ZLinkRoutedSpotPacketDispatch } from './spot-routed-spot-packet-dispatch';
import type { RequestResult } from '../backend/runtime-values';
import {
  replayActorHandoffBacklog,
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffResult
} from '../actors/actor-handoff';
import type {
  ZLinkEntryActorRuntime,
  ZLinkSpotActorTransferRuntime,
  ZLinkSpotActorHandoffRuntime,
  ZLinkSpotBoundSessionRuntime
} from './spot-runtime-ports';
import { ZLinkSpotLifecycleMetrics } from './spot-lifecycle-metrics';

interface ZLinkEntrySpotActivationOptions {
  readonly entrySpotType: Type<ZLinkEntrySpot>;
  readonly timerHandlers?: readonly ZLinkEntrySpotTimerHandlerRegistration[];
  readonly packetHandlers?: readonly ZLinkEntrySpotPacketHandlerRegistration[];
  readonly subscriptionHandlers?: readonly ZLinkEntrySpotSubscriptionHandlerRegistration[];
  readonly actorSendHandlers?: readonly ZLinkEntrySpotActorSendHandlerRegistration[];
  readonly actorRequestHandlers?: readonly ZLinkEntrySpotActorRequestHandlerRegistration[];
  readonly nativeSpot: ZLinkBackendSpot;
  readonly nativeNode: ZLinkBackendSpotNode;
  readonly createReceived: () => ZLinkBackendReceived;
  readonly createTopicMessage: () => ZLinkBackendTopicMessage;
  readonly nodeRid: RoutingId;
  readonly spotNodeName: string;
  readonly channelClient?: ZLinkChannelClient;
  readonly fanoutClient?: ZLinkFanoutClient;
  readonly spotPublisherClient?: ZLinkSpotPublisherClient;
  readonly routedTransport?: ZLinkSpotRoutedTransport;
  readonly spotRouterChannelIdForMesh?: (meshName: string) => string;
  readonly channelMeshNameForChannel?: (channelName: string) => string | undefined;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly workerRuntime?: ZLinkWorkerRuntime;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly entryActorRuntime?: ZLinkEntryActorRuntime;
  readonly actorTransferRuntime?: ZLinkSpotActorTransferRuntime;
  readonly boundSessionRuntime?: ZLinkSpotBoundSessionRuntime;
  readonly actorHandoffRuntime?: ZLinkSpotActorHandoffRuntime;
  readonly detachedTaskRunner?: ZLinkDetachedTaskRunner;
}

export class ZLinkEntrySpotActivation {
  private readonly serial: ZLinkSpotSerialExecutor;
  private readonly actorPacketMailboxes: ZLinkActorDispatchMailboxSet;
  private readonly timers: ZLinkSpotTimerRegistry;
  private readonly actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
  private readonly handlers = new DefaultZLinkSpotHandlerRegistry(this.actorHandlers);
  private readonly outbound: DefaultZLinkSpotOutbound;
  private readonly workerRuntime: ZLinkWorkerRuntime;
  private readonly lifecycleMetrics: ZLinkSpotLifecycleMetrics;
  private readonly packetDispatch: ZLinkRoutedSpotPacketDispatch;
  private initialized = false;
  private disposed = false;
  private actorDispatch?: ZLinkSpotActorJoinDispatch;

  entrySpot: ZLinkEntrySpot;
  readonly context: ZLinkEntrySpotContext;

  constructor(private readonly options: ZLinkEntrySpotActivationOptions) {
    this.serial = new ZLinkSpotSerialExecutor(false, options.nativeSpot.routingId);
    this.actorPacketMailboxes = new ZLinkActorDispatchMailboxSet();
    // Entry Spot lifecycle, timers and detached continuations use this serial
    // executor. Actor packets use the target actor mailbox.
    this.outbound = new DefaultZLinkSpotOutbound(
      this.serial,
      options.channelClient,
      options.fanoutClient,
      options.spotPublisherClient,
      options.routedTransport,
      options.spotRouterChannelIdForMesh ?? ((meshName) => meshName),
      undefined,
      options.spotNodeName,
      options.channelMeshNameForChannel
    );
    this.timers = new ZLinkSpotTimerRegistry(
      options.metrics,
      () => options.dispatchErrors?.flow.flowCreationEnabled() ?? true
    );
    this.lifecycleMetrics = new ZLinkSpotLifecycleMetrics(options.metrics);
    this.workerRuntime = options.workerRuntime ?? new ZLinkWorkerRuntime();
    this.context = createEntrySpotContext({
      spotId: String(options.nativeSpot.routingId),
      objectGeneration: toContextGeneration(entrySpotGeneration(options.nativeSpot)),
      nodeRid: options.nodeRid,
      handlers: this.handlers,
      outbound: this.outbound,
      timers: this.timers,
      serial: this.serial,
      getEntrySpot: () => this.entrySpot,
      spotNodeName: options.spotNodeName,
      providerResolver: options.providerResolver,
      runtimeEventPublisher: options.runtimeEventPublisher,
      workerRuntime: this.workerRuntime,
      destroyActor: options.entryActorRuntime === undefined
        ? undefined
        : (nodeRid, actor, signal) => options.entryActorRuntime!.destroyActor(
          options.nativeNode,
          nodeRid,
          actor,
          signal
        )
    });
    this.entrySpot = undefined as unknown as ZLinkEntrySpot;
    applyEntrySpotHandlerRegistrations(this.handlers, options.entrySpotType, {
      actorSendHandlers: options.actorSendHandlers,
      actorRequestHandlers: options.actorRequestHandlers,
      packetHandlers: options.packetHandlers,
      subscriptionHandlers: options.subscriptionHandlers
    });
    this.packetDispatch = new ZLinkRoutedSpotPacketDispatch({
      resolveActivation: (spotId) => spotId === this.spotId
        ? {
            spotId: this.spotId,
            spot: this.entrySpot as unknown as ZLinkSpot,
            serial: this.serial,
            handlers: this.handlers
          }
        : undefined,
      providerResolver: options.providerResolver,
      dispatchErrors: options.dispatchErrors
    });
  }

  /**
   * The Entry Spot serial dispatch line for Entry Spot-owned callbacks such as
   * lifecycle, timers, request continuations and worker completions. Actor
   * packets use the target actor mailbox instead.
   */
  get serialExecutor(): ZLinkSpotSerialExecutor {
    return this.serial;
  }

  get nodeRid(): RoutingId {
    return this.options.nodeRid;
  }

  get spotId(): RoutingId {
    return this.options.nativeSpot.routingId;
  }

  dispatchSubscriptionRecord(
    topic: string,
    parts: readonly Message[],
    sourceRid: RoutingId | null
  ): Promise<void> {
    if (this.actorDispatch === undefined) {
      throw new ZLinkConfigurationException(
        `Entry Spot '${this.options.spotNodeName}' subscription runtime is not configured.`
      );
    }
    return this.actorDispatch.dispatchSubscriptionRecord(topic, parts, sourceRid);
  }

  dispatchPacket(
    packetName: string | undefined,
    payload: unknown,
    context: { readonly channelName: string; readonly contentType?: string },
    returnResponse: boolean
  ): Promise<unknown> {
    return returnResponse
      ? this.packetDispatch.request(this.spotId, packetName, payload, context)
      : this.packetDispatch.send(this.spotId, packetName, payload, context);
  }

  dispatchPacketEncoded(
    packetName: string | undefined,
    decodePayload: () => unknown,
    context: { readonly channelName: string; readonly contentType?: string },
    returnResponse: boolean
  ): Promise<unknown> {
    return returnResponse
      ? this.packetDispatch.requestEncoded(this.spotId, packetName, decodePayload, context)
      : this.packetDispatch.sendEncoded(this.spotId, packetName, decodePayload, context);
  }

  async create(): Promise<void> {
    const entrySpot = await createProviderInstance(this.options.entrySpotType, this.options.providerResolver, this.context);
    this.entrySpot = entrySpot;
    Object.defineProperty(this.entrySpot, 'context', {
      configurable: true,
      enumerable: false,
      value: this.context
    });
    await addEntrySpotTimerRegistrations(
      this.timers,
      this.options.entrySpotType,
      this.entrySpot,
      this.serial,
      { timerHandlers: this.options.timerHandlers },
      {
          providerResolver: this.options.providerResolver,
          spotNodeName: this.options.spotNodeName,
          spotId: this.context.spotId,
        runtimeEventPublisher: this.options.runtimeEventPublisher
      }
    );
  }

  async configure(): Promise<void> {
    await this.entrySpot.configure?.();
  }

  async initialize(): Promise<void> {
    await this.serial.execute(() => this.entrySpot.onInitialize?.());
    this.attachActorJoinDispatch();
    this.initialized = true;
    this.lifecycleMetrics.opened('entry');
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    const errors: unknown[] = [];
    const cleanup = async (operation: () => Promise<void> | void) => {
      try {
        await operation();
      } catch (error) {
        errors.push(error);
      }
    };
    if (this.initialized) {
      await cleanup(() => this.serial.execute(() => invokeSpotClosing(
        this.entrySpot.onClosing?.bind(this.entrySpot),
        ZLinkSpotCloseReason.HostShutdown
      )));
    }
    await cleanup(() => this.actorDispatch?.dispose());
    await cleanup(() => this.timers.dispose());
    await cleanup(() => disposeLifecycleHandlers(this.entrySpot));
    await cleanup(() => this.options.nativeSpot.dispose());
    if (this.initialized) {
      this.lifecycleMetrics.closed('entry');
    }
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Entry Spot cleanup failed.');
  }

  notifyCreateActor(
    actor: ZLinkActor,
    createRequest: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<ZLinkActorCreateResponse> {
    throwIfAborted(signal);
    return this.serial.execute(async () =>
      await this.entrySpot.onCreateActor?.(actor, createRequest)
        ?? { accepted: true });
  }

  notifyJoinActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    return this.serial.execute(() => notifyEntryActorJoined(this.entrySpot, actor));
  }

  /**
   * Completes the stateful MeshNode return-to-Entry-Spot path after Core has
   * accepted the membership reply. The native receive path performs the same
   * actor-manager transaction in its admission callback.
   */
  async commitServiceActorJoin(
    actor: ZLinkActor,
    handoffBacklog: readonly ZLinkActorHandoffPacket[] = [],
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const notifyJoined = () => this.notifyJoinActor(actor, signal);
    if (this.options.entryActorRuntime === undefined) {
      await notifyJoined();
    } else {
      await this.options.entryActorRuntime.commitActorTransaction(actor, notifyJoined);
    }
    if (handoffBacklog.length > 0) {
      await this.replayActorBacklog(actor, handoffBacklog);
    }
  }

  /**
   * Entry Spot membership does not have an application admission callback.
   * The shared core round-trip therefore accepts a valid returning Actor and
   * runs only the post-commit membership notification.
   */
  private attachActorJoinDispatch(): void {
    const dispatch = new ZLinkSpotActorJoinDispatch({
      nativeSpot: this.options.nativeSpot,
      createReceived: this.options.createReceived,
      createTopicMessage: this.options.createTopicMessage,
      serial: this.serial,
      actors: {
        resolveActor: (actorId) => this.options.entryActorRuntime?.resolveActor(actorId),
        getTarget: () => this.entrySpot,
        defaultAccept: true,
        transfer: this.options.actorTransferRuntime === undefined ? { kind: 'disabled' } : {
          kind: 'enabled',
          runtime: this.options.actorTransferRuntime
        },
        commitNativeActor: (actor) => this.commitEntryActorTransaction(actor),
        commitTransferredActor: async (actor, backlog) => {
          await this.commitEntryActorTransaction(actor);
          return await this.replayActorBacklog(actor, backlog);
        }
      },
      packets: {
        handle: (actorId, parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
          this.dispatchActorPacket(actorId, parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef),
        bindRemoteSession: (actor, sourceNodeRid, sourceSessionRid, declaredTarget) => {
          if (routingIdsEqual(sourceNodeRid, this.options.nativeNode.routingId)) {
            return;
          }
          const target = declaredTarget
            ?? this.options.boundSessionRuntime?.resolveRemoteBoundSessionTarget(sourceNodeRid, sourceSessionRid);
          if (target !== undefined) {
            this.options.boundSessionRuntime?.rememberRemoteBoundSessionTarget(actor.actorId, {
              ...target,
              sessionNodeRid: sourceNodeRid,
              sessionRid: sourceSessionRid
            });
          }
          this.options.nativeNode.bindRemoteActorSession(actor, sourceNodeRid, sourceSessionRid);
        },
        replyNoBind: (info, parts, result) => this.replyActorNoBind(info, parts, result)
      },
      boundSessionRuntime: this.options.boundSessionRuntime,
      messageSerializers: this.options.messageSerializers,
      providerResolver: this.options.providerResolver,
      dispatchErrors: this.options.dispatchErrors,
      detachedTaskRunner: this.options.detachedTaskRunner
    });
    dispatch.configureSubscriptions(this.handlers.snapshot());
    dispatch.attach();
    this.actorDispatch = dispatch;
  }

  private async commitEntryActorTransaction(actor: ZLinkActor): Promise<void> {
    const notifyJoined = () => this.serial.execute(() =>
      notifyEntryActorJoined(this.entrySpot, actor));
    const runtime = this.options.entryActorRuntime;
    if (runtime === undefined) {
      await notifyJoined();
      return;
    }
    await runtime.commitActorTransaction(actor, notifyJoined);
  }

  notifyLeaveActor(
    actor: ZLinkActor,
    signal?: AbortSignal,
    _actorRef?: ActorRef,
    _membershipEpoch?: bigint
  ): Promise<void> {
    throwIfAborted(signal);
    return this.serial.execute(() =>
      this.entrySpot.onLeaveActor(actor));
  }

  notifyDisconnectActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    return this.serial.execute(() => this.entrySpot.onDisconnectActor?.(actor));
  }

  async dispatchActorPacket(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    const handoff = this.options.actorHandoffRuntime?.capture(
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      undefined,
      messageFollowOrigin,
      (replayedParts, replayReturnResponse, replayRemoteBoundSessionTarget, replayFallbackActorRef) =>
        this.actorPacketMailboxes.submit(actorId, () =>
          this.dispatchActorPacketInsideMailbox(
            actorId,
            replayedParts,
            replayReturnResponse,
            replayRemoteBoundSessionTarget,
            replayFallbackActorRef
          ))
    );
    if (handoff !== undefined) return await handoff;
    return this.actorPacketMailboxes.submit(actorId, () =>
      this.dispatchActorPacketInsideMailbox(
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal
      ));
  }

  private async replayActorBacklog(
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ): Promise<readonly ZLinkActorHandoffResult[]> {
    return await replayActorHandoffBacklog(
      backlog,
      (parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
        this.actorPacketMailboxes.submit(actor.context.actorId, () =>
          this.dispatchActorPacketInsideMailbox(
            actor.context.actorId,
            parts,
            returnResponse,
            remoteBoundSessionTarget,
            fallbackActorRef
          )),
      (index) => this.options.runtimeEventPublisher?.publish({
          sourceName: 'zlink.framework.actor-handoff',
          timestamp: new Date(),
          marker: 'backlog_enqueued',
          actorId: actor.context.actorId,
          index
        })
    );
  }

  private async dispatchActorPacketInsideMailbox(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void
  ): Promise<unknown> {
    return new ZLinkSpotActorPacketDispatch({
      spot: this.entrySpot as unknown as ZLinkSpot,
      spotId: () => String(this.options.nativeSpot.routingId),
      registry: this.actorHandlers,
      resolveActor: (targetActorId) => this.options.entryActorRuntime?.resolveActor(targetActorId),
      routeBeforeLocal: (
        targetActorId,
        targetParts,
        targetReturnResponse,
        targetRemoteBoundSessionTarget,
        targetFallbackActorRef
      ) =>
        this.options.entryActorRuntime?.routePacket(
          targetActorId,
          targetParts,
          targetReturnResponse,
          targetRemoteBoundSessionTarget,
          targetFallbackActorRef
        ),
      onRemoteBoundSessionTarget: (targetActorId, target) =>
        this.options.boundSessionRuntime?.rememberRemoteBoundSessionTarget(targetActorId, target),
      onDisconnectActor: (actor) => this.notifyDisconnectActor(actor),
      actorResponseSender: this.options.boundSessionRuntime?.sendActorResponse.bind(this.options.boundSessionRuntime),
      actorErrorSender: this.options.boundSessionRuntime?.sendActorError.bind(this.options.boundSessionRuntime),
      providerResolver: this.options.providerResolver,
      messageSerializers: this.options.messageSerializers,
      dispatchErrors: this.options.dispatchErrors
    }).dispatch(
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal
    );
  }

  private replyActorNoBind(
    info: ZLinkBackendActorRecvInfo,
    parts: readonly Message[],
    result: RequestResult
  ): void {
    this.options.nativeNode.replyActorNoBind(info, parts, result);
  }
}

function notifyEntryActorJoined(entrySpot: ZLinkEntrySpot, actor: ZLinkActor): Promise<void> {
  const callback = (entrySpot as unknown as {
    readonly onJoinedActor?: (joinedActor: ZLinkActor) => Promise<void>;
  }).onJoinedActor;
  return callback === undefined
    ? Promise.resolve()
    : callback.call(entrySpot, actor);
}

function toContextGeneration(generation: bigint): number {
  if (generation < 0n || generation > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new ZLinkConfigurationException(
      `Entry Spot object generation '${generation}' cannot be represented by the Node.js public context.`
    );
  }
  return Number(generation);
}

function entrySpotGeneration(spot: ZLinkBackendSpot): bigint {
  return spot.lifecycleGeneration
    ?? (spot as Partial<ZLinkBackendSpot>).status?.().lifecycleGeneration
    ?? 0n;
}

import type {
  ActorRef,
  Type,
  ZLinkActor,
  ZLinkBoundSession,
  ZLinkSessionActor,
  ZLinkSessionFactory,
  ZLinkSession,
  ZLinkMessageSerializer,
  ZLinkStreamCompressionCodec,
  ZLinkStreamCompressionOptions
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkSubmitResult } from '../messaging/submission-result';
import {
  ZLinkMessage
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { ZLinkDispatchErrorReporter } from '../channels';
import type {
  ZLinkBackendAdapterFactory,
  ZLinkBackendActorSessionNode,
  ZLinkBackendContext,
  ZLinkBackendMeshNode,
  ZLinkBackendSocketMonitor,
  ZLinkBackendStreamSocket
} from '../backend/contracts';
import type { ZLinkMeshCompletionTable } from '../backend/mesh-completion-table';
import type { ZLinkApplicationWorkClaim } from '../admission';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';
import type { ZLinkMeshSubmitterRegistry } from '../messaging';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import {
  messageToBytes,
  ZLinkStreamCodec,
  type ZLinkStreamFrameHeader,
  ZLinkStreamMessageKind,
  type ZLinkStreamReplyMessageKind
} from './protocol';
import {
  ZLinkActorSessionBindingRegistry
} from './actor-session-binding-registry';
import { ZLinkActorSessionLifecycleCoordinator } from './actor-session-lifecycle-coordinator';
import {
  decompressStreamPayload,
  resolveStreamCompressionCodec,
  ZLinkStreamFrameMessageFactory
} from './stream-frame-factory';
import { simpleMessage } from './stream-message-utils';
import {
  DefaultZLinkBoundSession,
  DefaultZLinkSessionActor,
  DefaultZLinkSessionContext
} from './session-context';
import {
  DefaultZLinkBoundSessionResponseTarget,
  type ZLinkBoundSessionResponseTarget
} from './bound-session-response-target';
import {
  ZLinkBoundSessionService,
  type ZLinkBoundSessionTransport
} from './bound-session-service';
import {
  ZLinkSessionActorCoordinator
} from './session-actor-coordinator';
import {
  ZLinkBoundActorRelaySender
} from './bound-actor-relay-sender';
import {
  ZLinkManagedStream
} from './managed-stream';
import { createStreamSessionInstance } from './session-provider';
import { DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE } from '../../contracts/Configuration/InternalDefaults';
import {
  ZLinkStreamSessionNodeRuntime as ZLinkStreamSessionNodeRuntimeCore,
  ZLinkStreamSessionRuntime as ZLinkStreamSessionRuntimeCore,
  type ZLinkStreamSessionNodeRuntimeOptions as ZLinkStreamSessionNodeRuntimeCoreOptions,
  type ZLinkStreamSessionRuntimeOptions as ZLinkStreamSessionRuntimeCoreOptions
} from './stream-session-runtime';
import { ZLinkStreamDispatchCapacity } from './stream-dispatch-capacity';
export { ZLinkPendingSessionRequest } from './session-requests';
export { ZLinkActorSessionLifecycleCoordinator } from './actor-session-lifecycle-coordinator';
export { ZLinkActorSessionBindingRegistry } from './actor-session-binding-registry';
export { zlinkStreamLz4CompressionCodec } from './stream-frame-factory';
export { ZLinkManagedStream } from './managed-stream';
export {
  DefaultZLinkBoundSession,
  DefaultZLinkBoundSessionFactory,
  DefaultZLinkSessionActor,
  DefaultZLinkSessionContext
} from './session-context';
export type { ZLinkBoundSessionResponseTarget } from './bound-session-response-target';
export type {
  ZLinkBoundSessionDisconnectOptions,
  ZLinkBoundSessionSendOptions,
  ZLinkBoundSessionTransport
} from './bound-session-service';

export interface ZLinkStreamBindingRuntimeOptions {
  readonly transport?: ZLinkBoundSessionTransport;
  readonly messageFactory?: ZLinkStreamMessageFactory;
  readonly streamPayloadCodec?: ZLinkStreamPayloadCodec;
  readonly streamCompression?: ZLinkStreamCompressionOptions;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly actorBindTimeoutMs?: number;
  readonly meshSubmitters?: ZLinkMeshSubmitterRegistry;
  readonly nativeActorMeshNameProvider?: () => string | undefined;
  readonly actorRefResolver?: (actor: ZLinkActor) => ActorRef;
  readonly actorAuthorityFenceResolver?: (
    actorId: string,
    signal?: AbortSignal
  ) => Promise<{
    readonly authorityOwnerGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
  } | undefined>;
  readonly nativeActorNodeProvider?: () => {
    status(): { readonly routingId: unknown };
  } | undefined;
  readonly confirmRemoteActorSessionBinding?: (
    actor: ActorRef,
    sessionRid: ActorRef['nodeRid'],
    signal?: AbortSignal
  ) => Promise<void>;
  readonly relay?: (actor: ZLinkSessionActor, header: ZLinkStreamFrameHeader, payload: Message, signal?: AbortSignal) => Promise<boolean>;
  readonly notifyDisconnected?: (actor: ZLinkSessionActor, signal?: AbortSignal) => Promise<void>;
  readonly flowCreationEnabled?: () => boolean;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
}

export interface ZLinkStreamPayloadCodec {
  encode(payload: unknown): {
    readonly codec: ZLinkStreamCodec;
    readonly payload: Uint8Array;
  };
}

export interface ZLinkStreamMessageFactory {
  createTextMessage(payload: string): Message;
  createBinaryMessage?(payload: Uint8Array): Message;
}

export interface ZLinkStreamSessionRuntimeOptions extends Omit<ZLinkStreamSessionRuntimeCoreOptions, 'bindingRuntime'> {
  readonly bindingRuntime?: ZLinkStreamBindingRuntime;
}

export interface ZLinkStreamSessionNodeRuntimeOptions extends Omit<ZLinkStreamSessionNodeRuntimeCoreOptions, 'bindingRuntime'> {
  readonly bindingRuntime?: ZLinkStreamBindingRuntime;
}

export interface ZLinkStreamRuntimeManagerOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly backendAdapterFactory: ZLinkBackendAdapterFactory;
  readonly context: ZLinkBackendContext;
  readonly bindingRuntime: ZLinkStreamBindingRuntime;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly acceptNewSession?: (meshName?: string) => boolean;
  readonly primaryMeshName?: string;
  readonly claimApplicationWork?: (meshName: string) => ZLinkApplicationWorkClaim;
  readonly inboundDispatchBudget?: ZLinkInboundDispatchBudget;
  readonly nativeMeshNode?: ZLinkBackendMeshNode;
  readonly meshCompletions?: ZLinkMeshCompletionTable;
  readonly nativeMeshNodeForName?: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly meshCompletionsForName?: (meshName: string) => ZLinkMeshCompletionTable | undefined;
}

interface ZLinkStartedStreamNode {
  readonly meshName?: string;
  readonly runtime: ZLinkStreamSessionNodeRuntimeCore;
  readonly socket: ZLinkBackendStreamSocket;
  readonly monitor: ZLinkBackendSocketMonitor;
  readonly nativeSessionService?: StreamSessionService;
  readonly nativeSessionServices?: readonly StreamSessionService[];
}

export class ZLinkStreamRuntimeManager {
  private readonly nodes = new Map<string, ZLinkStartedStreamNode>();
  private readonly dispatchCapacity = new ZLinkStreamDispatchCapacity();

  constructor(private readonly options: ZLinkStreamRuntimeManagerOptions) {}

  start(): void {
    if (this.options.registration.streamNodes.size === 0) {
      return;
    }
    const streamAdapter = this.options.backendAdapterFactory.createStreamAdapter();
    const monitoringAdapter = this.options.backendAdapterFactory.createMonitoringAdapter();
    for (const [nodeName, streamNode] of this.options.registration.streamNodes.entries()) {
      const actorDispatchEnabled = streamNode.actorDispatchEnabled === true;
      const applicationMeshName = actorDispatchEnabled ? this.options.primaryMeshName : undefined;
      const nativeMeshNode = actorDispatchEnabled ? this.options.nativeMeshNode : undefined;
      const meshCompletions = actorDispatchEnabled ? this.options.meshCompletions : undefined;
      const socket = streamAdapter.createStreamSocket(this.options.context);
      // Core uses -1 as the explicit unlimited value. The Framework value 0
      // means that it adds no separate STREAM cap, so preserve that meaning
      // when applying the socket option.
      const maxMessageSize = streamNode.maxMessageSize ?? DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE;
      socket.maxMessageSize = maxMessageSize === 0 ? -1 : maxMessageSize;
      const tlsServer = streamNode.tlsServer;
      if (tlsServer !== undefined) {
        socket.setTlsServer(
          tlsServer.certificatePath,
          tlsServer.keyPath,
          tlsServer.requireClientCertificate ?? false
        );
      }
      socket.bind(streamNode.bind!);
      const readablePoller = streamAdapter.createReadablePoller(socket);
      const nativeSessionRoutes = new Map<string, {
        readonly service: StreamSessionService;
        readonly completions: ZLinkMeshCompletionTable;
      }>();
      if (actorDispatchEnabled) {
        for (const [meshName, mesh] of this.options.registration.spotNodes) {
          const isPrimaryMesh = meshName === applicationMeshName;
          if (
            !isPrimaryMesh
            && mesh.objectRole !== 'client'
            && mesh.objectRole !== 'server'
          ) continue;
          const meshNode = this.options.nativeMeshNodeForName?.(meshName)
            ?? (isPrimaryMesh ? nativeMeshNode : undefined);
          const completions = this.options.meshCompletionsForName?.(meshName)
            ?? (isPrimaryMesh ? meshCompletions : undefined);
          const createService = meshNode?.createStreamSessionService;
          if (typeof createService !== 'function' || completions === undefined) continue;
          nativeSessionRoutes.set(meshName, {
            service: createService.call(meshNode, socket.nativeInstance as never),
            completions
          });
        }
      }
      const nativeSessionService = applicationMeshName === undefined
        ? undefined
        : nativeSessionRoutes.get(applicationMeshName)?.service;
      const monitor = monitoringAdapter.openSocketMonitor(socket);
      const sessionType = streamNode.session!;
      const sessionHandlerTypes = (
        streamNode as unknown as Record<symbol, readonly Type[] | undefined>
      )[Symbol.for('@zlink-systems/framework:session-handler-types')] ?? [];
      const claimApplicationWork = this.options.claimApplicationWork;
      const runtime = new ZLinkStreamSessionNodeRuntimeCore({
        nodeName,
        socket,
        readablePoller,
        nativeSessionService,
        meshCompletions,
        nativeSessionRouteForMesh: meshName => nativeSessionRoutes.get(meshName),
        monitor,
        bindingRuntime: this.options.bindingRuntime,
        acceptNewSession: () => this.options.acceptNewSession?.(applicationMeshName) !== false,
        claimApplicationWork: applicationMeshName === undefined
          || claimApplicationWork === undefined
          ? undefined
          : () => claimApplicationWork(applicationMeshName),
        inboundDispatchBudget: this.options.inboundDispatchBudget,
        dispatchErrors: this.options.dispatchErrors,
        metrics: this.options.metrics,
        providerResolver: this.options.providerResolver,
        messageSerializers: this.options.registration.messageSerializers,
        sessionFactory: (context) => createStreamSessionInstance(
          sessionType as Type<ZLinkSession> | Type<ZLinkSessionFactory>,
          this.options.providerResolver,
          context,
          sessionHandlerTypes
        )
      }, this.dispatchCapacity);
      runtime.start();
      this.nodes.set(nodeName, {
        meshName: applicationMeshName,
        runtime,
        socket,
        monitor,
        nativeSessionService,
        nativeSessionServices: [...nativeSessionRoutes.values()].map(route => route.service)
      });
    }
  }

  async dispose(): Promise<void> {
    const nodes = [...this.nodes.values()];
    this.nodes.clear();
    for (const node of nodes.reverse()) {
      await node.runtime.dispose();
      const nativeServices = node.nativeSessionServices
        ?? (node.nativeSessionService === undefined ? [] : [node.nativeSessionService]);
      for (const service of nativeServices) {
        service.shutdown(1000);
        service.close();
      }
      await node.monitor.dispose();
      await node.socket.dispose();
    }
  }

  async notifyServerDrain(meshName: string): Promise<void> {
    await Promise.all([...this.nodes.values()]
      .filter((node) => node.meshName === meshName)
      .map((node) => node.runtime.drainCloseSessions()));
  }

  async notifyUnscopedServerDrain(): Promise<void> {
    await Promise.all([...this.nodes.values()]
      .filter((node) => node.meshName === undefined)
      .map((node) => node.runtime.drainCloseSessions()));
  }

}

export class ZLinkStreamSessionRuntime extends ZLinkStreamSessionRuntimeCore {
  constructor(
    options: ZLinkStreamSessionRuntimeOptions,
    routingId: unknown,
    removeSession?: (sessionId: string, session: ZLinkStreamSessionRuntime) => void
  ) {
    super(
      {
        ...options,
        bindingRuntime: options.bindingRuntime ?? new ZLinkStreamBindingRuntime()
      },
      routingId,
      removeSession === undefined
        ? undefined
        : (sessionId, session) => removeSession(sessionId, session as unknown as ZLinkStreamSessionRuntime)
    );
  }
}

export class ZLinkStreamSessionNodeRuntime extends ZLinkStreamSessionNodeRuntimeCore {
  constructor(options: ZLinkStreamSessionNodeRuntimeOptions) {
    super({
      ...options,
      bindingRuntime: options.bindingRuntime ?? new ZLinkStreamBindingRuntime()
    });
  }
}

export class ZLinkStreamBindingRuntime {
  private readonly routes: ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>;
  private readonly frameMessages: ZLinkStreamFrameMessageFactory;
  private readonly compressionCodec: ZLinkStreamCompressionCodec | undefined;
  private readonly boundSessions: ZLinkBoundSessionService;
  private readonly sessionActors: ZLinkSessionActorCoordinator;
  private readonly boundActorRelay: ZLinkBoundActorRelaySender;

  constructor(options: ZLinkStreamBindingRuntimeOptions = {}) {
    this.routes = new ZLinkActorSessionBindingRegistry<DefaultZLinkSessionContext, DefaultZLinkSessionActor>();
    this.compressionCodec = resolveStreamCompressionCodec(options.streamCompression);
    this.frameMessages = new ZLinkStreamFrameMessageFactory(options);
    this.boundSessions = new ZLinkBoundSessionService(this.routes, this.frameMessages, options);
    const actorSessionLifecycle = new ZLinkActorSessionLifecycleCoordinator();
    this.sessionActors = new ZLinkSessionActorCoordinator(this.routes, this.boundSessions, this, options, actorSessionLifecycle);
    this.boundActorRelay = new ZLinkBoundActorRelaySender(this.routes, this.frameMessages, options, actorSessionLifecycle);
  }

  createSessionContext(
    stream: ZLinkManagedStream,
    close?: (signal?: AbortSignal) => Promise<void>,
    providerResolver?: ZLinkProviderResolver
  ): DefaultZLinkSessionContext {
    return new DefaultZLinkSessionContext(
      this,
      stream,
      close ?? ((signal) => stream.close(signal)),
      providerResolver
    );
  }

  createBoundSession(actorId: string): ZLinkBoundSession {
    return new DefaultZLinkBoundSession(this, actorId);
  }

  async bind(
    context: DefaultZLinkSessionContext,
    actorOrRef: ZLinkActor | ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    return await this.sessionActors.bind(context, actorOrRef, signal);
  }

  async bindOrGet(
    context: DefaultZLinkSessionContext,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<DefaultZLinkSessionActor> {
    return await this.sessionActors.bindOrGet(context, actorRef, signal);
  }

  find(actorId: string): DefaultZLinkSessionActor | undefined {
    return this.routes.find(actorId);
  }

  hasBoundSession(actorId: string): boolean {
    return this.routes.find(actorId) !== undefined;
  }

  captureBoundSessionResponseTarget(actor: ZLinkSessionActor): ZLinkBoundSessionResponseTarget | undefined {
    const bindingToken = (actor as { readonly bindingToken?: unknown }).bindingToken;
    if (typeof bindingToken !== 'string') {
      return undefined;
    }
    const route = this.routes.route(actor.actorId);
    if (route === undefined || route.actor !== actor || route.bindingToken !== bindingToken) {
      return undefined;
    }
    return new DefaultZLinkBoundSessionResponseTarget(
      this.frameMessages,
      route.context,
      actor.actorId
    );
  }

  async rebindActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    await this.sessionActors.rebindActor(actorRef, signal);
  }

  async refreshActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    await this.sessionActors.refreshActor(actorRef, signal);
  }

  async commitActorRoute(actorRef: ActorRef, signal?: AbortSignal): Promise<void> {
    await this.sessionActors.commitActorRoute(actorRef, signal);
  }

  sealActorRoute(input: {
    readonly actorId: string;
    readonly actorGeneration: bigint;
    readonly actorOwnershipGeneration: bigint;
    readonly bindingGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
    readonly sealId: string;
  }): bigint {
    return this.routes.seal(input.actorId, input.sealId, {
      objectGeneration: input.actorGeneration,
      authorityOwnerGeneration: input.actorOwnershipGeneration,
      bindingGeneration: input.bindingGeneration,
      ownerLeaseGeneration: input.ownerLeaseGeneration
    });
  }

  abortActorRouteSeal(actorId: string, sealId: string): boolean {
    return this.routes.abortSeal(actorId, sealId);
  }

  validateActorRouteSeal(actorId: string, sealId: string, acceptedHighWater: bigint): boolean {
    return this.routes.validateSeal(actorId, sealId, acceptedHighWater);
  }

  unbind(actorId: string, context: DefaultZLinkSessionContext, bindingToken: string): void {
    this.routes.unbind(actorId, context, bindingToken);
  }

  unbindActor(actorId: string): void {
    this.routes.unbindActor(actorId);
  }

  async cleanup(context: DefaultZLinkSessionContext): Promise<void> {
    await this.boundActorRelay.notifyPhysicalDisconnect(context);
  }

  async sendBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    return await this.boundSessions.sendBoundSession(actorId, message, packetName, metadata, signal);
  }

  async submitLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    return await this.boundSessions.submitLocalBoundSession(actorId, message, packetName, metadata, signal);
  }

  sendLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    return this.boundSessions.sendLocalBoundSession(actorId, message, packetName, metadata);
  }

  sendLocalBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean
  ): boolean {
    return this.boundSessions.sendLocalBoundSessionResponse(
      actorId,
      packetName,
      requestSeq,
      message,
      metadata,
      compressPayload
    );
  }

  sendLocalBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    return this.boundSessions.sendLocalBoundSessionError(actorId, packetName, requestSeq, error, metadata);
  }


  async sendNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    return await this.boundSessions.sendNativeBoundSession(node, actorRef, message, packetName, metadata, signal);
  }

  async sendNativeBoundSessionResponse(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    signal?: AbortSignal
  ): Promise<void> {
    await this.boundSessions.sendNativeBoundSessionResponse(
      node,
      actorRef,
      packetName,
      requestSeq,
      message,
      metadata,
      compressPayload,
      signal
    );
  }

  async sendNativeBoundSessionError(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<void> {
    await this.boundSessions.sendNativeBoundSessionError(node, actorRef, packetName, requestSeq, error, metadata, signal);
  }

  async disconnectNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    await this.boundSessions.disconnectNativeBoundSession(node, actorRef, signal);
  }

  async disconnectBoundSession(actorId: string, signal?: AbortSignal): Promise<void> {
    await this.boundSessions.disconnectBoundSession(actorId, signal);
  }

  async relay(
    actor: DefaultZLinkSessionActor,
    payload: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    return await this.boundActorRelay.relay(actor, payload, signal);
  }

  async notifyDisconnected(actor: DefaultZLinkSessionActor, signal?: AbortSignal): Promise<void> {
    await this.boundActorRelay.notifyDisconnected(actor, signal);
  }

  createTextMessage(payload: string): Message {
    return this.frameMessages.createTextMessage(payload);
  }

  createBinaryMessage(payload: Uint8Array): Message {
    return this.frameMessages.createBinaryMessage(payload);
  }

  decompressPayload(payload: Message): Message {
    return simpleMessage(decompressStreamPayload(messageToBytes(payload), this.compressionCodec)) as Message;
  }

  createJsonFrameMessage(
    kind: ZLinkStreamMessageKind,
    packetName: string,
    metadata: ReadonlyMap<string, string>,
    compressed: boolean,
    requestSeq: bigint | undefined,
    payload: unknown,
    correlationId?: string
  ): Message {
    return this.frameMessages.createJsonFrameMessage(kind, packetName, metadata, compressed, requestSeq, payload, correlationId);
  }

  createJsonReplyFrameMessage(
    requestHeader: ZLinkStreamFrameHeader,
    kind: ZLinkStreamReplyMessageKind,
    metadata: ReadonlyMap<string, string>,
    compressed: boolean,
    payload: unknown
  ): Message {
    return this.frameMessages.createJsonReplyFrameMessage(requestHeader, kind, metadata, compressed, payload);
  }

}

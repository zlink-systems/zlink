import type {
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkActorFactory,
  ZLinkDispatchOptions,
  ZLinkApplicationHwmProfile,
  ZLinkEntrySpot,
  ZLinkHandlerFilter,
  ZLinkLocationOptions,
  ZLinkLocationOptionValues,
  ZLinkLocationStore,
  ZLinkRelocationStore,
  ZLinkSpotRelocationReadinessMode,
  ZLinkUserSpotExecutionMode,
  ZLinkInstanceSpot,
  ZLinkMessageSerializer,
  ZLinkMetricsOptions,
  ZLinkNetworkOptions,
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkRouteMessageContext,
  ZLinkSpot,
  ZLinkStreamCompressionOptions,
  ZLinkTimerOptions
} from '@zlink-systems/framework';

/** Private composition values; not part of the framework public barrel. */
export interface ZLinkInboundDispatchOptionValues {
  applicationHwmBytes?: bigint;
  applicationHwmProfile: ZLinkApplicationHwmProfile;
  processMemoryLimitBytes?: bigint;
}

/** Private composition bridge used only by the Nest companion package. */
export interface ZLinkProviderResolver {
  get?<T>(type: Type<T>): T | undefined;
  create?<T>(type: Type<T>): T | Promise<T>;
}

export interface ZLinkWorkerOptions {
  readonly minThreads: number;
  readonly maxThreads: number;
  readonly idleTimeoutMs: number;
  readonly maxQueueLength: number;
}

export interface ZLinkNestIntegrationRuntimeHost {
  readonly channelRuntimeOptions: unknown;
  readonly routeMeshRuntimeOptions:
    import('@zlink-systems/framework').ZLinkRouteMeshRuntimeOptions;
  readonly boundSessionFactory: unknown;
  readonly locationRuntimeQuery?: unknown;
  readonly routeMeshRuntime: import('@zlink-systems/framework').ZLinkRouteMeshRuntime;
  readonly clientServerRuntime: import('@zlink-systems/framework').ZLinkClientServerRuntime;
  readonly fanoutRuntime: import('@zlink-systems/framework').ZLinkFanoutRuntime;
  createLocationHandleResolver(): unknown;
  start(): Promise<void>;
  stop(): Promise<void>;
  onApplicationBootstrap?(): Promise<void> | void;
  onApplicationShutdown?(): Promise<void> | void;
}

export interface ZLinkRouteChannelSendHandler {
  handle(payload: unknown, context: ZLinkRouteMessageContext): Promise<void>;
}

export interface ZLinkRouteChannelRequestHandler {
  handle(payload: unknown, context: ZLinkRouteMessageContext): Promise<unknown>;
}

// These records describe the private composition bridge between the Nest
// adapter and the framework runtime. They intentionally remain owned by the
// Nest package instead of becoming a framework package subpath.
export interface ZLinkFrameworkRegistration {
  readonly network: ZLinkNetworkOptions;
  readonly messageSerializers: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly codecs: ZLinkCodecRegistration;
  readonly requestTimeoutMs?: number;
  readonly actorFactories: ReadonlyMap<string, Type>;
  readonly actorTransferTimeoutMs?: number;
  readonly messageFollowDurationMs: number;
  readonly spotFactories: ReadonlySet<Type<ZLinkSpot>>;
  readonly channels: ReadonlyMap<string, ZLinkChannelOptions>;
  readonly channelClients: ReadonlySet<string>;
  readonly fanoutPublishers: ReadonlySet<string>;
  readonly routeChannels: ReadonlySet<string>;
  readonly routeChannelOptions: ReadonlyMap<string, ZLinkRouteChannelOptions>;
  readonly streamNodes: ReadonlyMap<string, ZLinkStreamNodeOptions>;
  readonly streamCompression?: ZLinkStreamCompressionOptions;
  readonly spotNodes: ReadonlyMap<string, ZLinkSpotNodeOptions>;
  readonly spotPublisherClients: ReadonlySet<string>;
  readonly filterTypes: readonly Type<ZLinkHandlerFilter>[];
  readonly worker?: ZLinkWorkerOptions;
  readonly inboundDispatch?: Partial<ZLinkInboundDispatchOptionValues>;
  readonly dispatch?: ZLinkDispatchOptions;
  readonly metrics?: ZLinkMetricsOptions;
  readonly locations: ZLinkLocationRegistration;
}

export interface ZLinkLocationRegistration {
  readonly useInMemoryStores: boolean;
  readonly storeInstance?: ZLinkLocationStore;
  readonly relocationStoreInstance?: ZLinkRelocationStore;
  readonly options: ZLinkLocationOptions;
}

export interface ZLinkCodecSerializerRegistration {
  readonly contentType: string;
  readonly serializer: ZLinkMessageSerializer;
}

export interface ZLinkStreamCodecRegistration {
  readonly contentType: string;
  readonly codec: unknown;
}

export interface ZLinkCodecRegistration {
  readonly serializers: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly streamCodecs: ReadonlyMap<string, unknown>;
}

export interface ZLinkCodecRegistryOptions {
  readonly serializers?: readonly ZLinkCodecSerializerRegistration[];
  readonly streamCodecs?: readonly ZLinkStreamCodecRegistration[];
}

export interface ZLinkFrameworkRegistrationOptions {
  readonly network?: Partial<ZLinkNetworkOptions>;
  readonly codecs?: ZLinkCodecRegistryOptions;
  readonly applicationVersion?: bigint;
  readonly maintenanceWave?: string;
  readonly requestTimeoutMs?: number;
  readonly spotFactories?: readonly Type<ZLinkSpot>[];
  readonly actorTransferTimeoutMs?: number;
  readonly messageFollowDurationMs?: number;
  readonly channels?: Readonly<Record<string, ZLinkChannelOptions>>;
  readonly routeChannels?: readonly (string | ZLinkRouteChannelOptions)[];
  readonly streamNodes?: Readonly<Record<string, ZLinkStreamNodeOptions>>;
  readonly streamCompression?: ZLinkStreamCompressionOptions;
  readonly spotNodes?: readonly (string | ZLinkSpotNodeRegistrationOptions)[] |
    Readonly<Record<string, ZLinkSpotNodeOptions>>;
  readonly spotPublisherClients?: readonly string[];
  readonly filters?: readonly Type<ZLinkHandlerFilter>[];
  readonly worker?: ZLinkWorkerOptions;
  readonly inboundDispatch?: Partial<ZLinkInboundDispatchOptionValues>;
  readonly dispatch?: ZLinkDispatchOptions;
  readonly metrics?: ZLinkMetricsOptions;
  readonly locations?: {
    readonly useInMemoryStores?: boolean;
    readonly storeInstance?: ZLinkLocationStore;
    readonly relocationStoreInstance?: ZLinkRelocationStore;
    readonly options?: Partial<ZLinkLocationOptionValues>;
  };
}

export interface ZLinkChannelOptions {
  readonly routingId?: string;
  readonly routingIdPrefix?: string;
  readonly requestTimeoutMs?: number;
  readonly client?: ZLinkClientCapabilityOptions;
  readonly publisher?: ZLinkPublisherCapabilityOptions;
  readonly routeMesh?: ZLinkRouteMeshChannelOptions;
  readonly publishHandlers?: readonly ZLinkChannelPublishHandlerRegistration[];
  readonly requestHandlers?: readonly ZLinkChannelRequestHandlerRegistration[];
  readonly sendHandlers?: readonly ZLinkChannelSendHandlerRegistration[];
  readonly server?: {
    readonly bind?: string;
    readonly bindHost?: string;
    readonly advertiseHost?: string;
    readonly port?: number;
    readonly routingId?: string;
    readonly weight?: number;
    readonly sendHighWaterMark?: number;
    readonly receiveHighWaterMark?: number;
    readonly sendTimeoutMs?: number;
    readonly maxMessageSize?: number;
  };
  readonly subscriber?: ZLinkClientCapabilityOptions;
}

export interface ZLinkClientCapabilityOptions {
  readonly manualConnections?: readonly string[];
  readonly sendHighWaterMark?: number;
  readonly receiveHighWaterMark?: number;
  readonly sendTimeoutMs?: number;
  readonly maxMessageSize?: number;
}

export interface ZLinkPublisherCapabilityOptions {
  readonly bind?: string;
  readonly bindHost?: string;
  readonly advertiseHost?: string;
  readonly port?: number;
}

export interface ZLinkRouteMeshChannelOptions {
  readonly requestTimeoutMs?: number;
  readonly bind?: string;
  readonly manualConnections?: readonly string[];
  readonly routingId?: string;
  readonly routingIdPrefix?: string;
  readonly weight?: number;
  readonly sendHighWaterMark?: number;
  readonly receiveHighWaterMark?: number;
  readonly sendTimeoutMs?: number;
  readonly maxMessageSize?: number;
  readonly sendHandlers?: readonly ZLinkRouteChannelSendHandlerRegistration[];
  readonly requestHandlers?: readonly ZLinkRouteChannelRequestHandlerRegistration[];
  readonly handlers?: readonly ZLinkRouteChannelHandlerOptions[];
}

export interface ZLinkRouteChannelOptions extends ZLinkRouteMeshChannelOptions {
  readonly routerChannelId: string;
}

export interface ZLinkStreamNodeOptions {
  readonly bind?: string;
  readonly bindHost?: string;
  readonly advertiseHost?: string;
  readonly port?: number;
  readonly actorDispatchEnabled?: boolean;
  readonly tlsServer?: {
    readonly certificatePath: string;
    readonly keyPath: string;
    readonly requireClientCertificate?: boolean;
  };
  readonly session?: Type;
}

export interface ZLinkSpotNodeRegistrationOptions extends ZLinkSpotNodeOptions {
  readonly name: string;
}

export interface ZLinkSpotNodeOptions {
  readonly objectRole?: 'client' | 'server';
  readonly placementWeight?: number;
  readonly actorLimit?: number;
  readonly spotLimit?: number;
  readonly activationConcurrencyLimit?: number;
  readonly instanceSpotIdleTimeoutMs?: number;
  readonly routingId?: string;
  readonly routingIdPrefix?: string;
  readonly router?: ZLinkSpotRouterCapabilityOptions;
  readonly pubSub?: ZLinkSpotPubSubCapabilityOptions;
  readonly entrySpotType?: Type<ZLinkEntrySpot>;
  readonly spotFactories?: readonly Type<ZLinkSpot>[];
  readonly spotFactoryRegistrations?: Readonly<Record<
    string,
    ZLinkObjectFactoryRegistration<ZLinkSpot, ZLinkSpot, ZLinkUserSpotFactoryConfiguration>
  >>;
  readonly instanceSpotFactories?: Readonly<Record<string, Type<ZLinkInstanceSpot>>>;
  readonly instanceSpotFactoryRegistrations?: Readonly<Record<
    string,
    ZLinkObjectFactoryRegistration<
      ZLinkInstanceSpot,
      ZLinkInstanceSpot,
      ZLinkInstanceSpotFactoryConfiguration
    >
  >>;
  readonly actorFactories?: Readonly<Record<string, Type> | Map<string, Type>>;
  readonly actorFactoryRegistrations?: Readonly<Record<
    string,
    ZLinkObjectFactoryRegistration<
      ZLinkActor,
      ZLinkActorFactory,
      ZLinkActorFactoryConfiguration
    >
  >>;
  readonly meshChannels?: Readonly<Record<string, ZLinkMeshChannelOptions>>;
  readonly routeSendHandlers?: readonly ZLinkRouteMeshSendHandlerRegistration[];
  readonly routeRequestHandlers?: readonly ZLinkRouteMeshRequestHandlerRegistration[];
  readonly requestTimeoutMs?: number;
  readonly publisherConfig?: {
    readonly sendHighWaterMark?: number;
    readonly sendTimeoutMs?: number;
    readonly lingerMs?: number;
  };
  readonly entrySpotTimerHandlers?: readonly ZLinkEntrySpotTimerHandlerRegistration[];
  readonly entrySpotPacketHandlers?: readonly ZLinkEntrySpotPacketHandlerRegistration[];
  readonly entrySpotSubscriptionHandlers?: readonly ZLinkEntrySpotSubscriptionHandlerRegistration[];
  readonly entrySpotActorSendHandlers?: readonly ZLinkEntrySpotActorSendHandlerRegistration[];
  readonly entrySpotActorRequestHandlers?: readonly ZLinkEntrySpotActorRequestHandlerRegistration[];
  readonly spotTimerHandlers?: readonly ZLinkSpotTimerHandlerRegistration[];
  readonly spotPacketHandlers?: readonly ZLinkSpotPacketHandlerRegistration[];
  readonly spotSubscriptionHandlers?: readonly ZLinkSpotSubscriptionHandlerRegistration[];
  readonly spotActorSendHandlers?: readonly ZLinkSpotActorSendHandlerRegistration[];
  readonly spotActorRequestHandlers?: readonly ZLinkSpotActorRequestHandlerRegistration[];
}

interface ZLinkObjectFactoryRegistration<T, TImplementation = T, TOptions = unknown> {
  readonly implementation: Type<TImplementation>;
  readonly options?: TOptions;
  readonly relocation: ZLinkRelocationConfiguration<T>;
}

declare const zlinkRelocationConfigurationBrand: unique symbol;

export type ZLinkRelocationConfiguration<T> = (
  | { readonly kind: 'disabled' }
  | { readonly kind: 'recreate' }
  | { readonly kind: 'snapshot'; readonly adapterType: Type }
) & { readonly [zlinkRelocationConfigurationBrand]?: T };

export interface ZLinkActorFactoryConfiguration {}

export interface ZLinkUserSpotFactoryConfiguration {
  readonly stableTypeLimit?: number;
  readonly executionMode: ZLinkUserSpotExecutionMode;
  readonly relocationReadiness: ZLinkSpotRelocationReadinessMode;
}

export interface ZLinkInstanceSpotFactoryConfiguration {
  readonly stableTypeLimit?: number;
}

export interface ZLinkMeshChannelOptions {
  readonly client?: boolean;
  readonly server?: boolean;
  readonly weight?: number;
  readonly handlerGroups?: readonly string[];
  readonly sendHandlers?: readonly ZLinkMeshSendHandlerRegistration[];
  readonly requestHandlers?: readonly ZLinkMeshRequestHandlerRegistration[];
}

export interface ZLinkMeshSendHandlerRegistration {
  readonly packetName: string;
  readonly handlerType: Type;
}

export interface ZLinkMeshRequestHandlerRegistration {
  readonly packetName: string;
  readonly handlerType: Type;
}

export interface ZLinkRouteMeshSendHandlerRegistration {
  readonly packetName: string;
  readonly handlerType: Type;
}

export interface ZLinkRouteMeshRequestHandlerRegistration {
  readonly packetName: string;
  readonly handlerType: Type;
}


export interface ZLinkSpotRouterCapabilityOptions {
  readonly bind?: string;
  readonly bindHost?: string;
  readonly advertiseHost?: string;
  readonly port?: number;
  readonly manualConnections?: readonly string[];
  readonly manualPeerConnections?: readonly { readonly peerRid: RoutingId; readonly endpoint: string }[];
  readonly routingId?: string;
}

export interface ZLinkSpotPubSubCapabilityOptions {
  readonly bind?: string;
  readonly manualConnections?: readonly string[];
  readonly routingId?: string;
}

export interface ZLinkRouteChannelHandlerOptions {
  readonly kind: 'send' | 'request';
  readonly packetName: string;
  readonly handler: ZLinkRouteChannelSendHandler | ZLinkRouteChannelRequestHandler;
}

export interface ZLinkRouteChannelSendHandlerRegistration {
  readonly packetName: string;
  readonly handler: ZLinkRouteChannelSendHandler;
}

export interface ZLinkRouteChannelRequestHandlerRegistration {
  readonly packetName: string;
  readonly handler: ZLinkRouteChannelRequestHandler;
}

export interface ZLinkChannelPublishHandlerRegistration {
  readonly packetName: string;
  readonly handler: { handle(payload: unknown, context: ZLinkPublishMessageContext): Promise<void> };
}

export interface ZLinkChannelRequestHandlerRegistration {
  readonly packetName: string;
  readonly handler: { handle(payload: unknown, context: ZLinkMessageContext): Promise<unknown> };
}

export interface ZLinkChannelSendHandlerRegistration {
  readonly packetName: string;
  readonly handler: { handle(payload: unknown, context: ZLinkMessageContext): Promise<void> };
}

export interface ZLinkEntrySpotTimerHandlerRegistration {
  readonly entrySpotType: Type<ZLinkEntrySpot>; readonly handlerType: Type;
  readonly name: string; readonly periodMs: number; readonly options?: ZLinkTimerOptions;
}
export interface ZLinkEntrySpotPacketHandlerRegistration {
  readonly entrySpotType: Type<ZLinkEntrySpot>; readonly handlerType: Type; readonly packetName?: string;
}
export interface ZLinkEntrySpotSubscriptionHandlerRegistration {
  readonly entrySpotType: Type<ZLinkEntrySpot>; readonly handlerType: Type;
  readonly channelName: string; readonly topic: string;
}
export interface ZLinkEntrySpotActorSendHandlerRegistration {
  readonly entrySpotType: Type<ZLinkEntrySpot>; readonly actorType: Type<ZLinkActor>;
  readonly handlerType: Type; readonly packetName: string;
}
export interface ZLinkEntrySpotActorRequestHandlerRegistration extends ZLinkEntrySpotActorSendHandlerRegistration {}
export interface ZLinkSpotTimerHandlerRegistration {
  readonly spotType: Type<ZLinkSpot>; readonly handlerType: Type;
  readonly name: string; readonly periodMs: number; readonly options?: ZLinkTimerOptions;
}
export interface ZLinkSpotPacketHandlerRegistration {
  readonly spotType: Type<ZLinkSpot>; readonly handlerType: Type; readonly packetName?: string;
}
export interface ZLinkSpotSubscriptionHandlerRegistration {
  readonly spotType: Type<ZLinkSpot>; readonly handlerType: Type;
  readonly channelName: string; readonly topic: string;
}
export interface ZLinkSpotActorSendHandlerRegistration {
  readonly spotType: Type<ZLinkSpot>; readonly actorType: Type<ZLinkActor>;
  readonly handlerType: Type; readonly packetName: string;
}
export interface ZLinkSpotActorRequestHandlerRegistration extends ZLinkSpotActorSendHandlerRegistration {}

# Node.js NestJS Host Adapter Public Interface

[Interface table of contents](README.en.md) · [Node.js contract table of contents](../README.en.md)

This document fixes the exact TypeScript declarations related to the
NestJS host adapter that `@zlink-systems/framework` and
`@zlink-systems/nestjs` export in ZLink Framework. Behavioral meaning is
owned by the [common spec](../../../../README.en.md) — this document
only defines names, generics, overloads, inheritance, members,
parameters, and return types.

## 1. Dynamic Module And DI Token

```ts
export declare function createZLinkDynamicModule(registration: ZLinkFrameworkRegistration): DynamicModule;

export declare const ZLINK_ACTOR_CLIENT: unique symbol;

export declare const ZLINK_ACTOR_MANAGER: unique symbol;

export declare const ZLINK_BOUND_SESSION_FACTORY: unique symbol;

export declare const ZLINK_CHANNEL_CLIENT: unique symbol;

export declare const ZLINK_CHANNEL_RUNTIME_OPTIONS: unique symbol;

export declare const ZLINK_CLIENT_SERVER_RUNTIME: unique symbol;

export declare const ZLINK_FANOUT_CLIENT: unique symbol;

export declare const ZLINK_FANOUT_RUNTIME: unique symbol;

export declare const ZLINK_FRAMEWORK_REGISTRATION: unique symbol;

export declare const ZLINK_FRAMEWORK_RUNTIME: unique symbol;

export declare const ZLINK_LOCATION_RUNTIME_QUERY: unique symbol;

export declare const ZLINK_ROUTE_MESH_RUNTIME: unique symbol;
```

## 2. Runtime Provider And Entry Spot Decorator

```ts
export declare const ZLINK_MESSAGE_METADATA_POLICY: unique symbol;

export declare const ZLINK_NEST_HANDLER_GROUP: unique symbol;

export declare const ZLINK_ROUTE_CLIENT: unique symbol;

export declare const ZLINK_SPOT_MANAGER: unique symbol;

export declare const ZLINK_SPOT_OUTBOUND: unique symbol;

export declare const ZLINK_SPOT_PUBLISHER_CLIENT: unique symbol;

export declare function zlinkDiscoverProviders(rootDir: string, options?: ZLinkNestProviderDiscoveryOptions): Provider[];

export declare class ZLinkDrainHealthIndicator {
    private readonly runtime;
    private readonly meshName;
    constructor(runtime: ZLinkRouteMeshRuntime, meshName: string);
    isHealthy(key?: string): Promise<Record<string, {
        readonly status: 'up';
    }>>;
}

export declare function zlinkEntrySpotActorRequestHandler<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor>(options: ZLinkNestEntrySpotActorRequestHandlerOptions<TEntrySpot, TActor>): ClassDecorator;

export declare function zlinkEntrySpotActorSendHandler<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor>(options: ZLinkNestEntrySpotActorSendHandlerOptions<TEntrySpot, TActor>): ClassDecorator;
```

## 3. Module, Handler, And Codec Registration

```ts
export declare function zlinkEntrySpotPacketHandler<TEntrySpot extends ZLinkEntrySpot>(options: ZLinkNestEntrySpotPacketHandlerOptions<TEntrySpot>): ClassDecorator;

export declare function zlinkEntrySpotSubscriptionHandler<TEntrySpot extends ZLinkEntrySpot>(options: ZLinkNestEntrySpotSubscriptionHandlerOptions<TEntrySpot>): ClassDecorator;

export declare function zlinkFramework(): ZLinkNestFrameworkOptionsBuilder;

export declare function zlinkHandler(groupName: string, kind: ZLinkNestHandlerKind, packetName?: string, options?: ZLinkNestHandlerOptions): ClassDecorator;

export declare function zlinkModule(metadata: ZLinkNestModuleMetadata): ClassDecorator;
export declare function zlinkModule(roleRoot: ZLinkNestModuleRoleRoot, metadata: ModuleMetadata): ClassDecorator;

export declare class ZLinkModule {
    static forRoot(options?: ZLinkModuleOptions): DynamicModule;
    static forRootFactory(options: ZLinkModuleFactoryOptions): DynamicModule;
}

export interface ZLinkModuleFactoryOptions<TArgs extends unknown[] = unknown[]> {
    readonly useFactory: (...args: TArgs) => ZLinkModuleOptions | Promise<ZLinkModuleOptions>;
    readonly inject?: { readonly [TIndex in keyof TArgs]: InjectionToken };
    readonly imports?: ModuleMetadata['imports'];
}

export interface ZLinkModuleOptions {
    readonly [ZLINK_MODULE_OPTIONS_BRAND]: true;
}

export interface ZLinkNestCodecRegistryBuilder extends ZLinkNestFrameworkOptionsBuilder {
    use(extension: ZLinkCodecExtension): this;
}

export interface ZLinkNestEntrySpotActorRequestHandlerOptions<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor> {
    readonly entrySpot: ZLinkNestTypeResolver<TEntrySpot>;
    readonly actor: ZLinkNestTypeResolver<TActor>;
    readonly packetName: string;
    readonly methodName?: string;
}
```

## 4. NestJS Host Configuration

```ts
export interface ZLinkNestEntrySpotActorSendHandlerOptions<TEntrySpot extends ZLinkEntrySpot, TActor extends ZLinkActor> {
    readonly entrySpot: ZLinkNestTypeResolver<TEntrySpot>;
    readonly actor: ZLinkNestTypeResolver<TActor>;
    readonly packetName: string;
    readonly methodName?: string;
}

export interface ZLinkNestEntrySpotPacketHandlerOptions<TEntrySpot extends ZLinkEntrySpot> {
    readonly entrySpot: ZLinkNestTypeResolver<TEntrySpot>;
    readonly packetName?: string;
}

export interface ZLinkNestEntrySpotSubscriptionHandlerOptions<TEntrySpot extends ZLinkEntrySpot> {
    readonly entrySpot: ZLinkNestTypeResolver<TEntrySpot>;
    readonly channelName: string;
    readonly topic: string;
}

export interface ZLinkNestFanoutChannelBuilder extends ZLinkNestFrameworkOptionsBuilder {
    enablePublisher(bind: string | undefined): this;
    enablePublisher(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    routingId(routingId: string | undefined): this;
    setRoutingIdPrefix(prefix: string): this;
    enableSubscriber(endpoint?: string | readonly string[]): this;
    addPublishHandler(packetName: string, handlerType: Type): this;
    addHandlerGroup(groupName: string): this;
}

export interface ZLinkNestFrameworkAdditionalOptions {
    readonly requestTimeoutMs?: number;
    readonly filters?: readonly Type<ZLinkHandlerFilter>[];
    readonly worker?: ZLinkWorkerOptions;
    readonly dispatch?: ZLinkDispatchOptions;
    readonly metrics?: ZLinkMetricsOptions;
}

export interface ZLinkLocationOptions {
    ownerLeaseRenewIntervalMs(value: number): this;
    ownerLeaseTtlMs(value: number): this;
    pollingIntervalMs(value: number): this;
    storeFailureGraceMs(value: number): this;
    ownerLeaseFencingMarginMs(value: number): this;
    ownerLeaseRenewTimeoutMs(value: number): this;
    routeCacheMaxAgeMs(value: number): this;
    messageFollowDurationMs(value: number): this;
    maxActiveOutboundRelocations(value: number): this;
    maxActiveInboundRelocations(value: number): this;
    maxConcurrentRelocationCaptures(value: number): this;
    maxConcurrentRelocationRestores(value: number): this;
    maxRelocationPayloadInFlightBytes(value: number): this;
}

export interface ZLinkNestFrameworkOptionsBuilder {
    options(options: ZLinkNestFrameworkAdditionalOptions): this;
    codecs(): ZLinkNestCodecRegistryBuilder;
    configureDispatch(): ZLinkDispatchOptionsBuilder;
    addLocationStore(store: ZLinkLocationStore): this;
    addRelocationStore(store: ZLinkRelocationStore): this;
    setApplicationVersion(version: bigint): this;
    setMaintenanceWave(waveId: string): this;
    setActorTransferTimeout(timeoutMs: number): this;
    setMessageFollowDuration(timeoutMs: number): this;
    configureStreamCompression(): ZLinkStreamCompressionBuilder;
    configureLocations(): ZLinkLocationOptions;
    configureInboundDispatch(): ZLinkInboundDispatchOptions;
    configureNetwork(): ZLinkNetworkOptions;
    addRouteMesh(name: string): ZLinkNestMeshNodeBuilder;
    addClientServerChannel(name: string): ZLinkNestClientServerChannelRoleBuilder;
    addFanoutChannel(name: string): ZLinkNestFanoutChannelBuilder;
    addStreamNode(name: string): ZLinkNestStreamNodeBuilder;
    build(): ZLinkModuleOptions;
}

export type ZLinkNestHandlerKind = 'request' | 'send' | 'publish';

export interface ZLinkNestHandlerOptions {
    readonly methodName?: string;
    readonly decodePayload?: (payload: Buffer, context: ZLinkMessageContext | ZLinkRouteMessageContext | ZLinkPublishMessageContext) => unknown;
    readonly encodeResult?: (result: unknown, context: ZLinkMessageContext | ZLinkRouteMessageContext) => unknown;
}

export interface ZLinkNestModuleMetadata extends ModuleMetadata {
    readonly providerDiscovery?: readonly ZLinkNestProviderDiscoveryRoot[];
}

export type ZLinkNestModuleRoleRoot = string;

export interface ZLinkNestProviderDiscoveryOptions {
    readonly recursive?: boolean;
}

export type ZLinkNestProviderDiscoveryRoot = string | {
    readonly rootDir: string;
    readonly options?: ZLinkNestProviderDiscoveryOptions;
};
```

## 5. MeshNode, Channel, Spot, And STREAM Builder

```ts
export interface ZLinkNestMeshNodeBuilder extends ZLinkNestFrameworkOptionsBuilder {
    channel(name: string): ZLinkNestMeshChannelBuilder;
    listen(endpoint: string): this;
    listen(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    routingId(routingId: string | undefined): this;
    setRoutingIdPrefix(prefix: string): this;
    setPlacementWeight(weight: number): this;
    setActorLimit(limit: number): this;
    setSpotLimit(limit: number): this;
    setActivationConcurrency(limit: number): this;
    setInstanceSpotIdleTimeout(timeoutMs: number): this;
    objects(): ZLinkNestMeshObjectRoleBuilder;
    configureRouterSocket(): ZLinkMeshNodeSocketConfig;
    configureSpotPublisher(): ZLinkSpotPublisherConfig;
    peerConnections(): ZLinkMeshPeerConnections;
    addSendHandler(packetName: string, handlerType: Type): this;
    addRequestHandler(packetName: string, handlerType: Type): this;
}

export interface ZLinkNestMeshObjectRoleBuilder extends ZLinkNestFrameworkOptionsBuilder {
    client(): ZLinkNestMeshObjectClientBuilder;
    server(): ZLinkNestMeshObjectServerBuilder;
}

export interface ZLinkNestMeshObjectClientBuilder extends ZLinkNestFrameworkOptionsBuilder {
}

export interface ZLinkNestMeshObjectServerBuilder extends ZLinkNestFrameworkOptionsBuilder {
    addEntrySpot<TEntrySpot extends ZLinkEntrySpot>(entrySpotType: Type<TEntrySpot>): this;
    addSpotFactory<TSpot extends ZLinkSpot>(
        spotType: string,
        implementation: Type<TSpot>,
        configure: (builder: ZLinkUserSpotFactoryBuilder<TSpot>) => void): this;
    addInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
        instanceSpotType: string,
        implementation: Type<TSpot>,
        configure: (builder: ZLinkInstanceSpotFactoryBuilder<TSpot>) => void): this;
    addActorFactory<TActor extends ZLinkActor>(
        actorType: string,
        factoryType: Type<ZLinkActorFactory<TActor>>,
        configure: (builder: ZLinkActorFactoryBuilder<TActor>) => void): this;
}

export interface ZLinkNestMeshChannelBuilder extends ZLinkNestFrameworkOptionsBuilder {
    client(): ZLinkNestMeshChannelClientBuilder;
    server(): ZLinkNestMeshChannelServerBuilder;
}

export interface ZLinkNestMeshChannelClientBuilder extends ZLinkNestFrameworkOptionsBuilder {
}

export interface ZLinkNestMeshChannelServerBuilder extends ZLinkNestFrameworkOptionsBuilder {
    setWeight(weight: number): this;
    addSendHandler(packetName: string, handlerType: Type): this;
    addRequestHandler(packetName: string, handlerType: Type): this;
    addHandlerGroup(groupName: string): this;
}

export interface ZLinkNestClientServerChannelRoleBuilder extends ZLinkNestFrameworkOptionsBuilder {
    client(): ZLinkNestClientServerChannelClientBuilder;
    server(): ZLinkNestClientServerChannelServerBuilder;
}

export interface ZLinkNestClientServerChannelClientBuilder extends ZLinkNestFrameworkOptionsBuilder {
    connect(endpoint: string): this;
}

export interface ZLinkNestClientServerChannelServerBuilder extends ZLinkNestFrameworkOptionsBuilder {
    listen(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    setWeight(weight: number): this;
    addSendHandler(packetName: string, handlerType: Type): this;
    addRequestHandler(packetName: string, handlerType: Type): this;
    addHandlerGroup(groupName: string): this;
}

export interface ZLinkNestSpotActorRequestHandlerOptions<TSpot extends ZLinkSpot, TActor extends ZLinkActor> {
    readonly spot: ZLinkNestTypeResolver<TSpot>;
    readonly actor: ZLinkNestTypeResolver<TActor>;
    readonly packetName: string;
    readonly methodName?: string;
}

export interface ZLinkNestSpotActorSendHandlerOptions<TSpot extends ZLinkSpot, TActor extends ZLinkActor> {
    readonly spot: ZLinkNestTypeResolver<TSpot>;
    readonly actor: ZLinkNestTypeResolver<TActor>;
    readonly packetName: string;
    readonly methodName?: string;
}

export interface ZLinkNestSpotPacketHandlerOptions<TSpot extends ZLinkSpot> {
    readonly spot: ZLinkNestTypeResolver<TSpot>;
    readonly packetName?: string;
}

export interface ZLinkNestSpotSubscriptionHandlerOptions<TSpot extends ZLinkSpot> {
    readonly spot: ZLinkNestTypeResolver<TSpot>;
    readonly channelName: string;
    readonly topic: string;
}

export interface ZLinkNestSpotTimerHandlerOptions<TSpot extends ZLinkSpot = ZLinkSpot> {
    readonly spot?: ZLinkNestTypeResolver<TSpot>;
    readonly entrySpot?: ZLinkNestTypeResolver<ZLinkEntrySpot>;
    readonly name?: string;
    readonly periodMs?: number;
    readonly options?: ZLinkTimerOptions;
}

export interface ZLinkNestStreamNodeBuilder extends ZLinkNestFrameworkOptionsBuilder {
    bind(endpoint: string | undefined): this;
    bind(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    enableActorDispatch(): this;
    setTlsServer(certificatePath: string, keyPath: string, requireClientCertificate?: boolean): this;
    registerSession<TSession extends ZLinkSession>(sessionType: Type<TSession> | Type<ZLinkSessionFactory<TSession>>): this;
}

export type ZLinkNestTypeResolver<T> = Type<T> | (() => Type<T>);

export declare function zlinkPublishHandler(groupName: string, packetName?: string, options?: ZLinkNestHandlerOptions): ClassDecorator;

export declare function zlinkRequestHandler(groupName: string, packetName?: string, options?: ZLinkNestHandlerOptions): ClassDecorator;

```

## 6. Spot Handler Decorator

```ts
export declare function zlinkSendHandler(groupName: string, packetName?: string, options?: ZLinkNestHandlerOptions): ClassDecorator;

export declare function zlinkSpotActorRequestHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(options: ZLinkNestSpotActorRequestHandlerOptions<TSpot, TActor>): ClassDecorator;

export declare function zlinkSpotActorSendHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(options: ZLinkNestSpotActorSendHandlerOptions<TSpot, TActor>): ClassDecorator;

export declare function zlinkSpotPacketHandler<TSpot extends ZLinkSpot>(options: ZLinkNestSpotPacketHandlerOptions<TSpot>): ClassDecorator;

export declare function zlinkSpotSubscriptionHandler<TSpot extends ZLinkSpot>(options: ZLinkNestSpotSubscriptionHandlerOptions<TSpot>): ClassDecorator;

export declare function zlinkSpotTimerHandler<TSpot extends ZLinkSpot = ZLinkSpot>(options?: ZLinkNestSpotTimerHandlerOptions<TSpot>): ClassDecorator;
```

## 7. Server HTTP Client Integration

To run an HTTP request in a server handler, an injected client
registered by name is used. A registered client is cleaned up together
with the Nest module's lifetime. `yield()` can only be chosen while the
current handler is running on a `SpotWide` User Spot or Instance Spot's
shared turn. In a different handler, it completes with
`invalidConfiguration`, without submitting the HTTP operation or
returning the turn.

```ts
export interface ZLinkNamedHttpClientOptions {
    readonly name: string;
    readonly baseUrl: string;
    readonly configure?: (builder: ZLinkHttpClientBuilder) => void;
}

export interface ZLinkHttpClientModuleOptions {
    readonly imports: ModuleMetadata['imports'];
    readonly clients: readonly ZLinkNamedHttpClientOptions[];
}

export interface ZLinkServerHttpRequestBuilder extends ZLinkHttpRequestBuilder {
    submit(): void;
    yield<T>(): Promise<HttpResponse<T>>;
}

export interface ZLinkServerHttpClient extends Omit<ZLinkHttpClient, 'get' | 'post' | 'put' | 'delete' | 'patch' | 'head' | 'options'> {
    get(path: string): ZLinkServerHttpRequestBuilder;
    post(path: string): ZLinkServerHttpRequestBuilder;
    put(path: string): ZLinkServerHttpRequestBuilder;
    delete(path: string): ZLinkServerHttpRequestBuilder;
    patch(path: string): ZLinkServerHttpRequestBuilder;
    head(path: string): ZLinkServerHttpRequestBuilder;
    options(path: string): ZLinkServerHttpRequestBuilder;
}

export declare function zlinkHttpClientToken(name: string): InjectionToken;

export declare class ZLinkHttpClientModule {
    static forRoot(options: ZLinkHttpClientModuleOptions): DynamicModule;
}
```

`applicationHwmBytes(undefined)` is Auto mode, `0n` is no limit, and a
positive value is the exact host-wide byte cap. `applicationHwmProfile`'s
default is `Balanced`. If `processMemoryLimitBytes(undefined)`, it
checks the finite OS cap applied to the process, such as a
container/cgroup/Windows Job Object, and the V8 managed heap cap
(`heap_size_limit`). If both are confirmed, the smaller value is used;
if only one is confirmed, that value is used. If neither can be
confirmed, the system's total physical memory is used. If the computed
result isn't positive, startup fails with a configuration error before
socket bind. The application listener's default `maxMessageSize` is
`16_777_216` bytes.

The NestJS builder also only registers the Entry Spot implementation
type. The Entry Spot's `SpotId` is issued by the framework in the
format `<prefix>-entry-<lowercase-canonical-uuid-v4>`, and there's no
caller-specified identity option.

An Object Server with even one `RecreateOnRelocation` or
`PreserveStateWith` factory, or even one registered Instance Spot
[factory](../../../../01-glossary.en.md#factory), must call
`addRelocationStore(...)` exactly once. Only a same-node configuration
with no
[Instance Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
factory where every factory is `DisableRelocation` can omit the
Relocation Store. A missing or duplicate registration is a
configuration error before socket bind.

# Node.js NestJS host adapter 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Node.js 계약 목차](../README.ko.md)

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 NestJS host adapter 관련 정확한 TypeScript declaration을 고정한다.
동작 의미는 [공통 스펙](../../../README.ko.md)이 소유하며, 이 문서는 이름, generic, overload,
상속, member, parameter와 반환형만 정의한다.

## 1. Dynamic module과 DI token

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

## 2. Runtime provider와 Entry Spot decorator

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

## 3. Module, handler와 codec registration

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

## 4. NestJS host configuration

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
    sessionRelocationSealTimeoutMs(value: number): this;
    relocationCutoverWaitTimeoutMs(value: number): this;
    relocationPayloadChunkLimitBytes(value: number): this;
    relocationInFlightPayloadBudgetBytes(value: number): this;
    relocationNodeInFlightPayloadBudgetBytes(value: number): this;
}

export interface ZLinkNestFrameworkOptionsBuilder {
    options(options: ZLinkNestFrameworkAdditionalOptions): this;
    disableImplicitHandlerAutoRegistration(): this;
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

`disableImplicitHandlerAutoRegistration()`은 이 builder가 만드는 module에서 Nest provider metadata와
role root를 scan해 handler를 자동 등록하는 동작을 끈다. 기본값은 자동 등록이다. 이 method를 호출해도
`addRequestHandler(...)`, `addSendHandler(...)`, Session의 `context.handlers.addHandler(...)`와
Spot의 `context.handlers`에 직접 등록한 handler는 유지되며 Nest DI로 instance를 만든다. 자동 등록과
수동 등록을 sample 하나에서 명확히 구분해야 할 때만 module 전체에 적용한다.

## 5. MeshNode, Channel, Spot과 STREAM builder

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

## 6. Spot handler decorator

```ts
export declare function zlinkSendHandler(groupName: string, packetName?: string, options?: ZLinkNestHandlerOptions): ClassDecorator;

export declare function zlinkSpotActorRequestHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(options: ZLinkNestSpotActorRequestHandlerOptions<TSpot, TActor>): ClassDecorator;

export declare function zlinkSpotActorSendHandler<TSpot extends ZLinkSpot, TActor extends ZLinkActor>(options: ZLinkNestSpotActorSendHandlerOptions<TSpot, TActor>): ClassDecorator;

export declare function zlinkSpotPacketHandler<TSpot extends ZLinkSpot>(options: ZLinkNestSpotPacketHandlerOptions<TSpot>): ClassDecorator;

export declare function zlinkSpotSubscriptionHandler<TSpot extends ZLinkSpot>(options: ZLinkNestSpotSubscriptionHandlerOptions<TSpot>): ClassDecorator;

export declare function zlinkSpotTimerHandler<TSpot extends ZLinkSpot = ZLinkSpot>(options?: ZLinkNestSpotTimerHandlerOptions<TSpot>): ClassDecorator;
```

## 7. Server HTTP client integration

서버 handler에서 HTTP 요청을 실행할 때는 이름으로 등록한 client를 주입받는다. 등록한 client는 Nest
module의 수명과 함께 정리된다. `yield()`는 현재 handler가 `SpotWide` User Spot 또는 Instance Spot의
shared turn에서 실행 중일 때만 선택할 수 있다. 다른 handler에서는 HTTP operation을 제출하거나 turn을
반환하지 않고 `invalidConfiguration`으로 완료한다.

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

`configureDispatch()`가 반환하는 `ZLinkDispatchOptionsBuilder`의 `coreHwmMemoryLimitBytes`,
`coreHwmBudgetBytes`, `coreHwmProfile`은 Core에 전달한다. Node binding은
양수 유한 V8 `heap_size_limit`를 runtime memory hint로 전달한다. Core와 job queue profile은 기본값
`Balanced`인 독립된 enum과 계산이다. Manual job cap은 `1..2,147,483,647`이고 생략하면 common startup
CPU snapshot과 32/64/128/256 계수를 사용한다. Range 위반과 overflow는 bind 전에 실패하며 runtime 중
다시 계산하지 않는다.
Application listener의 `maxMessageSize` 기본값은 `16_777_216` bytes다.

NestJS builder도 Entry Spot 구현 type만 등록한다. Entry Spot의 `SpotId`는 Framework가
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식으로 발급하며 caller 지정 identity option은 없다.

`RecreateOnRelocation` 또는 `PreserveStateWith` factory가 하나라도 있거나 Instance Spot [factory](../../../00-foundation/02-glossary.ko.md#factory)가 하나라도 등록된 Object Server는
`addRelocationStore(...)`를 정확히 한 번 호출해야 한다. [Instance Spot](../../../00-foundation/02-glossary.ko.md#entry-spot-user-spot과-instance-spot) factory가 없고 모든 factory가
`DisableRelocation`인 same-node 구성만 Relocation Store를 생략할 수 있다. 누락과 중복은 socket bind 전에 configuration
error다. Relocation Store는 Actor·Spot relocation의 state·queue·timer handoff payload를 보관하지
않는다 — handoff payload는 source memory에서 target으로 직접 전송한다. Store는 Instance Spot cold
activation의 최초 message·생성 정보 기록과, relocation 뒤 완료되는 pending request의 terminal 결과
기록을 소유하므로 이 등록 요구는 그대로 유지된다.

`sessionRelocationSealTimeoutMs(value)`는 startup-only 양수 finite millisecond 값이고 기본값은
3,000이다. 0, 음수, `NaN`, infinity, 정수가 아닌 값과 safe-integer 범위 초과는 socket bind 전에
configuration error다.

`relocationCutoverWaitTimeoutMs(value)`는 startup-only 양수 finite millisecond 값이고 기본값은
1,000이다. Target이 relay 수신 준비 reply 뒤 cutover를 기다리는 시간이며, source가 boundary batch
재전송 사본을 유지하는 시간과 같다. 0, 음수, `NaN`, infinity, 정수가 아닌 값과 safe-integer 범위
초과는 socket bind 전에 configuration error다.

`relocationPayloadChunkLimitBytes(value)`는 relocation payload를 나눈 encoded chunk 하나의 크기
상한이고 기본값은 262,144(256 KiB)다. 0, transport가 협상한 frame 한도를 넘는 값, 음수, `NaN`,
infinity, 정수가 아닌 값과 safe-integer 범위 초과는 socket bind 전에 configuration error다.

`relocationInFlightPayloadBudgetBytes(value)`는 peer 연결 하나에 대해 동시에 전송 중인 relocation
chunk의 accounted byte 합계 상한이고 기본값은 16,777,216(16 MiB)이다.
`relocationNodeInFlightPayloadBudgetBytes(value)`는 node 전체에 같은 합계 상한을 적용하고 기본값은
0이다. 두 예산 모두 0은 예산을 적용하지 않는다는 뜻이다. 음수, `NaN`, infinity, 정수가 아닌 값과
safe-integer 범위 초과는 socket bind 전에 configuration error다. 세 크기 설정 모두 startup-only이며
runtime이 자동으로 조정하지 않는다.

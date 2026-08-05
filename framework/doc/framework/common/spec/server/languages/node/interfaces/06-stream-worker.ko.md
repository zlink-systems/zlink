# Node.js STREAM, timer와 worker 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Node.js 계약 목차](../README.ko.md)

이 문서는 ZLink Framework에서 `@zlink-systems/framework`와
`@zlink-systems/nestjs`가 내보내는 STREAM, timer와 worker 관련 정확한 TypeScript declaration을 고정한다.
동작 의미는 [공통 스펙](../../../../README.ko.md)이 소유하며, 이 문서는 이름, generic, overload,
상속, member, parameter와 반환형만 정의한다.

## 1. Spot handler와 STREAM

```ts
export interface ZLinkSpotPublisherClient {
    publish(channelName: string, topic: string, event: unknown): ZLinkPublishCall;
}

export declare function ZLinkSpotRequest(packetName?: string): MethodDecorator;

export interface ZLinkSpotRequestHandler<TSpot, TRequest, TReply> {
    handle(spot: TSpot, request: TRequest, context: ZLinkMessageContext): Promise<TReply>;
}

export declare function ZLinkSpotSubscription(channelName: string, topic: string): MethodDecorator;

export interface ZLinkSpotSubscriptionHandler<TSpot, TEvent> {
    handle(spot: TSpot, event: TEvent, context: ZLinkPublishMessageContext): Promise<void>;
}

export interface ZLinkSpotTimerHandler<TSpot> {
    handle(spot: TSpot, tick: ZLinkTimerTick): Promise<void>;
}

export interface ZLinkStream {
    readonly sessionId: string;
    readonly routingId?: RoutingId;
    readonly localAddr?: string;
    readonly remoteAddr?: string;
    write(payload: ZLinkMessage, flags?: number): boolean;
    close(signal?: AbortSignal): Promise<void>;
}
```

## 2. STREAM node, compression과 timer

```ts
export interface ZLinkStreamCompressionBuilder {
    useDefault(): this;
    useLz4(): this;
    use(codec: ZLinkStreamCompressionCodec): this;
    disable(): this;
}

export interface ZLinkStreamCompressionCodec {
    compress(payload: Uint8Array): Uint8Array;
    decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array;
}

export interface ZLinkStreamCompressionOptions {
    readonly disabled?: boolean;
    readonly codec?: ZLinkStreamCompressionCodec;
}

export interface ZLinkStreamError {
    readonly error: ZLinkStreamSessionError;
    readonly message?: string;
}

export interface ZLinkStreamNodeBuilder {
    bind(endpoint: string): this;
    bind(port?: number): this;
    setBindHost(bindHost: string): this;
    setAdvertiseHost(advertiseHost: string): this;
    configureSocket(): ZLinkStreamSocketConfig;
    enableActorDispatch(): this;
    setTlsServer(certificatePath: string, keyPath: string, requireClientCertificate?: boolean): this;
    registerSession<TSession extends ZLinkSession>(sessionType: Type<TSession> | Type<ZLinkSessionFactory<TSession>>): this;
}

export interface ZLinkStreamSocketConfig {
    maxMessageSize: number;
}

export declare function ZLinkStreamPacket(): MethodDecorator;

export declare function ZLinkStreamRaw(): MethodDecorator;

export declare enum ZLinkStreamSessionError {
    Internal = "internal",
    TransportError = "transportError"
}

export interface ZLinkTimer {
    readonly isDisposed: boolean;
    cancel(signal?: AbortSignal): Promise<void>;
    dispose(): Promise<void>;
}

export interface ZLinkTimerOptions {
    overrunPolicy?: ZLinkTimerOverrunPolicy;
    maxCatchUpTicks?: number;
    stopOnUnhandledException?: boolean;
}
```

`configureSocket().maxMessageSize`의 기본값은 `64 KiB`다. 이 상한은 StreamNode가
Core STREAM에서 client→server로 받는 complete message에만 적용하며, 크기는 6-byte
prefix를 제외한 header와 payload의 합으로 계산한다. `0`은 Framework 상한을 사용하지
않도록 Core의 `-1`로 변환하고, 음수는 startup configuration error다. 상한을 넘은
message는 session handler에 일부도 전달하지 않고 server에서 `EMSGSIZE`와 진단 trace를
남긴 뒤 연결을 종료한다. raw client는 별도 wire error code를 받지 않고 연결 종료를
관찰한다. server→client outbound에는 이 Framework 상한을 적용하지 않는다.

## 3. Timer scheduling과 worker

```ts
export declare enum ZLinkTimerOverrunPolicy {
    SkipLateTicks = "skipLateTicks",
    CatchUpBounded = "catchUpBounded",
    DelayNextTick = "delayNextTick"
}

export interface ZLinkTimerTick {
    readonly name: string;
    readonly deliveryIndex: bigint;
    readonly scheduledIndex: bigint;
    readonly periodMs: number;
    readonly scheduledAt: Date;
    readonly startedAt: Date;
    readonly scheduledElapsedMs: number;
    readonly startedElapsedMs: number;
    readonly delayMs: number;
    readonly skippedTicks: bigint;
}

export declare enum ZLinkUnhandledDispatchAction {
    ReplyError = "replyError",
    LogAndDrop = "logAndDrop",
    Drop = "drop",
    Throw = "throw"
}

export interface ZLinkUnhandledDispatchOptions {
    request: ZLinkUnhandledDispatchAction;
    send: ZLinkUnhandledDispatchAction;
    publish: ZLinkUnhandledDispatchAction;
}

export interface ZLinkWorkerCall<T> {
    timeoutMs(durationMs: number): ZLinkWorkerCall<T>;
    submit(signal?: AbortSignal): Promise<T>;
    yield(signal?: AbortSignal): Promise<T>;
}

export interface ZLinkWorkerOptions {
    readonly minThreads: number;
    readonly maxThreads: number;
    readonly idleTimeoutMs: number;
    readonly maxQueueLength: number;
}
```

Request의 result-bearing `submit()`은 terminal reply가 나올 때까지 현재 owner turn을 유지한다.
Actor Join은 별도의 `defer()`로 등록하고 현재 handler가 정상 종료한 뒤 실행한다.
Worker call의 `submit()`도 Worker 결과가 나올 때까지 현재 turn을 유지한다. `yield()`는 `SpotWide` User Spot
또는 Instance Spot의 shared turn에서만 그 turn을
반환한다. 다른 실행 문맥에서는 worker를 제출하거나 turn을 반환하지 않고 `invalidConfiguration`으로
완료한다.

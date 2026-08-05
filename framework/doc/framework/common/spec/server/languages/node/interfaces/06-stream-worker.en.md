# Node.js STREAM, Timer, And Worker Public Interface

[Interface table of contents](README.en.md) · [Node.js contract table of contents](../README.en.md)

This document fixes the exact TypeScript declarations related to
STREAM, timer, and worker that `@zlink-systems/framework` and
`@zlink-systems/nestjs` export in ZLink Framework. Behavioral meaning is
owned by the [common spec](../../../../README.en.md) — this document
only defines names, generics, overloads, inheritance, members,
parameters, and return types.

## 1. Spot Handler And STREAM

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

## 2. STREAM Node, Compression, And Timer

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

The default of `configureSocket().maxMessageSize` is `64 KiB`. The limit
applies only to complete messages received by a StreamNode from client to
server through Core STREAM. Its size is the header bytes plus payload bytes,
excluding the 6-byte prefix. `0` maps to Core `-1`, meaning that Framework
doesn't add a limit; a negative value is a startup configuration error. A
message over the limit is never partly delivered to the session handler. The
server records `EMSGSIZE` and a diagnostic trace, then closes the connection.
The raw client observes the close without a separate wire error code. The
Framework limit doesn't apply to server-to-client outbound messages.

## 3. Timer Scheduling And Worker

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

A request's result-bearing `submit()` keeps the current owner turn
until the terminal reply comes out. Actor Join is registered with a
separate `defer()` and runs after the current handler finishes
normally. A worker call's `submit()` also keeps the current turn until
the worker result comes out. `yield()` only returns the turn on a
`SpotWide` User Spot or Instance Spot's shared turn. In a different
execution context, it completes with `invalidConfiguration` without
submitting the worker or returning the turn.

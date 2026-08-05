<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:end -->

# TypeScript Stream Connector

> This document is the **TypeScript projection** of the
> [Stream Connector Common Spec](../../32-stream-connector.en.md).
> Transport/wire/lifecycle/error meaning is owned by the common spec,
> and this document fixes the **exact public surface** that meaning
> has in TypeScript.

The TypeScript connector is a browser client connector provided as the
`@zlink-systems/stream-connector` package.
It's a module separate from the server framework, letting client code
use request/reply, dispatch (`Manual`/`Immediate`), and a typed payload
API. JSON, MessagePack, Protobuf, or a custom codec is injected as one
`codec` option when building a connector. The typed
`send`/`request`/`on`/`waitFor` surface encodes/decodes a work DTO with
the injected codec.

## 1. Target Execution Environment

**The connector responsible per engine × build target is owned by
[Common Spec §2](../../32-stream-connector.en.md).** Per that
assignment, what the TypeScript connector is responsible for is the
**browser family** (web client, Unity WebGL, Cocos Creator web, Godot
Web). A Node.js process isn't the connector's product execution
environment.

**Every engine building for web (browser/WASM) uses this connector
regardless of language.** Unity WebGL uses the
`com.zlink.stream-connector.webgl` UPM source adapter, which provides
the npm package root's browser bundle and the jslib/C# call boundary.
This adapter doesn't provide a separate wire runtime.

## 2. Entrypoint

The public entrypoint is a single package root,
`@zlink-systems/stream-connector`. This entrypoint directly exports the
browser implementation using the platform `WebSocket` and the ESM type
declaration. A `/browser` subpath and a Node conditional export aren't
provided.

**Contract:**

- The package root's bundle graph **doesn't include a Node-only
  module such as `net`/`tls`/`Buffer`.** The verification scope is
  owned by §7's document.
- The common wire layer (`@zlink-systems/stream-wire`) runs as **the
  same code** in both runtimes. The browser ESM and server CommonJS
  deliverables use the same source and wire constants, and their
  `Uint8Array` byte fixtures must match.

## 3. Transport

The scheme → transport mapping follows
[Common Spec §3.1](../../32-stream-connector.en.md). The only
transport the TypeScript connector can use is **`ws` and `wss`.**

**If the package root receives a `tcp://`/`tls://` endpoint, it fails
immediately with `ZlinkStreamErrorCode.ConfigurationError`.** It
doesn't attempt to connect and silently fail at runtime.

In the browser, `ws`/`wss` are implemented with the **platform's
native `WebSocket`.** Since the browser performs the handshake and
framing, the connector doesn't implement it directly.

### 3.1 Transport Factory Injection

The `transportFactory` option is an extension point for putting in a
test double (in-memory transport) or a platform-specific transport.
The default is the platform `WebSocket` adapter. It isn't used as a
Node transport compatibility point.

## 4. Public Surface

The public type the package root exposes is below.

```ts
interface ZlinkStreamFlow {
  readonly flowId: string;
  readonly flowOrigin: ZlinkFlowOrigin;
}

interface ZlinkStreamConnector {
  readonly isConnected: boolean;
  readonly state: ZlinkStreamConnectionState;
  readonly closeReason?: ZlinkStreamCloseReason;
  readonly options: RequiredZlinkStreamConnectorOptions;
  readonly pendingDispatchCount: number;

  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  waitFor<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamSequenceCall<TPayload>;
  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;

  onErrorReceived(handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void): Disposable;
  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable;
  onConnectionStateChanged(
    handler: (change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
  observeInbound(
    observer: (observation: ZlinkStreamInboundObservation, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
}

interface ZlinkStreamSendCall {
  packetName(name: string): ZlinkStreamSendCall;
  metadata(key: string, value: string): ZlinkStreamSendCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamSendCall;
  compress(): ZlinkStreamSendCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamSendCall;
  submit(): Promise<void>;
}

interface ZlinkStreamRequestCall {
  packetName(name: string): ZlinkStreamRequestCall;
  metadata(key: string, value: string): ZlinkStreamRequestCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamRequestCall;
  timeout(timeoutMs: number): ZlinkStreamRequestCall;
  compress(): ZlinkStreamRequestCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamRequestCall;
  submit<TReply = unknown>(signal?: AbortSignal): Promise<TReply>;
  submitEncoded(signal?: AbortSignal): Promise<ZlinkStreamEncodedPayload>;
  submit(callback: (result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void): void;
}

interface ZlinkStreamWaitCall<TPayload = ZlinkStreamEncodedPayload> {
  where(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): ZlinkStreamWaitCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamWaitCall<TPayload>;
  submit(signal?: AbortSignal): Promise<ZlinkStreamMessage<TPayload>>;
}

interface ZlinkStreamExpectNoneCall<TPayload = ZlinkStreamEncodedPayload> {
  within(windowMs: number): ZlinkStreamExpectNoneCall<TPayload>;
  run(signal?: AbortSignal): Promise<void>;
}

interface ZlinkStreamSequenceCall<TPayload = ZlinkStreamEncodedPayload> {
  expect(predicate: (payload: TPayload) => boolean): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly TPayload[]>;
}

interface Disposable { dispose(): void; }

interface ZlinkStreamMetadata {
  readonly count: number;
  readonly values: ReadonlyMap<string, string>;
  get(key: string): string | undefined;
  with(key: string, value: string): ZlinkStreamMetadata;
  withMany(values: Iterable<readonly [string, string]>): ZlinkStreamMetadata;
}

interface ZlinkStreamEncodedPayload {
  readonly codec: ZlinkStreamCodec;
  readonly payload: Uint8Array;
  readonly messageType?: Function;
}

interface ZlinkStreamMessage<TPayload = unknown> extends ZlinkStreamFlow {
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly payload: TPayload;
}

interface ZlinkStreamError {
  readonly code: ZlinkStreamErrorCode;
  readonly message: string;
  readonly cause?: unknown;
}

interface ZlinkStreamResult { readonly isSuccess: boolean; readonly error?: ZlinkStreamError; }
interface ZlinkStreamResultOf<T> extends ZlinkStreamResult { readonly value?: T; }

interface ZlinkStreamConnectionStateChanged {
  readonly previous: ZlinkStreamConnectionState;
  readonly current: ZlinkStreamConnectionState;
  readonly error?: ZlinkStreamError;
}

interface ZlinkStreamInboundObservation {
  readonly kind: ZlinkStreamMessageKind;
  readonly name: string;
  readonly codec: ZlinkStreamCodec;
  readonly requestSeq?: bigint;
  readonly flowId?: string;
  readonly flowOrigin?: ZlinkFlowOrigin;
  readonly metadata: ZlinkStreamMetadata;
  readonly payloadLength: number;
  readonly isCompressed: boolean;
  readonly receivedAt: Date;
  readonly payloadPreview: Uint8Array;
}

enum ZlinkStreamCodec { Raw = 0, Json = 1, MessagePack = 2, Protobuf = 3 }
enum ZlinkStreamTransport { WebSocket = 'webSocket', WebSocketSecure = 'webSocketSecure' }
enum ZlinkStreamCompression { None = 'none', Lz4 = 'lz4' }
enum ZlinkStreamDispatchMode { Manual = 'manual', Immediate = 'immediate' }
enum ZlinkStreamMessageKind { Send = 1, Request = 2, Response = 3, Error = 4, Control = 5 }
enum ZlinkStreamHeaderFlags {
  None = 0, HasRequestSeq = 0x01, HasMetadata = 0x02,
  PayloadCompressed = 0x04, HasCorrelationId = 0x08, HasFlowId = 0x10
}
enum ZlinkStreamConnectionState {
  Created = 'created', Connecting = 'connecting', Connected = 'connected',
  Reconnecting = 'reconnecting', Disconnected = 'disconnected', Closed = 'closed'
}
enum ZlinkStreamErrorCode {
  Disconnected = 'disconnected', ConfigurationError = 'configurationError',
  ValidationFailed = 'validationFailed', RequestTimeout = 'requestTimeout',
  ConnectTimeout = 'connectTimeout', FrameDecodeFailed = 'frameDecodeFailed',
  FrameTooLarge = 'frameTooLarge', SendFailed = 'sendFailed',
  CompressionFailed = 'compressionFailed', DecompressionFailed = 'decompressionFailed',
  TlsValidationFailed = 'tlsValidationFailed',
  UserCallbackFailed = 'userCallbackFailed', ObserverFailed = 'observerFailed',
  ObserverDropped = 'observerDropped', ReceivedMessageDropped = 'receivedMessageDropped',
  RemoteError = 'remoteError'
}

type ZlinkFlowOrigin = 'Inbound' | 'Timer' | 'Application' | 'Lifecycle';
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';

class ZlinkStreamException extends Error {
  constructor(readonly error: ZlinkStreamError);
}
```

Connector options and the transport/codec extension point are also
part of the package root's public surface. The optional value the user
specifies and the required value the connector fills a default into
and exposes are each fixed with a different interface.

```ts
interface ZlinkStreamMeterProvider {
  getMeter(name: string): {
    createCounter(name: string, options?: { readonly unit?: string }): {
      add(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
    createHistogram(name: string, options?: { readonly unit?: string }): {
      record(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
  };
}

interface ZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly transport?: ZlinkStreamTransport;
  readonly transportFactory?: ZlinkStreamTransportFactory;
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
  readonly waitTimeoutMs?: number;
  readonly heartbeat?: ZlinkStreamHeartbeatOptions;
  readonly reconnect?: ZlinkStreamReconnectOptions;
  readonly maxSendPayloadSize?: number;
  readonly maxReceivePayloadSize?: number;
  readonly maxReceivedMessages?: number;
  readonly maxInboundObserverNotifications?: number;
  readonly maxInboundObserverPayloadPreviewBytes?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver;
  readonly meterProvider?: ZlinkStreamMeterProvider;
}

interface ZlinkStreamHeartbeatOptions {
  readonly enabled?: boolean;
  readonly intervalMs?: number;
  readonly timeoutMs?: number;
}

interface ZlinkStreamReconnectOptions {
  readonly enabled?: boolean;
  readonly initialDelayMs?: number;
  readonly maxDelayMs?: number;
  readonly backoffFactor?: number;
  readonly maxAttempts?: number;
}

interface ZlinkStreamPacketNameResolver { resolve(payloadType: Function): string; }
interface ZlinkStreamPayloadCodec {
  encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload;
  decode<T = unknown>(payload: ZlinkStreamEncodedPayload, messageType?: Function): T;
}
interface ZlinkStreamCompressionCodec {
  compress(payload: Uint8Array): Uint8Array;
  decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array;
}
interface ZlinkStreamConnection {
  write(frame: Uint8Array, signal?: AbortSignal): Promise<void>;
  read?(signal?: AbortSignal): Promise<Uint8Array | undefined>;
  close(signal?: AbortSignal): Promise<void>;
}
interface ZlinkStreamTransportFactory {
  connect(options: RequiredZlinkStreamConnectorOptions, signal?: AbortSignal): Promise<ZlinkStreamConnection>;
}

interface RequiredZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly transport: ZlinkStreamTransport;
  readonly connectTimeoutMs: number;
  readonly requestTimeoutMs: number;
  readonly waitTimeoutMs: number;
  readonly heartbeat: Required<ZlinkStreamHeartbeatOptions>;
  readonly reconnect: Required<ZlinkStreamReconnectOptions>;
  readonly maxSendPayloadSize: number;
  readonly maxReceivePayloadSize: number;
  readonly maxReceivedMessages: number;
  readonly maxInboundObserverNotifications: number;
  readonly maxInboundObserverPayloadPreviewBytes: number;
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly meterProvider?: ZlinkStreamMeterProvider;
}
```

A connector is created with `zlinkStreamConnectorFactory.create(options)`.

- **Cancellation is delivered through an optional `AbortSignal`.** It
  doesn't replicate another language's cancellation token shape
  ([Async Execution And Coroutine Policy](../../../05-async-execution-policy.ko.md)).
- **An event subscription returns a `Disposable`.** Dispose it with
  that `Disposable`.
- `send`/`request`/`waitFor` don't execute immediately — they
  **return a call builder.** Attach `packetName(...)`/`metadata(...)`/
  `timeout(...)`/`compress()` on the builder and then submit with
  `submit()`. `send`'s `submit()` doesn't wait for a response, and
  only delivers async completion and failure, without transport result
  or admission status.
- For an outbound triggered by an inbound handler, call
  `flowFrom(message)`. This method copies the message's `flowId` and
  `flowOrigin` as a pair. An outbound that doesn't call it starts a new
  flow with `origin=application`. For the detailed async-context
  boundary, follow
  [Flow Correlation §6](../../../27-flow-correlation.en.md#6-async-work-and-execution-context).

The default value of an option is owned by
[Common Spec §6.1](../../32-stream-connector.en.md). TypeScript
expresses this as a field of `ZlinkStreamConnectorOptions`, and exposes
the fully resolved value as `RequiredZlinkStreamConnectorOptions`.

### 4.1 Test Wait Surface

The contract is owned by
[Common Spec §10.2](../../32-stream-connector.en.md). The TypeScript
surface is below.

**Push observation — connector method** (the same spot as `waitFor`).
Each returns a builder.

```ts
waitFor<T>(name: string): ZlinkStreamWaitCall<T>;          // waits until it arrives
expectNone<T>(name: string): ZlinkStreamExpectNoneCall<T>; // whether it doesn't arrive during .within(ms)
waitForSequence<T>(name: string): ZlinkStreamSequenceCall<T>; // .expect(p).expect(p)… in order
```

- `await expectNone<T>(name).within(ms).run(signal)` — **throws** if it
  arrives within the window. The symmetric of `waitFor`.
- `await waitForSequence<T>(name).expect(p1).expect(p2)….timeout(ms).run(signal)` —
  confirms a push of the same name arrives **in predicate order**, and
  returns the payload array. Verifies **"arrived in order"**, not "N
  arrived."
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as
  `waitFor<T>(name).where(message => message.payload.status === …)`.

- **Domain REST polling isn't this surface.** That's the HTTP client's
  job.

## 5. Inbound Observer And The Receive Queue

Observation meaning and the isolation/overflow rule is owned by
[Common Spec §10](../../32-stream-connector.en.md). This document only
fixes the TypeScript surface.

`observeInbound(...)` returns a `Disposable`, and can be registered
**only before** calling `connect(...)`. Registering after the
connection starts throws an error.

The two queues' bounds are adjusted with the options below. The
default is owned by [Common Spec §6.1](../../32-stream-connector.en.md).

| Option | Target Queue | The Code Reported To The Error Handler On Overflow |
|---|---|---|
| `maxInboundObserverNotifications` | Observer notification queue | `ZlinkStreamErrorCode.ObserverDropped` |
| `maxReceivedMessages` | Receive message queue (§10.1) | `ZlinkStreamErrorCode.ReceivedMessageDropped` |

An observer callback failure is reported as
`ZlinkStreamErrorCode.ObserverFailed`.
`maxInboundObserverPayloadPreviewBytes` decides the payload preview
length put in an observation.

## 6. Session Close Reason

The reason's value set and meaning is owned by
[Common Spec §6.3](../../32-stream-connector.en.md#63-close-reason).
This document only fixes the TypeScript surface.

`ZlinkStreamCloseReason` is a closed union.

```ts
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';
```

**In TypeScript, this value is exposed as the connector's read-only
`closeReason` property.** Since the `onDisconnected(...)` handler
doesn't take the reason as an argument, `closeReason` is read inside
the handler. It's `undefined` if it hasn't disconnected yet.

### 6.1 Metric

The TypeScript connector publishes
[Common Spec §6.2](../../32-stream-connector.en.md#62-connector-reconnect-instrument)'s
`zlink.stream.reconnects` and its closed attribute to `meterProvider`.
If the provider is omitted, it uses the OpenTelemetry API's global
meter provider. The application and E2E read the counter with the
OpenTelemetry public reader. A provider or exporter failure doesn't
change send, request, or connection state.

## 7. Verification

The verification scope of common behavior is owned by
[Common Stream Connector Spec §12](../../32-stream-connector.en.md#12-regression-test).
The TypeScript surface must verify that item with the same meaning in
the browser's WS/WSS environment. This document fixes the public
TypeScript signature and browser execution environment.

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:bottom:end -->

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

The Unity WebGL C# adapter projects the bound Actor surface with the same
meaning.

```csharp
public partial class ZlinkStreamConnector
{
    public IReadOnlyList<ZlinkStreamActor> Actors { get; }
    public ZlinkStreamActor? Actor(string actorId);
    public IDisposable OnActorBound(Action<ZlinkStreamActor> handler);
    public IDisposable OnActorUnbound(Action<ZlinkStreamActor> handler);
}

public sealed class ZlinkStreamActor
{
    public string ActorId { get; }
    public bool IsBound { get; }
    public ZlinkStreamSendCall Send(object payload);
    public ZlinkStreamRequestCall Request(object payload);
    public IDisposable On<TPayload>(string name, Action<ZlinkStreamMessage<TPayload>> handler);
}
```

The jslib JSON boundary carries only `actorId` and the bound/unbound
lifecycle events. `actor_slot` stays inside the TypeScript wire runtime and
is never exposed as a public C# value.

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
  readonly diagnosticsLevel: ZlinkStreamDiagnosticsLevel;

  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;
  receivedCount(name: string): number;                                  // the received count per packet name (§5)
  setDiagnosticsLevel(level: ZlinkStreamDiagnosticsLevel): void;        // changes the value without waiting
  setDiagnosticsLevelAsync(level: ZlinkStreamDiagnosticsLevel): Promise<void>; // the async pair changing the same value

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  // carries both the path where the caller states the packet name and the one deciding it from the payload constructor.
  waitFor<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamSequenceCall<TPayload>;
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

  // The Actor handles bound right now (common spec §5.6). The application never creates one.
  readonly actors: readonly ZlinkStreamActor[];
  actor(actorId: string): ZlinkStreamActor | undefined;
  onActorBound(handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void): Disposable;
  onActorUnbound(handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void): Disposable;
}

interface ZlinkStreamActor {
  readonly actorId: string;
  readonly isBound: boolean;                    // false after the unbound announcement
  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;       // carries this Actor's slot
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;                                // only messages whose counterpart is this Actor
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
  // adds the next predicate to apply in arrival order. Its argument is the message, not the payload.
  expect(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly ZlinkStreamMessage<TPayload>[]>;
}

interface Disposable { dispose(): void; }   // dispose() deregisters; calling it twice isn't an error

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
  readonly actorId?: string;                   // the counterpart bound Actor; undefined for a frame without a slot (common spec §5.6)
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
  UserCallbackFailed = 'userCallbackFailed',
  RemoteError = 'remoteError'
}

type ZlinkFlowOrigin = 'Inbound' | 'Timer' | 'Application' | 'Lifecycle';
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';

class ZlinkStreamException extends Error {
  constructor(readonly error: ZlinkStreamError);   // error.code is where the code is read
}
```

**`TlsValidationFailed` does not occur in a browser runtime**, because the
browser's WebSocket API does not distinguish a TLS failure from an ordinary
connection failure. The code stays in the closed enum above and simply goes
unused there; the set itself does not change
([Common Spec §9.1](../../32-stream-connector.en.md#91-the-closed-error-code-set)).

TypeScript carries an error as a `ZlinkStreamError`, and a throwing
surface puts that value in a `ZlinkStreamException`
([Common Spec §9.2](../../32-stream-connector.en.md#92-delivery--the-receiver-must-be-able-to-read-the-code)).
A bare `Error` is never thrown, so the caller decides which of the 13
codes in
[Common Spec §9](../../32-stream-connector.en.md#9-error-meaning) it is
through the caught value's `error.code`. A callback terminator delivers
the same value as `ZlinkStreamResultOf<T>.error`.

The **means of attaching a packet name to a type** that
[Common Spec §5](../../32-stream-connector.en.md#5-packet-model)
requires is a static member. TypeScript has no attribute or annotation,
so the name goes on the payload constructor.

```ts
class OrderChanged {
  static readonly packetName = 'order.changed';   // the packet name attached to the type
}
```

The default `nameResolver` prioritizes this static member and uses the
constructor's `name` when it is absent. A name the caller states with
the builder's `packetName(...)` takes priority over both.

Connector options and the transport/codec extension point are also
part of the package root's public surface. The optional value the user
specifies and the required value the connector fills a default into
and exposes are each fixed with a different interface.

```ts
interface ZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly codec?: ZlinkStreamPayloadCodec;   // the typed payload codec injection point of common spec §5.4; JSON when omitted
  readonly transport?: ZlinkStreamTransport;
  readonly transportFactory?: ZlinkStreamTransportFactory;
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
  readonly waitTimeoutMs?: number;
  readonly heartbeat?: ZlinkStreamHeartbeatOptions;
  readonly reconnect?: ZlinkStreamReconnectOptions;
  readonly maxSendPayloadSize?: number;
  readonly maxReceivePayloadSize?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver; // the name resolver injection point of common spec §5.4
  readonly diagnosticsLevel?: ZlinkStreamDiagnosticsLevel; // initial value at construction, default Errors
}

// The contract is owned by common spec §13. Default Errors; unknown values are a
// configuration error. Off: outbound frames create no flow pair (0x10 not set), and
// inbound flow value validation/delivery is skipped (structural length check kept,
// ZlinkStreamMessage.flowId/flowOrigin are undefined).
// Runtime read/write is provided by the connector's `diagnosticsLevel` getter and
// `setDiagnosticsLevel(level)`, with `setDiagnosticsLevelAsync(level)` as the async pair
// changing the same value; the synchronous surface does not wait for the async pair to
// complete (common spec §13, server spec 26 §4.1). Unknown values
// passed to `setDiagnosticsLevel` are rejected with the same ConfigurationError as at
// construction, leaving the previous value in place. A change applies starting with
// processing points that read the level afterward and is never applied retroactively
// to frames already built. `options.diagnosticsLevel` always matches the current
// effective level.
enum ZlinkStreamDiagnosticsLevel {
  Off = 'off',
  Errors = 'errors',
  Normal = 'normal',
  Detailed = 'detailed',
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
  readonly maxAttempts?: number | null;   // null means unlimited; otherwise it must be positive (common spec §6)
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
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly diagnosticsLevel: ZlinkStreamDiagnosticsLevel;
}
```

A connector is created with `zlinkStreamConnectorFactory.create(options)`.
**`create(options)` checks every option**, and on a validation failure
it builds no connector and throws a `ZlinkStreamException`
([Common Spec §6.3](../../32-stream-connector.en.md#63-option-validation)).
A single value out of range carries `ValidationFailed`, and a mismatch
between options carries `ConfigurationError` — a conflict between the
endpoint scheme and `transport`, the `tcp`/`tls` the browser doesn't
support, and a `compressionCodec` given together with
`compression: none` fall into the latter.

- **Cancellation is delivered through an optional `AbortSignal`.** It
  doesn't replicate another language's cancellation token shape
  ([Async Execution And Coroutine Policy](../../../server/01-execution/README.en.md)).
- **`on(...)` and an event subscription return a `Disposable`.**
  Deregister with that `Disposable`'s `dispose()`; disposing the same
  value twice isn't treated as an error
  ([Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode)).
- `send`/`request`/`waitFor` don't execute immediately — they
  **return a call builder.** Attach `packetName(...)`/`metadata(...)`/
  `timeout(...)`/`compress()` on the builder and then submit with
  `submit()`. `send`'s `submit()` doesn't wait for a response, and
  only delivers async completion and failure, without transport result
  or admission status.
- A received message exposes `flowId` and `flowOrigin`
  ([Common Spec §5.5](../../32-stream-connector.en.md#55-flow-exposure-and-propagation)).
  Both are `undefined` when `diagnosticsLevel` is `Off`.
- For an outbound triggered by an inbound handler, call
  `flowFrom(message)`. This method copies the message's `flowId` and
  `flowOrigin` as a pair. An outbound that doesn't call it starts a new
  flow with `origin=application`. For the detailed async-context
  boundary, follow
  [Flow Correlation §6](../../../server/06-observability/04-flow-correlation.en.md#6-async-work-and-execution-context).
- **Stating the flow explicitly through `flowFrom(message)` is a
  browser JavaScript environment constraint.** The browser has no
  ambient execution context corresponding to `AsyncLocalStorage`, so
  the connector cannot hold the current flow in the context where it
  runs a handler
  ([Common Spec §2.2](../../32-stream-connector.en.md#22-the-effect-of-environment-constraint-on-the-contract),
  [§5.5](../../32-stream-connector.en.md#55-flow-exposure-and-propagation)).
  Explicit delivery and ambient propagation differ only in how the call
  is written — the flow value on the built frame is the same. The
  current flow is never guessed from a mutable field on the connector
  or a module-global variable.

The default value of an option is owned by
[Common Spec §6.1](../../32-stream-connector.en.md). TypeScript
expresses this as a field of `ZlinkStreamConnectorOptions`, and exposes
the fully resolved value as `RequiredZlinkStreamConnectorOptions`.

### 4.1 Test Wait Surface

The contract is owned by
[Common Spec §10.1](../../32-stream-connector.en.md). The TypeScript
surface is below.

**Push observation — connector method** (the same spot as `waitFor`).
Each returns a builder.

```ts
waitFor<T>(nameOrType: string | Function): ZlinkStreamWaitCall<T>;          // waits until it arrives
expectNone<T>(nameOrType: string | Function): ZlinkStreamExpectNoneCall<T>; // whether it doesn't arrive during .within(ms)
waitForSequence<T>(nameOrType: string | Function): ZlinkStreamSequenceCall<T>; // .expect(p).expect(p)… in order
```

- **Each surface carries both the path where the caller states the
  packet name and the one deciding it from the payload constructor.**
  Given a constructor, options' `nameResolver` decides the name. A
  TypeScript type does not survive to runtime, so the argument of the
  type-based path is the constructor value.
- `await expectNone<T>(name).within(ms).run(signal)` — **throws a
  `ZlinkStreamException` carrying `ValidationFailed`** if it arrives
  within the window. The symmetric of `waitFor`.
- `await waitForSequence<T>(name).expect(p1).expect(p2)….timeout(ms).run(signal)` —
  confirms a push of the same name arrives **in predicate order**, and
  returns a `ZlinkStreamMessage<T>` array. Verifies **"arrived in
  order"**, not "N arrived."
- **The predicate and the return value handle
  `ZlinkStreamMessage<T>`.** The argument `where(...)` and `expect(...)`
  receive is the message, not the payload.
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as
  `waitFor<T>(name).where(message => message.payload.status === …)`.

- **Domain REST polling isn't this surface.** That's the HTTP client's
  job.

## 5. Receive Queue

The receive queue's contract is owned by
[Common Spec §10](../../32-stream-connector.en.md). The TypeScript surface
carries no receive-queue option or error code.

**`receivedCount(name)` returns the received count per packet name** (§4).
Consuming does not lower it, it is independent of the dispatch mode, and it
restarts at zero when the connection is established.

## 6. Session Close Reason

The reason's value set and meaning is owned by
[Common Spec §6.2](../../32-stream-connector.en.md#62-close-reason).
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

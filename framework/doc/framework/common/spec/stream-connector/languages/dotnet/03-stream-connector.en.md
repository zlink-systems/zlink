<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md) | [Previous: .NET System Structure](../../../server/languages/dotnet/interfaces/02-configuration-host.en.md)
<!-- framework-adapter-nav:end -->

[.NET spec table of contents](../../../server/languages/dotnet/README.en.md)

# .NET Stream Connector Public Contract

> This document is the **`.NET` projection** of the
> [Stream Connector Common Spec](../../32-stream-connector.en.md).
> **The target execution environment, transport, wire contract, packet
> model, connection lifecycle, error meaning, and default value are
> owned by the common spec.** This document only fixes the **exact
> public surface** that meaning has in `.NET`.
>
> Usage is owned by the
> [.NET Stream Connector guide](../../../../../dotnet/guide/stream-connector/INDEX.en.md).

## 1. Package And Boundary

The public package is `Zlink.Stream.Connector`. **It doesn't
depend on the ASP.NET Core host, Spot, actor, or location runtime.**

The exact member list and deployment archive is owned by the fixed
snapshot.

- [API snapshot](../../../../../../../languages/dotnet/contract/api/Systems.Zlink.Stream.Connector.api.txt)
- [package snapshot](../../../../../../../languages/dotnet/contract/packages/Zlink.Stream.Connector.package.txt)

This document doesn't repeat listing the
[snapshot](../../../server/00-foundation/02-glossary.en.md#snapshot)'s member — it fixes the
**surface structure and `.NET`-specific meaning.** The verification
procedure is owned by [this document §13](#13-regression-test).

**The target it's responsible for is a native build** (desktop/server,
Unity, Godot C#). Unity's native build uses the same
`Zlink.Stream.Connector` NuGet package with no separate
package. **It isn't responsible for a web (browser/WASM) build**
([Common Spec §2](../../32-stream-connector.en.md)).

## 2. Entrypoint

```csharp
public static class ZlinkStreamConnectorFactory
{
    public static IZlinkStreamConnector Create(ZlinkStreamConnectorOptions options);
}
```

**The implementation type is hidden. The factory returns the public
interface.**

## 3. `IZlinkStreamConnector`

```csharp
public interface IZlinkStreamConnector : IAsyncDisposable
{
    bool IsConnected { get; }
    ZlinkStreamConnectionState State { get; }
    ZlinkStreamCloseReason? CloseReason { get; } // the last close reason; null if never disconnected (§10)
    ZlinkStreamConnectorOptions Options { get; }
    int PendingDispatchCount { get; }
    int ReceivedCount(string name);             // the received count per packet name (§8)

    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }

    IZlinkStreamSendCall     Send(ZlinkStreamEncodedPayload payload);
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
    IZlinkStreamWaitCall     WaitFor(string name);
    IZlinkStreamExpectNoneCall ExpectNone(string name);
    IZlinkStreamSequenceCall WaitForSequence(string name);
    IDisposable              On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);


    IDisposable OnRequestSending(Action<ZlinkStreamRequestSendingContext> handler);
    IDisposable OnReplyReceived(Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask> handler);
    IDisposable OnConnectionStateChanged(Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler);
    IDisposable OnDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler);
    IDisposable OnErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask> handler);

    IReadOnlyList<IZlinkStreamActor> Actors { get; }        // the Actor handles bound right now (§3.1)
    IZlinkStreamActor? Actor(string actorId);               // the open handle of that id, or null
    IDisposable OnActorBound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);
    IDisposable OnActorUnbound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);
}
```

### 3.1 `IZlinkStreamActor`

The Actor handle of [common spec §5.6](../../32-stream-connector.en.md#56-bound-actor).
The connector creates it from `$zlink.actor.bound` and closes it on
`$zlink.actor.unbound`; the application never creates one.

```csharp
public interface IZlinkStreamActor
{
    string ActorId { get; }
    bool IsBound { get; }                                    // false after the unbound announcement

    IZlinkStreamSendCall    Send(ZlinkStreamEncodedPayload payload);     // carries this Actor's slot
    IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload);
    IDisposable On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler); // only messages whose counterpart is this Actor
}
```

- The builders of `Send`/`Request` are the same `IZlinkStreamSendCall`/`IZlinkStreamRequestCall`
  as at the connector level (§4). `Async()`/`Submit(...)` on a handle with
  `IsBound == false` end in `ValidationFailed`.
- The connector-level `On(name, …)` receives every message regardless of
  Actor; a handle's `On` receives only the messages whose counterpart is that
  Actor. A handler registered on both runs on both.
- `OnActorBound`/`OnActorUnbound` follow the same `IDisposable` rule as the
  other registration surfaces.
- The typed surface (§5) provides the same extension methods
  (`Send<TPayload>`, `Request<TPayload>`, `On<TPayload>`) on `IZlinkStreamActor`.

- **The connection events are registration methods, not C#
  `event`s.** An `event` returns nothing at subscription time, so it does
  not satisfy common spec §7 — a client that manages subscriptions
  against the lifetime of one screen would have to keep the delegate it
  registered.
- **A handler is called in registration order.** A handler
  failure doesn't end the connector runtime — it's reported as a
  `UserCallbackFailed` error.
- `PendingDispatchCount` is a **value for diagnosing dispatch pump
  status.** **It isn't used for application flow control.**
- `ReceivedCount(name)` returns a count per packet name. Counting and reset follow
  [Common Spec §10](../../32-stream-connector.en.md#10-receive-message-queue).
- **Every registration surface returns an `IDisposable`**
  ([Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode)).
  `On(...)`, `OnConnectionStateChanged`, `OnDisconnected`, and
  `OnErrorReceived` are alike. Calling `Dispose()` twice isn't treated as an
  error.

`.NET` carries an error as a `ZlinkStreamError`, and a throwing surface
puts that value in a `ZlinkStreamException`
([Common Spec §9.2](../../32-stream-connector.en.md#92-delivery--the-receiver-must-be-able-to-read-the-code)).
The caller reads the error code through `ZlinkStreamException.Error.Code`.

```csharp
public sealed record ZlinkStreamError(
    ZlinkStreamErrorCode Code,      // the closed set of 13 codes in common spec §9
    string Message,
    Exception? Exception = null);   // the causing exception; null when there is none

public sealed class ZlinkStreamException(ZlinkStreamError error)
    : Exception(error.Message, error.Exception)
{
    public ZlinkStreamError Error { get; } = error; // where the code is read
}
```

## 4. Call Builder

**Packet name and metadata are owned by the operation builder, not the
payload object.**

```csharp
public interface IZlinkStreamLifecycleCall
{
    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamSendCall
{
    IZlinkStreamSendCall PacketName(string name);
    IZlinkStreamSendCall Metadata(string key, string value);
    IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata);
    IZlinkStreamSendCall Compress();
    ValueTask Async(CancellationToken cancellationToken = default); // only delivers async completion and failure.
}

public interface IZlinkStreamRequestCall
{
    IZlinkStreamRequestCall PacketName(string name);
    IZlinkStreamRequestCall Metadata(string key, string value);
    IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata);
    IZlinkStreamRequestCall Compress();
    IZlinkStreamRequestCall Timeout(TimeSpan timeout);
    ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default);
    void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback);
    void Submit(Action<ZlinkStreamResult> callback);
}

public interface IZlinkStreamWaitCall
{
    // Timeout(...), Where(...) decide this wait's bound and predicate.
    ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(CancellationToken cancellationToken = default);
}
```

- **`Send` is a one-way transmission that doesn't wait for a reply.**
  `Async()`'s completion value has no transport result or admission
  status — it only delivers async completion and failure (§6). Use
  `Request` if a response is needed.
- **`Timeout(...)` only applies to that operation.**
- **`On(...)` is a persistent push handler, and `WaitFor(...)` is a
  one-time wait.** Production push handling uses `On(...)`, and
  sample/CLI/E2E waiting uses `WaitFor(...)`.
- **`Metadata` is copied as an immutable snapshot at send time.**

### 4.1 Request Hooks

The two hooks of [Common Spec §5.7](../../32-stream-connector.en.md#57-request-hooks) are projected as `OnRequestSending`/`OnReplyReceived` (§3) with the contexts below. The sending hook does not follow the dispatch mode, so it is a synchronous `Action`; the reply hook has the same shape as other receive callbacks.

```csharp
public sealed class ZlinkStreamRequestSendingContext
{
    public string RequestPacketName { get; }
    public string? ActorId { get; }
    public void SetMetadata(string key, string value);
}

public sealed class ZlinkStreamReplyReceivedContext
{
    public string RequestPacketName { get; }
    public string? ActorId { get; }
    public bool Succeeded { get; }
    public ZlinkStreamMessage<ZlinkStreamEncodedPayload>? Reply { get; }
    public ZlinkStreamError? Error { get; }
    public TimeSpan Elapsed { get; }
}
```

## 5. Typed Surface

The two name forms of Common Spec §5 appear as an overload that takes a packet name and one that doesn't, on the typed `On` of the connector and Actor handle and on the connector's `WaitFor`/`ExpectNone`/`WaitForSequence`. Typed `Send`/`Request` name the packet explicitly through the returned builder's `PacketName(string)`.

`ZlinkStreamTypedConnectorExtensions` provides `Send<TPayload>`,
`Request<TPayload>`, `On<TPayload>`, `WaitFor<TPayload>`,
`ExpectNone<TPayload>`, `WaitForSequence<TPayload>`, each returning a
typed builder.

**`IZlinkStreamPacketNameResolver` decides packet identity.** The means
of attaching a packet name to a type that
[Common Spec §5](../../32-stream-connector.en.md#5-packet-model)
requires is an attribute. The default resolver prioritizes this
attribute, and uses the type name if it is absent.

```csharp
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class ZlinkStreamPacketNameAttribute(string name) : Attribute
{
    public string Name { get; } = name; // the packet name attached to the type
}
```

- **A per-operation `PacketName(...)` override is allowed.** For an
  already-encoded raw payload and external protocol interop. **This is
  a different role from the server framework's typed registration
  descriptor, and isn't a basis for re-exposing packet name at the
  server handler call site.**
- **Even after typed decode, the connector-internal buffer or mutable
  transport header isn't exposed.**
- **A raw header object isn't exposed in the public API.**

The codec surface is `IZlinkStreamPayloadCodec` and
`IZlinkStreamCompressionCodec`. `ZlinkStreamJsonCodec` is the default
payload codec, and specifying `CompressionCodec` uses that
implementation instead of the built-in one.

The two injection points of
[Common Spec §5.4](../../32-stream-connector.en.md#54-codec) are the
following properties of `ZlinkStreamConnectorOptions`.

```csharp
public IZlinkStreamPayloadCodec? PayloadCodec { get; init; }          // ZlinkStreamJsonCodec when null
public IZlinkStreamPacketNameResolver NameResolver { get; init; }
    = new ZlinkStreamPacketNameResolver();                            // the default resolver
```

If a Framework codec extension must also provide the STREAM header
value, it implements the Stream Connector package's
`IZlinkStreamCodecRegistration`. This descriptor only owns
STREAM-specific information. The common serializer registry doesn't
reference a STREAM enum or compression package.

```csharp
public interface IZlinkStreamCodecRegistration
{
    string ContentType { get; }
    ZlinkStreamCodec Codec { get; }
}
```

## 6. Lifecycle And Completion Meaning

**This is a `.NET`-specific contract.** The state transition itself is
owned by [Common Spec §6](../../32-stream-connector.en.md).

- `Connect.Async(...)` completes **once connection and receive-loop
  preparation finish.**
- `Close.Async(...)` **outside a callback** completes once connection
  close and the connector's internal cleanup (failing pending requests,
  removing registrations) finish. It does not wait for an asynchronous
  completion a user handler returned ([Common Spec §7](../../32-stream-connector.en.md)).
- `Close.Async(...)` **inside a callback** **returns immediately after
  starting close, to avoid a circular wait.** Afterward,
  `Close.Async(...)` outside a callback, or `DisposeAsync()`, waits for
  the shared terminal result.
- **A repeated `Close` and `DisposeAsync()` share the same terminal
  result or failure.**
- **Waiting for its own callback's close with `DisposeAsync()` inside
  a callback isn't allowed as a circular wait — it's treated as an
  immediate error.**
- **A lifecycle waiter's `CancellationToken` only cancels that
  waiter.** It doesn't cancel an already-started shared close work.
- **Once a frame write has started, caller cancellation doesn't create
  a partial frame.**

## 7. Dispatch

`.NET` expresses the `Manual` pump in [Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode)
as `Dispatch.Async(...)`. [Common Spec §5.2](../../32-stream-connector.en.md#52-request-correlation)
owns outbound admission, ordering, and timeouts.

## 8. Receive Message History

A message of the name an `On(...)` handler is registered for is not
kept in the unread history once dispatch takes over a handler snapshot.
A message of a name with no handler stays in the unread history and
`WaitFor(...)` consumes it one at a time. Control frames such as
response and heartbeat do not pass through this history.

### 8.1 Test Wait Surface

The contract is owned by
[Common Spec §10](../../32-stream-connector.en.md). The `.NET`
surface is below.

**Push observation — connector method** (the same spot as §4's
`WaitFor`). Each returns a typed builder.

```csharp
IZlinkStreamWaitCall       WaitFor(string name);        // waits until it arrives
IZlinkStreamExpectNoneCall ExpectNone(string name);     // whether it doesn't arrive during .Within(window)
IZlinkStreamSequenceCall   WaitForSequence(string name); // .Expect(p).Expect(p)… in order
```

The typed surface is an extension method on
`ZlinkStreamTypedConnectorExtensions`. Each surface carries **both
an overload that decides the packet name from `TPayload` and one where
the caller states it**
([Common Spec §10.1.1](../../32-stream-connector.en.md#1011-push-observation-surface--the-waitfor-family)).
With no name given, `Options.NameResolver` decides the name from
`typeof(TPayload)`.

```csharp
public static ZlinkStreamTypedWaitBuilder<TPayload>       WaitFor<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedWaitBuilder<TPayload>       WaitFor<TPayload>(this IZlinkStreamConnector connector, string name);
public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(this IZlinkStreamConnector connector, string name);
public static ZlinkStreamTypedSequenceBuilder<TPayload>   WaitForSequence<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedSequenceBuilder<TPayload>   WaitForSequence<TPayload>(this IZlinkStreamConnector connector, string name);
```

The typed builder for negative observation and order verification
fixes the public interface below.

```csharp
public sealed class ZlinkStreamTypedExpectNoneBuilder<TPayload>
{
    // decides the observation window this packet must not arrive within.
    public ZlinkStreamTypedExpectNoneBuilder<TPayload> Within(TimeSpan window);
    public ValueTask Async(CancellationToken cancellationToken = default);
}

public sealed class ZlinkStreamTypedSequenceBuilder<TPayload>
{
    // adds the next typed predicate to apply in arrival order.
    public ZlinkStreamTypedSequenceBuilder<TPayload> Expect(
        Func<ZlinkStreamMessage<TPayload>, bool> predicate);
    public ZlinkStreamTypedSequenceBuilder<TPayload> Timeout(TimeSpan timeout);
    public ValueTask<IReadOnlyList<ZlinkStreamMessage<TPayload>>> Async(
        CancellationToken cancellationToken = default);
}
```

- `ExpectNone(name).Within(TimeSpan).Async(ct)` — **throws a
  `ZlinkStreamException` carrying `ValidationFailed`** if it arrives
  within the window. The symmetric of `WaitFor`.
- `WaitForSequence(name).Expect(p1).Expect(p2)…Timeout(t).Async(ct)` returns
  `IReadOnlyList<ZlinkStreamMessage<TPayload>>`; sequence observation and failure
  follow [Common Spec §10.1](../../32-stream-connector.en.md#101-test-wait-surface).
- **The predicate and the return value handle
  `ZlinkStreamMessage<TPayload>`.** The argument `Where(...)` and
  `Expect(...)` receive is the message, not the payload.
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as `WaitFor<T>(name).Where(p => p.Status == …)`.
  The connector doesn't know which field is status.

- **Domain REST polling (`GET /deliveries/{id}`, etc.) isn't this
  surface.** That's `ZLinkHttpClient`'s job.

## 9. Transport And TLS

The scheme → transport mapping is owned by
[Common Spec §3.1](../../32-stream-connector.en.md). `.NET` expresses
this as the `ZlinkStreamTransport` enum (`Tcp`, `Tls`, `WebSocket`,
`WebSocketSecure`).

- **The nullable `Transport` option isn't the path that picks the
  transport.** It's an **auxiliary value** confirming the URI scheme
  matches the configuration, and fails with `ConfigurationError` if
  mismatched.
- **TLS and WSS validate the certificate chain and host name by
  default.** `SkipServerCertificateValidation` defaults to `false` and
  is used **only for a test's self-signed certificate.**

## 10. Close Reason

The value set and meaning is owned by
[Common Spec §6.2](../../32-stream-connector.en.md#62-close-reason).
`.NET` expresses this as the `ZlinkStreamCloseReason` enum.

**The read surface is the `IZlinkStreamConnector.CloseReason`
property** (§3). Its type is `ZlinkStreamCloseReason?`, and it is `null`
when the connector has never disconnected. The `Disconnected` event's
argument `ZlinkStreamDisconnected.CloseReason` is a surface added on top
of that property.

**The `session-closing` frame's wire value is 1-6, and the `.NET`
enum's internal ordinal is 0-5.** Since the codec explicitly converts
between the two, **the enum isn't cast to an integer and used as the
wire value.**

Whether a receive bound violation is terminal, the close reason, and
the reconnect condition is owned by
[Common Spec §9](../../32-stream-connector.en.md#9-error-meaning).
`.NET` expresses that error as `ZlinkStreamErrorCode.FrameTooLarge`,
and the close reason as `ZlinkStreamCloseReason.TransportError`.

## 11. Flow

The connector never creates, sends, exposes, or propagates flow ([Common Spec §5.5](../../32-stream-connector.en.md#55-flow)). A received message additionally exposes only the Actor identity.

```csharp
public sealed record ZlinkStreamMessage<TPayload>(
    string Name,
    ZlinkStreamMetadata Metadata,
    TPayload Payload,
    string? ActorId = null);
```

## 12. Options And Validation

**The default value is owned by
[Common Spec §6.1](../../32-stream-connector.en.md).** `.NET`
expresses this as a property of `ZlinkStreamConnectorOptions`
(+ `ZlinkStreamHeartbeatOptions`, `ZlinkStreamReconnectOptions`).

The **unlimited reconnect** that
[Common Spec §6](../../32-stream-connector.en.md#6-connection-lifecycle)
requires is expressed as `null` on a nullable `int`.

```csharp
public int? MaxAttempts { get; init; } = 3; // null means unlimited
```

`ZlinkStreamConnectorFactory.Create(options)` rejects an option that fails
[Common Spec §6.3](../../32-stream-connector.en.md#63-option-validation) before creating the connector
and throws `ZlinkStreamException` carrying the corresponding error code.

## 13. Regression Test

| Test Case | Verification Standard |
|---------------|-----------|
| `StreamConnectorTests.ConnectorImplementationIsHiddenBehindPublicInterface` | The implementation type is hidden, and the [factory](../../../server/00-foundation/02-glossary.en.md#factory) returns the public interface. |
| `StreamConnectorTests.ConnectorCallInterfacesMatchTheFrozenSurface` | Fixes the exact member of the lifecycle, send, request, and wait call. |
| `StreamConnectorTests.ConnectorOptionsMatchTheFrozenDefaults` | Fixes the connector option's default value. |
| `StreamConnectorTests.ManualDispatchRunsHandlerOnDispatchCaller` | The Manual callback runs on the dispatch caller. |
| `StreamConnectorTests.ImmediateDispatchRunsHandlerWithoutManualDispatch` | The Immediate callback runs with no separate manual dispatch. |
| `StreamConnectorTests.ManualRequestCallbackAdmission_Is_Bounded_And_Never_Falls_Back_To_A_Background_Thread` | Request callback admission is bounded and doesn't allow a background bypass. |
| `StreamConnectorTests.RequestTimeoutRemovesPendingRequest` | Removes the pending request after timeout. |
| `StreamConnectorTests.TcpTypedRequestCorrelatesResponse` | Keeps typed request and response correlation. |
| `StreamConnectorTests.TypedConnectorUsesJsonByDefaultAndDecodeReply` | The typed default codec is JSON. |
| `StreamConnectorTests.PacketNameAttributeIsUsedByDefault` | Uses the [packet name](../../../server/00-foundation/02-glossary.en.md#packet-name) attribute as the default identity. |
| `StreamConnectorTests.DisconnectEventCarriesTheFrozenCloseReasonContract` | Fixes the disconnect event's closed close reason. |
| `StreamConnectorTests.SessionClosingPublishesServerDrainReasonAfterDisconnectedState` | Converts a session-closing frame to the `ServerDrain` reason. |
| `StreamConnectorTests.SharedCloseFaultIsObservedByRepeatedCloseAndDispose` | Repeated close and dispose observe the same failure. |
| `StreamConnectorTests.OneWayAsync_Waits_For_Bounded_Queue_Admission` | The one-way terminal waits asynchronously up to bounded queue acceptance and completes with no result value. |
| `StreamConnectorTests.RequestQueueWaitsForEarlierAcceptedOneWaySend` | Preserves the wire send order of an earlier-accepted one-way send and a later request. |
| `StreamConnectorTests.CallerCancellationDoesNotInterruptAnInProgressFrameWrite` | Once a frame write starts, caller cancellation doesn't create a partial frame. |
| `StreamConnectorTests.ConnectorOutboundFramesNeverCarryFlowAndOnlyRequestsCarryCorrelation` | Connector outbound frames omit the flow flag and only requests carry correlation IDs. |
| `StreamConnectorTests.RequestHooksRunInOrderAddWireMetadataAndIsolateFailures` | Checks hook registration order, wire metadata, and callback failure isolation. |
| `StreamConnectorTests.ReplyHookObservesRemoteFailureTimeoutAndClose` | Checks remote-error, timeout, and close outcomes observed by the reply hook. |
| `StreamConnectorTests.ManualSendingHookRunsOnRequestCallerAndReplyHookWaitsForDispatch` | In Manual mode the sending hook runs on the request caller and puts metadata on the wire without dispatch; the reply hook waits for dispatch. |
| `StreamConnectorTests.ActorHandlersIsolateMatchingNamesAndRequestHooksReportActorId` | Actor handlers isolate messages under the same name and hooks receive the Actor ID. |
| `StreamConnectorTests.HeaderProtocolEnforcesControlPacketContract` | Fixes a control packet's codec/flag/payload contract. |

Release verification confirms with `scripts/verify_packaged_contract.sh`
whether the source assembly, API snapshot, actual NuGet package, and a
clean consumer all have the same public contract.

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:bottom:end -->

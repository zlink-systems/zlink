<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md) | [Previous: .NET System Structure](../../../server/languages/dotnet/01-system-structure.en.md)
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

The public package is `Systems.Zlink.Stream.Connector`. **It doesn't
depend on the ASP.NET Core host, Spot, actor, or location runtime.**

The exact member list and deployment archive is owned by the fixed
snapshot.

- [API snapshot](../../../../../../../languages/dotnet/contract/api/Systems.Zlink.Stream.Connector.api.txt)
- [package snapshot](../../../../../../../languages/dotnet/contract/packages/Systems.Zlink.Stream.Connector.package.txt)

This document doesn't repeat listing the
[snapshot](../../../01-glossary.en.md#snapshot)'s member — it fixes the
**surface structure and `.NET`-specific meaning.** The verification
procedure is owned by [this document §15](#15-regression-test).

**The target it's responsible for is a native build** (desktop/server,
Unity, Godot C#). Unity's native build uses the same
`Systems.Zlink.Stream.Connector` NuGet package with no separate
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
    ZlinkStreamConnectorOptions Options { get; }
    int PendingDispatchCount { get; }

    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }

    IZlinkStreamSendCall     Send(ZlinkStreamEncodedPayload payload);
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
    IZlinkStreamWaitCall     WaitFor(string name);
    IZlinkStreamExpectNoneCall ExpectNone(string name);
    IZlinkStreamSequenceCall WaitForSequence(string name);
    IDisposable              On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);

    IDisposable ObserveInbound(Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer);

    event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? ConnectionStateChanged;
    event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>?           Disconnected;
    event Func<ZlinkStreamError, CancellationToken, ValueTask>?                  ErrorReceived;
}
```

- **An event handler is called in registration order.** A handler
  failure doesn't end the connector runtime — it's reported as a
  `UserCallbackFailed` error.
- `PendingDispatchCount` is a **value for diagnosing dispatch pump
  status.** **It isn't used for application flow control.**

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

## 5. Typed Surface

`ZlinkStreamTypedConnectorExtensions` provides `Send<TPayload>`,
`Request<TPayload>`, `On<TPayload>`, `WaitFor<TPayload>`,
`ExpectNone<TPayload>`, `WaitForSequence<TPayload>`, each returning a
typed builder.

**`IZlinkStreamPacketNameResolver` decides packet identity.** The
default resolver prioritizes `ZlinkStreamPacketNameAttribute`, and uses
the type name if the attribute is absent.

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
  close and terminal callback cleanup finish.
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

## 7. Dispatch And Bounded Admission

**This is a `.NET`-specific contract.**

| Item | Contract |
|---|---|
| `Manual` (default) | A receive callback/request callback/lifecycle event is processed in the **execution context that called `Dispatch.Async(...)`** |
| `Immediate` | **Runs inline on the receive path** (no separate dispatch work). A slow handler blocks the receive loop, so backpressure applies as is |
| `MaxPendingDispatchCallbacks` | **Applies only in `Manual`.** This bound includes not just a receive handler, but also the reserved slot preserving the completion callback of an already-accepted request. `Immediate` bypasses this bounded admission since it doesn't go through the queue |
| Outbound send queue | An order-preserving queue **separate** from the dispatch bound. Holds at most **4096** sends, and rejects with an **immediate error** on overflow |

- **A send accepted earlier is sent before a request started later.**
  A request waits for the response **only after its own frame's actual
  write finishes.**
- **A send doesn't route around callback execution on a background
  thread.**

## 8. Receive Message History

The unread receive history `WaitFor(...)` uses is bounded by
`MaxReceivedMessages`. **This bound doesn't block processing of a
control frame such as response and heartbeat.**

A message of the name an `On(...)` handler is registered for also
passes through the common receive message queue's admission. Once
dispatch takes over a handler snapshot, it isn't kept in the unread
history. A message of a name with no handler stays in the unread
history and `WaitFor(...)` consumes it one at a time. So
`MaxReceivedMessages` bounds both the pre-dispatch wait and the unread
history together. Since the inbound observer is an observation path
separate from this selection, it receives the frame snapshot in both
cases.

**If the queue is full, a newly arrived message is rejected and
`ReceivedMessageDropped` is reported**
([Common Spec §10.1](../../32-stream-connector.en.md)).

### 8.1 Test Wait Surface

The contract is owned by
[Common Spec §10.2](../../32-stream-connector.en.md). The `.NET`
surface is below.

**Push observation — connector method** (the same spot as §4's
`WaitFor`). Each returns a typed builder.

```csharp
IZlinkStreamWaitCall       WaitFor(string name);        // waits until it arrives
IZlinkStreamExpectNoneCall ExpectNone(string name);     // whether it doesn't arrive during .Within(window)
IZlinkStreamSequenceCall   WaitForSequence(string name); // .Expect(p).Expect(p)… in order

// typed: ZlinkStreamTypedConnectorExtensions provides WaitFor<T>/ExpectNone<T>/WaitForSequence<T>
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

- `ExpectNone(name).Within(TimeSpan).Async(ct)` — **throws an error**
  if it arrives within the window. The symmetric of `WaitFor`.
- `WaitForSequence(name).Expect(p1).Expect(p2)…Timeout(t).Async(ct)` —
  confirms a push of the same name arrives **in predicate order**, and
  returns the payload list. Verifies **"arrived in order"**, not "N
  arrived."
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as `WaitFor<T>(name).Where(p => p.Status == …)`.
  The connector doesn't know which field is status.

- **Domain REST polling (`GET /deliveries/{id}`, etc.) isn't this
  surface.** That's `ZLinkHttpClient`'s job.

## 9. Inbound Observer

Observation meaning and the isolation/overflow rule is owned by
[Common Spec §10](../../32-stream-connector.en.md). The `.NET`
surface's constraint is below.

- `ObserveInbound(...)` is registered **only before connection
  starts** and returns `IDisposable`.
- **Don't call the connector's send/request/wait/dispatch from an
  observer callback.**
- **The observer can't drop/transform/reply to a frame.**
- **`DisposeAsync()` ignores cancellation and waits until a running
  observer finishes.**

## 10. Transport And TLS

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

## 11. Close Reason

The value set and meaning is owned by
[Common Spec §6.3](../../32-stream-connector.en.md#63-close-reason).
`.NET` expresses this as the `ZlinkStreamCloseReason` enum and
**exposes it as the `Disconnected` event's argument
`ZlinkStreamDisconnected.CloseReason`.**

**The `session-closing` frame's wire value is 1-6, and the `.NET`
enum's internal ordinal is 0-5.** Since the codec explicitly converts
between the two, **the enum isn't cast to an integer and used as the
wire value.**

Whether a receive bound violation is terminal, the close reason, and
the reconnect condition is owned by
[Common Spec §9](../../32-stream-connector.en.md#9-error-meaning).
`.NET` expresses that error as `ZlinkStreamErrorCode.FrameTooLarge`,
and the close reason as `ZlinkStreamCloseReason.TransportError`.

## 12. Flow

**A connector outbound operation generates a UUIDv7 `flow_id` once,
with no separate public option.** A follow-up operation started inside
a callback **reuses the current inbound flow, and once the callback
ends, cleans up the ambient flow.**

The wire representation is owned by
[Common Spec §4.2](../../32-stream-connector.en.md) and
[flow-correlation](../../../27-flow-correlation.en.md).

## 13. Metric

The connector metric follows
[Stream Connector Common Contract §6.2](../../32-stream-connector.en.md#62-connector-reconnect-instrument)'s
name and closed label. The `.NET` connector publishes
`zlink.stream.reconnects` to the `System.Diagnostics.Metrics`
provider, and the application and E2E read it with `MeterListener`.
**A metric listener failure doesn't change the send/request result or
connection state.**

## 14. Options And Validation

**The default value is owned by
[Common Spec §6.1](../../32-stream-connector.en.md).** `.NET`
expresses this as a property of `ZlinkStreamConnectorOptions`
(+ `ZlinkStreamHeartbeatOptions`, `ZlinkStreamReconnectOptions`).

The common contract's `MaxInboundObserverPayloadPreviewBytes` bounds
the payload preview length in bytes, defaulting to 0. `.NET` projects
this common option as a property of the same name.

**`.NET`-only option:**

| Option | Default | Meaning |
|---|---|---|
| `MaxPendingDispatchCallbacks` | 1024 | The dispatch pending callback bound (§7) |

**Validation contract:**

| Violation | Failure |
|---|---|
| No endpoint | `ArgumentException` |
| Unsupported scheme, URI scheme/`Transport` mismatch | `ZlinkStreamException`'s `ConfigurationError` **before starting connection** |
| An invalid timeout/queue size/heartbeat/reconnect combination | `ValidationFailed` |

Every timeout and queue size option must be **positive**, and the
preview length **can't be negative.**

## 15. Regression Test

| Test Case | Verification Standard |
|---------------|-----------|
| `StreamConnectorTests.ConnectorImplementationIsHiddenBehindPublicInterface` | The implementation type is hidden, and the [factory](../../../01-glossary.en.md#factory) returns the public interface. |
| `StreamConnectorTests.ConnectorCallInterfacesMatchTheFrozenSurface` | Fixes the exact member of the lifecycle, send, request, and wait call. |
| `StreamConnectorTests.ConnectorOptionsMatchTheFrozenDefaults` | Fixes the connector option's default value. |
| `StreamConnectorTests.ManualDispatchRunsHandlerOnDispatchCaller` | The Manual callback runs on the dispatch caller. |
| `StreamConnectorTests.ImmediateDispatchRunsHandlerWithoutManualDispatch` | The Immediate callback runs with no separate manual dispatch. |
| `StreamConnectorTests.ManualRequestCallbackAdmission_Is_Bounded_And_Never_Falls_Back_To_A_Background_Thread` | Request callback admission is bounded and doesn't allow a background bypass. |
| `StreamConnectorTests.RequestTimeoutRemovesPendingRequest` | Removes the pending request after timeout. |
| `StreamConnectorTests.TcpTypedRequestCorrelatesResponse` | Keeps typed request and response correlation. |
| `StreamConnectorTests.TypedConnectorUsesJsonByDefaultAndDecodeReply` | The typed default codec is JSON. |
| `StreamConnectorTests.PacketNameAttributeIsUsedByDefault` | Uses the [packet name](../../../01-glossary.en.md#packet-name) attribute as the default identity. |
| `StreamConnectorTests.DisconnectEventCarriesTheFrozenCloseReasonContract` | Fixes the disconnect event's closed close reason. |
| `StreamConnectorTests.SessionClosingPublishesServerDrainReasonAfterDisconnectedState` | Converts a session-closing frame to the `ServerDrain` reason. |
| `StreamConnectorTests.SharedCloseFaultIsObservedByRepeatedCloseAndDispose` | Repeated close and dispose observe the same failure. |
| `StreamConnectorTests.OneWayAsync_Waits_For_Bounded_Queue_Admission` | The one-way terminal waits asynchronously up to bounded queue acceptance and completes with no result value. |
| `StreamConnectorTests.RequestQueueWaitsForEarlierAcceptedOneWaySend` | Preserves the wire send order of an earlier-accepted one-way send and a later request. |
| `StreamConnectorTests.CallerCancellationDoesNotInterruptAnInProgressFrameWrite` | Once a frame write starts, caller cancellation doesn't create a partial frame. |
| `StreamConnectorTests.InboundObserverRegistrationIsRejectedAfterConnectAndStopsAfterDispose` | Fixes the observer registration time and deregistration meaning. |
| `StreamConnectorTests.Dispose_Waits_For_Cancellation_Ignoring_Inbound_Observer` | Dispose waits for the observer to end, ignoring cancellation. |
| `StreamConnectorTests.InboundObserverFailureReportsObserverFailedAndMessageStillDispatches` | Reports the observer failure while continuing to process the original message. |
| `StreamConnectorTests.InboundObserverOverflowReportsObserverDroppedAndRequestStillCompletes` | Observer overflow doesn't block request completion. |
| `StreamConnectorTests.OutboundFrameCreatesFlowOnceAndCodecRemainsDeterministic` | Generates the outbound flow once and fixes the header codec result. |
| `StreamConnectorTests.HeaderProtocolEnforcesControlPacketContract` | Fixes a control packet's codec/flag/payload contract. |

Release verification confirms with `scripts/verify_packaged_contract.sh`
whether the source assembly, API snapshot, actual NuGet package, and a
clean consumer all have the same public contract.

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:bottom:end -->

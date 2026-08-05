# Stream Connector — Common Spec

[Spec table of contents](../README.en.md) | [Previous: Session Actor Dispatch](../20-session-actor-dispatch.en.md) | [Next: Location Runtime](../21-location-runtime.en.md)

> This document is the **language-neutral canonical document of the
> client stream connector**. It owns the target execution environment,
> transport, wire contract, packet (a transport unit combining header
> information and payload) model, connection lifecycle, error meaning,
> and deployment deliverable.
>
> The per-language public type and signature is fixed by
> [`languages/<lang>/`](README.en.md) —
> [cpp](languages/cpp/03-stream-connector.en.md) ·
> [dotnet](languages/dotnet/03-stream-connector.en.md) ·
> [java](languages/java/03-stream-connector.en.md) ·
> [typescript](languages/typescript/README.en.md). This document
> defines **what is guaranteed**, and the per-language spec defines
> **what shape that meaning takes in that language**
> ([Public Contract Governance](../00-public-contract-governance.en.md)).

## 1. Purpose And Scope

Stream Connector is the **client-side library that connects to the
server framework's STREAM model**. It lets the client send and receive
the same
[packet](../01-glossary.en.md#stream-packet) (header + payload) the
server session callback receives.

The [Connector](../01-glossary.en.md#stream-connector) doesn't include
a domain. The user composes their own protocol on top of it, such as
chat, game, equipment control, or notification.

**Dependency boundary:**

- The connector package **doesn't depend on the server framework
  package** (ASP.NET Core adapter, SPOT, Stage wrapper, etc.).
- The dependency is limited only to client-side runtime needed for
  connector execution, such as transport/codec/compression.
- The reverse direction is the same. The server framework package
  doesn't reference the connector.

## 2. Target Execution Environment

**This section is the starting point of this spec.** Because the
execution environment's constraint decides the contract. Which
connector to use is decided **not by language, but by "engine ×
build target".**

### 2.1 Connector Responsible Per Engine/Environment

| Target | Native Build | Web Build (Browser · WASM) |
|---|---|---|
| **Unity** | `.NET` connector | **TypeScript** connector — C# calls the JS layer through jslib interop |
| **Godot** | C++ connector (GDExtension) or `.NET` connector (Godot C#) | **TypeScript** connector |
| **Cocos** | C++ connector (Axmol adapter) | **TypeScript** connector (Cocos Creator web) |
| **Unreal** | C++ connector (plugin) | (not applicable) |
| **Browser web client** | — | **TypeScript** connector |
| **Desktop/server application** | `.NET` / Java / C++ connector | — |

**Summarized in one rule — the moment you build for web (browser/WASM),
you use the TypeScript connector regardless of language.** Because no
language can open an OS socket in a browser sandbox.

### 2.2 The Effect Of Environment Constraint On The Contract

| Environment | Constraint | Contract |
|---|---|---|
| Game engine (common) | An engine object can't be handled off the main thread | The default of dispatch mode, which decides the receive callback's execution context, is **`Manual`**. It's explicitly pumped on the main thread (§7). |
| Game engine (C++) | Some builds have exception/coroutine disabled | The C++ connector core is **no-exception/no-coroutine**. The public header doesn't expose `<coroutine>` |
| **Browser · WASM** | **Can't open an OS socket** (security sandbox) | **`tcp`/`tls` unusable.** Only `ws`/`wss` are used, running on top of the platform's native WebSocket API (§3.2) |
| Node.js | Not the TypeScript connector's product execution environment | Only handles the server process and browser test runner |

## 3. Transport

### 3.1 Endpoint Scheme → Transport

| URI Scheme | Transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

If transport is specified but doesn't match the endpoint scheme, it's
treated as a **configuration error**.

### 3.2 Per-Environment Transport Availability

| Environment | Available Transport |
|---|---|
| **Browser family** (web, Cocos web, Unity WebGL, Godot Web) | **Only `ws`, `wss`** |
| Native (`.NET`/C++/Java) | `tcp`, `tls`, `ws`, `wss` |

**If the browser family receives a `tcp://`/`tls://` endpoint, it
fails immediately with a configuration error.** It doesn't silently
fail at runtime. This is a platform constraint, not an implementation
constraint.

In the browser family, `ws`/`wss` are implemented with the **platform's
native WebSocket API.** Since the platform performs the handshake and
framing, the connector doesn't implement it directly.

## 4. Wire Contract

### 4.1 Frame

The leading 2 bytes of a STREAM frame are `header_size`.

```text
+----------------+----------------+----------------+----------------+
| u16 header_len | u32 payload_sz | header bytes   | payload bytes  |
+----------------+----------------+----------------+----------------+
```

### 4.2 Header

```text
+----------------+---------+----------+----------+------------------+
| format_marker  | kind u8 | codec u8 | flags u8 | request_seq u64? |
| u8 = 0xF2      |         |          |          |                  |
+----------------+---------+----------+----------+------------------+
| name u8+n | meta u16+n? | corr u8+n? | flow_id 36B + origin u8?   |
+-----------+-------------+------------+----------------------------+
```

- **The header's first byte is `format_marker = 0xF2`.** A different
  value is a decode error.
- `kind`/`codec` are encoded as a **1-byte enum**, not a string.
- Packet name is `u8 name_len + UTF-8 bytes`, at most **255 bytes**.
  **`Response` and `Error` don't carry a
  [packet name](../01-glossary.en.md#packet-name)** — encoded with
  `name_len = 0`. Since a response doesn't select a handler and
  correlation is already decided by `request_seq`, this field isn't
  used (see "reply correlation" in
  [03 Message Model](../04-message-model.en.md)).
- Metadata continues as `u16 meta_len + metadata bytes`, and
  correlation id as `u8 len + bytes`.
- The flow field's **36-byte `flow_id` and 1-byte `flow_origin`
  always exist together** or are both absent. The meaning is owned by
  [Message Flow Correlation §3](../27-flow-correlation.en.md#3-format-and-ownership).
- **Every multi-byte integer is network byte order.**

Application code doesn't build or modify this header directly — the
connector runtime owns it.

### 4.3 Flags

| Flag | Value | Meaning |
|---|---|---|
| has request seq | `0x01` | The `request_seq` field is present |
| has metadata | `0x02` | The `meta` field is present |
| payload compressed | `0x04` | The payload is compressed |
| has correlation id | `0x08` | The correlation id field is present |
| has flow id | `0x10` | The `flow_id`/`flow_origin` fields are present |

`has flow id` isn't set on a `Control` packet
([flow-correlation §3](../27-flow-correlation.en.md#3-format-and-ownership)).

### 4.4 Metadata

```text
+---------------+-------------+-------------+
| count u8      | entry...    | entry...    |
+---------------+-------------+-------------+

entry:
+-------------+-------------+-------------+-------------+
| key_len u8  | key bytes   | val_len u16 | value bytes |
+-------------+-------------+-------------+-------------+
```

Key and value are UTF-8 strings.

- `key_len` must be 1 or greater.
- If the same key appears twice, it's a **decode error**.
- `count` must match the following entry count.

**The size limit has two stages.**

| Stage | Bound |
|---|---|
| The wire `meta_len` field's representation limit | 65535 bytes |
| The bound the connector validates before sending | **1024 bytes** — a validation error if exceeded. **Not adjustable through a public option** |

Metadata only carries **small values**, such as trace id, locale,
tenant id.

### 4.5 Decode Error

The following are all decode errors.

- An unknown `kind`/`codec`/flag bit
- A mismatch between the `has request seq`/`has metadata` flag and
  actual field presence
- `Response` or `Error` whose `name_len` isn't `0`

### 4.6 Control Frame

`Control` kind is the connector-internal control frame. **An
application packet name can't use the `$zlink.` prefix.**

**A control frame's namespace is separated by packet kind.** Since a
control frame is only delivered as `Control` kind, even if the
application uses a string identical to a control name below as
`Send`/`Request` kind, dispatch isn't mixed up. Still, to avoid
confusion, don't use `session-closing` for an application packet. A
new control packet uses the `$zlink.` prefix.

A control frame has `Raw` codec, no request sequence, no metadata, and
no flow flag. **The payload differs per control packet.**

| Control Packet | Payload |
|---|---|
| `$zlink.heartbeat.ping` | **Empty** |
| `$zlink.heartbeat.pong` | **Empty** |
| `session-closing` | **Not empty** — see below |

`session-closing` is a control packet the server sends right before
closing a session, and the client reads it to confirm `closeReason`
([Host Relocate And Shutdown §9](../28-graceful-drain-handoff.en.md#9-moving-pending-messages-timers-and-sessions)).

```text
+------------+-------------------+----------------+--------------------+
| version u8 | close_reason u8   | diag_len u16   | diagnostic bytes   |
| = 1        | 1..6              | 0..512         | UTF-8              |
+------------+-------------------+----------------+--------------------+
```

| `close_reason` | Value |
|---|---|
| `ClientClose` | 1 |
| `IdleTimeout` | 2 |
| `HeartbeatTimeout` | 3 |
| `ServerDrain` | 4 |
| `ProtocolError` | 5 |
| `TransportError` | 6 |

An unknown version/reason, or `diag_len` exceeding 512 or mismatching
the actual payload length, is a decode error.

### 4.7 Payload Size Bound

Send and receive each have a payload bound. **The default is 64KB
(65536 bytes) for both**, and unlike the metadata bound, it's
**adjusted with an option.**

| Direction | Default Bound | On Violation |
|---|---|---|
| Send | 64KB | Fails as a validation error (§9) **before the transport write** |
| Receive | 64KB | `FrameTooLarge` (§9) |

**The bound applies only to the payload bytes with the length prefix
and encoded header subtracted.** On receiving a compressed frame, both
the wire's compressed payload and the decompression result are each
compared against the same receive bound. If either exceeds it, it
isn't delivered to the application handler or request completion. The
send bound is based on the payload actually written to the transport,
so a send that requested compression checks the compression result. An
application that needs a payload larger than 64KB explicitly increases
this value.

## 5. Packet Model

The user API doesn't handle raw header bytes.

- **The default packet name is the payload type name.**
- If the caller specifies a name explicitly, that takes priority.
- If auxiliary information is needed, it's added as a metadata
  key-value.
- **An API that handles arbitrary header bytes isn't put on the public
  surface.**

### 5.1 Message Kind

| Kind | Value | Meaning |
|---|---|---|
| Send | 1 | A one-way packet that doesn't wait for a response |
| Request | 2 | A packet that waits for a response |
| Response | 3 | A request's success response |
| Error | 4 | A request's failure response, or a stream error unrelated to a request |
| Control | 5 | A connector-internal control frame (§4.6) |

### 5.2 Request Correlation

`request_seq` is a `u64` correlation sequence the runtime manages, and
is put **only in request/response/error response.**

- Within the same connector instance, **`request_seq` must not be
  duplicated among concurrently pending requests.**
- The value `0` isn't used.

**Matching rule:**

| Situation | Behavior |
|---|---|
| `Send` sent | Sent with no `request_seq`. Not put in the pending map |
| `Request` sent | A new `request_seq` is assigned and registered in the pending map |
| `Response` received | The pending request of the same `request_seq` **completes as success** |
| `Error` received — has `request_seq` | The pending request of the same `request_seq` **completes as failure** |
| `Error` received — no `request_seq` | Delivered to the error surface as a **stream-level error** unrelated to a pending request (§9) |

- **`request_seq` is canonical for pending request matching.** Since
  `Response` and `Error` **have no packet name field at all**
  (`name_len = 0`), they also can't be matched by name. Which response
  belongs to which is already decided by sequence. The same terminal
  reply principle is used when relaying an Actor request in a STREAM
  session
  ([Session Actor Dispatch §3](../20-session-actor-dispatch.en.md#3-inbound-dispatch-and-reply)).
- **When a request timeout, close, or disconnect occurs, every pending
  request completes as failure and is removed from the map.** It isn't
  automatically resent after reconnection (§6).

### 5.3 Error Payload

`Error` kind's payload is **always a UTF-8 JSON object regardless of
codec configuration**, and the header's codec is `JSON`.

```json
{"code":"error_code","message":"message"}
```

To treat an application-domain error as a normal reply, use `Response`
kind with a user-defined payload, not `Error`.

### 5.4 Codec

| Codec | Value |
|---|---|
| Raw | 0 |
| JSON | 1 |
| MessagePack | 2 |
| Protobuf | 3 |

**JSON is the default codec.** Every language's connector takes one
typed payload codec as a connector creation option, used together for
typed send, request, and receive. MessagePack/Protobuf are provided by
an optional package with that codec implementation. A public API for
registering a codec per message type, or switching codec per
send/request operation, isn't provided. A Raw encoded payload can use
the codec number the payload specifies as is, for external protocol
interworking.

The TypeScript package root exports a browser-safe
`ZlinkStreamPayloadCodec`, injected as the `codec` option when building
a connector. Node framework serializer registration uses the same
package's `./framework` subpath. The two entry points use the same
codec number owned by `stream-wire`, but the browser module graph
mustn't reference the server framework runtime.

## 6. Connection Lifecycle

The following C# excerpt is a non-normative example to explain how
connection, manual dispatch, and close look in one connector interface.
It doesn't require the same signature in other languages, and the
exact .NET signature is defined by the
[.NET Stream Connector contract](languages/dotnet/03-stream-connector.en.md).

```csharp
public interface IZlinkStreamConnector : IAsyncDisposable
{
    ZlinkStreamConnectionState State { get; }
    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
}

public interface IZlinkStreamLifecycleCall
{
    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamRequestCall
{
    IZlinkStreamRequestCall PacketName(string name);
    IZlinkStreamRequestCall Timeout(TimeSpan timeout);
    ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default);
}
```

```csharp
await connector.Connect.Async(cancellationToken); // waits until connection and receive-loop preparation finish.

var reply = await connector
    .Request(payload)
    .PacketName("inventory.get")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(cancellationToken); // waits for the terminal reply of the same request sequence.

await connector.Dispatch.Async(cancellationToken); // processes a pending callback in the current context, in Manual mode.
await connector.Close.Async(cancellationToken);    // outside a callback, waits until the shared close work finishes.
```

| State | Meaning |
|---|---|
| `Created` | A Connector has been built but hasn't started connecting yet. |
| `Connecting` | The Connector is performing the initial connection. |
| `Connected` | The Connector has completed connection. |
| `Reconnecting` | The Connector is performing automatic reconnection. |
| `Disconnected` | The Connector's transport connection has dropped. |
| `Closed` | The Connector has been closed. The same connector object doesn't reconnect. |

A connection request behaves as follows depending on the current
state.

| Current State | Behavior |
|---|---|
| `Created` | The Connector starts the initial connection. |
| `Disconnected` | The Connector starts a manual reconnect. |
| `Connecting` | The caller waits until the already-in-progress connection attempt finishes. |
| `Connected` | Since already connected, the call completes immediately as success. |
| `Reconnecting` | The caller waits for the result of the in-progress automatic reconnect. |
| `Closed` | Since a closed connector can't reconnect, the call fails with an error. |

**Reconnect and pending request:**

- Automatic reconnect is **on by default.**
- A send during reconnect isn't queued — it **fails with a
  `Disconnected` error.**
- Once the connection drops, **every pending request fails**, and it's
  **not automatically resent** after reconnect.

**Heartbeat:**

- If on, sends a control ping at the specified interval.
- If no inbound frame arrives within the specified timeout, treats the
  transport as disconnected and applies the reconnect policy.
- **Even with heartbeat off, it still replies with pong to an inbound
  ping.**

### 6.1 Default Value

Even though the per-language name differs, **the default value must be
the same across every language.**

| Item | Default |
|---|---|
| Connect timeout | 5 seconds |
| Request timeout | 30 seconds |
| Wait timeout (waiting for a specific packet) | 5 seconds |
| Heartbeat | On — interval 1 second, timeout 5 seconds |
| Reconnect | On — initial delay 250ms, max delay 5 seconds, backoff factor 2.0, max attempts 3 |
| [Dispatch mode](../01-glossary.en.md#dispatch-mode) | `Manual` (§7) |
| Codec | JSON (§5.4) |
| Compression | Lz4 (§8) |
| Send/receive payload bound | 64KB each (§4.7) |
| Inbound observer queue | 1024 notifications, 0-byte payload preview (§10) |
| Receive message queue | 1024 messages (§10.1) |
| TLS certificate validation | On — the default of the validation-skip option is off, used only for a test's self-signed certificate |

### 6.2 Connector Reconnect Instrument

The Connector records automatic/manual reconnect attempt results with
the following metric. This instrument is owned by the client
connector, and the server session runtime doesn't guess or record
reconnect status on its behalf.

| Instrument | Kind | Unit | Label | Meaning |
|---|---|---|---|---|
| `zlink.stream.reconnects` | counter | `{reconnect}` | `transport`, `outcome`, `reason` | Cumulative Connector reconnect attempt result |

`outcome` is a closed value of `connected|failed|cancelled|shutdown`,
and `reason` is a closed value of
`transport_closed|connect_failed|tls_failed|timeout|requested`.
`transport` is one of §3.1's `tcp|tls|ws|wss`. Session ID and remote
endpoint aren't included in the label. The per-language connector
publishes the same name and closed label to the public metric provider
or sink the per-language exact interface decides. E2E and the
application use that provider's or sink's public reader, and don't
build a server-side proxy API. A reader, sink, or exporter failure
doesn't change send, request, or connection state.

### 6.3 Close Reason

Once the connection drops, the connector exposes a **close reason.**
The value set is a **closed set** aligned with the server-side
`close_reason`
([runtime-metrics §4](../25-runtime-metrics.en.md#4-object-and-stream)),
and the wire encoding is owned by §4.6's `session-closing` control
packet.

| Reason | Meaning |
|---|---|
| `ClientClose` | The client closed it |
| `IdleTimeout` | The server closed an idle session |
| `HeartbeatTimeout` | Disconnected because heartbeat didn't respond |
| `ServerDrain` | The server closed the session with **graceful drain** |
| `ProtocolError` | Disconnected due to a protocol violation |
| `TransportError` | Disconnected due to a transport-level failure |

A client that received `ServerDrain` looks at this value to **decide
reconnection and backoff**
([Host Relocate And Shutdown §9](../28-graceful-drain-handoff.en.md#9-moving-pending-messages-timers-and-sessions)).
**A capability for the server to specify a replacement endpoint isn't
included in this contract.**

The per-language document only owns the **type name and exposure
form** (whether a property or an event argument) expressing this
reason.

## 7. Dispatch Mode

| Mode | Behavior |
|---|---|
| **`Manual`** (default) | The receive loop doesn't directly call a handler/error/disconnect/request callback — it puts it in an internal queue. The user explicitly pumps it to run |
| `Immediate` | Runs directly on the receive path |

**The reason the default is `Manual` is a game engine constraint**
(§2.2). Since an engine object can't be handled off the main thread, it
must be pumped on the main thread to be safe.

The `waitFor`/`expectNone`/`waitForSequence` family isn't a registered
callback. Since this surface directly observes and consumes an
unconsumed packet in the receive message queue in both dispatch modes,
it doesn't need a separate dispatch pump even in `Manual`. `dispatch`
only runs a registered push handler, error/disconnect handler, and
request callback.

## 8. Compression

- The supported algorithm is **None and Lz4**, and the **default is
  Lz4.**
- The compression algorithm isn't written in the header per packet.
  **It's decided once as a connector option.**
- The `payload compressed` flag (§4.3) is just a mark that "this
  payload is compressed with that algorithm."
- **server → client**: if the server turns on the flag and sends, the
  connector decompresses **before** calling the typed callback.
- **client → server**: **only a send/request that explicitly requested
  compression** is compressed. Turning on the option doesn't
  automatically compress.
- **Compression only applies to the payload. The header isn't
  compressed.**
- **Setting `None` doesn't exchange a compressed frame.** A send/
  request that requested compression fails, and an inbound frame with
  the `payload compressed` flag on is rejected with
  `DecompressionFailed`.

## 9. Error Meaning

| Error | Meaning |
|---|---|
| `Disconnected` | No connection, or dropped |
| `ConfigurationError` | Invalid configuration (scheme mismatch, **a transport the environment doesn't support**, etc.) |
| `ValidationFailed` | Pre-send validation failure (metadata bound exceeded, send payload bound exceeded, etc.) |
| `RequestTimeout` | Reply wait time exceeded |
| `ConnectTimeout` | Connect time exceeded |
| `FrameDecodeFailed` | Frame/header decode failure (§4.5), or a structurally valid Error frame's JSON payload doesn't satisfy §5.3 |
| `FrameTooLarge` | The payload exceeded the receive bound |
| `SendFailed` | Send failure |
| `CompressionFailed` / `DecompressionFailed` | Compression/decompression failure |
| `TlsValidationFailed` | TLS validation failure |
| `ReceivedMessageDropped` | Receive message queue overflow (§10.1) |
| `UserCallbackFailed` | A user callback failed |
| `ObserverFailed` / `ObserverDropped` | Inbound observer callback failure / queue overflow |
| `RemoteError` | The server responded with an Error payload satisfying §5.3. If `request_seq` matches a pending request, that request fails; if absent or mismatched, it's delivered as an error event |

The effect an error has on the current operation and connection is
below. The per-language document only owns the error name's
expression — it doesn't change whether it's terminal, the close
reason, or the reconnect condition.

| Error | Current Operation | Connection | Close Reason | Automatic Reconnect |
|---|---|---|---|---|
| `ConfigurationError`, `ValidationFailed` | Call failure | Kept, or the pre-connect-attempt state kept | None | Not done |
| `RequestTimeout` | Only that request fails | Kept | None | Not done |
| `ConnectTimeout`, `TlsValidationFailed` | Connect failure | `Disconnected` | `TransportError` | Applies the reconnect option's attempt policy |
| `Disconnected`, `SendFailed` | The in-progress operation fails | `Disconnected` if the transport dropped | `TransportError` | Applied if the reconnect option is on |
| `FrameDecodeFailed` — frame/header | That frame isn't delivered, and the pending request fails | Ended | `TransportError` | Applied if the reconnect option is on |
| `FrameDecodeFailed` — Error JSON payload | If a matching `request_seq` exists, only that request fails; if absent or mismatched, delivered as an error event | Kept | None | Not done |
| `FrameTooLarge` | That frame isn't delivered, and the pending request fails | Ended | `TransportError` | Applied if the reconnect option is on |
| `CompressionFailed` | Only that send operation fails | Kept | None | Not done |
| `DecompressionFailed` | Only that receive packet or pending request fails | Kept | None | Not done |
| `ReceivedMessageDropped` | Only the newly arrived send is discarded | Kept | None | Not done |
| `UserCallbackFailed`, `ObserverFailed`, `ObserverDropped`, `RemoteError` | Delivered as an error event or the related callback/request | Kept | None | Not done |

**The delivery method differs by surface, but the meaning is the
same.**

- An async (await) surface **throws the error on failure.**
- A callback-based surface **delivers the failure as a result object.**
- A stream-level error with no request id is delivered as an
  **error event.**

## 10. Inbound Observer

A surface that **read-only observes** an inbound frame. Can be
registered **only before** connection starts.

- Observed values: message kind, packet name, codec, request sequence,
  metadata, payload byte length, whether compressed, receive time,
  payload preview
- **The default payload preview length is 0.**
- Metadata and preview are a snapshot. Even if the observer changes
  them, the value a request completion or handler sees doesn't change.
- The observer callback **isn't run directly on the receive path.** A
  slow log/metric transmission mustn't block receive processing.
- A callback failure is reported as `ObserverFailed`, and a queue
  overflow as `ObserverDropped`, **without blocking the original
  frame processing.**
- The observer notification queue is **separate** from the user
  receive message queue, and the **default bound is 1024
  notifications** (§6.1). Adjusted with an option.

### 10.1 Receive Message Queue

A `Send` packet the server sent stays in the **receive message queue**
until it moves to a handler (`on` family) or a wait surface (`waitFor`
family). The default bound is **1024 messages**, adjusted with an
option.

- **If the queue is full, a newly arrived send message is discarded
  and `ReceivedMessageDropped` is reported.** It doesn't evict a
  message already in the queue.
- **A response, error response, and heartbeat control frame aren't
  counted against this bound.** Because they're needed for request
  completion and connection keep-alive.
- This queue is **separate** from the inbound observer notification
  queue (§10).

### 10.2 Test Wait Surface

The connector provides a **wait surface for observing a push in a
test** as a public API. All five languages must provide the same
timeout, consumption order, and negative-observation meaning. A
general-purpose assertion unrelated to connector state, such as
condition checking, expected error, and timeout verification, isn't a
connector public contract. E2E owns that auxiliary code in each
language's `Client/Support`.

#### 10.2.1 Push Observation Surface — The `waitFor` Family

Something that can only be judged by observing the receive message
queue (§10.1). A method of the connector instance.

All three surfaces let the caller specify the packet name explicitly,
or decide it from the payload type. The exact argument and overload,
and the completion terminator (`.Async`/`.submit`/`.run`), are owned by
each language's document, and the remaining conditions are narrowed by
builder chaining.

| Surface | Contract | Failure |
|------|------|------|
| `waitFor<T>(name)` | Waits until that packet arrives. Narrowed with `.where(predicate)`/`.timeout(t)`. The default timeout is §6.1's `wait timeout` (5 seconds) | **Throws an error** if it doesn't arrive within the timeout (§10.1 specifies this surface consumes the queue) |
| `expectNone<T>(name)` | Confirms that packet **doesn't arrive** during `.within(window)` (negative). The symmetric of `waitFor` | **Throws an error** if it arrives within the window |
| `waitForSequence<T>(name)` | `.expect(p1).expect(p2)….timeout(t)` — confirms a push of the same name arrives **in the given predicate order** and returns the payload list | **Throws an error** if the order is wrong or it times out. This surface exists to verify **"arrived in order"**, not "N arrived" |

- **A status wait doesn't have a separate surface.** Since status is a
  field of the payload, it's expressed as
  `waitFor<T>(name).where(p => p.status == …)`. The connector must not
  know which field is status.
- **Domain REST polling (`/orders/{id}`, etc.) isn't this surface.**
  That's the HTTP client's job, and isn't put in the connector
  contract.

## 11. Deployment Deliverable

This spec also owns which deliverable each target is distributed as.
Because the deployment form reflects that environment's constraint.

| Target | Deliverable | Distribution Channel |
|---|---|---|
| Plain C++ client | `zlink-stream-connector` (`zlink::stream_connector`) | CMake · vcpkg · Conan |
| Server e2e/perf (C++) | `zlink-stream-e2e-client` (`zlink::stream_e2e_client`) | CMake · vcpkg · Conan |
| Unreal | `zlink-unreal-stream-connector` | source plugin |
| Godot (C++) | `zlink-godot-stream-connector` | source GDExtension |
| Cocos/Axmol | `zlink-axmol-connector` | source package |
| `.NET` (desktop/server) | `Systems.Zlink.Stream.Connector` | NuGet |
| **Unity (native)** | **Uses the `.NET` package above as is** (no dedicated package) | NuGet |
| **Godot C#** | **Uses the `.NET` package above as is** | NuGet |
| Java | `systems.zlink:zlink-stream-connector` | Maven |
| **Browser family** (web/Cocos web/Unity WebGL/Godot Web) | `@zlink-systems/stream-connector` package root | npm |
| **Unity WebGL adapter** | `@zlink-systems/stream-connector`'s browser bundle and jslib/C# interop source | `com.zlink.stream-connector.webgl` UPM source package |
| (common) wire layer | `@zlink-systems/stream-wire` | npm |

**Deployment principle:**

- **The web family shares one npm package root.** Since browser/Cocos
  web/Unity WebGL/Godot Web are all a browser runtime, packages aren't
  multiplied per target.
- **A native engine adapter is source-distributed** (Unreal plugin,
  GDExtension, Axmol CMake). It's convention to be incorporated as
  source into the engine build system.
- **Unity (native) and Godot C# don't have a separate package.** They
  use the `.NET` connector as is.

The Unity WebGL UPM package doesn't build a new wire runtime. It
includes the npm package root's browser bundle and only provides the
jslib/C# call boundary Unity requires as source. So browser and Unity
WebGL use the same TypeScript connector protocol and codec.

## 12. Regression Test

The verification items this spec requires. Even if the per-language
test name differs, the meaning must be the same.

| Item | Verification |
|---|---|
| Transport frame | Frame/header encoding/decoding follows §4 |
| **Per-environment transport availability** | **The TypeScript package root rejects `tcp://`/`tls://` as a configuration error** |
| **Browser bundle** | **The TypeScript package root bundle doesn't include a platform-only socket module** |
| Typed request/reply | Correlation and matching rule follows §5.2 |
| Error response | The `Error` payload is §5.3's JSON object, and splits into pending failure / stream error depending on `request_seq` presence |
| Pending request cleanup | On timeout/close/disconnect, every pending fails and is removed (§5.2) |
| Payload bound | The send bound applies **before the transport write**, and receive checks the wire payload and decompression result each (§4.7) |
| Metadata | Bound/duplicate/empty-key validation (§4.4) |
| Packet name | UTF-8 length limit (§4.2), `$zlink.` prefix reservation (§4.6), the per-language exact interface's default name/override rule |
| Codec | Connector option injection, codec number sharing, and browser/server dependency separation (§5.4) |
| Compression | Per-direction behavior (§8) |
| Error handling | Error meaning (§9) |
| Inbound observer | Observation/isolation/overflow (§10) |
| Connection lifecycle | State transition/reconnect/heartbeat (§6) |

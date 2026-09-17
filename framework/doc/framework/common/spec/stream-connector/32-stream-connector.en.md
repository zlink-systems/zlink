# Stream Connector — Common Spec

[Spec table of contents](../server/README.en.md) | [Previous: Session Actor Dispatch](../server/04-session/02-session-actor-binding.en.md) | [Next: Location Runtime](../server/05-location-relocation/01-location-runtime.en.md)

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
> ([Public Contract Governance](../server/00-foundation/01-public-contract-governance.en.md)).

## 1. Purpose And Scope

Stream Connector is the **client-side library that connects to the
server framework's STREAM model**. It lets the client send and receive
the same
[packet](../server/00-foundation/02-glossary.en.md#stream-packet) (header + payload) the
server session callback receives.

The [Connector](../server/00-foundation/02-glossary.en.md#stream-connector) doesn't include
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
| **Browser JavaScript** | **No ambient execution context** equivalent to `AsyncLocalStorage` | To continue a received message's flow, the caller **passes that flow explicitly** to the send call (§5.5) |
| Node.js | Not the TypeScript connector's product execution environment | Only handles the server process and browser test runner |

## 3. Transport

### 3.1 Endpoint Scheme → Transport

| URI Scheme | Transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

- **If transport isn't specified, the endpoint scheme decides the
  transport.** The table above is that mapping, and an option's fixed
  default doesn't override the scheme. A configuration that names only
  a `ws://` endpoint connects over WebSocket with no further option.
- **If the specified transport doesn't match the endpoint scheme, it
  fails with `ConfigurationError` (§9).** When the two values name
  different transports, there's no way to decide which one to follow.

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
  [packet name](../server/00-foundation/02-glossary.en.md#packet-name)** — encoded with
  `name_len = 0`. Since a response doesn't select a handler and
  correlation is already decided by `request_seq`, this field isn't
  used (see "reply correlation" in
  [03 Message Model](../server/00-foundation/05-message-model.en.md)).
- Metadata continues as `u16 meta_len + metadata bytes`, and
  correlation id as `u8 len + bytes`.
- The flow field's **36-byte `flow_id` and 1-byte `flow_origin`
  always exist together** or are both absent. The meaning is owned by
  [Message Flow Correlation §3](../server/06-observability/04-flow-correlation.en.md#3-format-and-ownership).
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
([flow-correlation §3](../server/06-observability/04-flow-correlation.en.md#3-format-and-ownership)).

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
([Complete Host Relocation Flow §9](../server/05-location-relocation/05-host-relocation-flow.en.md#12-moving-pending-messages-timers-and-sessions)).

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

- **The default packet name is the payload type's simple name.** No
  namespace or package qualifier is attached.
- **A name that varies with the compiler or the runtime isn't used as a
  packet name.** The packet name is a value agreed with the server, so
  a compiler-mangled name — anything that changes when the build
  environment changes — leaves the server unable to find a handler for
  the same type.
- **All five languages provide a way to attach a packet name directly
  to the payload type.** The form of that attachment (attribute,
  annotation, static member, and so on) is owned by the per-language
  document. A name attached to the type takes priority over the simple
  type name.
- If the caller specifies a name **per operation**, that takes the
  highest priority.
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
  ([Session Actor Dispatch §3](../server/04-session/02-session-actor-binding.en.md#5-bind-and-relay)).
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
send/request operation, isn't provided. **The typed payload codec and
the name resolver that decides a packet name from the payload type
(§5) are both injected as connector creation options.** Instead of a
surface that switches them per operation, the two injection points sit
in the creation options only. If neither is given, the JSON codec
and §5's default name rule are used, and the injection point's type
name is owned by the per-language document. A Raw encoded payload can
use the codec number the payload specifies as is, for external
protocol interworking.

The TypeScript package root exports a browser-safe
`ZlinkStreamPayloadCodec`, injected as the `codec` option when building
a connector. Node framework serializer registration uses the same
package's `./framework` subpath. The two entry points use the same
codec number owned by `stream-wire`, but the browser module graph
mustn't reference the server framework runtime.

### 5.5 Flow Exposure And Propagation

**A received message exposes `flow_id`/`flow_origin` alongside the
payload, the packet name, and the metadata.** All five languages expose
them. The format and meaning of the two values are owned by
[Flow correlation §3](../server/06-observability/04-flow-correlation.en.md#3-format-and-ownership),
and when the diagnostics level is `Off` the connector doesn't deliver
them, so both are empty (§13). The application reads these values to
line up its own log with the server trace as one flow.

A send or request started while a handler processes a received message
**continues that message's flow.** How it is continued is decided by the
execution environment.

| Execution Environment | How the flow is continued |
|---|---|
| A runtime with an ambient execution context | The connector holds the current flow in the context it runs the handler on, and a send/request started on that same context uses the value with no argument |
| A runtime with no ambient execution context (browser JavaScript) | The caller passes the flow of the message being processed to the send call explicitly. The name of that surface is owned by the per-language document |

- **The difference between the two is the calling form, not the
  guarantee.** Either way the built frame carries the same
  `flow_id`/`flow_origin`, and a send that continues no flow starts a
  new flow on both.
- **Placing an explicit surface on a runtime with no ambient context
  applies the requirement of
  [Flow correlation §6](../server/06-observability/04-flow-correlation.en.md#6-async-work-and-execution-context)
  to the connector.** Browser JavaScript has no surface equivalent to
  `AsyncLocalStorage`, so a value can't be held on the execution
  context (§2.2). As that same document forbids, the current flow isn't
  guessed from a process-global variable or a mutable connector field.

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
- **The reconnect max attempt count must be able to express
  unlimited.** The form of that expression (a null value, a negative
  number, a named constant, and so on) is owned by the per-language
  document. A client that keeps trying until the connection is restored
  specifies unlimited instead of writing a large number, which is what
  separates it from a configuration with a finite attempt count.
- **The delay between attempts carries randomness.** The base delay
  starts at the initial delay, is multiplied by the backoff factor on
  each attempt, and stops at the maximum delay. What is actually waited
  is **a value drawn between 50% and 100% of that base delay.**

    With a deterministic delay, every client that was attached comes
    back **at the same moment** once the server drops them. That moment
    is the heaviest one for the server, so the timing is spread out.

- **When the attempts run out, the disconnect handler runs.** After the
  last attempt fails the connection state becomes `Disconnected` and
  registered disconnect handlers run. A configuration set to unlimited
  never reaches this point.

    **Whether the handler receives the close reason as an argument is
    settled by the language.** Where it does not, the reason is read
    from the surface in §6.2. Either way there is a path to it.

    **Disconnect handlers run once when the transport drops and once
    when the attempts run out.** The first says "there is no connection
    right now"; the second says "bringing it back has been given up."
    Without the first, a client that received `ServerDrain` learns
    nothing while a reconnect is still succeeding; without the second,
    the moment reconnection was abandoned is invisible. A successful
    reconnect means the second never happens.

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
| [Dispatch mode](../server/00-foundation/02-glossary.en.md#dispatch-mode) | `Manual` (§7) |
| Codec | JSON (§5.4) |
| Compression | Lz4 (§8) |
| Send/receive payload bound | 64KB each (§4.7) |
| TLS certificate validation | On — the default of the validation-skip option is off, used only for a test's self-signed certificate |
| Diagnostics level | `Errors` (§13) |

### 6.2 Close Reason

Once the connection drops, the connector exposes a **close reason.**
The value set is a **closed set** aligned with the server-side
`close_reason`
([runtime-metrics §4](../server/06-observability/02-runtime-metrics.en.md#6-object-count-capacity-and-relocation-instruments)),
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
([Complete Host Relocation Flow §9](../server/05-location-relocation/05-host-relocation-flow.en.md#12-moving-pending-messages-timers-and-sessions)).
**A capability for the server to specify a replacement endpoint isn't
included in this contract.**

**The close reason can be read from the connector's read surface at any
time.** Code that didn't receive the disconnect event reads the same
value after the connection closed. If the connection has never dropped,
the value is empty. **A failed first connect also leaves a reason** —
the impact table in §9 settles it, so `ConnectTimeout` and
`TlsValidationFailed` give `TransportError`. Even where a connection was
never established, how the attempt ended is kept. The disconnect event carrying the reason as an
argument adds to this read surface rather than replacing it.
Reconnecting doesn't clear the value — the last close's reason is kept.

The per-language document only owns the **type name and the shape of
the read surface** (whether a property or a method) expressing this
reason.

### 6.3 Option Validation

**Every option item is validated.** Checking only some of them lets a
bad value elsewhere pass unnoticed, and the caller cannot tell a
configuration mistake from a connection failure.

- The endpoint/transport match (§3.1), the connect/request/wait
  timeouts, the heartbeat interval and timeout, the reconnect delays,
  backoff factor, and max attempts, the send/receive payload bounds,
  the codec and compression settings, the dispatch mode, and the
  diagnostics level are **all checked.** The values checked are the
  ones left after §6.1's defaults are applied.
- **Validation happens at the earliest point the language can report
  the failure.** A language whose creation surface can return a failure
  validates when the connector is built; one whose creation surface
  cannot validates when the connection is attempted. Either way the
  rejection comes **before a connection is made.**
- **A configuration that fails validation never reaches a connection.**
  Only options that passed go on to connect.
- A single value outside its allowed range is **`ValidationFailed`**;
  items that don't fit together are **`ConfigurationError`** (§9). A
  conflict between the endpoint scheme and the transport, a transport
  the environment doesn't support, and a compression codec supplied
  with compression turned off belong to the latter.
- **Delivery follows §9.2.** A build with exceptions disabled receives
  a value, and every other language receives an exception carrying the
  code. Throwing a language's standard exception as is leaves the
  caller unable to tell the two codes apart.

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

**Handler registration returns a value that can unregister it.** This
holds for the push handler and for the error/disconnect/connection
state handlers alike. If a registration can only be removed by closing
the connector, a client that registers and unregisters subscriptions
with the lifetime of one screen has to build the connection again. An
unregistered handler isn't run by later dispatches, and unregistering
the same value twice isn't treated as an error. The returned type name
is owned by the per-language document.

**Whether the returned value's lifetime is the registration's lifetime
is settled by the language.** A language that expresses ownership as a
value unregisters when that value goes away; there, the returned value
is kept for as long as the registration must live. Elsewhere the value
may be discarded and the registration stays until it is unregistered
explicitly.

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
| `ValidationFailed` | A validation failure — covers pre-send validation (metadata bound exceeded, send payload bound exceeded), option validation for a value outside its allowed range (§6.3), and a violation of a wait surface's observation condition (§10.1) |
| `RequestTimeout` | Reply wait time exceeded |
| `ConnectTimeout` | Connect time exceeded |
| `FrameDecodeFailed` | Frame/header decode failure (§4.5), or a structurally valid Error frame's JSON payload doesn't satisfy §5.3 |
| `FrameTooLarge` | The payload exceeded the receive bound |
| `SendFailed` | Send failure |
| `CompressionFailed` | Compression failure |
| `DecompressionFailed` | Decompression failure |
| `TlsValidationFailed` | TLS validation failure |
| `UserCallbackFailed` | A user callback failed |
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
| `UserCallbackFailed`, `RemoteError` | Delivered as an error event or the related callback/request | Kept | None | Not done |

### 9.1 The Closed Error Code Set

The **thirteen codes above are all of them.** An implementation neither adds nor
drops one. A per-language document owns only how the names are spelled.

One exception is stated explicitly — **`TlsValidationFailed` does not occur in a
browser runtime**, because the browser's WebSocket API does not distinguish a TLS
failure from an ordinary connection failure. The code stays in the set and simply
goes unused there. Varying the set per runtime would split the impact table above.

### 9.2 Delivery — The Receiver Must Be Able To Read The Code

**The delivery method differs by surface, but the meaning is the same.**

- An async (await) surface **delivers the error on failure.**
- A callback-based surface **delivers the failure as a result object.**
- A stream-level error with no request id is delivered as an **error event.**

Whichever the method, **the receiver must be able to tell which of the thirteen it
is.** That is this section's requirement; what carries it is up to the platform.

| Target | Delivery | How the code is read |
|---|---|---|
| A build with exceptions disabled (game engines) | Returned as a value. **Nothing is thrown** | The result object's error code |
| Other C++ consumers (e2e, tooling) | A throwing adapter over the same surface | The code the exception carries |
| Every other language | An exception is thrown | The code the exception carries |

**A language's standard exception type is not used as is.** Types such as
`IllegalArgumentException` or `InvalidOperationException` have nowhere to put the
code, so the caller cannot tell `ValidationFailed` from `ConfigurationError`. A
language that delivers by exception defines **a dedicated exception type that
carries the code.**

The same requirement applies to option validation failures and to violations of the
observation surfaces (§10.1).

## 10. Receive Message Queue

A `Send` packet the server sent stays in the **receive message queue** until it moves to a handler
(`on` family) or a wait surface (`waitFor` family).

- **The client keeps receiving and processing what it is given.** The queue has no bound, no
  message is discarded, and the connection is never closed over it.
- **The connector applies no backpressure.** It does not implement a socket of its own; it uses
  what the runtime provides (§2, §3.2). Browser and WASM builds run on the platform's native
  WebSocket API, which offers no surface for withholding reads. Flow control belongs to the
  server's STREAM socket and is outside this document.
- **A response, error response, and heartbeat control frame do not pass through this queue.**
  They are needed for request completion and connection keep-alive.
- **The connector exposes the received count per packet name as
  `receivedCount(name)`, and all five languages provide it.** The value is the
  **number received** under that name. **Consuming does not lower it** — the value
  stands whether a handler dispatched the message or a wait surface took it. **The
  count is independent of the dispatch mode** — whether `Manual` waits for a pump or
  `Immediate` runs on the receive path, the value rises when the packet arrives (§7).
  This surface exists for scenario assertions, so "how many arrived under this name"
  must still be answerable after an `on` handler has dispatched them.
- **The reference point is the moment the connection is established.** The count
  starts at zero when the connection is established and counts what arrives on that
  connection. A reconnect is a new connection, so it starts at zero again. A name
  never received is zero.
- **Establishing a connection also clears whatever the previous connection left
  unconsumed.** Resetting only the count and keeping the queue leaves the two
  describing different connections, and `waitFor` would hand back a packet from
  before the drop as if it belonged to the new connection.
- The value is for scenario assertions and diagnostics, not as a basis for flow
  control — the queue has no bound, so a larger value changes nothing about what the
  connector does.

Messages do not pile up in a client that works. A handler dispatches them or a wait surface
consumes them. If they do pile up it is a client bug, and neither discarding them nor closing the
connection gains anything — the client has to be restarted either way. So the connector holds no
policy for it.

### 10.1 Test Wait Surface

The connector provides a **wait surface for observing a push in a
test** as a public API. All five languages must provide the same
timeout, consumption order, and negative-observation meaning. A
general-purpose assertion unrelated to connector state, such as
condition checking, expected error, and timeout verification, isn't a
connector public contract. E2E owns that auxiliary code in each
language's `Client/Support`.

#### 10.1.1 Push Observation Surface — The `waitFor` Family

Something that can only be judged by observing the receive message
queue (§10). A method of the connector instance.

The surfaces below provide **both** paths for the packet name: naming
it at the call site, and deriving it from the payload type. Neither
path is offered alone. The exact argument and overload, and the
completion terminator (`.Async`/`.submit`/`.run`), are owned by each
language's document, and the remaining conditions are narrowed by
builder chaining.

- **The predicate and the return value carry messages, not payloads.**
  A payload alone hides the metadata and the packet name from the
  predicate. `T` is the type of the payload the message carries.
- **A failure of the observed condition is `ValidationFailed`** — not
  arriving within the timeout, arriving when it should not have,
  arriving out of order. **Where the connection has ended and the wait
  cannot continue, it is `Disconnected`.** That covers a call left
  waiting when the connector is closed or the connection drops: the
  condition did not fail, the place to observe it went away. How either
  reaches the caller is settled by §9.2 — as a value where exceptions
  are disabled, as an exception carrying the code everywhere else.

| Surface | Contract | Failure |
|------|------|------|
| `waitFor<T>(name)` | Waits until that packet arrives. Narrowed with `.where(predicate)`/`.timeout(t)`. The default timeout is §6.1's `wait timeout` (5 seconds) | **Fails with an error** if it doesn't arrive within the timeout (§10 specifies this surface consumes the queue) |
| `expectNone<T>(name)` | Confirms that packet **doesn't arrive** during `.within(window)` (negative). The symmetric of `waitFor` | **Fails with an error** if it arrives within the window |
| `waitForSequence<T>(name)` | `.expect(p1).expect(p2)….timeout(t)` — confirms a push of the same name arrives **in the given predicate order** and returns the message list | **Fails with an error** if the order is wrong or it times out. This surface exists to verify **"arrived in order"**, not "N arrived" |

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
| `.NET` (desktop/server) | `Zlink.Stream.Connector` | NuGet |
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
| Connection lifecycle | State transition/reconnect/heartbeat (§6) |
| **Option validation** | **Every option is checked before a connection is made; a value out of range is rejected as `ValidationFailed` and a mismatch between options as `ConfigurationError` (§6.3)** |
| **Transport inference** | **An unspecified transport is settled by the endpoint scheme, and a specified one that conflicts with the scheme is `ConfigurationError` (§3.1)** |
| **Error delivery** | **The receiver can tell which of §9's thirteen it is. A language's standard exception type is not used as is (§9.2)** |
| **Reconnect delay** | **The wait between attempts falls between 50% and 100% of the base delay. When the attempts run out the state becomes `Disconnected` and disconnect handlers run (§6)** |
| **Unregistration** | **All four registrations — push, error, disconnect, connection state — return a value that unregisters, and an unregistered handler is not run by later dispatches (§7)** |
| **Received count** | **`receivedCount(name)` counts what arrived, does not fall on consumption, and is independent of dispatch mode. It restarts at zero when the connection is established (§10)** |
| **Wait surfaces** | **Both the named path and the payload-type path exist, the predicate and the return value carry messages, a failed condition is `ValidationFailed`, and an ended connection is `Disconnected` (§10.1)** |
| **Flow exposure and propagation** | **A received message exposes the flow identifier and origin, and a runtime without an ambient context provides an explicit means of passing it (§5.5)** |
| **Close reason read surface** | **Code that did not receive the event reads the same value. A failed first connect still leaves a reason, and reconnecting does not clear it (§6.2)** |
| Diagnostics level | `Off` outbound frames carry no flow field/flag (0x10), inbound flow value validation is skipped, the `Errors` default keeps the current wire, and one-way `Send` carries no correlation id (§13) |

## 13. Diagnostics Level

The connector takes a diagnostics level as a creation-time option. The values and their
meaning are the four values `Off`/`Errors`/`Normal`/`Detailed` of
[Message flow tracing §4](../server/06-observability/03-message-flow-tracing.en.md#4-how-the-application-sets-the-recording-scope--level-and-sampling),
and **the default is `Errors`.** The connector is subject to the client connector rule of
[Flow correlation §4](../server/06-observability/04-flow-correlation.en.md#4-when-a-flow-is-created), so a
runtime level change follows
[Message flow tracing §4.1](../server/06-observability/03-message-flow-tracing.en.md#5-changing-the-record-level-at-runtime-and-the-cost-rule)
as is. The application can read and change the level without recreating the connector, the
change applies from the processing points after it, and already-built frames are not
retroactively changed. Each processing point reads the current level once and decides with
that value.

**A surface that reads the level and a synchronous surface that changes it are provided.**
Changing the level alters one value, so there is no completion for the caller to wait on. Where
asynchronous completion is that language's idiom, an asynchronous counterpart with the same
meaning is placed alongside — both surfaces change the same value, and the asynchronous
counterpart does not replace the synchronous one. Implementing the synchronous surface as a
blocking call over the asynchronous counterpart would make that call wait on its own completion
inside a receive callback, so the synchronous surface changes the value without waiting. The
names of both surfaces are owned by the per-language document.

When the level is not `Off`, the connector keeps the current behavior: it creates and
attaches `flow_id`/`flow_origin` to outbound frames (§4.2, flag `0x10`) and validates the
inbound frame's flow fields, delivering them on the received message.

When the level is `Off`, the client connector rule of
[Flow correlation §4](../server/06-observability/04-flow-correlation.en.md#4-when-a-flow-is-created) applies
as is.

- Outbound frames neither create nor attach `flow_id`/`flow_origin` (flag `0x10` is not
  set).
- Inbound flow fields keep **only the structural length check**; value validation
  (UUIDv7/origin range) and delivery on the received message are skipped.
- No observation-only work — flow creation, validation, propagation — is performed.

The diagnostics level does not affect protocol information. A request's correlation id is
created and preserved even at `Off`. Conversely, **a one-way `Send` never creates a
correlation id at any level** — [Flow correlation §2](../server/06-observability/04-flow-correlation.en.md#2-the-role-of-the-two-identifiers)'s
"a one-way message without a reply doesn't create a `correlation_id`" applies to the
connector as well (flag `0x08` is not set).

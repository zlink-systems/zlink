---
title: "Receiving Packets · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/05-receiving.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Receiving Packets

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Sending Packets](04-sending.en.md) | [Next: Connection Lifecycle](06-lifecycle.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/05-receiving.en.md) · **C#/.NET** · [Java](../../../java/guide/stream-connector/05-receiving.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/05-receiving.en.md) · [Node/TypeScript](../../../node/guide/stream-connector/05-receiving.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can receive packets the server sends first through a handler and release that registration
    when you choose. You can also wait for a single packet and read how many arrived under a name.

A packet the server sent stays in the **receive queue** until a handler takes it or a wait surface
consumes it. This chapter covers how packets leave that queue. Answers and heartbeats do not pass
through it — an answer goes straight to the request waiting for it, and a heartbeat serves the
connection itself.

## 1. Typed Receiving and Decoding

The connector finds a handler by the incoming packet name, decodes its payload, and places it in a
message. A message carries the packet name, payload, metadata, and Actor ID. A codec converts
payload bytes into an application type. One codec is configured when the connector is created.

### 1.1 Receiving — Registering a Typed Handler

These registrations come from the tutorial. The .NET, Java, Kotlin, and C++ examples receive
`NicknameChanged` in the existing JSON flow. The Node example receives the generated Protobuf
class `Ping`. Each tutorial README provides the execution procedure.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-push-en.html" title="Node typed Protobuf receiving example" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-push-en.html" target="_blank">↗ Open larger</a></p>

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:typed-receive"
```

### 1.2 How the Type Reaches the Codec

`on(Type, handler)` derives the packet name from the type and decodes the payload as that type.
The named form selects packets by an explicit wire name while specifying the decoding type
separately. This is useful when the type name differs from the server's packet name.

The `T` in `On<T>(handler)` and `On<T>(name, handler)` reaches `PayloadCodec.Decode<T>(payload)`.
Set the `PayloadCodec` option to `ZLinkProtobufCodec.Default` for Protobuf.

!!! note "Node Protobuf Codec"

    The patched codec decodes with the constructor supplied by `on(Type, handler)` and uses the factory fallback when no type is supplied.
    The envelope codec also forwards the handler type. [Node Protobuf Messaging](../../../node/guide/stream-connector/40-protobuf.en.md)
    describes the difference from released 0.28.0 and the execution steps.

## 2. Releasing a Registration

Registration returns **a value that can be released**. This keeps a client that registers a
subscription for the lifetime of one screen from having to rebuild the connection when the screen
closes. A released handler does not run afterwards, and releasing the same value twice is not an
error.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Receiving.cs:receiving-unsubscribe"
```

**Whether the value's lifetime is the registration's lifetime is decided by the language.** In a
language that expresses ownership as a value, the registration ends when the value goes away, so
the value is kept for as long as the handler must run. In the others the registration stays until it
is released explicitly, even if the value is discarded.

The connection-state, error, and disconnect handlers return the same kind of value. They are
covered by [Connection Lifecycle](06-lifecycle.en.md).

## 3. When a Handler Runs

Under the default setting the receive path does not call handlers directly; it queues them. When
the application calls the pump, the handlers queued so far run **in the execution context that
called it**. A game loop calls it once per frame. The pump processes what has accumulated and
returns; it does not wait for a new packet.

Switching to immediate execution runs handlers on the receive path with no pump. A slow handler then
blocks that path and delays the receive work behind it.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Receiving.cs:receiving-pump"
```

The wait surfaces below observe the receive queue directly rather than running registered handlers,
so they work in a configuration that never calls the pump.

## 4. Waiting for One Packet

To wait for a single packet at a point in a scenario, use a wait surface instead of registering a
handler. It consumes the matching packet and returns that message; a packet that does not match
stays in the queue for a later handler or wait. Without an explicit timeout, the connector's default
wait timeout applies.

A packet name can be explicit or derived from the payload type.

Use `WaitFor<T>()` or `WaitFor<T>(name)`.

The example below waits for one packet by payload type.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Receiving.cs:receiving-wait"
```

**Predicates and returns deal in messages, not payloads.** A predicate given the payload alone
cannot see the packet name or the metadata. When the value to filter on sits inside the payload,
read that field inside the predicate — there is no surface dedicated to a status field.

## 5. Packets That Must Not Arrive, and Arrival Order

Verifying a scenario means confirming not only that a packet arrived, but also that one did not and
that several arrived in order. The first names an observation window and confirms the packet does
not arrive during it. The second applies predicates in order, confirms packets of the same name
arrived in that order, and returns the list of messages.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Receiving.cs:receiving-sequence"
```

A failed observation — nothing arrived in time, something arrived that should not have, the order
was wrong — is reported as a validation failure. A wait that cannot continue because the connection
ended is reported as no connection. The caller has to tell an observation that did not hold from an
observation that lost its subject.

## 6. Received Counts

The count of packets **received** under a name is readable per name. Consuming one does not lower
it, so the count still answers how many arrived under that name after a handler has run through the
pump. The count also rises when the packet arrives, whatever the setting for when handlers run.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Receiving.cs:receiving-count"
```

The reference point is the moment the connection is established. The count starts at zero then, and
a reconnect is a new connection, so it starts at zero again. Unconsumed messages left from the
previous connection are cleared at the same moment — otherwise a wait surface would hand back a
packet from before the drop as if it belonged to the new connection.

## 7. The Receive Queue — No Limit Is Set

The connector keeps receiving and processing what arrives. There is no limit on the queue, no
message is dropped, and a long queue is never a reason to close the connection. The connector does
not implement a socket of its own and uses what the runtime provides, so there is no place to hold
reads back either.

In a client that works correctly, messages do not accumulate: a handler processes them or a wait
surface consumes them. Continued growth means the client is not calling the pump, which is why the
received count is not a basis for flow control.

## 8. With Several Actors — Tell Them Apart by Actor Handle

### 8.1 With One Actor

When the server binds only one Actor to this connection, keep the send and receive code you have. A
packet sent from the connector carries no Actor slot, and the server session hands a packet without a
slot to the one Actor bound to that connection
([Session and Actor Connection](../server/24-actor-session.en.md#32-forwarding-what-is-left)).

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:single-actor-send"
```

### 8.2 Get Handles from the Bound Notice

When the server binds several Actors to one connection, the connector creates an **Actor handle** for
each. When the server binds an Actor, the bound notice arrives before that Actor's first packet, and
when the binding ends, the unbound notice arrives after its last packet. The example below registers
both notices and finds the handles of the authenticated `p1` and `p2` by Actor ID.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-events"
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-send"
```

### 8.3 Send Through a Handle and Receive per Handle

A packet sent through a handle carries that Actor's slot, and the server session hands the packet to
that slot's Actor. A receive handler registered on a handle receives only the messages that Actor sent.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-per-handle-receive"
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-send-call"
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-receive"
```

Running it prints `pushed: speedy-p1, actor: p1` and `pushed: speedy-p2, actor: p2`.

### 8.4 Tell Actors Apart by Actor ID at the Connector Level

You can also receive at the connector level without a handle. The message's Actor ID then tells you
the server-side Actor. A message that arrived without a slot has an empty Actor ID.

```csharp
--8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-id-receive"
```

### 8.5 A Packet Sent to an Unbound Actor

An unbound handle is closed. Sending through a closed handle doesn't send and ends with
`ValidationFailed` in the connector. If a packet sent just before the unbind reaches the server late,
the server doesn't hand it to another Actor — a request ends with an `InvalidOperation` error reply,
and a one-way send is dropped.

## 9. Tutorial Execution Result

The unsubscribe, pump, predicate wait, sequence, and count examples run in each language's StreamClient `--receiving` mode.
`STREAM_RECEIVING_ENDPOINT` is the address of a server sending JSON packets. Node uses WebSocket; the other examples use TCP.
The verification peer sends `LeaderboardUpdate` twice, followed by `MatchFound` and ordered `OrderChanged` messages.
All five language programs produced the same result.

```text
receiving: handler=1, frames=1, match=match-7f3a, sequence=paid,shipped, count=2
```

The pump runs the handler once. After unsubscribe, the second packet still increases the received count without invoking the handler.
The program checks the matching message, the paid/shipped sequence, and the absence of OrderChanged before the sequence starts.

## 10. Next Chapters

- Connection state, reconnection, close reasons — [Connection Lifecycle](06-lifecycle.en.md)
- Errors raised on the receive path — [Error Handling](07-error-handling.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>

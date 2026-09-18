---
title: "Sending Packets · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/04-sending.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Sending Packets

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Connector Options](03-connector-options.en.md) | [Next: Receiving Packets](05-receiving.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/04-sending.en.md) · **C#/.NET** · [Java](../../../java/guide/stream-connector/04-sending.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/04-sending.en.md) · [Node/TypeScript](../../../node/guide/stream-connector/04-sending.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can tell a send that waits for an answer from one that does not, and you can fix the
    packet name and the metadata a packet carries.

A send call does not transmit immediately; it returns a builder. After the name, metadata,
compression, and timeout are set, **the terminal call starts the transmission.** A builder whose
terminal is never called does nothing.

## 1. send — Transmission Without an Answer

Packets such as position updates or input, where the server's answer is not needed, are sent with
send. The terminal reports completion and failure only; it carries nothing about what the server
did with the packet.

```csharp
await connector.Send(new PositionUpdate(102.5f, 0.0f, -44.3f))
    .PacketName("player.position")
    .Async();
```

## 2. request — Transmission That Waits for an Answer

Packets such as a login or a lookup, where the server's answer is needed, are sent with request.
An answer is matched by the sequence assigned to each request rather than by the packet name, so
several requests may be in flight and each completes on its own regardless of arrival order.

A timeout can be given per call; without one, the connector's default request timeout applies.

```csharp
var reply = await connector.Request(new LoginRequest("player-1", "tok-abc123"))
    .PacketName("auth.login")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<LoginReply>();

var sessionId = reply.SessionId;
```

When the connection drops, every pending request fails. None of them is retransmitted after a
reconnect, so the application decides which requests to send again.

## 3. How the Packet Name Is Decided

The server picks its handler by packet name. The name is decided in this order.

1. The name the caller set on the builder
2. The name declared on the payload type
3. The simple name of the payload type

No namespace or package qualifier is added. Names that vary with the build environment, such as a
compiler-generated name, are not used either — the server would fail to find a handler for the same
type.

When the same name is sent from several places, declaring it on the type is the better choice: the
name is then not repeated at every call site.

```csharp
[ZlinkStreamPacketName("order.changed")]
public sealed record OrderChanged(string OrderId);
```

Set the name at the call site only when sending an already encoded payload for an external
protocol.

## 4. Metadata

Metadata is a set of key-value pairs attached to a packet. It is the place for small values that
do not belong in the payload, such as a trace id, a locale, or a client version. The values are
copied at transmission time, so editing the original afterwards does not change the packet that
went out.

```csharp
await connector.Send(new ChatMessage("room-42", "hello"))
    .Metadata("x-locale", "ko-KR")
    .Metadata("x-client-version", "2.4.1")
    .Async();
```

Metadata cannot exceed 1024 bytes in total, and that limit is not adjustable through an option. A
packet that carries the same key twice, or an empty key, is rejected as well. Larger values belong
in the payload.

## 5. Asking for Compression

The compression algorithm is fixed when the connector is created, but that setting alone does not
compress every packet. **Only a send that asks for compression is compressed.** Asking for it on
large payloads such as a map chunk avoids paying the cost on small packets.

```csharp
await connector.Send(new WorldChunk(chunkData))
    .PacketName("world.chunk")
    .Compress()
    .Async();
```

In a configuration with compression turned off, asking for compression fails that send. Compression
applies to the payload only, never to the header.

## 6. Codec

The codec that turns a payload into bytes is chosen once, when the connector is created. There is
no surface for choosing one per call, so sending code deals only with the payload type and the
name. The default codec is JSON; MessagePack and Protobuf are provided by optional packages.

An integration that must forward already encoded bytes keeps the codec number recorded in the
payload.

## 7. Size Limits

A send payload over the limit fails **before the write to the transport**. The connection stays, so
only that call fails and other packets are unaffected. A send that asked for compression is
compared against the limit after compression.

To exchange payloads larger than 64KB, raise the send limit explicitly in
[Connector Options](03-connector-options.en.md).

## 8. Next Chapters

- Receiving what the server sends — [Receiving Packets](05-receiving.en.md)
- Where a send fails and with which code — [Error Handling](07-error-handling.en.md)

---
title: "Connector Options · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/03-connector-options.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Connector Options

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Installation and the First Connection](02-getting-started.en.md) | [Next: Sending Packets](04-sending.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/03-connector-options.en.md) · [C#/.NET](../../../dotnet/guide/stream-connector/03-connector-options.en.md) · [Java](../../../java/guide/stream-connector/03-connector-options.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/03-connector-options.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You know the values fixed when a connector is created, their defaults, and when an invalid
    configuration is rejected.

Options are passed once, when the connector is created. The connector keeps a copy and its read
surface returns the values actually in use.

## 1. Where Options Are Fixed

Anything left out takes its default. The smallest configuration is an endpoint.

```typescript
const connector = zlinkStreamConnectorFactory.create({
  endpoint: 'wss://game.example.com:443/stream',
  codec: zlinkStreamJsonCodec,
  connectTimeoutMs: 5_000,
  requestTimeoutMs: 10_000,
  reconnect: { maxAttempts: 5 },
  dispatchMode: ZlinkStreamDispatchMode.Manual
});
```

## 2. Defaults

| Item | Default |
|---|---|
| Connect timeout | 5 seconds |
| Request timeout | 30 seconds |
| Wait surface timeout | 5 seconds |
| Heartbeat | on — interval 1 second, timeout 5 seconds |
| Automatic reconnect | on — initial delay 250ms, maximum delay 5 seconds, backoff factor 2.0, 3 attempts |
| When receive callbacks run | pumped by the application |
| Codec | JSON |
| Compression | Lz4 |
| Send and receive payload limits | 64KB each |
| TLS certificate validation | on |

The names differ per language, but the defaults are the same, because the same configuration has
to behave the same in a game engine and in a desktop tool.

## 3. Endpoint and Transport

The endpoint scheme decides the transport. A configuration with only a `ws://` endpoint connects
over WebSocket with nothing else set.

The separate transport option is not a place to override the scheme; it is a place to **confirm
that the two agree**. A transport that disagrees with the endpoint scheme fails as a configuration
error, and so does `tcp://` or `tls://` on a browser runtime.

## 4. Timeouts

The timeouts bound different work.

- Connect timeout — how long one connect attempt may take
- Request timeout — how long an answer may take. Can be overridden per call
- Wait surface timeout — how long a specific packet may take to arrive. Can be overridden per call

When a request timeout expires, only that request fails and the connection stays. An answer that
arrives late does not complete a request that was already removed.

## 5. Heartbeat

The heartbeat checks periodically that the connection is alive. When it is on, the connector sends
a control packet on the configured interval, and if no frame arrives for the configured timeout it
treats the connection as dropped and applies the reconnect policy.

Turning the heartbeat off stops the outbound interval, not the duty to answer: an inbound ping
still gets a reply.

## 6. Automatic Reconnect

Automatic reconnect is on by default. The delay between attempts starts at the initial delay, is
multiplied by the backoff factor on each attempt, and stops at the maximum delay. The time actually
waited is drawn between 50% and 100% of that value — when a server drops once, every client that
was connected would otherwise reconnect at the same instant, which is exactly when the server is
under the most load.

A client that keeps trying until the connection comes back specifies **unlimited** rather than a
large number, so that it is distinguishable from a configuration with a finite count.

```typescript
reconnect: { maxAttempts: null }   // null means unlimited.
```

Reconnect behavior and the disconnect handler are covered by
[Connection Lifecycle](06-lifecycle.en.md).

## 7. When Receive Callbacks Run

Under the default setting the receive path does not call handlers directly; it queues them, and
they run in the execution context that called the pump. This is the default because engine objects
cannot be touched outside the main thread.

Switching to immediate execution runs handlers on the receive path with no pump. A slow handler
then blocks that path and delays the receive work behind it. Use it where there is no main-thread
constraint, such as a CLI or a tool.

The difference between the two settings, and registration, are covered by
[Receiving Packets](05-receiving.en.md).

## 8. Codec and Compression

The codec that turns a payload into bytes is injected once when the connector is created, and JSON
is used when none is given. There is no surface for registering a codec per message type or
choosing one per call. The rule that derives a packet name from a payload type is injected in the
same place.

The compression algorithm is likewise fixed once. A default of Lz4 does not mean every packet is
compressed: **only a send that asks for compression is compressed.** Asking for it in a
configuration with compression turned off fails that call, and a compressed frame received in that
configuration is rejected as a decompression error.

A packet the server compressed is decompressed by the connector before the handler runs.

## 9. Payload and Metadata Limits

The send and receive payload limits are 64KB each and are adjustable. When a compressed frame
arrives, the compressed payload on the wire and the decompressed result are each compared against
the receive limit. An application that exchanges payloads larger than 64KB raises the value
explicitly.

The metadata limit is 1024 bytes in total and **is not adjustable through an option.** Metadata is
the place for small values such as a trace id, a locale, or a tenant id; larger data belongs in the
payload.

## 10. TLS Certificate Validation

TLS and WSS validate the certificate chain and the host name. There is an option to skip that
validation, but it is off by default and is meant for self-signed certificates in tests. Turning it
on in a deployed configuration accepts a server certificate without trusting it.

## 11. When Options Are Validated

**Every option is validated.** Checking only some of them lets a wrong value elsewhere pass
silently, and the caller then cannot tell a configuration mistake from a connection failure.

Validation happens at the earliest point where the language can report the failure. A language
whose creation surface can return a failure validates when the connector is created; a language
whose creation surface has no such channel validates when the connection is attempted. Either way
the configuration is rejected **before a connection is made**, and a configuration that fails
validation never connects.

| Violation | Failure |
|---|---|
| A single value outside its range (negative timeout, zero payload limit) | validation failure |
| Two items that disagree (endpoint scheme against transport, a transport the runtime does not support, a compression codec in a configuration with compression off) | configuration error |

How those failures reach the caller, and how the code is read, are covered by
[Error Handling](07-error-handling.en.md).

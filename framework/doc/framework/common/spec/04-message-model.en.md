---
title: "Framework Message Contract"
---

# Framework Message Contract

[Spec Table Of Contents](README.ko.md) · [Previous: ZLink Framework Interaction Model](03-interaction-model.ko.md) · [Next: Async Execution And The Handler Turn](05-async-execution-policy.ko.md)

> **What this chapter defines** — the public contract for typed messages, application
> metadata, and responses and errors.

This document defines ZLink Framework's typed message, application metadata, and
response/error contract. Its audience is developers implementing the framework's public
contract and each language's service runtime. The framework envelope and its internal
multipart encoding use the same wire schema and golden fixtures across every language, and
each language's service runtime processes it on top of the raw transport. This format is
never exposed to the application.

## 1. Typed Messages

The application sends a message keyed by payload type and a registered handler. The
framework encodes the payload using the default typed JSON serializer and decodes it into
the receiving handler's argument type. The caller never registers a codec, serializer
registry, or encoder/decoder per message type.

A package that needs a separate wire format can use a framework-defined codec extension.
An extension is a host-level policy — it isn't passed repeatedly into a business handler or
individual send/request call. An API that handles raw bytes directly is used only for
transport inspection and codec extension implementation.

## 2. Message Kinds

| Kind | Meaning | Completion |
|---|---|---|
| Send | A one-way message delivered once to a target handler | Completes with no return data once the source-local queue accepts it; doesn't wait for the remote handler to complete |
| Request | A message where the target handler returns a reply or an error | Completes exactly once, with a reply, an error, a timeout, or a cancellation |
| Logical Multicast | A message published to matching Spots at each MeshNode of the target ChannelName | Completes with no return data once source-local capacity is secured and publish begins; doesn't tally per-target counts for monitoring |
| Classic fanout publish | A message published to the subscribers of an independent fanout channel | Completes with no return data once the local publisher queue accepts it; doesn't confirm subscriber receipt |
| STREAM send/request | A one-way message or a reply-requiring message sent to a connected session | Follows the session sequence and lifecycle contract |

A request's reply correlation is owned by an operation ID issued by transport, or by the
[session sequence](01-glossary.ko.md#session-sequence). Neither the packet name nor
application metadata is used as the reply matching key. A reply completes with exactly one
of a success payload or a framework error, and the same request can never complete twice.

An object creation request is a manager-operation input distinct from a regular
send/request. The framework encodes it with the typed codec into at most a 1 MiB payload
and records its immutable content reference and hash into a durable creation intent before
the placement reservation. A factory receives the logical key, ObjectGeneration, and
creation attempt together, and must converge to the same result even under at-least-once
execution of the same attempt. A CAS loser never sends the creation request as a regular
message. The content reference is kept until Ready commit or fenced failure cleanup
finishes. [ObjectGeneration](01-glossary.ko.md#objectgeneration), AuthorityOwnerGeneration,
attempt, and the owner lease token are used only for Store fencing and never included in
the application message payload or handler context.

### 2.1 MessageContext

A regular send/request and an Actor handler receive a common `MessageContext`. This
context provides the current message's nullable MeshName, nullable ChannelName,
PacketName, ContentType, immutable Metadata, and a UTF-8-exact nullable CorrelationId.
CorrelationId is null on a send and non-null on a request. MeshName is non-null on
RouteMesh and Spot/Actor dispatch, and null on ClientServer/STREAM. Connection
cancellation isn't owned by a universal context — it's owned by each language's handler
argument or a Session-specific context. Node direct provides `RouteMessageContext`,
Logical Multicast provides `PublishMessageContext`, and STREAM dispatch provides
`SessionMessageContext`, each carrying that path's additional information.

There's no separate marker context per Send/Request/SpotActor. An Actor request's context
carries no reply metadata/compression option, and there's no separate reply call either. A
handler filter receives a filter-specific context that carries both the current
`MessageContext` info and the public dispatch kind. This value only distinguishes Node
direct send/request, Channel send/request, and classic fanout. It doesn't expose socket
kind, endpoint, framework-internal owner classification, or a dispatch descriptor. Dispatch
kind isn't added to the Spot/Actor/Logical Multicast/STREAM contexts, where a filter
doesn't apply. The Object lifecycle Context and the current message's MessageContext are
separate contracts.

### 2.2 Global Object Reference JSON

`ActorRef`'s and `SpotRef`'s typed JSON contract uses the same property names and JSON
types across every language. Every property is required, and property names are
case-sensitive. A duplicate property, `null`, an unknown property, and an
out-of-range generation are all rejected. Deserialization doesn't normalize the ID or route
string.

```json
{
  "actorId": "player-42",
  "objectGeneration": "17",
  "meshName": "game",
  "nodeRid": "game-node-0123456789abcdef0123456789abcdef"
}
```

```json
{
  "spotId": "room-42",
  "objectGeneration": "9",
  "meshName": "game",
  "nodeRid": "game-node-0123456789abcdef0123456789abcdef"
}
```

`actorId` and `spotId` are global logical IDs, and `meshName`/`nodeRid` are the location
snapshot at the time of lookup. `objectGeneration` is a decimal string with no leading
zero, from `"1"` to `"9223372036854775807"`. Numeric tokens, a sign, a decimal point, and
an exponent are not allowed.

## 3. Application Metadata

Application metadata is a small key-value snapshot carried separately from the business
payload. Node direct, [ChannelName](01-glossary.ko.md#channelname), Spot direct, Actor, and
STREAM send/request all use the same contract.

| Item | Contract |
|---|---|
| Key and value | UTF-8, and never contain NUL |
| Total size | At most 1024 bytes, including the encoded key and value and structural overhead |
| Same key | The value most recently set on the outbound builder applies |
| Receiving | The handler context provides an unmodifiable [snapshot](01-glossary.ko.md#snapshot) |
| Lifetime | Valid until the handler turn ends; the application copies a value if it needs to keep it |
| Malformed input | Treated as a protocol error, without calling the handler |
| Reply | Request metadata isn't auto-copied, and a regular reply provides no metadata setter |

Metadata's internal frame layout and encoding aren't part of the public contract. The
framework maintains the boundary between payload and metadata, and never makes the
application assemble or parse a frame, even on a path that needs relaying.

## 4. Delivery Rules

| Path | Metadata delivery |
|---|---|
| [Node direct](01-glossary.ko.md#node-direct) and ChannelName | The source snapshot is delivered to the handler context of the selected [MeshNode](01-glossary.ko.md#meshnode) |
| [Spot](01-glossary.ko.md#spot) | Delivered to the application claim at the global Spot ID's current [Ready](01-glossary.ko.md#ready) owner |
| [Logical Multicast](01-glossary.ko.md#logical-multicast) | The same publish snapshot is delivered to each matching Spot handler |
| Actor | Delivered to the Actor handler context, without passing through a Spot callback |
| STREAM session | Delivered to the session send/request context |
| Relay from a bound session to an Actor | Only keys allowed by the root metadata policy's session-to-actor allowlist are delivered |
| Relay from an Actor to a bound session | Only keys allowed by the root metadata policy's actor-to-session allowlist are delivered |

When the framework creates a new request, it doesn't auto-copy the original metadata. It's
included in the new outbound snapshot only if the caller explicitly passed the current
handler's metadata. Trace information that needs auto-propagation is managed as a separate
framework field by [Message Flow Correlation](27-flow-correlation.ko.md).

## 5. Ownership And Size Limits

The caller owns the outbound builder and payload until the submit call returns. Once the
framework accepts the submit, it keeps the needed payload/metadata reference or a copy for
the operation's lifetime. The caller is never made to manage the lifetime of a transport
buffer, native message pointer, or multipart part.

The message context, metadata, and payload view passed to a handler are read-only during
the callback. The application doesn't dispose of them, and it copies whatever value it
needs to keep after the callback ends. The framework cleans up the lifecycle of received
payload storage, reply correlation, and the route envelope together with callback
completion.

The same ownership rule applies while an object creation is pending. So that Location
Store I/O and the [factory](01-glossary.ko.md#factory) don't depend on the caller's payload
object or native buffer lifetime, the framework's service runtime pins the immutable
encoded payload in the content store. After Ready or fenced failure, it releases, once,
the payload storage owned by that attempt.

The payload's max size follows the target transport's `MaxMessageSize`. If the whole
message exceeds the limit, no part of it is delivered, and the entire submit or receive
fails. Per-target submission and result aggregation for Logical Multicast are defined by
[Spot Messaging](12-spot-messaging.ko.md).

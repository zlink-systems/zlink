---
title: "STREAM Server Session"
---

# STREAM Server Session

[Spec table of contents](README.en.md) · [Previous: Spot/Actor Routing](18-object-routing.ko.md) · [Next: Session-Actor Dispatch](20-session-actor-dispatch.en.md)

> **What this chapter defines** — the public contract for a server-side STREAM
> session, the execution unit keeping packet processing and request correlation
> from accepting one connection until it closes.


This document defines the public contract for a server-side STREAM session — the
execution unit keeping packet processing and request correlation from accepting
one connection until it closes — in ZLink Framework. Its audience is framework
developers implementing the server session surface, dispatch, registration,
codec, and error boundary. The client-side contract is defined by the
[Stream Connector Common Spec](stream-connector/32-stream-connector.en.md), and
the two documents share the same wire contract. Per-language types and
signatures are fixed by the STREAM documents in the
[per-language Server interface table of contents](server/languages/README.ko.md).

## 1. Purpose

STREAM differs in nature from regular request-response. The following become
much more important axes.

- Connection lifetime
- Peer identification
- Packet framing
- Session lifecycle

The framework handles STREAM as a header-based packet session. It doesn't hand
the raw byte stream directly to the application.

## 2. Basic Direction

- The framework decodes the stream header, puts packet name and metadata into
  the dispatch context, and delivers it to the session callback together with
  a payload not yet converted into a business object.
- The framework operates STREAM transport ingress in `recv` mode in every
  language. It doesn't register Core's STREAM packet callback or raw receive
  callback to receive application packets. This rule applies identically
  regardless of how a per-language binding expresses it.
- The framework's internal `recv loop` reads raw parts and assembles header
  framing. If this loop can't hand a packet to the managed queue, it doesn't
  perform the next receive, so the Core receive pipe's HWM acts as the
  backpressure boundary.
- The application distinguishes the packet to process by
  [packet name](01-glossary.en.md#packet-name) and uses the framework's common
  decoder surface. This surface converts payload into a business object using
  the registered codec registry, so the handler doesn't directly pick a
  per-codec helper (§5).
- The transport body only handles header framing — it isn't responsible for
  converting payload into a business object.
- Session connect and disconnect lifecycle callbacks are provided as a basic
  surface.
- The error callback only delivers a transport error attributed to that
  session — not an application exception (§6).

The following functionality isn't part of this contract's scope.

- A recv loop the application runs directly (§4)
- Direct raw chunk handling
- Custom header framing. The header binary format is an internal protocol the
  framework and connector share — the application isn't given a setting to
  change this format.

## 3. Dispatch Model

The framework's internal `recv loop` doesn't run the application session
callback directly. The framework puts the packet on a managed queue and then
runs the session callback. Framework dispatch, DI, and logging are applied
consistently at this queue boundary.

A handler filter doesn't apply to
[STREAM session dispatch](01-glossary.en.md#stream-session). The scope and
execution rules of filters on other dispatches are set by
[Framework API §8.1](06-framework-api.en.md#81-handler-filter).

- A session callback receives a dispatch context holding packet name, metadata,
  and request information, along with payload.
- The runtime preserves request header values inside the dispatch context. The
  application doesn't build a header object or pass it into a relay call
  again.
- The routing ID — the peer identity value obtained from the `recv` result —
  is delivered to session dispatch without information loss.

### 3.1 Reply Correlation

The `Response` and `Error` a session builds return the original request's
request sequence as-is. The client only uses this sequence to find the
pending request.

- The `Response`/`Error` header doesn't carry a packet name. A reply doesn't
  select a handler, so that field isn't needed, and filling it with a
  different value per language only breaks diagnostics.
- The typed reply's decode type is the type the client specified at call time —
  not selected by name.
- `Error` also returns the same sequence.

The full rule is defined by [Message Model](04-message-model.en.md)'s reply
correlation contract.

## 4. The Framework's Internal Recv Loop And The Application Surface

The framework internally owns the `recv loop` but doesn't expose a raw receive
loop as an application public surface. The application only uses the session
callback — receive order, cancellation, backpressure, and header framing are
managed by the framework.

Transport ingress in every framework language follows this boundary.

```text
Core receive pipe
    -> Framework recv loop
    -> header framing and queue admission
    -> session callback
```

Bypassing queue admission using the Core packet callback or raw receive
callback means the Core receive pipe's HWM can't bound the application queue,
so it doesn't satisfy the framework contract. The framework doesn't read a new
packet while queue admission is failing, doesn't drop an already-received
packet, and doesn't redeliver the same packet to the callback.

## 5. Codec Layer Separation

The framework's basic surface only provides session, session context, stream,
and message.

- Object conversion is handled by a separate codec extension operating on the
  framework message, not the raw transport message.
- A specific codec implementation isn't directly mixed into raw transport or
  the framework's basic runtime.
- A session handler doesn't directly call a per-codec helper. Even when
  switching between JSON/Protobuf/MessagePack/custom codec, business code uses
  the same decode surface.
- The server framework, HTTP client host, and stream connector share the codec
  number, content-type, and typed-payload-selection contract, but don't share
  a registry instance. The server owns a per-server-root registry; the HTTP
  client owns a per-host registry
  ([HTTP Client §5](http-client/12-http-client.en.md#5-codec)); the connector
  owns a per-connector-instance typed codec option
  ([Stream Connector §5.4](stream-connector/32-stream-connector.en.md#54-codec)).

## 6. Error Boundary

| Error | Where it goes |
|---|---|
| A transport error attributed to that session | Delivered to the session error callback. |
| Handshake failure | A failure before the session was built, so there's no target to run a session callback on. Only recorded in runtime monitoring. |
| A socket/node-level error | Can't be attributed to a single specific session's error, so a session callback isn't run — it's recorded in runtime monitoring. |
| An application handler exception | Not a transport error, so the session error callback isn't run — the handler exception-handling path is used instead. |

The session error callback is restricted to only the axis of re-surfacing a
monitor-observable transport error at the session level.

The termination reason when a session closes matches the closed set in
[Stream Connector §6.3](stream-connector/32-stream-connector.en.md#63-close-reason),
and the instrument is owned by
[runtime-metrics §4](25-runtime-metrics.en.md#4-object-and-stream).

## 7. Registration Model

A stream node is registered explicitly. Implicit registration based on
attributes or decorators isn't provided.

The following .NET excerpt shows how to register a bind endpoint, TLS, and
session type on one Stream node. It doesn't require the same signature in
other languages; the exact .NET contract is defined by the
[.NET Configuration Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

```csharp
public interface IZLinkStreamNodeBuilder
{
    IZLinkStreamNodeBuilder Bind(string endpoint);
    IZLinkStreamNodeBuilder Bind(int port = 0);
    IZLinkStreamNodeBuilder SetBindHost(string bindHost);
    IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkSocketConfig ConfigureSocket();
    IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false);
    IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession;
}
```

```csharp
options
    .AddStreamNode("gateway")
    .Bind(7400)
    .SetBindHost("0.0.0.0")
    .SetAdvertiseHost("node-a.example.net")
    .ConfigureSocket().MaxMessageSize = 64 * 1024; // default cap for complete STREAM messages received from client to server.
    .SetTlsServer(
        "server.crt",
        "server.key",
        requireClientCertificate: true)
    .AddSession<GatewaySession>(); // registers the single session type this node uses.
```

`ConfigureSocket().MaxMessageSize` is this StreamNode's Core STREAM inbound option. Its
default is `64 KiB`; a complete message is measured as header bytes plus payload bytes,
excluding the 6-byte prefix. The cap applies only to messages received from client to
server, not to messages sent from server to client. `0` maps to Core `-1`, meaning no
separate Framework cap, while a positive value is finite. A negative value is a startup
configuration error. A message above the cap is not partially delivered to the session
handler; the server records `EMSGSIZE` and closes the connection. No error code is sent
to a raw client, so it observes only the connection closing.

Axes of the registration surface:

| Axis | Meaning |
|---|---|
| Stream node name | Identifies the node. |
| Bind endpoint | Must be specified. |
| Session type registration | Only one session is registered per Stream node. |

### 7.1 TLS

A Stream node can use TLS. Enabling TLS requires specifying a certificate path
and key path together. Whether to require a client certificate is chosen in
the same server TLS configuration. The default is not to require it; if set to
require it, a connection that fails client-certificate verification is
rejected before a session is built. The client-side transport choice is
determined by the endpoint scheme
([Stream Connector §3](stream-connector/32-stream-connector.en.md)).

### 7.2 Startup Validation

The following fail as configuration errors before the host starts.

| Condition | Result |
|---|---|
| The stream node name is empty. | Fails startup as a configuration error. |
| The same stream node name was registered twice. | Fails startup as a configuration error, since node name is a runtime identifier. |
| No bind endpoint. | Fails startup as a configuration error. |
| The same session type was registered twice. | Fails startup as a configuration error. |
| Two or more sessions registered on one node. | Fails startup as a configuration error. |
| TLS is enabled but the certificate path is empty. | Fails startup as a configuration error. |
| TLS is enabled but the key path is empty. | Fails startup as a configuration error. |
| Client certificate was required without configuring TLS server. | Fails startup as a configuration error. |

It must be clear at registration time that this node is a header-based packet
path.

## 8. From Session To Actor

The contract for handing a packet a session received off to an actor is owned
by [session-actor-dispatch](20-session-actor-dispatch.en.md).

A session callback doesn't directly change [Spot](01-glossary.en.md#spot)
state. It only proceeds as far as submitting an Actor dispatch or Spot call
([Stage Wrapper On Spot §3](17-stage-wrapper-on-spot.en.md#3-preserving-the-spot-turn)).

Even if the Actor is on a different MeshNode, the physical STREAM socket and
session object are kept on the current session owner. The framework only
delivers bind control, Actor ingress, and Actor push as raw ROUTER service
records between [MeshNode](01-glossary.en.md#meshnode)s. It doesn't expose the
target Node RID, binding generation (the order in which a binding was replaced
within the same session [owner](01-glossary.en.md#owner) process lifecycle),
authority fence, or command 24/36/38's codec to the application. When closing
a session, a tombstone of the current
[binding generation](01-glossary.en.md#binding-generation) is submitted, so a
late-arriving close from a previous bind can't release a new binding. The
exact per-command delivery contract is defined by
[Session-Actor Dispatch §4](20-session-actor-dispatch.en.md#4-how-a-session-holds-an-actor-route).

On a physical disconnect, the framework automatically notifies every Actor in
the current binding snapshot via its stored route. An application callback
doesn't iterate the bound list itself. One Actor's failure doesn't block
notification and cleanup of other Actors, and the Spot disconnect callback
runs at most once per exact binding identity. Public `NotifyDisconnected`
remains a logical notification the application sends while the connection is
kept.

If a bound Actor relocates, the physical STREAM socket and Session object are
kept as-is. Once the target Actor is restored and owner/membership commit
finishes, the target Actor starts processing messages. Then the target
runtime sends `sessionActorLocationUpdateReqMsg`, asking the session owner to
update that Actor's binding route — stored by the session owner as the path to
deliver to the current Actor owner — to the target owner. Along with the route
switch, the bound-session current Actor location snapshot is also updated to
the target MeshName/NodeRid, keeping the same ActorId/ObjectGeneration. The
route and location snapshot of a different Actor on the same Session not
included in the relocation don't change. Once updated, the session owner
sends `sessionActorLocationUpdateResMsg`. Without a response, the target
runtime resends the same request starting 1 second after the first send, at
intervals of 1, 2, 4, 5 seconds, then keeps a 5-second interval afterward. The
target Actor keeps processing messages while waiting for the response, and a
message arriving on the previous route is delivered by the Message Follow
route. A route update is only allowed for the same ObjectGeneration, and the
application doesn't rebind to learn about the relocation. A new incarnation
needs an explicit bind.

## 9. Implementation And Contract-Test Verification Requirements

| Item | Verification |
|---|---|
| Dispatch path | The framework's internal recv loop reads a packet, passes it through the managed queue, and then runs the session callback |
| Peer identity preservation | The [routing id](01-glossary.en.md#routing-id) from the recv result is delivered to session dispatch without loss |
| Registration validation | Registering two or more sessions on the same node fails at startup |
| Error boundary | A handshake/socket error doesn't surface to the session error callback |
| Auth and dispatch | Authentication and packet dispatch complete between the connector and session node |
| Termination and resumption | A stream termination fails a pending request, and messaging resumes after a new session's auth/bind |
| Reply correlation | The `Response`/`Error` header has no packet name, and the client matches by sequence alone to complete normally |
| Cross-node Actor delivery | The physical STREAM stays on the session owner — only command 38/24/36 records are delivered between MeshNodes |

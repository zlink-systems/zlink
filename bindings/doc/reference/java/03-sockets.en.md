[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `Socket` (the shared base every socket type extends), `CommonSocketOptions`
and its per-type subclasses, the eight concrete socket interfaces, and the handler functional
interfaces. Every socket's `send`/`publish`/`request`/`reply` returns the operation-builder family
documented in the Messaging category — this category only covers where each builder starts and
what each socket type uniquely adds. Unlike dotnet, **there is no shared `IConnectableSocket`
layer** — `bind`/`connect`/`unbind`/`disconnect`/`disconnectRid` are redeclared independently on
each concrete socket interface rather than inherited from one shared connectable-socket tier. The
exact signatures are owned by
[`contracts/sockets/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/sockets/).

---

## `Socket` shared base

The base interface every socket type extends: options, monitoring, TLS, disposal.

```java
socket.setTlsServer(certPem, keyPem, true);
try (SocketMonitor monitor = socket.monitorOpen(MonitorEventType.CONNECTED)) { /* ... */ }
socket.close();
```

**Options.**

| Member | Meaning |
| --- | --- |
| `options()` | returns `CommonSocketOptions` (below) |
| `monitorOpen()` | opens a monitor subscribed to every event |
| `monitorOpen(MonitorEventType... events)` | opens a monitor subscribed only to the given events — varargs, not a bitmask flags parameter |
| `setTlsServer(String certPem, String keyPem, boolean requireClientCert)` | apply before `bind` |
| `setTlsClient(String caCertPem, String hostname, boolean trustSystem)` | apply before `connect` |
| `setSendReadyHandler(SendReadyHandler handler)` | registers a back-pressure-cleared callback |
| `close()` | closes the native socket |

`Socket` itself declares no `bind`/`connect` — each concrete socket interface below redeclares its
own lifecycle methods.

**Completion result.** All members are synchronous with no return value except `options()`/
`monitorOpen(...)` (returns `SocketMonitor`, Eventing category). `Socket extends AutoCloseable`.

**When to use.** Call `setTlsServer`/`setTlsClient` before `bind`/`connect` respectively.

---

## `CommonSocketOptions`

The typed options facade shared by every socket type, reached via `socket.options()`. **Only the
members below are `public`** — `CommonSocketOptions` also declares `affinity`, `rate`,
`recoveryInterval`, `handshakeInterval`, `routeValueMaxSize`, `tos`, `multicastHops`,
`multicastMaxTpdu`, `bindToDevice`, `tcpKeepaliveCount`, `tcpKeepaliveIdle`,
`tcpKeepaliveInterval`, `tcpMaxRt`, `conflate`, `blocky`, `invertMatching`, `fd`, `events`,
`socketType`, and `zmpMetadata`, but every one of those is package-private and unreachable from
application code — a materially narrower public surface than dotnet/cpp's equivalent facade.

```java
socket.options().sendHwm(100_000L);
socket.options().linger(Duration.ofSeconds(1));
socket.options().submitRetryMode(SubmitRetryMode.LOCAL_FAILURE);
```

**Options.**

| Member | Type | Meaning |
| --- | --- | --- |
| `linger()`/`linger(Duration)` | `Duration` | upper bound on how long `close()` waits for pending sends to flush |
| `sendHwm()`/`sendHwm(long)` | `long`, unsigned 64-bit bit pattern | outbound accounted-byte HWM; `0` means unlimited; use `Long.toUnsignedString(long)` to display values above `Long.MAX_VALUE` |
| `recvHwm()`/`recvHwm(long)` | `long`, unsigned 64-bit bit pattern | inbound accounted-byte HWM; same shape as `sendHwm` |
| `sendBuffer()`/`sendBuffer(int)` | `int` | OS-level socket send buffer size |
| `recvBuffer()`/`recvBuffer(int)` | `int` | OS-level socket receive buffer size |
| `sendTimeout()`/`sendTimeout(Duration)` | `Duration` | upper bound on how long a blocking send waits |
| `recvTimeout()`/`recvTimeout(Duration)` | `Duration` | upper bound on how long a blocking receive waits |
| `immediate()`/`immediate(boolean)` | `boolean` | whether a send requires a live connection now, instead of queueing until one exists |
| `ridDuplicatePolicy()`/`ridDuplicatePolicy(RidDuplicatePolicy)` | `RidDuplicatePolicy` | what happens when a peer reuses an existing routing id |
| `connectTimeout()`/`connectTimeout(Duration)` | `Duration` | upper bound on how long connect handshake waits |
| `ipv6()`/`ipv6(boolean)` | `boolean` | whether the socket accepts IPv6 connections |
| `tcpNoDelay()`/`tcpNoDelay(boolean)` | `boolean` | disables Nagle's algorithm when `true` |
| `tcpKeepalive()`/`tcpKeepalive(int)` | `int` (tri-state, not `boolean`) | OS TCP keepalive mode |
| `maxMessageSize()`/`maxMessageSize(long)` | `long` | maximum size in bytes of a single accepted message |
| `backlog()`/`backlog(int)` | `int` | pending-connection queue length for a listening socket |
| `reconnectInterval()`/`reconnectInterval(Duration)` | `Duration` | delay between reconnect attempts |
| `reconnectIntervalMax()`/`reconnectIntervalMax(Duration)` | `Duration` | cap on the reconnect delay |
| `submitRetryMode()`/`submitRetryMode(SubmitRetryMode)` | `SubmitRetryMode` | whether a failed submit retries automatically on local back-pressure |
| `submitRetryTimeout()`/`submitRetryTimeout(Duration)` | `Duration` | retry timeout when `submitRetryMode()` is `LOCAL_FAILURE` |
| `submitRetryAttempts()`/`submitRetryAttempts(int)` | `int` | retry attempt cap when `submitRetryMode()` is `LOCAL_FAILURE` |
| `lastEndpoint()` | `String`, read-only | the concrete resolved bind address |

**Completion result.** Every getter/setter is synchronous.

**When to use.** Set `sendHwm`/`recvHwm` and `linger` before the socket starts exchanging
messages when the defaults don't fit the deployment. Treat the package-private options as
unavailable to application code — a spec-level question outside this reference's scope, not
something to route around.

---

## `PairSocket`

An exclusive one-to-one peering socket with no routing.

```java
try (PairSocket pair = context.createPairSocket()) {
    pair.send().message(Message.from("ping")).submit();
    Received received = new Received();
    if (pair.recv(received, RecvFlags.NONE)) { /* ... */ }
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `bind(String)` / `unbind(String)` | starts/stops listening on an address |
| `connect(String)` / `disconnect(String)` | connects/disconnects to a peer address |
| `disconnectRid(RoutingId)` | disconnects the peer identified by that routing id |
| `send()` | starts the shared `SendOperation` builder |
| `recv(Received result, RecvFlags flags)` | populates `result` with the next message |

**Completion result.** `recv` returns `boolean` — `false` only when `RecvFlags.DONT_WAIT` is set
and no message is available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests.

```java
try (DealerSocket dealer = context.createDealerSocket()) {
    dealer.setRoutingId(RoutingId.from("worker-3"));
    List<Message> reply = dealer.request().message(Message.from("payload")).await();
}
```

**Options.** Adds to `Socket`'s shared surface:

| Member | Meaning |
| --- | --- |
| `bind`/`connect`/`unbind`/`disconnect`/`disconnectRid` | same shape as `PairSocket` |
| `setRoutingId(RoutingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `send()` / `recv(Received, RecvFlags)` | same shape as `PairSocket` |
| `request()` | starts the shared `RequestOperation` builder; no target parameter — DEALER has no API-level peer routing id |
| `options()` | overridden to return `DealerSocketOptions`: `probe()`/`probe(boolean)` (sends an empty probe on connect); `requestTimeout(Duration)` — **set-only, no getter**; `peerWeight(int)` — **set-only, no getter**, load-balancing weight, unlike dotnet's `PeerWeight` which has both |

**Completion result.** `recv` follows the same `boolean` convention as `PairSocket`.

**When to use.** Set `setRoutingId` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request context (`Received.reply()`) or an explicit ROUTER reply surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id, and can reply to a specific peer's request.

```java
try (RouterSocket router = context.createRouterSocket()) {
    router.send(peerRid).message(Message.from("hello")).submit();
    router.setCompletionControlHandler((rid, parts) -> { /* ... */ });
}
```

**Options.** Adds to `Socket`'s shared surface:

| Member | Meaning |
| --- | --- |
| `bind`/`connect`/`unbind`/`disconnect`/`disconnectRid` | same shape as `PairSocket` |
| `setRoutingId(RoutingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `send(RoutingId)` | starts the shared `SendOperation`, addressed to that peer |
| `recv(Received, RecvFlags)` | same shape as `PairSocket` |
| `request(RoutingId)` | Messaging category's `RequestOperation`, addressed to a specific peer |
| `reply(RoutingId, long requestSequence)` | Messaging category's `ReplyOperation`, answering that peer's request |
| `trySendCompletionControl(RoutingId peerRid, List<Message> parts)` | sends an opaque control record to a peer over its existing connection; does not consume `parts` |
| `setCompletionControlHandler(CompletionControlHandler handler)` | registers the callback that receives incoming completion-control records; the callback owns every message in `parts` and must close it once |
| `options()` | returns `RouterSocketOptions`: `mandatory()`/`mandatory(boolean)` (error instead of silent drop on an unknown route); `handover()`/`handover(boolean)` (shorthand over `ridDuplicatePolicy`); `probe()`/`probe(boolean)`; `connectRoutingId()` (`Optional<RoutingId>`, read-only)/`setConnectRoutingId(RoutingId)` — asymmetric naming between getter and setter; `requestTimeout()`/`requestTimeout(Duration)` — **both directions, unlike Dealer's set-only**; `peerWeight()`/`peerWeight(int)` — **both directions, unlike Dealer's set-only** |

**Completion result.** `trySendCompletionControl` returns `boolean` — `false` only when the
completion connection is back-pressured. `recv` follows the `boolean` convention above.

**When to use.** Use `request(peerRid)`/`reply(rid, requestSequence)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer. Use
`trySendCompletionControl`/`setCompletionControlHandler` for an opaque bounded control record
independent from application-level receive.

---

## `PubSocket` / `XPubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; XPUB
additionally surfaces subscriber subscription/unsubscription events.

```java
try (PubSocket pub = context.createPubSocket()) {
    pub.publish("prices").message(Message.from(tick)).submit();
}

try (XPubSocket xpub = context.createXPubSocket()) {
    SubscriptionEvent evt = new SubscriptionEvent();
    if (xpub.receiveSubscriptionEvent(evt, RecvFlags.NONE)) { /* ... */ }
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `bind`/`connect`/`unbind`/`disconnect`/`disconnectRid` | same shape as `PairSocket` |
| `setRoutingId(RoutingId)` | `PubSocket` only — **has no matching `getRoutingId()`**, set-only, unlike dotnet's `IPubSocket` which has both; `XPubSocket` has neither `setRoutingId` nor `getRoutingId` at all |
| `publish(String topicId)` | starts the shared `SendOperation`; `XPubSocket` redeclares its own copy of this method |
| `receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags)` | `XPubSocket` only; populates `result` with the next subscribe/unsubscribe |
| `options()` | both return `PubSocketOptions` (the same facade type — not two separate ones): `verbose()`/`verbose(boolean)`, `verboser()`/`verboser(boolean)` (deliver every (un)subscribe message, including duplicates); `noDrop()`/`noDrop(boolean)` (error instead of silent drop on back-pressure); `manual()`/`manual(boolean)` (subscriptions require `approveSubscribe`/`rejectSubscribe` instead of auto-accept — the getter returns a client-side cached value set by `manual(boolean)`, not a native read-back of the socket option); `manualLastValue()`/`manualLastValue(boolean)` (manual mode that also replays the last cached message per topic to a newly accepted subscriber); `welcomeMessage()`/`welcomeMessage(Message)` (sent automatically to each newly connected subscriber); `topicsCount()` — read-only; `approveSubscribe(RoutingId)`/`rejectSubscribe(RoutingId)` — **set-only, no getters** |

**Completion result.** `receiveSubscriptionEvent` returns `boolean` — same convention as `recv`
above.

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`receiveSubscriptionEvent`, or manual admission via `PubSocketOptions.manual()`/
`approveSubscribe`/`rejectSubscribe`; otherwise the two behave the same for publishing.

---

## `SubSocket` / `XSubSocket`

SUB subscribes to topics with subscriptions set as socket options; XSUB carries its subscriptions
as messages instead.

```java
try (SubSocket sub = context.createSubSocket()) {
    sub.setSubscription("prices.");
    TopicMessage msg = new TopicMessage();
    if (sub.subscribe(msg, RecvFlags.NONE)) { /* ... */ }
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `bind`/`connect`/`unbind`/`disconnect`/`disconnectRid` | same shape as `PairSocket` |
| `setSubscription(String filter)` / `unsetSubscription(String filter)` | adds/removes a topic filter; subscriptions accumulate |
| `subscriptionAt(int index)` | `Optional<SubscriptionEntry>` — the filter at that index, empty when out of range |
| `subscribe(TopicMessage result, RecvFlags flags)` | populates `result` with the next matching publish |
| `options()` | returns `SubSocketOptions`: `topicsCount()` only, read-only — the only per-type option either socket has |

**`XSubSocket` has the identical member set** — every method independently redeclared with the
same signatures; the only difference between the two types is what SUB/XSUB themselves mean at
the wire level, not anything visible in this contract.

**Completion result.** `subscribe` returns `boolean` — same convention as `recv` above.

**When to use.** Use `SubSocket` for the common case; use `XSubSocket` specifically when
subscriptions must be carried as ordinary messages instead.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type.

```java
try (StreamSocket stream = context.createStreamSocket()) {
    stream.onPacket((routingId, header, body) -> { /* owns header/body */ });
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `bind(String)` / `unbind(String)` | starts/stops listening on an address — **no `connect`/`disconnect`/`disconnectRid` on this interface**, unlike every other socket type above |
| `setRoutingId(RoutingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `send(RoutingId)` | starts the shared `SendOperation`, addressed to that peer |
| `recv(Received result, RecvFlags flags)` | populates `result` with the next packet |
| `onPacket(StreamPacketHandler handler)` | registers a callback-driven packet loop; the handler receives `(RoutingId routingId, Message header, Message body)` and owns both |
| `options()` | returns `StreamSocketOptions`: `notifyEnabled()`/`notify(boolean)` — **the getter and setter have different names** (`notifyEnabled()`, not `notify()`); delivers peer connect/disconnect as application messages when enabled |

**Completion result.** `recv` follows the `boolean` convention above.

**When to use.** Use `onPacket` for a callback-driven packet loop.

---

## Handler functional interfaces

Every callback registration point in this category takes a `@FunctionalInterface`.

| Interface | Registered by | Signature |
|---|---|---|
| `SendReadyHandler` | `Socket.setSendReadyHandler(...)` | `void onReady()` |
| `StreamPacketHandler` | `StreamSocket.onPacket(...)` | `void onPacket(RoutingId routingId, Message header, Message body)` |
| `CompletionControlHandler` | `RouterSocket.setCompletionControlHandler(...)` | `void onControl(RoutingId sourceRoutingId, List<Message> parts)` — owns every message in `parts` |
| `RequestCallback` | `RequestSubmitOperation.submit(callback)`/`RequestCallbackSubmitOperation.submit(callback)` (Messaging category) | `void onComplete(RequestResult result, List<Message> parts)` — owns `parts` only when `result == RequestResult.OK` |

---

## Socket enums

Shared enums referenced across every entry above.

| Enum | Used by | Values |
|---|---|---|
| `SocketType` | Internal socket-kind identification | `ANY`, `PAIR`, `PUB`, `SUB`, `DEALER`, `ROUTER`, `XPUB`, `XSUB`, `STREAM` |
| `AutoHwmProfile` | `ContextOptions.autoHwmProfile` (Core category) | `COMPACT`, `LOW_LATENCY`, `BALANCED`, `THROUGHPUT` |
| `AutoHwmRecalcReason` | Monitor status (Eventing category); its `value()`/`fromValue()` helpers are package-private | `NONE`, `INITIAL`, `ROLE_CHANGE`, `POLICY_TOGGLE`, `REFRESH`, `DEFERRED_SHRINK` |
| `RidDuplicatePolicy` | `CommonSocketOptions.ridDuplicatePolicy`, `RouterSocketOptions.handover` | `REJECT`, `HANDOVER` |
| `SubmitRetryMode` | `CommonSocketOptions.submitRetryMode` | `OFF`, `LOCAL_FAILURE` |
| `SendFlags` | Every send/request/reply builder's `.flags(...)` stage (Messaging category) | `NONE`, `DONT_WAIT` |
| `RecvFlags` | Every `recv`/`subscribe`/`receiveSubscriptionEvent` | `NONE`, `DONT_WAIT` |
| `SendResult` | The outcome of a non-blocking send attempt | `SENT`, `BACKPRESSURED`, `NOT_READY` |
| `SubmitResult` | Mirrored by `ZlinkSubmitException` (Errors category) | `OK`, `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`, `TERMINATED`, `INVALID_HANDLE`, `INVALID_ARGUMENT`, `NOT_SUPPORTED`, `INVALID_STATE`, `THREAD_VIOLATION`, `OUT_OF_MEMORY`, `SEQ_EXHAUSTED`, `INTERNAL_ERROR`, `NOT_ADMITTED` |
| `RecvResult` | Mirrored by `ZlinkRecvException` (Errors category) | `OK`, `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206) |
| `RequestResult` | Mirrored by `ZlinkRequestException` (Errors category), delivered by `RequestCallback` | `OK`, `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |

**When to use.** `DONT_WAIT` on either flags enum turns a blocking call into a non-blocking one
that reports `false`/back-pressure instead of blocking.

---

See [`contracts/sockets/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/sockets/)
and the [Java binding spec](../../spec/java/README.en.md) for the full rationale.

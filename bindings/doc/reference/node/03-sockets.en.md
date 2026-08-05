[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `Socket`/`ConnectableSocket` (the shared base interfaces), `CommonSocketOptions`
and its per-type extensions, the eight concrete socket interfaces, and the shared constants. Every
socket's `send`/`publish`/`request`/`reply` returns the operation-builder family documented in the
Messaging category — this category only covers where each builder starts and what each socket type
uniquely adds. Sockets are created via the top-level `createXxxSocket(ctx)` functions (Core
category), not a method on `Context`. The exact signatures are owned by
[`contracts/sockets/`](../../../../bindings/node/src/zlink/contracts/sockets/).

---

## `Socket` / `ConnectableSocket` shared base

The base interfaces every socket type extends: binding, TLS, monitoring, disposal, and (every
socket type except `StreamSocket`) outbound connection.

```ts
socket.bind('tcp://*:5555');
socket.setTlsServer(certPath, keyPath, true);
const monitor = socket.monitorOpen(SOCKET_MONITOR_EVENT_ALL);
socket.close();
```

**Options.** Every concrete socket type below extends `ConnectableSocket` **except `StreamSocket`**,
which extends `Socket` directly and adds its own `disconnectRid` independently (see below). A
`BaseSocket` union type (`PairSocket | PubSocket | SubSocket | DealerSocket | RouterSocket |
XPubSocket | XSubSocket | StreamSocket`) is exported for APIs — such as `proxy`/`proxySteerable`,
Core category — that accept any concrete socket type.

| Member | Meaning |
| --- | --- |
| `bind(endpoint: string)` / `unbind(endpoint: string)` | starts/stops listening on an address |
| `close()` | closes the native socket |
| `monitorOpen(events?: number, handler?: SocketMonitorHandler)` | opens a monitor; unlike every other language covered so far, **the handler can be registered directly at open time** as this method's second parameter, instead of always requiring a separate `onEvent`-style call afterward |
| `setTlsServer(cert, key, requireClientCert?)` | apply before `bind` |
| `setTlsClient(ca, hostname, trustSystem?)` | apply before `connect` |
| `ConnectableSocket.connect(endpoint: string)` / `disconnect(endpoint: string)` | connects/disconnects to a peer address |
| `ConnectableSocket.disconnectRid(routingId: RoutingId)` | disconnects the peer identified by that routing id |

**Completion result.** All members are synchronous with no return value except `monitorOpen`, which
returns `MonitorSocket` (Eventing category) synchronously — the caller owns and must close it.

**When to use.** Call `setTlsServer`/`setTlsClient` before `bind`/`connect` respectively. Pass a
handler directly to `monitorOpen(events, handler)` when there is no need to hold the monitor for
`recv`-style draining separately.

---

## `CommonSocketOptions` and per-type extensions

The typed options facade shared by every socket type, reached via `socket.options`. A plain
interface of mutable properties (get/set through normal property access) rather than
getter/setter method pairs.

```ts
socket.options.sendHwm = 100_000n;
socket.options.linger = 1000;
socket.options.submitRetryMode = SubmitRetryMode.LocalFailure;
```

**Options — `CommonSocketOptions`.**

| Member | Type | Meaning |
| --- | --- | --- |
| `linger` | `number`, ms | upper bound on how long `close()` waits for pending sends to flush |
| `sendHwm` / `recvHwm` | `bigint`, accounted-byte HWM | outbound/inbound HWM; `0n` means unlimited |
| `sendTimeout` / `recvTimeout` / `connectTimeout` | `number`, ms | upper bound on how long a blocking send/receive/connect handshake waits |
| `immediate` | `boolean` | whether a send requires a live connection now, instead of queueing until one exists |
| `ridDuplicatePolicy` | `RidDuplicatePolicyValue` | what happens when a peer reuses an existing routing id |
| `ipv6` | `boolean` | whether the socket accepts IPv6 connections |
| `tcpNoDelay` | `boolean` | disables Nagle's algorithm when `true` |
| `tcpKeepalive` | `number`, -1/0/1 | OS TCP keepalive mode |
| `maxMsgSize` | `bigint` | maximum size in bytes of a single accepted message; `-1n` means no limit |
| `lastEndpoint` | `string`, read-only | the concrete resolved bind address |
| `backlog` | `number` | pending-connection queue length for a listening socket |
| `reconnectInterval` / `reconnectIntervalMax` | `number`, ms | delay between reconnect attempts / cap on that delay |
| `submitRetryMode` | **typed as plain `number`, not `SubmitRetryModeValue`** — an inconsistency against `ridDuplicatePolicy`'s typed property | whether a failed submit retries automatically on local back-pressure |
| `submitRetryTimeout` | `number`, ms | retry timeout when `submitRetryMode` requests it |
| `submitRetryAttempts` | `number` | retry attempt cap when `submitRetryMode` requests it |

**Options — per-type extensions.**

| Type | Member | Meaning |
| --- | --- | --- |
| `DealerSocketOptions` | `probe` (`boolean`) | sends an empty probe on connect |
| | `requestTimeout` (`number`, ms) | request timeout |
| | `peerWeight` (`number`, 0-100) | load-balancing weight |
| `RouterSocketOptions` | `mandatory` (`boolean`) | error instead of silent drop on an unknown route |
| | `handover` (`boolean`) | convenience wrapper over `ridDuplicatePolicy` |
| | `probe` (`boolean`) | sends an empty probe on connect |
| | `connectRoutingId` (`RoutingId \| null`, read-only) / `setConnectRoutingId(routingId)` | asymmetric getter/setter naming |
| | `requestTimeout` / `peerWeight` | same shape as Dealer's, both directions |
| `StreamSocketOptions` | `notify` (`boolean`) | delivers peer connect/disconnect as application messages when enabled |
| `PubSocketOptions` | `verbose` / `verboser` (`boolean`) | deliver every (un)subscribe message, including duplicates |
| | `noDrop` (`boolean`) | error instead of silent drop on back-pressure |
| | `manual` / `manualLastValue` (`boolean`) | subscriptions require `approveSubscribe`/`rejectSubscribe`; `manualLastValue` also replays the last cached message per topic to a newly accepted subscriber |
| | `topicsCount` (`number`, read-only) | active subscription count |
| | `welcomeMessage()` / `setWelcomeMessage(message: MessageLike)` | sent automatically to each newly connected subscriber; the getter returns a `Message` the caller owns |
| | `approveSubscribe(routingId)` / `rejectSubscribe(routingId)` | require `manual`; set-only, no getters |
| `SubSocketOptions` | `topicsCount` (read-only) | the only per-type option this socket has |

**Completion result.** Every property read/write is synchronous.

**When to use.** Set `sendHwm`/`recvHwm` and `linger` before the socket starts exchanging messages
when the defaults don't fit the deployment. Read `lastEndpoint` after binding to a wildcard address.

---

## `PairSocket`

An exclusive one-to-one peering socket with no routing. Serves as the base shape `DealerSocket`
extends (see below) — unlike dotnet/java/cpp, which give Pair and Dealer a separate shared
`IMessageSocket`-style base, here `DealerSocket extends PairSocket` directly.

```ts
const pair = createPairSocket(ctx);
pair.send().message(Message.from('ping')).submit();
const received = new Received();
if (pair.recv(received)) { /* ... */ }
```

**Options.**

| Member | Meaning |
| --- | --- |
| `options` | `CommonSocketOptions` |
| `send()` | starts the shared `SendOperation` builder |
| `recv(result: Received, flags?: RecvFlags)` | populates `result` with the next message |
| `setSendReadyHandler(handler: () => void)` | registers a back-pressure-cleared callback |

**Completion result.** `recv` returns `boolean` — `false` only when `RecvFlags.DontWait` is set and
no message is available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests. Extends `PairSocket`
directly (not a separate shared message-socket interface).

```ts
const dealer = createDealerSocket(ctx);
dealer.setRoutingId(RoutingId.from('worker-3'));
const reply = await dealer.request().message(Message.from('payload')).submit();
```

**Options.** Everything from `PairSocket`, plus:

| Member | Meaning |
| --- | --- |
| `options` | overridden to `DealerSocketOptions` |
| `setRoutingId(routingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `request()` | starts the shared `RequestOperation` builder; no target parameter — DEALER has no API-level peer routing id |

**Completion result.** `recv` (inherited) follows the same `boolean` convention as `PairSocket`.

**When to use.** Set `setRoutingId` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request context (`Received.reply()`) or an explicit ROUTER reply surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id, and can reply to a specific peer's request.
Extends `ConnectableSocket` directly (not `PairSocket`).

```ts
const router = createRouterSocket(ctx);
router.send(peerRid).message(Message.from('hello')).submit();
router.setCompletionControlHandler((rid, parts) => { /* ... */ });
```

**Options.**

| Member | Meaning |
| --- | --- |
| `options` | `RouterSocketOptions` |
| `send(routingId)` | starts the shared `SendOperation`, addressed to that peer |
| `recv(result: Received, flags?: RecvFlags)` | populates `result` with the next message |
| `setSendReadyHandler(handler)` | registers a back-pressure-cleared callback |
| `setRoutingId(routingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `request(peerRid)` | Messaging category's `RequestOperation`, addressed to a specific peer |
| `reply(peerRid, requestSeq: bigint)` | Messaging category's `ReplyOperation`, answering that peer's request |
| `trySendCompletionControl(peerRid, parts: readonly MessageLike[])` | sends an opaque control record to a peer over its existing connection; does not consume `parts` |
| `setCompletionControlHandler(handler: (sourceRoutingId, parts: Message[]) => void)` | registers the callback that receives incoming completion-control records; the handler owns the received messages |

**Completion result.** `trySendCompletionControl` returns `boolean` — `false` only when the
completion connection is back-pressured. `recv` follows the `boolean` convention above.

**When to use.** Use `request(peerRid)`/`reply(peerRid, requestSeq)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer. Use
`trySendCompletionControl`/`setCompletionControlHandler` for an opaque bounded control record
independent from application-level receive.

---

## `PubSocket` / `XPubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; XPUB additionally
surfaces subscriber subscription/unsubscription events. `XPubSocket extends PubSocket`.

```ts
const pub = createPubSocket(ctx);
pub.publish('prices').message(Message.from(tick)).submit();

const xpub = createXPubSocket(ctx);
const evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** `PubSocket extends ConnectableSocket`; `XPubSocket extends PubSocket`.

| Member | Meaning |
| --- | --- |
| `options` | `PubSocketOptions` |
| `publish(topic: string)` | starts the shared `SendOperation` builder |
| `setSendReadyHandler(handler)` | registers a back-pressure-cleared callback |
| `receiveSubscriptionEvent(result: SubscriptionEvent, flags?: RecvFlags)` | `XPubSocket` only; populates `result` with the next subscribe/unsubscribe |

**No `setRoutingId`/`getRoutingId` on `PubSocket`** (unlike dotnet's `IPubSocket`, which has both).

**Completion result.** `receiveSubscriptionEvent` returns `boolean` (same convention as `recv`
above).

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`receiveSubscriptionEvent`, or manual admission via `PubSocketOptions.manual`/`approveSubscribe`/
`rejectSubscribe`; otherwise the two behave the same for publishing.

---

## `SubSocket` / `XSubSocket`

SUB subscribes to topics with subscriptions set as socket options; XSUB carries its subscriptions
as messages instead. `XSubSocket extends SubSocket {}` — a completely empty interface body, the
plainest possible delta-only declaration among every wrapper binding covered so far.

```ts
const sub = createSubSocket(ctx);
sub.setSubscription('prices.');
const msg = new TopicMessage();
if (sub.subscribe(msg)) { /* ... */ }
```

**Options.** `SubSocket extends ConnectableSocket`.

| Member | Meaning |
| --- | --- |
| `options` | `SubSocketOptions` |
| `setSubscription(filter: string)` / `unsetSubscription(filter: string)` | adds/removes a topic filter; subscriptions accumulate |
| `subscriptionAt(index: number)` | `SubscriptionEntry \| null` — the filter at that index |
| `subscribe(result: TopicMessage, flags?: RecvFlags)` | populates `result` with the next matching publish |

`XSubSocket` adds nothing at all — every member is the inherited `SubSocket` surface, unchanged.

**Completion result.** `subscribe` returns `boolean` (same convention as `recv` above).

**When to use.** Use `SubSocket` for the common case; use `XSubSocket` specifically when
subscriptions must be carried as ordinary messages instead — the choice is entirely about which
concrete type you construct (`createSubSocket` vs. `createXSubSocket`), since the interface itself
adds nothing to distinguish them.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type. Extends `Socket` (not `ConnectableSocket`) and declares its own
`disconnectRid` independently.

```ts
const stream = createStreamSocket(ctx);
stream.setPacketHandler((sourceRid, header, body) => { /* owns header/body */ });
```

**Options.**

| Member | Meaning |
| --- | --- |
| `options` | `StreamSocketOptions` |
| `send(routingId)` | starts the shared `SendOperation`, addressed to that peer |
| `recv(result: Received, flags?: RecvFlags)` | populates `result` with the next packet |
| `setPacketHandler(handler: StreamPacketHandler)` | registers a callback-driven packet loop |
| `setSendReadyHandler(handler)` | registers a back-pressure-cleared callback |
| `setRoutingId(routingId)` / `getRoutingId()` | assigns/reads this socket's own routing id, observed by peers on connect |
| `disconnectRid(routingId)` | declared directly on this interface, since `StreamSocket` does not inherit `ConnectableSocket`'s copy |

**Completion result.** `recv` follows the `boolean` convention above. The packet handler owns both
`header` and `body` messages it receives.

**When to use.** Use `setPacketHandler` for a callback-driven packet loop.

---

## Socket constants

Shared constant objects and their derived types, referenced across every entry above.

| Constant | Used by | Values |
|---|---|---|
| `SocketType` | Internal socket-kind identification | **Dual-cased**: both `ANY`/`PAIR`/`PUB`/`SUB`/`DEALER`/`ROUTER`/`XPUB`/`XSUB`/`STREAM` and `Any`/`Pair`/`Pub`/`Sub`/`Dealer`/`Router`/`XPub`/`XSub`/`Stream` are exported as aliases for the identical numeric values |
| `SOCKET_MONITOR_EVENT_ALL` | `Socket.monitorOpen(events)` | `0xFFFF` — a convenience "subscribe to everything" constant; individual lifecycle event flags (`Connected`, `Disconnected`, etc.) are the `MonitorEventType` constant object, declared in the Eventing category's `contracts/eventing/monitor.ts` rather than here in `socket_constants.ts` |
| `RidDuplicatePolicy` | `CommonSocketOptions.ridDuplicatePolicy`, `RouterSocketOptions.handover` | `Reject`, `Handover` |
| `SubmitRetryMode` | `CommonSocketOptions.submitRetryMode` (property itself typed as plain `number`) | `Off`, `LocalFailure` |
| `SendFlags` | Every send/request/reply builder's `.flags(...)` stage (Messaging category) | `None`, `DontWait` |
| `RecvFlags` | Every `recv`/`subscribe`/`receiveSubscriptionEvent` | `None`, `DontWait` |
| `PollEventFlag` | Poller registration/wait (Eventing category) | `PollIn`, `PollOut`, `PollErr`, `PollPri`, `PollCompletion` |

**When to use.** `DontWait` on either flags constant turns a blocking call into a non-blocking one
that reports `false`/back-pressure instead of blocking. Pass `SOCKET_MONITOR_EVENT_ALL` to
`monitorOpen(events)` to subscribe to every lifecycle event, or OR together specific
`MonitorEventType` values (Eventing category) as a raw numeric mask to filter a subscription.

---

See [`contracts/sockets/`](../../../../bindings/node/src/zlink/contracts/sockets/) and the
[Node binding spec](../../spec/node/README.en.md) for the full rationale.

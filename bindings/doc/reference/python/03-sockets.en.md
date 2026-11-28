[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `_SocketContract` (the shared base `Protocol`), `CommonSocketOptions` and its
per-type extensions, the eight concrete socket `Protocol` types, and the send/request/reply
operation-builder family (declared here, not in Messaging — see the README). The exact signatures
are owned by
[`contracts/sockets/`](../../../../bindings/python/src/zlink/contracts/sockets/).

---

## `_SocketContract` shared base (private by convention)

The base `Protocol` every socket type extends: binding, disposal, options. Named with a leading
underscore — Python's convention for "not public API," even though `socket.py`'s module-level
`__getattr__` is how every concrete socket type is actually reached from this package.

```python
socket.bind("tcp://*:5555")
with socket:
    ...
```

| Member | Meaning |
| --- | --- |
| `bind(endpoint)` | starts listening on an address |
| `unbind(endpoint)` | stops listening on the bound local endpoint |
| `set_tls_server(cert, key, require_client_cert=False)` | sets the server certificate, key, and client certificate requirement before binding |
| `set_tls_client(ca_cert, hostname, trust_system=False)` | sets the trusted CA, expected server name, and system CA policy before connecting. `ca_cert` and `hostname` accept `None`. |
| `close()` | closes the native socket |
| `options` | property, the typed options facade for this socket type |
| `__enter__` / `__exit__` | sync context-manager only — **no `__aenter__`/`__aexit__` here**, unlike the async-and-sync-both pattern every other resource type in this binding follows |

| Operation | Completion result |
| --- | --- |
| `bind(endpoint)` | Returns `None` on success. Raises `BindError` on failure. |
| `unbind(endpoint)` | Returns `None` on success. Raises `ConnectError` on failure. |
| `set_tls_server(...)` | Returns `None` on success. Raises `ConfigError` on failure. |
| `set_tls_client(...)` | Returns `None` on success. Raises `ConfigError` on failure. |

**When to use.** Every concrete socket type below extends this Protocol and adds its own
`connect`/`disconnect`/send/recv surface.

---

## `CommonSocketOptions` and per-type extensions

The typed options facade shared by every socket type, reached via `socket.options`.

```python
socket.options.send_high_water_mark = 100_000
socket.options.linger_ms = 1000
socket.options.submit_retry_mode = SubmitRetryMode.LOCAL_FAILURE
```

**Options — `CommonSocketOptions`.** All plain get/set properties.

| Member | Meaning |
| --- | --- |
| `linger_ms` | upper bound on how long `close()` waits for pending sends to flush |
| `send_high_water_mark` / `receive_high_water_mark` | outbound/inbound accounted-byte HWM |
| `send_timeout_ms` / `receive_timeout_ms` | upper bound on how long a blocking send/receive waits |
| `immediate` | whether a send requires a live connection now, instead of queueing until one exists |
| `rid_duplicate_policy` | what happens when a peer reuses an existing routing id |
| `connect_timeout_ms` | upper bound on how long connect handshake waits |
| `ipv6` | whether the socket accepts IPv6 connections |
| `tcp_no_delay` | disables Nagle's algorithm when `True` |
| `tcp_keepalive` | OS TCP keepalive mode |
| `max_message_size` | maximum size in bytes of a single accepted message |
| `backlog` | pending-connection queue length for a listening socket |
| `reconnect_interval_ms` / `reconnect_interval_max_ms` | delay between reconnect attempts / cap on that delay |
| `submit_retry_mode` | whether a failed submit retries automatically on local back-pressure |
| `submit_retry_timeout_ms` / `submit_retry_attempts` | retry timeout/attempt cap when `submit_retry_mode` requests it |
| `heartbeat_interval_ms` | interval between heartbeat pings on an idle connection — **no other language covered so far exposes this property** |
| `heartbeat_ttl_ms` | how long the remote keeps the connection alive without a heartbeat — same exclusivity |
| `heartbeat_timeout_ms` | how long to wait for a heartbeat reply before treating the connection as dead — same exclusivity |

**Options — per-type extensions.**

| Type | Member | Meaning |
| --- | --- | --- |
| `DealerSocketOptions` | `probe` | sends an empty probe on connect |
| | `weight` | load-balancing weight |
| | `request_timeout_ms` | request timeout |
| `RouterSocketOptions` | `mandatory` | error instead of silent drop on an unknown route |
| | `handover` | convenience wrapper over `rid_duplicate_policy` |
| | `probe` | sends an empty probe on connect |
| | `connect_routing_id` | the *only* routing-id-shaped surface anywhere in this binding's socket contracts — see the README's note that no socket type has `set_routing_id`/`get_routing_id` of its own |
| | `weight` / `request_timeout_ms` | both directions |
| `StreamSocketOptions` | `notify` | delivers peer connect/disconnect as application messages when enabled |
| `PubSocketOptions` | `verbose` / `verboser` | deliver every (un)subscribe message, including duplicates |
| | `manual` / `manual_last_value` | subscriptions require `approve_subscribe`/`reject_subscribe`; `manual_last_value` also replays the last cached message per topic to a newly accepted subscriber |
| | `no_drop` | error instead of silent drop on back-pressure |
| | `welcome_message` | sent automatically to each newly connected subscriber |
| | `topics_count` | read-only, active subscription count |
| | `approve_subscribe(routing_id)` / `reject_subscribe(routing_id)` | set-only, no getters |
| `SubSocketOptions` | `topics_count` | read-only — the only per-type option this socket has |

**Completion result.** Every property read/write is synchronous.

**When to use.** Set `send_high_water_mark`/`receive_high_water_mark` and `linger_ms` before the
socket starts exchanging messages when the defaults don't fit the deployment. Use the three
`heartbeat_*` properties to tune ZMTP-level liveness detection independent of the transport's own
TCP keep-alive.

---

## `PairSocket`

An exclusive one-to-one peering socket with no routing.

```python
pair = create_pair_socket(ctx)
pair.send().message(b"ping").submit_sync()
received = create_received()
if pair.recv_into(received):
    ...
```

**Options.** Plus the `_SocketContract` base surface.

| Member | Meaning |
| --- | --- |
| `connect(endpoint)` / `disconnect(endpoint)` | connects/disconnects to a peer address |
| `disconnect_rid(peer_rid)` | disconnects the peer identified by that routing id |
| `send()` | starts the shared `SendOp` builder |
| `recv_into(received, *, flags=0)` | populates `received` with the next message, returns `bool` |

**Completion result.** `recv_into` returns `False` only when `DONT_WAIT` is set and no message is
available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests.

```python
# The dealer is connected. The caller drives the completion poller while the reply is pending.
completion_poller.add_socket(dealer, PollEventFlag.POLLCOMPLETION, slot=1)
submission = dealer.request().message(b"payload").timeout(5.0).submit()
reply = await submission.reply
```

**Options.** **No `set_routing_id`/`get_routing_id` at all** — see the README's note.

| Member | Meaning |
| --- | --- |
| `dealer_options` | property, returns `DealerSocketOptions` |
| `connect(endpoint)` / `disconnect(endpoint)` | connects/disconnects to a peer address |
| `send()` | starts the shared `SendOp` builder |
| `request()` | starts the shared `RequestOp` builder; no target parameter — DEALER has no API-level peer routing id |
| `recv_into(received, *, flags=0)` | populates `received` with the next message |

**Completion result.** `recv_into` follows the same `False`-on-`DONT_WAIT` convention as
`PairSocket`.

**When to use.** DEALER has no protocol envelope helper to reply to an arbitrary token — reply from
a received request context (`Received.reply()`, Messaging category) or an explicit ROUTER reply
surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id, and can reply to a specific peer's request.

```python
# The caller drives the completion poller while admission is pending.
completion_poller.add_socket(router, PollEventFlag.POLLCOMPLETION, slot=1)
submission = router.send(peer_rid).message(b"hello").submit()
await submission.admitted
```

**Options.** ROUTER uses the same managed send/request terminals as DEALER. Core keeps the selected
application pipe and its paired completion pipe together; the binding exposes terminal results,
not a separate completion-control channel.

| Member | Meaning |
| --- | --- |
| `router_options` | property, returns `RouterSocketOptions` |
| `connect(endpoint)` / `disconnect(endpoint)` | connects/disconnects to a peer address |
| `send(routing_id)` | starts the shared `SendOp`, addressed to that peer |
| `request(routing_id)` | Messaging category's shared `RequestOp`, addressed to a specific peer |
| `reply(routing_id, token)` | the shared `ReplyOp`, consuming the opaque `ReplyToken` received from this ROUTER |
| `recv_into(received, *, flags=0)` | populates `received` with the next message |

**Completion result.** `recv_into` follows the convention above.

**When to use.** Use `request(routing_id)`/`reply(routing_id, token)` for ROUTER-initiated or
ROUTER-answered request/reply. Keep the token opaque, socket-owned, and one-shot.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; SUB subscribes
with subscriptions set as socket options; XPUB/XSUB add subscriber-event surfacing and
message-carried subscriptions respectively.

```python
pub = create_pub_socket(ctx)
pub.publish("prices").message(tick).submit()

sub = create_sub_socket(ctx)
sub.set_subscription("prices.")
msg = create_topic_message()
if sub.subscribe_into(msg):
    ...
```

**Options.**

| Type | Member | Meaning |
| --- | --- | --- |
| `PubSocket` | `pub_options` | the per-type options facade |
| | `connect(endpoint)` / `disconnect(endpoint)` | connects/disconnects to a peer address |
| | `publish(topic)` | starts the shared `SendOp` builder |
| `SubSocket` | `sub_options` | the per-type options facade |
| | `connect(endpoint)` / `disconnect(endpoint)` | connects/disconnects to a peer address |
| | `set_subscription(topic)` / `unset_subscription(topic)` | adds/removes a topic filter; subscriptions accumulate |
| | `subscription_at(index)` | `(filter, is_pattern)` at that index, or `None` when Core returns `ENOENT` |
| | `subscribe_into(topic_message, *, flags=0)` | populates `topic_message` with the next matching publish |
| `XPubSocket` | `pub_options` / `connect` / `disconnect` / `publish` | same shape as `PubSocket` |
| | `receive_subscription_event_into(event, *, flags=0)` | populates `event` with the next subscribe/unsubscribe |
| `XSubSocket` | `sub_options` / `connect` / `disconnect` / `set_subscription` / `unset_subscription` / `subscription_at` / `subscribe_into` | **entirely independent `Protocol` declaration with the identical member set to `SubSocket`** — no shared base type links the two beyond the matching shape |

The binding supplies its own buffer on the first call to `zlink_subscription_at`. A successful
call completes the query, including when the filter is empty. If Core returns `ENOBUFS`, the
binding calls Core once more with the required length Core returned. Other Core failures
raise `ConfigError`.

**Completion result.** `subscribe_into`/`receive_subscription_event_into` follow the
`False`-on-`DONT_WAIT` convention above.

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`receive_subscription_event_into`, or manual admission via `PubSocketOptions.manual`/
`approve_subscribe`/`reject_subscribe`. Use `XSubSocket` specifically when subscriptions must be
carried as ordinary messages instead — the choice is entirely about which factory you call
(`create_sub_socket` vs. `create_xsub_socket`), since the two Protocols' member sets are identical.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type.

```python
stream = create_stream_socket(ctx)
stream.stream_options.recv_mode = StreamRecvMode.PACKET
packet = StreamPacket()
if stream.recv_packet_into(packet):
    packet.close()
```

**Options.** **No `connect`/`disconnect` declared on this Protocol** — matching every other
language's STREAM asymmetry.

| Member | Meaning |
| --- | --- |
| `stream_options` | the per-type options facade |
| `send(routing_id)` | starts the shared `SendOp`, addressed to that peer |
| `recv_into(received, *, flags=0)` | pulls the next RAW-mode record |
| `recv_packet_into(packet, *, flags=0)` | pulls the next PACKET-mode header/body pair into reusable caller-owned storage. A concurrent receive into the same `StreamPacket` raises `RecvError` with `RecvResult.INVALID_STATE` and `internal_errno == errno.EBUSY`. |
| `disconnect_rid(peer_rid)` | disconnects the peer identified by that routing id |

**Completion result.** `recv_into` and `recv_packet_into` follow the convention above. The caller
owns the packet and closes its header/body with `packet.close()`.

**When to use.** Set `stream_options.recv_mode` to `StreamRecvMode.RAW` or `.PACKET` before the
first successful bind/connect; changing it afterward raises invalid state. Drain the selected
pull API.

---

## Send / request / reply operation-builder shape

The fluent builder every socket type's `send`/`publish`/`request`/`reply` entry point above returns
to accumulate parts and reach a terminal submit. All builder stages extend the shared
`_FluentMessageOp` base Protocol.

```python
# Register the dealer once and drive the poller while either stage is pending.
completion_poller.add_socket(dealer, PollEventFlag.POLLCOMPLETION, slot=1)
submission = dealer.send().message(part1).message(part2).submit()
await submission.admitted

submission = dealer.request().message(payload).timeout(5.0).submit()
reply = await submission.reply

received.reply().message(b"ok").submit()
```

**Options.**

| Stage | Member | Meaning |
| --- | --- | --- |
| `_FluentMessageOp` (shared base) | `message(payload)` | add one part, starts/continues the chain |
| | `messages(*payloads)` | add several parts in one call — **declared directly on the shared base Protocol here**, unlike other languages where the multi-part convenience is a separate extension method |
| `RequestOp` | `timeout(timeout)` | sets the reply timeout in seconds, measured from admission |
| `PublishOp extends _FluentMessageOp` | `flags(flags)` / `submit()` | synchronous lossy/NODROP publication; the only operation family retaining send flags |
| `ReplyOp extends _FluentMessageOp` | `submit()` | terminal |

| Terminal | Completion result |
| --- | --- |
| `SendOp.submit()` | Returns a `SendSubmission` immediately. Read the initial result in `submission.result` and await `submission.admitted` for admission. |
| `SendOp.submit_sync()` | Blocks until admission or failure. |
| `RequestOp.submit()` | Returns a `RequestSubmission` immediately. It exposes the same result and admission properties as a send submission. Await `submission.reply` for caller-owned reply parts. |
| `RequestOp.submit_sync()` | Blocks until it returns caller-owned reply parts or raises an error. |
| `PublishOp.submit()` | Completes publication synchronously. |
| `ReplyOp.submit()` | Submits the reply synchronously. |

A submitted `Message` is consumed when Core admits it. A synchronous rejection leaves it with the caller. When an asynchronous send or request returns `BACKPRESSURED` with a wait token, the binding consumes the original once it has secured its own copy for resubmission. A consumed `Message` reports empty data and size zero. To keep using the payload, make a copy with `Message.copy()` before submitting.

Register the socket for `PollEventFlag.POLLCOMPLETION` before submitting an asynchronous request
or a send that may wait for admission. Call [`Poller.wait()`](04-eventing.en.md#poller) while
admission or a reply is pending. The [async request sample](../../../../bindings/python/samples/request_reply_async_sample.py)
shows registration, poller execution alongside the responder, and reply cleanup.

**When to use.** Use `messages(*payloads)` to add several parts in one call instead of chaining
`.message(...)` per part. Use `submit_sync()` when the calling thread can block.

---

## Socket enums

| Enum | Used by | Values |
|---|---|---|
| `SocketType` | Internal socket-kind identification | `ANY`, `PAIR`, `PUB`, `SUB`, `DEALER`, `ROUTER`, `XPUB`, `XSUB`, `STREAM` |
| `SendFlags` | `PublishOp.flags(...)` only | `NONE`, `DONT_WAIT` |
| `RecvFlags` | Every `recv_into`/`subscribe_into`/`receive_subscription_event_into` | `NONE`, `DONT_WAIT` |
| `SubmitResult` | Mirrored by `SubmitError` (Errors category) | `OK`, `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`, `TERMINATED`, `INVALID_HANDLE`, `INVALID_ARGUMENT`, `NOT_SUPPORTED`, `INVALID_STATE`, `THREAD_VIOLATION`, `OUT_OF_MEMORY`, `SEQ_EXHAUSTED`, `INTERNAL_ERROR`, `NOT_ADMITTED` |
| `RequestResult` | Mirrored by `RequestError` (Errors category) | `OK`, `TIMED_OUT`(101), `NOT_FOUND`(102), `TERMINATED`(103), `PROTOCOL_ERROR`(104), `INTERNAL_ERROR`(105), `REJECTED`(106), `CONFLICT`(107), `BUSY`(108), `NOT_CONNECTED`(109), `INVALID_ARGUMENT`(110), `INVALID_STATE`(111), `NOT_SUPPORTED`(112), `BACKPRESSURED`(113) |
| `RecvResult` | Mirrored by `RecvError` (Errors category) | `OK`, `NO_DATA`(201), `BUSY`(202), `TERMINATED`(203), `INVALID_HANDLE`(204), `NOT_SUPPORTED`(205), `INTERNAL_ERROR`(206), `BUFFER_TOO_SMALL`(207), `INVALID_STATE`(208) — the fuller 8-value set (matching node's, not dotnet/cpp/java/rust's 6-value set) |
| `HandlerResult` | retained result family; current public completion/event paths use terminals or pull | `OK`, `INVALID_ARGUMENT`(301), `BUSY`(302), `NOT_SUPPORTED`(303), `DEADLOCK`(304), `INVALID_HANDLE`(305), `INTERNAL_ERROR`(306) |
| `RidDuplicatePolicy` | `CommonSocketOptions.rid_duplicate_policy`, `RouterSocketOptions.handover` | `REJECT`, `HANDOVER` |
| `SubmitRetryMode` | `CommonSocketOptions.submit_retry_mode` | `OFF`, `LOCAL_FAILURE` |

**When to use.** `DONT_WAIT` on publish or receive turns a blocking call into a non-blocking one that
reports `False`/back-pressure instead of blocking.

---

See [`contracts/sockets/`](../../../../bindings/python/src/zlink/contracts/sockets/) and the
[Python binding spec](../../spec/python/README.en.md) for the full rationale.

[한국어](03-sockets.ko.md) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `CommonSocketOptions` and its per-type extensions, the eight concrete socket
structs, and shared flag/enum types. **There is no shared cross-socket-type base trait** — each
socket type is a standalone struct with its own inherent `impl` block declaring
`bind`/`connect`/`unbind`/`disconnect`/TLS methods independently; the four PUB/SUB/XPUB/XSUB types
share their common surface through an internal macro (`impl_pubsub_common!`) rather than a public
trait. Every socket's `send`/`publish`/`request`/`reply` returns the operation-builder family
documented in the Messaging category. The exact signatures are owned by
[`contracts/sockets/`](../../../../bindings/rust/src/contracts/sockets/).

---

## Shared socket surface (no base trait)

**Options.** Every concrete socket type below independently declares this same method set. Every
member returns a `Result`, unlike languages where the equivalent lifecycle call has no error path.

| Member | Meaning |
| --- | --- |
| `close(&mut self) -> Result<(), CloseError>` | closes the native socket |
| `bind(&self, addr: &str) -> Result<(), BindError>` | starts listening on an address |
| `unbind(&self, addr: &str) -> Result<(), ConnectError>` | stops listening on an address |
| `last_endpoint(&self) -> Result<String, ConfigError>` | the concrete resolved bind address |
| `set_tls_cert(&self, cert: &str)` / `set_tls_key(&self, key: &str)` / `set_tls_ca(&self, ca_cert: &str)` / `set_tls_hostname(&self, hostname: &str)` / `set_tls_trust_system(&self, bool)` | individual TLS setters — **exist here as public methods**, unlike every other language covered so far, which only exposes the combined form |
| `set_tls_server(&self, cert, key, require_client_cert: bool)` | combined server-side TLS setup, apply before `bind` |
| `set_tls_client(&self, ca_cert, hostname, trust_system: bool)` | combined client-side TLS setup, apply before `connect` |
| `connect(&self, addr: &str)` / `disconnect(&self, addr: &str)` | connects/disconnects to a peer address; declared on every socket type except `StreamSocket` (see below) |
| `disconnect_rid(&self, peer_rid: &RoutingId)` | disconnects the peer identified by that routing id; same exception as above |

**Completion result.** Every one of these methods returns a `Result`, unlike languages where the
equivalent lifecycle call has no error path.

**When to use.** Call the individual `set_tls_cert`/`set_tls_key`/etc. setters when TLS
configuration needs to be assembled incrementally (for example, values arriving from separate
configuration sources); use the combined `set_tls_server`/`set_tls_client` for the common one-call
case.

---

## `CommonSocketOptions`

The typed options facade shared by every socket type, reached via `socket.common_options()`.

```rust
let options = socket.common_options();
options.set_send_high_water_mark(100_000)?;
options.set_linger(Duration::from_secs(1))?;
options.set_submit_retry_mode(SubmitRetryMode::LocalFailure)?;
```

**Options.** Every getter/setter below returns `Result<T, ConfigError>`.

| Member | Meaning |
| --- | --- |
| `linger()` / `set_linger(Duration)` | upper bound on how long `close()` waits for pending sends to flush |
| `send_high_water_mark()` / `set_send_high_water_mark(u64)` | outbound accounted-byte HWM; `0` means unlimited |
| `receive_high_water_mark()` / `set_receive_high_water_mark(u64)` | inbound accounted-byte HWM; `0` means unlimited |
| `send_timeout()` / `set_send_timeout(Duration)` | upper bound on how long a blocking send waits |
| `receive_timeout()` / `set_receive_timeout(Duration)` | upper bound on how long a blocking receive waits |
| `immediate()` / `set_immediate(bool)` | whether a send requires a live connection now, instead of queueing until one exists |
| `rid_duplicate_policy()` / `set_rid_duplicate_policy(RidDuplicatePolicy)` | what happens when a peer reuses an existing routing id |
| `connect_timeout()` / `set_connect_timeout(Duration)` | upper bound on how long connect handshake waits |
| `ipv6()` / `set_ipv6(bool)` | whether the socket accepts IPv6 connections |
| `tcp_no_delay()` / `set_tcp_no_delay(bool)` | disables Nagle's algorithm when `true` |
| `tcp_keepalive()` / `set_tcp_keepalive(bool)` | OS TCP keepalive mode — **a plain on/off `bool` here, unlike other languages' tri-state -1/0/1 integer** |
| `max_message_size()` / `set_max_message_size(i64)` | maximum size in bytes of a single accepted message |
| `backlog()` / `set_backlog(i32)` | pending-connection queue length for a listening socket |
| `reconnect_interval()` / `set_reconnect_interval(Duration)` | delay between reconnect attempts |
| `reconnect_interval_max()` / `set_reconnect_interval_max(Duration)` | cap on the reconnect delay |
| `submit_retry_mode()` / `set_submit_retry_mode(SubmitRetryMode)` | whether a failed submit retries automatically on local back-pressure |
| `submit_retry_timeout()` / `set_submit_retry_timeout(Duration)` | retry timeout when `submit_retry_mode()` is `LocalFailure` |
| `submit_retry_attempts()` / `set_submit_retry_attempts(i32)` | retry attempt cap when `submit_retry_mode()` is `LocalFailure` |

**Completion result.** Every getter/setter is synchronous, returning `Result<_, ConfigError>`.

**When to use.** Set `send_high_water_mark`/`receive_high_water_mark` and `linger` before the
socket starts exchanging messages when the defaults don't fit the deployment.

---

## `PairSocket`

An exclusive one-to-one peering socket with no routing.

```rust
let pair = ctx.pair_socket()?;
pair.send().message(Message::try_from("ping")?)?.submit()?;
let mut received = Received::empty();
if pair.recv(&mut received, RecvFlags::NONE)? { /* ... */ }
```

**Options.** Plus the shared lifecycle/TLS surface above.

| Member | Meaning |
| --- | --- |
| `send(&self) -> SendOp<Empty>` | starts the shared `SendOp` builder |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | populates `out` with the next message |
| `on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn() + Send + 'static` | registers a back-pressure-cleared callback |
| `common_options() -> CommonSocketOptions<'_>` | the shared options facade |

**Completion result.** `recv` returns `Ok(false)` only when `RecvFlags::DONT_WAIT` is set and no
message is available.

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `DealerSocket`

Load-balances sends across its connected peers and can issue routed requests.

```rust
let dealer = ctx.dealer_socket()?;
dealer.set_routing_id(&"worker-3".into())?;
dealer.request()
    .message(Message::try_from("payload")?)
    .submit(|result| { /* result: Result<Vec<Message>, RequestError> */ })?;
```

**Options.** Same shared surface as `PairSocket`, plus:

| Member | Meaning |
| --- | --- |
| `request(&self) -> RequestOp<Empty>` | starts the shared `RequestOp` builder; no target parameter — DEALER has no API-level peer routing id |
| `set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError>` / `routing_id(&self) -> Result<RoutingId, ConfigError>` | assigns/reads this socket's own routing id, observed by peers on connect |
| `dealer_options() -> DealerSocketOptions<'_>` | returns the per-type options facade: `set_probe(bool)` — set-only, no getter; `weight()`/`set_weight(u32)`; `set_request_timeout(Duration)` — set-only, no getter, matching every other language's Dealer asymmetry on this option |

**Completion result.** `recv` (same shape as `PairSocket`) follows the same convention.

**When to use.** Set `set_routing_id` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request context (`Received::reply()`) or an explicit ROUTER reply surface instead.

---

## `RouterSocket`

Routes messages to peers addressed by routing id, and can reply to a specific peer's request.

```rust
let router = ctx.router_socket()?;
router.send(&peer_rid).message(Message::try_from("hello")?)?.submit()?;
```

**Options.** Same shared lifecycle/TLS surface as `PairSocket` (it declares its own copy
independently, same as every other socket type here), plus:

| Member | Meaning |
| --- | --- |
| `send(&self, target: &RoutingId) -> SendOp<Empty>` | starts the shared `SendOp`, addressed to that peer |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | populates `out` with the next message |
| `request(&self, peer_rid: &RoutingId) -> RequestOp<Empty>` | Messaging category's `RequestOp`, addressed to a specific peer |
| `reply(&self, rid: &RoutingId, request_seq: u64) -> ReplyOp<Empty>` | Messaging category's `ReplyOp`, answering that peer's request |
| `on_send_ready<F>(...)` | registers a back-pressure-cleared callback |
| `set_routing_id(&self, &RoutingId)` / `routing_id(&self)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `common_options()` | the shared options facade |
| `router_options() -> RouterSocketOptions<'_>` | returns the per-type options facade: `set_mandatory(bool)` — set-only; `set_probe(bool)` — set-only; `set_connect_routing_id(&RoutingId)` — set-only (**no getter for the assigned connect routing id in this binding**, unlike dotnet's/cpp's read-only `ConnectRoutingId`/`connect_routing_id()` property); `weight()`/`set_weight(u32)`; `request_timeout()`/`set_request_timeout(Duration)` — both directions, unlike Dealer's |

**Completion result.** `recv` follows the same convention as `PairSocket`. **This binding does not
declare `try_send_completion_control`/`set_completion_control_handler`** — the opaque
Completion-control surface documented on ROUTER in dotnet/cpp/java/node has no equivalent public
entry point here.

**When to use.** Use `request(peer_rid)`/`reply(rid, request_seq)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; SUB subscribes
with subscriptions set as socket options; XPUB/XSUB add subscriber-event surfacing and
message-carried subscriptions respectively. All four share their lifecycle/TLS surface via the
internal `impl_pubsub_common!` macro rather than a public trait.

```rust
let pub_socket = ctx.pub_socket()?;
pub_socket.publish("prices").message(Message::try_from(tick)?)?.submit()?;

let sub = ctx.sub_socket()?;
sub.set_subscription("prices.")?;
let mut msg = TopicMessage::empty();
if sub.subscribe(&mut msg, RecvFlags::NONE)? { /* ... */ }
```

**Options.**

| Type | Member | Meaning |
| --- | --- | --- |
| `PubSocket` | `publish(&self, topic: &str) -> SendOp<Empty>` | starts the shared `SendOp` builder; panics if `topic` fails an internal fixed-size check (via `fixed_topic_or_panic`) |
| | `on_send_ready<F>(...)` | registers a back-pressure-cleared callback |
| | `common_options()` | the shared options facade |
| | `pub_options() -> PubSocketOptions<'_>` | the per-type options facade |
| `SubSocket` / `XSubSocket` | `subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError>` | populates `out` with the next matching publish |
| | `set_subscription(&self, filter: &str)` / `unset_subscription(&self, filter: &str)` | adds/removes a topic filter; subscriptions accumulate |
| | `subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError>` | a `(filter, is_pattern)` tuple rather than a named struct/record — the filter at that index |
| | `common_options()` | the shared options facade |
| | `sub_options() -> SubSocketOptions<'_>` | the per-type options facade |
| `XPubSocket` | `receive_subscription_event(&self, out: &mut SubscriptionEvent, flags: RecvFlags) -> Result<bool, RecvError>` | adds this on top of the `PubSocket` shape, in its own `impl` block (not an extension of `PubSocket`'s type); populates `out` with the next subscribe/unsubscribe |

**Neither `PubSocket` nor `XPubSocket` declares `set_routing_id`/`routing_id`** (no routing-id
surface at all on either type in this binding). **`XSubSocket`'s `impl` block is a full,
independent copy of `SubSocket`'s** — same method set, same signatures, no shared type linking the
two beyond the identical shape.

**Completion result.** `subscribe`/`receive_subscription_event` follow the `Ok(false)`-on-`DONT_WAIT`
convention above.

**When to use.** Use `XPubSocket` specifically to observe subscriber churn via
`receive_subscription_event`, or manual admission via `PubSocketOptions::set_manual`/
`approve_subscribe`/`reject_subscribe`. Use `XSubSocket` specifically when subscriptions must be
carried as ordinary messages instead — the choice is entirely about which constructor you call
(`ctx.sub_socket()` vs. `ctx.xsub_socket()`), since the two types' method sets are identical.

---

## `StreamSocket`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by
every other socket type.

```rust
let stream = ctx.stream_socket()?;
stream.on_packet(|routing_id, header, body| { /* owns header/body */ })?;
```

**Options.**

| Member | Meaning |
| --- | --- |
| `send(&self, target: &RoutingId) -> SendOp<Empty>` | starts the shared `SendOp`, addressed to that peer |
| `recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>` | populates `out` with the next packet; this binding's `recv` additionally captures the source routing id as a send/reply context on `out`, so a subsequent `out.send()` addresses the packet's sender — a STREAM-specific enrichment not documented as such on other languages |
| `disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError>` | declared directly, since `StreamSocket` has no `connect`/`disconnect` at all — it never declares the connect/disconnect/disconnect_rid trio the other socket types share |
| `on_packet<F>(&mut self, handler: F) -> Result<(), HandlerError> where F: Fn(RoutingId, Message, Message) + Send + 'static` | registers a callback-driven packet loop; the handler owns both `header` and `body`, dropped when it returns |
| `on_send_ready<F>(...)` | registers a back-pressure-cleared callback |
| `set_routing_id(&self, &RoutingId)` / `routing_id(&self)` | assigns/reads this socket's own routing id, observed by peers on connect |
| `common_options()` | the shared options facade |
| `stream_options() -> StreamSocketOptions<'_>` | the per-type options facade: `set_notify(bool)`/`notify()` — delivers peer connect/disconnect as application messages when enabled |

**Completion result.** `recv` follows the `Ok(false)`-on-`DONT_WAIT` convention above.

**When to use.** Use `on_packet` for a callback-driven packet loop.

---

## Shared flags and enums

| Type | Used by | Values |
|---|---|---|
| `SendFlags` (tuple struct wrapping `u32`) | Every send/request/reply builder's `.flags(...)` stage (Messaging category) | `NONE`, `DONT_WAIT` (associated consts, not enum variants) |
| `RecvFlags` (tuple struct wrapping `u32`) | Every `recv`/`subscribe`/`receive_subscription_event` | `NONE`, `DONT_WAIT` (associated consts) |
| `RidDuplicatePolicy` | `CommonSocketOptions::rid_duplicate_policy` | `Reject`, `Handover` |
| `SubmitRetryMode` | `CommonSocketOptions::submit_retry_mode` | `Off`, `LocalFailure` |

**When to use.** `SendFlags`/`RecvFlags` are `struct`s with `pub const NONE`/`DONT_WAIT`
associated constants and a `bits()` accessor, not `enum` types — a design choice distinct from
every other language's flag representation covered so far (dotnet/java's `[Flags] enum`, cpp's
class with static members, node's frozen object constants).

---

See [`contracts/sockets/`](../../../../bindings/rust/src/contracts/sockets/) and the
[Rust binding spec](../../spec/rust/README.en.md) for the full rationale.

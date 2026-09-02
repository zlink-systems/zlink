[한국어](https://zlink-systems.github.io/zlink/ko/bindings/reference/cpp/03-sockets/) | English

[Reference index](README.en.md)

# 03. Sockets

This category covers `socket_t` (the shared, non-publicly-constructible base every concrete socket
type derives from), `common_socket_options_t` and its per-type subclasses, the eight concrete
`send`/`publish`/`request`/`reply` returns the operation-builder family documented in the Messaging
category — this category only covers where each builder starts and what each concrete type
uniquely adds. Unlike dotnet's `ISocket`/`IStreamSocket` interfaces, C++ does not expose role
interfaces by default — each socket type is a concrete RAII class; `socket_t` itself is not
publicly constructible (its constructor is `protected`). The exact signatures are owned by
[`Contracts/Sockets/`](../../../../bindings/cpp/include/zlink/Contracts/Sockets/).

---

## `socket_t` shared base

The non-publicly-constructible base every concrete socket type derives from: lifetime, binding,
TLS, monitoring. Data-plane `send`/`receive`/`publish` are `protected` here — each concrete socket
type below re-exposes the subset it needs as public methods.

```cpp
socket.bind ("tcp://*:5555");
socket.set_tls_server (cert_path, key_path, /*require_client_cert=*/true);
zlink::socket_monitor_t monitor = socket.monitor_open (zlink::monitor_event::all);
socket.close ();
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `valid()` | — | whether this socket is still usable |
| `close()` | — | releases the native socket immediately |
| `bind(const std::string&)` / `unbind(const std::string&)` | — | starts/stops listening on an address |
| `connect(const std::string&)` / `disconnect(const std::string&)` | — | connects/disconnects to a peer address |
| `disconnect_rid(const routing_id_t&)` | — | disconnects the peer identified by that routing id |
| `monitor_open(monitor_event events_) const` | `monitor_event::all` | returns `socket_monitor_t` (Eventing category) |
| `options()` | — | returns `common_socket_options_t`, below |
| `set_tls_server(cert, key, require_client_cert)` | `require_client_cert = false` | apply before `bind` |
| `set_tls_client(ca_cert, hostname, trust_system)` | `trust_system = false` | apply before `connect` |

**Completion result.** All synchronous, no return value except `valid()`/`monitor_open()`/
`options()`. `socket_t` is move-only (copy deleted); its destructor does not implicitly close.

**When to use.** Call `set_tls_server`/`set_tls_client` before `bind`/`connect`. Construct a
concrete socket type (below) directly — the base has no public constructor.

---

## `common_socket_options_t` and per-type option facades

The typed options facade shared by every socket type, reached via `socket.options()`.

```cpp
socket.options ().send_hwm (zlink::byte_count_t::bytes (100'000));
socket.options ().linger (std::chrono::seconds (1));
socket.options ().submit_retry_mode (zlink::submit_retry_mode_t::local_failure);
```

**Options.** `common_socket_options_t`:

| Member | Type | Meaning |
| --- | --- | --- |
| `linger()` | `std::chrono::milliseconds` | upper bound on how long `close()` waits for pending sends to flush |
| `send_hwm()` / `recv_hwm()` | `byte_count_t`, accounted-byte limits | send/receive queue limit — see Core category's byte-HWM note |
| `send_timeout()` / `recv_timeout()` / `connect_timeout()` | `std::chrono::milliseconds` | upper bound on how long the matching blocking operation waits |
| `immediate()` | `bool` | whether a send requires a live connection now, instead of queueing until one exists |
| `ipv6()` | `bool` | whether the socket accepts IPv6 connections |
| `tcp_no_delay()` | `bool` | disables Nagle's algorithm when `true` |
| `tcp_keepalive()` | `tcp_keepalive_mode_t` | OS TCP keepalive mode |
| `rid_duplicate_policy()` | `rid_duplicate_policy_t` | what happens when a peer reuses an existing routing id |
| `max_message_size()` | `byte_size_t` | maximum size in bytes of a single accepted message |
| `backlog()` | `socket_backlog_t` | pending-connection queue length for a listening socket |
| `reconnect_interval()` / `reconnect_interval_max()` | `std::chrono::milliseconds` | delay between reconnect attempts, and its cap |
| `submit_retry_mode()` | `submit_retry_mode_t` | whether a failed submit retries automatically on local back-pressure |
| `submit_retry_timeout()` | `std::chrono::milliseconds` | retry timeout when `submit_retry_mode()` is `local_failure` |
| `submit_retry_attempts()` | `int` | retry attempt cap when `submit_retry_mode()` is `local_failure` |
| `last_endpoint()` | `std::string`, read-only | the concrete resolved bind address |

Per-type subclasses (each constructed from a reference to their matching socket type):

| Type | Adds |
| --- | --- |
| `router_socket_options_t` | `mandatory()`, `handover()`, `probe()`, `connect_routing_id()` (`std::optional<routing_id_t>`), `request_timeout()`, `peer_weight()` (`peer_weight_t`) |
| `dealer_socket_options_t` | `probe()`, `request_timeout()`, `peer_weight()` |
| `stream_socket_options_t` | `notify()` (`bool`) |
| `pub_socket_options_t` | `verbose()`/`verboser()`/`no_drop()`/`manual()`/`manual_last_value()` (`bool`), `welcome_message()` (`message_t`), `approve_subscribe(const routing_id_t&)`/`reject_subscribe(const routing_id_t&)`, `topics_count()` (`int`) |
| `sub_socket_options_t` | `topics_count()` only |

**Completion result.** Every getter/setter is synchronous.

**When to use.** Set `send_hwm`/`recv_hwm` and `linger` before the socket starts exchanging
messages when the defaults don't fit the deployment.

---

## `pair_socket_t`

An exclusive one-to-one peering socket with no routing.

```cpp
zlink::pair_socket_t pair (ctx);
std::move (pair.send ()).message (part).submit ();
zlink::received_t received;
if (pair.recv (received) == 0) { /* ... */ }
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit pair_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `send()` | — | starts the shared `send_operation_t` builder |
| `recv(received_t&, recv_flags_t)` / `recv(message_t&, recv_flags_t)` | `recv_flags_t::none` | latter is a single-part shortcut |

**Completion result.** `recv` returns `int` directly — `0` on success, a `recv_result_t` value on
receive failure or no data, `-1` only for a binding-local failure with `errno` set (this
convention, rather than dotnet's `bool`, is shared by every concrete socket type's `recv` in this
category).

**When to use.** Use PAIR for an exclusive point-to-point link — it has no peer routing and does
not load-balance.

---

## `dealer_socket_t`

Load-balances sends across its connected peers and can issue routed requests.

```cpp
zlink::dealer_socket_t dealer (ctx);
dealer.set_routing_id (zlink::routing_id_t::from (std::string ("worker-3")));
auto reply = std::move (dealer.request ()).message (payload).async ().get ();
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit dealer_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `send()` / `recv(received_t&, recv_flags_t)` / `recv(message_t&, recv_flags_t)` | `recv_flags_t::none` | same shape as `pair_socket_t` |
| `request()` | — | starts the shared `request_operation_t`; no target parameter — DEALER has no API-level peer routing id |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | assigns/reads this socket's own routing id, observed by peers on connect |
| `options()` | — | returns `dealer_socket_options_t` |

**Completion result.** `recv` follows the same `int` convention as `pair_socket_t`.

**When to use.** Set `set_routing_id` before connecting so peers observe it from the first message.
DEALER has no protocol envelope helper to reply to an arbitrary token — reply from a received
request context (`received_t::reply()`) or an explicit ROUTER reply surface instead.

---

## `router_socket_t`

Routes messages to peers addressed by routing id, and can reply to a specific peer's request.

```cpp
zlink::router_socket_t router (ctx);
std::move (router.send (peer_rid)).message (part).submit ();
std::move (router.reply (peer_rid, reply_token)).message (reply).submit ();
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit router_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `send(const routing_id_t&)` | — | starts the shared `send_operation_t`, addressed to that peer |
| `recv(received_t&, recv_flags_t)` | `recv_flags_t::none` | populates the envelope with the next message |
| `recv(routing_id_t& source_rid_out_, message_t& part_out_, recv_flags_t)` | `recv_flags_t::none` | pull-based single-part receive; caller may keep a long-lived `received_t` across calls to reuse storage without reallocation |
| `request(const routing_id_t&)` | — | Messaging category's `request_operation_t`, addressed to a specific peer |
| `reply(const routing_id_t&, reply_token_t)` | — | Messaging category's `reply_operation_t`, answering the received request identified by the opaque token |
| `recv(...)` reply token | — | `received_t::reply_token()` carries the token; `received_t::reply()` captures it with the source RID |
| request terminal | — | reply or terminal failure is returned by `request(...).submit()`/`.async()`, never as application DATA |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | assigns/reads this socket's own routing id, observed by peers on connect |
| `options()` | — | returns `router_socket_options_t` |

**Completion result.** `recv` follows the `int` convention above. Request terminals settle from
the socket completion queue.

**When to use.** `request(peer_rid)`/`reply(rid, reply_token)` for ROUTER-initiated or
ROUTER-answered request/reply, where DEALER cannot address a specific peer. The token is opaque,
socket-bound metadata from receive and must not be synthesized.

---

## `pub_socket_t` / `xpub_socket_t`

PUB publishes topic-filtered messages, dropping ones with no matching subscriber; XPUB additionally
surfaces subscriber subscription/unsubscription events. Both derive from the internal
`publisher_socket_t` base (not itself publicly constructible).

```cpp
zlink::pub_socket_t pub (ctx);
std::move (pub.publish ("prices")).message (tick).submit ();

zlink::xpub_socket_t xpub (ctx);
zlink::subscription_event_t evt;
if (xpub.receive_subscription_event (evt) == 0) { /* ... */ }
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit pub_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `publish(const std::string& topic_id_)` | — | starts the shared `send_operation_t` |
| `options()` | — | returns `pub_socket_options_t` |
| `receive_subscription_event(subscription_event_t&, recv_flags_t)` | `recv_flags_t::none` | populates the event with the next subscribe/unsubscribe; `xpub_socket_t` only |

`pub_socket_t` has no `set_routing_id`/`get_routing_id` in this projection (unlike dotnet's
`IPubSocket`, which has both) — neither does `xpub_socket_t`.

**Completion result.** `receive_subscription_event` returns `int` (same convention as `recv`
above).

**When to use.** `xpub_socket_t` specifically to observe subscriber churn via
`receive_subscription_event`, or manual admission via `pub_socket_options_t::manual()`/
`approve_subscribe`/`reject_subscribe`; otherwise the two behave the same for publishing.

---

## `sub_socket_t` / `xsub_socket_t`

SUB subscribes to topics with subscriptions set as socket options; XSUB carries its subscriptions
as messages instead. Both derive from the internal `subscriber_socket_t` base — but each concrete
type re-declares its own public overloads with different signatures, so treat the base's shape as
internal plumbing, not the public contract callers use directly.

```cpp
zlink::sub_socket_t sub (ctx);
sub.set_subscription ("prices.");
zlink::topic_message_t msg;
if (sub.subscribe (msg) == 0) { /* ... */ }
```

**Options.** `sub_socket_t`:

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit sub_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `set_subscription(const std::string&)` / `unset_subscription(const std::string&)` | — | adds/removes a topic filter; subscriptions accumulate; returns `void`, not `[[nodiscard]] int` like the base |
| `subscription_at(size_t, std::string&, bool* = nullptr)` | — | writes the filter at that index into the output parameters |
| `subscription_at(size_t)` | — | value-returning overload, returns `subscription_filter_t` |
| `subscribe(topic_message_t&, recv_flags_t)` | `recv_flags_t::none` | populates the envelope with the next matching publish; returns `int`, not the base's throwing value-return form |
| `subscribe_part(std::optional<routing_id_t>& source_rid_out_, std::string& topic_out_, message_t& part_out_, bool& has_more_out_, recv_flags_t)` | `recv_flags_t::none` | pull-based single-part subscribe receive |
| `options()` | — | returns `sub_socket_options_t` |

`xsub_socket_t` has the identical member set to `sub_socket_t` — every method is separately
re-declared rather than inherited unchanged, but no member's behavior differs between the two
beyond what SUB/XSUB themselves mean.

**Completion result.** `subscribe`/`subscribe_part` return `int` (same convention as `recv`
above).

**When to use.** `sub_socket_t` for the common case; `xsub_socket_t` specifically when
subscriptions must be carried as ordinary messages instead.

---

## `stream_socket_t`

Exchanges framed packets directly with raw TCP peers, outside the zlink wire protocol used by every
other socket type.

```cpp
zlink::stream_socket_t stream (ctx);
stream.options ().recv_mode (zlink::stream_recv_mode_t::packet);
zlink::stream_packet_t packet;
bool ok = stream.recv_packet (packet, zlink::recv_flags_t::none);
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `explicit stream_socket_t(context_t&)` | — | constructs the socket, bound to that context |
| `send(const routing_id_t&)` | — | starts the shared `send_operation_t`, addressed to that peer |
| `recv(received_t&, recv_flags_t)` | `recv_flags_t::none` | pulls the next raw record in `stream_recv_mode_t::raw` |
| `recv_packet(stream_packet_t&, recv_flags_t)` | `recv_flags_t::none` | pulls one header/body packet with source RID in `stream_recv_mode_t::packet` |
| `set_routing_id(const routing_id_t&)` / `get_routing_id(routing_id_t&) const` | — | assigns/reads this socket's own routing id, observed by peers on connect |
| `options()` | — | returns `stream_socket_options_t` |

**Completion result.** `recv` follows the `int` convention above; `recv_packet` returns `false`
only for DONTWAIT no-data and otherwise transfers the packet messages to `stream_packet_t`.

**When to use.** Select RAW or PACKET with `options().recv_mode(...)` before bind/connect, then
call only the matching pull receive family.

---


Runs a bidirectional message-forwarding loop between two sockets (optionally steerable via a
control socket). Free functions, not socket methods — declared alongside `socket_t` in this
category rather than on a static facade (unlike dotnet's `Zlink.Proxy(...)`).

```cpp
zlink::proxy (frontend, backend);
zlink::proxy (frontend, backend, capture);
```

**Options.**

| Member | Meaning |
| --- | --- |
| `proxy(socket_t& frontend_, socket_t& backend_)` | forwards messages between the two sockets until the context terminates |
| `proxy(socket_t&, socket_t&, socket_t& capture_)` | same, plus a copy of every forwarded message sent to `capture_` |

**Completion result.** Both block the calling thread until the context is terminated (or, for
thread.

application needs to pause/resume/terminate the loop from another thread via the control socket.

---

## Socket enums and flags

Shared types referenced across every entry above.

| Type | Used by | Values |
|---|---|---|
| `socket_type` | Internal socket-kind identification | `any`, `pair`, `pub`, `sub`, `dealer`, `router`, `xpub`, `xsub`, `stream` |
| `rid_duplicate_policy_t` | `common_socket_options_t::rid_duplicate_policy`, `router_socket_options_t::handover` | `reject`, `handover` |
| `submit_retry_mode_t` | `common_socket_options_t::submit_retry_mode` | `off`, `local_failure` |
| `tcp_keepalive_mode_t` | `common_socket_options_t::tcp_keepalive` | `os_default`, `off`, `on` |
| `send_flags_t` (class w/ static members, not an `enum`) | synchronous publish flags | `none`, `dontwait` |
| `recv_flags_t` (class w/ static members, not an `enum`) | Every `recv`/`subscribe`/`receive_subscription_event` | `none`, `dontwait` |
| `send_result_t` | The outcome of a non-blocking send attempt | `sent`, `backpressured`, `not_ready` |
| `submit_result_t` | Thrown as `submit_error_t` (Errors category) | Mirrors `zlink_submit_result_t` (see Errors category) |
| `recv_result_t` | The `int` a `recv`-family call returns on failure/no-data | `ok`, `no_data`(201), `busy`(202), `terminated`(203), `invalid_handle`(204), `not_supported`(205), `internal_error`(206) |

**When to use.** `send_flags_t`/`recv_flags_t` are classes wrapping an `int`, not scoped `enum
class` types — `static const` members (`send_flags_t::dontwait`) rather than an enumerator, unlike
dotnet's `[Flags] enum SendFlags`. `dontwait` on either turns a blocking call into a non-blocking
one that reports back-pressure/no-data instead of blocking.

---

See [`Contracts/Sockets/`](../../../../bindings/cpp/include/zlink/Contracts/Sockets/) and the
[C++ binding spec](../../spec/cpp/README.en.md) for the full rationale.

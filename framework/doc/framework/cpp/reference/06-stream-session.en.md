# 06. Stream session

[Reference index](README.en.md)

This category covers the entry points used inside STREAM session code
(`packet_stream_session_t`, `stream_t`, `session_actor_manager_t`, `session_actor_t`) and the
entry point used for a bound session inside Actor code (`bound_session_t`). The exact signatures
are owned by the
[STREAM session exact interface](../../common/spec/server/languages/cpp/interfaces/06-stream-session.en.md)
and the
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.en.md)
(Korean-only).

---

## Session callback implementation (`packet_stream_session_t`)

Processes the lifecycle/packet events this STREAM session receives. Since C++ has no assembly
reflection, it does not register a handler class per packet type the way .NET does — these are
virtual members that `TSession`, registered via `register_session<TSession>()`
(topology-discovery category), overrides directly.

```cpp
class game_session_t : public zlink::framework::packet_stream_session_t {
public:
    zlink::framework::task_t<void> on_connected(
      zlink::framework::stream_t &stream) override;
    zlink::framework::task_t<void> on_disconnected(
      zlink::framework::stream_t &stream) override;
    zlink::framework::task_t<void> on_error(
      zlink::framework::stream_t &stream,
      const zlink::framework::stream_error_t &error) override;
    zlink::framework::task_t<void> on_packet(
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &context,
      const zlink::framework::message_t &payload) override;
};
```

**Options.** `context.packet_name` in `on_packet(...)` distinguishes which packet it is — a
single override processes every packet this session receives, with no separate registry call.

**Completion result.** All four callbacks return `task_t<void>`. `on_connected`/
`on_disconnected` each run once per connect/disconnect, `on_error` on every transport error, and
`on_packet` once per packet after the Framework's internal recv loop finishes header framing and
queue admission. A handshake failure happens before the session is created, so it is recorded
only in runtime monitoring, not `on_error`.

**When to use.** Every host that uses the `stream-session` topology implements this. Only a
packet (request) whose `context.can_reply` is `true` can be answered with `reply_packet`.

---

## `write_packet` (stream_t)

Sends a one-way message to the connected client.

```cpp
co_await stream
  .write_packet(zlink::framework::message_t::from(server_tick_t{tick_number}))
  .submit();
```

**Options.** `stream_send_call_t` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the client |
| `.packet_name(name)` | The `packet_name` of the payload type | Explicitly specifies this packet's name |
| `.compress()` | Uncompressed | Compresses the payload with the registered stream compression codec |
| `.submit()` | Required terminal | Waits only until source-local admission |

**Completion result.** The same one-way completion kinds as the messaging-execution category —
waits until the socket send timeout and, if still not admitted, completes as a
`framework_exception_t` with `deadline_exceeded`; a connection disconnect is `unavailable`.

**When to use.** Use this for a server-initiated push message, not a client-sent request. Use
`reply_packet` to answer a client's request.

---

## `reply_packet` (stream_t)

Responds to the request packet currently being processed.

```cpp
co_await stream
  .reply_packet(zlink::framework::message_t::from(get_player_state_result_t{state}))
  .submit();
```

**Options.** `stream_write_call_t` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the client |
| `.compress()` | Uncompressed | Compresses the payload with the registered stream compression codec |
| `.submit()` | Required terminal | Waits only until source-local admission |

**Completion result.** Atomically claims this request's one-shot reply token and then sends. A
second `reply_packet` call made with the same token fails to claim it, does not attempt the
transport, and ends as an exceptional completion. The caller's request timeout is not carried
over the wire, so this reply's admission deadline uses only the STREAM socket send timeout. No
late reply is sent after the timeout.

**When to use.** Use this only for a packet (request) whose `session_message_context_t::can_reply`
is `true`. Use `write_packet` to send a new message that was not client-initiated.

---

## `bind` / `bind_or_get` (session_actor_manager_t)

Binds an Actor to this STREAM session so the Actor side can push over this connection. Called via
`stream_t::actors()`.

```cpp
zlink::framework::session_actor_t bound =
  co_await stream.actors().bind_or_get(actor_ref).submit<zlink::framework::session_actor_t>();
```

**Options.** This call has no modifiers — it only takes `actor_ref_t`. The returned
`request_call_t<session_actor_t>` uses the same `.timeout(...)`/terminal shape as a request call
in the messaging-execution category.

**Completion result.** `bind` creates a new binding every time. `bind_or_get` returns the
already-bound one if the same incarnation is already bound. A binding is fixed to one exact
incarnation of `actor_id + object_generation`. No mapping completes with `not_found`, a differing
generation with `invalid_operation`, and a pre-commit seal in progress with `unavailable`.
`find(actor_id)` synchronously queries an already-bound handle, and `bound()` returns the full
list bound to the current session.

**When to use.** Bind when an Actor must push directly over this client connection. Even across
a relocation, `session_actor_t::ref()` refreshes to the current location snapshot, so the
application does not need to bind again.

---

## `relay` / `notify_disconnected` (session_actor_t)

Delivers a payload from the Actor side to the client, or notifies of a connection disconnect,
through the `session_actor_t` obtained from bind.

```cpp
co_await bound.relay(zlink::framework::message_t::from(room_updated_t{state}));
```

**Options.** Neither call has modifiers — both only take the payload (`relay`). `relay` also has
an overload that takes a `session_message_context_t`.

**Completion result.** `relay` is a one-way `task_t<void>` operation that completes normally once
source-local admission is accepted. `notify_disconnected` is a notification that signals a
logical disconnect while the connection remains open, and waits until the callback's terminal.
Because a physical disconnect is automatically notified by the Framework to every current
binding, this call is not a substitute path for that.

**When to use.** Use this from Actor-side code to deliver directly to a specific bound client. A
reply to a request is handled by `reply_packet` on the Session side.

---

## `send` (bound_session_t, inside Actor code)

Sends a one-way message from an Actor to the client bound to it. Called via the
`bound_session_t` that `actor_context_t::bound_session()` returns.

```cpp
co_await context_.bound_session()
  .send(inventory_changed_t{item})
  .submit();
```

**Options.** `bound_session_send_call_t` provides `.metadata(...)` and the required terminal
`.submit()`.

**Completion result.** The same one-way completion kinds as the messaging-execution category.
This surface does not provide a new request operation aimed at the client — a reply to a client
request is handled by the Actor request handler's return value.

**When to use.** Use this from Actor-side code to push to the bound client. Use the
`write_packet` entry above to send directly from the Session side. Use
`bound_session_t::disconnect()` to disconnect.

---

## `close` (ending a connection)

Closes the session or the raw transport handle. Provided by `stream_t::close()`.

```cpp
co_await stream.close();
```

**Options.** This call has no modifiers.

**Completion result.** Closes the connection. Calling it again on an already-closed connection
carries no separate exception contract this document defines — check the exact interface for the
precise re-call semantics.

**When to use.** Use this when the application must voluntarily disconnect this STREAM
connection. Use `bound_session_t::disconnect()` to disconnect a bound client connection from the
Actor side.

---

## `disconnect` (inside Actor code, bound session)

Disconnects the client bound to an Actor. Called via `bound_session_t::disconnect()`.

```cpp
co_await context_.bound_session().disconnect();
```

**Options.** This call has no modifiers.

**Completion result.** Disconnects the connection with the bound session.

**When to use.** Use this from Actor-side code when a specific client connection no longer needs
to be kept. Use the `close` entry to disconnect directly from the Session side.

---

See the
[STREAM session exact interface](../../common/spec/server/languages/cpp/interfaces/06-stream-session.en.md)
and the
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.en.md)
(Korean-only) for the full rationale.

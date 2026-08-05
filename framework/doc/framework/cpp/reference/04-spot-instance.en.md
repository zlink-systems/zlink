# 04. Spot instance

[Reference index](README.en.md)

This category covers the external entry points `spot_manager_t`/`route_client_t`/
`spot_publisher_client_t` provide, and the entry points used inside Spot code via
`spot_context_t`/`spot_common_context_t`. The exact signatures are owned by the
[Spot exact interface](../../common/spec/server/languages/cpp/interfaces/04-spots.en.md)
(Korean-only).

---

## `spot_manager_t::create`

Always creates a new User Spot. The Framework issues a new global SpotId.

```cpp
zlink::framework::spot_create_result_t created = co_await spot_manager
  .create("room")
  .in_mesh("play")
  .creation_request(create_room_t{"ranked"})
  .timeout(std::chrono::seconds{5})
  .submit();

std::string spot_id = created.spot.spot_id();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.in_mesh(mesh_name)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to create the Spot in. Omitting it with two or more candidates completes with `invalid_operation`; none completes with `not_configured`; a nonexistent specified Mesh completes with `not_found` |
| `.creation_request(message_t)` / `.creation_request<TRequest>(TRequest)` | None (empty request) | The creation request passed to the Spot's `on_create(...)` |
| `.timeout(milliseconds)` | Default applied to the whole of resolve/factory/initialize | The upper bound until the entire creation reaches a terminal state |
| `.submit()` | terminal (pick one) | Waits until creation completes |
| `.yield()` | terminal (pick one) | Only valid inside a `spot_wide` handler |

**Completion result.** `spot_create_result_t::state` is `created` (newly created). If the Spot's
`on_create(...)` rejects it, it is `rejected` and `reply` carries the rejection message. Setting
the same option twice, or calling a terminal twice, completes with `invalid_operation`; not
finishing within the deadline completes with `deadline_exceeded`.

**When to use.** Use this when a new instance is always needed. Use `get_or_create` to reuse an
existing one and only create when there is none.

---

## `spot_manager_t::get_or_create`

Returns the Ready Spot with the specified SpotId if it exists, and creates a new one otherwise.

```cpp
zlink::framework::spot_create_result_t existing_or_created = co_await spot_manager
  .get_or_create("lobby-eu", "lobby")
  .in_mesh("play")
  .creation_request(create_lobby_t{"eu"})
  .submit();
```

**Options.** The same as `create` — `.in_mesh(...)`, `.creation_request(...)`, `.timeout(...)`,
terminal `.submit()` or `.yield()`.

**Completion result.** If `state` is `existing`, it returns the already-existing Spot as-is and
ignores `creation_request`. `created` means a new one was made. If the same SpotId is currently
contended in a `creating` state, it waits for that result and joins it; if cleanup makes it
missing, it re-competes for a new reservation. If the stable type differs from the existing
authority, it completes with `type_mismatch`.

**When to use.** Use this when an idempotent "use if it exists, create if it doesn't" by SpotId
is needed. Use `create` if a new instance is always needed.

---

## `spot_manager_t::find` / `close`

Queries an existing Spot, or closes the exact incarnation.

```cpp
std::optional<zlink::framework::spot_ref_t> spot =
  co_await spot_manager.find("lobby-eu");

if (spot) {
    bool closed = co_await spot_manager.close(*spot);
}
```

**Options.** Neither call has modifiers — both only take the target identifier.

**Completion result.** `find` returns `std::nullopt` if there is no Ready Spot. `close` returns
`false` if the incarnation does not exist, completes with `invalid_operation` if the generation
differs, and `unavailable` while a pre-commit seal is in progress. If a User Spot still has Actor
membership, it returns `false` and does not automatically leave/destroy the Actor.

**When to use.** Use this when you need to check current existence or explicitly terminate a
Spot. `close` does not close a different incarnation on behalf of a stale `spot_ref_t`.

---

## `send_to_spot<TMessage>`

Sends a one-way message to a single global SpotId. The external client (`route_client_t`) and
Spot code (`spot_common_context_t`) provide the same shape.

```cpp
co_await route_client
  .send_to_spot("room-42", player_joined_room_t{"player-1"})
  .submit();

// activating a new Instance Spot on demand (cold activation) before sending
co_await route_client
  .send_to_spot("device-42", device_command_t{"reboot"})
  .instance_spot("device")
  .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the handler |
| `.instance_spot()` | None (resolves User Spot only) | Performs cold activation if missing. The stable type can be omitted only if exactly one Instance Spot type is registered |
| `.instance_spot(stable_type)` | — | The stable type must be specified when several types are registered |
| `.in_mesh(mesh_name)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to first create a missing Instance Spot in. Using it without an instance marker completes with `invalid_operation` |
| `.submit()` | Required terminal | Waits only until source-local admission |

**Completion result.** No SpotId and no Instance marker completes with `not_found`. If
`.instance_spot(...)` was used but the existing authority is a User Spot, or differs from the
specified type, it completes with `type_mismatch`. Other completion kinds follow the same common
rules as the messaging-execution category.

**When to use.** Use this for Spot messaging where no reply is needed. Use `request_to_spot` if a
reply is needed.

---

## `request_to_spot<TRequest>`

Sends and receives a typed request/reply to a single global SpotId.

```cpp
room_state_t reply = co_await route_client
  .request_to_spot("room-42", get_room_state_t{})
  .timeout(std::chrono::seconds{3})
  .submit<room_state_t>();
```

**Options.** In addition to the same `.instance_spot(...)`/`.in_mesh(...)` as `send_to_spot`, this
adds the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(milliseconds)` | The MeshNode's request default timeout | The deadline covering resolve, cold activation, handler, and reply altogether |
| `.submit<TReply>()` | terminal (pick one) | Waits until the reply arrives |
| `.yield<TReply>()` | terminal (pick one) | Only valid inside a `spot_wide` User Spot/Instance Spot handler. Calling it elsewhere completes with `invalid_operation` |

**Completion result.** In addition to the same failure kinds as `send_to_spot`, if the factory or
initialize fails during cold activation, it completes as a typed failure — the Framework does not
retry internally.

**When to use.** Use this when the reply value is needed. Use `send_to_spot` if it is one-way.

---

## `publish<TEvent>` (Spot Logical Multicast)

Publishes a typed event to subscribers by ChannelName and topic. `spot_publisher_client_t`
(external) and `spot_common_context_t::publish` (inside Spot code) provide the same shape.

```cpp
co_await spot_publisher_client
  .publish("room.events", "room-42", room_state_changed_t{"started"});
```

**Options.** This call returns a `publish_call_t` that requires `submit()` after an optional
`.metadata(...)` (external `spot_publisher_client_t` only) — the topic is a required argument.

**Completion result.** A normal completion means publish admission finished. It does not wait for
subscriber reception. Unlike classic fanout `publish` in the messaging-execution category, the
owner MeshNode is determined by ChannelName alone, and the caller does not pass a MeshName
separately.

**When to use.** Use this to notify observers of a Spot state change. If a direct reply from a
subscriber is needed, use `request_to_spot` instead of this entry.

---

## `add_timer<THandler>` (inside Spot code)

Registers a periodic timer belonging to a Spot. Called via
`spot_common_context_t::add_timer(...)`.

```cpp
zlink::framework::timer_t timer = context_.add_timer<room_tick_handler_t>(
  "room-tick",
  std::chrono::seconds{1},
  zlink::framework::timer_options_t{
      .overrun_policy =
        zlink::framework::timer_overrun_policy_t::skip_late_ticks,
  });
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `options.overrun_policy` | `skip_late_ticks` | Whether to skip when a tick falls behind, catch up within a bound, or delay the next tick |
| `options.max_catch_up_ticks` | 1 | The maximum ticks to catch up at once when `catch_up_bounded` |
| `options.stop_on_unhandled_exception` | `false` | Whether to stop the timer on a handler exception |

**Completion result.** Returns a `timer_t`. Because the timer is a logical registration belonging
to this Spot, it is automatically carried over on relocation and the application does not need to
re-register it at the target. Cancel it with `cancel()`.

**When to use.** Use this when a Spot needs periodic work.

---

## `run_cpu_worker` / `run_io_worker` (inside Spot code)

Runs work on a separate worker without blocking the Spot's owner turn.

```cpp
// CPU-bound work takes a synchronous callable.
int result = co_await context_
  .run_cpu_worker([](std::stop_token token) {
      return compute_expensive_score(token);
  })
  .timeout(std::chrono::seconds{2})
  .submit();

// Work that waits on I/O takes a callable that returns task_t<TResult>.
std::string fetched = co_await context_
  .run_io_worker([](std::stop_token token) -> zlink::framework::task_t<std::string> {
      co_return co_await fetch_remote_profile(token);
  })
  .submit();
```

**Options.** `worker_call_t<TResult>` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(milliseconds)` | `worker_options_t`'s default | The upper bound for the work to complete |
| `.submit()` | terminal (pick one) | Waits until completion |
| `.yield()` | terminal (pick one) | Only valid inside a `spot_wide` handler |

**Completion result.** Returns `TResult`, or completes with `deadline_exceeded` on timeout. The
worker pool size (`min_threads`/`max_threads`) and idle timeout are configured only before the
host starts.

**When to use.** Use `run_cpu_worker`, which takes a synchronous callable, for CPU-bound
computation, and `run_io_worker`, which takes a callable returning `task_t<TResult>`, for work
that waits on I/O. Both exist to avoid blocking the owner turn's sequential execution.

---

## Handler registration (inside Spot code, `configure()`)

Registers the handlers that process the packets/requests/subscriptions/member Actor messages a
Spot receives. Called via `spot_context_t::handlers()` (User Spot)/
`instance_spot_context_t::handlers()` (Instance Spot), and only from inside the `configure()`
override.

```cpp
void room_spot_t::configure() {
    context().handlers()
      .add_handler<&room_spot_t::start_game>()
      .add_subscribe<&room_spot_t::on_score_event>("game.scores", "world")
      .add_actor_send<&room_spot_t::on_player_command>();
}
```

**Options.** The registration method differs by what the handler processes.

| Target | Registration method |
| --- | --- |
| One-way packet/request in front of a User Spot | `spot_handler_registry_t::add_handler<Method>(packet_name = {})` |
| A Logical Multicast subscription event | `spot_handler_registry_t::add_subscribe<Method>(channel_name, topic)` |
| A one-way packet in front of a User Spot's member Actor | `spot_handler_registry_t::add_actor_send<Method>(packet_name = {})` |
| A request in front of a User Spot's member Actor | `spot_handler_registry_t::add_actor_request<Method>(packet_name = {})` |
| A packet in front of an Instance Spot | `instance_spot_handler_registry_t::add_handler<Method>(packet_name = {})` |

`Method` is a member-function pointer passed as a non-type template parameter (in the form
`&room_spot_t::start_game`). Since C++ has no assembly reflection, the Spot's own member
functions are registered directly instead of building a separate handler class.

**Completion result.** Registers synchronously with no return value. Omitting the packet name
uses the `packet_name` of the message type the handler processes, and the C++ type name if there
is none. A duplicate handler key under the same owner surfaces as a configuration error in
`app.run(...)`'s startup validation.

**When to use.** Registers every handler this Spot will process, each time `configure()` is
called. See the registration entries in the topology-discovery category for Node/Channel
handlers, and the stream-session category for STREAM session handlers.

---

## `outbound()` — `send_to_channel` / `request_to_channel` (inside Spot code)

Sends a one-way message by ChannelName, or exchanges a typed request/reply, from inside Spot
code. Provided by the `channel_client_t` that `spot_common_context_t::outbound()` returns, in the
same shape as `send_to_channel`/`request_to_channel` in the messaging-execution category.

```cpp
leaderboard_t reply = co_await context_.outbound()
  .request_to_channel("leaderboard.api", get_leaderboard_t{})
  .submit<leaderboard_t>();
```

**Options.** Takes the same modifiers as `send_to_channel`/`request_to_channel` in the
messaging-execution category.

**Completion result.** Same as the completion kinds in the messaging-execution category.

**When to use.** Use this when a Spot must call a handler on a different ChannelName from inside
its own code, rather than an external client doing so. Use `send_to_spot`/`request_to_spot` to
call another Spot directly.

---

## `leave_actor` / `close` / `destroy_actor` (inside Spot code, termination/departure)

Removes a member Actor from this Spot, closes the Spot itself, or destroys an Actor from an Entry
Spot.

```cpp
co_await context_.leave_actor(actor);        // User Spot: only removes the member Actor
bool closed = co_await context_.close();     // User/Instance Spot: closes this Spot itself
co_await entry_context_.destroy_actor(actor); // Entry Spot: destroys the Actor entirely
```

**Options.** None of the three calls has modifiers — they only take the target
(`leave_actor`/`destroy_actor`).

**Completion result.** `leave_actor` (`spot_context_t` only) only releases member Actor
membership and does not destroy the Actor itself. `close` (`spot_context_t`/
`instance_spot_context_t`) uses the same completion kinds as the manager's `close(spot_ref)`
(the earlier entry in the spot-instance category), but targets this Spot itself.
`destroy_actor` (`entry_spot_context_t` only) destroys the Actor entirely — unlike `leave_actor`,
it removes the Actor itself rather than releasing membership.

**When to use.** Use `leave_actor` to remove a member Actor from this Spot without moving it
elsewhere, `close` to terminate the Spot itself, and `destroy_actor` to entirely remove an Actor
that is no longer needed at an Entry Spot.

---

## `relocation_ready().defer()` (inside Spot code)

In a `spot_wide` Spot that has chosen `application_signaled` readiness mode, defers the
relocation boundary to just before the next application turn.

```cpp
context_.relocation_ready().defer();
```

**Options.** This call has no modifiers.

**Completion result.** No return value. Registers the relocation boundary after the current
handler ends. If it did not move, or aborted before commit, it receives a `continued` completion
at the source; if it moved, it receives a `relocated` completion at the target, via
`on_relocation_ready_completed(...)`. `any_turn_boundary` mode, a `per_actor` Spot, an
Entry/Instance Spot, outside a Spot turn, or a duplicate call in the same turn all complete with
`invalid_operation`.

**When to use.** Use this when the application must precisely control the relocation moment down
to a specific turn boundary. This call is not needed under the default `any_turn_boundary` mode.

---

See the
[Spot exact interface](../../common/spec/server/languages/cpp/interfaces/04-spots.en.md)
(Korean-only) for the full rationale.

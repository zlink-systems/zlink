# 05. Actor relocation

[Reference index](README.en.md)

This category covers the external entry points `actor_manager_t`/`actor_client_t` provide, the
entry point for joining a Spot from inside Actor code via `actor_context_t`, and relocation policy
selection. The exact signatures are owned by the
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.en.md)
(Korean-only).

---

## `actor_manager_t::create`

Always creates a new Actor.

```cpp
zlink::framework::actor_create_result_t created = co_await actor_manager
  .create(zlink::framework::actor_id_t{"player-1"}, "player")
  .in_mesh("play")
  .creation_request(spawn_player_t{"player-1"})
  .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.in_mesh(mesh_name)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to create the Actor in. Omitting it with two or more candidates completes with `invalid_operation`; none completes with `not_configured`; a nonexistent specified Mesh completes with `not_found` |
| `.creation_request(message_t)` / `.creation_request<TCreation>(TCreation)` | None (empty request) | The request passed at Actor factory creation time |
| `.timeout(milliseconds)` | 5 seconds | The deadline covering resolve/reservation/factory/Ready barrier altogether |
| `.submit()` | terminal (pick one) | Waits until creation completes |
| `.yield()` | terminal (pick one) | Only valid inside a `spot_wide` handler |

**Completion result.** `actor_create_result_t` (a `std::variant`) completes as one of
`actor_create_created_t` (newly created) or `actor_create_rejected_t` (the factory rejected it).
If a Ready incarnation of the same ActorId already exists, it completes with an
`already_exists` error rather than either alternative — `actor_create_existing_t` only exists for
`get_or_create`. If a Ready incarnation exists but its stable type differs, it is
`type_mismatch`.

**When to use.** Use this when a new Actor is always needed. Use `get_or_create` to reuse an
existing one and only create when there is none.

---

## `actor_manager_t::get_or_create`

Returns the Ready Actor with the same ActorId if it exists, and creates a new one otherwise.

```cpp
zlink::framework::actor_create_result_t existing_or_created = co_await actor_manager
  .get_or_create(zlink::framework::actor_id_t{"player-1"}, "player")
  .in_mesh("play")
  .creation_request(spawn_player_t{"player-1"})
  .submit();
```

**Options.** The same as `create` — `.in_mesh(...)`, `.creation_request(...)`, `.timeout(...)`,
terminal `.submit()` or `.yield()`.

**Completion result.** `actor_create_existing_t` returns the already-existing Actor and ignores
`creation_request`. Contending with a creating attempt waits for that result and joins it; a
distinct operation receives `actor_create_existing_t` after Ready and does not share the earlier
reply.

**When to use.** Use this when an idempotent "use if it exists, create if it doesn't" by ActorId
is needed.

---

## `find` / `find_spot` / `destroy` (manager)

Queries an existing Actor, queries the Spot it currently participates in, or terminates the exact
incarnation.

```cpp
std::optional<zlink::framework::actor_ref_t> actor =
  co_await actor_manager.find(zlink::framework::actor_id_t{"player-1"});
std::optional<zlink::framework::spot_ref_t> spot =
  co_await actor_manager.find_spot(zlink::framework::actor_id_t{"player-1"});

if (actor) {
    bool destroyed = co_await actor_manager.destroy(*actor);
}
```

**Options.** None of the three calls has modifiers — all only take the target identifier.

**Completion result.** `find` returns `std::nullopt` if there is no Ready Actor. `find_spot`
returns `std::nullopt` if there is no current User Spot membership. `destroy` returns `false` if
the incarnation does not exist, completes with `invalid_operation` if the generation differs, and
`unavailable` while a pre-commit seal is in progress.

**When to use.** Use this when you need to check current existence/membership, or explicitly
terminate an Actor.

---

## `send` / `request` (actor_client_t)

Sends a one-way message, or exchanges a typed request/reply, to a single global ActorId. Used
from an external client.

```cpp
co_await actor_client
  .send(zlink::framework::actor_id_t{"player-1"}, grant_item_t{"sword"})
  .submit();

inventory_t reply = co_await actor_client
  .request(zlink::framework::actor_id_t{"player-1"}, get_inventory_t{})
  .timeout(std::chrono::seconds{3})
  .submit<inventory_t>();
```

**Options.** `send` only has `.metadata(...)` and terminal `.submit()`. `request` additionally
has the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(milliseconds)` | The MeshNode's request default timeout | The upper bound for waiting on the reply |
| `.submit<TReply>()` | terminal (pick one) | Waits until the reply arrives |
| `.yield<TReply>()` | terminal (pick one) | Only valid inside a `spot_wide` handler |
| `.submit_message()` / `.yield_message()` | terminal (pick one) | Receives a raw `message_t` instead of a typed reply |

**Completion result.** No ActorId completes with `not_found`. The remaining completion kinds
follow the same common rules as the messaging-execution category.

**When to use.** Use `send` if no reply is needed, and `request` if one is.

---

## `join_spot` / `join_entry_spot` (inside Actor code)

Joins the current Actor to a User Spot or an Entry Spot. Called via
`actor_context_t::join_spot(...)`/`join_entry_spot(...)` — unlike other entries, the only terminal
here is `defer()`, not `submit`/`yield`.

```cpp
context_
  .join_spot("room-42", join_room_request_t{"player-1"})
  .timeout(std::chrono::seconds{5})
  .defer();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(milliseconds)` | 5 seconds | A monotonic absolute deadline |
| `.defer()` | Required terminal | A synchronous call with no result. Only registers the join intent and an inactive barrier — it does not start the target lookup immediately |

**Completion result.** `defer()` itself has no return value. If the current handler ends
normally, the barrier activates and executes the Join; if the handler fails, the barrier is
discarded. The actual result (accepted/rejected/failed) is delivered asynchronously via the
`actor_t::on_join_completed(...)` callback carrying the same 128-bit operation ID — one of the
`std::variant` alternatives `actor_join_accepted_t`/`actor_join_rejected_t`/
`actor_join_failed_t`.

**When to use.** Use this to move an Actor to a different Spot, or return it to an Entry Spot.
Calling it from an Actor in an Entry Spot or a `per_actor` User Spot completes with
`invalid_operation`.

---

## Relocation policy selection (at Actor factory registration time)

Choose exactly one, in the `configure` callback of `add_actor_factory<TActor, TFactory>(...)`
(topology-discovery category).

| Policy | Behavior on cross-node move | When to use |
| --- | --- | --- |
| `disable_relocation()` | Rejects the move itself before Capture | When this Actor must never be moved to another node |
| `recreate_on_relocation()` | Recreates the same logical identity via the target factory. Does not restore application state | When an Actor may be recreated without state |
| `preserve_state_with<TAdapter>()` | Moves an opaque byte vector via `actor_relocation_adapter_t<TActor>::capture`/`restore` | When state must be preserved across the move |

**Completion result.** `preserve_state_with`'s `capture(...)` result is capped at 64 MiB.
Capture/Restore can each be called multiple times within the same relocation, so both callbacks
must be retry-safe — they must not depend on an external side effect executing exactly once.

**When to use.** Which of the three policies you choose determines this Actor type's entire
relocation behavior — it is decided once, at factory registration time, and cannot be changed
per call afterward.

---

See the
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.en.md)
(Korean-only) for the full rationale.

# C++ Actor Exact Interface

Spot relocation that includes an Actor bound to a Session restores the
Actor and queue at the target, commits owner and membership, and then
starts message processing. The target runtime sends
`sessionActorLocationUpdateReqMsg` to update the binding route and the
bound session's current Actor location snapshot. Even without a
response, Actor processing doesn't stop, and the same request is
resent at a fixed interval. The snapshot provides the target
MeshName/NodeRid. Since relocation itself isn't a physical/logical
disconnect, it doesn't run the Actor disconnect callback. It doesn't
change the route and physical connection of a different Actor not
included in the relocation target.

[C++ exact interface table of contents](README.en.md) · [Actor Model](../../../../14-actor-model.en.md) ·
[Spot · Actor Membership](../../../../15-spot-actor.en.md)

## 1. Identity And Maintenance Policy

The exact declaration of `actor_context_t`, which an Actor uses, is
owned by the [Spot interface](04-spots.en.md).

```cpp
namespace zlink::framework {

class actor_id_t final {
public:
    explicit actor_id_t(std::string value);
    std::string_view value() const noexcept;
    auto operator<=>(const actor_id_t &) const = default;
};

class actor_ref_t final {
public:
    actor_ref_t(actor_id_t actor_id,
      std::uint64_t object_generation,
      std::string mesh_name,
      node_rid_t node_rid);

    const actor_id_t &actor_id() const noexcept;
    std::uint64_t object_generation() const noexcept;
    std::string_view mesh_name() const noexcept;
    const node_rid_t &node_rid() const noexcept;
};

struct actor_join_accepted_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_join_rejected_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    std::optional<message_t> reply;
};

struct actor_join_failed_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    framework_error_kind_t error_kind;
};

using actor_join_completion_t = std::variant<
  actor_join_accepted_t,
  actor_join_rejected_t,
  actor_join_failed_t>;

class actor_t {
public:
    virtual ~actor_t() = default;
    virtual actor_context_t &context() noexcept = 0;
    virtual const actor_context_t &context() const noexcept = 0;
    virtual void configure() {}
    virtual task_t<void> on_join_completed(
      const actor_join_completion_t &completion);
};

template <typename TActor>
  requires std::derived_from<TActor, actor_t>
class actor_factory_t {
public:
    virtual ~actor_factory_t() = default;
    virtual task_t<std::shared_ptr<TActor>> create(
      actor_context_t context,
      std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_relocation_adapter_t {
public:
    virtual ~actor_relocation_adapter_t() = default;
    virtual task_t<std::vector<std::byte>> capture(
      TActor &actor,
      std::stop_token operation_cancellation) = 0;
    virtual task_t<void> restore(
      TActor &actor,
      std::vector<std::byte> payload,
      std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_factory_builder_t {
public:
    void disable_relocation();
    void recreate_on_relocation();
    template <typename TAdapter>
      requires std::derived_from<TAdapter, actor_relocation_adapter_t<TActor>>
    void preserve_state_with();
};

} // namespace zlink::framework
```

`actor_id_t` is a UTF-8 `1..255`-byte exact global identity. The
constructor rejects an invalid value with `std::invalid_argument` and
doesn't apply trim, case folding, or Unicode normalization.
`actor_ref_t` is an immutable location snapshot holding the global
ActorId, a non-zero `1..9223372036854775807` ObjectGeneration, and the
MeshName/NodeRid at lookup time. It isn't used as an ordinary message
target. A separate `actor_ref_snapshot_t` isn't provided.

`actor_t` is a typed lifecycle base that owns the `actor_context_t` the
Framework wired in. ActorId and ObjectGeneration are read from
`context()`, and a separate identity value isn't stored independently.
The Framework builds the concrete Actor with
`actor_factory_t<TActor>::create(...)` and then calls `configure()`.
The factory only uses the exact context and cancellation it received,
and doesn't take ActorId, a different owner RID, relocation phase, or
Store token as a duplicate input.

Join completion's 128-bit operation ID is a completion idempotency ID
— it isn't `RelocationId`, a reservation ID, or an aggregate commit ID.
Same-node and cross-node completion retry is bounded to the current
source and target process lifetime. After process exit, a different
runtime doesn't automatically replay the completion.

Every Actor factory configure callback selects exactly one policy.
`preserve_state_with<TAdapter>()`'s `TAdapter` must implement
`actor_relocation_adapter_t<TActor>`, and a different adapter type
fails as a configuration error before socket bind. The adapter
exchanges application state only as an opaque byte vector, and doesn't
receive typed state, a separate contract identifier, a message
wrapper, authority, relocation reference, relocation phase, or
operation ID.

The Framework only calls the adapter in
`preserve_state_with<TAdapter>()`'s cross-node Actor materialization.
This includes maintenance handoff, remote User/Entry Spot join, and
each Actor participant of a whole User Spot relocation. It doesn't
call the adapter on a same-node join/relocation, and a
`DisableRelocation` cross-node operation is rejected before
`capture(...)`. A `RecreateOnRelocation` policy also doesn't
capture/restore the application payload. A whole User Spot relocation
uses `spot_relocation_adapter_t<TSpot>` for the Spot root, and this
Actor adapter for each Actor participant.

`capture(...)`'s result is at most 64 MiB, and an empty vector is
valid. Ownership of the returned byte vector moves to the Framework,
and the byte vector passed to `restore(...)` is owned by that async
call. If capture throws or ends as a failed task, admission is
restored after durable abort and source normalization. A failed
restore's instance is discarded, and the same immutable payload is
applied to the instance the new attempt's factory built. If the
Framework cancels a callback due to the operation deadline, it's
classified as `deadline_exceeded`. Because a retry within the same
source and target process can call either method more than once, the
implementation must be retry-safe. A different target isn't
automatically selected. The Framework doesn't guarantee exactly-once
for the adapter's external side effect.

## 2. ID-Only Messaging

```cpp
namespace zlink::framework {

class actor_send_call_t {
public:
    actor_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class actor_request_call_t {
public:
    actor_request_call_t &timeout(std::chrono::milliseconds timeout);
    actor_request_call_t &metadata(std::string key, std::string value);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();

    task_t<message_t> submit_message();
    task_t<message_t> yield_message();
};

class actor_client_t {
public:
    virtual ~actor_client_t() = default;

    template <typename TMessage>
    actor_send_call_t send(actor_id_t actor_id, TMessage message);

    template <typename TRequest>
    actor_request_call_t request(actor_id_t actor_id, TRequest request);
};

} // namespace zlink::framework
```

Actor send and request only take a global `actor_id_t` as target. There's
no overload that takes [MeshName](../../../../01-glossary.en.md#meshname),
ActorRef, [owner](../../../../01-glossary.en.md#owner) NodeRid, or the
current SpotId. The runtime only caches a positive Ready route and
doesn't keep a negative cache. A missing route is distinguished as
`not_found`, and an exact-ref generation mismatch as
`invalid_operation`.

## 3. Single-Use Manager Operation

```cpp
namespace zlink::framework {

struct actor_create_existing_t {
    actor_ref_t actor;
};

struct actor_create_created_t {
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_create_rejected_t {
    std::optional<message_t> reply;
};

using actor_create_result_t = std::variant<
  actor_create_existing_t,
  actor_create_created_t,
  actor_create_rejected_t>;

class actor_create_call_t {
public:
    actor_create_call_t(actor_create_call_t &&) noexcept;
    actor_create_call_t &operator=(actor_create_call_t &&) noexcept;
    actor_create_call_t(const actor_create_call_t &) = delete;
    actor_create_call_t &operator=(const actor_create_call_t &) = delete;

    actor_create_call_t &in_mesh(std::string mesh_name);
    actor_create_call_t &creation_request(message_t request);

    template <typename TCreation>
    actor_create_call_t &creation_request(TCreation request);

    actor_create_call_t &timeout(std::chrono::milliseconds timeout);
    task_t<actor_create_result_t> submit();
    task_t<actor_create_result_t> yield();
};

class actor_manager_t {
public:
    virtual ~actor_manager_t() = default;
    virtual actor_create_call_t create(
      actor_id_t actor_id, std::string stable_type) = 0;
    virtual actor_create_call_t get_or_create(
      actor_id_t actor_id, std::string stable_type) = 0;
    virtual task_t<std::optional<actor_ref_t>> find(actor_id_t actor_id) = 0;
    virtual task_t<std::optional<spot_ref_t>> find_spot(
      actor_id_t actor_id) = 0;
    virtual task_t<bool> destroy(actor_ref_t actor) = 0;
};

} // namespace zlink::framework
```

A call object sets each option at most once, and calls `submit()` only
once too. A duplicate option is `invalid_operation`, and a second
submit is also `invalid_operation`. When `in_mesh` is omitted, if there
is exactly one object-role Mesh, it's auto-selected; with zero it's
`not_configured`, and with more than one it's `invalid_operation`. An
unknown Mesh is `not_found`.

`Create` returns `already_exists` for an existing identity, and returns
`actor_create_created_t` or `actor_create_rejected_t` for a new
attempt. `GetOrCreate` returns a
[Ready](../../../../01-glossary.en.md#ready) Actor of the same stable
type as `actor_create_existing_t` without a callback. If it's Creating,
it waits for the authority change, and a CAS loser doesn't start a
separate factory or callback. A different operation receives
`actor_create_existing_t` after Ready, races a new reservation after
cleanup, and doesn't share the preceding application reply. Only a
resend with the same source Node RID/lifecycle generation/`OperationId`
reads the correlation-free `creation-operation-terminal-v1` envelope
and re-encodes the reply with the current correlation/reply route. The
terminal is kept for 5 minutes after the original deadline. A callback
exception isn't a rejected result — it's a typed creation failure. A
different type is `type_mismatch`.
[Deadline](../../../../01-glossary.en.md#deadline) applies across
resolve, reservation, factory, and Ready as a whole. `Find` only
returns a Ready ref and doesn't create one. `FindSpot` only returns the
Ready `spot_ref_t` of the current User Spot membership, and returns an
empty optional for Entry
[membership](../../../../01-glossary.en.md#membership) or a Missing
Actor. `Destroy` only changes the exact ActorRef.
With no matching incarnation it's `false`, a different generation is
`invalid_operation`, and while moving it's `unavailable`.
A public Actor directory and local Actor bind overload aren't
provided.

Actor creation confirms the selected owner MeshNode's
[Entry Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot)
membership together inside the Ready barrier. Actor work payload is
delivered directly to the Actor queue regardless of membership kind,
without going through the Entry Spot callback. The original creation
payload and an ordinary message aren't hidden-retried to a different
owner or new incarnation. If the caller receives a timeout,
cancellation, or moving result, it must explicitly start a new
operation.

## 4. STREAM Exact-Ref Binding

```cpp
namespace zlink::framework {

class session_actor_t {
public:
    const actor_ref_t &ref() const noexcept;
    task_t<void> relay(const message_t &payload);
    task_t<void> relay(
      const session_message_context_t &context,
      const message_t &payload);
    task_t<void> notify_disconnected();
};

class session_actor_manager_t {
public:
    std::vector<session_actor_t> bound() const;
    std::optional<session_actor_t> find(actor_id_t actor_id) const;
    request_call_t<session_actor_t> bind(actor_ref_t actor_ref);
    request_call_t<session_actor_t> bind_or_get(actor_ref_t actor_ref);
};

} // namespace zlink::framework
```

Bind sends a control request only once, to the exact ActorRef location
the caller submitted. On a stale/moving result, it doesn't look up the
global ActorId again or auto-bind to a fresh incarnation. `find(...)`
only looks up an Actor already bound to that STREAM session — it isn't
the global Actor directory.

Once Actor relocation commits, `session_actor_t::ref()` returns a
current location snapshot with the same ActorId/ObjectGeneration and
the target MeshName/NodeRid, and the stored binding route is also
updated at the same point. A previous `actor_ref_t` value the caller
copied and kept doesn't change. The application doesn't call
`bind(...)` again to learn about relocation.

A one-way push through the current STREAM binding is a
connection-bound operation. If there's no valid binding or the
connection generation changed, it ends with a session-not-bound or
stale result, and the Framework doesn't find a different session and
resubmit. Connection close doesn't change the Actor's
[Spot](../../../../01-glossary.en.md#spot) membership or automatically
end the Actor.

## 5. Public Trace Category

The declarations in this document belong to public trace's
`actor-relocation` category. The common meaning is owned by
[Actor Model](../../../../14-actor-model.en.md),
[Spot · Actor Membership](../../../../15-spot-actor.en.md), and
[Session Actor Dispatch](../../../../20-session-actor-dispatch.en.md).

`yield()` and `yield_message()` declared in this document are only
valid while the current Actor handler is running in a `SpotWide` User
Spot's shared execution gate. If an Entry Spot Actor or a `PerActor`
User Spot's Actor calls them, they complete with `invalid_operation`
without submitting the operation or returning the turn. `submit()` can
be used in every Actor execution context.

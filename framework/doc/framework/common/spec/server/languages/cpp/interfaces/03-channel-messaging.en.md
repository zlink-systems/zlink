# C++ Channel Messaging Exact Interface

[C++ exact interface table of contents](README.en.md) · [MeshNode](../../../../13-mesh-node.en.md) ·
[Framework API](../../../../06-framework-api.en.md)

## 1. RouteMesh Registration

The RouteMesh builder registers one physical mesh and its MeshNode. A
logical channel is added to the same builder as membership, and
doesn't create a separate socket.
The RouteMesh status interface the application queries is defined by
the [C++ monitoring exact interface](08-monitoring.en.md).

```cpp
namespace zlink::framework {

struct mesh_peer_connection_t {
    std::uint64_t intent_id = 0;
    std::optional<zlink::routing_id_t> expected_routing_id;
    std::string endpoint;
};

class mesh_peer_connections_t {
public:
    void connect(std::string endpoint);
    void connect(zlink::routing_id_t expected_routing_id, std::string endpoint);
    void disconnect(std::string endpoint);
    std::vector<mesh_peer_connection_t> list_connections() const;
};

class mesh_channel_server_builder_t;
class mesh_channel_client_builder_t {};

class mesh_channel_builder_t {
public:
    mesh_channel_client_builder_t client();
    mesh_channel_server_builder_t server();
};

class mesh_channel_server_builder_t {
public:
    mesh_channel_server_builder_t &set_weight(int weight);
    mesh_channel_server_builder_t &add_handler_group(std::string group_name);

    template <typename THandler, typename TMessage>
    mesh_channel_server_builder_t &add_send_handler(std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    mesh_channel_server_builder_t &add_request_handler(std::string packet_name = {});
};

class network_options_t {
public:
    std::string bind_host() const;
    network_options_t &set_bind_host(std::string host);
    std::optional<std::string> advertise_host() const;
    network_options_t &set_advertise_host(std::optional<std::string> host);
};

class client_server_channel_client_builder_t {
public:
    client_server_channel_client_builder_t &connect(std::string endpoint);
};

class client_server_channel_server_builder_t {
public:
    client_server_channel_server_builder_t &listen(std::uint16_t port = 0);
    client_server_channel_server_builder_t &set_bind_host(std::string host);
    client_server_channel_server_builder_t &set_advertise_host(std::string host);
    client_server_channel_server_builder_t &set_weight(int weight);
    client_server_channel_server_builder_t &add_handler_group(std::string group_name);

    template <typename THandler, typename TMessage>
    client_server_channel_server_builder_t &add_send_handler(
      std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    client_server_channel_server_builder_t &add_request_handler(
      std::string packet_name = {});
};

struct mesh_node_socket_config_t {
    std::int64_t max_message_size = 16777216;
    zlink::byte_count_t send_high_water_mark =
        zlink::byte_count_t::bytes(4'096'000);
    zlink::byte_count_t receive_high_water_mark =
        zlink::byte_count_t::bytes(4'096'000);
    std::uint64_t mailbox_message_budget = 1024;
    std::uint64_t mailbox_byte_budget = 64 * 1024 * 1024;
    std::optional<std::chrono::milliseconds> receive_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
};

enum class object_role_t : std::uint8_t {
    none = 0,
    client = 1,
    server = 2
};

enum class user_spot_execution_mode_t {
    spot_wide = 0,
    per_actor = 1
};

enum class spot_relocation_readiness_mode_t {
    any_turn_boundary = 0,
    application_signaled = 1
};

class mesh_node_builder_t {
public:
    mesh_channel_builder_t channel(std::string channel_name);
    mesh_node_builder_t &listen(std::string endpoint);
    mesh_node_builder_t &listen(std::uint16_t port = 0);
    mesh_node_builder_t &set_bind_host(std::string host);
    mesh_node_builder_t &set_advertise_host(std::string host);
    mesh_node_builder_t &set_routing_id(zlink::routing_id_t routing_id);
    mesh_node_builder_t &set_automatic_routing_id_prefix(std::string prefix);
    mesh_node_builder_t &set_object_role(object_role_t role);
    mesh_node_builder_t &set_placement_weight(int weight);
    mesh_node_builder_t &set_actor_limit(std::int32_t limit);
    mesh_node_builder_t &set_spot_limit(std::int32_t limit);
    mesh_node_builder_t &set_activation_concurrency(std::int32_t limit);
    mesh_node_builder_t &set_instance_spot_idle_timeout(
      std::chrono::milliseconds timeout);
    mesh_node_socket_config_t &configure_router_socket();
    mesh_peer_connections_t &peer_connections();
    mesh_node_builder_t &set_default_request_timeout(std::chrono::milliseconds timeout);

    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_route_send_handler(std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_route_request_handler(std::string packet_name = {});

    template <typename TEntrySpot>
      requires std::derived_from<
        TEntrySpot, entry_spot_t<typename TEntrySpot::actor_type>>
    mesh_node_builder_t &add_entry_spot();

    template <typename TEntrySpot>
      requires std::derived_from<
        TEntrySpot, entry_spot_t<typename TEntrySpot::actor_type>>
    mesh_node_builder_t &add_entry_spot(
      std::function<std::shared_ptr<TEntrySpot>(
        class entry_spot_context_t)> factory);

    template <typename TSpot>
      requires std::derived_from<
        TSpot, spot_t<typename TSpot::actor_type>>
    mesh_node_builder_t &add_spot_factory(
      std::string stable_type,
      std::function<std::shared_ptr<TSpot>(
        class spot_context_t)> factory,
      std::function<void(user_spot_factory_builder_t<TSpot> &)> configure);

    template <typename TSpot>
      requires std::derived_from<TSpot, instance_spot_t>
    mesh_node_builder_t &add_instance_spot_factory(
      std::string stable_type,
      std::function<std::shared_ptr<TSpot>(
        class instance_spot_context_t)> factory,
      std::function<void(instance_spot_factory_builder_t<TSpot> &)> configure);

    template <typename TActor, typename TActorFactory>
      requires std::derived_from<TActor, actor_t> &&
        std::derived_from<TActorFactory, actor_factory_t<TActor>>
    mesh_node_builder_t &add_actor_factory(
      std::string stable_type,
      std::shared_ptr<TActorFactory> factory,
      std::function<void(actor_factory_builder_t<TActor> &)> configure);

};

enum class client_server_role_t { client, server, client_and_server };

enum class client_server_server_state_t {
    configured,
    connecting,
    ready,
    draining,
    disconnected,
    rejected
};

struct client_server_server_snapshot_t {
    zlink::routing_id_t server_rid;
    std::uint64_t lifecycle_generation;
    int weight;
    bool ready;
    client_server_server_state_t state;
    std::string descriptor_source;
    std::optional<std::string> last_failure;
};

struct client_server_channel_snapshot_t {
    std::string channel_name;
    client_server_role_t local_role;
    bool selectable;
    int ready_server_count;
    int connection_intent_count;
    int pending_request_count;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
    std::vector<client_server_server_snapshot_t> servers;
    location_runtime_snapshot_t location;
};

struct client_server_runtime_event_t {
    std::string identifier;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    std::optional<zlink::routing_id_t> server_rid;
    std::optional<std::uint64_t> lifecycle_generation;
    std::optional<int> weight;
    std::optional<bool> ready;
    std::optional<client_server_server_state_t> state;
    std::optional<std::string> reason;
};

class client_server_runtime_t {
public:
    virtual client_server_channel_snapshot_t snapshot(
      std::string channel_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t> observe(
      std::string channel_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<client_server_runtime_event_t> &)> observer) = 0;
    virtual bool is_ready(std::string channel_name) const = 0;
};

enum class fanout_publisher_connection_state_t {
    connecting,
    ready,
    disconnected,
    reconnecting,
    excluded_draining,
    excluded_stale
};

struct fanout_publisher_connection_snapshot_t {
    zlink::routing_id_t publisher_rid;
    std::uint64_t lifecycle_generation;
    bool connection_intent;
    bool ready;
    fanout_publisher_connection_state_t state;
    std::optional<std::string> last_failure;
};

struct fanout_channel_snapshot_t {
    std::string channel_name;
    std::size_t connection_intent_count;
    std::size_t ready_connection_count;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
    std::vector<fanout_publisher_connection_snapshot_t> publishers;
    location_runtime_snapshot_t location;
};

struct fanout_publisher_changed_event_t {
    static constexpr std::string_view event_identifier =
      "zlink.runtime.fanout.publisher_changed";
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    fanout_publisher_connection_snapshot_t entry;

    constexpr std::string_view identifier() const noexcept {
        return event_identifier;
    }
};

struct fanout_location_changed_event_t {
    static constexpr std::string_view event_identifier =
      "zlink.runtime.location.store_changed";
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    location_runtime_snapshot_t location;

    constexpr std::string_view identifier() const noexcept {
        return event_identifier;
    }
};

using fanout_runtime_event_t = std::variant<
  fanout_publisher_changed_event_t,
  fanout_location_changed_event_t>;

class fanout_runtime_observation_t {
public:
    virtual ~fanout_runtime_observation_t() = default;
    virtual void close() = 0;
};

class fanout_runtime_t {
public:
    virtual fanout_channel_snapshot_t snapshot(std::string channel_name) const = 0;
    virtual std::unique_ptr<fanout_runtime_observation_t> observe(
      std::string channel_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<fanout_runtime_event_t> &)> observer) = 0;
};

class mesh_channel_runtime_options_t {
public:
    virtual ~mesh_channel_runtime_options_t() = default;
    virtual int weight() const = 0;
    virtual void weight(int value) = 0;
};

class route_mesh_runtime_options_t {
public:
    virtual ~route_mesh_runtime_options_t() = default;
    virtual int placement_weight() const = 0;
    virtual void placement_weight(int value) = 0;
    virtual mesh_channel_runtime_options_t &channel(std::string channel_name) = 0;
};

} // namespace zlink::framework
```

`mailbox_message_budget` and `mailbox_byte_budget` bound the message
count and byte sum the per-owner application mailbox can hold. Byte
accounting doesn't count only payload size — it adds
`payload size + metadata size + a fixed per-job cost`. Even with an
empty payload, one job isn't `0` bytes, and even for a large payload,
the fixed cost is still added. If the sum exceeds `std::uint64_t`'s
representable range, it's pinned to the maximum value and that submit
is rejected. The accounting rule is owned by
[Framework API §8.2](../../../../06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime).
Both values are set only before startup. `0` isn't unlimited — it
selects the finite default the Framework profile decides. A Logical
Multicast local target also judges admission using this capacity
limit.

After `channel(channel_name)`, `client()` or `server()` is called
exactly once. Only the builder `server()` returns sets weight and
handler. A [MeshNode](../../../../01-glossary.en.md#meshname) with no
Server [membership](../../../../01-glossary.en.md#membership) can also
start. `add_client_server_channel(channel_name)` puts one-way request
start authority only on the client, and the server only performs
receiving-send/request processing and reply. The ClientServer builder
can call one or both of `client()` and `server()`, but each role is
registered at most once. The registration key is `(ChannelName, Role)`,
and duplicate registration of the same role is a startup error. A
different role shares the same ChannelName's topology as a separate
registration. The [RouteMesh](../../../../01-glossary.en.md#routemesh)
single-role-selection and
[ChannelName](../../../../01-glossary.en.md#channelname) conflict rule
don't change.

A peer connection isn't needed only when both MeshNodes are Object
Client and neither has RouteMesh Channel Server membership. The same
applies when only Channel Client membership is registered. If either
side has Channel Server membership including weight `0`, the connection
is made and liveness is kept. ClientServer and classic fanout are
separate physical topologies, so they aren't included in this
judgment. A RouteMesh Channel Server can also be registered on an
Object Client, but an application Node direct handler can't be
registered. Specifying an Object Client RID as a Node direct target
ends as `not_found` without switching to a different RID.

RouteMesh Channel Server, ClientServer Server, and node-wide placement
weight are all `int`, ranging `0..10000`, defaulting to `100`. A value
outside the range is a configuration error in both startup config and
runtime change. Weighted selection computes the sum of candidate
weight using at least a 64-bit integer.

Root BindHost's default is `127.0.0.1`. If AdvertiseHost is omitted, a
non-wildcard [BindHost](../../../../01-glossary.en.md#bindhost) is
used, and for a wildcard BindHost,
[AdvertiseHost](../../../../01-glossary.en.md#advertisehost) must be
specified. If the automatic discovery listener's port is omitted, or
the listener call itself is omitted, port `0` is used.
A per-listener host setting takes priority over the root default.

The automatic RID prefix is `[A-Za-z0-9._-]` 1..64 characters. The
runtime builds an RID in the format
`prefix-<lowercase-canonical-uuid-v4>`, and limits the whole RID to 255
bytes or fewer. UUID v4 is expressed as a lowercase canonical string of
`8-4-4-4-12` digits. If the active descriptor
[owner](../../../../01-glossary.en.md#owner) CAS conflicts, it doesn't
retry with a new UUID — it immediately fails startup with
`routing_id_conflict`. A fixed RID is allowed only in explicit manual
topology with Object role `none`.

Object role `server` includes `client` capability. `client` and
`server` require a Location Store, and `none` doesn't create a
manager, factory, or hidden local object runtime. Placement
[weight](../../../../01-glossary.en.md#weight) is `0..10000`, defaulting
to 100, and 0 excludes it only from a new create/relocation target. A
value outside the range is a configuration error in both startup
config and runtime change. Node Actor limit and Node Spot limit
default to `0`, meaning no limit.
`set_actor_limit(...)` counts every Actor present in the Entry Spot and
User Spot, and `set_spot_limit(...)` counts User/Instance Spot but
excludes Entry Spot. Both node limits allow `0..2147483647`, and `0`
means no limit. If a User/Instance Spot factory's stable type limit is
specified, the range is `1..2147483647`. If omitted, no separate limit
is put on that stable type.
A per-Actor-stable-type limit isn't provided.

`set_activation_concurrency(...)` sets the process-local concurrent
execution gate for factory and initialization, defaulting to `128` and
allowing only positive values. `0` or negative is a configuration
error before socket bind. Population capacity and activation
concurrency aren't merged into the same counter or option. Every value
is fixed before the MeshNode lifecycle starts.

`set_instance_spot_idle_timeout(...)` is the reference time for
cleaning up an idle
[Instance Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot).
The default is `std::chrono::milliseconds::zero()`, and `0` means no
cleanup. The allowed range is `0` and positive values, and negative is
a configuration error before socket bind. The value is fixed before
the MeshNode lifecycle starts, and a runtime setter isn't provided.
It's a separate setting from the Worker's `idle_timeout(...)`, and
they don't inherit each other's value. Only Instance Spot is a cleanup
target — Entry Spot and User Spot aren't affected by this setting. The
idle judgment condition, the delivery of
`spot_close_reason_t::idle_evicted`, and the cold activation rule after
cleanup are owned by
[Spot Model §6.2](../../../../11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot).

Descriptor capacity is only used for a candidate filter. The Framework
only runs the factory after atomically obtaining the Location Store's
typed bundle reservation at the selected node. An Actor reserves one
Actor slot, and a Spot reserves one whole Spot slot plus that stable
type's slot. A `SpotWide` User Spot's and `N` member Actors' aggregate
relocation reserves 1 Spot total, 1 of that Spot's stable type, and `N`
Actor totals all-or-none. If every candidate's reservation fails due to
capacity, it completes with `capacity_exceeded` without calling an
application factory or handler.
An Actor/User Spot/Instance Spot
[factory](../../../../01-glossary.en.md#factory) always specifies a
relocation policy, and there's no overload that omits it. A
state-preserving Actor factory needs
`actor_relocation_adapter_t<TActor>`, and a state-preserving
User/[Instance Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot)
factory needs `spot_relocation_adapter_t<TSpot>`. If the factory kind
and adapter kind or instance type don't match, it fails as a
configuration error before socket bind.

`user_spot_execution_mode_t::per_actor` only allows the recreate Spot
policy. Registering Disabled or a state-preserving Spot policy together
is a configuration error before socket bind. A PerActor Spot is a
stateless execution shell, and the Actor policy and
`actor_relocation_adapter_t<TActor>` each handle Actor state.

`relocation_readiness` defaults to `any_turn_boundary`.
`application_signaled` is only allowed with `spot_wide`, and
registering it together with `per_actor` is a configuration error
before socket bind. Since the Spot callback is a default no-op virtual
member, an application override isn't required.

The factory and Entry Spot member are only valid with Object role
`server`. Even though a single C++ builder exposes this member
together, a combination registering a factory with `none` or `client`
role fails as a configuration error before socket bind.

The Entry Spot ID is issued by the Framework at startup. A public
member for the caller to pass a Spot ID or configure a per-Entry-Spot
option isn't provided. Only after Entry Spot factory registration and
initialization complete does the Framework publish the RID to the
[descriptor](../../../../01-glossary.en.md#descriptor) and resolver.

The RID format is `<prefix>-entry-<lowercase-canonical-uuid-v4>`, using
a UUID v4 generated separately from the MeshNode. The Framework's
internal MeshNode descriptor's `entry_spot_id` provides the exact
mapping of the lifecycle. If the global Spot ID conflicts with an
active owner, it doesn't retry with a new UUID — it immediately fails
startup with a configuration exception. If a caller-specified
User/Instance Spot ID matches this reserved format, it's rejected with
`invalid_operation` before starting the Store and factory.

The fully encoded MeshNode descriptor the Framework builds from every
registration must be at most 1 MiB. Spot type and stateful object
capability collection are each at most 1024. The runtime validates the
completed descriptor all at once before socket bind. Exceeding the
bound fails startup — it doesn't truncate/split the collection or
publish part of the descriptor.

Topology registration only uses `zlink_framework_options_t`'s three
entry points: `add_route_mesh(...)`, `add_client_server_channel(...)`,
and `add_fanout_channel(...)`. RouteMesh picks its role with
`add_route_mesh(...).channel(...).client()` or `.server()`. A generic
channel builder or node builder that rebuilds the same topology isn't
kept. The Framework internals combine the binding types below.

- `zlink::context_t`
- `zlink::router_socket_t`
- `zlink::dealer_socket_t`
- `zlink::pub_socket_t`
- `zlink::sub_socket_t`
- MeshNode runtime handler
- `zlink::stream_socket_t`

## 2. ClientServer · Fanout Builder

ClientServer and Fanout use ChannelName but compose different physical
topologies. A ClientServer client starts send/request and the server
performs handler/reply. Fanout composes a publisher/subscriber role.

```cpp
namespace zlink::framework {

class endpoint_connections_t;

class fanout_channel_builder_t {
public:
    fanout_channel_builder_t &enable_publisher(std::string endpoint);
    fanout_channel_builder_t &enable_publisher(std::uint16_t port = 0);
    fanout_channel_builder_t &set_bind_host(std::string host);
    fanout_channel_builder_t &set_advertise_host(std::string host);
    fanout_channel_builder_t &set_routing_id(
      zlink::routing_id_t publisher_routing_id);
    fanout_channel_builder_t &set_automatic_routing_id_prefix(
      std::string prefix);
    fanout_channel_builder_t &enable_subscriber();
    fanout_channel_builder_t &connect(std::string endpoint);
    endpoint_connections_t subscriber_connections();
    fanout_channel_builder_t &add_handler_group(std::string group_name);
};

class client_server_channel_builder_t {
public:
    client_server_channel_client_builder_t client();
    client_server_channel_server_builder_t server();
};

} // namespace zlink::framework
```

Request timeout is set on the call object's `.timeout(...)` and the
route request fluent surface. The pending queue bound is owned by
`zlink_framework_options_t::set_max_pending(...)` per runtime unit.
The C++ public contract doesn't create a per-role timeout/pending
option absent from `.NET`'s role builder.

The internal mapping is below.

| Capability | Binding implementation basis |
|------------|------------------|
| server | `zlink::router_socket_t` |
| client | `zlink::dealer_socket_t` |
| publisher | `zlink::pub_socket_t` |
| subscriber | `zlink::sub_socket_t` |

Since ClientServer and Fanout are different physical topologies, they
can't share a ChannelName in the same process. Each topology's
connection set and descriptor are kept separate.

Automatic RouteMesh compares RID in canonical byte order, and only the
MeshNode with the smaller RID connects to the counterpart endpoint. A
manual topology can connect from one or both sides depending on
application endpoint configuration. If bidirectional connection or
[automatic discovery](../../../../01-glossary.en.md#automatic-discovery)
contention/a stale snapshot creates a duplicate candidate, handshake
and admission check the same RID and lifecycle generation and keep
only one in ready state.

A ClientServer client can use manual endpoint and
[location store](../../../../01-glossary.en.md#location-store)
automatic discovery together. If the two sources point to the same
Server RID and
[lifecycle generation](../../../../01-glossary.en.md#lifecycle-generation),
the connection intent and ready target are merged into one. In both
automatic and manual, only the client connects to server — the server
doesn't look for a client endpoint or start an outbound connect.

If both Client and Server are registered on the same process, a local
Server that finished listener and service admission is also included
in the same candidate set as a remote Server. The same
[Ready](../../../../01-glossary.en.md#ready), positive weight, and
non-draining conditions apply — there's no local priority or remote
exclusion rule. Even when a local Server is selected, the actual
transport message is delivered from the client DEALER to the server
ROUTER. A direct handler call doesn't bypass codec, HWM, timeout,
cancellation, correlation, or terminal completion.
`client_and_server` is used only for the aggregate projection of the
channel snapshot, and isn't selected in the builder or used as a
registration key.

A fanout publisher that registered a location store selects either a
fixed Publisher RID or an automatic RID prefix before startup, and
publishes a dedicated descriptor. A publisher without a Store can be
used as a target the listener endpoint is passed to manually, but
doesn't perform automatic discovery registration. `enable_subscriber()`
with no argument looks up every valid publisher descriptor of the same
ChannelName from the location store and connects to all of them.
`connect(endpoint)` composes a manual subscriber using only the
specified endpoint. Setting both subscriber modes together on one
channel fails startup. An automatic subscriber needs a location store,
and a host that only uses manual publisher and manual subscriber
doesn't need a store if it has no other location capability. A
publisher only publishes a descriptor and doesn't start an outbound
connect to a subscriber endpoint. Only a subscriber connects to a
publisher endpoint, and an automatic subscriber makes one connection
intent per Publisher RID and lifecycle generation.
`subscriber_connections()` is a runtime handle pointing to the same
[manual endpoint](../../../../01-glossary.en.md#manual-endpoint) set as
the builder's `connect(endpoint)`. This handle provides endpoint
connect, disconnect, and current listing, and doesn't change automatic
discovery results.

An automatic subscriber registered with no endpoint reads
`fanout_channel_snapshot_t` per ChannelName from `fanout_runtime_t` and
observes `fanout_runtime_event_t`.
`fanout_publisher_changed_event_t::entry` only provides the Publisher
RID and public connection status. Descriptor revision and endpoint are
only used Framework-internally to judge identity and stale state.
`fanout_location_changed_event_t::location` delivers store
degraded/recovered status even with 0 publishers. The two alternatives
of `std::variant` don't mix each other's payload as an optional field.
Each variant's `identifier()` returns `static constexpr
event_identifier`, so the caller can't change the identifier. `state`
and event identifier directly use the lowercase identifier from
[Runtime Monitoring](../../../../24-runtime-monitoring.en.md). This
runtime is read-only and doesn't change `subscriber_connections()`'s
manual endpoint set. Looking up a ChannelName registered only with a
manual subscriber is a configuration error.

The unit `client_server_runtime_t::observe(...)` and
`fanout_runtime_t::observe(...)` deliver is `observed_status_t<TStatus>`,
declared by the [Monitoring interface](08-monitoring.en.md). Since
ClientServer and fanout are topology sources with ChannelName as the
source key in
[Runtime Monitoring §3](../../../../24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes)'s
source table, both streams also wrap the event variant in the same
envelope and deliver a per-observer loss tally together. The only
difference is that the value in the `status` field is an event variant,
not a snapshot — `loss`'s meaning and the reset/saturation rule are
the same.

`fanout_runtime_observation_t` only owns the lifetime of a fanout
observer registration and `close()`. Since the
`fanout_runtime_t::observe(...)` callback only receives
`fanout_runtime_event_t`, it doesn't mix with a RouteMesh/ClientServer
event or a raw socket event.
`close()` stops putting a new event into that one observation's queue
and discards any not-yet-consumed event. An already-running callback
can still return, but a new callback isn't started after `close()`
returns. Closing its own observation inside a callback doesn't create
a deadlock. Close doesn't change a different observer, automatic
connection, or `subscriber_connections()`'s manual endpoint set.
`close()` guarantees the same result no matter how many times it's
called. A derived observation handle's destructor performs the same
deregistration as `close()`, so destroying a
`std::unique_ptr<fanout_runtime_observation_t>` without an explicit
`close()` leaves no observer registration behind.

`connection_intent=true` means the automatic planner requested an
endpoint connection — it isn't transport readiness. `ready=true`,
`ready_connection_count`, and the publisher changed event's `ready`
state all reflect the publisher-only SUB socket's native-ready and
that socket's first valid application record or liveness beacon
receipt. `disconnected` reflects a native disconnect or a 15-second
inbound timeout. The `connect` return, one native-ready, and the
internal active target count don't change this value first.

So connection configuration such as `listen`, `connect`, and
`enable_subscriber` is put on the ClientServer server, ClientServer
client, and Fanout builder, not a generic channel builder.

## 3. Handler Registry

The handler registry is a surface that handles a typed payload at the
function level.

```cpp
namespace zlink::framework {

enum class handler_execution_t {
    inline_on_runtime = 0,
    offload = 1
};

struct handler_options_t {
    std::optional<std::string> packet_name;
    handler_execution_t execution = handler_execution_t::inline_on_runtime;
};

class endpoint_connections_t {
public:
    void connect(std::string endpoint);
    void disconnect(std::string endpoint);
    std::vector<std::string> list_connections() const;
};

// the numeric value is a stable key for observation/diagnostic data, so it's fixed (framework API §13).
enum class framework_error_kind_t {
    not_found = 0,
    already_exists = 1,
    type_mismatch = 2,
    not_configured = 3,
    rejected = 4,
    unavailable = 5,
    capacity_exceeded = 6,
    deadline_exceeded = 7,
    shutting_down = 8,
    protocol_error = 9,
    invalid_operation = 10,
    data_lost = 11,
    internal_failure = 12
};

class framework_exception_t : public std::exception {
public:
    framework_error_kind_t kind() const noexcept;
    // provides a diagnostic error_code together if there's a platform cause.
    // the application's error branch uses kind().
    std::error_code code() const noexcept;
    const char *what() const noexcept override;
};

template <typename TReply>
class request_call_t {
public:
    request_call_t &timeout(std::chrono::milliseconds timeout);
    request_call_t &metadata(std::string key, std::string value);
    task_t<TReply> submit();
    task_t<TReply> yield();
};

class channel_request_call_t {
public:
    channel_request_call_t &timeout(std::chrono::milliseconds timeout);
    channel_request_call_t &metadata(std::string key, std::string value);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();
};

class send_call_t {
public:
    send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class bound_session_send_call_t {
public:
    bound_session_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class stream_send_call_t {
public:
    ~stream_send_call_t();
    stream_send_call_t(stream_send_call_t &&) noexcept;
    stream_send_call_t &operator=(stream_send_call_t &&) noexcept;
    stream_send_call_t(const stream_send_call_t &) = delete;
    stream_send_call_t &operator=(const stream_send_call_t &) = delete;

    stream_send_call_t &metadata(std::string key, std::string value);
    stream_send_call_t &packet_name(std::string packet_name);
    stream_send_call_t &compress();
    task_t<void> submit();
};

class stream_write_call_t {
public:
    using metadata_map_t = std::map<std::string, std::string>;

    ~stream_write_call_t();
    stream_write_call_t(stream_write_call_t &&) noexcept;
    stream_write_call_t &operator=(stream_write_call_t &&) noexcept;
    stream_write_call_t(const stream_write_call_t &) = delete;
    stream_write_call_t &operator=(const stream_write_call_t &) = delete;

    stream_write_call_t &metadata(std::string key, std::string value);
    stream_write_call_t &compress();
    task_t<void> submit();
};

template <typename TActor>
class bind_actor_call_t {
public:
    bind_actor_call_t &timeout(std::chrono::milliseconds timeout);
    task_t<TActor> async();
};

class message_metadata_t {
public:
    std::optional<std::string_view> find(std::string_view key) const;
    bool contains(std::string_view key) const;
    bool empty() const noexcept;
    const std::map<std::string, std::string> &values() const noexcept;
};

struct message_context_t {
    std::optional<std::string> mesh_name;
    std::optional<std::string> channel_name;
    std::string packet_name;
    std::optional<std::string> content_type;
    message_metadata_t metadata;
    std::optional<std::string> correlation_id;
};

struct publish_message_context_t : message_context_t {
    std::string topic;
    std::optional<std::string> source;
};

enum class handler_dispatch_kind_t {
    node_direct_send = 0,
    node_direct_request = 1,
    channel_send = 2,
    channel_request = 3,
    classic_fanout = 4
};

struct handler_filter_context_t : message_context_t {
    handler_dispatch_kind_t dispatch_kind;
};

using handler_next_t = std::function<task_t<void>()>;

} // namespace zlink::framework
```

`message_context_t` only provides inbound message information and
doesn't include cancellation state. Request reply wait cancellation is
handled by the call object, and STREAM connection close and
cancellation are handled by the session lifecycle.

The handler owner type is resolved from the service collection. An
ordinary application registers handler and service together inside
`add_zlink_framework(...)`.

```cpp
options.services().add_transient<order_handler_t>();

options.handlers()
  .group ("orders-api")
  .add<order_created_handler_t> ();
```

The STREAM application work path doesn't directly receive a header
object. The C++ stream session and actor relay use one `zlink::message_t`
payload, and the header value needed for reply and relay is kept by
runtime-internal dispatch state. A separate `_raw`-named public API
isn't kept.

Handler dispatch receives the binding's `zlink::message_t` and
`zlink::multipart_t`, converts them to a typed payload through the
serializer, resolves the owner from DI, and then calls the method.

A handler method can take only the payload, or the payload followed by
a typed context. A request and send handler receives
`message_context_t`, and an event/publish handler receives
`publish_message_context_t`. The channel context provides nullable
ChannelName, packet name, nullable content type, immutable metadata,
and correlation ID. The Node direct context only adds the source RID
to this common information. A raw multipart header or dispatch table
isn't exposed as a public context.

A handler filter, like `.NET`'s handler filter, handles common
processing before and after a handler call. An ordinary application
registers it with `options.use_filter<TFilter>()` in configuration. A
filter type provides
`task_t<void> invoke(const handler_filter_context_t &, handler_next_t)`.
To continue processing, it calls `co_await next()`. A filter doesn't
build or replace a request reply.

The application scope is Node direct send/request, RouteMesh/ClientServer
Channel send/request, and the Classic Fanout subscription handler. It
doesn't apply to a Spot/Actor handler, Logical Multicast subscription,
or a STREAM session.

A send that didn't call `next()` completes normally without running
the current handler. Classic Fanout only ends the current handler and
continues processing other matching handlers. A request completes as
`rejected` instead of a normal reply. `next()` can be called only
once, and a second call is an `invalid_operation` error.

A handler and filter are resolved in the same handler invocation scope
built per dispatch. Classic Fanout builds a separate scope per matching
handler, and one handler's filter interruption or failure doesn't
cancel another handler. Descriptor lookup, serializer selection, DI
resolve order, and how the filter chain is stored aren't exposed as
public API.

A STREAM handler is separate from an ordinary request/send/event
handler. The Framework runtime only supports the typed packet
approach, and doesn't put a user-defined header framing on the
Framework public surface.

The Framework-internal `recv loop` receives a STREAM raw part,
completes header framing and queue admission, and then runs the
stream callback. A core packet callback or raw receive callback
doesn't bypass queue admission. The packet/lifecycle callback of the
same stream session is processed serially in a managed queue. A stream
handler that's CPU-bound or might block specifies an offload execution
policy.

A request handler's return value allows `TReply` or `task_t<TReply>`.
A handler returning `task_t<TReply>` has the same meaning as `.NET`'s
`async Task<TReply>` handler, and a call that must wait for a result
like an internal request is used as `co_await call.submit()`. A
one-way send/push receives the bounded admission result up to the send
timeout with `co_await call.submit()`. If accepted immediately, the
prepared task can complete right away, without waiting for remote
handler completion.

A Handler coroutine completes as `task_t<T>` without a blocking wait.
The same task's terminal result is confirmed only once, and a
duplicate completion doesn't change the previously confirmed result.
Handler execution scheduler and continuation placement aren't exposed
in the public API.

## 4. Messaging API

The messaging surface user code injects and uses instead of a raw
socket is below.

```cpp
namespace zlink::framework {

struct send_options_t {
    std::optional<std::string> packet_name;
};

struct request_options_t {
    std::optional<std::string> packet_name;
    std::optional<std::chrono::milliseconds> timeout;
};

class publisher_t {
public:
    template <typename TEvent>
    fanout_publish_call_t publish(std::string channel_name,
      TEvent event);

    template <typename TEvent>
    fanout_publish_call_t publish(std::string channel_name,
      std::string topic, TEvent event);
};

class spot_publisher_client_t {
public:
    template <typename TEvent>
    publish_call_t publish(std::string channel_name, std::string topic,
                           const TEvent &event) const;
};

class request_client_t {
public:
    template <typename TCommand>
    send_call_t send(std::string_view channel_name, const TCommand &command,
      send_options_t options = {});

    template <typename TRequest>
    channel_request_call_t request(std::string_view channel_name,
      const TRequest &request,
      request_options_t options = {});
};

class message_bus_t {
public:
    ~message_bus_t();
    message_bus_t(message_bus_t &&) noexcept;
    message_bus_t &operator=(message_bus_t &&) noexcept;
    message_bus_t(const message_bus_t &) = default;
    message_bus_t &operator=(const message_bus_t &) = default;

    template <typename TRequest>
    channel_request_call_t request(
      std::string channel_name,
      TRequest request);

    template <typename TMessage>
    send_call_t send(std::string channel_name, TMessage message);

    template <typename TEvent>
    send_call_t publish(
      std::string channel_name,
      std::string topic,
      TEvent event);

    std::chrono::milliseconds default_request_timeout(
      const std::string &channel_name) const;
};

class spot_send_call_t;
class spot_request_call_t;

class route_client_t {
public:
    ~route_client_t();
    route_client_t(route_client_t &&) noexcept;
    route_client_t &operator=(route_client_t &&) noexcept;
    route_client_t(const route_client_t &) = default;
    route_client_t &operator=(const route_client_t &) = default;

    // node target — for the infra layer and owner-consistent routing.
    template <typename TMessage>
    route_send_call_t send_to_node(std::string mesh_name,
      zlink::routing_id_t target_node_rid,
      TMessage message);

    template <typename TRequest>
    channel_request_call_t request_to_node(std::string mesh_name,
      zlink::routing_id_t target_node_rid,
      TRequest request);

    template <typename TMessage>
    route_send_call_t send_to_channel(std::string channel_name,
      TMessage message);

    template <typename TRequest>
    channel_request_call_t request_to_channel(std::string channel_name,
      TRequest request);

    template <typename TMessage>
    spot_send_call_t send_to_spot(spot_id_t target, TMessage message);

    template <typename TRequest>
    spot_request_call_t request_to_spot(spot_id_t target, TRequest request);
};

class route_send_call_t {
public:
    route_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class spot_send_call_t {
public:
    spot_send_call_t &metadata(std::string key, std::string value);
    spot_send_call_t &instance_spot();
    spot_send_call_t &instance_spot(std::string stable_type);
    spot_send_call_t &in_mesh(std::string mesh_name);
    task_t<void> submit();
};

class spot_request_call_t {
public:
    spot_request_call_t &timeout(std::chrono::milliseconds timeout);
    spot_request_call_t &metadata(std::string key, std::string value);
    spot_request_call_t &instance_spot();
    spot_request_call_t &instance_spot(std::string stable_type);
    spot_request_call_t &in_mesh(std::string mesh_name);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();
};

class fanout_publish_call_t {
public:
    task_t<void> submit();
};

class publish_call_t {
public:
    publish_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

} // namespace zlink::framework
```

`send_to_spot(...)`'s and `request_to_spot(...)`'s target is always one
global `spot_id_t`. The fluent option expresses a Missing Instance
Spot's cold activation intent, and doesn't add
[MeshName](../../../../01-glossary.en.md#meshname), stable type, owner
RID, or generation to the address. A call with no instance marker set
is existing-only, and ends with `not_found` for a Missing RID.

`instance_spot()` omits the
[stable type](../../../../01-glossary.en.md#stable-type), and
`instance_spot(stable_type)` specifies the stable type. `in_mesh(...)`
applies only to the first placement of a Missing RID together with the
Instance marker. The marker and this option can each be set only once
per call, and a duplicate setting is `invalid_operation`. A terminal
operation among `submit()` or `yield<TReply>()` can also be started
only once, and a second call is `invalid_operation`.

The public API stays based on channel name and typed payload
regardless of transport kind. `publisher_t::publish(...)` provides
both a convenience call that uses the typed event's
[packet name](../../../../01-glossary.en.md#packet-name) as topic and
a call that specifies the
[topic](../../../../01-glossary.en.md#topic) explicitly. Both calls
are used for classic fanout, and the Framework decides the codec. If
the specified topic is the internal liveness exact byte `01 5A 4C 46 31`,
it doesn't start transport and raises `framework_exception_t`.
`fanout_publish_call_t::submit()` completes normally once the local
publisher transport accepts the event. It doesn't return subscriber
count or receive completion. `publish_call_t` is
[Logical Multicast](../../../../01-glossary.en.md#logical-multicast)-only.
It completes normally once the publisher local queue accepts the
event, even with 0 subscribers.

Every server one-way call's `submit()` and Session Actor `relay(...)`
don't produce a normal completion value. Normal completion means the
operation family's defined source-local queue accepted the message.
It doesn't wait for remote handler execution, subscriber receipt,
remote Spot queue acceptance, or application callback completion. If
queue capacity is short, it waits for a capacity signal up to that
family's send timeout, and submits the message exactly once if space
opens within the deadline. `backpressured` isn't a public terminal
result or an immediately-thrown application exception. Timeout
completes as `deadline_exceeded`, route disconnection as `unavailable`,
and runtime shutdown as a `framework_exception_t` of `shutting_down`
kind. Absence of Actor/Spot/Mesh/session target uses the existing
error kind the operation family defines. A C++ server call has no
separate cancellation argument. Not keeping or destroying the returned
task doesn't guarantee the operation is cancelled, and the same
operation isn't automatically resubmitted after timeout or shutdown.
An invalid argument/state and a duplicate submit complete as
`framework_exception_t`. A STREAM reply's valid first terminator
atomically claims and consumes a one-shot reply token before starting
transport. If two calls built from the same token race, the call that
fails the claim doesn't attempt transport and completes as
`framework_exception_t`. Even if the call that consumed the token ends
with `deadline_exceeded`, the token can't be used again, and an
already-used token is also treated as exceptional completion. A STREAM
reply isn't given the client request timeout — it only uses that
STREAM socket's send timeout.

RouteMesh node/Channel/[Spot](../../../../01-glossary.en.md#spot)/Actor
uses the selected MeshNode ROUTER's send timeout, ClientServer uses the
client DEALER's,
[classic fanout](../../../../01-glossary.en.md#classic-fanout) uses the
publisher socket's, and STREAM send/reply uses that STREAM socket's
send timeout. A bound session uses one framework socket send timeout
even if the local/remote Actor route changes. A one-way call doesn't
have a per-call `timeout(...)`. Without socket or MeshNode
configuration, a 1-second default is used instead of an infinite wait.
The socket/MeshNode `std::chrono::milliseconds` value used for one-way
admission only allows the `1..INT_MAX` range. `0`, negative, and
exceeding the bound are rejected as a configuration error at setting
time or, at latest, at startup, and aren't switched to the default.

Logical Multicast's `publish_call_t::submit()` does a direct handoff to
a bounded I/O executor. If a worker slot isn't obtained immediately, it
waits for capacity up to the send timeout. Once a slot is obtained, it
calls the raw binding publish exactly once. The point this call starts
is the operation commit barrier. After the transaction starts, an
individual target failure doesn't roll back an already-accepted target
or automatically retry the whole publish. Per-target
accept/failure results of remote transport and local Spot queue aren't
returned or aggregated in monitoring. It completes normally even with
0 target snapshots. Remote Spot queue submission and remote/local
handler execution or completion aren't `task_t<void>` completion
conditions.

The framework registers the following services by default. The user
can inject and use them from DI instead of constructing them directly.

- `message_bus_t`
- `publisher_t` (classic fanout client)
- `spot_publisher_client_t` (MeshNode Logical Multicast client)
- `request_client_t`
- `route_client_t`

## 5. Channel Surface

```cpp
struct route_message_context_t : message_context_t {
    zlink::routing_id_t source_node_rid;
};
class channel_client_t {
public:
    template <typename TRequest>
    channel_request_call_t request_to_channel(
      std::string channel_name,
      TRequest request);

    template <typename TMessage>
    send_call_t send_to_channel(
      std::string channel_name,
      TMessage message);
};
```

`add_entry_spot<TEntrySpot>()` with no argument uses the
`TEntrySpot(entry_spot_context_t)` constructor. An explicit factory
also receives the Context the Framework built first, by value, and
moves it into the application object. A User/Instance Spot factory
must also each receive the exact Context, and an overload that injects
or replaces the Context after creation isn't provided.

## 6. Handler

```cpp
enum class handler_kind_t;   // request / send / publish
```

`yield()` declared in this document's request and Spot request builder
is only valid while the caller owns a `SpotWide` User Spot's or
Instance Spot's shared turn. In any other execution context, it
completes with `invalid_operation` without submitting a message or
operation, or returning the turn. Actor Join is registered only through
`defer()` inside the current handler, and doesn't provide `async()` or
`yield()`.

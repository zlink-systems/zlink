# 02. Topology discovery

[Reference index](README.en.md)

This category covers the topology registration entry points `zlink_framework_options_t` provides,
and the entry points that query RouteMesh/ClientServer/Fanout operational status. The exact
signatures are owned by the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
and the
[Configuration and host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
(Korean-only). Every registration entry point is a call made at host configuration time.

---

## `add_route_mesh` (configuration time)

Registers one physical MeshNode. The starting point for RouteMesh-based topology.

```cpp
auto play = options.add_route_mesh("play")
  .listen(5501)
  .set_automatic_routing_id_prefix("play")
  .set_placement_weight(100);
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen(port)` | Does not bind if omitted | The receiving endpoint for this MeshNode |
| `.set_bind_host(string)` / `.set_advertise_host(string)` | Follows `configure_network()`'s root default | The bind/advertise host that applies only to this MeshNode |
| `.set_routing_id(routing_id)` / `.set_automatic_routing_id_prefix(prefix)` | Issued by the Framework | A fixed RID, or the prefix of an issued RID (1..64 characters of `[A-Za-z0-9._-]`) |
| `.set_object_role(object_role_t)` | `none` | One of `client`/`server`/`none`. `server` includes `client` capability, and both `client`/`server` require a Location Store |
| `.set_placement_weight(int)` | 100 (range `0..10000`) | The relative weight for placing new Actors/Spots on this node |
| `.set_actor_limit(int32_t)` / `.set_spot_limit(int32_t)` | `0` (unlimited) | The Actor/Spot capacity this node accepts |
| `.set_activation_concurrency(int32_t)` | 128 (positive) | The concurrent-execution cap for activation admission |
| `.set_default_request_timeout(milliseconds)` | This MeshNode's default request timeout | The value `request_to_node`/`request_to_channel` (messaging-execution category) uses when `.timeout(...)` is omitted |
| `.set_instance_spot_idle_timeout(milliseconds)` | `0` (never reclaims) | The Instance Spot idle reclaim time |
| `.configure_router_socket()` | `mesh_node_socket_config_t` default | This MeshNode's ROUTER socket HWM/buffer/timeout (`max_message_size`, `send_high_water_mark`, etc.) |
| `.channel(channel_name)` | — | Enters this MeshNode's RouteMesh Channel role registration. See the RouteMesh Channel registration entry |
| `.peer_connections()` | — | See the Manual peer connections entry |
| `.add_route_send_handler<THandler, TMessage>(packet_name = {})` | The packet name is determined from the message type | Registers a Node-direct one-way handler. The target `send_to_node` (messaging-execution category) calls |
| `.add_route_request_handler<THandler, TRequest, TReply>(packet_name = {})` | The packet name is determined from the message type | Registers a Node-direct request handler. The target `request_to_node` calls |

**Completion result.** Registers synchronously with no return value. An invalid combination (a
duplicate MeshName, a missing listener setting, etc.) surfaces as a configuration error in
`app.run(...)`'s validation before the socket bind.

**When to use.** Every host that uses RouteMesh registers at least one MeshNode. A node that only
uses manual peers and needs no distributed discovery can start without a Location Store.

---

## Object role registration (configuration time)

Registers how a MeshNode treats Actors/Spots (whether it only acts as a Client, or hosts them as a
Server).

```cpp
play.add_entry_spot<game_entry_spot_t>();

play.add_spot_factory<room_spot_t>(
  "room",
  [](zlink::framework::spot_context_t context) {
      return std::make_shared<room_spot_t>(std::move(context));
  },
  [](auto &factory) {
      factory.set_execution_mode(
        zlink::framework::user_spot_execution_mode_t::spot_wide);
      factory.template preserve_state_with<room_relocation_adapter_t>();
  });

play.add_actor_factory<player_actor_t, player_actor_factory_t>(
  "player",
  std::make_shared<player_actor_factory_t>(),
  [](auto &factory) {
      factory.template preserve_state_with<player_relocation_adapter_t>();
  });
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.add_entry_spot<TEntrySpot>()` / `.add_entry_spot<TEntrySpot>(factory)` | None | Registers an Entry Spot type dedicated to external entry. Omitting the factory uses the `TEntrySpot(entry_spot_context_t)` constructor |
| `.add_spot_factory<TSpot>(stable_type, factory, configure)` | None | Registers a stable User Spot type. `configure` receives exactly one of `preserve_state_with<TAdapter>`/`recreate_on_relocation`/`disable_relocation`, in addition to `set_stable_type_limit`/`set_execution_mode`/`set_relocation_readiness` |
| `.add_instance_spot_factory<TSpot>(stable_type, factory, configure)` | None | Registers a cold-activation Instance Spot type. `configure` receives exactly one relocation policy, in addition to `set_stable_type_limit` |
| `.add_actor_factory<TActor, TFactory>(stable_type, factory, configure)` | None | Registers a stable Actor type. `configure` receives exactly one relocation policy (an Actor factory has no `stable_type_limit`) |

**Completion result.** Registers synchronously with no return value. Adapter/factory mismatches
for a stable type intending to use relocation, and type duplicates, surface as a configuration
error in `app.run(...)`'s startup validation.

**When to use.** Register the corresponding role when this node actually hosts Actors/Spots
(`server`), or only references Actors/Spots another node hosts as a messaging target (`client`).
See the actor-relocation category for relocation-policy selection criteria.

---

## RouteMesh Channel registration (configuration time)

Registers logical ChannelName membership within the same MeshNode.

```cpp
play.channel("play.api").server()
  .set_weight(100)
  .add_handler_group("api");

play.channel("play.events").client();
```

**Options.** After `channel(channel_name)`, call `.client()` or `.server()` exactly once.
`.client()` only creates the send path and has no modifiers. Commonly used modifiers of
`.server()` are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.set_weight(int)` | 100 (range `0..10000`) | The relative weight for this Server to be selected as a request/send target. `0` excludes it from selection |
| `.add_handler_group(group_name)` | None | Links a handler group registered with `options.handlers().group(group_name)` |
| `.add_send_handler<THandler, TMessage>(packet_name = {})` | The packet name is determined from the message type | Registers a one-way handler directly on this channel |
| `.add_request_handler<THandler, TRequest, TReply>(packet_name = {})` | The packet name is determined from the message type | Registers a request/reply handler directly on this channel |

**Completion result.** Registers synchronously with no return value. If a handler with the same
packet name is exposed on the same channel twice, `app.run(...)` fails at startup configuration
before it receives any message.

**When to use.** Use `.server()` when registering a handler that `send_to_channel`/
`request_to_channel` (messaging-execution category) will receive. If this MeshNode only calls
another node's Server and places no handler of its own, register only `.client()`. Use
`add_client_server_channel` instead if communication must cross different processes.

---

## `add_client_server_channel` (configuration time)

Registers an independent ClientServer Channel unrelated to RouteMesh.

```cpp
options.add_client_server_channel("payments.api").server()
  .listen(6001)
  .set_weight(100)
  .add_handler_group("payments");

options.add_client_server_channel("payments.api").client()
  .connect("payments-1:6001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.server().listen(port = 0)` | `0` (automatic bind) | This Server's receiving port |
| `.server().set_bind_host(string)` / `.set_advertise_host(string)` | Root `configure_network()` default | The bind/advertise host that applies only to this Server |
| `.server().set_weight(int)` / `.add_send_handler`/`.add_request_handler` | Same as RouteMesh Channel Server | Weight and handler registration |
| `.client().connect(endpoint)` | manual | Connects to a specific Server manually. Omitting it finds the target via automatic discovery |

**Completion result.** Registers synchronously with no return value. A Client/Server using
automatic discovery without a Location Store registration surfaces as a configuration error in
startup validation.

**When to use.** Use this for request/reply or one-way messaging between independent services
that are not RouteMesh members. Between nodes in the same RouteMesh, use RouteMesh Channel
registration instead.

---

## `add_fanout_channel` (configuration time)

Registers a channel dedicated to classic fanout. It is the target `publisher_t::publish`
(messaging-execution category) publishes to.

```cpp
options.add_fanout_channel("lobby.events")
  .enable_publisher(7001)
  .add_handler_group("events");

// automatic subscriber — automatically discovers publishers of the same ChannelName from the
// location store.
options.add_fanout_channel("lobby.events")
  .enable_subscriber();

// manual subscriber — uses only the specified endpoints. Combining it with enable_subscriber()
// fails startup.
options.add_fanout_channel("lobby.events")
  .connect("lobby-1:7001");
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.enable_publisher(endpoint)` / `.enable_publisher(port = 0)` | None | Registers this channel's publisher role and receiving endpoint |
| `.set_bind_host(string)` / `.set_advertise_host(string)` / `.set_routing_id(routing_id)` / `.set_automatic_routing_id_prefix(prefix)` | Root default, or issued by the Framework | The bind/advertise host and RID that apply only to the publisher |
| `.enable_subscriber()` | — | automatic subscriber. Finds every valid publisher of the same ChannelName from the Location Store |
| `.connect(endpoint)` | — | manual subscriber. Uses only the specified endpoint |
| `.subscriber_connections()` | — | Returns a runtime handle (`connect`/`disconnect`/`list_connections`) over the set of manual subscriber endpoints |
| `.add_handler_group(group_name)` | None | Links a typed event handler group |

**Completion result.** Registers synchronously with no return value. Configuring both automatic
subscriber and manual subscriber on the same fanout channel surfaces as a startup failure.

**When to use.** Use this when creating a new observation/notification channel where the
publisher need not know its subscribers. If a reply is needed, use RouteMesh Channel or
ClientServer Channel registration instead.

---

## `add_stream_node` (configuration time)

Registers a listener that accepts external STREAM connections.

```cpp
options.add_stream_node("public-gateway")
  .bind(9001)
  .enable_actor_dispatch()
  .register_session<game_session_t>();
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind(port = 0)` | `0` (automatic bind) | This STREAM listener's receiving port |
| `.set_bind_host(string)` / `.set_advertise_host(string)` | Root `configure_network()` default | The bind/advertise host that applies only to this listener |
| `.set_tls_server(cert_path, key_path, require_client_certificate = false)` | No TLS | TLS server certificate/key, and whether to require mutual authentication |
| `.enable_actor_dispatch()` | Disabled | Dispatches an incoming message to a bound Actor via global ActorId lookup. Calling it twice on the same builder fails startup |
| `.register_session<TSession>()` | None | Registers a Session type that inherits `packet_stream_session_t`. A stream node declares exactly one packet session |
| `.register_session(name)` | — | An overload dedicated to low-level configuration that must name the Session explicitly |

**Completion result.** Registers synchronously with no return value. A TLS configuration error or
a duplicate `register_session` call surfaces as a configuration error in startup validation.

**When to use.** Use this to open a gateway that external clients connect to directly over the
STREAM protocol. See the stream-session category for the exact Session/Actor wiring rules.

---

## Manual peer connections (configuration time and runtime)

Connects to a specific endpoint manually, without automatic discovery. Called via
`mesh_node_builder_t::peer_connections()`.

```cpp
play.peer_connections().connect("play-node-2:5501");
std::vector<zlink::framework::mesh_peer_connection_t> connections =
  play.peer_connections().list_connections();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.connect(endpoint)` | No expected RID | The admission handshake determines the remote identity |
| `.connect(expected_routing_id, endpoint)` | — | Does not admit if the handshake identity differs |
| `.disconnect(endpoint)` | — | Releases a registered connection |
| `.list_connections()` | — | Queries the currently registered connection list |

**Completion result.** Registers/releases synchronously with no return value. If both MeshNodes
are Object Clients and neither has RouteMesh Channel Server membership, this connection intent
stays in the list but never becomes a ready peer. If either side has any Channel Server
membership, including one with weight `0`, ordinary peer admission/liveness rules apply.

**When to use.** Use this to configure RouteMesh with a fixed peer list, without automatic
discovery (a Location Store).

---

## `use_filter<TFilter>` (configuration time)

Inserts common logic (authentication, logging, etc.) in front of every handler dispatch.

```cpp
options.use_filter<authentication_filter_t>();

class authentication_filter_t {
public:
    zlink::framework::task_t<void> invoke(
      const zlink::framework::handler_filter_context_t &context,
      zlink::framework::handler_next_t next) {
        if (!is_authenticated(context)) {
            co_return; // not calling next() ends the request as rejected
        }
        co_await next();
    }
};
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.use_filter<TFilter>()` | None (runs in registration order) | Adds a filter type to the dispatch chain |

**Completion result.** Registers synchronously with no return value. Calling `next()` runs the
remaining filters and the handler. Not calling `next()` in a request ends it as `rejected`, and
calling `next()` twice fails with `invalid_operation`. `context.dispatch_kind` distinguishes
`node_direct_send`/`node_direct_request`/`channel_send`/`channel_request`/`classic_fanout` —
`channel_send`/`channel_request` include both RouteMesh and ClientServer.

**When to use.** Use this when common preprocessing/validation must repeat across individual
handlers. A filter does not construct the business reply itself — it only expresses rejection,
and the handler does the rest. Does not apply to Spot/Actor handlers or STREAM sessions.

---

## Other host-wide options (configuration time)

Configuration that ends with a single simple value, which `zlink_framework_options_t` provides.

```cpp
options.services().add_singleton<order_repository_t>();
options.configure_network().set_bind_host("0.0.0.0");
options.configure_inbound_dispatch()
  .set_application_hwm_profile(
    zlink::framework::application_hwm_profile_t::low_latency);
options.metadata()
  .allow_session_to_actor("trace-id")
  .allow_actor_to_session("server-region");
options.configure_stream_compression().use_lz4();
options.set_application_version(2);
options.handler_coroutine_workers(8);
```

**Options.** Commonly used entries are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.services()` | — | `service_collection_t`. Registers the service lifetime (`add_singleton`/`add_scoped`/`add_transient`/`add_factory`) of handlers and hosted components |
| `.metadata().allow_session_to_actor(key)` / `.allow_actor_to_session(key)` | Keys not specified are not forwarded | Adds a metadata key to forward across the STREAM session↔Actor relay to a direction-specific allowlist |
| `.configure_network()` | `bind_host()` is `127.0.0.1` | The default bind/advertise host used unless an individual listen call overrides it |
| `.worker()` | `worker_options_t` default | The bounded worker pool's minimum/maximum thread count, idle timeout, and queue cap (the pool `RunCpuWorker`/`RunIoWorker` use) |
| `.configure_inbound_dispatch()` | `application_hwm_profile_t::balanced` | The inbound application HWM size/profile, and the process memory cap |
| `.configure_dispatch()` | Framework default policy | Dispatch/diagnostics options. See the observability-diagnostics category |
| `.configure_stream_compression()` | No compression | The STREAM default compression codec (`use_default()`/`use_lz4()`/`use(codec)`/`disable()`) |
| `.set_max_pending(count)` | Framework default | The host-wide pending-queue cap |
| `.set_application_version(version)` / `.set_maintenance_wave(wave)` | `0` / none (no exclusion) | The deployment version and maintenance wave every local MeshNode publishes |
| `.set_default_request_timeout(timeout)` | Framework default | The host-wide default request timeout |
| `.handler_coroutine_workers(count)` | Framework default | The number of workers that run handler coroutines |
| `.codecs()` | Only JSON registered | `options.codecs().use(extension)`. See the Codec registration entry in messaging-execution category |

**Completion result.** Most execute synchronously with no return value; `.services()`/
`.configure_network()`/`.worker()`/`.configure_inbound_dispatch()`/`.configure_dispatch()` return
the corresponding builder or options object to continue further configuration on. Exceeding a
value's range surfaces as a configuration error in startup validation.

**When to use.** Use this to adjust host-wide settings that end with a single simple value and do
not belong to a dedicated category above (host lifecycle, topology registration, diagnostics).

---

## Runtime weight query/change

Changes placement weight or channel weight without redeploying.

```cpp
zlink::framework::route_mesh_runtime_options_t &placement =
  route_mesh_runtime_options; // instance injected from DI
placement.placement_weight(50); // lowers the share of new Actor/Spot placement routed to this node
placement.channel("play.api").weight(0); // excludes this Channel Server from selection
```

**Options.** This entry point has two independent properties.

| Property | Default | Meaning |
| --- | --- | --- |
| `route_mesh_runtime_options_t::placement_weight()`/`(value)` | The value at registration time | The node-level Actor/Spot placement weight |
| `route_mesh_runtime_options_t::channel(name).weight()`/`(value)` | The value at registration time | The ChannelName-level Server selection weight |

**Completion result.** A synchronous get/set. It applies immediately with no separate completion
signal. Querying an unregistered ChannelName is a configuration error.

**When to use.** Use this to adjust placement or traffic share while running. Transport options,
including `max_message_size`, cannot be changed through this path — configure them only before
startup.

---

## Topology status query/observation

Checks the operational status of each of RouteMesh/ClientServer/Fanout. The three runtimes provide
the same shape (one `snapshot` query, streaming observation with `observe`).

```cpp
zlink::framework::mesh_node_snapshot_t status = route_mesh_runtime.snapshot("play");
bool can_place_new_objects = status.is_ready && status.placement.is_available;

auto observation = route_mesh_runtime.observe(
  "play",
  /*capacity=*/64,
  [](const auto &observed) {
      // check observed.status.channels, observed.status.peers
  });
```

**Options.** The correspondence among the three runtimes is as follows.

| Runtime | Target | Returned snapshot |
| --- | --- | --- |
| `route_mesh_runtime_t` | MeshName | `mesh_node_snapshot_t` (includes channels, peers, placement) |
| `client_server_runtime_t` | ChannelName | `client_server_channel_snapshot_t` (includes servers) |
| `fanout_runtime_t` | ChannelName | `fanout_channel_snapshot_t` (includes publishers) |

**Completion result.** `snapshot(...)` is a synchronous call that returns a value immediately.
`observe(...)` delivers `observed_status_t<TStatus>` to the callback, and the `loss` field
(`coalesced_count`/`discarded_terminal_count`) tells you whether observations were lost. Ending
the observation calls `close()` on the returned observation handle (or destroys the
`unique_ptr`). Querying a fanout that only registered a manual ChannelName via
`fanout_runtime_t` is a configuration error.

**When to use.** Use this to judge a specific MeshName/ChannelName's availability, or to narrow
the scope of a failure. If host-wide status is needed, use `is_ready` (host-lifecycle category)
or `status()` (observability-diagnostics category).

---

See the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
and the
[Configuration and host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
(Korean-only) for the full rationale.

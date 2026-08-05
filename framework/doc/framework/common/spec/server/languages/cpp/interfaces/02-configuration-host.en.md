# C++ Configuration And Host Exact Interface

[C++ exact interface table of contents](README.en.md)

## 1. Host Maintenance

Host lifecycle operation splits into `relocate()` and `shutdown()`.
`relocate()` moves the current object to a node fitting the selected
operational purpose and keeps the host in `relocated` state. The
application checks the relocation result and calls `shutdown()` when
needed. If relocation isn't needed, only `shutdown()` can be called.

The application must always specify a purpose when calling
`relocate()`. `planned_maintenance` is used to move objects to a
different node of the same application version and then inspect or
restart the current host. `rolling_update` is used only to move
objects to an application-specified higher version.

The existing `retire()`, `drain()`, and `await_drained()` are removed
from the public interface. `stop()` and `request_stop()` start the
host's `shutdown()`. A component lifecycle operation taking MeshName
isn't provided.

```cpp
namespace zlink::framework {

enum class framework_runtime_state_t {
    preparing = 0,
    serving = 1,
    relocating = 2,
    relocated = 3,
    draining = 4,
    stopped = 5,
    error = 6
};

enum class relocation_outcome_t {
    relocated = 0,
    blocked = 1
};

enum class relocation_mode_t {
    planned_maintenance = 0,
    rolling_update = 1
};

enum class relocation_reason_t {
    none = 0,
    target_unavailable = 1,
    store_unavailable = 2,
    relocation_disabled = 3,
    state_incompatible = 4,
    deadline_exceeded = 5,
    relocation_failed = 6,
    runtime_not_ready = 7,
    manual_topology_unsupported = 8,
    shutdown_requested = 9,
    operation_in_progress = 10
};

struct relocation_options_t {
    relocation_mode_t mode;
    std::optional<std::int64_t> target_application_version;
    std::optional<std::chrono::milliseconds> deadline;
};

struct relocation_result_t {
    relocation_mode_t mode;
    std::int64_t effective_target_application_version;
    relocation_outcome_t outcome;
    relocation_reason_t reason;
};

enum class termination_outcome_t {
    stopped = 0,
    force_stopped = 1
};

enum class termination_reason_t {
    none = 0,
    deadline_exceeded = 1,
    teardown_failed = 2
};

struct termination_result_t {
    termination_outcome_t outcome;
    termination_reason_t reason;
};

} // namespace zlink::framework
```

`relocation_options_t::mode` is required. In `planned_maintenance`,
`target_application_version` isn't specified. The Framework only
selects the same application version as source, and records the
source version in the result's
`effective_target_application_version`. In `rolling_update`, a
`target_application_version` greater than source must be specified.
The Framework only selects an application version exactly matching
this value, and records the same value in the result too. Violating
this option combination has the Framework reject the call with
`std::invalid_argument` without changing the shared operation and host
state.

If `deadline` is empty, the default 30 seconds is used.
`wait_cancellation` only interrupts the waiter and doesn't cancel an
already-started shared operation. A concurrent call using the same
`relocation_options_t` joins the already-running shared operation and
receives the same terminal result. A concurrent call with a different
mode, target application version, or deadline doesn't join the
existing operation or change it — it returns
`blocked/operation_in_progress`. This result's mode and effective
target version reflect the valid option the rejected call requested.

`relocate()` returns `blocked` without changing admission and state if
the continuity preflight fails. On success it returns `relocated/none`,
and keeps the host process and infrastructure connection.
`shutdown()` doesn't return `blocked`.

Both modes narrow candidates in the following order.

1. `planned_maintenance` keeps only candidates whose version matches
   source. `rolling_update` keeps only candidates that exactly match the
   requested target version.
2. It excludes a candidate belonging to the same non-empty maintenance
   wave as source.
3. It applies a stable-type and relocation-policy/adapter compatibility
   check to the remaining candidates.
4. It checks the population capacity of the compatible candidates.
5. It applies node-wide placement weight on the final candidate set.

Since version and maintenance wave conditions are applied before
capability/capacity/weight, a rolling update doesn't fall back to a
same-version node, and planned maintenance doesn't select a
higher-version node. If there's no candidate after the first step, or
no target satisfying the later conditions, it returns
`blocked/target_unavailable`. Multiple relocation units all use the
same effective target version, but can each select a different
eligible node.

If there's even one local manual RouteMesh peer, ClientServer client
endpoint, fanout subscriber endpoint, or manual fanout publisher,
`relocate()` returns `blocked/manual_topology_unsupported`. Automatic
RouteMesh switches to `relocating` only after the same RID/lifecycle
generation as the descriptor becomes admitted/ready in source's Core
peer table. This restriction doesn't apply to `shutdown()`.

`blocked/deadline_exceeded` is the result of the deadline ending before
every target's `Prepared` completion and host `Relocating` descriptor
publication. It's also used, instead of `relocation_disabled`, when
connection-bound work and a bound-session request weren't terminal
drained within the pre-`Captured`
[deadline](../../../../01-glossary.en.md#deadline). The Framework
cleans up the relocation reference and reservation, releases the
reversible seal, and restores host state and admission. If every
target is `Prepared` and `Relocating` publication succeeds, it
completes every relocation unit and switches to `relocated`.

If `shutdown()` starts during `relocating`, the current atomic
relocation unit is confirmed as terminal and the remaining relocation
is aborted. The relocation waiter receives
`blocked/shutdown_requested`, and the host switches to `draining` and
continues shutdown. Calling `shutdown()` on an already-`relocated` host
only cleans up the remaining connection and resource. Calling it from
`serving` switches to `draining` without relocation.

The valid combinations of `relocation_result_t` and
`termination_result_t` are below. The caller judges every object's
move as finished only when the relocation result is `relocated/none`.

| Result | Reason |
|---|---|
| `relocated` | `none` |
| `blocked` | `target_unavailable`, `store_unavailable`, `relocation_disabled`, `state_incompatible`, `deadline_exceeded`, `relocation_failed`, `runtime_not_ready`, `manual_topology_unsupported`, `shutdown_requested`, `operation_in_progress` |
| `stopped` | `none` |
| `force_stopped` | `deadline_exceeded`, `teardown_failed` |

## 2. App / Host

`app_t` is the framework's outermost public type. The user builds an
app with `app_t::create()`, configures services, handlers, and zlink
runtime together in `add_zlink_framework(...)`, and then calls `run`.
A low-level runtime builder isn't exposed directly on the ordinary
application surface.

```cpp
namespace zlink::framework {

class app_t {
public:
    app_t();
    ~app_t();
    app_t(app_t &&) noexcept;
    app_t &operator=(app_t &&) noexcept;
    app_t(const app_t &) = delete;
    app_t &operator=(const app_t &) = delete;

    static app_t create();

    config_builder_t &config() noexcept;
    logging_builder_t &logging() noexcept;
    health_builder_t &health() noexcept;

    app_t &add_module(module_t &module);
    app_t &add_zlink_framework(
      std::function<void(zlink_framework_options_t &)> configure);
    template <typename TModule, typename... TArgs>
    app_t &add_zlink_framework(TArgs &&...args);
    app_t &add_hosted_service(std::unique_ptr<hosted_service_t> service);

    bool is_ready() const noexcept;
    app_t &set_message_flow_mode(message_flow_log_mode_t mode) noexcept;
    message_flow_log_mode_t message_flow_mode() const noexcept;

    task_t<relocation_result_t> relocate(
      relocation_options_t options,
      std::stop_token wait_cancellation = {});
    task_t<termination_result_t> shutdown(
      std::chrono::milliseconds deadline = std::chrono::seconds{30},
      std::stop_token wait_cancellation = {});

    int run(int argc, char **argv);
    void stop() noexcept;
    void request_stop() noexcept;
};

} // namespace zlink::framework
```

`run` returns `int`. The return value must be usable as the process
exit code. Handler exception, runtime error, and signal shutdown are
collected by the host, which closes the shutdown path.

## 3. Service Registration

Since the C++ binding has no DI concept, the Framework manages service
lifetime for the handler and hosted component. The application only
registers the service type and lifetime. The runtime's provider, scope,
and type-erased factory invocation aren't included in the public
interface.

```cpp
namespace zlink::framework {

enum class service_lifetime_t {
    singleton = 0,
    scoped = 1,
    transient = 2
};

class service_collection_t {
public:
    service_collection_t();
    ~service_collection_t();
    service_collection_t(service_collection_t &&) noexcept;
    service_collection_t &operator=(service_collection_t &&) noexcept;
    service_collection_t(const service_collection_t &) = delete;
    service_collection_t &operator=(const service_collection_t &) = delete;

    template <typename T>
    service_collection_t &add_singleton();

    template <typename T, typename... TDependencies>
    service_collection_t &add_singleton();

    template <typename T>
    service_collection_t &add_singleton(std::unique_ptr<T> instance);

    template <typename T>
    service_collection_t &add_scoped();

    template <typename T, typename... TDependencies>
    service_collection_t &add_scoped();

    template <typename T>
    service_collection_t &add_transient();

    template <typename T, typename... TDependencies>
    service_collection_t &add_transient();

    template <typename T, typename... TDependencies, typename TFactory>
    service_collection_t &add_factory(
      TFactory factory,
      service_lifetime_t lifetime = service_lifetime_t::transient);
};

} // namespace zlink::framework
```

The default construction rule is below.

- `add_singleton<T>()` and `add_transient<T>()` auto-construct only a
  default-constructible type.
- A type with a constructor dependency specifies the dependency type,
  like `add_singleton<T, Dep1, Dep2>()`, `add_scoped<T, Dep1, Dep2>()`,
  `add_transient<T, Dep1, Dep2>()`. The Framework resolves `Dep1`,
  `Dep2` from the internal provider and then calls `T(Dep1 &, Dep2 &)`.
- `add_scoped<T>()` only resolves inside a scope the framework owns.
- Only when complex external object construction or conditional
  construction is needed, use
  `add_factory<T, Dep1, Dep2>()`. The
  [Factory](../../../../01-glossary.en.md#factory) only receives a
  typed reference of the declared dependency, not a runtime provider or
  scope.
- A Channel/HTTP handler class, once registered to a handler group or
  HTTP route, is built by the Framework in the dispatch scope. The
  application doesn't register the same handler type to the service
  collection again.
- A Spot packet/Actor payload handler is a Spot member function, so
  it isn't a separate service. A timer handler class is built once in
  the Spot activation scope, and the same activation's ticks reuse it.
  Only the dependency declared in the timer handler's
  `dependency_types` is resolved in that scope.
- An external DI library, such as `Boost.Ext.DI`, isn't put as a public
  dependency.

`scoped` lifetime is a DI lifetime the Framework owns, not a raw
transport capability. The Framework internally builds the scope
boundary of handler dispatch, STREAM session, and Spot activation. A
channel handler builds a scope per dispatch,
[STREAM session](../../../../01-glossary.en.md#stream-session) has a
session scope, and [Spot](../../../../01-glossary.en.md#spot) and
Entry Spot have an activation scope. An actor factory is resolved in
the actor creation scope, and the actor instance itself is owned by
the actor runtime.

An example is below.

```cpp
options.services()
  .add_singleton<order_repository_t>()
  .add_transient<order_service_t, order_repository_t>()
  .add_transient<order_handler_t, order_service_t>();
```

## 4. Hosted Service And Module

A hosted service is a background worker tied to the app lifecycle.

```cpp
namespace zlink::framework {

class hosted_service_t {
public:
    virtual ~hosted_service_t() = default;
    virtual void start() = 0;
    virtual void request_stop() noexcept {}
    virtual void stop() noexcept = 0;
};

class module_t {
public:
    virtual ~module_t() = default;
    virtual void configure(zlink_framework_options_t &options) = 0;
};

template <typename TModule>
concept framework_module_contract_t =
  requires(TModule &module,
    zlink_framework_options_t &options) {
      module.configure(options);
  };

} // namespace zlink::framework
```

A module bundles service registration, runtime configuration, and
handler registration into one `zlink_framework_options_t`. It doesn't
expose runtime wiring order to the application by passing a separate
low-level builder and handler registry.

`app_t::add_zlink_framework(options_callback)` is the C++ high-level
configuration entry point corresponding to `.NET`'s
`AddZLinkFramework(options => ...)`. Since C++ has no assembly
reflection, it doesn't directly port only `.NET`'s
`AddHandlersFromAssemblyOf(...)`. Instead, it first picks a handler
group, then specifies the handler type inside that group and registers
it with `options.handlers().group(group_name).add<THandler>()`,
`add_send<THandler>()`, `add_publish<THandler>()`.
The remaining codec, discovery, RouteMesh membership, and handler group
configuration keep the same reading level as `.NET`.

Since JSON is the default codec, it isn't separately registered. The
user doesn't list every request/reply message type in a codec setting.
The C++ framework reads a handler's `request_type`, `reply_type` in
`options.handlers().group(...).add<THandler>()` and internally selects
the default JSON serializer. A send handler reads `message_type`, and
a publish handler reads `event_type`, registering serializer and
handler registry entries the same way. `options.codecs().use(...)`
isn't a step for listing an ordinary message type — it's an advanced
extension point that wires a payload that can't be expressed in
default JSON, or a separate binary serializer extension. So a
request/send/publish handler can be bundled under the same group name
and connected to a channel with the channel builder's
`.add_handler_group(...)`.
The handler group must match the channel kind. A
[RouteMesh](../../../../01-glossary.en.md#routemesh) ChannelName can
take a request/send handler group, and a fanout channel can only take
a publish handler group. Connecting a mismatched group fails as a
configuration error when writing options.
If a handler of the same packet name is exposed on the same channel
twice, it fails as a startup configuration error before the host
receives a message. A duplicate handler isn't treated as a runtime
request protocol error.
This rule applies to fluent options' handler group path. It doesn't
allow duplication whether a channel references a group first and a
handler comes in later, or a handler is registered first and a channel
references the group later.
A MeshNode can have a ROUTER listen endpoint and zero or more
[ChannelName](../../../../01-glossary.en.md#channelname)
[membership](../../../../01-glossary.en.md#membership)s. A
[MeshNode](../../../../01-glossary.en.md#meshname) dedicated to call or
Node direct can start with no membership, and a MeshNode providing a
Channel handler must register at least one Server membership. Each
Server ChannelName can have a request/send handler group. A fanout
subscriber must register at least one publish handler group.
If a Channel/HTTP handler has a constructor dependency, it specifies
the dependency type like
`using dependency_types = zlink::framework::dependency_list_t<dep1_t, dep2_t>;`.
The framework builds the dependency and handler in each dispatch
scope. A handler isn't registered as a singleton service.
`logger_t<THandler>` is a framework default dependency. If a handler
puts `logger_t<THandler>` in `dependency_types`, DI injects the
category logger like `.NET`'s `ILogger<T>` without the user writing a
separate service registration. The log output target is decided by
host logging configuration, such as `app.logging().use_console()`,
`app.logging().use_file(...)`, not handler registration. If a custom
category is needed, `logger_factory_t` can be received as a dependency
to build a category logger inside the handler.

```cpp
app.add_zlink_framework ([&](zlink::framework::zlink_framework_options_t &options) {
    options.use_filter<audit_filter_t>();
    options.metadata()
      .allow_session_to_actor("trace-id")
      .allow_actor_to_session("trace-id");

    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300)
      .set_routing_id(topology.application_rid);
    mesh.channel(sample_names_t::api_channel)
      .server()
      .add_handler_group("api");
    mesh.channel(sample_names_t::play_channel).client();

    // sets the message-flow observation level used at startup.
    options.configure_dispatch().message_flow(
      zlink::framework::message_flow_log_mode_t::errors_only);

    options.handlers()
      .group("api")
      .add<authenticate_player_handler_t>()
      .add<match_bingo_api_handler_t>()
      .add_send<player_command_handler_t>();

    options.handlers()
      .group("events")
      .add_publish<notification_event_handler_t>();
});
```

Using automatic peer discovery looks up the same
[MeshName](../../../../01-glossary.en.md#meshname)'s MeshNode
descriptor from a registered `location_store_t` provider. The official
Redis package is one of the usable providers.
A manual peer is registered with `peer_connections().connect(endpoint)`
or the overload that also takes an expected RID. A fanout subscriber's
endpoint list is separate from RouteMesh peer intent.

In this structure, the sample `main.cpp`, role `*HostFactory`, and an
ordinary user configuration example don't need to know implementation
details directly — a handler member function pointer, a DI factory
lambda for a handler, a monitoring channel string, serializer smoke
verification, or codec registration listing every message type. If
such content shows up, the framework options builder is considered not
deep enough.

`zlink_framework_options_t`'s user surface is limited to the fluent
options builder. An ordinary user configuration doesn't directly expose
a low-level channel runtime builder. The C++-internal runtime builder
may still have a low-level API, but sample- and guide-level
configuration uses a form whose role is immediately visible, like
below.

`options.configure_dispatch(...)` doesn't build an interface graph — it
passes a `dispatch_options_t` value to the lambda. This value holds
Spot and STREAM dispatch mode, unhandled request/send/publish policy,
and message flow diagnostics configuration. A native dispatch token,
queue slot, or handler lookup table doesn't appear on this surface.
The diagnostics sample rate must be between `0.0` and `1.0`, and NaN
isn't allowed.
Since send and publish have no reply path, `reply_error` can't be used
for their unhandled policy.

```cpp
options.add_location_store(redis_location_store);

auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
  .listen(7300)
  .set_routing_id(topology.application_rid);
mesh.channel(sample_names_t::api_channel)
  .server()
  .add_handler_group("api");

options.add_fanout_channel(sample_names_t::notification_channel)
  .enable_publisher(7400)
  .set_routing_id(topology.notification_publisher_rid)
  .enable_subscriber() // automatically discovers a publisher of the same ChannelName from the location store.
  .add_handler_group("events");

mesh.channel(sample_names_t::game_channel).client();
mesh.peer_connections().connect(topology.play_router_endpoint);
mesh.add_entry_spot<session_entry_spot_t>();

options.add_stream_node(sample_names_t::stream_name)
  .bind(7500)
  .enable_actor_dispatch()
  .register_session<client_session_t>()
```

The Entry Spot's SpotId is issued by the Framework in the format
`<prefix>-entry-<lowercase-canonical-uuid-v4>`. A public option for the
application to set the Entry Spot's RoutingId or a fixed SpotId isn't
provided.

`enable_actor_dispatch()` configures session Actor dispatch to use
global ActorId lookup and exact ActorRef bind. It doesn't take a
target MeshName or infer one from the first MeshNode. Don't call it on
a STREAM node that doesn't use Actor dispatch. Calling it twice on the
same builder fails startup.
`register_session<TSession>()` is a typed session registration surface
matching `.NET`'s `RegisterSession<TSession>()`. `TSession` must
inherit `packet_stream_session_t`, and is registered to the framework
service collection as a stream-session scope service. If
`TSession::session_name` exists, that value is used as the native
packet session name; otherwise a type-name-based message name is used.
`register_session(name)` is left for a low-level configuration that
must specify the session name directly.
One stream node declares only one packet session. Calling
`register_session<T>()` and `register_session(...)` more than once
isn't overwritten with the last value — it's treated as a
configuration error.

A MeshNode opens a ROUTER endpoint with `listen(...)` and selects a
role after `channel(...)`. An automatic peer is configured with the
Redis [descriptor](../../../../01-glossary.en.md#descriptor), and a
manual peer with `peer_connections()`. Node/Channel/Spot/Actor messages
use the same MeshNode ROUTER.
In fluent options, a value used as an identifier or connection address
— channel name, handler group name, endpoint, MeshName, stream node
name — doesn't allow an empty or whitespace-only string. An invalid
value is closed as a framework error at builder-call or options-apply
time, without being passed down to the low-level socket/runtime.
Spot code uses [Node direct](../../../../01-glossary.en.md#node-direct),
ChannelName select-one, and Logical Multicast as a client of the owner
MeshNode.
[Logical Multicast](../../../../01-glossary.en.md#logical-multicast)
doesn't compose a separate PUB/SUB role. Only classic fanout uses an
independent PUB/SUB socket.

### 4.1 HTTP Hosting

HTTP hosting is the C++ framework surface corresponding to ASP.NET
Core Minimal API's `MapGet`, `MapPost`, `MapPut`, `MapDelete`. MVC
controller, Razor page, template rendering, and WebSocket transport
aren't in scope. Instead, route handler, DI scope, JSON binding,
middleware/filter, logging, validation, error mapping, and zlink
channel calls are provided inside the same application host.

```cpp
namespace zlink::framework {

class http_options_builder_t {
public:
    http_options_builder_t &listen(std::string endpoint);
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);
    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);

    template <typename THandler>
    http_options_builder_t &map_get(std::string path);

    template <typename THandler>
    http_options_builder_t &map_post(std::string path);

    template <typename THandler>
    http_options_builder_t &map_put(std::string path);

    template <typename THandler>
    http_options_builder_t &map_delete(std::string path);

    template <typename TMiddleware>
    http_options_builder_t &use();

    http_options_builder_t &map_health(std::string path);
    http_options_builder_t &map_readiness(std::string path);
    http_options_builder_t &map_liveness(std::string path);
    const http_options_snapshot_t &snapshot() const noexcept;
    void validate() const;
};

struct http_context_t {
    http_method_t method;
    std::string path;
    std::string correlation_id;
    std::map<std::string, std::string> request_headers;
    std::map<std::string, std::string> response_headers;
    std::optional<std::string> response_body;
    int response_status;

    http_context_t &response_header(std::string name, std::string value);
    http_context_t &json_response(int status, std::string body);
};

struct http_request_t {
    http_method_t method;
    std::string path;
    std::string target;
    std::string query_string;
    std::string correlation_id;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> route_values;
    std::map<std::string, std::string> query_values;
    std::string body;
    std::string content_type;
    std::string remote_endpoint;
};

struct http_response_t {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    http_response_t &header(std::string name, std::string value);
};

class handler_options_builder_t {
public:
    class group_builder_t {
    public:
        template <typename THandler>
        group_builder_t &add();

        template <typename THandler>
        group_builder_t &add_send();

        template <typename THandler>
        group_builder_t &add_publish();
    };

    group_builder_t group(std::string group_name);
};

class metadata_policy_builder_t {
public:
    metadata_policy_builder_t &add_forwarded_metadata_key(std::string key);
    metadata_policy_builder_t &allow_session_to_actor(std::string key);
    metadata_policy_builder_t &allow_actor_to_session(std::string key);
};

class codec_options_builder_t {
public:
    template <typename TExtension>
    codec_options_builder_t &use(const TExtension &extension);
};

enum class application_hwm_profile_t {
    compact = 0,
    low_latency = 1,
    balanced = 2,
    throughput = 3
};

class inbound_dispatch_options_t {
public:
    inbound_dispatch_options_t &set_application_hwm_bytes(
      std::optional<std::uint64_t> value);
    inbound_dispatch_options_t &set_application_hwm_profile(
      application_hwm_profile_t value);
    inbound_dispatch_options_t &set_process_memory_limit_bytes(
      std::optional<std::uint64_t> value);
};

class zlink_framework_options_t {
public:
    handler_options_builder_t handlers();
    codec_options_builder_t codecs();
    metadata_policy_builder_t metadata();
    network_options_t &configure_network();
    worker_options_t &worker();
    dispatch_options_t &configure_dispatch();
    dispatch_options_t dispatch_options() const;
    location_options_t &configure_locations();
    inbound_dispatch_options_t &configure_inbound_dispatch();
    location_options_t location_options() const;
    zlink_framework_options_t &set_max_pending(std::size_t count);
    zlink_framework_options_t &set_application_version(
      std::int64_t application_version);
    zlink_framework_options_t &set_maintenance_wave(
      std::optional<std::string> maintenance_wave);
    zlink_framework_options_t &set_default_request_timeout(
      std::chrono::milliseconds timeout);
    service_collection_t &services() noexcept;
    zlink_framework_options_t &add_location_store(
      std::shared_ptr<location_store_t> store);
    client_server_channel_builder_t add_client_server_channel(
      std::string channel_name);
    fanout_channel_builder_t add_fanout_channel(std::string channel_name);
    mesh_node_builder_t add_route_mesh(std::string mesh_name);
    stream_node_options_builder_t add_stream_node(std::string stream_name);
    stream_compression_options_builder_t configure_stream_compression();
    http_options_builder_t &http() noexcept;
    template <typename TFilter>
    zlink_framework_options_t &use_filter();

    zlink_framework_options_t &handler_coroutine_workers(
      std::size_t worker_count);
    std::size_t handler_coroutine_workers() const noexcept;
    zlink_framework_options_t &add_relocation_store(
      std::shared_ptr<relocation_store_t> store);
};

} // namespace zlink::framework
```

An application using location runtime registers exactly one Location
Store with `add_location_store(...)`. If there's even one
`RecreateOnRelocation` or `PreserveStateWith` factory, or even one
Instance Spot factory, it also registers exactly one Relocation Store
with `add_relocation_store(...)`. A same-node configuration with no
[Instance Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot)
factory and only `DisableRelocation` factories doesn't need a
Relocation Store. If a needed Store is missing, or the same capability
is duplicate-registered, the Framework terminates with a configuration
error before socket bind.

`configure_network()` returns the process-wide BindHost and
AdvertiseHost default, and a per-listener setting overrides this value.
`worker()` returns the bounded worker pool's min/max thread count,
idle timeout, and queue bound. Both options can be changed only before
host start.

`configure_inbound_dispatch()` returns one host-wide setting. If HWM is
specified as `std::nullopt`, it's Auto; `0` means no limit; a positive
value uses an exact byte bound. The profile default is `balanced`.
Process memory limit only allows `std::nullopt` or a positive value.
In Auto mode with no explicit value, it checks a finite OS bound
applied to the process, such as a container/cgroup/Windows Job Object.
Since C++ has no managed heap bound, it uses the confirmed OS bound,
and if there's no OS bound either, it uses the total system physical
memory. If the computation result isn't positive, it fails as a
configuration error before socket bind.

Application version and maintenance wave are set once for the whole
host. Version is a non-negative signed 64-bit deployment ordinal
defaulting to 0, and every local MeshNode publishes the same value. An
empty optional wave means maintenance wave exclusion isn't used.

A usage example is below.

```cpp
app.add_zlink_framework([&](auto &options) {
    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300) // opens the endpoint this RouteMesh receives peer messages on.
      .set_routing_id(topology.application_rid); // identifies this node within the same mesh.
    mesh.channel(sample_names_t::api_channel)
      .server() // publishes this node as a request-processing candidate for api_channel.
      .add_handler_group("api"); // connects the DI handler group to register with the Channel handler.
    mesh.channel(sample_names_t::play_channel)
      .client(); // registers only the play_channel call path, with no Server membership.

    options.http()
      .listen(topology.api_http_endpoint) // opens the listener an HTTP client connects to.
      .map_post<create_game_http_handler_t>("/games"); // connects POST /games to the DI handler.
});
```

An HTTP handler uses the same type alias convention as a message
handler.

```cpp
class create_game_http_handler_t {
public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      dependency_list_t<request_client_t, logger_t<create_game_http_handler_t>>;

    explicit create_game_http_handler_t(
      request_client_t &client,
      logger_t<create_game_http_handler_t> &logger);

    task_t<create_game_http_res_t> handle(const create_game_http_req_t &request);
};
```

`map_get<THandler>(...)`, `map_post<THandler>(...)`,
`map_put<THandler>(...)`, `map_delete<THandler>(...)` register the
handler type to DI, register the JSON serializer for `request_type` and
`reply_type`, and connect `method + path` to the HTTP route table. It
builds a DI scope per request and resolves the handler. The DTO the
handler returns becomes the JSON response body, defaulting to
`200 OK`.

An HTTP handler supports all of the following shapes.

- typed DTO: `reply_type handle(const request_type &request)`
- typed DTO async: `task_t<reply_type> handle(const request_type &request)`
- typed DTO + context:
  `reply_type handle(const request_type &request, http_context_t &context)`
- typed DTO + context async:
  `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- typed DTO + request:
  `reply_type handle(const request_type &request, const http_request_t &http)`
- typed DTO + request async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- typed DTO + request + context:
  `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed DTO + request + context async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response: `http_response_t handle(const request_type &request)`
- typed response + context:
  `http_response_t handle(const request_type &request, http_context_t &context)`
- typed response async: `task_t<http_response_t> handle(const request_type &request)`
- typed response + context async:
  `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- typed response + request:
  `http_response_t handle(const request_type &request, const http_request_t &http)`
- typed response + request async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- typed response + request + context:
  `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response + request + context async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- raw HTTP request: `http_response_t handle(const http_request_t &request)`
- raw HTTP request async: `task_t<http_response_t> handle(const http_request_t &request)`

`http_request_t` and `http_response_t` are framework public types. A
raw HTTP handler also doesn't receive a `Boost.Beast` request, socket,
or SSL stream. `map_*<THandler>(...)` determines the handler shape at
compile time. For a typed route with multiple overloads, argument shape
is checked before return type. The shape receiving both
`http_request_t` and `http_context_t` is selected first, then
`http_request_t`, `http_context_t`, and DTO-only shape in that order.
Providing both a typed route and a raw route shape on one handler at
once must fail via static assertion or startup validation.

A route parameter and query string bind to the `request_type` DTO. For
example, a value from `/games/{gameId}/moves?actorId=p1` merges with
the body DTO to form the handler request. If the same field exists in
body, route, and query at once, route, query, body take priority in
that order. This priority follows the ASP.NET Core-style route handler
usability rule that an identifier shown in the URL is a more explicit
input than the request body.

`use<TMiddleware>()` wires cross-cutting processing, such as exception,
logging, validation, auth, correlation id, before and after the route
handler. A middleware/filter doesn't take a Beast or Asio type — it
only handles `http_context_t` and a framework DTO.
If a middleware provides `before(http_context_t&)` or
`after(http_context_t&)`, the runtime calls it before/after the route
handler. A request's `X-Correlation-Id` or `X-Request-Id` goes into
`http_context_t::correlation_id` and propagates as the response's
`X-Correlation-Id`. If a middleware sets `json_response(...)` in
`before(...)`, the runtime returns that JSON response without calling
the handler. `map_health(...)`, `map_readiness(...)`,
`map_liveness(...)` expose the `app.health()` report as an HTTP
endpoint.

`listen(...)` takes both `http://` and `https://` endpoints. Using an
`https://` endpoint requires setting the server certificate and private
key with `configure_tls(...)`. The TLS configuration public surface
only uses framework values, such as file path, PEM data, reload
policy, and doesn't expose an OpenSSL or Boost.Asio SSL type.

The HTTP runtime is tied to the app lifecycle as a `hosted_service_t`.
`Boost.Beast`, `Boost.Asio`, and OpenSSL/SSL context types only exist
in the runtime implementation and don't appear in a public header. An
HTTP error response maps to `400`, `404`, `405`, `500`, `503`, `504`
based on `framework_error_kind_t`.

Even when sending a request to a different channel inside a handler,
the caller shouldn't see a low-level request/reply template pair or a
blocking wait. C++ expresses the same reading level as `.NET`'s
`await client.RequestAsync<TReply>(...)` like below.

In a sample namespace, `using zlink::framework::task_t;` is put and
`task_t<T>` is written short. Repeating
`zlink::framework::task_t<T>` on every handler signature would show
namespace noise before async meaning. The framework public contract
document can use the full name, but the application sample and guide
example default to the short alias.

```cpp
task_t<match_bingo_api_res_t> handle(const match_bingo_api_req_t &request)
{
    allocate_bingo_room_res_t allocated = co_await _client
      .request(sample_names_t::play_channel,
               allocate_bingo_room_req_t { request.mode })
      .submit<allocate_bingo_room_res_t>();

    co_return match_bingo_api_res_t { allocated.room_id };
}
```

A sample handler doesn't pull the result directly with
`.submit().result().value()`. That code would make the handler look
like it's performing a blocking wait inside the runtime, which doesn't
fit the goal of providing the same async model across every language
version.

```cpp
class order_module_t final : public zlink::framework::module_t {
public:
    void configure(
      zlink::framework::zlink_framework_options_t &options) override
    {
        options.services().add_singleton<order_repository_t>();
        options.services()
          .add_factory<order_service_t, order_repository_t>(
            [](order_repository_t &repository) {
              return std::make_unique<order_service_t>(repository);
        });
        options.services().add_transient<order_handler_t>();
        options.handlers().group("orders").add_publish<order_handler_t>();
    }
};
```

## 5. Configuration And Logging

Configuration puts JSON, environment variables, and CLI args as the
default Framework surface.

```cpp
namespace zlink::framework {

enum class optional_t {
    no = 0,
    yes = 1
};

class configuration_model_t {
public:
    configuration_model_t &set(std::string key, std::string value);
    bool contains(std::string_view key) const;
    bool has_section(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
};

class configuration_section_t {
public:
    configuration_section_t(
      const configuration_model_t &model,
      std::string prefix);
    std::string key() const;
    bool contains(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
    std::string require(std::string_view key) const;
};

template <typename T>
concept configuration_bindable =
  requires(const configuration_section_t &section) {
      { T::bind(section) } -> std::same_as<T>;
  };

class config_builder_t {
public:
    configuration_model_t &model() noexcept;
    const configuration_model_t &model() const noexcept;
    config_builder_t &load_json(std::string path);
    config_builder_t &load_json(std::string path, optional_t optional);
    config_builder_t &load_env(std::string prefix);
    config_builder_t &load_cli(int argc, char **argv);
    config_builder_t &use_environment(std::string name);
    std::string environment() const;
    bool is_environment(std::string_view name) const;
    configuration_section_t section(std::string prefix) const;

    template <configuration_bindable T>
    std::optional<T> bind(std::string prefix) const;

    template <configuration_bindable T>
    T bind_required(std::string prefix) const;
};

enum class log_level_t {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
    off = 6
};
enum class logging_backend_t { builtin = 0, structured = 1 };
enum class logging_overflow_policy_t {
    drop_debug = 0,
    drop_oldest = 1,
    block = 2
};
struct log_field_t { std::string key; std::string value; };
struct log_record_t {
    log_level_t level = log_level_t::info;
    std::string category;
    std::string message;
    std::vector<log_field_t> fields;
    std::chrono::system_clock::time_point timestamp;
    std::thread::id thread_id;
};
struct logging_async_options_t {
    std::size_t queue_capacity = 8192;
    logging_overflow_policy_t overflow_policy =
      logging_overflow_policy_t::drop_debug;
};
struct rotating_file_options_t {
    std::size_t max_file_size = 10 * 1024 * 1024;
    std::size_t max_files = 5;
};

template <typename TCategory = void>
class logger_t {
public:
    logger_t() = default;
    bool is_enabled(log_level_t level) const noexcept;
    void log(log_level_t level,
      std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void log_with_fields(log_level_t level,
      std::string message,
      std::vector<log_field_t> fields) const;
    void trace(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void debug(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void info(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void warn(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void error(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void critical(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    const std::string &category() const noexcept;
};

class logger_factory_t {
public:
    logger_factory_t();
    logger_t<> create(std::string category) const;

    template <typename TCategory>
    logger_t<TCategory> create() const;
};

class logging_builder_t {
public:
    using sink_t = std::function<void(const log_record_t &)>;

    logging_builder_t &use_console();
    logging_builder_t &use_file(std::string path);
    logging_builder_t &use_rotating_file(
      std::string path,
      rotating_file_options_t options = {});
    logging_builder_t &use_callback_sink(sink_t sink);
    logging_builder_t &use_provider(std::string name, sink_t sink);
    logging_builder_t &use_async(logging_async_options_t options = {});
    logging_builder_t &use_backend(logging_backend_t backend);
    logging_builder_t &disable_record_capture();
    logging_builder_t &set_max_captured_records(std::size_t max);
    logging_builder_t &set_min_level(log_level_t level);
    logging_builder_t &set_level(std::string level);

    bool console_enabled() const noexcept;
    bool has_output_sink() const noexcept;
    bool async_enabled() const noexcept;
    logging_backend_t backend() const noexcept;
    log_level_t min_level() const noexcept;
    const std::string &level() const noexcept;
    const std::vector<std::string> &file_paths() const noexcept;
    const std::vector<std::string> &provider_names() const noexcept;
    const std::vector<log_record_t> &captured_records() const noexcept;
    logger_factory_t factory() const;
    logger_t<> create_logger(std::string category) const;
};

} // namespace zlink::framework
```

The JSON loader uses `nlohmann/json`. YAML is left as a configuration
extension if needed. The application configures the standard logging
provider and health surface. Runtime event DTO, metric payload
callback, exporter lifecycle, and provider-internal registry aren't a
public contract.

### 5.1 Instance Spot Metric

The following six instrument names, which the Framework records to
the standard metric provider for Instance activation, are used as-is
in byte form. Kind, unit, label, and the closed outcome value are
owned by
[Runtime Metrics §4](../../../../25-runtime-metrics.en.md#4-object-and-stream).

- `zlink.instance_spot.activations`
- `zlink.instance_spot.activation.duration`
- `zlink.instance_spot.pending.messages`
- `zlink.instance_spot.pending.bytes`
- `zlink.instance_spot.claim.conflicts`
- `zlink.instance_spot.takeovers`

A one-way activation failure doesn't make a separate instrument — it's
recorded in `zlink.mesh_node.messages.dropped` with
`surface=instance_spot`. The `instance_spot_type` label only uses the
bounded type name registered at startup, and doesn't use Spot ID, owner
ID, or an internal authority field as a label.

### 5.2 Message-Flow Dispatch Error Event

An unregistered message and dispatch failure observation is handled
through the message flow observer's `event_id=zlink.dispatch_error`,
`outcome=failed` event.
Per-channel or per-spot observer registration isn't a public contract
of this version. A request failure ends as an error reply if it has a
reply path, and a path with no reply frame, such as a local actor call,
completes the `task_t` or pending operation with a framework error. A
one-way failure is dropped but leaves a default log, counter, and
message-flow event.

The exact dispatch option declaration is owned by the
[Monitoring interface](08-monitoring.en.md).

`message_flow_event_t`'s dispatch error event is a snapshot carrying
`surface`, `message_kind`, `reason`, `action`, `packet_name`,
`channel_name`, `topic`, `spot_id`, `instance_spot_type`,
`activation_state`, `actor_id`, `source_rid`, `correlation_id`. It
doesn't include native message ownership or a frame reference.

The default mode before start is decided by
`configure_dispatch().message_flow(...)`. Only
`app_t::set_message_flow_mode(...)` owns the runtime mode change, and
a per-channel or per-Spot toggle isn't provided.

```cpp
app.add_zlink_framework([](auto &options) {
  options.configure_dispatch()
    .set_message_flow_observer(
      std::make_shared<my_message_flow_observer>());
});
```

## 6. HTTP Route And Middleware

**The HTTP hosting scenario is owned by
[60](../60-http-hosting.ko.md) · [61](../61-embedded-http-server.ko.md).**
Only the public type is fixed here.

```cpp
enum class http_method_t { get, post, put, delete_ };

class http_route_t
{
public:
    http_method_t method;
    std::string   path;
    std::string   handler_name;
    bool context_response_precedence = false;  // prioritizes the response the context built
    bool validates_json_content_type = true;   // validates the JSON content type
};

struct http_tls_options_t {
    std::string certificate_file;
    std::string private_key_file;
};
struct http_endpoint_t { std::string uri; std::optional<http_tls_options_t> tls; };
struct http_server_options_t {
    std::size_t max_connections = 1024;
    std::size_t max_request_body_size = 1024 * 1024;
    std::size_t max_header_size = 64 * 1024;
    std::chrono::milliseconds request_headers_timeout{5000};
    std::chrono::milliseconds request_body_timeout{5000};
    std::chrono::milliseconds write_timeout{5000};
    std::chrono::milliseconds keep_alive_timeout{5000};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    std::size_t max_keep_alive_requests = 100;
};
struct http_options_snapshot_t {
    std::vector<http_endpoint_t> endpoints;
    std::vector<http_route_t> routes;
    std::vector<std::string> middleware_names;
    http_server_options_t server;
    std::optional<std::string> health_path;
    std::optional<std::string> readiness_path;
    std::optional<std::string> liveness_path;
};

class http_tls_options_builder_t {
public:
    explicit http_tls_options_builder_t(http_tls_options_t &options) noexcept;
    http_tls_options_builder_t &certificate_file(std::string path);
    http_tls_options_builder_t &private_key_file(std::string path);
};

class http_server_options_builder_t {
public:
    explicit http_server_options_builder_t(
      http_server_options_t &options) noexcept;
    http_server_options_builder_t &set_max_connections(std::size_t value);
    http_server_options_builder_t &set_max_request_body_size(std::size_t bytes);
    http_server_options_builder_t &set_max_header_size(std::size_t bytes);
    http_server_options_builder_t &set_request_headers_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_request_body_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_write_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_keep_alive_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_graceful_shutdown_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_max_keep_alive_requests(
      std::size_t value);
};
```

- **Middleware is a `before`/`after` pair.** It isn't a `next` delegate
  approach — the shape is different from a
  [handler filter](../../../../06-framework-api.en.md).
- **A middleware instance is built with `create_instance`, receiving
  the DI provider together.**

## 7. Transport

```cpp
enum class transport_scheme_t {
    tcp = 0,
    ipc = 1,
    tls = 2,
    websocket = 3,
    websocket_tls = 4
};

class transport_endpoint_t
{
public:
    transport_endpoint_t (transport_scheme_t scheme, std::string uri);
    transport_scheme_t scheme() const noexcept;
    const std::string &uri() const noexcept;
    static transport_endpoint_t parse(std::string uri);
};
```

**An endpoint holds a scheme and URI together.** The meaning of the
scheme→transport mapping is owned by
[Stream Connector §3](../../../../stream-connector/32-stream-connector.en.md).

## 8. Registration Builder

**The registration surface is a builder hierarchy.** Each builder only
owns its own role's configuration.

The formal channel/MeshNode builder declaration is owned by
[Channel messaging](03-channel-messaging.en.md). The STREAM builder and
compression option are owned by
[STREAM session](06-stream-session.en.md), and the handler/codec/
metadata builder is owned by this document's configuration
registration surface.

- RouteMesh ChannelName and a classic fanout channel are different
  namespace and socket contracts.
- Spot/Actor registration is put on the owner `mesh_node_builder_t`.

The meaning of claim progress during drain is owned by
[Graceful Drain §5](../../../../28-graceful-drain-handoff.en.md).

## 9. Configuration Lookup

**A section is a view cut by prefix.** The rule for merging
configuration sources as a hierarchy is owned by
[01 §5](../01-system-structure.en.md).

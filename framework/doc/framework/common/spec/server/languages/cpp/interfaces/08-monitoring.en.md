# C++ Monitoring Exact Interface

[C++ exact interface table of contents](README.en.md)

Endpoint, lifecycle generation, and descriptor source are only used by
the Framework to judge stale registration information and connection.
Admission/claim/reservation, pending work, and connection intent also
aren't included in public status.

## 1. Host Lifecycle Observation

Host-unit status is separate from the RouteMesh/ClientServer/fanout
snapshot. `topology_state_t` represents one registered topology's
availability and isn't reused as host lifecycle status.

RouteMesh peer status uses
[Channel messaging](03-channel-messaging.en.md)'s `peer_state_t`.
`not_connected` is a state where connection is needed but there's no
ready connection. `not_required` is a normal state where connection
isn't needed because neither Object Client has RouteMesh Channel
Server membership. The same applies when only Channel Client
membership is registered. If either side has Channel Server membership
including weight `0`, absence of connection is `not_connected`. Both
are excluded from the ready-peer count, but `not_required` isn't
included in liveness/health failure aggregation.

```cpp
struct observation_loss_t {
    std::uint64_t coalesced_count = 0;
    std::uint64_t discarded_terminal_count = 0;
};

template <typename TStatus>
struct observed_status_t final {
    TStatus status;
    observation_loss_t loss;
};

struct inbound_dispatch_status_t {
    std::uint64_t application_hwm_bytes;
    std::uint64_t pending_payload_bytes;
    std::uint64_t queued_payload_bytes;
    std::uint64_t active_payload_bytes;
    bool application_receive_paused;
    std::uint64_t pending_completion_sends;
    std::uint64_t completion_send_limit;
};

struct framework_runtime_status_t {
    framework_runtime_state_t state;
    bool is_ready;
    bool accepting_work;
    std::optional<std::chrono::system_clock::time_point> operation_deadline;
    std::optional<relocation_result_t> relocation_result;
    std::optional<termination_result_t> termination_result;
    inbound_dispatch_status_t inbound_dispatch;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class framework_runtime_t {
public:
    virtual ~framework_runtime_t() = default;
    virtual framework_runtime_status_t status() const = 0;
    virtual std::unique_ptr<runtime_observation_t> observe(
      std::size_t capacity,
      std::function<void(
        const observed_status_t<framework_runtime_status_t> &)> observer) = 0;
};
```

`is_ready` is `true` only when
`state == framework_runtime_state_t::serving`. `accepting_work`
indicates whether the host accepts a new application operation.
Status only provides the value the application needs to judge a
lifecycle operation's result and readiness. It doesn't include
relocation unit count, queue, barrier, and Store-internal state.

### 1.1 RouteMesh Status

```cpp
enum class mesh_node_state_t {
    starting,
    ready,
    degraded,
    stopping,
    stopped,
    failed
};

enum class topology_reason_t {
    runtime_not_ready,
    no_ready_peer,
    no_ready_target,
    location_unavailable,
    capacity_exceeded,
    draining,
    internal_failure
};

enum class peer_state_t {
    connecting,
    ready,
    draining,
    not_connected,
    not_required
};

struct mesh_peer_snapshot_t {
    zlink::routing_id_t node_rid;
    peer_state_t state;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_channel_snapshot_t {
    std::string channel_name;
    bool is_ready;
    std::uint32_t ready_target_count;
};

struct mesh_placement_snapshot_t {
    bool is_available;
    std::uint32_t active_actor_count;
    std::uint32_t active_spot_count;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_node_snapshot_t {
    std::string mesh_name;
    mesh_node_state_t state;
    bool is_ready;
    std::uint32_t ready_peer_count;
    std::vector<mesh_channel_snapshot_t> channels;
    std::vector<mesh_peer_snapshot_t> peers;
    mesh_placement_snapshot_t placement;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class route_mesh_runtime_t {
public:
    virtual mesh_node_snapshot_t snapshot(std::string mesh_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t> observe(
      std::string mesh_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<mesh_node_snapshot_t> &)> observer) = 0;
    virtual bool is_ready(std::string mesh_name) const = 0;
};
```

The unit `observe(...)` delivers is `observed_status_t<TStatus>`.
`status` is the complete status/snapshot shared among observers, and
`loss` is the loss tally belonging only to this one observation. The
reason the tally isn't put in status, and the meaning of the two
counters, is owned by
[Runtime Monitoring §3](../../../../24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

`observation_loss_t::coalesced_count` is the count of intermediate
status this observer didn't see due to per-source latest-slot merging,
and `discarded_terminal_count` is the count of terminal status
discarded for exceeding the retention bound. The two aren't merged
into one. Both counters start at `0` on every `observe(...)` call,
increase monotonically within the same observation, and are pinned at
`9223372036854775807` (`2^63 - 1`) once exceeded. This bound is the
same across the four languages — matched to the maximum value Java's
`long` can express. The Framework doesn't end an observation just
because the observer queue is full.

`observe(...)` delivers a complete snapshot after the change, not a
general-purpose event combining nullable fields. Peer status only
provides the Node RID, current state, and the reason it's unavailable.
Placement only provides whether new objects can be accepted and the
current process's active Actor/Spot count. Per-stable-type capacity,
activation concurrency, and reservation failure are
Framework-internal placement judgment values.

`mesh_placement_snapshot_t::is_available` is `true` only when there's
headroom in both Actor or Spot capacity and activation concurrency.
Activation concurrency's current value and limit aren't exposed as a
separate snapshot field.

## 2. Message Flow Observation

```cpp
enum class message_flow_log_mode_t {
    off = 0,
    errors_only = 1,
    key_transitions = 2,
    verbose = 3,
    diagnostic = 4
};
enum class message_flow_outcome_t {
    received = 0,
    dispatched = 1,
    replied = 2,
    dropped = 3,
    sent = 4,
    reply_received = 5,
    error = 6
};
enum class dispatch_error_surface_t {
    channel = 0,
    route_mesh_channel = 1,
    spot_route = 2,
    spot_subscription = 3,
    spot_actor = 4,
    stream_session = 5
};
enum class dispatch_message_kind_t {
    request = 0,
    send = 1,
    publish = 2,
    response = 3,
    error = 4,
    actor_request = 5,
    actor_send = 6
};
enum class dispatch_error_reason_t {
    handler_missing = 0,
    payload_decode_failed = 1,
    handler_exception = 2,
    invalid_frame = 3,
    reply_path_missing = 4,
    unexpected_reply = 5
};
enum class dispatch_error_action_t {
    reply_error = 0,
    drop = 1,
    fail_caller = 2
};
enum class flow_origin_t : std::uint8_t
{ inbound = 1, timer = 2, application = 3, lifecycle = 4 };
struct message_dispatch_error_event_t {
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    dispatch_error_reason_t reason;
    dispatch_error_action_t action;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> correlation_id;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};
struct message_flow_event_t {
    message_flow_outcome_t outcome;
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> correlation_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::size_t> message_size;
    std::optional<dispatch_error_reason_t> error_reason;
    std::optional<dispatch_error_action_t> error_action;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};
class message_flow_observer_t {
public:
    virtual ~message_flow_observer_t() = default;
    virtual void on_message_flow(const message_flow_event_t &event) = 0;
};

class dispatch_diagnostics_options_t {
public:
    message_flow_log_mode_t message_flow() const noexcept;
    double sample_rate() const noexcept;
    bool include_message_sizes() const noexcept;
    const std::optional<std::string> &log_file() const noexcept;
    const std::optional<std::string> &label() const noexcept;
    const std::shared_ptr<std::atomic<message_flow_log_mode_t>> &
      live_mode() const noexcept;
    message_flow_log_mode_t effective_message_flow() const noexcept;
};

struct dispatch_options_t {
    dispatch_diagnostics_options_t diagnostics;
    std::shared_ptr<message_flow_observer_t> message_flow_observer;
    std::function<void(const message_flow_event_t &)> message_flow_callback;
    std::optional<logger_t<>> diagnostics_logger;

    dispatch_options_t &set_message_flow_observer(
      std::shared_ptr<message_flow_observer_t> observer);
    dispatch_options_t &set_message_flow_observer(
      std::function<void(const message_flow_event_t &)> observer);
    dispatch_options_t &message_flow(message_flow_log_mode_t mode);
    dispatch_options_t &trace_sample_rate(double rate);
    dispatch_options_t &include_message_sizes(bool include);
    dispatch_options_t &trace_log_file(std::string path);
    dispatch_options_t &trace_label(std::string id);
    dispatch_options_t &message_flow_live(
      std::shared_ptr<std::atomic<message_flow_log_mode_t>> live);
};
```

The meaning is owned by
[Message Flow Tracing](../../../../26-message-flow-tracing.en.md) and
[Flow Correlation](../../../../27-flow-correlation.en.md).

## 3. Health

```cpp
enum class health_status_t {
    healthy = 0,
    degraded = 1,
    unhealthy = 2
};
enum class health_check_scope_t {
    readiness = 0,
    liveness = 1,
    readiness_and_liveness = 2
};

struct health_check_result_t
{
    std::string name, component;
    health_status_t     status = health_status_t::healthy;
    health_check_scope_t scope = health_check_scope_t::readiness_and_liveness;
    std::string message;
};

struct health_report_t
{
    health_status_t status = health_status_t::healthy;
    health_status_t readiness = health_status_t::healthy;
    health_status_t liveness = health_status_t::healthy;
    std::vector<health_check_result_t> checks;
    bool ready () const noexcept;   // readiness != unhealthy
    bool live  () const noexcept;   // liveness  != unhealthy
};

class health_builder_t
{
public:
    health_builder_t();
    ~health_builder_t();
    health_builder_t(health_builder_t &&) noexcept;
    health_builder_t &operator=(health_builder_t &&) noexcept;
    health_builder_t(const health_builder_t &) = delete;
    health_builder_t &operator=(const health_builder_t &) = delete;

    health_builder_t &add_zlink_runtime_check (std::string name = "zlink.runtime");
    health_builder_t &add_channel_check        (std::string name);
    health_builder_t &add_location_check       (std::string name);
    health_builder_t &add_stream_endpoint_check(std::string name);
    health_builder_t &add_hosted_service_check (std::string name);
    health_builder_t &set_status(
      std::string name,
      health_status_t status,
      std::string message = {});

    health_report_t report () const;
};
```

**Readiness and liveness are separate.** Readiness to accept traffic
and process survival (liveness) are different questions.
**`degraded` doesn't block `ready()`/`live()`.**

## 4. Structured Logging And Metric Provider Boundary

The application configures the standard logging provider with
[Configuration and host](02-configuration-host.en.md)'s
`logging_builder_t`. Runtime state change and diagnostic information
are delivered through `log_record_t`'s identifier and field.

The following type and registration API aren't a public contract.

- Per-socket/Spot/Actor/STREAM raw event DTO
- Raw event handler and source registration builder
- Metric sample DTO and application callback
- Exporter lifecycle, registry, and provider-internal state

Peer and Channel's current status is checked through
`route_mesh_runtime_t`'s snapshot and observation. Host status is
checked through `app_t::runtime_state()`, `is_ready()`,
`relocate(...)`, and `shutdown(...)` results. Metric name, kind, unit,
and label are owned by
[Runtime Metric And Aggregation Rule](../../../../25-runtime-metrics.en.md).

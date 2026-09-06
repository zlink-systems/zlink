# C++ Monitoring Per-Language Interface

[C++ per-language interface table of contents](README.en.md)

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

struct core_hwm_status_t {
    std::optional<std::uint64_t> configured_memory_limit_bytes;
    std::optional<std::uint64_t> configured_budget_bytes;
    core_hwm_profile_t configured_profile;
    std::uint64_t effective_budget_bytes;
    std::uint64_t total_applied_hwm_bytes;
    std::uint64_t core_queue_accounted_bytes;
    std::uint64_t application_accounted_bytes;
    std::uint64_t current_accounted_bytes;
    std::uint64_t provisional_accounted_bytes;
    std::uint64_t peak_accounted_bytes;
    std::uint64_t completion_current_accounted_bytes;
    std::uint64_t completion_peak_accounted_bytes;
    std::uint64_t completion_pending_message_count;
    std::uint64_t total_messaging_accounted_bytes;
    std::uint64_t monitor_queue_applied_hwm_bytes;
    std::uint64_t monitor_queue_accounted_bytes;
    std::uint64_t total_instance_applied_hwm_bytes;
    std::uint64_t total_instance_accounted_bytes;
    std::uint64_t blocked_ratio_ppm;
    std::uint64_t active_directional_queue_count;
    std::uint64_t active_completion_directional_queue_count;
    std::uint64_t active_send_queue_count;
    std::uint64_t active_receive_queue_count;
    std::uint64_t outstanding_application_lease_count;
    std::uint64_t retired_queue_count;
    std::uint64_t deferred_origin_credit_bytes;
};

enum class application_job_queue_pressure_state_t { running, paused };

struct application_job_queue_status_t {
    application_job_queue_profile_t configured_profile;
    std::optional<std::uint32_t> configured_manual_max;
    std::uint32_t effective_processor_count;
    std::uint32_t effective_max_queued_application_jobs;
    std::uint32_t reserved_supply_permits;
    std::uint32_t queued_application_jobs;
    std::uint32_t permits_in_use;
    std::uint32_t peak_permits_in_use;
    std::uint32_t capacity_waiters;
    std::uint64_t capacity_wait_count;
    std::chrono::nanoseconds capacity_wait_duration;
    std::uint32_t configured_pause_threshold_percent;
    std::uint32_t configured_resume_threshold_percent;
    std::uint32_t pause_permit_count;
    std::uint32_t resume_permit_count;
    application_job_queue_pressure_state_t pressure_state;
    std::chrono::nanoseconds current_pause_duration;
};

struct host_capacity_status_t {
    std::uint64_t measurement_epoch;
    core_hwm_status_t core_hwm;
    application_job_queue_status_t application_job_queue;
};

struct framework_runtime_status_t {
    framework_runtime_state_t state;
    bool is_ready;
    bool accepting_work;
    std::optional<std::chrono::system_clock::time_point> operation_deadline;
    std::optional<relocation_result_t> relocation_result;
    std::optional<termination_result_t> termination_result;
    host_capacity_status_t capacity;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class framework_runtime_t {
public:
    virtual ~framework_runtime_t() = default;
    virtual framework_runtime_status_t status() const = 0;
    virtual void reset_capacity_metrics() = 0;
    virtual listener_status_t listener_status(
      listener_kind_t kind,
      std::string name) const = 0;
    virtual std::unique_ptr<runtime_observation_t> observe(
      std::size_t capacity,
      std::function<void(
        const observed_status_t<framework_runtime_status_t> &)> observer) = 0;
};
```

In `core_hwm_status_t`, `application_accounted_bytes`, `outstanding_application_lease_count`,
`retired_queue_count`, and `deferred_origin_credit_bytes` are ABI-reserved compatibility fields
and are always `0` since 0.13.1. The framework projects them unchanged and does not reinterpret
them as Application Job Queue pressure.

`is_ready` is `true` only when
`state == framework_runtime_state_t::serving`. `accepting_work`
indicates whether the host accepts a new application operation.
Status only provides the value the application needs to judge a
lifecycle operation's result and readiness. It doesn't include
relocation unit count, queue, barrier, and Store-internal state.

### 1.1 Local Listener Identity

The Framework exposes the endpoint that a local listener has confirmed
after binding. It does not expose a remote descriptor, connection
generation, or transport socket handle.

```cpp
enum class listener_kind_t {
    route_mesh,
    client_server,
    fanout,
    stream
};

struct listener_status_t {
    listener_kind_t kind;
    std::string name;
    std::string endpoint;
    std::chrono::system_clock::time_point observed_at;
};
```

`framework_runtime_t::listener_status(...)` returns the current
advertised endpoint after the named local listener has completed its
bind. If the listener is unknown or has not completed binding, it
throws `framework_exception_t` with
`framework_error_kind_t::not_configured`. A listener configured with
port `0` therefore reports the non-zero port selected by the operating
system. The endpoint uses the listener's `AdvertiseHost` when one is
configured; otherwise it uses the confirmed bind host.

The `name` is the configured MeshName, ChannelName, or StreamNodeName.
For a classic fanout publisher it is the ChannelName. The caller uses
the returned value for observation and readiness checks; it does not
copy the value into another listener's configuration.

### 1.2 RouteMesh Status

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

`observe(...)` delivers `observed_status_t<TStatus>`. `status` has type `TStatus`,
`loss` has type `observation_loss_t`, and both loss counters have type `std::uint64_t`.
[Runtime monitoring §7.2](../../../06-observability/01-runtime-monitoring.en.md#72-coalescing) owns the delivery unit, loss-counter semantics, range and saturation,
and subscription lifetime contract.

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

## 2. Message Flow Diagnostics

```cpp
enum class message_flow_log_mode_t {
    off = 0,
    errors = 1,
    normal = 2,
    detailed = 3
};

class dispatch_diagnostics_options_t {
public:
    message_flow_log_mode_t message_flow() const noexcept;
    double sample_rate() const noexcept;
    bool include_message_sizes() const noexcept;
};

struct dispatch_options_t {
    dispatch_diagnostics_options_t diagnostics;

    dispatch_options_t &message_flow(message_flow_log_mode_t mode);
    dispatch_options_t &trace_sample_rate(double rate);
    dispatch_options_t &include_message_sizes(bool include);
};

class inbound_dispatch_options_t {
public:
    inbound_dispatch_options_t &set_core_hwm_memory_limit_bytes(
      std::optional<std::uint64_t> value);
    inbound_dispatch_options_t &set_core_hwm_budget_bytes(
      std::optional<std::uint64_t> value);
    inbound_dispatch_options_t &set_core_hwm_profile(core_hwm_profile_t value);
    inbound_dispatch_options_t &set_application_job_queue_profile(
      application_job_queue_profile_t value);
    inbound_dispatch_options_t &set_max_queued_application_jobs(
      std::optional<std::uint32_t> value);
    inbound_dispatch_options_t &set_application_job_queue_pause_threshold_percent(
      std::uint32_t value);
    inbound_dispatch_options_t &set_application_job_queue_resume_threshold_percent(
      std::uint32_t value);
};
```

`off`, `errors`, `normal`, and `detailed` respectively mean disabled
diagnostics, errors only, key transitions, and detailed diagnostics. The
startup diagnostics level defaults to `errors` when omitted. The diagnostics part of
startup dispatch configuration only provides level, sampling rate, and whether to
include message size. Atomic runtime read/change of the level is owned by
`app_t::message_flow_mode()` and `app_t::set_message_flow_mode(...)`. The
application configures logger/trace/metric providers through the host's
standard logging and telemetry configuration, and the
Framework writes structured records to those providers. A message-flow
dispatch option doesn't take a file path, label, exporter lifecycle, or
provider sink. A message-flow observer callback, runtime error sink, and raw event DTO aren't the public
contract. A provider call failure does not change the original message
operation's terminal result and is isolated as separate diagnostics. The
remaining meaning is owned by
[Message Flow Tracing](../../../06-observability/03-message-flow-tracing.en.md) and
[Flow Correlation](../../../06-observability/04-flow-correlation.en.md).

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

C++ provides a public raw monitoring surface for Spot timer failures only.
The `monitoring_builder_t` returned by `app_t::monitoring()` registers a
SpotNode source with `add_spot_events(source_name)` and receives
`spot_event_t` via `on_spot_event(handler)`. `spot_event_t` carries
`source_name`, `timestamp`, `event`, and `spot_timer_diagnostic_t`, where
`event` is `timer_handler_failed` or
`timer_stopped_after_unhandled_exception`. Events from unregistered sources
are not delivered, and a handler exception does not affect timer/runtime
behavior.

The following type and registration API remain outside the public contract.

- Per-socket/Actor/STREAM raw event DTO
- Raw event handler and source registration builder other than the Spot
  timer failure surface above
- Metric sample DTO and application callback
- Exporter lifecycle, registry, and provider-internal state

Peer and Channel's current status is checked through
`route_mesh_runtime_t`'s snapshot and observation. Host status is
checked through `app_t::runtime_state()`, `is_ready()`,
`relocate(...)`, and `shutdown(...)` results. Metric name, kind, unit,
and label are owned by
[Runtime Metric And Aggregation Rule](../../../06-observability/02-runtime-metrics.en.md).

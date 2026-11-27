/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/actors/actor_client.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/configuration/endpoint_connections.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/handlers/handler_registry_runtime.hpp"
#include "runtime/host/framework_runtime.hpp"
#include "runtime/http/http_host_service.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_host_service.hpp"
#include "runtime/locations/location_lifecycle.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/streams/admission_queue_node.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/utils/weak_runtime_owner.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <variant>

namespace zlink::framework::detail
{

void configure_handler_invocation_executor ();

namespace
{

class store_actor_directory_t final : public actor_directory_t
{
  public:
    store_actor_directory_t (runtime::live_location_reader_t &store,
                             std::shared_ptr<runtime::actor_location_observer_t> actor_locations,
                             std::shared_ptr<std::string> actor_mesh_name) :
        _store (store),
        _actor_locations (std::move (actor_locations)),
        _actor_mesh_name (std::move (actor_mesh_name))
    {
    }

    task_t<std::optional<actor_ref_t>> find (std::string actor_id) override
    {
        auto read = co_await runtime::await_result (
          _store.read_authority (runtime::actor_authority_key (actor_id)));
        if (!read) {
            co_return detail::propagate_failure<std::optional<actor_ref_t>> (
              read, "actor authority lookup failed");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        const auto projection = snapshot ? runtime::decode_actor_authority_payload (
                                             snapshot->payload, snapshot->object_generation)
                                         : std::nullopt;
        if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor || !projection
            || projection->actor.actor_id ().value () != actor_id) {
            co_return result_t<std::optional<actor_ref_t>>::success (std::nullopt);
        }
        co_return result_t<std::optional<actor_ref_t>>::success (projection->actor);
    }

  private:
    runtime::live_location_reader_t &_store;
    std::shared_ptr<runtime::actor_location_observer_t> _actor_locations;
    std::shared_ptr<std::string> _actor_mesh_name;
};

bool has_inbound_channel (const std::vector<channel_snapshot_t> &channels)
{
    for (const auto &channel : channels) {
        if (channel.server.enabled && !channel.server.bind_endpoints.empty ()) {
            return true;
        }
        if (channel.subscriber.enabled
            && (channel.subscriber.discovery || !channel.subscriber.connect_endpoints.empty ())) {
            return true;
        }
    }
    return false;
}

void validate_object_store_configuration (
  const std::vector<std::shared_ptr<mesh_node_builder_state_t>> &registrations,
  bool has_location_store,
  bool has_relocation_store)
{
    const auto requires_location_store =
      std::any_of (registrations.begin (), registrations.end (), [] (const auto &registration) {
          return registration && registration->object_role != object_role_t::none;
      });
    if (requires_location_store && !has_location_store) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Object Client and Object Server MeshNodes require a Location Store");
    }

    bool requires_relocation_store = false;
    for (const auto &registration : registrations) {
        if (!registration || registration->object_role != object_role_t::server
            || !registration->spot_state)
            continue;

        const auto relocatable =
          registration->spot_state->lane
            .run_checked ([&] {
                if (!registration->spot_state->snapshot.instance_spot_names.empty ())
                    return true;
                const auto relocatable_actor = std::any_of (
                  registration->spot_state->actor_factories.begin (),
                  registration->spot_state->actor_factories.end (), [] (const auto &factory) {
                      const auto policy = factory.second.relocation.kind;
                      return policy == factory_relocation_kind_t::recreate
                             || policy == factory_relocation_kind_t::preserve_state;
                  });
                const auto relocatable_spot =
                  std::any_of (registration->spot_state->spot_factory_relocations.begin (),
                               registration->spot_state->spot_factory_relocations.end (),
                               [] (const auto &factory) {
                                   const auto policy = factory.second.kind;
                                   return policy == factory_relocation_kind_t::recreate
                                          || policy == factory_relocation_kind_t::preserve_state;
                               });
                return relocatable_actor || relocatable_spot;
            })
            .get ();
        if (relocatable) {
            requires_relocation_store = true;
            break;
        }
    }
    if (requires_relocation_store && !has_relocation_store) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Relocatable Object Server factories require a Relocation Store");
    }
}

zlink::routing_id_t
location_owner_node_rid (const std::vector<std::shared_ptr<mesh_node_builder_state_t>> &mesh_nodes)
{
    for (const auto &node : mesh_nodes) {
        if (node && node->routing_id) {
            return *node->routing_id;
        }
    }
    return zlink::routing_id_t::from ("framework");
}

using framework_observer_state_t =
  observation_detail::runtime_observer_state_t<framework_runtime_status_t>;

bool same_runtime_status (const framework_runtime_status_t &left,
                          const framework_runtime_status_t &right)
{
    return left.state == right.state && left.is_ready == right.is_ready
           && left.accepting_work == right.accepting_work
           && left.safe_to_shutdown == right.safe_to_shutdown
           && left.operation_deadline == right.operation_deadline
           && left.relocation_result == right.relocation_result
           && left.termination_result == right.termination_result
           && left.capacity == right.capacity;
}

class framework_runtime_status_source_t
{
  public:
    explicit framework_runtime_status_source_t (std::shared_ptr<app_state_access_t> access) :
        _access (std::move (access)), _worker ([this] { run (); })
    {
    }

    ~framework_runtime_status_source_t () { close (); }

    framework_runtime_status_t snapshot ()
    {
        std::shared_lock state_lock (_access->mutex);
        std::lock_guard lock (_mutex);
        return snapshot_locked ();
    }

    void reset_capacity_metrics ()
    {
        std::shared_lock state_lock (_access->mutex);
        auto *state = _access->state;
        if (!state) {
            throw framework_exception_t (
              framework_error_kind_t::shutting_down,
              "Framework runtime is no longer attached to a Core context");
        }
        if (!state->host_capacity) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Framework Host Capacity is not configured");
        }
        state->host_capacity->reset_metrics ();
    }

    void close () noexcept
    {
        if (_closed.exchange (true, std::memory_order_acq_rel))
            return;
        _changed.notify_all ();
        if (_worker.joinable ())
            _worker.join ();
    }

    std::shared_ptr<framework_observer_state_t>
    observe (std::size_t capacity,
             std::function<void (const observed_status_t<framework_runtime_status_t> &)> observer);

  private:
    // Both the lifetime and source locks cover collection, publication and enqueueing.
    framework_runtime_status_t snapshot_locked ()
    {
        auto *state = _access->state;
        if (!state) {
            return _last.value_or (framework_runtime_status_t{});
        }
        framework_runtime_status_t result;
        result.state = state->runtime_state.load (std::memory_order_acquire);
        result.is_ready = result.state == framework_runtime_state_t::serving;
        result.accepting_work = result.state == framework_runtime_state_t::serving;
        result.safe_to_shutdown = true;
        for (const auto &registration : detail::mesh_node_runtime_t::registrations (state->zlink)) {
            if (!registration || !registration->spot_state)
                continue;
            const auto node = detail::spot_node_runtime_t (registration->spot_state).native_node ();
            if (node)
                result.safe_to_shutdown =
                  result.safe_to_shutdown && node->status ().safe_to_shutdown;
        }
        {
            std::lock_guard lock (state->relocation_operation.mutex);
            const auto &operation = state->relocation_operation;
            if (operation.started && !operation.terminal)
                result.operation_deadline = operation.deadline_at;
            if (operation.terminal)
                result.relocation_result = operation.result;
        }
        {
            std::lock_guard lock (state->termination_operation.mutex);
            const auto &operation = state->termination_operation;
            if (operation.started && !operation.terminal)
                result.operation_deadline = operation.deadline_at;
            if (operation.terminal)
                result.termination_result = operation.result;
        }
        if (state->host_capacity) {
            try {
                result.capacity = state->host_capacity->snapshot ();
            }
            catch (...) {
                // The context can become terminal between lifecycle
                // observation and teardown. The last coherent capacity value
                // remains available through the source snapshot.
            }
        }
        if (_last && same_runtime_status (*_last, result))
            return *_last;
        result.observed_at = std::chrono::system_clock::now ();
        result.sequence = _last ? _last->sequence + 1 : 1;
        _last = result;

        const bool terminal = result.state == framework_runtime_state_t::stopped
                              || result.state == framework_runtime_state_t::error;
        for (auto iterator = _observers.begin (); iterator != _observers.end ();) {
            if (auto observer = iterator->lock ()) {
                observer->enqueue ("framework-runtime", result, terminal);
                ++iterator;
            } else {
                iterator = _observers.erase (iterator);
            }
        }
        return result;
    }

    void run () noexcept;

    std::shared_ptr<app_state_access_t> _access;
    std::mutex _mutex;
    std::optional<framework_runtime_status_t> _last;
    std::atomic_bool _closed{false};
    std::condition_variable _changed;
    std::vector<std::weak_ptr<framework_observer_state_t>> _observers;
    std::thread _worker;
};

std::shared_ptr<framework_observer_state_t> framework_runtime_status_source_t::observe (
  std::size_t capacity,
  std::function<void (const observed_status_t<framework_runtime_status_t> &)> observer)
{
    std::shared_lock state_lock (_access->mutex);
    auto state = std::make_shared<framework_observer_state_t> (
      capacity, std::move (observer), _access->state ? _access->state->monitoring : nullptr);
    state->start ();
    std::lock_guard lock (_mutex);
    auto initial = snapshot_locked ();
    _observers.erase (std::remove_if (_observers.begin (), _observers.end (),
                                      [] (const auto &item) { return item.expired (); }),
                      _observers.end ());
    _observers.emplace_back (state);
    const bool terminal = initial.state == framework_runtime_state_t::stopped
                          || initial.state == framework_runtime_state_t::error;
    state->enqueue ("framework-runtime", std::move (initial), terminal);
    return state;
}

void framework_runtime_status_source_t::run () noexcept
{
    while (!_closed.load (std::memory_order_acquire)) {
        snapshot ();
        std::unique_lock lock (_mutex);
        _changed.wait_for (lock, std::chrono::milliseconds (10),
                           [&] { return _closed.load (std::memory_order_acquire); });
    }
}

class framework_runtime_observation_t final : public runtime_observation_t
{
  public:
    explicit framework_runtime_observation_t (std::shared_ptr<framework_observer_state_t> state) :
        _state (std::move (state))
    {
    }

    ~framework_runtime_observation_t () override { close (); }

    void close () noexcept override
    {
        if (_state) {
            _state->close ();
            _state.reset ();
        }
    }

  private:
    std::shared_ptr<framework_observer_state_t> _state;
};

class public_framework_runtime_t final : public framework_runtime_t
{
  public:
    explicit public_framework_runtime_t (app_state_t &state) :
        _source (std::make_shared<framework_runtime_status_source_t> (state.status_access)),
        _listeners (state.listener_statuses)
    {
    }

    ~public_framework_runtime_t () override { _source->close (); }

    framework_runtime_status_t status () const override { return _source->snapshot (); }

    void reset_capacity_metrics () override { _source->reset_capacity_metrics (); }

    listener_status_t listener_status (listener_kind_t kind, std::string name) const override
    {
        const auto status = _listeners->find (kind, name);
        if (!status)
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "listener is not ready or is not registered");
        return *status;
    }

    std::vector<http_listener_status_t> http_listener_statuses () const override
    {
        return _listeners->http_listeners ();
    }

    std::unique_ptr<runtime_observation_t> observe (
      std::size_t capacity,
      std::function<void (const observed_status_t<framework_runtime_status_t> &)> observer) override
    {
        if (capacity == 0)
            throw std::invalid_argument ("runtime observation capacity must be positive");
        if (!observer)
            throw std::invalid_argument ("runtime observation callback is required");
        return std::make_unique<framework_runtime_observation_t> (
          _source->observe (capacity, std::move (observer)));
    }

  private:
    std::shared_ptr<framework_runtime_status_source_t> _source;
    std::shared_ptr<runtime::listener_status_registry_t> _listeners;
};

} // namespace

} // namespace zlink::framework::detail

namespace
{

using zlink::framework::detail::one_way_native_submit_result;

// Value parameters keep request state in the coroutine frame after the caller destroys its closure.
zlink::framework::request_task_t<
  zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>>
request_mesh_spot_from_app_task (
  std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t> mesh,
  std::string source_spot_id,
  zlink::routing_id_t target_node,
  std::string target_spot,
  std::uint64_t target_spot_generation,
  zlink::framework::runtime::messaging::message_parts_t parts,
  std::chrono::milliseconds timeout)
{
    using namespace zlink::framework;
    detail::host::pending_operation_t operation;
    auto submission =
      mesh->request_to_spot (source_spot_id, target_node, target_spot, target_spot_generation,
                             parts.items (), operation, timeout);
    auto admitted = co_await detail::request_admission_source_t{};
    mesh->observe_request_failure (operation, admitted, "MeshNode Spot request");
    const auto submitted = co_await submission;
    if (submitted != zlink::submit_result_t::ok) {
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::unavailable, "MeshNode Spot request was not submitted");
    }
    co_await detail::request_admitted_t{};
    co_return co_await mesh->await_request_reply (operation, "MeshNode Spot request");
}

zlink::framework::request_task_t<
  zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>>
request_mesh_channel_from_app_task (
  std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t> mesh,
  std::string channel_name,
  zlink::framework::runtime::messaging::message_parts_t parts,
  std::chrono::milliseconds timeout)
{
    using namespace zlink::framework;
    detail::host::pending_operation_t operation;
    auto submission = mesh->request_to_channel (channel_name, parts.items (), operation, timeout);
    auto admitted = co_await detail::request_admission_source_t{};
    mesh->observe_request_failure (operation, admitted, "RouteMesh channel request");
    const auto submitted = co_await submission;
    if (submitted != zlink::submit_result_t::ok) {
        if (submitted == zlink::submit_result_t::terminated) {
            co_return detail::boundary_failure<runtime::messaging::message_parts_t> (
              detail::boundary_error_t::shutdown, "RouteMesh channel request runtime is stopped");
        }
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          runtime::messaging::map_submit_result_error_kind (submitted),
          "RouteMesh channel request was not submitted");
    }
    co_await detail::request_admitted_t{};
    co_return co_await mesh->await_request_reply (operation, "RouteMesh channel request");
}

} // namespace

namespace zlink::framework
{

zlink_framework_options_t &app_t::add_zlink_framework ()
{
    if (_state->framework_applied) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "ZLink Framework configuration is already applied");
    }
    if (_state->framework_options) {
        return *_state->framework_options;
    }
    auto bus = _state->zlink.message_bus ();
    auto channel = detail::channel_runtime_t::from (bus);
    channel.bind_runtime_admission ([started = _state->run_started, draining = _state->draining] {
        return std::pair{draining->load (std::memory_order_acquire),
                         started->load (std::memory_order_acquire)};
    });
    _state->framework_hosted_service_position = _state->hosted_services.size ();
    detail::channel_runtime_t::from (_state->zlink.message_bus ())
      .bind_serializers (_state->serializers);
    _state->monitoring->diagnostics_logger =
      _state->logging.create_logger ("zlink.framework.runtime");
    if (!_state->services.contains (std::type_index (typeid (logger_factory_t)))) {
        _state->services.add_singleton<logger_factory_t> (
          std::make_unique<logger_factory_t> (_state->logging.factory ()));
    }
    if (!_state->services.contains (std::type_index (typeid (detail::actor_gateway_runtime_t)))) {
        _state->services.add_singleton<detail::actor_gateway_runtime_t> ();
    }
    if (!_state->services.contains (std::type_index (typeid (session_actor_manager_t)))) {
        _state->services.add_factory<session_actor_manager_t> (
          [] (service_provider_t &provider) {
              return std::make_unique<session_actor_manager_t> (
                provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
          },
          service_lifetime_t::scoped);
    }
    _state->services.add_singleton<channel_client_t> (
      std::make_unique<channel_client_t> (_state->zlink.message_bus ()));
    _state->services.add_singleton<channel_runtime_options_t> (
      std::make_unique<channel_runtime_options_t> (_state->zlink.message_bus ()));
    _state->services.add_singleton<publisher_t> (
      std::make_unique<publisher_t> (_state->zlink.publisher ()));
    _state->services.add_factory<route_client_t> (
      [this] (service_provider_t &) {
          return std::make_unique<route_client_t> (
            _state->zlink.route_client (_state->serializers));
      },
      service_lifetime_t::singleton);
    if (!_state->services.contains (std::type_index (typeid (serializer_registry_t)))) {
        _state->services.add_factory<serializer_registry_t> (
          [serializers = &_state->serializers] (service_provider_t &) {
              return std::shared_ptr<serializer_registry_t> (
                serializers, [] (serializer_registry_t *) noexcept {});
          },
          service_lifetime_t::singleton);
    }
    _state->framework_options = std::make_unique<zlink_framework_options_t> (
      _state->services, _state->handlers, _state->serializers, _state->zlink);
    return *_state->framework_options;
}

app_t &app_t::add_zlink_framework (std::function<void (zlink_framework_options_t &)> configure)
{
    auto &options = add_zlink_framework ();
    if (configure) {
        configure (options);
    }
    _apply_zlink_framework ();
    return *this;
}

void app_t::_apply_zlink_framework ()
{
    if (_state->framework_applied || !_state->framework_options) {
        return;
    }
    auto &options = *_state->framework_options;
    const auto first_framework_service = _state->hosted_services.size ();
    // Route message-flow records through the application's standard logging
    // provider. With no configured sink diagnostics remain silent.
    // Install the shared, runtime-mutable message-flow mode so set_message_flow_mode
    // can flip tracing on/off live. Seeded from the configured mode; shared across
    // all surfaces because dispatch options copy the shared_ptr.
    // Seed the (already-created) shared atomic from the configured mode and share it
    // with every surface via dispatch options. The shared_ptr is never reassigned,
    // so runtime set_message_flow_mode races only on the atomic (safe).
    _state->message_flow_mode->store (options.configure_dispatch ().diagnostics.message_flow (),
                                      std::memory_order_relaxed);
    detail::dispatch_options_access_t::set_live_mode (options.configure_dispatch (),
                                                      _state->message_flow_mode);
    if (_state->logging.has_output_sink ()) {
        detail::dispatch_options_access_t::set_logger (
          options.configure_dispatch (),
          _state->logging.factory ().create ("zlink.framework.dispatch"));
    }
    detail::dispatch_options_access_t::bind_telemetry (options.configure_dispatch (),
                                                       _state->monitoring->metric_provider,
                                                       _state->monitoring->metric_meter);
    const auto http_snapshot = options.http ().snapshot ();
    const auto job_queue_configuration = runtime::resolve_application_job_queue_configuration (
      options.application_job_queue_profile (), options.max_queued_application_jobs (),
      runtime::detect_application_job_queue_processor_limits (
        options.handler_coroutine_workers () == 0
          ? std::nullopt
          : std::optional<std::uint32_t> (static_cast<std::uint32_t> (std::min<std::size_t> (
              options.handler_coroutine_workers (), std::numeric_limits<std::uint32_t>::max ())))),
      options.application_job_queue_pause_threshold_percent (),
      options.application_job_queue_resume_threshold_percent ());
    auto core_context = std::make_shared<zlink::context_t> ();
    auto core_options = core_context->options ();
    core_options.auto_hwm_enabled (true);
    core_options.core_hwm_profile (
      static_cast<zlink::auto_hwm_profile> (options.core_hwm_profile ()));
    if (options.core_hwm_budget_bytes () != 0) {
        core_options.core_hwm_budget_bytes (
          zlink::byte_count_t::bytes (options.core_hwm_budget_bytes ()));
    }
    if (options.core_hwm_memory_limit_bytes () != 0) {
        core_options.core_hwm_memory_limit_bytes (
          zlink::byte_count_t::bytes (options.core_hwm_memory_limit_bytes ()));
    }
    detail::zlink_builder_access_t::bind_shared_core_context (_state->zlink,
                                                              std::move (core_context));
    const auto monitoring = _state->monitoring;
    _state->application_job_queue = std::make_shared<runtime::application_job_queue_t> (
      job_queue_configuration,
      [monitoring] {
          detail::monitoring_runtime_t (monitoring).publish_application_job_queue_failure ();
      },
      [monitoring] (std::uint64_t local_waiters, std::uint32_t maximum) {
          detail::monitoring_runtime_t (monitoring)
            .publish_local_job_backlog (local_waiters, maximum);
      });
    _state->host_capacity = std::make_shared<runtime::host_capacity_runtime_t> (
      detail::zlink_builder_access_t::shared_core_context (_state->zlink),
      _state->application_job_queue,
      options.core_hwm_memory_limit_bytes () == 0
        ? std::nullopt
        : std::optional<std::uint64_t> (options.core_hwm_memory_limit_bytes ()),
      options.core_hwm_budget_bytes () == 0
        ? std::nullopt
        : std::optional<std::uint64_t> (options.core_hwm_budget_bytes ()),
      options.core_hwm_profile (), _state->monitoring);
    detail::channel_runtime_t::from (_state->zlink.message_bus ())
      .bind_core_context (detail::zlink_builder_access_t::shared_core_context (_state->zlink));
    if (!_state->services.contains (std::type_index (typeid (framework_runtime_t)))) {
        auto *state = _state.get ();
        _state->services.add_factory<framework_runtime_t> (
          [state] (service_provider_t &) {
              return std::unique_ptr<framework_runtime_t> (
                std::make_unique<detail::public_framework_runtime_t> (*state));
          },
          service_lifetime_t::singleton);
    }
    options.apply ();
    const bool has_public_location_store =
      _state->services.contains (std::type_index (typeid (location_store_t)));
    const bool has_public_relocation_store =
      _state->services.contains (std::type_index (typeid (relocation_store_t)));
    detail::configure_app_store_repositories (_state.get ());
    const auto actor_location_observer = std::make_shared<runtime::actor_location_observer_t> ();
    const auto actor_mesh_name = std::make_shared<std::string> ();
    if (_state->services.contains (std::type_index (typeid (location_repository_t)))) {
        if (!_state->services.contains (
              std::type_index (typeid (runtime::stateful::authority_relocation_port_t)))) {
            _state->services.add_factory<runtime::stateful::authority_relocation_port_t> (
              [] (service_provider_t &provider) {
                  return std::unique_ptr<runtime::stateful::authority_relocation_port_t> (
                    std::make_unique<runtime::stateful::public_authority_store_adapter_t> (
                      provider.get_required<location_repository_t> ()));
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (std::type_index (typeid (runtime::location_runtime_t)))) {
            const auto location_options = options.location_options ();
            _state->services.add_factory<runtime::location_runtime_t> (
              [location_options, monitoring = _state->monitoring] (service_provider_t &provider) {
                  const auto resolver = detail::service_scope_access_t::get_required_shared<
                    runtime::store_location_resolvers_t> (provider);
                  auto runtime = std::make_unique<runtime::location_runtime_t> (
                    provider.get_required<location_repository_t> (), location_options,
                    [resolver] (authority_key_t key, std::optional<std::string> version) {
                        resolver->observe_authority_version (key, std::move (version));
                    });
                  runtime->bind_monitoring (monitoring);
                  return runtime;
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (
              std::type_index (typeid (runtime::live_location_reader_t)))) {
            const auto location_options = options.location_options ();
            _state->services.add_factory<runtime::live_location_reader_t> (
              [location_options] (service_provider_t &provider) {
                  return std::make_unique<runtime::live_location_reader_t> (
                    provider.get_required<location_repository_t> (), location_options);
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (std::type_index (typeid (runtime::location_lifecycle_t)))) {
            _state->services.add_factory<runtime::location_lifecycle_t> (
              [] (service_provider_t &provider) {
                  return std::make_unique<runtime::location_lifecycle_t> (
                    provider.get_required<runtime::location_runtime_t> ());
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (
              std::type_index (typeid (runtime::store_location_resolvers_t)))) {
            const auto resolver_location_options = options.location_options ();
            _state->services.add_factory<runtime::store_location_resolvers_t> (
              [resolver_location_options, actor_location_observer] (service_provider_t &provider) {
                  return std::make_shared<runtime::store_location_resolvers_t> (
                    provider.get_required<runtime::live_location_reader_t> (),
                    resolver_location_options, actor_location_observer);
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (
              std::type_index (typeid (runtime::spot_address_resolver_t)))) {
            _state->services.add_factory<runtime::spot_address_resolver_t> (
              [] (service_provider_t &provider) {
                  // Shares the store resolvers' ownership: runtimes that bind this
                  // resolver keep it for their late operations.
                  const auto resolvers = detail::service_scope_access_t::get_required_shared<
                    runtime::store_location_resolvers_t> (provider);
                  return std::shared_ptr<runtime::spot_address_resolver_t> (resolvers,
                                                                            resolvers.get ());
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (
              std::type_index (typeid (runtime::actor_address_resolver_t)))) {
            _state->services.add_factory<runtime::actor_address_resolver_t> (
              [] (service_provider_t &provider) {
                  return std::shared_ptr<runtime::actor_address_resolver_t> (
                    &provider.get_required<runtime::store_location_resolvers_t> (),
                    [] (runtime::actor_address_resolver_t *) noexcept {});
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (std::type_index (typeid (actor_directory_t)))) {
            _state->services.add_factory<actor_directory_t> (
              [actor_location_observer, actor_mesh_name] (service_provider_t &provider) {
                  return std::shared_ptr<actor_directory_t> (
                    std::make_shared<detail::store_actor_directory_t> (
                      provider.get_required<runtime::live_location_reader_t> (),
                      actor_location_observer, actor_mesh_name));
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (std::type_index (typeid (location_readiness_t)))) {
            _state->services.add_factory<location_readiness_t> (
              [] (service_provider_t &provider) {
                  return std::shared_ptr<location_readiness_t> (
                    &provider.get_required<runtime::store_location_resolvers_t> (),
                    [] (location_readiness_t *) noexcept {});
              },
              service_lifetime_t::singleton);
        }
        if (!_state->services.contains (std::type_index (typeid (location_runtime_query_t)))) {
            const auto location_options = options.location_options ();
            _state->services.add_factory<location_runtime_query_t> (
              [location_options, actor_location_observer] (service_provider_t &provider) {
                  return std::shared_ptr<location_runtime_query_t> (
                    std::make_shared<runtime::store_location_runtime_query_t> (
                      provider.get_required<runtime::live_location_reader_t> (),
                      provider.get_required<runtime::location_runtime_t> (), location_options,
                      actor_location_observer));
              },
              service_lifetime_t::singleton);
        }
    }
    detail::bind_zlink_monitoring (_state->zlink, _state->monitoring);
    detail::bind_stream_serializers (_state->zlink, _state->serializers);
    auto &actor_gateway_runtime =
      _state->services.build_provider ().get_required<detail::actor_gateway_runtime_t> ();
    auto location_provider = _state->services.build_provider ();
    const auto locations = location_provider.get<runtime::location_runtime_t> ();
    auto *location_runtime = locations ? &locations->get () : nullptr;
    actor_gateway_runtime.bind_serializers (_state->serializers);
    actor_gateway_runtime.set_dispatch (options.configure_dispatch ());
    auto channel_runtime = detail::channel_runtime_t::from (_state->zlink.message_bus ());
    channel_runtime.bind_listener_statuses (_state->listener_statuses);
    channel_runtime.bind_fanout_advertise_hosts (options.runtime_fanout_advertise_hosts ());
    const auto channel_snapshot = channel_runtime.channel_snapshots ();
    auto channel_runtime_manager = detail::channel_runtime_manager_t::from (_state->zlink);
    channel_runtime_manager.initialize_route_channels (_state->zlink);
    auto mesh_node_registrations = detail::mesh_node_runtime_t::registrations (_state->zlink);
    detail::validate_object_store_configuration (mesh_node_registrations, has_public_location_store,
                                                 has_public_relocation_store);
    const auto stream_snapshot = detail::stream_runtime_t::from (_state->zlink).snapshots ();
    const auto live_locations = location_provider.get<runtime::live_location_reader_t> ();
    auto queue_settings = runtime::admission_queue::configure (
      stream_snapshot, mesh_node_registrations,
      has_public_location_store && live_locations ? &live_locations->get () : nullptr);
    const auto shared_core_context =
      detail::zlink_builder_access_t::shared_core_context (_state->zlink);
    for (const auto &registration : mesh_node_registrations) {
        registration->lane
          .run ([&] {
              registration->core_context = shared_core_context;
              registration->application_jobs = _state->application_job_queue;
          })
          .get ();
    }
    _state->has_manual_service_topology = [options, mesh_node_registrations, channel_runtime,
                                           channel_runtime_manager] () mutable {
        const auto has_manual_endpoints = [] (const auto &channel_connections) {
            return detail::endpoint_connections_runtime_t::has_connections (
              channel_connections.second);
        };
        if (std::ranges::any_of (options.client_endpoint_connections (), has_manual_endpoints)
            || std::ranges::any_of (options.subscriber_endpoint_connections (),
                                    has_manual_endpoints)
            || std::ranges::any_of (channel_runtime.channel_snapshots (), [] (const auto &channel) {
                   return channel.publisher.enabled && !channel.publisher.discovery;
               })) {
            return true;
        }
        for (const auto &registration : mesh_node_registrations) {
            if (registration->lane.run ([&] { return !registration->peer_connections.empty (); })
                  .get ())
                return true;
        }
        for (const auto &route_id : channel_runtime_manager.route_channel_ids ()) {
            if (!channel_runtime_manager.get_route_channel (route_id)
                   .manual_connections ()
                   .empty ()) {
                return true;
            }
        }
        return false;
    };
    auto monitoring_state = _state->monitoring;
    auto application_mesh_registration =
      std::find_if (mesh_node_registrations.begin (), mesh_node_registrations.end (),
                    [] (const auto &registration) {
                        return registration->spot_state->snapshot.entry_spot_name.has_value ();
                    });
    if (application_mesh_registration == mesh_node_registrations.end ()
        && !mesh_node_registrations.empty ())
        application_mesh_registration = mesh_node_registrations.begin ();
    const auto application_mesh_name =
      application_mesh_registration != mesh_node_registrations.end ()
        ? (*application_mesh_registration)->mesh_name
        : std::string{};
    *actor_mesh_name = application_mesh_name;
    {
        auto provider = _state->services.build_provider ();
        const auto spot_resolver = location_runtime
                                     ? detail::service_scope_access_t::get_required_shared<
                                         runtime::spot_address_resolver_t> (provider)
                                     : nullptr;
        if (location_runtime) {
            channel_runtime.bind_spot_address_resolver (spot_resolver);
            provider.get_required<runtime::store_location_resolvers_t> ().set_actor_mesh_name (
              application_mesh_name);
        }
        auto route_client = provider.get_required<route_client_t> ();
        for (const auto &registration : mesh_node_registrations) {
            registration->spot_state->dispatch = options.configure_dispatch ();
            registration->spot_state->worker_options = options.worker ();
            registration->spot_state->monitoring = monitoring_state;
            detail::spot_node_runtime_t spot_runtime (registration->spot_state);
            spot_runtime.set_message_follow_duration (
              options.location_options ().message_follow_duration);
            if (_state->services.contains (
                  std::type_index (typeid (runtime::stateful::relocation_store_port_t)))) {
                auto &relocations =
                  provider.get_required<runtime::stateful::relocation_store_port_t> ();
                spot_runtime.bind_relocation_store (
                  std::shared_ptr<runtime::stateful::relocation_store_port_t> (
                    &relocations, [] (auto *) noexcept {}));
            }
            if (_state->services.contains (
                  std::type_index (typeid (runtime::stateful::authority_relocation_port_t)))) {
                auto &authority =
                  provider.get_required<runtime::stateful::authority_relocation_port_t> ();
                spot_runtime.bind_relocation_authority (
                  std::shared_ptr<runtime::stateful::authority_relocation_port_t> (
                    &authority, [] (auto *) noexcept {}));
            }
            if (location_runtime)
                spot_runtime.bind_location_lifecycle (
                  provider.get_required<runtime::location_lifecycle_t> ());
            spot_runtime.bind_spot_location_resolver (spot_resolver);
            spot_runtime.bind_drain_flag (_state->draining);
            spot_runtime.bind_host_lifecycle ([access = _state->status_access] {
                std::shared_lock lock (access->mutex);
                detail::host_lifecycle_snapshot_t snapshot;
                if (!access->state) {
                    snapshot.phase = framework_runtime_state_t::stopped;
                    return snapshot;
                }
                snapshot.phase = access->state->runtime_state.load (std::memory_order_acquire);
                auto &operation = access->state->termination_operation;
                std::lock_guard operation_lock (operation.mutex);
                snapshot.shutdown_deadline = operation.deadline_at;
                if (operation.started)
                    snapshot.cleanup_cancellation = operation.cleanup_cancellation.get_token ();
                return snapshot;
            });
            spot_runtime.set_route_client (route_client);
            spot_runtime.bind_service_provider (provider);
        }
    }
    if (!mesh_node_registrations.empty ()
        && !_state->services.contains (std::type_index (typeid (spot_manager_t)))) {
        _state->services.add_singleton<spot_manager_t> (std::make_unique<spot_manager_t> (
          detail::spot_node_runtime_t ((*application_mesh_registration)->spot_state).manager ()));
    }
    if (!mesh_node_registrations.empty ()
        && !_state->services.contains (std::type_index (typeid (spot_publisher_client_t)))) {
        auto provider = _state->services.build_provider ();
        _state->services.add_singleton<spot_publisher_client_t> (
          std::make_unique<spot_publisher_client_t> (provider.get_required<spot_manager_t> (),
                                                     _state->serializers));
    }
    const auto location_owner = detail::location_owner_node_rid (mesh_node_registrations);
    if (location_runtime)
        add_hosted_service (std::make_unique<runtime::location_host_service_t> (location_owner));
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes;
    const auto callback_registrations = mesh_node_registrations;
    runtime::mesh_node_host_service_t *mesh_node_service = nullptr;
    if (!mesh_node_registrations.empty ()) {
        actor_gateway_runtime.on_membership (
          [node = std::weak_ptr<detail::spot_node_builder_state_t> (
             (*application_mesh_registration)->spot_state)] (const actor_ref_t &actor) {
              const auto owner = node.lock ();
              return owner ? detail::spot_node_runtime_t (owner).actor_spot (actor) : std::nullopt;
          });
        auto mesh_service = std::make_unique<runtime::mesh_node_host_service_t> (
          _state->monitoring->runtime_failures, std::move (mesh_node_registrations),
          _state->serializers, _state->handlers, options.dispatch_options (),
          _state->listener_statuses, _state->application_job_queue);
        mesh_node_service = mesh_service.get ();
        mesh_nodes = mesh_service->nodes ();
        _state->route_mesh_nodes = mesh_nodes;
        auto provider = _state->services.build_provider ();

        std::shared_ptr<runtime::stateful::authority_relocation_port_t> relocation_authority;
        std::shared_ptr<runtime::stateful::relocation_store_port_t> relocation_store;
        std::shared_ptr<runtime::stateful::aggregate_authority_port_t>
          aggregate_relocation_authority;
        if (_state->services.contains (
              std::type_index (typeid (runtime::stateful::authority_relocation_port_t)))
            && _state->services.contains (
              std::type_index (typeid (runtime::stateful::relocation_store_port_t)))) {
            relocation_authority = std::shared_ptr<runtime::stateful::authority_relocation_port_t> (
              &provider.get_required<runtime::stateful::authority_relocation_port_t> (),
              [] (auto *) noexcept {});
            relocation_store = std::shared_ptr<runtime::stateful::relocation_store_port_t> (
              &provider.get_required<runtime::stateful::relocation_store_port_t> (),
              [] (auto *) noexcept {});
            aggregate_relocation_authority =
              std::make_shared<runtime::stateful::public_aggregate_authority_adapter_t> (
                provider.get_required<location_repository_t> ());
        }
        for (const auto &mesh_node : mesh_nodes) {
            mesh_node->bind_delay_owner (_state->delays);
            if (location_runtime)
                mesh_node->configure_session_route_owner (
                  [location_runtime] { return location_runtime->current_owner_token (); });
            if (relocation_authority && relocation_store) {
                const auto location_options = options.location_options ();
                runtime::stateful::relocation_limits_t relocation_limits;
                relocation_limits.outbound_units = location_options.max_active_outbound_relocations;
                relocation_limits.inbound_units = location_options.max_active_inbound_relocations;
                relocation_limits.capture_callbacks =
                  location_options.max_concurrent_relocation_captures;
                relocation_limits.restore_callbacks =
                  location_options.max_concurrent_relocation_restores;
                relocation_limits.payload_chunk_limit_bytes =
                  location_options.relocation_payload_chunk_limit_bytes;
                relocation_limits.in_flight_payload_budget_bytes =
                  location_options.relocation_in_flight_payload_budget_bytes;
                relocation_limits.node_in_flight_payload_budget_bytes =
                  location_options.relocation_node_in_flight_payload_budget_bytes;
                relocation_limits.cutover_wait_timeout =
                  location_options.relocation_cutover_wait_timeout;
                relocation_limits.message_follow_duration =
                  location_options.message_follow_duration;
                mesh_node->configure_relocation_runtime (relocation_authority, relocation_store,
                                                         aggregate_relocation_authority,
                                                         relocation_limits);
            }
            mesh_node->configure_bound_session_relocation_resolver (
              [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state ()] (
                const runtime::stateful::object_ref_t &source)
                -> task_t<std::optional<detail::bound_session_relocation_route_t>> {
                  auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
                    detail::lock_runtime_owner (weak_actor_gateway_runtime));
                  if (source.kind != runtime::stateful::object_kind_t::actor
                      || source.object_generation == 0 || source.authority_owner_generation == 0)
                      co_return std::nullopt;

                  const auto actor =
                    detail::actor_ref_access_t::make (node_rid_t::from_string (source.node_id), {},
                                                      source.key, source.object_generation);
                  const auto route =
                    co_await actor_gateway_runtime.bound_session_route_async (actor);
                  if (!route || !route->session_rid)
                      co_return std::nullopt;
                  if (route->object_generation != source.object_generation
                      || route->authority_owner_generation != source.authority_owner_generation
                      || route->node_generation == 0 || route->binding_generation == 0) {
                      throw std::runtime_error ("Bound Session relocation route is stale");
                  }

                  // Session-Actor binding §4: command 42 carries the Session owner
                  // identity stored at bind unchanged.
                  if (route->session_owner_id.empty ()
                      || route->session_owner_lease_generation == 0)
                      throw std::runtime_error (
                        "Bound Session route has no Session owner identity");
                  co_return detail::bound_session_relocation_route_t{
                    route->node_rid,
                    route->node_generation,
                    {route->session_owner_id,
                     static_cast<std::int64_t> (route->session_owner_lease_generation)},
                    *route->session_rid,
                    route->binding_generation};
              });
        }
        if (!_state->services.contains (std::type_index (typeid (actor_manager_t))))
            _state->services.add_singleton<actor_manager_t> (
              std::make_unique<actor_manager_t> (mesh_service->actor_manager ()));
        add_hosted_service (std::move (mesh_service));
    }
    std::vector<std::weak_ptr<detail::mesh_node_runtime_t>> weak_mesh_nodes (mesh_nodes.begin (),
                                                                             mesh_nodes.end ());
    for (const auto &mesh : mesh_nodes) {
        mesh->configure_operation_owner_resolver (
          [weak_mesh_nodes] (runtime::foundation::call_id_t operation, std::uint64_t reply_route_id)
            -> std::optional<runtime::mesh::raw_mesh_node_owner_t::operation_owner_t> {
              for (const auto &weak : weak_mesh_nodes)
                  if (const auto candidate = weak.lock ())
                      if (auto owner = candidate->local_operation_owner (operation, reply_route_id))
                          return owner;
              return std::nullopt;
          });
    }
    if (!mesh_nodes.empty ()) {
        const auto application_mesh_it =
          std::find_if (mesh_nodes.begin (), mesh_nodes.end (), [&] (const auto &mesh) {
              return mesh->mesh_name () == application_mesh_name;
          });
        const auto application_mesh =
          application_mesh_it != mesh_nodes.end () ? *application_mesh_it : mesh_nodes.front ();
        for (const auto &registration : callback_registrations) {
            detail::spot_node_runtime_t spot_runtime (registration->spot_state);
            spot_runtime.on_actor_message_follow (
              [weak_application_mesh = std::weak_ptr (application_mesh)] (
                const actor_ref_t &actor, const runtime::messaging::envelope_header_t &header,
                const zlink::message_t &payload, std::chrono::milliseconds timeout,
                const zlink::routing_id_t &source_node,
                const runtime::protocol::actor_route_fence_t &stale_route, std::uint8_t hop_count,
                const runtime::protocol::wire_operation_id_t &operation,
                std::uint64_t reply_route_id) -> task_t<std::optional<zlink::message_t>> {
                  const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
                  co_return co_await application_mesh->relay_application_actor (
                    actor, header, payload, timeout, source_node, stale_route, hop_count, operation,
                    reply_route_id);
              });
            spot_runtime.on_actor_handoff_terminal (
              [weak_application_mesh = std::weak_ptr (application_mesh)] (
                const zlink::routing_id_t &source_node,
                const zlink::routing_id_t &source_owner_node,
                const runtime::protocol::wire_operation_id_t &operation,
                std::uint64_t reply_route_id,
                const runtime::protocol::actor_route_fence_t &source_fence,
                const result_t<zlink::message_t> &terminal) -> task_t<bool> {
                  const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
                  runtime::messaging::client_call_codec_t codec;
                  auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                                       "node", "__zlink.actorHandoffTerminal",
                                                       std::chrono::seconds (30));
                  header.metadata.insert_or_assign (
                    std::string (detail::actor_handoff_operation_high_key),
                    std::to_string (operation.high));
                  header.metadata.insert_or_assign (
                    std::string (detail::actor_handoff_operation_low_key),
                    std::to_string (operation.low));
                  header.metadata.insert_or_assign (
                    std::string (detail::actor_handoff_reply_route_key),
                    std::to_string (reply_route_id));
                  header.metadata.insert_or_assign (
                    std::string (detail::actor_handoff_source_node_key),
                    source_owner_node.to_hex ());
                  header.metadata.insert_or_assign (
                    std::string (detail::actor_handoff_parking_node_key), source_node.to_hex ());
                  if (!source_fence.actor_id.empty ()) {
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_actor_id_key),
                        source_fence.actor_id);
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_object_generation_key),
                        std::to_string (source_fence.object_generation));
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_target_node_key),
                        zlink::routing_id_t::from (source_fence.target_node_routing_id).to_hex ());
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_target_node_generation_key),
                        std::to_string (source_fence.target_node_generation));
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_authority_generation_key),
                        std::to_string (source_fence.authority_owner_generation));
                      header.metadata.insert_or_assign (
                        std::string (detail::actor_handoff_route_lease_generation_key),
                        std::to_string (source_fence.owner_lease_generation));
                  }
                  header.metadata.insert_or_assign ("__zlink.actorHandoffTerminalSuccess",
                                                    terminal ? "1" : "0");
                  if (!terminal) {
                      header.metadata.insert_or_assign (
                        "__zlink.actorHandoffTerminalErrorKind",
                        std::to_string (static_cast<int> (terminal.error_kind ())));
                      header.metadata.insert_or_assign ("__zlink.actorHandoffTerminalErrorMessage",
                                                        terminal.error ()
                                                          ? terminal.error ()->what ()
                                                          : "Actor handoff request failed");
                  }
                  auto body = terminal ? terminal.value ().copy () : zlink::message_t{};
                  auto parts = runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (
                    header, std::move (body));
                  co_return co_await application_mesh->native_node ().send_to_node (
                    source_node, std::move (parts).take_items ())
                    == zlink::submit_result_t::ok;
              });
            spot_runtime.on_actor_leave_notification (
              [weak_application_mesh = std::weak_ptr (application_mesh)] (
                const zlink::routing_id_t &target_node,
                std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
                  const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
                  co_return co_await application_mesh->native_node ().send_to_node (
                    target_node, std::move (parts));
              });
        }
    }
    if (!mesh_nodes.empty ()
        && !_state->services.contains (std::type_index (typeid (route_mesh_runtime_t)))) {
        auto provider = _state->services.build_provider ();
        auto location_runtime = provider.get<location_runtime_query_t> ();
        auto location_store = provider.get<location_repository_t> ();
        auto mesh_runtime = std::make_shared<runtime::route_mesh_runtime_service_t> (
          mesh_nodes, location_runtime ? &location_runtime->get () : nullptr,
          location_store ? &location_store->get () : nullptr, _state->monitoring,
          [access = _state->status_access] {
              std::shared_lock lock (access->mutex);
              return access->state ? access->state->runtime_state.load (std::memory_order_acquire)
                                   : framework_runtime_state_t::stopped;
          });
        _state->services.add_factory<route_mesh_runtime_t> (
          [mesh_runtime] (service_provider_t &) {
              return std::static_pointer_cast<route_mesh_runtime_t> (mesh_runtime);
          },
          service_lifetime_t::singleton);
        add_hosted_service (
          std::make_unique<runtime::route_mesh_runtime_host_service_t> (std::move (mesh_runtime)));
    }
    if (!mesh_nodes.empty ()
        && !_state->services.contains (std::type_index (typeid (route_mesh_runtime_options_t)))) {
        _state->services.add_singleton<route_mesh_runtime_options_t> (
          std::make_unique<runtime::route_mesh_runtime_options_service_t> (mesh_nodes));
    }
    if (location_runtime && !mesh_nodes.empty ()) {
        detail::configure_app_instance_spot (_state.get (), options, channel_runtime,
                                             mesh_node_registrations, weak_mesh_nodes);
    }
    const auto spot_router_channels = options.location_options ().spot_router_channels;
    for (const auto &mesh : mesh_nodes) {
        const auto source_spot_id = detail::new_user_spot_id ();
        auto send_to_spot = [mesh, source_spot_id] (
                              const zlink::routing_id_t &target_node,
                              const std::string &target_spot, std::uint64_t target_spot_generation,
                              runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
            const auto submitted = co_await mesh->send_to_spot (
              source_spot_id, target_node, target_spot, target_spot_generation, parts.items ());
            co_return one_way_native_submit_result (submitted, "MeshNode Spot send");
        };
        auto request_to_spot = [mesh, source_spot_id] (const zlink::routing_id_t &target_node,
                                                       const std::string &target_spot,
                                                       std::uint64_t target_spot_generation,
                                                       runtime::messaging::message_parts_t parts,
                                                       std::chrono::milliseconds timeout)
          -> request_task_t<result_t<runtime::messaging::message_parts_t>> {
            return request_mesh_spot_from_app_task (mesh, source_spot_id, target_node, target_spot,
                                                    target_spot_generation, std::move (parts),
                                                    timeout);
        };
        const auto mesh_name = mesh->mesh_name ();
        const auto claimed_as_route_alias =
          std::any_of (spot_router_channels.begin (), spot_router_channels.end (),
                       [&mesh_name] (const auto &mapping) {
                           return mapping.first != mesh_name && mapping.second == mesh_name;
                       });
        if (!claimed_as_route_alias) {
            channel_runtime.bind_spot_mesh_transport (mesh_name, send_to_spot, request_to_spot);
        }
        if (const auto route = spot_router_channels.find (mesh_name);
            route != spot_router_channels.end () && route->second != mesh_name) {
            /* SpotHandle keeps the configured routing alias opaque. RouteMesh
             * owns the physical MeshNode, so that alias must select the same
             * node instead of a different MeshNode that happens to use the
             * alias as its MeshName. */
            channel_runtime.bind_spot_mesh_transport (route->second, std::move (send_to_spot),
                                                      std::move (request_to_spot));
        }
        struct send_mesh_node_t
        {
            static task_t<result_t<void>>
            submit (std::decay_t<decltype (mesh)> mesh,
                    std::decay_t<decltype (mesh_node_service)> mesh_node_service,
                    zlink::routing_id_t target,
                    runtime::messaging::message_parts_t parts,
                    const std::optional<zlink::routing_id_t> &local_rid)
            {
                auto submitted = local_rid && *local_rid == target
                                   ? mesh_node_service->submit_local_node_send (
                                       mesh, std::move (parts).take_items ())
                                   : mesh->send_to_node (target, std::move (parts).take_items ());
                return detail::project_task_terminal<result_t<void>> (
                  submitted,
                  [mesh = std::move (mesh), mesh_node_service = std::move (mesh_node_service)] (
                    const result_t<zlink::submit_result_t> &result) {
                      return result_t<result_t<void>>::success (
                        one_way_native_submit_result (result.value (), "MeshNode send"));
                  });
            }

            static task_t<result_t<void>>
            resume (std::decay_t<decltype (mesh)> mesh,
                    std::decay_t<decltype (mesh_node_service)> mesh_node_service,
                    zlink::routing_id_t target,
                    runtime::messaging::message_parts_t parts,
                    task_t<std::optional<zlink::routing_id_t>> identity)
            {
                const auto local_rid = co_await identity;
                co_return co_await submit (std::move (mesh), mesh_node_service, target,
                                           std::move (parts), local_rid);
            }

            static task_t<result_t<void>>
            run (std::decay_t<decltype (mesh)> mesh,
                 std::decay_t<decltype (mesh_node_service)> mesh_node_service,
                 zlink::routing_id_t target,
                 runtime::messaging::message_parts_t parts)
            {
                try {
                    auto identity = mesh->routing_id_async ();
                    if (!identity.await_ready ())
                        return resume (std::move (mesh), mesh_node_service, target,
                                       std::move (parts), std::move (identity));
                    return submit (std::move (mesh), mesh_node_service, target, std::move (parts),
                                   identity.await_resume ());
                }
                catch (...) {
                    return task_t<result_t<void>> (
                      detail::current_exception_result<result_t<void>> ());
                }
            }
        };
        channel_runtime.bind_mesh_node_transport (
          mesh_name,
          [mesh, mesh_node_service] (
            zlink::routing_id_t target,
            runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
              return send_mesh_node_t::run (mesh, mesh_node_service, target, std::move (parts));
          },
          [mesh] (
            const zlink::routing_id_t &target, runtime::messaging::message_parts_t parts,
            std::chrono::milliseconds timeout,
            detail::request_receiver_t<result_t<runtime::messaging::message_parts_t>> receiver) {
              mesh->request_node_reply (target, std::move (parts), timeout, std::move (receiver));
          });
        for (const auto &channel_name : mesh->channel_names ()) {
            channel_runtime.bind_mesh_channel_transport (
              channel_name,
              [mesh,
               channel_name] (runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
                  const auto submitted =
                    co_await mesh->send_to_channel (channel_name, parts.items ());
                  co_return one_way_native_submit_result (submitted, "RouteMesh channel send");
              },
              [mesh, channel_name] (runtime::messaging::message_parts_t parts,
                                    std::chrono::milliseconds timeout)
                -> request_task_t<result_t<runtime::messaging::message_parts_t>> {
                  return request_mesh_channel_from_app_task (mesh, channel_name, std::move (parts),
                                                             timeout);
              });
        }
    }
    if (location_runtime && !mesh_nodes.empty ()) {
        const auto application_mesh_it =
          std::find_if (mesh_nodes.begin (), mesh_nodes.end (), [&] (const auto &mesh) {
              return mesh->mesh_name () == application_mesh_name;
          });
        const auto application_mesh =
          application_mesh_it != mesh_nodes.end () ? *application_mesh_it : mesh_nodes.front ();
        auto actor_manager = _state->services.build_provider ().get_required<actor_manager_t> ();
        auto *serializers = &_state->serializers;
        const auto request_timeout = std::chrono::seconds (30);
        const auto stream_runtime = detail::stream_runtime_t::from (_state->zlink);
        actor_gateway_runtime.on_create (
          [actor_manager,
           serializers] (std::string actor_type, std::string actor_id,
                         const std::optional<zlink::message_t> &creation_payload) mutable {
              auto call = actor_manager.get_or_create (actor_id_t (std::move (actor_id)),
                                                       std::move (actor_type));
              if (creation_payload)
                  call.creation_request (message_t::from_raw (*creation_payload, serializers));
              const auto created = call.async ().result ();
              if (!created)
                  return result_t<actor_ref_t>::failure (
                    created.error_kind (),
                    created.error () ? created.error ()->what () : "Actor creation failed");
              if (const auto *existing = std::get_if<actor_create_existing_t> (&created.value ()))
                  return result_t<actor_ref_t>::success (existing->actor);
              if (const auto *new_actor = std::get_if<actor_create_created_t> (&created.value ()))
                  return result_t<actor_ref_t>::success (new_actor->actor);
              return result_t<actor_ref_t>::failure (framework_error_kind_t::rejected,
                                                     "Actor creation was rejected");
          });
        detail::configure_app_actor_gateway (
          _state.get (), options, actor_gateway_runtime, mesh_node_service, application_mesh,
          weak_mesh_nodes, stream_runtime, request_timeout, application_mesh_registration);
    }
    if (!_state->services.contains (std::type_index (typeid (actor_client_t)))) {
        _state->services.add_factory<actor_client_t> (
          [mesh_nodes, actor_location_observer,
           admission = detail::channel_runtime_t::from (_state->zlink.message_bus ()),
           location_options = options.location_options ()] (service_provider_t &provider) mutable {
              return runtime::make_actor_client (
                provider.get_required<runtime::live_location_reader_t> (),
                provider.get_required<serializer_registry_t> (), mesh_nodes,
                actor_location_observer, location_options,
                [admission] { admission.admit_outbound (); });
          },
          service_lifetime_t::singleton);
    }
    /* endpoint_connections live attach (CONN-001): client-channel handles
     * mutate the runtime connection bundle from now on; disconnects apply to
     * the same set the requests iterate. */
    {
        auto channel_state = detail::channel_runtime_t::from (_state->zlink.message_bus ());
        for (auto &[connections_channel, connections] : options.client_endpoint_connections ()) {
            detail::endpoint_connections_runtime_t::attach (
              connections,
              [channel_state, connections_channel] (const std::string &endpoint) mutable {
                  channel_state.add_client_manual_connection (connections_channel, endpoint);
              },
              [channel_state, connections_channel] (const std::string &endpoint) mutable {
                  channel_state.remove_client_manual_connection (connections_channel, endpoint);
              });
        }
        for (auto &[connections_channel, connections] :
             options.subscriber_endpoint_connections ()) {
            detail::endpoint_connections_runtime_t::attach (
              connections,
              [channel_state, connections_channel] (const std::string &endpoint) mutable {
                  channel_state.add_subscriber_manual_connection (connections_channel, endpoint);
              },
              [channel_state, connections_channel] (const std::string &endpoint) mutable {
                  channel_state.remove_subscriber_manual_connection (connections_channel, endpoint);
              });
        }
    }
    const auto read_framework_state = [state = _state.get ()] {
        return state->runtime_state.load (std::memory_order_acquire);
    };
    std::shared_ptr<runtime::client_server::client_server_location_runtime_t> client_server_runtime;
    if (!_state->services.contains (std::type_index (typeid (client_server_runtime_t)))) {
        auto provider = _state->services.build_provider ();
        const auto locations = provider.get<runtime::location_runtime_t> ();
        const auto store = provider.get<location_repository_t> ();
        client_server_runtime =
          std::make_shared<runtime::client_server::client_server_location_runtime_t> (
            _state->zlink.message_bus (), channel_snapshot,
            locations ? &locations->get () : nullptr, store ? &store->get () : nullptr,
            store ? &store->get () : nullptr, provider, _state->serializers, _state->handlers,
            read_framework_state, options.runtime_client_server_advertise_hosts (),
            _state->listener_statuses, _state->application_job_queue);
        _state->services.add_factory<client_server_runtime_t> (
          [owner = std::weak_ptr (client_server_runtime)] (service_provider_t &) {
              return std::static_pointer_cast<client_server_runtime_t> (
                detail::lock_runtime_owner (owner));
          },
          service_lifetime_t::transient);
    }
    std::shared_ptr<runtime::fanout::fanout_location_runtime_t> fanout_runtime;
    const auto needs_fanout_runtime =
      std::any_of (channel_snapshot.begin (), channel_snapshot.end (), [] (const auto &channel) {
          return (channel.publisher.enabled && channel.publisher.discovery)
                 || (channel.subscriber.enabled && channel.subscriber.discovery);
      });
    if (location_runtime && needs_fanout_runtime
        && !_state->services.contains (std::type_index (typeid (fanout_runtime_t)))) {
        auto provider = _state->services.build_provider ();
        fanout_runtime = std::make_shared<runtime::fanout::fanout_location_runtime_t> (
          _state->zlink.message_bus (), channel_snapshot,
          provider.get_required<runtime::location_runtime_t> (),
          provider.get_required<location_repository_t> (),
          provider.get_required<location_repository_t> (), provider, _state->serializers,
          _state->handlers, read_framework_state, detail::handler_invocation_executor (),
          options.runtime_fanout_advertise_hosts (), _state->listener_statuses,
          _state->application_job_queue);
        _state->services.add_factory<fanout_runtime_t> (
          [owner = std::weak_ptr (fanout_runtime)] (service_provider_t &) {
              return std::static_pointer_cast<fanout_runtime_t> (
                detail::lock_runtime_owner (owner));
          },
          service_lifetime_t::transient);
    }
    auto auto_connect_service = std::make_unique<runtime::location_auto_connect_host_service_t> (
      _state->monitoring->runtime_failures, _state->zlink.message_bus (), channel_snapshot,
      _state->handlers, _state->serializers, read_framework_state,
      options.runtime_client_server_advertise_hosts (), options.runtime_fanout_advertise_hosts (),
      options.route_mesh_client_channels (), mesh_nodes,
      [mesh_node_service] (location_write_intent_t intent, std::stop_token cancellation,
                           std::chrono::steady_clock::time_point deadline) {
          return mesh_node_service ? mesh_node_service->republish_after_store_recovery (
                                       intent, cancellation, deadline)
                                   : task_t<bool> (result_t<bool>::success (true));
      },
      std::move (client_server_runtime), std::move (fanout_runtime), _state->listener_statuses);
    _state->startup_publication = [service = auto_connect_service.get ()] {
        return service->descriptors_published ();
    };
    add_hosted_service (std::move (auto_connect_service));
    if (detail::has_inbound_channel (channel_snapshot)) {
        add_hosted_service (std::make_unique<runtime::channel_host_service_t> (
          _state->monitoring->runtime_failures, _state->zlink.message_bus (), channel_snapshot,
          _state->handlers, _state->serializers, options.runtime_client_server_advertise_hosts (),
          _state->application_job_queue, _state->listener_statuses));
    }
    if (!stream_snapshot.empty ()) {
        detail::configure_stream_dispatch_executor ();
        auto stream_runtime = detail::stream_runtime_t::from (_state->zlink);
        auto stream_service = std::make_unique<runtime::stream_host_service_t> (
          _state->monitoring->runtime_failures, stream_runtime, stream_snapshot,
          options.stream_session_factories (), options.session_replacement_callback_timeout (),
          mesh_nodes.empty () ? nullptr : mesh_nodes.front (), stream_runtime.advertise_hosts (),
          _state->listener_statuses, _state->application_job_queue, _state->host_capacity);
        stream_service->bind_drain_flag (_state->draining);
        stream_service->bind_shutdown_cancellation (
          _state->termination_operation.cleanup_cancellation.get_token ());
        stream_service->bind_delay_owner (_state->delays);
        stream_service->bind_monitoring (_state->monitoring);
        if (queue_settings) {
            const auto queue_mesh = std::ranges::find_if (mesh_nodes, [&] (const auto &node) {
                return node->mesh_name () == queue_settings->mesh_name;
            });
            stream_service->bind_admission_queue (
              std::make_shared<runtime::admission_queue::queue_node_t> (
                std::move (*queue_settings), location_provider.get_required<route_client_t> (),
                *queue_mesh, *location_runtime));
        }
        add_hosted_service (std::move (stream_service));
    }
    if (!http_snapshot.endpoints.empty ()) {
        add_hosted_service (std::make_unique<runtime::http_host_service_t> (
          http_snapshot, _state->health, options.handler_coroutine_workers (),
          _state->listener_statuses));
    }
    // Runtime services and application services share this registry. Validate
    // the completed catalog before any hosted service starts accepting messages.
    for (const auto &registration : callback_registrations) {
        registration->lane
          .run ([&] {
              for (const auto &[name, lifecycle] : registration->spot_state->spot_lifecycles) {
                  if (lifecycle.validate_dependencies)
                      lifecycle.validate_dependencies (_state->services);
              }
          })
          .get ();
    }
    detail::configure_handler_invocation_executor ();
    if (_state->framework_hosted_service_position) {
        const auto position = *_state->framework_hosted_service_position;
        std::rotate (_state->hosted_services.begin () + static_cast<std::ptrdiff_t> (position),
                     _state->hosted_services.begin ()
                       + static_cast<std::ptrdiff_t> (first_framework_service),
                     _state->hosted_services.end ());
    }
    _state->framework_applied = true;
}

} // namespace zlink::framework

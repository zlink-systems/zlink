/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/configuration/app.hpp>
#include <runtime/locations/location_repository.hpp>

#include "runtime/actors/actor_client.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/host/framework_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/http/http_host_service.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_host_service.hpp"
#include "runtime/locations/location_lifecycle.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/configuration/endpoint_connections.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/host/relocation_target_eligibility.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/streams/stream_host_service.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::detail
{
void configure_handler_invocation_executor ();
void shutdown_handler_invocation_executor () noexcept;
} // namespace zlink::framework::detail

namespace zlink::framework::detail
{

std::vector<zlink::message_t> encode_bound_session_frame (const stream_runtime_t &runtime,
                                                          const stream_header_t &header,
                                                          const zlink::message_t &payload)
{
    auto encoded_frame = runtime.encode_frame (header, payload);
    if (!encoded_frame)
        throw framework_exception_t (encoded_frame.error_kind (),
                                     encoded_frame.error () ? encoded_frame.error ()->what ()
                                                            : "bound Session frame encode failed");
    return {zlink::message_t::from (std::move (encoded_frame.value ()))};
}

std::pair<stream_header_t, zlink::message_t>
decode_bound_session_frame (const stream_runtime_t &runtime,
                            const std::vector<zlink::message_t> &parts)
{
    if (parts.size () != 1)
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "bound Session frame must contain one part");
    const auto bytes = parts.front ().to_bytes ();
    if (bytes.size () < 6)
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "bound Session frame prefix is incomplete");
    const auto header_size =
      (static_cast<std::size_t> (bytes[0]) << 8) | static_cast<std::size_t> (bytes[1]);
    const auto payload_size =
      (static_cast<std::size_t> (bytes[2]) << 24) | (static_cast<std::size_t> (bytes[3]) << 16)
      | (static_cast<std::size_t> (bytes[4]) << 8) | static_cast<std::size_t> (bytes[5]);
    if (header_size > bytes.size () - 6 || payload_size != bytes.size () - 6 - header_size)
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "bound Session frame size does not match its prefix");
    std::vector<std::uint8_t> header_bytes (bytes.begin () + 6, bytes.begin () + 6 + header_size);
    const auto header = runtime.decode_header (header_bytes);
    if (!header)
        throw framework_exception_t (header.error_kind (),
                                     header.error () ? header.error ()->what ()
                                                     : "bound Session header decode failed");
    return {header.value (), zlink::message_t::from (std::vector<std::uint8_t> (
                               bytes.begin () + 6 + header_size, bytes.end ()))};
}

class session_ingress_completion_t final
{
  public:
    session_ingress_completion_t () = default;
    ~session_ingress_completion_t () noexcept
    {
        if (_registry && _dispatch)
            (void) _registry->complete_inbound (*_dispatch);
    }

    session_ingress_completion_t (const session_ingress_completion_t &) = delete;
    session_ingress_completion_t &operator= (const session_ingress_completion_t &) = delete;

    void arm (runtime::stateful::stream_session_registry_t &registry,
              runtime::stateful::stream_dispatch_t dispatch)
    {
        _registry = &registry;
        _dispatch = std::move (dispatch);
    }

    runtime::stateful::stateful_error_t complete () noexcept
    {
        if (!_registry || !_dispatch)
            return runtime::stateful::stateful_error_t::none;
        auto dispatch = std::exchange (_dispatch, std::nullopt);
        return _registry->complete_inbound (*dispatch);
    }

  private:
    runtime::stateful::stream_session_registry_t *_registry = nullptr;
    std::optional<runtime::stateful::stream_dispatch_t> _dispatch;
};

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
        auto read = _store.read_authority (runtime::actor_authority_key (actor_id)).result ();
        if (!read) {
            return task_t<std::optional<actor_ref_t>> (
              detail::propagate_failure<std::optional<actor_ref_t>> (
                read, "actor authority lookup failed"));
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        const auto projection = snapshot ? runtime::decode_actor_authority_payload (
                                             snapshot->payload, snapshot->object_generation)
                                         : std::nullopt;
        if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor || !projection
            || projection->actor.actor_id ().value () != actor_id) {
            return task_t<std::optional<actor_ref_t>> (
              result_t<std::optional<actor_ref_t>>::success (std::nullopt));
        }
        return task_t<std::optional<actor_ref_t>> (
          result_t<std::optional<actor_ref_t>>::success (projection->actor));
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

        std::lock_guard<std::recursive_mutex> lock (registration->spot_state->mutex);
        if (!registration->spot_state->snapshot.instance_spot_names.empty ()) {
            requires_relocation_store = true;
            break;
        }
        const auto relocatable_actor =
          std::any_of (registration->spot_state->actor_factories.begin (),
                       registration->spot_state->actor_factories.end (), [] (const auto &factory) {
                           const auto policy = factory.second.relocation.kind;
                           return policy == factory_relocation_kind_t::recreate
                                  || policy == factory_relocation_kind_t::preserve_state;
                       });
        const auto relocatable_spot = std::any_of (
          registration->spot_state->spot_factory_relocations.begin (),
          registration->spot_state->spot_factory_relocations.end (), [] (const auto &factory) {
              const auto policy = factory.second.kind;
              return policy == factory_relocation_kind_t::recreate
                     || policy == factory_relocation_kind_t::preserve_state;
          });
        if (relocatable_actor || relocatable_spot) {
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

class app_state_t;

struct app_state_access_t
{
    mutable std::shared_mutex mutex;
    app_state_t *state = nullptr;
};

runtime::hosted_service_lifecycle_t *lifecycle_of (hosted_service_t *service) noexcept
{
    return dynamic_cast<runtime::hosted_service_lifecycle_t *> (service);
}

class app_state_t
{
  public:
    app_state_t () :
        status_access (std::make_shared<app_state_access_t> ()),
        monitoring (std::make_shared<monitoring_runtime_state_t> ()),
        monitoring_builder (monitoring),
        listener_statuses (std::make_shared<runtime::listener_status_registry_t> ())
    {
        status_access->state = this;
    }
    ~app_state_t ()
    {
        if (application_job_queue)
            application_job_queue->stop ();
        const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
        const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                                   && std::string_view (trace_value) != "";
        if (trace_enabled) {
            std::cerr << "zlink-cpp-host-stop stage=before-app-state-destroy-services" << std::endl;
        }
        hosted_services.clear ();
        {
            std::unique_lock lock (status_access->mutex);
            status_access->state = nullptr;
        }
        if (trace_enabled) {
            std::cerr << "zlink-cpp-host-stop stage=after-app-state-destroy-services" << std::endl;
        }
    }

    void start_hosted_services (service_provider_t &provider,
                                std::vector<hosted_service_t *> &started)
    {
        for (const auto &service : hosted_services) {
            service->start (provider);
            started.push_back (service.get ());
        }
    }

    void stop_hosted_services (const std::vector<hosted_service_t *> &started) noexcept
    {
        const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
        const bool trace_enabled = trace_value != nullptr && std::string_view (trace_value) != "0"
                                   && std::string_view (trace_value) != "";
        auto stop_service = [trace_enabled] (hosted_service_t *service) {
            if (trace_enabled) {
                std::cerr << "zlink-cpp-host-stop stage=before service="
                          << typeid (*service).name () << std::endl;
            }
            service->stop ();
            if (trace_enabled) {
                std::cerr << "zlink-cpp-host-stop stage=after service=" << typeid (*service).name ()
                          << std::endl;
            }
        };
        std::vector<hosted_service_t *> request_order (started.rbegin (), started.rend ());
        std::stable_sort (request_order.begin (), request_order.end (),
                          [] (auto *left, auto *right) {
                              const auto *left_lifecycle = lifecycle_of (left);
                              const auto *right_lifecycle = lifecycle_of (right);
                              const auto left_priority =
                                left_lifecycle ? left_lifecycle->shutdown_request_priority () : 0;
                              const auto right_priority =
                                right_lifecycle ? right_lifecycle->shutdown_request_priority () : 0;
                              return left_priority > right_priority;
                          });
        for (auto *service : request_order)
            service->request_stop ();

        std::vector<hosted_service_t *> stop_order (started.rbegin (), started.rend ());
        std::stable_sort (stop_order.begin (), stop_order.end (), [] (auto *left, auto *right) {
            const auto *left_lifecycle = lifecycle_of (left);
            const auto *right_lifecycle = lifecycle_of (right);
            const auto left_priority =
              left_lifecycle ? left_lifecycle->shutdown_stop_priority () : 0;
            const auto right_priority =
              right_lifecycle ? right_lifecycle->shutdown_stop_priority () : 0;
            return left_priority > right_priority;
        });
        for (auto *service : stop_order)
            stop_service (service);
    }

    template <typename TResult>
    struct lifecycle_waiter_t : public std::enable_shared_from_this<lifecycle_waiter_t<TResult>>
    {
        task_completion_source_t<TResult> completion;
        std::atomic_bool completed = false;
        std::optional<std::stop_callback<std::function<void ()>>> cancellation;

        task_t<TResult> task () { return completion.task (); }

        void arm (std::stop_token token)
        {
            if (!token.stop_possible ())
                return;
            std::weak_ptr<lifecycle_waiter_t<TResult>> weak = this->shared_from_this ();
            cancellation.emplace (token, [weak] {
                if (auto waiter = weak.lock ())
                    waiter->cancel ();
            });
        }

        void complete (TResult result)
        {
            if (completed.exchange (true, std::memory_order_acq_rel))
                return;
            completion.complete (result_t<TResult>::success (std::move (result)));
        }

        void cancel ()
        {
            if (completed.exchange (true, std::memory_order_acq_rel))
                return;
            completion.complete (detail::boundary_failure<TResult> (
              detail::boundary_error_t::cancelled, "lifecycle waiter was cancelled"));
        }
    };

    using relocation_waiter_t = lifecycle_waiter_t<relocation_result_t>;
    using termination_waiter_t = lifecycle_waiter_t<termination_result_t>;

    struct relocation_operation_t
    {
        std::mutex mutex;
        bool started = false;
        bool terminal = false;
        bool shutdown_requested = false;
        relocation_options_t options{};
        relocation_result_t result{};
        std::int64_t source_application_version = 0;
        std::chrono::milliseconds deadline{30000};
        std::optional<std::chrono::system_clock::time_point> deadline_at;
        std::vector<std::shared_ptr<relocation_waiter_t>> waiters;
        std::thread worker;

        ~relocation_operation_t ()
        {
            if (worker.joinable ())
                worker.join ();
        }
    };

    struct termination_operation_t
    {
        std::mutex mutex;
        bool started = false;
        bool terminal = false;
        termination_result_t result{};
        std::chrono::milliseconds deadline{30000};
        std::optional<std::chrono::system_clock::time_point> deadline_at;
        std::vector<std::shared_ptr<termination_waiter_t>> waiters;
        std::thread worker;

        ~termination_operation_t ()
        {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    };

    std::shared_ptr<std::atomic_bool> draining = std::make_shared<std::atomic_bool> (false);
    std::atomic<framework_runtime_state_t> runtime_state = framework_runtime_state_t::preparing;
    std::shared_ptr<app_state_access_t> status_access;
    relocation_operation_t relocation_operation;
    termination_operation_t termination_operation;
    std::mutex termination_teardown_mutex;
    std::condition_variable termination_teardown_changed;
    bool run_active = false;
    bool teardown_complete = false;

    service_collection_t services;
    handler_registry_t handlers;
    config_builder_t config;
    logging_builder_t logging;
    std::shared_ptr<monitoring_runtime_state_t> monitoring;
    monitoring_builder_t monitoring_builder;
    std::shared_ptr<runtime::application_job_queue_t> application_job_queue;
    std::shared_ptr<runtime::host_capacity_runtime_t> host_capacity;
    std::shared_ptr<runtime::listener_status_registry_t> listener_statuses;
    health_builder_t health;
    zlink_builder_t zlink;
    serializer_registry_t serializers;
    std::vector<std::unique_ptr<hosted_service_t>> hosted_services;
    std::vector<std::shared_ptr<mesh_node_runtime_t>> route_mesh_nodes;
    std::function<bool ()> has_manual_service_topology;
    std::atomic_bool stop_requested = false;
    int exit_code = 0;
    // Shared, runtime-mutable message-flow mode (set_message_flow_mode). Created
    // once here (never reassigned) so concurrent set/apply only touch the atomic,
    // not the shared_ptr. Installed into dispatch options at apply.
    std::shared_ptr<std::atomic<message_flow_log_mode_t>> message_flow_mode =
      std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::errors);
};

namespace
{

using framework_observer_state_t =
  observation_detail::runtime_observer_state_t<framework_runtime_status_t>;

bool same_runtime_status (const framework_runtime_status_t &left,
                          const framework_runtime_status_t &right)
{
    return left.state == right.state && left.is_ready == right.is_ready
           && left.accepting_work == right.accepting_work
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

    framework_runtime_status_t snapshot () const
    {
        std::shared_lock state_lock (_access->mutex);
        auto *state = _access->state;
        if (!state) {
            std::lock_guard lock (_mutex);
            return _last.value_or (framework_runtime_status_t{});
        }
        framework_runtime_status_t result;
        result.state = state->runtime_state.load (std::memory_order_acquire);
        result.is_ready = result.state == framework_runtime_state_t::serving;
        result.accepting_work = result.state == framework_runtime_state_t::serving;
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
        result.observed_at = std::chrono::system_clock::now ();
        {
            std::lock_guard lock (_mutex);
            if (!_last || !same_runtime_status (*_last, result))
                ++_sequence;
            result.sequence = _sequence;
            _last = result;
        }
        return result;
    }

    bool closed () const noexcept { return _closed.load (std::memory_order_acquire); }

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
    void run () noexcept;

    std::shared_ptr<app_state_access_t> _access;
    mutable std::mutex _mutex;
    mutable std::optional<framework_runtime_status_t> _last;
    mutable std::uint64_t _sequence = 0;
    std::atomic_bool _closed{false};
    std::condition_variable _changed;
    std::mutex _observers_mutex;
    std::vector<std::weak_ptr<framework_observer_state_t>> _observers;
    std::thread _worker;
};

std::shared_ptr<framework_observer_state_t> framework_runtime_status_source_t::observe (
  std::size_t capacity,
  std::function<void (const observed_status_t<framework_runtime_status_t> &)> observer)
{
    auto state = std::make_shared<framework_observer_state_t> (capacity, std::move (observer));
    state->start ();
    {
        std::lock_guard lock (_observers_mutex);
        _observers.erase (std::remove_if (_observers.begin (), _observers.end (),
                                          [] (const auto &item) { return item.expired (); }),
                          _observers.end ());
        _observers.emplace_back (state);
    }
    auto initial = snapshot ();
    const bool terminal = initial.state == framework_runtime_state_t::stopped
                          || initial.state == framework_runtime_state_t::error;
    state->enqueue ("framework-runtime", std::move (initial), terminal);
    return state;
}

void framework_runtime_status_source_t::run () noexcept
{
    std::uint64_t last_sequence = 0;
    while (!_closed.load (std::memory_order_acquire)) {
        auto status = snapshot ();
        if (status.sequence != last_sequence) {
            last_sequence = status.sequence;
            std::vector<std::shared_ptr<framework_observer_state_t>> observers;
            {
                std::lock_guard lock (_observers_mutex);
                for (auto iterator = _observers.begin (); iterator != _observers.end ();) {
                    if (auto observer = iterator->lock ()) {
                        observers.push_back (std::move (observer));
                        ++iterator;
                    } else {
                        iterator = _observers.erase (iterator);
                    }
                }
            }
            const bool terminal = status.state == framework_runtime_state_t::stopped
                                  || status.state == framework_runtime_state_t::error;
            for (const auto &observer : observers)
                observer->enqueue ("framework-runtime", status, terminal);
        }
        std::unique_lock lock (_observers_mutex);
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

volatile std::sig_atomic_t g_stop_signal_requested = 0;

void handle_process_signal (int) noexcept
{
    g_stop_signal_requested = 1;
}

bool host_stop_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    return value != nullptr && std::string_view (value) != "0" && std::string_view (value) != "";
}

struct instance_spot_activation_trace_context_t
{
    std::string packet_name;
    std::string mesh_name;
    zlink::routing_id_t target_node;
    zlink::framework::spot_id_t spot_id;
    std::string instance_spot_type;
    std::string correlation_id;
    std::uint64_t source_node_generation = 0;
};

std::optional<instance_spot_activation_trace_context_t>
make_instance_spot_activation_trace_context (
  bool may_emit,
  std::string_view packet_name,
  std::string_view mesh_name,
  const zlink::routing_id_t &target_node,
  const zlink::framework::spot_id_t &spot_id,
  const zlink::framework::runtime::protocol::instance_spot_activation_header_t &header)
{
    if (!may_emit)
        return std::nullopt;
    return instance_spot_activation_trace_context_t{std::string (packet_name),
                                                    std::string (mesh_name),
                                                    target_node,
                                                    spot_id,
                                                    header.target.stable_type,
                                                    std::to_string (header.operation.high) + ":"
                                                      + std::to_string (header.operation.low),
                                                    header.source_node_generation};
}

void trace_instance_spot_activation (
  const zlink::framework::dispatch_options_t &dispatch,
  const std::optional<zlink::framework::runtime::flow_value_t> &flow,
  zlink::framework::message_flow_outcome_t outcome,
  zlink::framework::dispatch_message_kind_t message_kind,
  const instance_spot_activation_trace_context_t &context,
  std::optional<zlink::framework::message_flow_result_t> result = std::nullopt,
  std::optional<zlink::framework::message_flow_reason_t> reason = std::nullopt)
{
    zlink::framework::detail::message_flow_tracer_t (dispatch).trace (outcome, result, [&] {
        zlink::framework::message_flow_event_t event{
          outcome,
          zlink::framework::dispatch_error_surface_t::instance_spot,
          message_kind,
          context.packet_name,
          std::nullopt,
          std::nullopt,
          context.correlation_id,
          std::nullopt,
          std::string (context.spot_id),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::exception_ptr{},
          std::nullopt,
          std::nullopt};
        if (flow && !flow->flow_id.empty ()) {
            event.flow_id = flow->flow_id;
            event.flow_origin = flow->origin;
        }
        event.mesh_name = context.mesh_name;
        event.target_rid = context.target_node.to_string ();
        event.instance_spot_type = context.instance_spot_type;
        event.activation_state =
          outcome == zlink::framework::message_flow_outcome_t::reply_received
              && (!result || *result == zlink::framework::message_flow_result_t::succeeded)
            ? "ready"
            : "activating";
        event.source_mesh_generation = context.source_node_generation;
        if (result)
            event.result = *result;
        if (reason)
            event.reason = *reason;
        return event;
    });
}

zlink::framework::result_t<void> one_way_native_submit_result (zlink::submit_result_t result,
                                                               std::string_view operation)
{
    using namespace zlink::framework;
    switch (result) {
        case zlink::submit_result_t::ok:
            return result_t<void>::success ();
        case zlink::submit_result_t::backpressured:
            // Backpressure is never a public terminal: it completes at the send
            // deadline as DeadlineExceeded (spec 05-async-execution-policy;
            // 32-framework-error-model:70), represented as a timeout boundary
            // like a request timeout — matching submit_result_mapper.
            return detail::boundary_failure<void> (detail::boundary_error_t::timed_out,
                                                   std::string (operation) + " is backpressured");
        case zlink::submit_result_t::not_found:
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            std::string (operation) + " target was not found");
        case zlink::submit_result_t::not_admitted:
            return result_t<void>::failure (framework_error_kind_t::rejected,
                                            std::string (operation) + " admission was rejected");
        case zlink::submit_result_t::not_connected:
            return result_t<void>::failure (framework_error_kind_t::unavailable,
                                            std::string (operation) + " route is not connected");
        case zlink::submit_result_t::terminated:
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                                   std::string (operation) + " runtime is stopped");
        case zlink::submit_result_t::invalid_argument:
        case zlink::submit_result_t::invalid_handle:
        case zlink::submit_result_t::invalid_state:
        case zlink::submit_result_t::thread_violation:
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            std::string (operation) + " rejected an invalid call");
        case zlink::submit_result_t::not_supported:
        case zlink::submit_result_t::out_of_memory:
        case zlink::submit_result_t::seq_exhausted:
        case zlink::submit_result_t::internal_error:
            return result_t<void>::failure (
              runtime::messaging::map_submit_result_error_kind (result),
              std::string (operation) + " was not submitted");
        default:
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            std::string (operation) + " was not submitted");
    }
}

zlink::framework::task_t<
  zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>>
await_mesh_request_completion (zlink::framework::detail::mesh_node_runtime_t &mesh,
                               const zlink::framework::detail::host::call_id_t &operation,
                               std::string_view operation_name)
{
    using namespace zlink::framework;
    try {
        auto completion = co_await mesh.await_completion (operation);
        if (completion.record.terminal_result != static_cast<int> (zlink::request_result_t::ok)) {
            co_return detail::result_access_t::failure<
              zlink::framework::runtime::messaging::message_parts_t> (
              zlink::framework::runtime::messaging::request_failure_mapper_t{}
                .reply_header_exception (
                  static_cast<std::uint32_t> (completion.record.terminal_result),
                  static_cast<std::uint32_t> (completion.record.failure_errno),
                  std::string (operation_name)));
        }
        co_return result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
          zlink::framework::runtime::messaging::message_parts_t (std::move (completion.parts)));
    }
    catch (const framework_exception_t &error) {
        co_return
          detail::result_access_t::failure<zlink::framework::runtime::messaging::message_parts_t> (
            error);
    }
}

} // namespace

namespace zlink::framework
{

app_t::app_t () : _state (std::make_unique<detail::app_state_t> ())
{
}

app_t::~app_t ()
{
}

app_t::app_t (app_t &&) noexcept = default;

app_t &app_t::operator= (app_t &&) noexcept = default;

app_t app_t::create ()
{
    return {};
}

app_advanced_t::app_advanced_t (app_t &app) noexcept : _app (&app)
{
}

service_collection_t &app_advanced_t::services () noexcept
{
    return _app->_services ();
}

handler_registry_t &app_advanced_t::handlers () noexcept
{
    return _app->_handlers ();
}

zlink_builder_t &app_advanced_t::zlink () noexcept
{
    return _app->_zlink_builder ();
}

config_builder_t &app_t::config () noexcept
{
    return _state->config;
}

logging_builder_t &app_t::logging () noexcept
{
    return _state->logging;
}

monitoring_builder_t &app_t::monitoring () noexcept
{
    return _state->monitoring_builder;
}

app_t &app_t::set_message_flow_mode (message_flow_log_mode_t mode) noexcept
{
    // Note: before apply() this is overwritten by the configured mode (config seeds
    // at apply); the intended use is runtime toggling after the app is running.
    _state->message_flow_mode->store (mode, std::memory_order_relaxed);
    return *this;
}

message_flow_log_mode_t app_t::message_flow_mode () const noexcept
{
    return _state->message_flow_mode->load (std::memory_order_relaxed);
}

health_builder_t &app_t::health () noexcept
{
    return _state->health;
}

app_advanced_t app_t::advanced () noexcept
{
    return app_advanced_t (*this);
}

service_collection_t &app_t::_services () noexcept
{
    return _state->services;
}

handler_registry_t &app_t::_handlers () noexcept
{
    return _state->handlers;
}

zlink_builder_t &app_t::_zlink_builder () noexcept
{
    return _state->zlink;
}

serializer_registry_t &app_t::_serializers () noexcept
{
    return _state->serializers;
}

app_t &app_t::add_zlink_framework (std::function<void (zlink_framework_options_t &)> configure)
{
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
    zlink_framework_options_t options (_state->services, _state->handlers, _state->serializers,
                                       _state->zlink);
    if (configure) {
        configure (options);
    }
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
    const auto http_snapshot = options.http ().snapshot ();
    const auto job_queue_configuration = runtime::resolve_application_job_queue_configuration (
      options.application_job_queue_profile (), options.max_queued_application_jobs (),
      runtime::detect_application_job_queue_processor_limits (
        options.handler_coroutine_workers () == 0
          ? std::nullopt
          : std::optional<std::uint32_t> (static_cast<std::uint32_t> (std::min<std::size_t> (
              options.handler_coroutine_workers (), std::numeric_limits<std::uint32_t>::max ())))));
    auto core_context = std::make_shared<zlink::context_t> ();
    auto core_options = core_context->options ();
    core_options.auto_hwm_enabled (true);
    core_options.core_hwm_profile (
      static_cast<zlink::auto_hwm_profile> (options.core_hwm_profile ()));
    if (options.core_hwm_budget_bytes () != 0) {
        core_options.core_hwm_budget_bytes (
          zlink::byte_count_t::bytes (options.core_hwm_budget_bytes ()));
    } else if (options.core_hwm_memory_limit_bytes () != 0) {
        core_options.core_hwm_memory_limit_bytes (
          zlink::byte_count_t::bytes (options.core_hwm_memory_limit_bytes ()));
    }
    detail::zlink_builder_access_t::bind_shared_core_context (_state->zlink,
                                                              std::move (core_context));
    _state->application_job_queue =
      std::make_shared<runtime::application_job_queue_t> (job_queue_configuration);
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
    if (_state->services.contains (std::type_index (typeid (relocation_store_t)))
        && !_state->services.contains (std::type_index (typeid (relocation_repository_t)))) {
        _state->services.add_factory<relocation_repository_t> (
          [] (service_provider_t &provider) {
              return std::static_pointer_cast<relocation_repository_t> (
                std::make_shared<runtime::provider_relocation_repository_t> (
                  provider.get_required<relocation_store_t> ()));
          },
          service_lifetime_t::singleton);
    }
    if (_state->services.contains (std::type_index (typeid (location_store_t)))
        && !_state->services.contains (std::type_index (typeid (location_repository_t)))) {
        _state->services.add_factory<location_repository_t> (
          [] (service_provider_t &provider) {
              return std::static_pointer_cast<location_repository_t> (
                std::make_shared<runtime::provider_location_repository_t> (
                  provider.get_required<location_store_t> ()));
          },
          service_lifetime_t::singleton);
    }
    if (_state->services.contains (std::type_index (typeid (relocation_repository_t)))
        && !_state->services.contains (
          std::type_index (typeid (runtime::stateful::relocation_store_port_t)))) {
        _state->services.add_factory<runtime::stateful::relocation_store_port_t> (
          [] (service_provider_t &provider) {
              return std::unique_ptr<runtime::stateful::relocation_store_port_t> (
                std::make_unique<runtime::stateful::public_relocation_store_adapter_t> (
                  provider.get_required<relocation_repository_t> ()));
          },
          service_lifetime_t::singleton);
    }
    if (!_state->services.contains (std::type_index (typeid (location_repository_t)))) {
        auto store = std::make_shared<runtime::in_memory_location_repository_t> ();
        _state->services.add_factory<location_repository_t> (
          [store] (service_provider_t &) {
              return std::static_pointer_cast<location_repository_t> (store);
          },
          service_lifetime_t::singleton);
    }
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
          [location_options] (service_provider_t &provider) {
              return std::make_unique<runtime::location_runtime_t> (
                provider.get_required<location_repository_t> (), location_options);
          },
          service_lifetime_t::singleton);
    }
    if (!_state->services.contains (std::type_index (typeid (runtime::live_location_reader_t)))) {
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
    const auto actor_location_observer = std::make_shared<runtime::actor_location_observer_t> ();
    const auto actor_mesh_name = std::make_shared<std::string> ();
    if (!_state->services.contains (
          std::type_index (typeid (runtime::store_location_resolvers_t)))) {
        const auto resolver_location_options = options.location_options ();
        _state->services.add_factory<runtime::store_location_resolvers_t> (
          [resolver_location_options, actor_location_observer] (service_provider_t &provider) {
              return std::make_unique<runtime::store_location_resolvers_t> (
                provider.get_required<runtime::live_location_reader_t> (),
                resolver_location_options, actor_location_observer);
          },
          service_lifetime_t::singleton);
    }
    if (!_state->services.contains (std::type_index (typeid (runtime::spot_address_resolver_t)))) {
        _state->services.add_factory<runtime::spot_address_resolver_t> (
          [] (service_provider_t &provider) {
              return std::shared_ptr<runtime::spot_address_resolver_t> (
                &provider.get_required<runtime::store_location_resolvers_t> (),
                [] (runtime::spot_address_resolver_t *) noexcept {});
          },
          service_lifetime_t::singleton);
    }
    if (!_state->services.contains (std::type_index (typeid (runtime::actor_address_resolver_t)))) {
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
    detail::bind_zlink_monitoring (_state->zlink, _state->monitoring);
    detail::bind_stream_serializers (_state->zlink, _state->serializers);
    auto &actor_gateway_runtime =
      _state->services.build_provider ().get_required<detail::actor_gateway_runtime_t> ();
    _state->services.build_provider ()
      .get_required<runtime::location_runtime_t> ()
      .bind_monitoring (_state->monitoring);
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
    const auto shared_core_context =
      detail::zlink_builder_access_t::shared_core_context (_state->zlink);
    for (const auto &registration : mesh_node_registrations)
        registration->core_context = shared_core_context;
    _state->has_manual_service_topology = [options, mesh_node_registrations,
                                           channel_runtime_manager] () mutable {
        if (!options.client_endpoint_connections ().empty ()
            || !options.subscriber_endpoint_connections ().empty ()) {
            return true;
        }
        for (const auto &registration : mesh_node_registrations) {
            std::lock_guard lock (registration->mutex);
            if (!registration->peer_connections.empty ())
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
    const auto application_mesh_registration =
      std::find_if (mesh_node_registrations.begin (), mesh_node_registrations.end (),
                    [] (const auto &registration) {
                        return registration->spot_state->snapshot.entry_spot_name.has_value ();
                    });
    const auto application_mesh_name =
      application_mesh_registration != mesh_node_registrations.end ()
        ? (*application_mesh_registration)->mesh_name
        : (mesh_node_registrations.empty () ? std::string{}
                                            : mesh_node_registrations.front ()->mesh_name);
    *actor_mesh_name = application_mesh_name;
    {
        auto provider = _state->services.build_provider ();
        channel_runtime.bind_spot_address_resolver (
          provider.get_required<runtime::spot_address_resolver_t> ());
        provider.get_required<runtime::store_location_resolvers_t> ().set_actor_mesh_name (
          application_mesh_name);
        auto &location_lifecycle = provider.get_required<runtime::location_lifecycle_t> ();
        auto &spot_resolver = provider.get_required<runtime::spot_address_resolver_t> ();
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
            spot_runtime.bind_location_lifecycle (location_lifecycle);
            spot_runtime.bind_spot_location_resolver (spot_resolver);
            spot_runtime.bind_drain_flag (_state->draining);
            spot_runtime.set_route_client (route_client);
            spot_runtime.bind_service_provider (provider);
        }
    }
    if (!mesh_node_registrations.empty ()
        && !_state->services.contains (std::type_index (typeid (spot_manager_t)))) {
        _state->services.add_singleton<spot_manager_t> (std::make_unique<spot_manager_t> (
          detail::spot_node_runtime_t (application_mesh_registration
                                           != mesh_node_registrations.end ()
                                         ? (*application_mesh_registration)->spot_state
                                         : mesh_node_registrations.front ()->spot_state)
            .manager ()));
    }
    if (!mesh_node_registrations.empty ()
        && !_state->services.contains (std::type_index (typeid (spot_publisher_client_t)))) {
        auto provider = _state->services.build_provider ();
        _state->services.add_singleton<spot_publisher_client_t> (
          std::make_unique<spot_publisher_client_t> (provider.get_required<spot_manager_t> (),
                                                     _state->serializers));
    }
    const auto location_owner = detail::location_owner_node_rid (mesh_node_registrations);
    add_hosted_service (std::make_unique<runtime::location_host_service_t> (location_owner));
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes;
    const auto callback_registrations = mesh_node_registrations;
    runtime::mesh_node_host_service_t *mesh_node_service = nullptr;
    if (!mesh_node_registrations.empty ()) {
        auto mesh_service = std::make_unique<runtime::mesh_node_host_service_t> (
          std::move (mesh_node_registrations), _state->serializers, _state->handlers,
          options.dispatch_options (), _state->listener_statuses, _state->application_job_queue);
        mesh_node_service = mesh_service.get ();
        mesh_nodes = mesh_service->nodes ();
        _state->route_mesh_nodes = mesh_nodes;
        auto provider = _state->services.build_provider ();
        auto &location_runtime = provider.get_required<runtime::location_runtime_t> ();
        auto &location_store = provider.get_required<location_repository_t> ();
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
                location_store);
        }
        for (const auto &mesh_node : mesh_nodes) {
            mesh_node->configure_session_route_owner (
              [&location_runtime] { return location_runtime.current_owner_token (); });
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
              [actor_gateway_runtime, &location_store,
               mesh_name = mesh_node->mesh_name ()] (const runtime::stateful::object_ref_t &source)
                -> std::optional<detail::bound_session_relocation_route_t> {
                  if (source.kind != runtime::stateful::object_kind_t::actor
                      || source.object_generation == 0 || source.authority_owner_generation == 0)
                      return std::nullopt;

                  const auto actor =
                    detail::actor_ref_access_t::make (node_rid_t::from_string (source.node_id), {},
                                                      source.key, source.object_generation);
                  const auto route = actor_gateway_runtime.bound_session_route (actor);
                  if (!route || !route->session_rid)
                      return std::nullopt;
                  if (route->object_generation != source.object_generation
                      || route->authority_owner_generation != source.authority_owner_generation
                      || route->node_generation == 0 || route->binding_generation == 0) {
                      throw std::runtime_error ("Bound Session relocation route is stale");
                  }

                  location_page_request_t page;
                  do {
                      auto listed =
                        location_store.list_mesh_nodes (mesh_name, page).result ().value ();
                      const auto owner = std::find_if (
                        listed.items.begin (), listed.items.end (),
                        [&route] (const mesh_node_descriptor_t &descriptor) {
                            return descriptor.rid == route->node_rid
                                   && descriptor.lifecycle_generation == route->node_generation;
                        });
                      if (owner != listed.items.end ()) {
                          if (owner->owner_id.empty () || owner->lease_generation <= 0) {
                              throw std::runtime_error (
                                "Bound Session owner has no exact lease fence");
                          }
                          return detail::bound_session_relocation_route_t{
                            route->node_rid,
                            route->node_generation,
                            {owner->owner_id, owner->lease_generation},
                            *route->session_rid,
                            route->binding_generation,
                            route->session_sequence};
                      }
                      page.continuation_token = std::move (listed.continuation_token);
                  } while (page.continuation_token);
                  throw std::runtime_error ("Bound Session owner descriptor was not found");
              });
        }
        if (!_state->services.contains (std::type_index (typeid (actor_manager_t))))
            _state->services.add_singleton<actor_manager_t> (
              std::make_unique<actor_manager_t> (mesh_service->actor_manager ()));
        add_hosted_service (std::move (mesh_service));
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
              [application_mesh] (
                const actor_ref_t &actor, const runtime::messaging::envelope_header_t &header,
                const zlink::message_t &payload, std::chrono::milliseconds timeout,
                const zlink::routing_id_t &source_node,
                const runtime::protocol::actor_route_fence_t &stale_route, std::uint8_t hop_count,
                const runtime::protocol::wire_operation_id_t &operation,
                std::uint64_t reply_route_id) {
                  return application_mesh->relay_application_actor (
                    actor, header, payload, timeout, source_node, stale_route, hop_count, operation,
                    reply_route_id);
              });
            spot_runtime.on_actor_handoff_terminal (
              [application_mesh] (const zlink::routing_id_t &source_node,
                                  const zlink::routing_id_t &source_owner_node,
                                  const runtime::protocol::wire_operation_id_t &operation,
                                  std::uint64_t reply_route_id,
                                  const runtime::protocol::actor_route_fence_t &source_fence,
                                  const result_t<zlink::message_t> &terminal) -> task_t<bool> {
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
                  const auto body = terminal ? terminal.value () : zlink::message_t{};
                  auto parts =
                    runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, body);
                  co_return co_await application_mesh->send_to_node (source_node, parts.items ())
                    == zlink::submit_result_t::ok;
              });
            spot_runtime.on_actor_leave_notification (
              [application_mesh] (
                const zlink::routing_id_t &target_node,
                std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
                  co_return co_await application_mesh->send_to_node (target_node, parts);
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
          location_store ? &location_store->get () : nullptr, _state->monitoring);
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
    if (!mesh_nodes.empty ()) {
        auto provider = _state->services.build_provider ();
        auto &location_store = provider.get_required<location_repository_t> ();
        auto operation_sequence = std::make_shared<std::atomic<std::uint64_t>> (1);
        struct selected_instance_target_t
        {
            std::shared_ptr<detail::mesh_node_runtime_t> source;
            mesh_node_descriptor_t target;
            std::string stable_type;
        };
        auto select_instance_target =
          [mesh_nodes, &location_store] (const spot_id_t &spot_id,
                                         const detail::spot_activation_intent_t &intent)
          -> result_t<selected_instance_target_t> {
            std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> sources;
            for (const auto &mesh : mesh_nodes) {
                if (!intent.mesh_name || mesh->mesh_name () == *intent.mesh_name)
                    sources.push_back (mesh);
            }
            if (sources.empty ())
                return result_t<selected_instance_target_t>::failure (
                  intent.mesh_name ? framework_error_kind_t::not_found
                                   : framework_error_kind_t::not_configured,
                  "No Instance Spot source Mesh is configured");
            if (!intent.mesh_name && sources.size () != 1)
                return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::invalid_operation,
                  "More than one object Mesh is configured; select one with in_mesh");
            const auto source = sources.front ();
            std::vector<mesh_node_descriptor_t> candidates;
            std::vector<mesh_node_descriptor_t> visible_targets;
            location_page_request_t page;
            do {
                auto listed =
                  location_store.list_mesh_nodes (source->mesh_name (), page).result ().value ();
                for (auto &descriptor : listed.items) {
                    if (descriptor.state != framework_runtime_state_t::serving
                        || descriptor.object_role != object_role_t::server
                        || descriptor.placement_weight <= 0)
                        continue;
                    visible_targets.push_back (descriptor);
                    const auto capable = std::any_of (
                      descriptor.object_capabilities.begin (),
                      descriptor.object_capabilities.end (), [&] (const auto &capability) {
                          return capability.object_kind == placement_object_kind_t::instance_spot
                                 && (!intent.stable_type
                                     || capability.stable_type == *intent.stable_type);
                      });
                    const auto spot_capacity = descriptor.capacity.spots;
                    const auto typed_capacity = std::find_if (
                      descriptor.capacity.spot_types.begin (),
                      descriptor.capacity.spot_types.end (), [&] (const auto &typed) {
                          return typed.object_kind == placement_object_kind_t::instance_spot
                                 && intent.stable_type && typed.stable_type == *intent.stable_type;
                      });
                    const auto typed_available =
                      !intent.stable_type || typed_capacity == descriptor.capacity.spot_types.end ()
                      || typed_capacity->usage.limit == 0
                      || typed_capacity->usage.active + typed_capacity->usage.reserved
                           < static_cast<std::uint64_t> (typed_capacity->usage.limit);
                    if (capable && typed_available
                        && (spot_capacity.limit == 0
                            || spot_capacity.active + spot_capacity.reserved
                                 < static_cast<std::uint64_t> (spot_capacity.limit)))
                        candidates.push_back (std::move (descriptor));
                }
                page.continuation_token = std::move (listed.continuation_token);
            } while (page.continuation_token);
            const auto authority =
              location_store.read_authority (runtime::spot_authority_key (spot_id))
                .result ()
                .value ();
            if (const auto *snapshot = std::get_if<authority_snapshot_t> (&authority);
                snapshot && snapshot->allocation.state == placement_allocation_state_t::active
                && snapshot->allocation.object_kind == placement_object_kind_t::instance_spot
                && snapshot->allocation.target.mesh_name == source->mesh_name ()
                && (!intent.stable_type
                    || *intent.stable_type == snapshot->allocation.stable_type)) {
                const auto current = std::find_if (
                  visible_targets.begin (), visible_targets.end (),
                  [&] (const mesh_node_descriptor_t &candidate) {
                      return candidate.rid.to_string ()
                               == snapshot->allocation.target.node_rid.value ()
                             && candidate.lifecycle_generation
                                  == snapshot->allocation.target.node_lifecycle_generation;
                  });
                if (current != visible_targets.end ()) {
                    return result_t<selected_instance_target_t>::success (
                      {source, *current, snapshot->allocation.stable_type});
                }
            }
            if (candidates.empty ())
                return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::not_found, "No eligible Instance Spot target is Ready");
            std::set<std::string> stable_types;
            for (const auto &candidate : candidates)
                for (const auto &capability : candidate.object_capabilities)
                    if (capability.object_kind == placement_object_kind_t::instance_spot
                        && (!intent.stable_type || capability.stable_type == *intent.stable_type))
                        stable_types.insert (capability.stable_type);
            if (!intent.stable_type && stable_types.size () != 1)
                return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Instance Spot stable type is required when the Mesh publishes multiple types");
            const auto stable_type =
              intent.stable_type ? *intent.stable_type : *stable_types.begin ();
            candidates.erase (
              std::remove_if (
                candidates.begin (), candidates.end (),
                [&] (const auto &candidate) {
                    const auto capable = std::any_of (
                      candidate.object_capabilities.begin (), candidate.object_capabilities.end (),
                      [&] (const auto &capability) {
                          return capability.object_kind == placement_object_kind_t::instance_spot
                                 && capability.stable_type == stable_type;
                      });
                    const auto typed = std::find_if (
                      candidate.capacity.spot_types.begin (), candidate.capacity.spot_types.end (),
                      [&] (const auto &capacity) {
                          return capacity.object_kind == placement_object_kind_t::instance_spot
                                 && capacity.stable_type == stable_type;
                      });
                    return !capable
                           || (typed != candidate.capacity.spot_types.end ()
                               && typed->usage.limit > 0
                               && typed->usage.active + typed->usage.reserved
                                    >= static_cast<std::uint64_t> (typed->usage.limit));
                }),
              candidates.end ());
            if (candidates.empty ())
                return result_t<selected_instance_target_t>::failure (
                  framework_error_kind_t::not_found,
                  "No eligible Instance Spot target has capacity");
            const auto index = std::hash<std::string>{}(std::string (spot_id)) % candidates.size ();
            return result_t<selected_instance_target_t>::success (
              {source, candidates[index], stable_type});
        };
        auto make_activation = [operation_sequence] (const selected_instance_target_t &selected,
                                                     const spot_id_t &spot_id, bool request,
                                                     bool has_metadata,
                                                     std::chrono::milliseconds timeout) {
            const auto source_status = selected.source->status ();
            const auto operation = operation_sequence->fetch_add (1, std::memory_order_relaxed);
            auto operation_scope = static_cast<std::uint64_t> (std::hash<std::string>{}(
                                     source_status.routing_id ().to_string ()))
                                   ^ source_status.lifecycle_generation ();
            if (operation_scope == 0)
                operation_scope = source_status.lifecycle_generation () != 0
                                    ? source_status.lifecycle_generation ()
                                    : 1;
            return runtime::protocol::instance_spot_activation_header_t{
              {selected.target.rid.to_bytes (), selected.target.lifecycle_generation,
               std::string (spot_id), selected.target.mesh_name, selected.stable_type,
               std::to_string (selected.target.descriptor_revision),
               static_cast<std::uint64_t> (
                 std::chrono::duration_cast<std::chrono::milliseconds> (
                   std::chrono::system_clock::now ().time_since_epoch () + timeout)
                   .count ())},
              source_status.lifecycle_generation (),
              source_status.routing_id ().to_bytes (),
              std::nullopt,
              request,
              {operation_scope, operation},
              0,
              has_metadata};
        };
        channel_runtime.bind_instance_spot_activator (
          [select_instance_target, make_activation, serializers = &_state->serializers,
           dispatch = options.dispatch_options ()] (
            const spot_id_t &spot_id, const detail::spot_activation_intent_t &intent,
            const std::string &packet_name, std::type_index message_type,
            std::function<encoded_payload_t (serializer_registry_t &)> encode_payload,
            const std::map<std::string, std::string> &metadata) -> task_t<result_t<void>> {
              auto flow_scope = runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application, detail::message_flow_tracer_t (dispatch).mode ());
              const auto flow = runtime::flow_context_t::current ();
              auto selected = select_instance_target (spot_id, intent);
              if (!selected)
                  co_return detail::propagate_failure<void> (
                    selected, "Instance Spot target selection failed");
              auto metadata_frame = detail::mesh_metadata_codec_t::encode (metadata);
              auto header = make_activation (selected.value (), spot_id, false,
                                             !metadata_frame.empty (), std::chrono::seconds (30));
              const auto payload = encode_payload (*serializers);
              runtime::protocol::application_payload_t application_payload{
                packet_name, serializers->content_type (message_type), payload.to_bytes ()};
              if (flow && !flow->flow_id.empty ()) {
                  application_payload.flow_id = flow->flow_id;
                  application_payload.flow_origin = flow->origin;
              }
              const auto trace_context = make_instance_spot_activation_trace_context (
                detail::message_flow_tracer_t (dispatch).enabled_for (message_flow_outcome_t::sent),
                packet_name, selected.value ().target.mesh_name, selected.value ().target.rid,
                spot_id, header);
              if (trace_context) {
                  trace_instance_spot_activation (dispatch, flow, message_flow_outcome_t::sent,
                                                  dispatch_message_kind_t::send, *trace_context);
              }
              const auto submitted =
                co_await selected.value ().source->send_instance_spot_activation_remote (
                  selected.value ().target.rid, std::move (header),
                  metadata_frame.empty () ? std::optional<std::vector<std::uint8_t>>{}
                                          : std::make_optional (std::move (metadata_frame)),
                  std::move (application_payload));
              co_return submitted
                ? result_t<void>::success ()
                : result_t<void>::failure (framework_error_kind_t::unavailable,
                                           "Instance Spot activation was not admitted");
          },
          [select_instance_target, make_activation, serializers = &_state->serializers,
           dispatch = options.dispatch_options ()] (
            const spot_id_t &spot_id, const detail::spot_activation_intent_t &intent,
            std::string packet_name, std::type_index request_type,
            std::function<encoded_payload_t (serializer_registry_t &)> encode_payload,
            std::chrono::milliseconds timeout,
            std::map<std::string, std::string> metadata) -> task_t<zlink::message_t> {
              auto flow_scope = runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application, detail::message_flow_tracer_t (dispatch).mode ());
              const auto flow = runtime::flow_context_t::current ();
              auto selected = select_instance_target (spot_id, intent);
              if (!selected)
                  co_return detail::propagate_failure<zlink::message_t> (
                    selected, "Instance Spot target selection failed");
              auto metadata_frame = detail::mesh_metadata_codec_t::encode (metadata);
              auto header = make_activation (selected.value (), spot_id, true,
                                             !metadata_frame.empty (), timeout);
              const auto payload = encode_payload (*serializers);
              runtime::protocol::application_payload_t application_payload{
                packet_name, serializers->content_type (request_type), payload.to_bytes ()};
              if (flow && !flow->flow_id.empty ()) {
                  application_payload.flow_id = flow->flow_id;
                  application_payload.flow_origin = flow->origin;
              }
              const auto trace_context = make_instance_spot_activation_trace_context (
                detail::message_flow_tracer_t (dispatch).capture_enabled (), packet_name,
                selected.value ().target.mesh_name, selected.value ().target.rid, spot_id, header);
              if (trace_context) {
                  trace_instance_spot_activation (dispatch, flow, message_flow_outcome_t::sent,
                                                  dispatch_message_kind_t::request, *trace_context);
              }
              auto completion =
                std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
              auto output = completion->task ();
              const auto submitted =
                co_await selected.value ().source->activate_instance_spot_remote (
                  selected.value ().target.rid, std::move (header),
                  metadata_frame.empty () ? std::optional<std::vector<std::uint8_t>>{}
                                          : std::make_optional (std::move (metadata_frame)),
                  std::move (application_payload), timeout,
                  [completion, dispatch, flow, trace_context] (
                    runtime::foundation::operation_terminal_t terminal,
                    runtime::protocol::reply_header_t reply,
                    std::optional<runtime::protocol::application_payload_t> application_reply) {
                      if (terminal != runtime::foundation::operation_terminal_t::completed) {
                          if (trace_context) {
                              trace_instance_spot_activation (
                                dispatch, flow, message_flow_outcome_t::reply_received,
                                dispatch_message_kind_t::request, *trace_context,
                                message_flow_result_t::failed);
                          }
                          completion->complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::unavailable,
                            "Instance Spot activation transport did not complete"));
                          return;
                      }
                      if (reply.terminal_result != 0) {
                          if (trace_context) {
                              trace_instance_spot_activation (
                                dispatch, flow, message_flow_outcome_t::reply_received,
                                dispatch_message_kind_t::request, *trace_context,
                                message_flow_result_t::failed,
                                message_flow_reason_t::activation_rejected);
                          }
                          completion->complete (detail::result_access_t::failure<zlink::message_t> (
                            runtime::messaging::request_failure_mapper_t{}.reply_header_exception (
                              reply.terminal_result, reply.failure_code,
                              "Instance Spot activation request")));
                          return;
                      }
                      if (!application_reply) {
                          if (trace_context) {
                              trace_instance_spot_activation (
                                dispatch, flow, message_flow_outcome_t::reply_received,
                                dispatch_message_kind_t::request, *trace_context,
                                message_flow_result_t::failed);
                          }
                          completion->complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::protocol_error,
                            "Instance Spot activation reply payload is missing"));
                          return;
                      }
                      if (trace_context) {
                          trace_instance_spot_activation (
                            dispatch, flow, message_flow_outcome_t::reply_received,
                            dispatch_message_kind_t::response, *trace_context);
                      }
                      completion->complete (result_t<zlink::message_t>::success (
                        zlink::message_t::from (std::move (application_reply->payload))));
                  });
              if (!submitted) {
                  if (trace_context) {
                      trace_instance_spot_activation (
                        dispatch, flow, message_flow_outcome_t::reply_received,
                        dispatch_message_kind_t::request, *trace_context,
                        message_flow_result_t::failed, message_flow_reason_t::activation_rejected);
                  }
                  completion->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::unavailable,
                    "Instance Spot activation was not admitted"));
              }
              co_return co_await output;
          });
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
          -> task_t<result_t<runtime::messaging::message_parts_t>> {
            detail::host::call_id_t operation;
            const auto submitted = co_await mesh->request_to_spot (
              source_spot_id, target_node, target_spot, target_spot_generation, parts.items (),
              operation, timeout);
            if (submitted != zlink::submit_result_t::ok) {
                co_return result_t<runtime::messaging::message_parts_t>::failure (
                  framework_error_kind_t::unavailable, "MeshNode Spot request was not submitted");
            }
            co_return co_await await_mesh_request_completion (*mesh, operation,
                                                              "MeshNode Spot request");
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
        channel_runtime.bind_mesh_node_transport (
          mesh_name,
          [mesh, mesh_node_service] (
            const zlink::routing_id_t &target,
            runtime::messaging::message_parts_t parts) -> task_t<result_t<void>> {
              const auto local_rid = mesh->routing_id ();
              if (local_rid && *local_rid == target) {
                  co_return one_way_native_submit_result (
                    mesh_node_service->submit_local_node_send (mesh,
                                                               std::move (parts).take_items ()),
                    "MeshNode send");
              }
              co_return one_way_native_submit_result (
                co_await mesh->send_to_node (target, parts.items ()), "MeshNode send");
          },
          [mesh] (const zlink::routing_id_t &target, runtime::messaging::message_parts_t parts,
                  std::chrono::milliseconds timeout)
            -> task_t<result_t<runtime::messaging::message_parts_t>> {
              detail::host::call_id_t operation;
              const auto submitted =
                co_await mesh->request_to_node (target, parts.items (), operation, timeout);
              if (submitted != zlink::submit_result_t::ok) {
                  if (submitted == zlink::submit_result_t::not_found) {
                      co_return result_t<runtime::messaging::message_parts_t>::failure (
                        framework_error_kind_t::not_found, "MeshNode request target was not found");
                  }
                  if (submitted == zlink::submit_result_t::terminated) {
                      co_return detail::boundary_failure<runtime::messaging::message_parts_t> (
                        detail::boundary_error_t::shutdown, "MeshNode request runtime is stopped");
                  }
                  co_return result_t<runtime::messaging::message_parts_t>::failure (
                    runtime::messaging::map_submit_result_error_kind (submitted),
                    "MeshNode request was not submitted");
              }
              co_return co_await await_mesh_request_completion (*mesh, operation,
                                                                "MeshNode request");
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
                -> task_t<result_t<runtime::messaging::message_parts_t>> {
                  detail::host::call_id_t operation;
                  const auto submitted = co_await mesh->request_to_channel (
                    channel_name, parts.items (), operation, timeout);
                  if (submitted != zlink::submit_result_t::ok) {
                      if (submitted == zlink::submit_result_t::terminated) {
                          co_return detail::boundary_failure<runtime::messaging::message_parts_t> (
                            detail::boundary_error_t::shutdown,
                            "RouteMesh channel request runtime is stopped");
                      }
                      co_return result_t<runtime::messaging::message_parts_t>::failure (
                        runtime::messaging::map_submit_result_error_kind (submitted),
                        "RouteMesh channel request was not submitted");
                  }
                  co_return co_await await_mesh_request_completion (*mesh, operation,
                                                                    "RouteMesh channel request");
              });
        }
    }
    if (!mesh_nodes.empty ()) {
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
              const auto created = call.submit ().result ();
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
        actor_gateway_runtime.on_join_entry_spot (
          [application_mesh] (const actor_ref_t &actor, const zlink::message_t &request,
                              std::chrono::milliseconds timeout) {
              const auto routing_id = application_mesh->routing_id ();
              const auto target_node =
                routing_id ? node_rid_t::from_string (routing_id->to_string ()) : actor.node_rid ();
              return application_mesh->join_application_actor_to_entry_spot (actor, target_node,
                                                                             request, timeout);
          });
        actor_gateway_runtime.on_join_spot (
          [application_mesh, actor_gateway_runtime,
           spot_locations = &_state->services.build_provider ()
                               .get_required<runtime::spot_address_resolver_t> ()] (
            const actor_ref_t &actor, spot_id_t target_spot, const zlink::message_t &request,
            std::string packet_name, std::string content_type,
            std::chrono::milliseconds timeout) -> task_t<detail::actor_join_reply_t> {
              auto located = co_await spot_locations->resolve_spot_address ({}, target_spot);
              if (!located) {
                  co_return result_t<detail::actor_join_reply_t>::failure (
                    framework_error_kind_t::not_found, "target Spot location was not found");
              }
              const auto &target = *located;
              if (target.object_generation == 0) {
                  co_return result_t<detail::actor_join_reply_t>::failure (
                    framework_error_kind_t::not_found,
                    "target Spot lifecycle generation was not published");
              }
              if (target.owner.lease_generation > 0) {
                  application_mesh->observe_spot_authority (
                    target.node_rid, target.spot_id, target.object_generation,
                    target.node_generation, target.authority_owner_generation,
                    static_cast<std::uint64_t> (target.owner.lease_generation));
              }
              const auto bound_session = actor_gateway_runtime.bound_session_route (actor);
              co_return co_await application_mesh->join_application_actor_to_spot (
                actor, target, request, timeout,
                bound_session ? std::make_optional (bound_session->node_rid) : std::nullopt,
                bound_session ? bound_session->session_rid : std::nullopt, std::move (packet_name),
                std::move (content_type));
          });
        actor_gateway_runtime.on_join_barrier ([application_mesh] (const actor_ref_t &actor) {
            return application_mesh->reserve_application_actor_join_barrier (actor);
        });
        actor_gateway_runtime.on_bound_session ([] (const actor_ref_t &) {
            // The STREAM native binder owns the exact physical Session RID
            // and binding generation, so it performs the remote bind.
            return result_t<void>::success ();
        });
        actor_gateway_runtime.on_bound_session_send (
          [application_mesh, actor_gateway_runtime,
           stream_runtime] (const actor_ref_t &actor, std::uint64_t expected_binding_generation,
                            const detail::stream_header_t &header,
                            const zlink::message_t &payload) mutable -> task_t<result_t<void>> {
              try {
                  const auto route = actor_gateway_runtime.bound_session_route (actor);
                  if (!route || !route->session_rid || route->binding_generation == 0
                      || route->authority_owner_generation == 0
                      || route->owner_lease_generation == 0
                      || (expected_binding_generation != 0
                          && expected_binding_generation != route->binding_generation)) {
                      co_return result_t<void>::failure (framework_error_kind_t::not_configured,
                                                         "Actor bound Session route is not ready");
                  }
                  const auto local = application_mesh->native_node ().status ();
                  if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
                      actor_gateway_runtime.trace_bound_session_send_stage (
                        std::string (actor.actor_id ().value ()), "actor_owner_push_target",
                        "session_rid=" + route->session_rid->to_hex ()
                          + " binding_generation=" + std::to_string (route->binding_generation));
                  }
                  const auto local_actor = detail::actor_ref_access_t::make (
                    node_rid_t::from_string (local.routing_id ().to_string ()),
                    std::string (detail::actor_ref_access_t::actor_type (actor)),
                    std::string (actor.actor_id ().value ()), actor.object_generation ());
                  /* Stage traces emit at detailed only: build the callback
                   * (and its actor-id copy) exclusively when it can emit, so
                   * the silent send path pays neither the std::function nor
                   * the per-stage string conversions. */
                  detail::backend::raw_send_stage_trace_t stage_trace;
                  if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
                      stage_trace = [actor_gateway_runtime,
                                     actor_id = std::string (actor.actor_id ().value ())] (
                                      std::string_view stage, std::string_view result) mutable {
                          actor_gateway_runtime.trace_bound_session_send_stage (actor_id, stage,
                                                                                result);
                      };
                  }
                  const auto submitted =
                    co_await application_mesh->native_node ().send_bound_session (
                      local_actor, route->node_rid, route->binding_generation,
                      route->authority_owner_generation, route->owner_lease_generation,
                      encode_bound_session_frame (stream_runtime, header, payload),
                      std::move (stage_trace));
                  co_return one_way_native_submit_result (submitted,
                                                          "Framework Actor bound Session send");
              }
              catch (const framework_exception_t &error) {
                  co_return detail::result_access_t::failure<void> (error);
              }
              catch (const std::exception &error) {
                  co_return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                     error.what ());
              }
          });
        actor_gateway_runtime.on_relay (
          [application_mesh, actor_gateway_runtime, request_timeout] (
            const actor_ref_t &actor, actor_context_t, const detail::stream_header_t &header,
            const zlink::message_t &payload,
            std::optional<detail::bound_session_relay_source_t> bound_session_source) mutable
          -> task_t<std::optional<zlink::message_t>> {
              detail::session_ingress_completion_t ingress;
              auto routed_actor = actor;
              runtime::protocol::actor_route_fence_t stale_route;
              std::optional<zlink::routing_id_t> session_owner;
              if (bound_session_source) {
                  auto &sessions = application_mesh->native_node ().sessions ();
                  auto [admission, dispatch] = sessions.admit_inbound (
                    bound_session_source->session_rid.to_hex (),
                    bound_session_source->binding_generation,
                    std::string (actor.actor_id ().value ()),
                    bound_session_source->session_sequence, request_timeout);
                  if (admission != runtime::stateful::stateful_error_t::none || !dispatch) {
                      co_return result_t<std::optional<zlink::message_t>>::failure (
                        admission == runtime::stateful::stateful_error_t::moving
                          ? framework_error_kind_t::unavailable
                          : framework_error_kind_t::invalid_operation,
                        "bound Session ingress was not admitted by the relocation barrier");
                  }
                  ingress.arm (sessions, *dispatch);
                  session_owner = application_mesh->routing_id ();
                  const auto completion_admitted =
                    session_owner
                      ? actor_gateway_runtime.begin_session_relay_completion (
                          actor, *session_owner, bound_session_source->session_rid,
                          bound_session_source->binding_generation, dispatch->inbound_sequence)
                      : result_t<void>::failure (framework_error_kind_t::not_found,
                                                 "bound Session owner identity is unavailable");
                  if (!completion_admitted) {
                      co_return result_t<std::optional<zlink::message_t>>::failure (
                        completion_admitted.error_kind (),
                        completion_admitted.error () != nullptr
                          ? completion_admitted.error ()->what ()
                          : "bound Session relay completion was not admitted");
                  }
                  routed_actor = detail::actor_ref_access_t::make (
                    node_rid_t::from_string (dispatch->binding.actor.node_id),
                    std::string (detail::actor_ref_access_t::actor_type (actor)),
                    dispatch->binding.actor.key, dispatch->binding.actor.object_generation);
                  stale_route = runtime::protocol::actor_route_fence_t{
                    dispatch->binding.actor.key,
                    dispatch->binding.actor.object_generation,
                    zlink::routing_id_t::from (dispatch->binding.actor.node_id).to_bytes (),
                    dispatch->binding.target_node_generation,
                    dispatch->binding.actor.authority_owner_generation,
                    dispatch->binding.owner_lease_generation};
                  bound_session_source->session_sequence = dispatch->inbound_sequence;
              }
              const auto admitted_source = bound_session_source;
              //  Spec 20-session-actor-dispatch §3/§7 — the admitted ingress
              //  and relay-sequence fences must complete exactly once even
              //  when the application handler throws. Skipping the tail
              //  leaves the owner's recorded sequence behind, so every later
              //  request on this binding is rejected with "bound Session
              //  relay completion is not current or next".
              std::exception_ptr relay_failure;
              std::optional<zlink::message_t> relayed;
              try {
                  relayed = co_await (
                    stale_route.owner_lease_generation == 0
                      ? application_mesh->relay_application_actor (routed_actor, header, payload,
                                                                   request_timeout, true,
                                                                   std::move (bound_session_source))
                      : application_mesh->relay_application_actor (
                          routed_actor,
                          runtime::messaging::envelope_header_t{
                            .kind = header.kind () == detail::stream_message_kind_t::send
                                      ? runtime::messaging::message_kind_t::command
                                      : runtime::messaging::message_kind_t::request,
                            .channel_name = "actor",
                            .message_name = std::string (header.packet_name ()),
                            .content_type =
                              std::string (detail::stream_content_type (header.codec ())),
                            .metadata = header.metadata ().values ()},
                          payload, request_timeout, zlink::routing_id_t::from (std::uint32_t{0}),
                          stale_route, 0, runtime::protocol::wire_operation_id_t{}, 0, true,
                          std::move (bound_session_source)));
              }
              catch (...) {
                  relay_failure = std::current_exception ();
              }
              const auto completed = ingress.complete ();
              result_t<void> observed = result_t<void>::success ();
              if (admitted_source) {
                  observed =
                    session_owner
                      ? actor_gateway_runtime.complete_session_relay (
                          actor, *session_owner, admitted_source->session_rid,
                          admitted_source->binding_generation, admitted_source->session_sequence)
                      : result_t<void>::failure (framework_error_kind_t::not_found,
                                                 "bound Session owner identity is unavailable");
              }
              //  The original application error stays the single terminal;
              //  fence completion above ran regardless, and its own failure
              //  is reported only when the relay itself succeeded.
              if (relay_failure)
                  std::rethrow_exception (relay_failure);
              if (completed != runtime::stateful::stateful_error_t::none) {
                  co_return result_t<std::optional<zlink::message_t>>::failure (
                    framework_error_kind_t::internal_failure,
                    "bound Session ingress completion lost its exact fence");
              }
              if (!observed) {
                  co_return result_t<std::optional<zlink::message_t>>::failure (
                    observed.error_kind (),
                    observed.error () != nullptr
                      ? observed.error ()->what ()
                      : "bound Session ingress high-water was not published");
              }
              co_return relayed;
          });
        actor_gateway_runtime.on_disconnect (
          [mesh_nodes, request_timeout] (const actor_ref_t &actor) -> task_t<void> {
              bool notified = false;
              result_t<void> last = result_t<void>::failure (
                framework_error_kind_t::not_found, "Actor disconnect RouteMesh was not found");
              for (const auto &mesh : mesh_nodes) {
                  try {
                      co_await mesh->notify_application_actor_disconnected (
                        actor, actor.node_rid (), request_timeout);
                      notified = true;
                  }
                  catch (const framework_exception_t &error) {
                      last = detail::result_access_t::failure<void> (error);
                  }
                  catch (const std::exception &error) {
                      last = result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
                  }
              }
              if (!notified) {
                  throw framework_exception_t (last.error_kind (),
                                               last.error () != nullptr
                                                 ? last.error ()->what ()
                                                 : "Actor disconnect RouteMesh failed");
              }
              co_return;
          });
        application_mesh->configure_bound_session_operations (
          runtime::host::bound_session_operations_t{
            [actor_gateway_runtime, application_mesh,
             stream_runtime] (const runtime::protocol::bound_session_bind_t &bind,
                              const zlink::routing_id_t &session_owner,
                              std::uint64_t session_owner_generation) mutable {
                const auto actor = detail::actor_ref_access_t::make (
                  node_rid_t::from_string (
                    zlink::routing_id_t::from (bind.actor.target_node_routing_id).to_string ()),
                  {}, bind.actor.actor_id, bind.actor.object_generation);
                const auto session_rid = zlink::routing_id_t::from (bind.session_routing_id);
                actor_gateway_runtime.trace_bound_session_send_stage (
                  bind.actor.actor_id, "bound_session_bind_receive",
                  "new_session_rid=" + session_rid.to_hex ()
                    + " new_binding_generation=" + std::to_string (bind.binding.generation));
                if (bind.binding.state
                    == runtime::protocol::bound_session_binding_state_t::tombstone) {
                    const auto retired = actor_gateway_runtime.retire_bound_session_route (
                      actor, session_owner, session_rid, bind.binding.generation);
                    return runtime::host::bound_session_bind_operation_result_t{
                      retired ? runtime::stateful::stateful_error_t::none
                              : runtime::stateful::stateful_error_t::conflict,
                      std::nullopt};
                }
                auto sink = [application_mesh, actor_gateway_runtime, stream_runtime, actor,
                             session_owner, session_rid,
                             binding_generation = bind.binding.generation,
                             authority_owner_generation = bind.actor.authority_owner_generation,
                             owner_lease_generation = bind.actor.owner_lease_generation] (
                              std::string packet_name, stream_codec_t codec,
                              const zlink::message_t &payload) mutable -> task_t<void> {
                    try {
                        const detail::stream_header_t header (
                          detail::stream_message_kind_t::send, codec,
                          detail::stream_header_flags_t::none, std::nullopt,
                          std::move (packet_name));
                        const detail::actor_bound_session_route_t staged_route{
                          session_owner,
                          session_rid,
                          actor.object_generation (),
                          0,
                          authority_owner_generation,
                          owner_lease_generation,
                          binding_generation,
                          0,
                          0};
                        const auto current_route =
                          actor_gateway_runtime.resolve_bound_session_push_route (actor,
                                                                                  staged_route);
                        if (!current_route || !current_route->session_rid) {
                            throw framework_exception_t (
                              framework_error_kind_t::not_configured,
                              "Framework Actor bound Session route is unavailable");
                        }
                        const auto submitted =
                          co_await application_mesh->native_node ().send_bound_session (
                            actor, current_route->node_rid, current_route->binding_generation,
                            current_route->authority_owner_generation,
                            current_route->owner_lease_generation,
                            encode_bound_session_frame (stream_runtime, header, payload),
                            [actor_gateway_runtime,
                             actor_id = std::string (actor.actor_id ().value ())] (
                              std::string_view stage, std::string_view result) mutable {
                                actor_gateway_runtime.trace_bound_session_send_stage (
                                  actor_id, std::string (stage), std::string (result));
                            });
                        const auto result = one_way_native_submit_result (
                          submitted, "Framework Actor bound Session send");
                        if (!result) {
                            throw result.error ()
                              ? *result.error ()
                              : framework_exception_t (framework_error_kind_t::internal_failure,
                                                       "Framework Actor bound Session send failed");
                        }
                        co_return;
                    }
                    catch (const framework_exception_t &error) {
                        throw error;
                    }
                };
                const auto transition = actor_gateway_runtime.replace_session_route (
                  actor, std::move (sink),
                  detail::actor_bound_session_route_t{
                    session_owner, session_rid, bind.actor.object_generation,
                    session_owner_generation, bind.actor.authority_owner_generation,
                    bind.actor.owner_lease_generation, bind.binding.generation, 0, 0},
                  stream_codec_t::message_pack);
                if (!transition) {
                    return runtime::host::bound_session_bind_operation_result_t{
                      runtime::stateful::stateful_error_t::conflict, std::nullopt};
                }
                std::optional<runtime::protocol::bound_session_replaced_t> replacement;
                const auto &change = transition.value ();
                actor_gateway_runtime.trace_bound_session_send_stage (
                  bind.actor.actor_id, "actor_owner_route_publish",
                  "session_rid=" + session_rid.to_hex ()
                    + " binding_generation=" + std::to_string (bind.binding.generation)
                    + " replaced=" + (change.changed ? "true" : "false"));
                if (change.current
                    && change.current->binding_generation != bind.binding.generation) {
                    actor_gateway_runtime.trace_bound_session_send_stage (
                      bind.actor.actor_id, "actor_owner_route_publish_stale_ignored",
                      "session_rid=" + session_rid.to_hex () + " binding_generation="
                        + std::to_string (bind.binding.generation) + " current_binding_generation="
                        + std::to_string (change.current->binding_generation));
                }
                if (change.changed && change.previous && change.previous->session_rid
                    && change.previous->node_generation != 0
                    && change.previous->binding_generation != 0) {
                    replacement = runtime::protocol::bound_session_replaced_t{
                      bind.actor,
                      runtime::protocol::retired_bound_session_route_fence_t{
                        change.previous->node_rid.to_bytes (), change.previous->node_generation,
                        change.previous->node_rid.to_string (), change.previous->node_generation,
                        change.previous->session_rid->to_bytes (),
                        change.previous->binding_generation}};
                }
                return runtime::host::bound_session_bind_operation_result_t{
                  runtime::stateful::stateful_error_t::none, std::move (replacement)};
            },
            [actor_gateway_runtime,
             stream_runtime] (const runtime::protocol::bound_session_send_t &send,
                              std::vector<zlink::message_t> parts) mutable {
                try {
                    if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
                        actor_gateway_runtime.trace_bound_session_send_stage (
                          send.actor.actor_id, "session_node_receive",
                          "binding_generation="
                            + std::to_string (send.expected_binding_generation));
                    }
                    const auto actor = detail::actor_ref_access_t::make (
                      node_rid_t::from_string (
                        zlink::routing_id_t::from (send.actor.target_node_routing_id).to_string ()),
                      {}, send.actor.actor_id, send.actor.object_generation);
                    auto [header, payload] = decode_bound_session_frame (stream_runtime, parts);
                    const auto dispatched = actor_gateway_runtime.dispatch_bound_session_send (
                      actor, std::string (header.packet_name ()), header.codec (), payload);
                    return dispatched ? runtime::stateful::stateful_error_t::none
                                      : runtime::stateful::stateful_error_t::conflict;
                }
                catch (...) {
                    return runtime::stateful::stateful_error_t::invalid;
                }
            },
            [actor_gateway_runtime] (
              const runtime::protocol::bound_session_replaced_t &replacement) mutable {
                (void) actor_gateway_runtime.dispatch_bound_session_replaced (replacement);
            },
            [actor_gateway_runtime] (const runtime::protocol::session_relocation_route_t &route,
                                     const runtime::stateful::stream_binding_t &previous,
                                     const runtime::stateful::stream_binding_t &target) mutable {
                return actor_gateway_runtime.commit_session_relocation_route (route, previous,
                                                                              target);
            },
            [actor_gateway_runtime] (const runtime::protocol::session_relocation_route_t &route,
                                     std::uint64_t target_owner_lease_generation) mutable {
                return actor_gateway_runtime.prepare_session_relocation_target_route (
                  route, target_owner_lease_generation);
            },
            [actor_gateway_runtime] (const runtime::protocol::bound_session_send_t &send) mutable {
                return actor_gateway_runtime.confirm_session_remote_tenure (send);
            },
            [actor_gateway_runtime,
             stream_runtime] (const runtime::protocol::bound_session_send_t &send) mutable
            -> std::optional<runtime::host::bound_session_operations_t::delivery_capability_t> {
                if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
                    actor_gateway_runtime.trace_bound_session_send_stage (
                      send.actor.actor_id, "session_node_receive",
                      "binding_generation=" + std::to_string (send.expected_binding_generation));
                }
                const auto actor = detail::actor_ref_access_t::make (
                  node_rid_t::from_string (
                    zlink::routing_id_t::from (send.actor.target_node_routing_id).to_string ()),
                  {}, send.actor.actor_id, send.actor.object_generation);
                auto admitted = actor_gateway_runtime.admit_bound_session_delivery (
                  actor, send.expected_binding_generation);
                if (!admitted)
                    return std::nullopt;
                return [admitted = std::move (*admitted),
                        stream_runtime] (std::vector<zlink::message_t> parts) mutable {
                    try {
                        auto [header, payload] = decode_bound_session_frame (stream_runtime, parts);
                        const auto dispatched =
                          admitted (std::string (header.packet_name ()), header.codec (), payload);
                        return dispatched ? runtime::stateful::stateful_error_t::none
                                          : runtime::stateful::stateful_error_t::conflict;
                    }
                    catch (...) {
                        return runtime::stateful::stateful_error_t::invalid;
                    }
                };
            }});
    }
    if (!_state->services.contains (std::type_index (typeid (actor_client_t)))) {
        _state->services.add_factory<actor_client_t> (
          [mesh_nodes, actor_location_observer,
           location_options = options.location_options ()] (service_provider_t &provider) mutable {
              auto route_runtime = provider.get<route_mesh_runtime_t> ();
              return runtime::make_actor_client (
                provider.get_required<runtime::live_location_reader_t> (),
                provider.get_required<serializer_registry_t> (), mesh_nodes,
                actor_location_observer, location_options,
                route_runtime ? &route_runtime->get () : nullptr);
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
    const auto stream_snapshot = detail::stream_runtime_t::from (_state->zlink).snapshots ();
    std::shared_ptr<runtime::client_server::client_server_location_runtime_t> client_server_runtime;
    if (!_state->services.contains (std::type_index (typeid (client_server_runtime_t)))) {
        auto provider = _state->services.build_provider ();
        client_server_runtime =
          std::make_shared<runtime::client_server::client_server_location_runtime_t> (
            _state->zlink.message_bus (), channel_snapshot,
            provider.get_required<runtime::location_runtime_t> (),
            provider.get_required<location_repository_t> (),
            provider.get_required<location_repository_t> (), provider, _state->serializers,
            _state->handlers, options.runtime_client_server_advertise_hosts (),
            _state->listener_statuses, _state->application_job_queue);
        _state->services.add_factory<client_server_runtime_t> (
          [client_server_runtime] (service_provider_t &) {
              return std::static_pointer_cast<client_server_runtime_t> (client_server_runtime);
          },
          service_lifetime_t::singleton);
    }
    std::shared_ptr<runtime::fanout::fanout_location_runtime_t> fanout_runtime;
    const auto needs_fanout_runtime =
      std::any_of (channel_snapshot.begin (), channel_snapshot.end (), [] (const auto &channel) {
          return (channel.publisher.enabled && channel.publisher.discovery)
                 || (channel.subscriber.enabled && channel.subscriber.discovery);
      });
    if (needs_fanout_runtime
        && !_state->services.contains (std::type_index (typeid (fanout_runtime_t)))) {
        auto provider = _state->services.build_provider ();
        fanout_runtime = std::make_shared<runtime::fanout::fanout_location_runtime_t> (
          _state->zlink.message_bus (), channel_snapshot,
          provider.get_required<runtime::location_runtime_t> (),
          provider.get_required<location_repository_t> (),
          provider.get_required<location_repository_t> (), provider, _state->serializers,
          _state->handlers, options.runtime_fanout_advertise_hosts (), _state->listener_statuses,
          _state->application_job_queue);
        _state->services.add_factory<fanout_runtime_t> (
          [fanout_runtime] (service_provider_t &) {
              return std::static_pointer_cast<fanout_runtime_t> (fanout_runtime);
          },
          service_lifetime_t::singleton);
    }
    add_hosted_service (std::make_unique<runtime::location_auto_connect_host_service_t> (
      _state->zlink.message_bus (), channel_snapshot, _state->handlers, _state->serializers,
      options.runtime_client_server_advertise_hosts (), options.runtime_fanout_advertise_hosts (),
      options.route_mesh_client_channels (), mesh_nodes,
      [mesh_node_service] {
          return mesh_node_service == nullptr
                 || mesh_node_service->republish_after_store_recovery ();
      },
      std::move (client_server_runtime), std::move (fanout_runtime), _state->listener_statuses));
    if (detail::has_inbound_channel (channel_snapshot)) {
        add_hosted_service (std::make_unique<runtime::channel_host_service_t> (
          _state->zlink.message_bus (), channel_snapshot, _state->handlers, _state->serializers,
          _state->application_job_queue));
    }
    if (!stream_snapshot.empty ()) {
        detail::configure_stream_dispatch_executor ();
        auto stream_runtime = detail::stream_runtime_t::from (_state->zlink);
        auto stream_service = std::make_unique<runtime::stream_host_service_t> (
          stream_runtime, stream_snapshot, options.stream_session_factories (),
          mesh_nodes.empty () ? nullptr : mesh_nodes.front (), stream_runtime.advertise_hosts (),
          _state->listener_statuses, _state->application_job_queue);
        stream_service->bind_drain_flag (_state->draining);
        stream_service->bind_monitoring (_state->monitoring);
        add_hosted_service (std::move (stream_service));
    }
    if (!http_snapshot.endpoints.empty ()) {
        add_hosted_service (std::make_unique<runtime::http_host_service_t> (
          http_snapshot, _state->health, options.handler_coroutine_workers ()));
    }
    runtime::configure_handler_coroutine_executor (options.handler_coroutine_workers ());
    detail::configure_handler_invocation_executor ();
    return *this;
}

app_t &app_t::add_module (module_t &module)
{
    module.configure_services (_state->services);
    module.configure_zlink (_state->zlink);
    module.configure_handlers (_state->handlers);
    return *this;
}

app_t &app_t::add_hosted_service (std::unique_ptr<hosted_service_t> service)
{
    if (!service) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "hosted service must not be null");
    }
    _state->hosted_services.push_back (std::move (service));
    return *this;
}

int app_t::run (int argc, char **argv)
{
    _state->config.load_cli (argc, argv);
    detail::serializer_registry_access_t::freeze (_state->serializers);
    _state->config.model ().set ("host.signal_handlers", "installed");
    g_stop_signal_requested = 0;
    std::signal (SIGINT, handle_process_signal);
    std::signal (SIGTERM, handle_process_signal);

    auto provider = _state->services.build_provider ();
    {
        std::lock_guard lock (_state->termination_teardown_mutex);
        if (_state->runtime_state.load (std::memory_order_acquire)
            == framework_runtime_state_t::stopped)
            return 0;
        _state->run_active = true;
        _state->teardown_complete = false;
    }
    std::vector<hosted_service_t *> started;
    try {
        _state->start_hosted_services (provider, started);
        auto expected = framework_runtime_state_t::preparing;
        (void) _state->runtime_state.compare_exchange_strong (
          expected, framework_runtime_state_t::serving, std::memory_order_acq_rel);
        try {
            runtime::runtime_metrics_t drain_metrics (_state->monitoring);
            if (drain_metrics.enabled ()) {
                drain_metrics.observable ("zlink.drain.state", "{state}", 1,
                                          {{"state", "serving"}});
            }
        }
        catch (...) {
        }
        while (!_state->stop_requested.load (std::memory_order_acquire)) {
            if (g_stop_signal_requested != 0) {
                g_stop_signal_requested = 0;
                (void) shutdown ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }
    catch (...) {
        _state->runtime_state.store (framework_runtime_state_t::error, std::memory_order_release);
        _state->stop_hosted_services (started);
        detail::channel_runtime_t::from (_state->zlink.message_bus ()).shutdown ();
        detail::drain_zlink_builder_runtime (_state->zlink);
        runtime::shutdown_handler_coroutine_executor ();
        detail::shutdown_stream_dispatch_executor ();
        detail::shutdown_handler_invocation_executor ();
        provider.close ();
        {
            std::lock_guard lock (_state->termination_teardown_mutex);
            _state->run_active = false;
            _state->teardown_complete = true;
        }
        _state->termination_teardown_changed.notify_all ();
        throw;
    }

    const bool trace_enabled = host_stop_trace_enabled ();
    _state->stop_hosted_services (started);
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=before-channel-runtime-shutdown" << std::endl;
    }
    detail::channel_runtime_t::from (_state->zlink.message_bus ()).shutdown ();
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-channel-runtime-shutdown" << std::endl;
        std::cerr << "zlink-cpp-host-stop stage=before-drain-runtime" << std::endl;
    }
    detail::drain_zlink_builder_runtime (_state->zlink);
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-drain-runtime" << std::endl;
        std::cerr << "zlink-cpp-host-stop stage=before-coroutine-executor-shutdown" << std::endl;
    }
    runtime::shutdown_handler_coroutine_executor ();
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-coroutine-executor-shutdown" << std::endl;
        std::cerr << "zlink-cpp-host-stop stage=before-stream-executor-shutdown" << std::endl;
    }
    detail::shutdown_stream_dispatch_executor ();
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-stream-executor-shutdown" << std::endl;
        std::cerr << "zlink-cpp-host-stop stage=before-handler-invocation-executor-shutdown"
                  << std::endl;
    }
    detail::shutdown_handler_invocation_executor ();
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-handler-invocation-executor-shutdown"
                  << std::endl;
        std::cerr << "zlink-cpp-host-stop stage=before-provider-close" << std::endl;
    }
    provider.close ();
    if (trace_enabled) {
        std::cerr << "zlink-cpp-host-stop stage=after-provider-close" << std::endl;
    }
    {
        std::lock_guard lock (_state->termination_teardown_mutex);
        _state->run_active = false;
        _state->teardown_complete = true;
    }
    _state->termination_teardown_changed.notify_all ();
    _state->runtime_state.store (framework_runtime_state_t::stopped, std::memory_order_release);
    return _state->stop_requested.load (std::memory_order_acquire) ? 0 : _state->exit_code;
}

namespace
{

enum class shutdown_force_reason_t
{
    deadline_exceeded,
    teardown_failed
};

struct shutdown_completed_t
{
};

struct shutdown_forced_t
{
    shutdown_force_reason_t reason;
};

using shutdown_progress_t = std::variant<shutdown_completed_t, shutdown_forced_t>;

const char *drain_state_name (detail::drain_state_t state) noexcept
{
    switch (state) {
        case detail::drain_state_t::serving:
            return "serving";
        case detail::drain_state_t::draining:
            return "draining";
        case detail::drain_state_t::stopped:
            return "stopped";
        case detail::drain_state_t::force_stopping:
            return "force_stopping";
    }
    return "unknown";
}

bool capacity_available_for_relocation (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved < static_cast<std::uint64_t> (usage.limit);
}

bool supports_relocation_source (const mesh_node_descriptor_t &source,
                                 const mesh_node_descriptor_t &candidate,
                                 std::int64_t target_application_version)
{
    if (candidate.state != framework_runtime_state_t::serving
        || candidate.object_role != object_role_t::server || candidate.placement_weight <= 0
        || candidate.application_version != target_application_version
        || (source.maintenance_wave && candidate.maintenance_wave == source.maintenance_wave)) {
        return false;
    }
    for (const auto &required : source.object_capabilities) {
        const auto supported = std::find_if (
          candidate.object_capabilities.begin (), candidate.object_capabilities.end (),
          [&] (const object_capability_t &value) {
              return value.object_kind == required.object_kind
                     && value.stable_type == required.stable_type
                     && (required.policy != maintenance_policy_kind_t::snapshot
                         || (value.policy == maintenance_policy_kind_t::snapshot
                             && value.has_snapshot_adapter));
          });
        if (supported == candidate.object_capabilities.end ())
            return false;
        if (required.object_kind == placement_object_kind_t::actor) {
            if (!capacity_available_for_relocation (candidate.capacity.actors))
                return false;
        } else {
            if (!capacity_available_for_relocation (candidate.capacity.spots))
                return false;
            const auto typed = std::find_if (
              candidate.capacity.spot_types.begin (), candidate.capacity.spot_types.end (),
              [&] (const spot_type_capacity_t &value) {
                  return value.object_kind == required.object_kind
                         && value.stable_type == required.stable_type;
              });
            if (typed != candidate.capacity.spot_types.end ()
                && !capacity_available_for_relocation (typed->usage)) {
                return false;
            }
        }
    }
    return candidate.activation_concurrency.limit <= 0
           || candidate.activation_concurrency.active
                < static_cast<std::uint32_t> (candidate.activation_concurrency.limit);
}

struct relocation_preflight_t
{
    std::optional<relocation_reason_t> blocker;
    std::int64_t effective_target_application_version = 0;
    std::int64_t source_application_version = 0;
};

relocation_preflight_t relocation_topology_preflight_once (detail::app_state_t &state,
                                                           const relocation_options_t &options)
{
    if (state.has_manual_service_topology && state.has_manual_service_topology ())
        return {relocation_reason_t::manual_topology_unsupported, 0};
    if (state.route_mesh_nodes.empty ())
        return {relocation_reason_t::target_unavailable, 0};

    try {
        auto provider = state.services.build_provider ();
        auto &store = provider.get_required<location_repository_t> ();
        auto &live = provider.get_required<runtime::live_location_reader_t> ();

        std::set<std::string> local_rids;
        std::optional<std::int64_t> source_application_version;
        for (const auto &node : state.route_mesh_nodes) {
            if (node) {
                if (const auto rid = node->routing_id ())
                    local_rids.insert (rid->to_hex ());
            }
        }

        for (const auto &node : state.route_mesh_nodes) {
            if (!node)
                continue;
            const auto local_rid = node->routing_id ();
            const auto status = node->status ();
            if (!local_rid || status.lifecycle_generation () == 0)
                return {relocation_reason_t::runtime_not_ready, 0};

            std::vector<mesh_node_descriptor_t> descriptors;
            location_page_request_t page;
            do {
                auto listed = store.list_mesh_nodes (node->mesh_name (), page).result ().value ();
                descriptors.insert (descriptors.end (),
                                    std::make_move_iterator (listed.items.begin ()),
                                    std::make_move_iterator (listed.items.end ()));
                page.continuation_token = std::move (listed.continuation_token);
            } while (page.continuation_token);

            const auto source = std::find_if (
              descriptors.begin (), descriptors.end (),
              [&] (const mesh_node_descriptor_t &descriptor) {
                  return descriptor.rid.to_hex () == local_rid->to_hex ()
                         && descriptor.lifecycle_generation == status.lifecycle_generation ();
              });
            if (source == descriptors.end () || !live.owner_admission_lifetime (source->owner_id)) {
                return {relocation_reason_t::store_unavailable, 0};
            }
            if (source_application_version
                && *source_application_version != source->application_version) {
                return {relocation_reason_t::state_incompatible, 0};
            }
            source_application_version = source->application_version;
            if (options.mode == relocation_mode_t::rolling_update
                && *options.target_application_version <= source->application_version) {
                throw std::invalid_argument (
                  "rolling update target application version must be greater "
                  "than the source version");
            }
            const auto target_application_version =
              options.mode == relocation_mode_t::planned_maintenance
                ? source->application_version
                : *options.target_application_version;

            const auto replacement = std::any_of (
              descriptors.begin (), descriptors.end (),
              [&] (const mesh_node_descriptor_t &candidate) {
                  return !local_rids.contains (candidate.rid.to_hex ())
                         && candidate.lifecycle_generation != 0
                         && live.owner_admission_lifetime (candidate.owner_id)
                         && supports_relocation_source (*source, candidate,
                                                        target_application_version)
                         && node->has_admitted_peer (candidate.rid, candidate.lifecycle_generation);
              });
            if (!replacement) {
                return {relocation_reason_t::target_unavailable, target_application_version};
            }
        }
        const auto effective = options.mode == relocation_mode_t::planned_maintenance
                                 ? *source_application_version
                                 : *options.target_application_version;
        return {std::nullopt, effective, *source_application_version};
    }
    catch (const std::invalid_argument &) {
        throw;
    }
    catch (...) {
        return {relocation_reason_t::store_unavailable, 0};
    }
}

std::chrono::milliseconds relocation_topology_poll_interval (detail::app_state_t &state) noexcept
{
    try {
        auto provider = state.services.build_provider ();
        if (auto runtime = provider.get<runtime::location_runtime_t> ()) {
            return std::max (runtime->get ().options ().polling_interval,
                             std::chrono::milliseconds (1));
        }
    }
    catch (...) {
    }
    return std::chrono::milliseconds (25);
}

void wait_for_relocation_topology_poll (std::chrono::steady_clock::time_point deadline_at,
                                        std::chrono::milliseconds polling_interval)
{
    std::this_thread::sleep_until (
      std::min (deadline_at, std::chrono::steady_clock::now () + polling_interval));
}

relocation_preflight_t
relocation_topology_preflight_until (detail::app_state_t &state,
                                     const relocation_options_t &options,
                                     std::chrono::steady_clock::time_point deadline_at)
{
    const auto polling_interval = relocation_topology_poll_interval (state);
    for (;;) {
        auto result = relocation_topology_preflight_once (state, options);
        if (result.blocker != relocation_reason_t::target_unavailable
            || std::chrono::steady_clock::now () >= deadline_at) {
            return result;
        }
        wait_for_relocation_topology_poll (deadline_at, polling_interval);
    }
}

template <typename Predicate>
std::optional<mesh_node_descriptor_t>
wait_for_relocation_target (runtime::store_location_resolvers_t &peers,
                            std::string_view mesh_name,
                            std::chrono::steady_clock::time_point deadline_at,
                            std::chrono::milliseconds polling_interval,
                            const std::function<bool ()> &shutdown_requested,
                            Predicate &&eligible,
                            relocation_reason_t &failure_reason)
{
    for (;;) {
        auto live = peers.list_live_mesh_nodes (std::string (mesh_name)).result ().value ();
        // Spec step 6: apply node-wide placement weight to the final eligible
        // candidate set with a deterministic weighted draw instead of taking the
        // first eligible node in store order. The candidate key pairs RID with
        // lifecycle generation so a manual fixed-RID node appearing at two
        // generations is not collapsed and the winner maps back to the exact
        // descriptor.
        const auto candidate_key = [] (const mesh_node_descriptor_t &candidate) {
            return candidate.rid.to_hex () + '\0' + std::to_string (candidate.lifecycle_generation);
        };
        std::vector<std::pair<std::string, std::uint32_t>> eligible_by_weight;
        for (const auto &candidate : live) {
            if (eligible (candidate) && candidate.placement_weight > 0) {
                eligible_by_weight.emplace_back (
                  candidate_key (candidate),
                  static_cast<std::uint32_t> (candidate.placement_weight));
            }
        }
        if (const auto chosen = select_weighted_relocation_target (eligible_by_weight)) {
            const auto target =
              std::find_if (live.begin (), live.end (), [&] (const auto &candidate) {
                  return candidate_key (candidate) == *chosen;
              });
            if (target != live.end ())
                return *target;
        }
        if (shutdown_requested ()) {
            failure_reason = relocation_reason_t::shutdown_requested;
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now () >= deadline_at) {
            failure_reason = relocation_reason_t::target_unavailable;
            return std::nullopt;
        }
        wait_for_relocation_topology_poll (deadline_at, polling_interval);
    }
}

bool publish_mesh_descriptor_state (detail::app_state_t &state,
                                    framework_runtime_state_t desired) noexcept
{
    bool published = true;
    for (const auto &service : state.hosted_services) {
        auto *lifecycle = detail::lifecycle_of (service.get ());
        if (lifecycle && !lifecycle->publish_descriptor_state (desired)) {
            published = false;
        }
    }
    return published;
}

std::vector<std::string> begin_application_relocation_readiness (detail::app_state_t &state)
{
    std::vector<std::string> meshes;
    for (const auto &service : state.hosted_services) {
        auto *lifecycle = detail::lifecycle_of (service.get ());
        if (!lifecycle)
            continue;
        lifecycle->visit_relocation_nodes ([&] (const auto &node) {
            if (!node
                || std::find (meshes.begin (), meshes.end (), node->mesh_name ()) != meshes.end ())
                return;
            auto runtime = detail::spot_node_runtime_t::from (state.zlink, node->mesh_name ());
            if (!runtime)
                return;
            runtime->begin_relocation_readiness ();
            meshes.push_back (node->mesh_name ());
        });
    }
    return meshes;
}

void cancel_application_relocation_readiness (detail::app_state_t &state,
                                              const std::vector<std::string> &meshes) noexcept
{
    for (const auto &mesh : meshes) {
        auto runtime = detail::spot_node_runtime_t::from (state.zlink, mesh);
        if (runtime)
            runtime->end_relocation_readiness ({});
    }
}

} // namespace

bool app_t::is_ready () const noexcept
{
    return runtime_state () == framework_runtime_state_t::serving;
}

framework_runtime_state_t app_t::runtime_state () const noexcept
{
    return _state->runtime_state.load (std::memory_order_acquire);
}

task_t<relocation_result_t> app_t::relocate (relocation_options_t options,
                                             std::stop_token wait_cancellation)
{
    if (options.mode == relocation_mode_t::planned_maintenance
        && options.target_application_version) {
        throw std::invalid_argument (
          "planned maintenance does not accept a target application version");
    }
    if (options.mode == relocation_mode_t::rolling_update && !options.target_application_version) {
        throw std::invalid_argument ("rolling update requires a target application version");
    }
    const auto deadline = options.deadline.value_or (std::chrono::seconds (30));
    if (deadline <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("relocation deadline must be greater than zero");
    options.deadline = deadline;
    const auto preflight_deadline_at = std::chrono::steady_clock::now () + deadline;

    auto &operation = _state->relocation_operation;
    std::thread completed_worker;
    {
        std::lock_guard lock (operation.mutex);
        if (!operation.started && operation.worker.joinable ()) {
            completed_worker = std::move (operation.worker);
        }
    }
    if (completed_worker.joinable ()) {
        completed_worker.join ();
    }

    relocation_preflight_t preflight;
    if (runtime_state () != framework_runtime_state_t::serving) {
        preflight.blocker = relocation_reason_t::runtime_not_ready;
    } else {
        preflight = relocation_topology_preflight_until (*_state, options, preflight_deadline_at);
        if (!preflight.blocker
            && (!_state->services.contains (std::type_index (typeid (relocation_repository_t)))
                || !_state->services.contains (std::type_index (typeid (location_repository_t))))) {
            preflight.blocker = relocation_reason_t::store_unavailable;
        }
    }

    std::shared_ptr<detail::app_state_t::relocation_waiter_t> waiter;
    task_t<relocation_result_t> task (result_t<relocation_result_t>::success ({}));
    {
        std::lock_guard lock (operation.mutex);
        if (operation.started && !operation.terminal) {
            // A concurrent call joins the running operation when it targets the
            // same mode and effective target application version. The deadline
            // is not part of the join key: the first call's deadline fixes the
            // shared operation, and a later joining call neither extends nor
            // shortens it.
            const bool joins_running_operation =
              operation.options.mode == options.mode
              && operation.options.target_application_version == options.target_application_version;
            if (!joins_running_operation) {
                // The rejected call's result reflects the valid option it
                // requested, not the running operation's target: rolling_update
                // requests its explicit target version, planned_maintenance
                // requests the source application version.
                const auto rejected_effective_target =
                  options.mode == relocation_mode_t::rolling_update
                    ? *options.target_application_version
                    : operation.source_application_version;
                return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
                  {options.mode, rejected_effective_target, relocation_outcome_t::blocked,
                   relocation_reason_t::operation_in_progress}));
            }
        } else if (operation.terminal) {
            return task_t<relocation_result_t> (
              result_t<relocation_result_t>::success (operation.result));
        } else {
            if (preflight.blocker) {
                return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
                  {options.mode, preflight.effective_target_application_version,
                   relocation_outcome_t::blocked, *preflight.blocker}));
            }
            const auto readiness_meshes = begin_application_relocation_readiness (*_state);
            if (!publish_mesh_descriptor_state (*_state, framework_runtime_state_t::relocating)) {
                (void) publish_mesh_descriptor_state (*_state, framework_runtime_state_t::serving);
                cancel_application_relocation_readiness (*_state, readiness_meshes);
                return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
                  {options.mode, preflight.effective_target_application_version,
                   relocation_outcome_t::blocked, relocation_reason_t::store_unavailable}));
            }
            operation.deadline = std::chrono::duration_cast<std::chrono::milliseconds> (
              preflight_deadline_at - std::chrono::steady_clock::now ());
            if (operation.deadline <= std::chrono::milliseconds::zero ()) {
                (void) publish_mesh_descriptor_state (*_state, framework_runtime_state_t::serving);
                cancel_application_relocation_readiness (*_state, readiness_meshes);
                return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
                  {options.mode, preflight.effective_target_application_version,
                   relocation_outcome_t::blocked, relocation_reason_t::target_unavailable}));
            }
            operation.started = true;
            operation.options = options;
            operation.source_application_version = preflight.source_application_version;
            operation.deadline_at = std::chrono::system_clock::now () + operation.deadline;
            operation.result.mode = options.mode;
            operation.result.effective_target_application_version =
              preflight.effective_target_application_version;
            _state->runtime_state.store (framework_runtime_state_t::relocating,
                                         std::memory_order_release);
            auto *state = _state.get ();
            operation.worker = std::thread ([state] {
                auto running = std::make_shared<task_t<void>> (run_shared_relocation (*state));
                detail::observe_task_completion (*running, [running] (const result_t<void> &) {});
            });
        }
        waiter = std::make_shared<detail::app_state_t::relocation_waiter_t> ();
        task = waiter->task ();
        operation.waiters.push_back (waiter);
    }
    waiter->arm (wait_cancellation);
    return task;
}

task_t<void> app_t::run_shared_relocation (detail::app_state_t &state)
{
    auto &operation = state.relocation_operation;
    const auto deadline_at = std::chrono::steady_clock::now () + operation.deadline;
    relocation_result_t terminal{
      operation.options.mode, operation.result.effective_target_application_version,
      relocation_outcome_t::blocked, relocation_reason_t::relocation_failed};
    std::vector<std::string> readiness_meshes;
    std::map<std::string, std::vector<spot_id_t>> relocated_ready_spots;

    auto shutdown_requested = [&] {
        std::lock_guard lock (operation.mutex);
        return operation.shutdown_requested;
    };
    auto complete = [&] (relocation_result_t result) {
        for (const auto &mesh_name : readiness_meshes) {
            auto runtime = detail::spot_node_runtime_t::from (state.zlink, mesh_name);
            if (runtime) {
                const auto found = relocated_ready_spots.find (mesh_name);
                runtime->end_relocation_readiness (
                  found != relocated_ready_spots.end () ? found->second : std::vector<spot_id_t>{});
            }
        }
        std::unique_lock lock (operation.mutex);
        const bool interrupted = operation.shutdown_requested;
        if (interrupted) {
            result.outcome = relocation_outcome_t::blocked;
            result.reason = relocation_reason_t::shutdown_requested;
        }
        if (result.outcome == relocation_outcome_t::relocated && !interrupted) {
            if (!publish_mesh_descriptor_state (state, framework_runtime_state_t::relocated)) {
                result.outcome = relocation_outcome_t::blocked;
                result.reason = relocation_reason_t::store_unavailable;
            }
        }
        if (result.outcome == relocation_outcome_t::relocated) {
            state.runtime_state.store (framework_runtime_state_t::relocated,
                                       std::memory_order_release);
        } else if (!interrupted) {
            (void) publish_mesh_descriptor_state (state, framework_runtime_state_t::serving);
            state.runtime_state.store (framework_runtime_state_t::serving,
                                       std::memory_order_release);
        }

        std::vector<std::shared_ptr<detail::app_state_t::relocation_waiter_t>> waiters;
        operation.terminal = result.outcome == relocation_outcome_t::relocated;
        operation.started = operation.terminal;
        if (!operation.terminal) {
            operation.shutdown_requested = false;
            operation.deadline_at.reset ();
        }
        operation.result = result;
        waiters = std::move (operation.waiters);
        operation.waiters.clear ();
        lock.unlock ();
        for (auto &waiter : waiters)
            waiter->complete (result);
    };

    try {
        auto provider = state.services.build_provider ();
        auto peers = provider.get<runtime::store_location_resolvers_t> ();
        auto location_store = provider.get<location_repository_t> ();
        if (!peers || !location_store) {
            terminal.reason = relocation_reason_t::store_unavailable;
            complete (terminal);
            co_return;
        }
        const auto topology_poll_interval = relocation_topology_poll_interval (state);

        std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> relocation_nodes;
        for (const auto &service : state.hosted_services) {
            if (auto *lifecycle = detail::lifecycle_of (service.get ()))
                lifecycle->visit_relocation_nodes ([&relocation_nodes] (const auto &node) {
                    if (node)
                        relocation_nodes.push_back (node);
                });
        }

        for (const auto &node : relocation_nodes) {
            auto spot_runtime = detail::spot_node_runtime_t::from (state.zlink, node->mesh_name ());
            if (!spot_runtime)
                continue;
            if (std::find (readiness_meshes.begin (), readiness_meshes.end (), node->mesh_name ())
                == readiness_meshes.end ()) {
                spot_runtime->begin_relocation_readiness ();
                readiness_meshes.push_back (node->mesh_name ());
            }
            const auto local_rid = node->routing_id ();
            auto application_units = spot_runtime->application_relocation_units ();
            while (std::any_of (application_units.begin (), application_units.end (),
                                [] (const auto &unit) { return !unit.ready; })) {
                if (shutdown_requested () || std::chrono::steady_clock::now () >= deadline_at) {
                    terminal.reason = shutdown_requested ()
                                        ? relocation_reason_t::shutdown_requested
                                        : relocation_reason_t::deadline_exceeded;
                    complete (terminal);
                    co_return;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                application_units = spot_runtime->application_relocation_units ();
            }

            // Source's own descriptor supplies the maintenance wave and the
            // declared relocation policy per capability for the candidate
            // narrowing below. Match RID *and* lifecycle generation exactly as
            // the preflight does, so a stale generation of a fixed-RID source is
            // never used to drive eligibility; and fail closed if the source
            // can't be read, matching the preflight's store_unavailable — never
            // proceed with a default descriptor, which would silently disable
            // the maintenance-wave and snapshot-policy gates.
            mesh_node_descriptor_t source_descriptor;
            {
                const auto source_generation = node->status ().lifecycle_generation ();
                auto live_source =
                  peers->get ().list_live_mesh_nodes (node->mesh_name ()).result ().value ();
                const auto found =
                  std::find_if (live_source.begin (), live_source.end (),
                                [&] (const mesh_node_descriptor_t &descriptor) {
                                    return local_rid
                                           && descriptor.rid.to_hex () == local_rid->to_hex ()
                                           && descriptor.lifecycle_generation == source_generation;
                                });
                if (found == live_source.end ()) {
                    terminal.reason = relocation_reason_t::store_unavailable;
                    complete (terminal);
                    co_return;
                }
                source_descriptor = *found;
            }

            std::set<std::string> aggregate_actor_ids;
            for (const auto &unit : application_units) {
                const auto target = wait_for_relocation_target (
                  peers->get (), node->mesh_name (), deadline_at, topology_poll_interval,
                  shutdown_requested,
                  [&] (const mesh_node_descriptor_t &peer) {
                      if (local_rid && peer.rid.to_hex () == local_rid->to_hex ())
                          return false;
                      std::vector<std::string> actor_types;
                      actor_types.reserve (unit.actors.size ());
                      for (const auto &actor : unit.actors)
                          actor_types.emplace_back (
                            ::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
                      // The actual target selection applies the same frozen
                      // candidate narrowing the preflight applies
                      // (supports_relocation_source): a serving Object Server on
                      // the effective version, excluded from source's non-empty
                      // maintenance wave, with relocation-policy/adapter
                      // compatibility, positive placement weight, and
                      // population/reservation capacity; and, like the preflight,
                      // the candidate's RID/generation must be an admitted Core
                      // peer.
                      return relocation_unit_target_eligible (
                               source_descriptor, peer,
                               terminal.effective_target_application_version, unit.spot_type,
                               actor_types)
                             && node->has_admitted_peer (peer.rid, peer.lifecycle_generation);
                  },
                  terminal.reason);
                if (!target) {
                    complete (terminal);
                    co_return;
                }

                std::vector<runtime::stateful::object_ref_t> sources;
                std::vector<std::string> stable_types;
                const auto spot_source =
                  node->native_node ().resolve_spot (std::string (unit.spot_id));
                if (!spot_source) {
                    terminal.reason = relocation_reason_t::state_incompatible;
                    complete (terminal);
                    co_return;
                }
                sources.push_back (*spot_source);
                stable_types.push_back (unit.spot_type);
                for (const auto &actor : unit.actors) {
                    const auto source = node->native_node ().resolve_actor (actor);
                    if (!source) {
                        terminal.reason = relocation_reason_t::state_incompatible;
                        complete (terminal);
                        co_return;
                    }
                    sources.push_back (*source);
                    stable_types.emplace_back (
                      ::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
                    aggregate_actor_ids.insert (std::string (actor.actor_id ().value ()));
                }

                std::vector<authority_snapshot_t> authorities;
                for (std::size_t index = 0; index != sources.size (); ++index) {
                    const auto &source = sources[index];
                    const auto authority_key =
                      source.kind == runtime::stateful::object_kind_t::actor
                        ? runtime::actor_authority_key (source.key)
                        : runtime::spot_authority_key (source.key);
                    const auto authority_read =
                      location_store->get ().read_authority (authority_key).result ().value ();
                    const auto *authority = std::get_if<authority_snapshot_t> (&authority_read);
                    if (!authority
                        || authority->allocation.state != placement_allocation_state_t::active
                        || authority->allocation.stable_type != stable_types[index]
                        || authority->object_generation != source.object_generation
                        || authority->authority_owner_generation
                             != source.authority_owner_generation) {
                        terminal.reason = relocation_reason_t::state_incompatible;
                        complete (terminal);
                        co_return;
                    }
                    authorities.push_back (*authority);
                }

                const auto moved = co_await node->relocate_application_unit (
                  std::move (sources), std::move (stable_types), *target, authorities);
                if (moved.terminal != runtime::stateful::relocation_terminal_t::completed) {
                    terminal.reason = relocation_reason_t::relocation_failed;
                    complete (terminal);
                    co_return;
                }
                relocated_ready_spots[node->mesh_name ()].push_back (unit.spot_id);
            }

            const auto actors = spot_runtime->local_actor_refs ();
            if (actors.empty ())
                continue;
            for (const auto &actor : actors) {
                if (aggregate_actor_ids.contains (std::string (actor.actor_id ().value ())))
                    continue;
                if (shutdown_requested ()) {
                    terminal.reason = relocation_reason_t::shutdown_requested;
                    complete (terminal);
                    co_return;
                }
                const auto now = std::chrono::steady_clock::now ();
                if (now >= deadline_at) {
                    terminal.reason = relocation_reason_t::deadline_exceeded;
                    complete (terminal);
                    co_return;
                }
                const auto target = wait_for_relocation_target (
                  peers->get (), node->mesh_name (), deadline_at, topology_poll_interval,
                  shutdown_requested,
                  [&] (const mesh_node_descriptor_t &peer) {
                      if (local_rid && peer.rid.to_hex () == local_rid->to_hex ())
                          return false;
                      // Single-Actor relocation target: same frozen narrowing as
                      // the shared path, with no user Spot requirement.
                      return relocation_unit_target_eligible (
                               source_descriptor, peer,
                               terminal.effective_target_application_version, {},
                               {std::string (
                                 ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                   actor))})
                             && node->has_admitted_peer (peer.rid, peer.lifecycle_generation);
                  },
                  terminal.reason);
                if (!target) {
                    complete (terminal);
                    co_return;
                }

                const auto authority_key =
                  runtime::actor_authority_key (actor.actor_id ().value ());
                const auto authority_read =
                  location_store->get ().read_authority (authority_key).result ().value ();
                const auto *authority = std::get_if<authority_snapshot_t> (&authority_read);
                if (!authority
                    || authority->allocation.state != placement_allocation_state_t::active
                    || authority->allocation.object_kind != placement_object_kind_t::actor
                    || authority->allocation.stable_type
                         != ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
                    || authority->object_generation != actor.object_generation ()
                    || authority->allocation.target.node_rid.value () != local_rid->to_string ()) {
                    terminal.reason = relocation_reason_t::state_incompatible;
                    complete (terminal);
                    co_return;
                }

                const auto moved =
                  co_await node->relocate_application_actor (actor, *target, *authority);
                if (moved.terminal != runtime::stateful::relocation_terminal_t::completed) {
                    terminal.reason = relocation_reason_t::relocation_failed;
                    complete (terminal);
                    co_return;
                }
            }
        }
        terminal.outcome = relocation_outcome_t::relocated;
        terminal.reason = relocation_reason_t::none;
    }
    catch (...) {
        terminal.reason = relocation_reason_t::store_unavailable;
    }
    complete (terminal);
    co_return;
}

task_t<termination_result_t> app_t::shutdown (std::chrono::milliseconds deadline,
                                              std::stop_token wait_cancellation)
{
    if (deadline <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("shutdown deadline must be greater than zero");
    {
        std::lock_guard relocation_lock (_state->relocation_operation.mutex);
        if (_state->relocation_operation.started && !_state->relocation_operation.terminal) {
            _state->relocation_operation.shutdown_requested = true;
        }
    }
    auto &operation = _state->termination_operation;
    std::shared_ptr<detail::app_state_t::termination_waiter_t> waiter;
    task_t<termination_result_t> task (result_t<termination_result_t>::success ({}));
    {
        std::lock_guard lock (operation.mutex);
        if (operation.terminal) {
            return task_t<termination_result_t> (
              result_t<termination_result_t>::success (operation.result));
        }
        if (!operation.started) {
            operation.started = true;
            operation.deadline = deadline;
            operation.deadline_at = std::chrono::system_clock::now () + deadline;
            _state->draining->store (true, std::memory_order_release);
            _state->runtime_state.store (framework_runtime_state_t::draining,
                                         std::memory_order_release);
            auto *state = _state.get ();
            operation.worker = std::thread ([state] { run_shared_shutdown (*state); });
        }
        waiter = std::make_shared<detail::app_state_t::termination_waiter_t> ();
        task = waiter->task ();
        operation.waiters.push_back (waiter);
    }
    waiter->arm (wait_cancellation);
    return task;
}

void app_t::run_shared_shutdown (detail::app_state_t &state) noexcept
{
    const auto started_at = std::chrono::steady_clock::now ();
    const auto deadline_at = started_at + state.termination_operation.deadline;
    auto monitoring = detail::monitoring_runtime_t (state.monitoring);
    auto emit_state = [&] (detail::drain_state_t drain_state) {
        try {
            monitoring.publish_drain (detail::drain_event_t{drain_state});
            runtime::runtime_metrics_t drain_metrics (state.monitoring);
            if (drain_metrics.enabled ()) {
                drain_metrics.observable ("zlink.drain.state", "{state}", 1,
                                          {{"state", drain_state_name (drain_state)}});
            }
        }
        catch (...) {
        }
    };

    emit_state (detail::drain_state_t::draining);

    shutdown_progress_t result = shutdown_completed_t{};
    bool force_state_emitted = false;
    auto force = [&] (shutdown_force_reason_t reason) {
        if (!std::holds_alternative<shutdown_completed_t> (result))
            return;
        result = shutdown_forced_t{reason};
        if (!force_state_emitted) {
            force_state_emitted = true;
            emit_state (detail::drain_state_t::force_stopping);
        }
    };

    while (std::chrono::steady_clock::now () < deadline_at) {
        bool relocation_active = false;
        {
            std::lock_guard lock (state.relocation_operation.mutex);
            relocation_active =
              state.relocation_operation.started && !state.relocation_operation.terminal;
        }
        if (!relocation_active)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    {
        std::lock_guard lock (state.relocation_operation.mutex);
        if (state.relocation_operation.started && !state.relocation_operation.terminal) {
            force (shutdown_force_reason_t::deadline_exceeded);
        }
    }

    for (const auto &service : state.hosted_services) {
        if (auto *lifecycle = detail::lifecycle_of (service.get ()))
            lifecycle->seal_application_dispatch ();
    }

    auto publish_draining_markers = [&] (bool wait_for_propagation) {
        bool marker_published = false;
        try {
            auto provider = state.services.build_provider ();
            if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                location_runtime->get ().set_draining (true);
                marker_published = location_runtime->get ().republish_peer_rows_draining ();
            } else {
                marker_published = true;
            }
        }
        catch (...) {
            marker_published = false;
        }
        while (std::chrono::steady_clock::now () < deadline_at && !marker_published) {
            std::this_thread::sleep_until (std::min (
              deadline_at, std::chrono::steady_clock::now () + std::chrono::milliseconds (100)));
            try {
                auto provider = state.services.build_provider ();
                if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                    marker_published = location_runtime->get ().republish_peer_rows_draining ();
                }
            }
            catch (...) {
            }
        }
        if (!marker_published) {
            force (shutdown_force_reason_t::teardown_failed);
            return;
        }

        if (!wait_for_propagation)
            return;
        const bool has_auto_connect = std::any_of (
          state.hosted_services.begin (), state.hosted_services.end (), [] (const auto &service) {
              const auto *lifecycle = detail::lifecycle_of (service.get ());
              return lifecycle && lifecycle->participates_in_drain_propagation ();
          });
        if (!has_auto_connect)
            return;
        try {
            auto provider = state.services.build_provider ();
            auto &location_runtime = provider.get_required<runtime::location_runtime_t> ();
            const auto propagation_bound = location_runtime.options ().polling_interval
                                           + std::chrono::seconds (5)
                                           + std::chrono::milliseconds (100);
            if (std::chrono::steady_clock::now () + propagation_bound > deadline_at) {
                std::this_thread::sleep_until (deadline_at);
                force (shutdown_force_reason_t::deadline_exceeded);
            } else {
                std::this_thread::sleep_for (propagation_bound);
            }
        }
        catch (...) {
            force (shutdown_force_reason_t::teardown_failed);
        }
    };

    /* Publish the draining state before waiting for accepted callbacks. A
     * caller must stop selecting this host while an already accepted handler
     * is still completing. */
    if (std::holds_alternative<shutdown_completed_t> (result)) {
        state.runtime_state.store (framework_runtime_state_t::draining, std::memory_order_release);
        publish_draining_markers (false);
        if (std::holds_alternative<shutdown_completed_t> (result)
            && !publish_mesh_descriptor_state (state, framework_runtime_state_t::draining)) {
            force (shutdown_force_reason_t::teardown_failed);
        }
    }

    /* Admission is sealed before this barrier. Each callback accepted before
     * the seal owns a pending/active count until its terminal reply or send
     * completion, so a normal request completion cannot close its Spot. */
    if (std::holds_alternative<shutdown_completed_t> (result)) {
        auto outbound_pending = [&state] () -> bool {
            try {
                return detail::channel_runtime_t::from (state.zlink.message_bus ()).pending_count ()
                       > 0;
            }
            catch (...) {
            }
            return false;
        };
        while (outbound_pending () && std::chrono::steady_clock::now () < deadline_at)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        if (outbound_pending ()) {
            force (shutdown_force_reason_t::deadline_exceeded);
        } else {
            for (const auto &service : state.hosted_services) {
                auto *lifecycle = detail::lifecycle_of (service.get ());
                if (lifecycle && !lifecycle->wait_for_accepted_callbacks_until (deadline_at)) {
                    force (shutdown_force_reason_t::deadline_exceeded);
                    break;
                }
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        for (const auto &service : state.hosted_services) {
            auto *lifecycle = detail::lifecycle_of (service.get ());
            if (lifecycle && !lifecycle->wait_for_accepted_callbacks_until (deadline_at)) {
                force (shutdown_force_reason_t::deadline_exceeded);
                break;
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        for (const auto &service : state.hosted_services) {
            auto *lifecycle = detail::lifecycle_of (service.get ());
            if (lifecycle && !lifecycle->drain_sessions_until (deadline_at)) {
                force (std::chrono::steady_clock::now () >= deadline_at
                         ? shutdown_force_reason_t::deadline_exceeded
                         : shutdown_force_reason_t::teardown_failed);
                break;
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        bool spots_closed = true;
        for (const auto &snapshot : detail::spot_node_runtime_t::snapshots (state.zlink)) {
            auto runtime = detail::spot_node_runtime_t::from (state.zlink, snapshot.name);
            if (runtime && !runtime->close_all_user_spots ()) {
                spots_closed = false;
                break;
            }
        }
        if (!spots_closed)
            force (std::chrono::steady_clock::now () >= deadline_at
                     ? shutdown_force_reason_t::deadline_exceeded
                     : shutdown_force_reason_t::teardown_failed);
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        try {
            auto provider = state.services.build_provider ();
            if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                if (!location_runtime->get ().cleanup_owner ()) {
                    force (shutdown_force_reason_t::teardown_failed);
                }
            }
        }
        catch (...) {
            force (shutdown_force_reason_t::teardown_failed);
        }
    }

    const bool force_stopped = std::holds_alternative<shutdown_forced_t> (result);
    if (force_stopped) {
        /* graceful-drain-handoff §7: active sessions receive the reason code
         * before forced teardown; the notification is bounded and never
         * blocks the terminal result indefinitely. */
        try {
            for (const auto &service : state.hosted_services) {
                if (auto *lifecycle = detail::lifecycle_of (service.get ())) {
                    lifecycle->force_close_sessions ();
                    runtime::runtime_metrics_t metrics (state.monitoring);
                    if (metrics.enabled ()) {
                        metrics.counter ("zlink.drain.forced", "{event}", 1, {{"kind", "session"}});
                    }
                }
            }
        }
        catch (...) {
        }
    }
    if (!force_stopped)
        emit_state (detail::drain_state_t::stopped);
    try {
        runtime::runtime_metrics_t drain_metrics (state.monitoring);
        if (drain_metrics.enabled ()) {
            const auto elapsed =
              std::chrono::duration<double> (std::chrono::steady_clock::now () - started_at)
                .count ();
            drain_metrics.histogram ("zlink.drain.duration", "s", elapsed,
                                     {{"outcome", force_stopped ? "force_stopped" : "drained"}});
        }
    }
    catch (...) {
    }

    termination_reason_t terminal_reason = termination_reason_t::none;
    if (const auto *forced = std::get_if<shutdown_forced_t> (&result)) {
        switch (forced->reason) {
            case shutdown_force_reason_t::deadline_exceeded:
                terminal_reason = termination_reason_t::deadline_exceeded;
                break;
            case shutdown_force_reason_t::teardown_failed:
                terminal_reason = termination_reason_t::teardown_failed;
                break;
        }
    }
    termination_result_t terminal{force_stopped ? termination_outcome_t::force_stopped
                                                : termination_outcome_t::stopped,
                                  terminal_reason};
    state.stop_requested.store (true, std::memory_order_release);
    bool teardown_completed = true;
    {
        std::unique_lock lock (state.termination_teardown_mutex);
        if (state.run_active) {
            teardown_completed = state.termination_teardown_changed.wait_until (
              lock, deadline_at, [&] { return state.teardown_complete; });
        }
    }
    if (!teardown_completed && terminal.outcome == termination_outcome_t::stopped) {
        terminal.outcome = termination_outcome_t::force_stopped;
        terminal.reason = termination_reason_t::deadline_exceeded;
    }
    std::vector<std::shared_ptr<detail::app_state_t::termination_waiter_t>> waiters;
    {
        std::lock_guard lock (state.termination_operation.mutex);
        if (!state.termination_operation.terminal) {
            state.termination_operation.terminal = true;
            state.termination_operation.result = terminal;
            waiters = std::move (state.termination_operation.waiters);
            state.termination_operation.waiters.clear ();
        }
    }
    for (auto &waiter : waiters) {
        waiter->complete (terminal);
    }
    state.runtime_state.store (framework_runtime_state_t::stopped, std::memory_order_release);
}

void app_t::stop () noexcept
{
    try {
        (void) shutdown ();
    }
    catch (...) {
        _state->stop_requested.store (true, std::memory_order_release);
    }
}

void app_t::request_stop () noexcept
{
    stop ();
}

} // namespace zlink::framework

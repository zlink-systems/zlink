/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/configuration/app.hpp>
#include "runtime/diagnostics/host_state_metric.hpp"
#include "runtime/timers/async_delay.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

class application_job_queue_t;
class host_capacity_runtime_t;
class hosted_service_lifecycle_t;
class listener_status_registry_t;
class mesh_node_host_service_t;

} // namespace zlink::framework::runtime

namespace zlink::framework::detail
{

class actor_gateway_runtime_t;
class channel_runtime_t;
class mesh_node_runtime_t;
class stream_runtime_t;
class app_state_t;

struct app_state_access_t
{
    mutable std::shared_mutex mutex;
    app_state_t *state = nullptr;
};

class app_state_t
{
  public:
    app_state_t ();
    ~app_state_t ();

    void start_hosted_services (service_provider_t &provider,
                                std::vector<hosted_service_t *> &started);
    static task_t<void> enter_serving_after_publication (std::shared_ptr<app_state_access_t> access,
                                                         task_t<bool> published);
    void close_framework_resources (service_provider_t &provider) noexcept;
    void stop_hosted_services (const std::vector<hosted_service_t *> &started) noexcept;

    template <typename TResult>
    struct lifecycle_waiter_t : public std::enable_shared_from_this<lifecycle_waiter_t<TResult>>
    {
        task_completion_source_t<TResult> completion;
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
            completion.complete (result_t<TResult>::success (std::move (result)));
        }

        void cancel ()
        {
            completion.complete (detail::result_access_t::failure<TResult> (
              detail::make_cancellation_exception ("lifecycle waiter was cancelled")));
        }
    };

    using relocation_waiter_t = lifecycle_waiter_t<relocation_result_t>;
    using termination_waiter_t = lifecycle_waiter_t<termination_result_t>;

    struct relocation_operation_t
    {
        std::mutex mutex;
        std::chrono::steady_clock::time_point metric_started{};
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

        // Caller holds mutex. Cancellation belongs to the returned waiter;
        // the source operation still owns its accepted terminal result.
        task_t<relocation_result_t>
        register_terminal_waiter_locked (std::shared_ptr<relocation_waiter_t> &waiter)
        {
            if (!started || terminal)
                return task_t<relocation_result_t> (
                  result_t<relocation_result_t>::success (result));
            waiter = std::make_shared<relocation_waiter_t> ();
            auto task = waiter->task ();
            waiters.push_back (waiter);
            return task;
        }

        ~relocation_operation_t ()
        {
            if (worker.joinable ())
                worker.join ();
        }
    };

    struct termination_operation_t
    {
        std::mutex mutex;
        std::chrono::steady_clock::time_point metric_started{};
        bool started = false;
        bool terminal = false;
        termination_result_t result{};
        std::chrono::milliseconds deadline{default_stop_timeout};
        std::optional<std::chrono::system_clock::time_point> deadline_at;
        std::vector<std::shared_ptr<termination_waiter_t>> waiters;
        std::thread worker;
        std::stop_source cleanup_cancellation;

        ~termination_operation_t ()
        {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    };

    std::shared_ptr<std::atomic_bool> draining = std::make_shared<std::atomic_bool> (false);
    runtime::host_state_metric_t runtime_state;
    std::function<task_t<bool> ()> startup_publication = [] {
        return task_t<bool> (result_t<bool>::success (true));
    };
    std::shared_ptr<app_state_access_t> status_access;
    std::mutex termination_teardown_mutex;
    delay_owner_t delays{termination_teardown_mutex};
    relocation_operation_t relocation_operation;
    termination_operation_t termination_operation;
    std::condition_variable termination_teardown_changed;
    std::size_t active_relocation_preflights = 0;
    bool run_active = false;
    std::shared_ptr<std::atomic_bool> run_started = std::make_shared<std::atomic_bool> (false);
    bool teardown_complete = false;

    class relocation_preflight_guard_t
    {
      public:
        explicit relocation_preflight_guard_t (app_state_t &state) : _state (&state)
        {
            std::lock_guard lock (_state->termination_teardown_mutex);
            ++_state->active_relocation_preflights;
        }

        relocation_preflight_guard_t (const relocation_preflight_guard_t &) = delete;
        relocation_preflight_guard_t &operator= (const relocation_preflight_guard_t &) = delete;

        ~relocation_preflight_guard_t () { release (); }

        void release () noexcept
        {
            if (!_state)
                return;
            {
                std::lock_guard lock (_state->termination_teardown_mutex);
                --_state->active_relocation_preflights;
            }
            _state->termination_teardown_changed.notify_all ();
            _state = nullptr;
        }

      private:
        app_state_t *_state;
    };

    void wait_relocation_cleanup ();
    void complete_termination (termination_result_t terminal);

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
    std::unique_ptr<zlink_framework_options_t> framework_options;
    std::optional<std::size_t> framework_hosted_service_position;
    bool framework_applied = false;
    std::vector<std::unique_ptr<hosted_service_t>> hosted_services;
    std::vector<std::shared_ptr<mesh_node_runtime_t>> route_mesh_nodes;
    std::function<bool ()> has_manual_service_topology;
    std::atomic_bool stop_requested = false;
    // Shared, runtime-mutable message-flow mode (set_message_flow_mode). Created
    // once here (never reassigned) so concurrent set/apply only touch the atomic,
    // not the shared_ptr. Installed into dispatch options at apply.
    std::shared_ptr<std::atomic<message_flow_log_mode_t>> message_flow_mode =
      std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::errors);
};

runtime::hosted_service_lifecycle_t *lifecycle_of (hosted_service_t *service) noexcept;
result_t<void> one_way_native_submit_result (zlink::submit_result_t result,
                                             std::string_view operation);

void configure_app_actor_gateway (
  app_state_t *_state,
  zlink_framework_options_t &options,
  actor_gateway_runtime_t &actor_gateway_runtime,
  runtime::mesh_node_host_service_t *mesh_node_service,
  const std::shared_ptr<mesh_node_runtime_t> &application_mesh,
  std::vector<std::weak_ptr<mesh_node_runtime_t>> &weak_mesh_nodes,
  const stream_runtime_t &stream_runtime,
  const std::chrono::seconds request_timeout,
  const std::vector<std::shared_ptr<mesh_node_builder_state_t>>::iterator
    &application_mesh_registration);

void configure_app_instance_spot (
  app_state_t *_state,
  zlink_framework_options_t &options,
  channel_runtime_t &channel_runtime,
  std::vector<std::shared_ptr<mesh_node_builder_state_t>> &mesh_node_registrations,
  std::vector<std::weak_ptr<mesh_node_runtime_t>> &weak_mesh_nodes);

void configure_app_store_repositories (app_state_t *_state);

} // namespace zlink::framework::detail

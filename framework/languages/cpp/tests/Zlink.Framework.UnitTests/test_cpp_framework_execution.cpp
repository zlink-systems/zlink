/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/timers/timer_runtime.hpp"

#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{

#ifndef ZLINK_SERIAL_EXECUTION_CONFORMANCE_PATH
#error "serial execution conformance fixture path is required"
#endif

#ifndef ZLINK_RUNTIME_OBSERVATION_CONFORMANCE_PATH
#error "runtime observation conformance fixture path is required"
#endif

const nlohmann::json &serial_execution_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_SERIAL_EXECUTION_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "serial execution conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    } ();
    return fixture;
}

const nlohmann::json &runtime_observation_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_RUNTIME_OBSERVATION_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "runtime observation conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    } ();
    return fixture;
}

class controlled_worker_scheduler_t final : public zlink::framework::detail::worker_scheduler_t
{
  public:
    bool try_schedule (std::function<void (std::stop_token)> work) override
    {
        if (queue_full) {
            return false;
        }
        std::lock_guard lock (mutex);
        worker_jobs.push (std::move (work));
        return true;
    }

    void post_owner (std::function<void ()> work) override
    {
        std::lock_guard lock (mutex);
        owner_jobs.push (std::move (work));
    }

    void run_worker_job ()
    {
        std::function<void (std::stop_token)> job;
        {
            std::lock_guard lock (mutex);
            job = std::move (worker_jobs.front ());
            worker_jobs.pop ();
        }
        job (cancellation.get_token ());
    }

    void run_owner_job ()
    {
        std::function<void ()> job;
        {
            std::lock_guard lock (mutex);
            job = std::move (owner_jobs.front ());
            owner_jobs.pop ();
        }
        job ();
    }

    std::size_t worker_job_count () const
    {
        std::lock_guard lock (mutex);
        return worker_jobs.size ();
    }

    std::size_t owner_job_count () const
    {
        std::lock_guard lock (mutex);
        return owner_jobs.size ();
    }

    std::stop_token stop_token () const noexcept override
    {
        return cancellation.get_token ();
    }

    void request_stop () noexcept
    {
        cancellation.request_stop ();
    }

    bool queue_full = false;
    mutable std::mutex mutex;
    std::queue<std::function<void (std::stop_token)>> worker_jobs;
    std::queue<std::function<void ()>> owner_jobs;
    std::stop_source cancellation;
};

class wire_actor_join_authority_store_t final :
    public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    std::optional<zlink::framework::authority_snapshot_t> spot_snapshot;

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority (zlink::framework::authority_key_t key,
                    std::stop_token) override
    {
        const auto &selected = key.value.starts_with ("zla1:s:") ? spot_snapshot : snapshot;
        if (selected) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{*selected}));
        }
        return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
          zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
            zlink::framework::authority_read_result_t{
              zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}));
    }
};

struct timer_activation_dependency_t
{
    timer_activation_dependency_t () { ++created; }
    ~timer_activation_dependency_t () { ++destroyed; }

    static inline std::atomic_int created{0};
    static inline std::atomic_int destroyed{0};
};

struct timer_activation_spot_t
{
};

struct timer_activation_handler_t
{
    using dependency_types =
      zlink::framework::dependency_list_t<timer_activation_dependency_t>;

    explicit timer_activation_handler_t (
      timer_activation_dependency_t &dependency) :
        dependency (&dependency)
    {
        ++created;
    }

    ~timer_activation_handler_t () { ++destroyed; }

    zlink::framework::task_t<void>
    handle (timer_activation_spot_t &,
            const zlink::framework::timer_tick_t &)
    {
        auto *expected = static_cast<timer_activation_dependency_t *> (nullptr);
        if (!observed_dependency.compare_exchange_strong (
              expected, dependency)
            && expected != dependency) {
            dependency_mismatch = true;
        }
        ++calls;
        co_return;
    }

    timer_activation_dependency_t *dependency;
    static inline std::atomic_int created{0};
    static inline std::atomic_int destroyed{0};
    static inline std::atomic_int calls{0};
    static inline std::atomic<timer_activation_dependency_t *>
      observed_dependency{nullptr};
    static inline std::atomic_bool dependency_mismatch{false};
};

bool verify_timer_handler_activation_lifetime ()
{
    timer_activation_dependency_t::created = 0;
    timer_activation_dependency_t::destroyed = 0;
    timer_activation_handler_t::created = 0;
    timer_activation_handler_t::destroyed = 0;
    timer_activation_handler_t::calls = 0;
    timer_activation_handler_t::observed_dependency = nullptr;
    timer_activation_handler_t::dependency_mismatch = false;

    zlink::framework::service_collection_t services;
    services.add_scoped<timer_activation_dependency_t> ();
    auto root = services.build_provider ();
    zlink::framework::serializer_registry_t serializers;

    auto run_activation = [&] {
        const auto handlers_before =
          timer_activation_handler_t::created.load ();
        const auto dependencies_before =
          timer_activation_dependency_t::created.load ();
        const auto calls_before =
          timer_activation_handler_t::calls.load ();
        timer_activation_handler_t::observed_dependency = nullptr;
        timer_activation_handler_t::dependency_mismatch = false;
        auto state =
          std::make_shared<zlink::framework::detail::spot_context_state_t> ();
        state->activation_scope =
          std::make_shared<zlink::framework::detail::service_scope_t> (
            zlink::framework::detail::service_scope_t::create (
              root,
              zlink::framework::detail::service_scope_kind_t::spot_activation));
        state->channel_runtime =
          std::make_shared<zlink::framework::detail::channel_runtime_state_t> ();
        state->channel_runtime->serializers = &serializers;
        state->spot_instance =
          std::make_shared<timer_activation_spot_t> ();

        auto context =
          zlink::framework::detail::spot_context_access_t::create (
            state);
        auto first =
          context.add_timer<timer_activation_handler_t> (
            "first", std::chrono::hours (24));
        auto second =
          context.add_timer<timer_activation_handler_t> (
            "second", std::chrono::hours (24));

        auto timer_runtime =
          zlink::framework::detail::timer_runtime_t::from (context);
        const auto first_result =
          timer_runtime.dispatch_fire_count_async (first, 1).result ();
        const auto second_result =
          timer_runtime.dispatch_fire_count_async (second, 1).result ();
        if (!first_result || !second_result) {
            std::cerr << "timer activation dispatch failed: "
                      << static_cast<int> (first_result.error_kind ()) << ", "
                      << static_cast<int> (second_result.error_kind ()) << '\n';
            return false;
        }
        zlink::framework::timer_options_t catch_up_options;
        catch_up_options.overrun_policy =
          zlink::framework::timer_overrun_policy_t::catch_up_bounded;
        catch_up_options.max_catch_up_ticks = 3;
        auto catch_up =
          context.add_timer<timer_activation_handler_t> (
            "catch-up", std::chrono::hours (24), catch_up_options);
        std::vector<zlink::framework::timer_tick_t> caught_up;
        const auto catch_up_result = timer_runtime.dispatch_fire_count (
          catch_up, 5,
          [&] (const zlink::framework::timer_tick_t &tick) {
              caught_up.push_back (tick);
          });
        if (!catch_up_result || caught_up.size () != 3
            || caught_up[0].scheduled_index != 3
            || caught_up[0].skipped_ticks != 2
            || caught_up[1].scheduled_index != 4
            || caught_up[2].scheduled_index != 5) {
            std::cerr << "bounded timer catch-up mismatch\n";
            return false;
        }
        for (int index = 0; index < 300; ++index) {
            if (!timer_runtime.dispatch_fire_count (catch_up, 1)) {
                return false;
            }
        }
        const auto tick_history =
          timer_runtime.delivered_ticks (catch_up);
        if (tick_history.size ()
              != zlink::framework::detail::timer_state_t::
                   observation_history_limit
            || tick_history.back ().scheduled_index != 305) {
            std::cerr << "timer observation history must stay bounded\n";
            return false;
        }

        bool oversized_catch_up_rejected = false;
        try {
            auto invalid_options = catch_up_options;
            invalid_options.max_catch_up_ticks =
              static_cast<std::uint64_t> (
                std::numeric_limits<int>::max ())
              + 1;
            (void) context.add_timer<timer_activation_handler_t> (
              "invalid-catch-up", std::chrono::hours (24),
              invalid_options);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            oversized_catch_up_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::protocol_error;
        }
        if (!oversized_catch_up_rejected) {
            std::cerr << "oversized timer catch-up count must be rejected\n";
            return false;
        }
        const auto reused =
          timer_activation_handler_t::created.load ()
              == handlers_before + 1
          && timer_activation_dependency_t::created.load ()
               == dependencies_before + 1
          && timer_activation_handler_t::calls.load ()
               >= calls_before + 2
          && timer_activation_handler_t::observed_dependency.load ()
               != nullptr
          && !timer_activation_handler_t::dependency_mismatch.load ();
        if (!reused) {
            std::cerr << "timer activation reuse mismatch: handlers="
                      << timer_activation_handler_t::created.load ()
                      << " deps="
                      << timer_activation_dependency_t::created.load ()
                      << " calls="
                      << timer_activation_handler_t::calls.load ()
                      << '\n';
            return false;
        }

        state->detach_application_instance (false);
        const auto released =
          timer_activation_handler_t::destroyed.load ()
            == timer_activation_handler_t::created.load ()
          && timer_activation_dependency_t::destroyed.load ()
               == timer_activation_dependency_t::created.load ();
        if (!released) {
            std::cerr << "timer activation release mismatch: handlers="
                      << timer_activation_handler_t::created.load () << "/"
                      << timer_activation_handler_t::destroyed.load ()
                      << " deps="
                      << timer_activation_dependency_t::created.load () << "/"
                      << timer_activation_dependency_t::destroyed.load ()
                      << '\n';
        }
        return released;
    };

    if (!run_activation ()
        || timer_activation_handler_t::created.load () != 1) {
        return false;
    }
    if (!run_activation ()
        || timer_activation_handler_t::created.load () != 2) {
        return false;
    }
    return timer_activation_handler_t::destroyed.load () == 2
           && timer_activation_dependency_t::destroyed.load () == 2;
}

bool verify_close_waits_for_timer_callback_barrier ()
{
    const auto handlers_created_before =
      timer_activation_handler_t::created.load ();
    const auto handlers_destroyed_before =
      timer_activation_handler_t::destroyed.load ();
    const auto dependencies_created_before =
      timer_activation_dependency_t::created.load ();
    const auto dependencies_destroyed_before =
      timer_activation_dependency_t::destroyed.load ();

    zlink::framework::service_collection_t services;
    services.add_scoped<timer_activation_dependency_t> ();
    auto root = services.build_provider ();
    auto state =
      std::make_shared<zlink::framework::detail::spot_context_state_t> ();
    state->node =
      std::make_shared<zlink::framework::detail::spot_node_builder_state_t> (
        "timer-close-race");
    state->spot_id = "timer-close-race-spot";
    state->spot_instance =
      std::make_shared<timer_activation_spot_t> ();
    state->activation_scope =
      std::make_shared<zlink::framework::detail::service_scope_t> (
        zlink::framework::detail::service_scope_t::create (
          root,
          zlink::framework::detail::service_scope_kind_t::spot_activation));
    auto handler = std::make_shared<timer_activation_handler_t> (
      state->activation_scope->provider ()
        .get_required<timer_activation_dependency_t> ());
    state->timer_handler_instances.emplace (
      std::type_index (typeid (timer_activation_handler_t)),
      handler);
    handler.reset ();

    auto context =
      zlink::framework::detail::spot_context_access_t::create (
        state);
    if (!state->enter_callback ()) {
        return false;
    }

    const auto first_close = context.close ().result ();
    const auto repeated_close = context.close ().result ();
    if (!first_close || !first_close.value ()
        || !repeated_close || !repeated_close.value ()
        || state->closed
        || timer_activation_handler_t::destroyed.load ()
             != handlers_destroyed_before
        || timer_activation_dependency_t::destroyed.load ()
             != dependencies_destroyed_before) {
        return false;
    }

    state->leave_callback ();
    if (!state->closed || state->activation_scope
        || !state->timer_handler_instances.empty ()
        || timer_activation_handler_t::created.load ()
             != handlers_created_before + 1
        || timer_activation_handler_t::destroyed.load ()
             != handlers_destroyed_before + 1
        || timer_activation_dependency_t::created.load ()
             != dependencies_created_before + 1
        || timer_activation_dependency_t::destroyed.load ()
             != dependencies_destroyed_before + 1) {
        return false;
    }

    const auto after_close = context.close ().result ();
    return after_close && !after_close.value ()
           && timer_activation_handler_t::destroyed.load ()
                == handlers_destroyed_before + 1
           && timer_activation_dependency_t::destroyed.load ()
                == dependencies_destroyed_before + 1;
}

zlink::framework::spot_context_t
context_with_scheduler (const std::shared_ptr<controlled_worker_scheduler_t> &scheduler)
{
    auto state = std::make_shared<zlink::framework::detail::spot_context_state_t> ();
    state->worker_scheduler = scheduler;
    return zlink::framework::detail::spot_context_access_t::create (
      state);
}

bool wait_until (const std::function<bool ()> &predicate)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return false;
}

zlink::framework::task_t<void> run_request_turn_probe (
  std::shared_ptr<zlink::framework::detail::task_completion_source_t<int>> reply,
  std::shared_ptr<std::vector<int>> order,
  std::shared_ptr<std::mutex> order_gate,
  bool release_turn)
{
    {
        std::lock_guard lock (*order_gate);
        order->push_back (1);
    }
    zlink::framework::request_call_t<int> call (
      "TurnProbe", [reply] (const auto &, auto, const auto &) { return reply->task (); });
    const auto value = release_turn ? co_await call.yield () : co_await call.submit ();
    if (value != 7) {
        throw std::runtime_error ("turn probe reply mismatch");
    }
    {
        std::lock_guard lock (*order_gate);
        order->push_back (3);
    }
    co_return;
}

bool verify_request_turn_mode (bool release_turn, const std::vector<int> &expected)
{
    zlink::framework::runtime::offload_executor_t executor (2);
    zlink::framework::runtime::serial_execution_queue_t queue (
      executor, 4,
      zlink::framework::runtime::serial_execution_queue_t::error_handler_t{},
      zlink::framework::runtime::serial_lane_policy_t::spot_wide ());
    auto reply = std::make_shared<zlink::framework::detail::task_completion_source_t<int>> ();
    auto order = std::make_shared<std::vector<int>> ();
    auto order_gate = std::make_shared<std::mutex> ();

    queue.post_async ("request", [reply, order, order_gate, release_turn] (auto complete) {
        auto task = std::make_shared<zlink::framework::task_t<void>> (
          run_request_turn_probe (reply, order, order_gate, release_turn));
        zlink::framework::observe_task_completion (
          *task, [task, complete = std::move (complete)] (const auto &result) mutable {
              complete ([task, result] {
                  if (!result) {
                      throw std::runtime_error ("turn probe request failed");
                  }
              });
          });
    });
    queue.post ("sibling", [order, order_gate] {
        std::lock_guard lock (*order_gate);
        order->push_back (2);
    });

    if (!wait_until ([&] {
            std::lock_guard lock (*order_gate);
            return release_turn ? order->size () >= 2 : order->size () == 1;
        })) {
        return false;
    }
    if (!release_turn) {
        std::lock_guard lock (*order_gate);
        if (*order != std::vector<int>{1}) {
            return false;
        }
    }
    reply->complete (zlink::framework::result_t<int>::success (7));
    if (!wait_until ([&] {
            std::lock_guard lock (*order_gate);
            return order->size () == 3;
        })) {
        return false;
    }
    queue.drain ();
    std::lock_guard lock (*order_gate);
    if (*order != expected) {
        std::cerr << "turn mode=" << (release_turn ? "yield" : "async") << " order=";
        for (const auto item : *order) {
            std::cerr << item;
        }
        std::cerr << '\n';
    }
    return *order == expected;
}

bool verify_serial_resume_capacity_failure_is_terminal_and_deferred ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_options_t options;
    options.application_message_capacity = 1;
    options.application_byte_capacity = serial_execution_queue_t::fixed_work_byte_cost;
    options.lifecycle_message_capacity = 1;
    options.lifecycle_byte_capacity = serial_execution_queue_t::fixed_work_byte_cost;
    serial_execution_queue_t queue (
      executor, options, {}, serial_lane_policy_t::spot_wide ());

    auto reply = std::make_shared<detail::task_completion_source_t<int>> ();
    auto task_finished = std::make_shared<std::atomic_bool> (false);
    auto observed_kind = std::make_shared<std::atomic_int> (-1);

    if (!queue.try_post_async (
          "resume-capacity",
          [reply, task_finished, observed_kind] (auto complete) {
              auto task = std::make_shared<task_t<void>> (
                run_request_turn_probe (
                  reply,
                  std::make_shared<std::vector<int>> (),
                  std::make_shared<std::mutex> (),
                  true));
              observe_task_completion (
                *task,
                [task, task_finished, observed_kind,
                 complete = std::move (complete)] (const auto &result) mutable {
                    if (!result) {
                        observed_kind->store (
                          static_cast<int> (result.error_kind ()),
                          std::memory_order_release);
                    }
                    task_finished->store (true, std::memory_order_release);
                    complete ([] {});
                });
          },
          serial_work_options_t{serial_work_lane_t::application,
                                serial_execution_queue_t::fixed_work_byte_cost})) {
        return false;
    }

    // The probe uses its own order vector above; the first turn must still
    // release before the filler can occupy the only application slot.
    std::mutex filler_gate;
    std::condition_variable filler_changed;
    bool filler_entered = false;
    bool release_filler = false;
    bool filler_posted = false;
    for (int attempt = 0; attempt < 100 && !filler_posted; ++attempt) {
        filler_posted = queue.try_post (
          "resume-filler",
          [&] {
              std::unique_lock lock (filler_gate);
              filler_entered = true;
              filler_changed.notify_all ();
              filler_changed.wait (lock, [&] { return release_filler; });
          },
          serial_work_options_t{serial_work_lane_t::application,
                                serial_execution_queue_t::fixed_work_byte_cost});
        if (!filler_posted)
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    if (!filler_posted) {
        queue.cancel_pending ();
        return false;
    }
    {
        std::unique_lock lock (filler_gate);
        if (!filler_changed.wait_for (
              lock, std::chrono::seconds (1), [&] { return filler_entered; })) {
            queue.cancel_pending ();
            return false;
        }
    }

    reply->complete (result_t<int>::success (7));
    if (!wait_until ([&] {
            return task_finished->load (std::memory_order_acquire);
        })) {
        {
            std::lock_guard lock (filler_gate);
            release_filler = true;
        }
        filler_changed.notify_all ();
        queue.drain ();
        return false;
    }
    const auto terminal = observed_kind->load (std::memory_order_acquire);
    if (terminal
        != static_cast<int> (framework_error_kind_t::capacity_exceeded)) {
        {
            std::lock_guard lock (filler_gate);
            release_filler = true;
        }
        filler_changed.notify_all ();
        queue.drain ();
        return false;
    }

    {
        std::lock_guard lock (filler_gate);
        release_filler = true;
    }
    filler_changed.notify_all ();
    queue.drain ();
    return true;
}

bool verify_serial_queue_lanes_and_byte_budget ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_options_t options;
    options.application_message_capacity = 4;
    options.application_byte_capacity = 512;
    options.lifecycle_message_capacity = 2;
    options.lifecycle_byte_capacity = 512;
    options.lifecycle_burst_limit = 2;
    options.owner_time_budget = std::chrono::milliseconds::zero ();
    serial_execution_queue_t queue (executor, options);

    std::mutex gate;
    std::condition_variable changed;
    bool first_entered = false;
    bool release_first = false;
    std::vector<std::string> order;

    if (!queue.try_post ("application-first", [&] {
            {
                std::lock_guard lock (gate);
                first_entered = true;
                changed.notify_all ();
            }
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return release_first; });
            order.push_back ("application-first");
        }, serial_work_options_t{serial_work_lane_t::application, 256})) {
        return false;
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                              [&] { return first_entered; })) {
            release_first = true;
            changed.notify_all ();
            return false;
        }
    }

    const auto application = serial_work_options_t{
      serial_work_lane_t::application, 256};
    const auto lifecycle = serial_work_options_t{
      serial_work_lane_t::lifecycle, 256};
    if (!queue.try_post ("application-second", [&] {
            order.push_back ("application-second");
        }, application)
        || !queue.try_post ("lifecycle-first", [&] {
               order.push_back ("lifecycle-first");
           }, lifecycle)
        || !queue.try_post ("lifecycle-second", [&] {
               order.push_back ("lifecycle-second");
           }, lifecycle)
        || queue.try_post ("application-over-byte-limit", [] {}, application)
        || queue.try_post ("lifecycle-over-message-limit", [] {}, lifecycle)) {
        return false;
    }
    if (queue.pending_count (serial_work_lane_t::application) != 2
        || queue.pending_count (serial_work_lane_t::lifecycle) != 2
        || queue.pending_bytes () != 1024) {
        return false;
    }

    {
        std::lock_guard lock (gate);
        release_first = true;
        changed.notify_all ();
    }
    queue.drain ();
    return order
             == std::vector<std::string>{"application-first", "lifecycle-first",
                                         "lifecycle-second", "application-second"}
           && queue.pending_count () == 0
           && queue.pending_bytes () == 0;
}

bool verify_host_reserved_application_bypasses_owner_capacity ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (1);
    serial_execution_queue_options_t options;
    options.application_message_capacity = 1;
    options.application_byte_capacity =
      serial_execution_queue_t::fixed_work_byte_cost;
    serial_execution_queue_t queue (executor, options);

    std::mutex gate;
    std::condition_variable changed;
    std::optional<serial_execution_queue_t::async_completion_t>
      complete_active;
    bool active_entered = false;
    bool reserved_follower_ran = false;
    if (!queue.try_post_async (
          "owner-cap-active",
          [&] (auto complete) {
              std::lock_guard lock (gate);
              complete_active.emplace (std::move (complete));
              active_entered = true;
              changed.notify_all ();
          })) {
        return false;
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                              [&] { return active_entered; })) {
            return false;
        }
    }

    const serial_work_options_t host_reserved{
      serial_work_lane_t::application,
      serial_execution_queue_t::fixed_work_byte_cost,
      true};
    if (!queue.try_post (
          "host-capacity-reserved",
          [&] { reserved_follower_ran = true; }, host_reserved)
        || queue.pending_count (serial_work_lane_t::application) != 2) {
        if (complete_active) {
            (*complete_active) ([] {});
        }
        queue.drain ();
        return false;
    }
    (*complete_active) ([] {});
    queue.drain ();
    return reserved_follower_ran && queue.pending_count () == 0;
}

bool verify_serial_queue_owner_time_budget ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (1);
    serial_execution_queue_options_t options;
    options.application_message_capacity = 8;
    options.application_byte_capacity = 8 * serial_execution_queue_t::fixed_work_byte_cost;
    options.lifecycle_message_capacity = 8;
    options.lifecycle_byte_capacity = 8 * serial_execution_queue_t::fixed_work_byte_cost;
    options.owner_time_budget = std::chrono::milliseconds (100);
    serial_execution_queue_t batched_queue (executor, options);

    std::vector<int> batched_order;
    for (int value = 1; value <= 4; ++value) {
        if (!batched_queue.try_post ("budget-batch", [&batched_order, value] {
                batched_order.push_back (value);
            })) {
            return false;
        }
    }
    batched_queue.drain ();
    if (batched_order != std::vector<int>{1, 2, 3, 4}
        || batched_queue.pending_count () != 0) {
        return false;
    }

    options.owner_time_budget = std::chrono::milliseconds (1);
    serial_execution_queue_t expiring_queue (executor, options);
    std::vector<int> expiring_order;
    if (!expiring_queue.try_post ("budget-expired", [&expiring_order] {
            std::this_thread::sleep_for (std::chrono::milliseconds (3));
            expiring_order.push_back (1);
        })
        || !expiring_queue.try_post ("budget-after-expiry", [&expiring_order] {
               expiring_order.push_back (2);
           })) {
        return false;
    }
    expiring_queue.drain ();
    return expiring_order == std::vector<int>{1, 2}
           && expiring_queue.pending_count () == 0;
}

bool verify_cancellable_serial_submission_lifecycle ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    // If the executor rejects the drain job, enqueue rollback removes exactly
    // one item and restores both capacity counters for the next admission.
    {
        offload_executor_t executor (1);
        executor.drain ();
        serial_execution_queue_options_t options;
        options.application_message_capacity = 1;
        options.application_byte_capacity =
          serial_execution_queue_t::fixed_work_byte_cost + 37;
        serial_execution_queue_t queue (executor, options);
        const serial_work_options_t work_options{
          serial_work_lane_t::application,
          serial_execution_queue_t::fixed_work_byte_cost + 37};
        std::atomic_int work_runs = 0;
        std::atomic_int cancel_calls = 0;

        const bool first_accepted = queue.try_post (
          "rejected-drain-job", [&] { ++work_runs; }, work_options);
        const bool first_rollback_clean =
          queue.pending_count (serial_work_lane_t::application) == 0
          && queue.pending_bytes () == 0;
        const auto second = queue.try_post_cancellable_async (
          "rejected-drain-job-after-rollback",
          [&] (auto complete) {
              ++work_runs;
              complete ([] {});
          },
          [&] { ++cancel_calls; }, work_options);
        const bool second_rollback_clean =
          queue.pending_count (serial_work_lane_t::application) == 0
          && queue.pending_bytes () == 0;
        queue.drain ();
        if (first_accepted || !first_rollback_clean || second
            || second.error_kind () != framework_error_kind_t::shutting_down
            || !second_rollback_clean || work_runs.load () != 0
            || cancel_calls.load () != 0) {
            return false;
        }
    }

    // A queued cancellation removes its reservation before the stop callback
    // runs, so a capacity-one lane can accept its replacement immediately.
    {
        offload_executor_t executor (1);
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_options_t options;
        options.application_message_capacity = 1;
        options.application_byte_capacity =
          serial_execution_queue_t::fixed_work_byte_cost;
        serial_execution_queue_t queue (executor, options);
        std::atomic_int cancelled = 0;
        std::atomic_int cancelled_work_runs = 0;
        std::atomic_int replacement_runs = 0;
        const auto submission = queue.try_post_cancellable_async (
          "queued-cancellable",
          [&] (auto complete) {
              ++cancelled_work_runs;
              complete ([] {});
          },
          [&] { ++cancelled; });
        const bool accepted = submission.has_value ();
        const auto outcome = accepted
          ? queue.cancel_submission (submission.value ())
          : serial_cancel_submission_outcome_t::already_terminal;
        const bool replacement_accepted = queue.try_post (
          "replacement-after-unlink", [&] { ++replacement_runs; });

        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!accepted
            || outcome
                 != serial_cancel_submission_outcome_t::queued_cancelled
            || !replacement_accepted || cancelled.load () != 1
            || cancelled_work_runs.load () != 0
            || replacement_runs.load () != 1
            || queue.pending_count () != 0) {
            return false;
        }
    }

    // An active cancellation requests cooperative stop. The active turn and
    // its capacity reservation remain until work acknowledges completion.
    {
        offload_executor_t executor (1);
        serial_execution_queue_options_t options;
        options.application_message_capacity = 2;
        options.application_byte_capacity =
          2 * serial_execution_queue_t::fixed_work_byte_cost;
        serial_execution_queue_t queue (executor, options);
        std::mutex gate;
        std::condition_variable changed;
        std::optional<serial_execution_queue_t::async_completion_t> acknowledge;
        bool entered = false;
        bool follower_ran = false;
        std::atomic_int stop_requests = 0;
        const auto submission = queue.try_post_cancellable_async (
          "active-cancellable",
          [&] (auto complete) {
              std::lock_guard lock (gate);
              entered = true;
              acknowledge.emplace (std::move (complete));
              changed.notify_all ();
          },
          [&] {
              ++stop_requests;
              changed.notify_all ();
          });
        if (!submission) {
            return false;
        }
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                queue.cancel_pending ();
                return false;
            }
        }
        if (!queue.try_post ("follower-after-cancel", [&] {
                std::lock_guard lock (gate);
                follower_ran = true;
                changed.notify_all ();
            })) {
            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*acknowledge);
            }
            finish ([] {});
            queue.drain ();
            return false;
        }

        const auto first_cancel = queue.cancel_submission (submission.value ());
        const auto repeated_cancel = queue.cancel_submission (submission.value ());
        bool follower_ran_before_ack = false;
        {
            std::unique_lock lock (gate);
            follower_ran_before_ack = changed.wait_for (
              lock, std::chrono::milliseconds (50),
              [&] { return follower_ran; });
        }
        serial_execution_queue_t::async_completion_t finish;
        {
            std::lock_guard lock (gate);
            finish = std::move (*acknowledge);
        }
        finish ([] {});
        queue.drain ();
        if (first_cancel
              != serial_cancel_submission_outcome_t::active_cancel_requested
            || repeated_cancel
                 != serial_cancel_submission_outcome_t::active_cancel_requested
            || follower_ran_before_ack || !follower_ran
            || stop_requests.load () != 1
            || queue.cancel_submission (submission.value ())
                 != serial_cancel_submission_outcome_t::already_terminal) {
            return false;
        }
    }

    // Shutdown removes queued followers but does not consume a generic async
    // turn's completion. The original work remains the only owner that can
    // acknowledge and release its active reservation.
    {
        offload_executor_t executor (1);
        serial_execution_queue_t queue (executor, 2);
        std::mutex gate;
        std::condition_variable changed;
        std::optional<serial_execution_queue_t::async_completion_t> acknowledge;
        bool entered = false;
        std::atomic_int follower_runs = 0;
        std::atomic_int acknowledged_completions = 0;
        if (!queue.try_post_async (
              "generic-active-during-cancel-pending",
              [&] (auto complete) {
                  std::lock_guard lock (gate);
                  acknowledge.emplace (std::move (complete));
                  entered = true;
                  changed.notify_all ();
              })) {
            return false;
        }
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                return false;
            }
        }
        if (!queue.try_post (
              "generic-follower-during-cancel-pending",
              [&] { ++follower_runs; })) {
            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*acknowledge);
            }
            finish ([] {});
            queue.drain ();
            return false;
        }

        queue.cancel_pending ();
        const bool active_reservation_retained = queue.pending_count () == 1;
        serial_execution_queue_t::async_completion_t finish;
        {
            std::lock_guard lock (gate);
            finish = std::move (*acknowledge);
        }
        finish ([&] { ++acknowledged_completions; });
        queue.drain ();
        if (!active_reservation_retained || !queue.closed ()
            || follower_runs.load () != 0
            || acknowledged_completions.load () != 1
            || queue.pending_count () != 0) {
            return false;
        }
    }

    // Queue shutdown cancels queued cancellable submissions and makes their
    // identifiers terminal without running their work.
    {
        offload_executor_t executor (1);
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_t queue (executor, 1);
        std::atomic_int cancelled = 0;
        std::atomic_int work_runs = 0;
        const auto submission = queue.try_post_cancellable_async (
          "cancelled-by-shutdown",
          [&] (auto complete) {
              ++work_runs;
              complete ([] {});
          },
          [&] { ++cancelled; });
        if (!submission) {
            {
                std::lock_guard lock (worker_gate);
                release_worker = true;
            }
            worker_changed.notify_all ();
            return false;
        }
        queue.cancel_pending ();
        const auto terminal = queue.cancel_submission (submission.value ());
        const bool rejected_after_close = !queue.try_post ("closed", [] {});
        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!queue.closed () || !rejected_after_close
            || terminal
                 != serial_cancel_submission_outcome_t::already_terminal
            || cancelled.load () != 1 || work_runs.load () != 0) {
            return false;
        }
    }

    // Queued deferred barriers receive cancellation before shutdown removes
    // their work, so activation becomes terminal and drain still converges.
    {
        offload_executor_t executor (1);
        std::mutex worker_gate;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        if (!executor.try_submit_internal ([&] {
                std::unique_lock lock (worker_gate);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_gate);
            if (!worker_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return worker_entered; })) {
                release_worker = true;
                worker_changed.notify_all ();
                return false;
            }
        }

        serial_execution_queue_t queue (executor, 2);
        auto join_barrier = queue.reserve_barrier_next (
          "queued-join-barrier-cancellation");
        auto handoff_barrier = queue.reserve_handoff_barrier (
          "queued-handoff-barrier-cancellation");
        queue.cancel_pending ();
        const auto join_activation = join_barrier
          ? join_barrier.value ()->activate ([] {})
          : result_t<void>::success ();
        const auto handoff_activation = handoff_barrier
          ? handoff_barrier.value ()->activate ([] {})
          : result_t<void>::success ();
        {
            std::lock_guard lock (worker_gate);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue.drain ();
        if (!join_barrier || !handoff_barrier || join_activation
            || handoff_activation
            || join_activation.error_kind ()
                 != framework_error_kind_t::invalid_operation
            || handoff_activation.error_kind ()
                 != framework_error_kind_t::invalid_operation
            || queue.pending_count () != 0) {
            return false;
        }
    }

    // Completing and then throwing reports the late exception without a
    // second queue release. Each work callback receives only its own turn.
    {
        offload_executor_t executor (1);
        std::atomic_int errors = 0;
        std::atomic_int completions = 0;
        std::atomic_int followers = 0;
        std::atomic_bool first_had_turn = false;
        std::atomic_bool completion_had_turn = false;
        std::atomic_bool follower_had_turn = false;
        serial_execution_queue_t queue (
          executor, 2,
          [&] (const std::string &name, const std::exception_ptr &error) {
              if (name != "throw-after-complete" || !error)
                  return;
              try {
                  std::rethrow_exception (error);
              }
              catch (const std::runtime_error &failure) {
                  if (std::string_view (failure.what ())
                      == "throw-after-complete") {
                      ++errors;
                  }
              }
          });
        if (!queue.try_post_async (
              "throw-after-complete",
              [&] (auto complete) {
                  first_had_turn = static_cast<bool> (
                    detail::capture_current_serial_turn ());
                  complete ([&] {
                      completion_had_turn = static_cast<bool> (
                        detail::capture_current_serial_turn ());
                      ++completions;
                  });
                  throw std::runtime_error ("throw-after-complete");
              })
            || !queue.try_post ("after-throw", [&] {
                   follower_had_turn = static_cast<bool> (
                     detail::capture_current_serial_turn ());
                   ++followers;
               })) {
            queue.cancel_pending ();
            return false;
        }
        queue.drain ();
        if (errors.load () != 1 || completions.load () != 1
            || followers.load () != 1 || !first_had_turn.load ()
            || completion_had_turn.load () || !follower_had_turn.load ()
            || detail::capture_current_serial_turn ()
            || queue.pending_count () != 0) {
            return false;
        }
    }

    return true;
}

bool verify_spot_serial_task_async_shutdown_settlement ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // A queued lifecycle callback is removed by shutdown and settles its
    // observer exactly once without invoking application work.
    {
        auto executor = std::make_shared<runtime::offload_executor_t> (
          1, 8, "spot-serial-queued-cancel");
        std::mutex worker_mutex;
        std::condition_variable worker_changed;
        bool worker_entered = false;
        bool release_worker = false;
        if (!executor->try_submit_internal ([&] {
                std::unique_lock lock (worker_mutex);
                worker_entered = true;
                worker_changed.notify_all ();
                worker_changed.wait (lock, [&] { return release_worker; });
            })) {
            return false;
        }
        {
            std::unique_lock lock (worker_mutex);
            if (!worker_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return worker_entered; })) {
                return false;
            }
        }
        auto owner = std::make_shared<spot_context_state_t> ();
        owner->serial_executor = executor;
        owner->serial_queue =
          std::make_shared<runtime::serial_execution_queue_t> (
            *executor, runtime::serial_execution_queue_options_t{});
        auto queue = owner->serial_queue;
        std::mutex result_mutex;
        std::condition_variable result_changed;
        std::optional<result_t<void>> result;
        std::atomic_int work_calls = 0;
        std::atomic_int completion_calls = 0;
        owner->run_serial_task_async (
          "queued-spot-lifecycle-cancel",
          [&] () -> task_t<void> {
              ++work_calls;
              co_return;
          },
          [&] (result_t<void> value) {
              ++completion_calls;
              {
                  std::lock_guard lock (result_mutex);
                  result.emplace (std::move (value));
              }
              result_changed.notify_all ();
          });
        queue->cancel_pending ();
        {
            std::unique_lock lock (result_mutex);
            if (!result_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return result.has_value (); })) {
                return false;
            }
        }
        {
            std::lock_guard lock (worker_mutex);
            release_worker = true;
        }
        worker_changed.notify_all ();
        queue->drain ();
        if (*result || result->error_kind ()
                         != framework_error_kind_t::shutting_down
            || work_calls.load () != 0 || completion_calls.load () != 1) {
            return false;
        }
    }

    // Active cancellation is cooperative: the owner remains valid and the
    // observer remains pending until the callback task acknowledges terminal.
    {
        auto executor = std::make_shared<runtime::offload_executor_t> (
          1, 8, "spot-serial-active-cancel");
        auto owner = std::make_shared<spot_context_state_t> ();
        owner->serial_executor = executor;
        owner->serial_queue =
          std::make_shared<runtime::serial_execution_queue_t> (
            *executor, runtime::serial_execution_queue_options_t{});
        auto queue = owner->serial_queue;
        auto callback_terminal =
          std::make_shared<task_completion_source_t<void>> ();
        std::mutex gate;
        std::condition_variable changed;
        bool entered = false;
        std::optional<result_t<void>> result;
        std::atomic_int completion_calls = 0;
        owner->run_serial_task_async (
          "active-spot-lifecycle-cancel",
          [callback_terminal, &gate, &changed,
           &entered] () -> task_t<void> {
              {
                  std::lock_guard lock (gate);
                  entered = true;
              }
              changed.notify_all ();
              co_await callback_terminal->task ();
          },
          [&] (result_t<void> value) {
              ++completion_calls;
              {
                  std::lock_guard lock (gate);
                  result.emplace (std::move (value));
              }
              changed.notify_all ();
          });
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                return false;
            }
        }
        std::weak_ptr<spot_context_state_t> weak_owner = owner;
        queue->cancel_pending ();
        owner.reset ();
        {
            std::unique_lock lock (gate);
            if (changed.wait_for (
                  lock, std::chrono::milliseconds (50),
                  [&] { return result.has_value (); })
                || weak_owner.expired ()) {
                return false;
            }
        }
        callback_terminal->complete (result_t<void>::success ());
        {
            std::unique_lock lock (gate);
            if (!changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return result.has_value (); })) {
                return false;
            }
        }
        queue->drain ();
        queue.reset ();
        if (*result || result->error_kind ()
                         != framework_error_kind_t::shutting_down
            || completion_calls.load () != 1 || !weak_owner.expired ()) {
            return false;
        }
    }
    return true;
}

bool verify_common_dispatch_limits ()
{
    using namespace zlink::framework::runtime;
    const auto &fixture = serial_execution_fixture ();
    const auto &limits = fixture.at ("limits");
    const serial_execution_queue_options_t queue_options;
    const receive_batch_budget_t receive_options;
    return fixture.at ("fixture") == "zlink.framework.serial-execution"
           && fixture.at ("version") == 1
           && queue_options.application_message_capacity
                == limits.at ("application").at ("messageCapacity")
           && queue_options.application_message_capacity
                == dispatch_limits::application_mailbox_messages
           && queue_options.application_byte_capacity
                == limits.at ("application").at ("byteCapacity")
           && queue_options.application_byte_capacity
                == dispatch_limits::application_mailbox_bytes
           && queue_options.lifecycle_message_capacity
                == limits.at ("lifecycle").at ("messageCapacity")
           && queue_options.lifecycle_message_capacity
                == dispatch_limits::control_mailbox_messages
           && queue_options.lifecycle_byte_capacity
                == limits.at ("lifecycle").at ("byteCapacity")
           && queue_options.lifecycle_byte_capacity
                == dispatch_limits::control_mailbox_bytes
           && queue_options.owner_time_budget
                == std::chrono::milliseconds (
                  limits.at ("ownerTimeBudgetMilliseconds")
                    .get<std::int64_t> ())
           && queue_options.owner_time_budget
                == dispatch_limits::owner_time_budget
           && queue_options.lifecycle_burst_limit
                == limits.at ("lifecycleBurstLimit")
           && queue_options.lifecycle_burst_limit
                == dispatch_limits::lifecycle_burst_limit
           && serial_execution_queue_t::fixed_work_byte_cost
                == limits.at ("fixedWorkByteCost")
           && serial_execution_queue_t::fixed_work_byte_cost
                == dispatch_limits::fixed_work_byte_cost
           && receive_options.max_messages
                == dispatch_limits::receive_batch_messages
           && receive_options.max_bytes
                == dispatch_limits::receive_batch_bytes
           && receive_options.max_elapsed
                == dispatch_limits::receive_batch_time;
}

bool verify_fixture_accounting_boundaries ()
{
    using namespace zlink::framework::runtime;
    try {
        const auto &fixture = serial_execution_fixture ();
        for (const auto &scenario : fixture.at ("accountingScenarios")) {
            const auto lane_name = scenario.at ("lane").get<std::string> ();
            const auto lane = lane_name == "application"
              ? serial_work_lane_t::application
              : lane_name == "lifecycle"
                  ? serial_work_lane_t::lifecycle
                  : throw std::runtime_error ("unknown serial work lane");
            const auto payload_bytes =
              scenario.at ("retainedPayloadBytesPerWork")
                .get<std::size_t> ();
            const auto accepted =
              scenario.at ("acceptedWorkCount").get<std::size_t> ();
            const auto byte_cost =
              serial_execution_queue_t::fixed_work_byte_cost + payload_bytes;
            if (accepted == 0
                || scenario.at ("nextAdmission") != "capacityExceeded"
                || !scenario.at ("runningWorkConsumesReservation")
                       .get<bool> ()) {
                return false;
            }

            offload_executor_t executor (2);
            serial_execution_queue_t queue (executor,
                                             serial_execution_queue_options_t{});
            std::mutex gate;
            std::condition_variable changed;
            bool active = false;
            std::optional<serial_execution_queue_t::async_completion_t>
              finish_active;
            const auto options = serial_work_options_t{lane, byte_cost};
            if (!queue.try_post_async (
                  scenario.at ("name").get<std::string> (),
                  [&] (auto complete) {
                      std::lock_guard lock (gate);
                      finish_active.emplace (std::move (complete));
                      active = true;
                      changed.notify_all ();
                  },
                  options)) {
                return false;
            }
            {
                std::unique_lock lock (gate);
                if (!changed.wait_for (
                      lock, std::chrono::seconds (1), [&] { return active; })) {
                    return false;
                }
            }
            for (std::size_t index = 1; index < accepted; ++index) {
                if (!queue.try_post ("fixture-boundary", [] {}, options))
                    return false;
            }
            if (queue.try_post ("fixture-over-boundary", [] {}, options)
                || queue.pending_count (lane) != accepted
                || queue.pending_bytes () != accepted * byte_cost) {
                return false;
            }

            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*finish_active);
            }
            finish ([] {});
            queue.drain ();
            if (queue.pending_count () != 0 || queue.pending_bytes () != 0
                || !queue.try_post ("fixture-after-terminal", [] {}, options)) {
                return false;
            }
            queue.drain ();
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool verify_fixture_arbitration_and_owner_isolation ()
{
    using namespace zlink::framework::runtime;
    try {
        const auto &fixture = serial_execution_fixture ();
        const auto &invariants = fixture.at ("dispatchInvariants");
        if (!invariants.at ("applicationAndLifecycleUseDistinctFifos")
               .get<bool> ()
            || !invariants.at ("applicationAndLifecycleHaveIndependentAdmission")
                  .get<bool> ()
            || !invariants.at ("emptyToNonEmptySchedulesImmediately")
                  .get<bool> ()
            || !invariants.at ("pollingIsNotAProgressMechanism").get<bool> ()
            || invariants.at ("implicitInlineExecution").get<bool> ()
            || !invariants.at ("resumeAfterYieldUsesNewTurn").get<bool> ()) {
            return false;
        }
        const auto same_owner = [&] (std::string_view target)
          -> const nlohmann::json & {
            const auto &rules = fixture.at ("sameOwnerCalls");
            const auto found = std::find_if (
              rules.begin (), rules.end (), [&] (const auto &rule) {
                  return rule.at ("target") == target;
              });
            if (found == rules.end ())
                throw std::runtime_error ("same-owner fixture rule is missing");
            return *found;
        };
        const auto &self_actor = same_owner ("selfActor");
        const auto &same_spot = same_owner ("sameSpot");
        const auto &member_actor =
          same_owner ("differentMemberActorOnSameSpot");
        const auto &different_owner = same_owner ("differentOwner");
        if (self_actor.at ("async") != "invalidOperation"
            || self_actor.at ("yield") != "invalidOperation"
            || self_actor.at ("actorClaimAfterYield") != "retained"
            || same_spot.at ("async") != "invalidOperation"
            || same_spot.at ("yield") != "resumeOnNewTurn"
            || member_actor.at ("async") != "invalidOperation"
            || member_actor.at ("yield") != "resumeOnNewTurn"
            || member_actor.at ("actorClaimAfterYield") != "retained"
            || different_owner.at ("async") != "awaitWithoutGateRelease"
            || different_owner.at ("yield") != "resumeOnNewTurn") {
            return false;
        }

        const auto &scenario = fixture.at ("arbitrationScenarios").at (0);
        const auto applications =
          scenario.at ("applicationInput").get<std::vector<std::string>> ();
        const auto lifecycle =
          scenario.at ("lifecycleInput").get<std::vector<std::string>> ();
        const auto expected =
          scenario.at ("expectedSelection").get<std::vector<std::string>> ();
        if (lifecycle.empty ())
            return false;

        offload_executor_t arbitration_executor (2);
        serial_execution_queue_t arbitration_queue (
          arbitration_executor, serial_execution_queue_options_t{});
        std::mutex order_gate;
        std::condition_variable order_changed;
        std::vector<std::string> order;
        std::optional<serial_execution_queue_t::async_completion_t>
          release_first;
        if (!arbitration_queue.try_post_async (
              lifecycle.front (),
              [&] (auto complete) {
                  std::lock_guard lock (order_gate);
                  order.push_back (lifecycle.front ());
                  release_first.emplace (std::move (complete));
                  order_changed.notify_all ();
              },
              {serial_work_lane_t::lifecycle,
               serial_execution_queue_t::fixed_work_byte_cost})) {
            return false;
        }
        {
            std::unique_lock lock (order_gate);
            if (!order_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return release_first.has_value (); })) {
                return false;
            }
        }
        for (const auto &name : applications) {
            if (!arbitration_queue.try_post (
                  name,
                  [&, name] {
                      std::lock_guard lock (order_gate);
                      order.push_back (name);
                  },
                  {serial_work_lane_t::application,
                   serial_execution_queue_t::fixed_work_byte_cost})) {
                return false;
            }
        }
        for (auto item = std::next (lifecycle.begin ());
             item != lifecycle.end (); ++item) {
            if (!arbitration_queue.try_post (
                  *item,
                  [&, name = *item] {
                      std::lock_guard lock (order_gate);
                      order.push_back (name);
                  },
                  {serial_work_lane_t::lifecycle,
                   serial_execution_queue_t::fixed_work_byte_cost})) {
                return false;
            }
        }
        serial_execution_queue_t::async_completion_t finish_first;
        {
            std::lock_guard lock (order_gate);
            finish_first = std::move (*release_first);
        }
        finish_first ([] {});
        arbitration_queue.drain ();
        {
            std::lock_guard lock (order_gate);
            if (order != expected)
                return false;
        }

        offload_executor_t shared_executor (2);
        serial_execution_queue_t owner_a (
          shared_executor, serial_execution_queue_options_t{});
        serial_execution_queue_t owner_b (
          shared_executor, serial_execution_queue_options_t{});
        std::mutex progress_gate;
        std::condition_variable progress_changed;
        std::optional<serial_execution_queue_t::async_completion_t>
          release_owner_a;
        bool owner_a_lifecycle_ran = false;
        std::size_t owner_b_runs = 0;
        bool inline_execution = false;
        const auto caller_thread = std::this_thread::get_id ();
        if (!owner_a.try_post_async (
              "owner-a-active",
              [&] (auto complete) {
                  std::lock_guard lock (progress_gate);
                  release_owner_a.emplace (std::move (complete));
                  progress_changed.notify_all ();
              })) {
            return false;
        }
        {
            std::unique_lock lock (progress_gate);
            if (!progress_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return release_owner_a.has_value (); })) {
                return false;
            }
        }
        const auto application_capacity =
          fixture.at ("limits").at ("application")
            .at ("messageCapacity").get<std::size_t> ();
        for (std::size_t index = 1; index < application_capacity; ++index) {
            if (!owner_a.try_post ("owner-a-saturated", [] {}))
                return false;
        }
        if (owner_a.try_post ("owner-a-over-capacity", [] {})
            || !owner_a.try_post (
              "owner-a-lifecycle",
              [&] {
                  std::lock_guard lock (progress_gate);
                  owner_a_lifecycle_ran = true;
              },
              {serial_work_lane_t::lifecycle,
               serial_execution_queue_t::fixed_work_byte_cost})) {
            return false;
        }
        const auto record_owner_b_progress = [&] {
            std::lock_guard lock (progress_gate);
            inline_execution = inline_execution
                               || std::this_thread::get_id () == caller_thread;
            ++owner_b_runs;
            progress_changed.notify_all ();
        };
        if (!owner_b.try_post (
              "owner-b-lifecycle", record_owner_b_progress,
              {serial_work_lane_t::lifecycle,
               serial_execution_queue_t::fixed_work_byte_cost})
            || !owner_b.try_post (
              "owner-b-application", record_owner_b_progress)) {
            return false;
        }
        {
            std::unique_lock lock (progress_gate);
            if (!progress_changed.wait_for (
                  lock, std::chrono::seconds (1),
                  [&] { return owner_b_runs == 2; })
                || owner_a_lifecycle_ran || inline_execution) {
                return false;
            }
        }
        serial_execution_queue_t::async_completion_t finish_owner_a;
        {
            std::lock_guard lock (progress_gate);
            finish_owner_a = std::move (*release_owner_a);
        }
        finish_owner_a ([] {});
        owner_a.drain ();
        owner_b.drain ();
        std::lock_guard lock (progress_gate);
        return owner_a_lifecycle_ran;
    }
    catch (...) {
        return false;
    }
}

bool verify_serial_lane_policies ()
{
    using namespace zlink::framework::runtime;
    const auto entry = serial_lane_policy_t::entry_spot ();
    const auto spot_wide = serial_lane_policy_t::spot_wide ();
    const auto per_actor = serial_lane_policy_t::per_actor_spot ();
    const auto session = serial_lane_policy_t::session ();
    const auto actor_delivery = serial_lane_policy_t::actor_delivery ();

    const auto *entry_spot = std::get_if<spot_lane_policy_t> (&entry.value ());
    const auto *wide_spot = std::get_if<spot_lane_policy_t> (&spot_wide.value ());
    const auto *actor_spot = std::get_if<spot_lane_policy_t> (&per_actor.value ());
    return entry_spot
           && entry_spot->execution == spot_lane_execution_t::entry
           && entry_spot->lifecycle == spot_lane_lifecycle_t::active
           && wide_spot
           && wide_spot->execution == spot_lane_execution_t::spot_wide
           && wide_spot->lifecycle == spot_lane_lifecycle_t::active
           && actor_spot
           && actor_spot->execution == spot_lane_execution_t::per_actor
           && actor_spot->lifecycle == spot_lane_lifecycle_t::active
           && std::holds_alternative<session_lane_policy_t> (session.value ())
           && std::holds_alternative<actor_delivery_lane_policy_t> (
             actor_delivery.value ())
           && !entry.allows_turn_yield ()
           && spot_wide.allows_turn_yield ()
           && !per_actor.allows_turn_yield ()
           && !session.allows_turn_yield ()
           && !actor_delivery.allows_turn_yield ()
           && std::is_constructible_v<spot_lane_policy_t,
                                      spot_lane_execution_t,
                                      spot_lane_lifecycle_t>
           && std::is_constructible_v<session_lane_policy_t,
                                      session_lane_lifecycle_t>
           && !std::is_constructible_v<session_lane_policy_t,
                                       spot_lane_lifecycle_t>
           && !std::is_constructible_v<actor_delivery_lane_policy_t,
                                       spot_lane_lifecycle_t>
           && !std::is_constructible_v<actor_delivery_lane_policy_t,
                                       session_lane_lifecycle_t>;
}

bool verify_runtime_observation_loss_and_terminal_retention ()
{
    struct probe_status_t
    {
        int sequence = 0;
        std::string source;
        std::string value;
    };
    using observer_t = zlink::framework::observation_detail::
      runtime_observer_state_t<probe_status_t>;

    std::mutex gate;
    std::condition_variable changed;
    bool first_entered = false;
    bool release_first = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> received;
    auto observer = std::make_shared<observer_t> (
      1,
      [&] (const auto &observed) {
          std::unique_lock lock (gate);
          received.push_back (observed);
          if (observed.status.sequence == 1) {
              first_entered = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return release_first; });
          }
          changed.notify_all ();
      });
    observer->start ();
    observer->enqueue ("A", probe_status_t{1});
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                              [&] { return first_entered; })) {
            release_first = true;
            changed.notify_all ();
            lock.unlock ();
            observer->close ();
            return false;
        }
    }
    observer->enqueue ("A", probe_status_t{2});
    observer->enqueue ("A", probe_status_t{3});
    observer->enqueue ("A", probe_status_t{4}, true);
    {
        std::lock_guard lock (gate);
        release_first = true;
        changed.notify_all ();
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] {
                return received.size () == 2;
            })) {
            return false;
        }
        if (received[0].status.sequence != 1
            || received[1].status.sequence != 4
            || received[1].loss.coalesced_count != 2
            || received[1].loss.discarded_terminal_count != 0) {
            return false;
        }
    }
    observer->close ();

    std::mutex terminal_gate;
    std::condition_variable terminal_changed;
    bool terminal_first_entered = false;
    bool terminal_release_first = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> terminals;
    auto terminal_observer = std::make_shared<observer_t> (
      1,
      [&] (const auto &observed) {
          std::unique_lock lock (terminal_gate);
          terminals.push_back (observed);
          if (observed.status.sequence == 10) {
              terminal_first_entered = true;
              terminal_changed.notify_all ();
              terminal_changed.wait (
                lock, [&] { return terminal_release_first; });
          }
          terminal_changed.notify_all ();
      });
    terminal_observer->start ();
    terminal_observer->enqueue ("A", probe_status_t{10});
    {
        std::unique_lock lock (terminal_gate);
        if (!terminal_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return terminal_first_entered; })) {
            terminal_release_first = true;
            terminal_changed.notify_all ();
            lock.unlock ();
            terminal_observer->close ();
            return false;
        }
    }
    terminal_observer->enqueue ("A", probe_status_t{11}, true);
    terminal_observer->enqueue ("A", probe_status_t{12}, true);
    {
        std::lock_guard lock (terminal_gate);
        terminal_release_first = true;
        terminal_changed.notify_all ();
    }
    {
        std::unique_lock lock (terminal_gate);
        if (!terminal_changed.wait_for (lock, std::chrono::seconds (1), [&] {
                return terminals.size () == 2;
            })) {
            return false;
        }
        if (terminals[1].status.sequence != 12
            || terminals[1].loss.discarded_terminal_count != 1) {
            return false;
        }
    }
    terminal_observer->close ();

    const auto &fixture = runtime_observation_fixture ();
    if (fixture.at ("fixture") != "zlink.framework.runtime-observation"
        || fixture.at ("version") != 1)
        return false;
    const auto scenario = std::find_if (
      fixture.at ("scenarios").begin (),
      fixture.at ("scenarios").end (),
      [] (const auto &candidate) {
          return candidate.at ("name")
                 == "multi-source-retention-and-terminal-overflow";
      });
    if (scenario == fixture.at ("scenarios").end ())
        return false;

    std::mutex fixture_gate;
    std::condition_variable fixture_changed;
    bool fixture_blocked = false;
    bool release_fixture = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>>
      fixture_received;
    auto fixture_observer = std::make_shared<observer_t> (
      scenario->at ("terminalCapacity").get<std::size_t> (),
      [&] (const auto &observed) {
          std::unique_lock lock (fixture_gate);
          fixture_received.push_back (observed);
          if (observed.status.sequence == -1) {
              fixture_blocked = true;
              fixture_changed.notify_all ();
              fixture_changed.wait (lock, [&] { return release_fixture; });
          }
          fixture_changed.notify_all ();
      });
    fixture_observer->start ();
    fixture_observer->enqueue (
      "blocker", probe_status_t{-1, "blocker", "blocker"});
    {
        std::unique_lock lock (fixture_gate);
        if (!fixture_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return fixture_blocked; })) {
            release_fixture = true;
            fixture_changed.notify_all ();
            lock.unlock ();
            fixture_observer->close ();
            return false;
        }
    }
    for (const auto &operation : scenario->at ("operations")) {
        const auto source = operation.at ("source").get<std::string> ();
        fixture_observer->enqueue (
          source,
          probe_status_t{
            operation.at ("sequence").get<int> (),
            source,
            operation.at ("value").get<std::string> ()},
          operation.at ("kind") == "terminal");
    }
    {
        std::lock_guard lock (fixture_gate);
        release_fixture = true;
        fixture_changed.notify_all ();
    }

    const auto expected_count =
      1 + scenario->at ("expectedTerminalFifo").size ()
      + scenario->at ("expectedRetainedIntermediateBySource").size ();
    {
        std::unique_lock lock (fixture_gate);
        if (!fixture_changed.wait_for (
              lock, std::chrono::seconds (2), [&] {
                  return fixture_received.size () == expected_count;
              }))
            return false;

        std::size_t received_index = 1;
        for (const auto &expected : scenario->at ("expectedTerminalFifo")) {
            const auto &actual = fixture_received.at (received_index++).status;
            if (actual.source
                  != expected.at ("source").get<std::string> ()
                || actual.sequence != expected.at ("sequence").get<int> ()
                || actual.value
                     != expected.at ("value").get<std::string> ())
                return false;
        }
        for (const auto &[source, expected] :
             scenario->at ("expectedRetainedIntermediateBySource").items ()) {
            const auto &actual = fixture_received.at (received_index++).status;
            if (actual.source != source
                || actual.sequence != expected.at ("sequence").get<int> ()
                || actual.value
                     != expected.at ("value").get<std::string> ())
                return false;
        }
        const auto &loss = fixture_received.back ().loss;
        if (loss.coalesced_count
              != std::stoull (scenario->at ("expectedLoss")
                                .at ("coalescedIntermediateCount")
                                .get<std::string> ())
            || loss.discarded_terminal_count
                 != std::stoull (scenario->at ("expectedLoss")
                                   .at ("discardedTerminalCount")
                                   .get<std::string> ()))
            return false;
    }
    fixture_observer->close ();

    const auto saturation_scenario = std::find_if (
      fixture.at ("scenarios").begin (),
      fixture.at ("scenarios").end (),
      [] (const auto &candidate) {
          return candidate.at ("name")
                 == "loss-counters-saturate-independently";
      });
    if (saturation_scenario == fixture.at ("scenarios").end ())
        return false;
    auto coalesced_loss = static_cast<std::uint64_t> (std::stoull (
      saturation_scenario->at ("initialLoss")
        .at ("coalescedIntermediateCount")
        .get<std::string> ()));
    auto discarded_loss = static_cast<std::uint64_t> (std::stoull (
      saturation_scenario->at ("initialLoss")
        .at ("discardedTerminalCount")
        .get<std::string> ()));
    for (int index = 0;
         index
         < saturation_scenario->at ("increments")
             .at ("coalescedIntermediateCount")
             .get<int> ();
         ++index) {
        zlink::framework::observation_detail::
          increment_runtime_observation_loss (coalesced_loss);
    }
    for (int index = 0;
         index
         < saturation_scenario->at ("increments")
             .at ("discardedTerminalCount")
             .get<int> ();
         ++index) {
        zlink::framework::observation_detail::
          increment_runtime_observation_loss (discarded_loss);
    }
    if (coalesced_loss
          != std::stoull (
            saturation_scenario->at ("expectedLoss")
              .at ("coalescedIntermediateCount")
              .get<std::string> ())
        || discarded_loss
             != std::stoull (
               saturation_scenario->at ("expectedLoss")
                 .at ("discardedTerminalCount")
                 .get<std::string> ()))
        return false;

    std::mutex lifetime_gate;
    std::condition_variable lifetime_changed;
    bool lifetime_blocked = false;
    bool release_lifetime = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>>
      lifetime_received;
    auto lifetime_observer = std::make_shared<observer_t> (
      1,
      [&] (const auto &observed) {
          std::unique_lock lock (lifetime_gate);
          lifetime_received.push_back (observed);
          if (observed.status.sequence == -1) {
              lifetime_blocked = true;
              lifetime_changed.notify_all ();
              lifetime_changed.wait (
                lock, [&] { return release_lifetime; });
          }
          lifetime_changed.notify_all ();
      });
    lifetime_observer->start ();
    lifetime_observer->enqueue (
      "blocker", probe_status_t{-1, "blocker", "blocker"});
    {
        std::unique_lock lock (lifetime_gate);
        if (!lifetime_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return lifetime_blocked; })) {
            release_lifetime = true;
            lifetime_changed.notify_all ();
            lock.unlock ();
            lifetime_observer->close ();
            return false;
        }
    }
    lifetime_observer->enqueue (
      "A", probe_status_t{1, "A", "terminal-A"}, true);
    lifetime_observer->enqueue (
      "A", probe_status_t{2, "A", "suppressed-A"});
    lifetime_observer->enqueue (
      "B", probe_status_t{3, "B", "terminal-B"}, true);
    lifetime_observer->enqueue (
      "A", probe_status_t{4, "A", "restarted-A"});
    {
        std::lock_guard lock (lifetime_gate);
        release_lifetime = true;
        lifetime_changed.notify_all ();
    }
    {
        std::unique_lock lock (lifetime_gate);
        if (!lifetime_changed.wait_for (
              lock, std::chrono::seconds (2), [&] {
                  return lifetime_received.size () == 3;
              }))
            return false;
        if (lifetime_received[1].status.value != "terminal-B"
            || lifetime_received[2].status.value != "restarted-A"
            || lifetime_received[2].loss.coalesced_count != 1
            || lifetime_received[2].loss.discarded_terminal_count != 1)
            return false;
    }
    lifetime_observer->close ();

    std::mutex shared_gate;
    std::condition_variable shared_changed;
    bool slow_entered = false;
    bool release_slow = false;
    bool fast_delivered = false;
    auto slow_observer = std::make_shared<observer_t> (
      1,
      [&] (const auto &) {
          std::unique_lock lock (shared_gate);
          slow_entered = true;
          shared_changed.notify_all ();
          shared_changed.wait (lock, [&] { return release_slow; });
      });
    auto fast_observer = std::make_shared<observer_t> (
      1,
      [&] (const auto &) {
          std::lock_guard lock (shared_gate);
          fast_delivered = true;
          shared_changed.notify_all ();
      });
    slow_observer->start ();
    fast_observer->start ();
    slow_observer->enqueue ("slow", probe_status_t{1});
    {
        std::unique_lock lock (shared_gate);
        if (!shared_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return slow_entered; })) {
            release_slow = true;
            shared_changed.notify_all ();
            lock.unlock ();
            slow_observer->close ();
            fast_observer->close ();
            return false;
        }
    }
    fast_observer->enqueue ("fast", probe_status_t{1});
    {
        std::unique_lock lock (shared_gate);
        if (!shared_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return fast_delivered; })) {
            release_slow = true;
            shared_changed.notify_all ();
            lock.unlock ();
            slow_observer->close ();
            fast_observer->close ();
            return false;
        }
        release_slow = true;
        shared_changed.notify_all ();
    }
    slow_observer->close ();
    fast_observer->close ();
    return true;
}

bool verify_idle_instance_spot_eviction_closes_local_context ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = std::make_shared<spot_node_builder_state_t> ("idle-node");
    node->instance_spot_idle_timeout = std::chrono::seconds (1);

    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->spot_id = "instance-1";
    context->spot_name = "instance-player";
    context->lifecycle_domain =
      detail::spot_lifecycle_domain_t::instance ();
    context->object_generation = 7;
    context->authority_owner_generation = 11;
    context->spot_instance = std::make_shared<int> (1);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    const auto set_last_application_work = [&context] (auto age) {
        context->last_application_work_completed_ns.store (
          std::chrono::duration_cast<std::chrono::nanoseconds> (
            std::chrono::steady_clock::now ().time_since_epoch ())
            .count ()
            - std::chrono::duration_cast<std::chrono::nanoseconds> (age).count (),
          std::memory_order_relaxed);
    };
    context->last_application_work_completed_ns.store (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
        .count ()
        - std::chrono::duration_cast<std::chrono::nanoseconds> (
          std::chrono::milliseconds (100))
            .count (),
      std::memory_order_relaxed);

    bool closing_called = false;
    bool location_visible_while_closing = false;
    spot_close_reason_t closing_reason = spot_close_reason_t::explicit_close;
    context->lifecycle.on_closing = [&] (
      void *, const spot_closing_context_t &closing, std::stop_token) {
        closing_called = true;
        location_visible_while_closing =
          node->spot_contexts_by_id.contains (context->spot_id)
          && node->spot_names_by_id.contains (context->spot_id)
          && context->node == node;
        closing_reason = closing.reason;
    };

    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (
      context->spot_id, spot_context_access_t::create (context));

    bool admission_called = false;
    bool late_application_post_rejected = false;
    node->admit_instance_spot_idle_eviction = [&] (
      const spot_id_t &spot_id,
      std::string_view spot_name,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation,
      std::function<bool ()> close_local) {
        admission_called = true;
        if (spot_id != "instance-1" || spot_name != "instance-player"
            || object_generation != 7 || authority_owner_generation != 11)
            return false;
        late_application_post_rejected = !context->try_post_serial (
          "late-idle-eviction-application", [] {});
        return close_local ();
    };

    spot_node_runtime_t runtime (node);
    set_last_application_work (std::chrono::milliseconds (100));
    runtime.evict_idle_spots ();
    if (admission_called || context->idle_eviction_in_progress) {
        return false;
    }
    set_last_application_work (std::chrono::seconds (2));
    runtime.evict_idle_spots ();
    executor->drain ();

    const bool result = admission_called && late_application_post_rejected && closing_called
                        && location_visible_while_closing
                        && closing_reason == spot_close_reason_t::idle_evicted
                        && context->closed && !context->node
                        && !context->spot_instance
                        && node->spot_contexts_by_id.empty ()
                        && node->spot_ids_by_name.empty ()
                        && node->spot_names_by_id.empty ();
    return result;
}

bool verify_explicit_instance_spot_close_releases_authority_after_callback ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace service = zlink::framework::runtime::host;

    auto node = std::make_shared<spot_node_builder_state_t> (
      "instance-explicit-close-node");
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->spot_id = "instance-explicit-close";
    context->spot_name = "instance-player";
    context->lifecycle_domain = spot_lifecycle_domain_t::instance ();
    context->object_generation = 13;
    context->authority_owner_generation = 17;
    context->spot_instance = std::make_shared<int> (1);

    std::vector<std::string> order;
    int begin_calls = 0;
    int completion_calls = 0;
    context->lifecycle.on_closing = [&] (
      void *, const spot_closing_context_t &closing, std::stop_token) {
        if (closing.reason != spot_close_reason_t::explicit_close
            || !node->spot_contexts_by_id.contains (context->spot_id)) {
            order.push_back ("invalid-local-cleanup");
            return;
        }
        order.push_back ("local-cleanup");
    };
    node->begin_instance_spot_close = [&] (
      const spot_id_t &spot_id,
      std::string_view stable_type,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation)
      -> std::optional<service::instance_spot_close_completion_t> {
        ++begin_calls;
        if (spot_id != context->spot_id
            || stable_type != context->spot_name
            || object_generation != context->object_generation
            || authority_owner_generation
                 != context->authority_owner_generation) {
            return std::nullopt;
        }
        order.push_back ("authority-closing");
        return service::instance_spot_close_completion_t{
          [&] (bool local_closed) {
              ++completion_calls;
              order.push_back (
                local_closed ? "authority-released" : "authority-restored");
              return local_closed;
          }};
    };

    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (
      context->spot_id, spot_context_access_t::create (context));

    if (!context->enter_callback ()) {
        return false;
    }
    auto public_context = spot_context_access_t::create (context);
    const auto close_result = public_context.close ().result ();
    if (!close_result || !close_result.value ()) {
        context->leave_callback ();
        return false;
    }

    const bool deferred = begin_calls == 1 && completion_calls == 0
                          && context->close_requested
                          && context->callback_admission_closed
                          && !context->closed && context->spot_instance
                          && context->node == node
                          && !context->enter_callback ();
    context->leave_callback ();
    context->leave_callback ();

    return deferred && completion_calls == 1 && context->closed
           && !context->node && !context->spot_instance
           && node->spot_contexts_by_id.empty ()
           && node->spot_ids_by_name.empty ()
           && node->spot_names_by_id.empty ()
           && order
                == std::vector<std::string>{
                  "authority-closing", "local-cleanup",
                  "authority-released"};
}

bool verify_remote_actor_prepare_is_idempotent ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> (
      "actor-prepare-idempotency-node");
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string (
      "actor-prepare-idempotency-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime =
      std::make_shared<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor =
      std::make_shared<runtime::offload_executor_t> (
        2, 64, "actor-prepare-idempotency");
    target->serial_queue =
      std::make_shared<runtime::serial_execution_queue_t> (
        *target->serial_executor, 64,
        runtime::serial_execution_queue_t::error_handler_t{},
        runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (
      target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    int admission_calls = 0;
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [&] (void *, std::string_view,
                          const zlink::message_t &,
                          serializer_registry_t &) {
        ++admission_calls;
        return spot_actor_join_result_t::accept (
          message_t::from (std::string ("accepted")));
    };
    target->actor_admissions.emplace (
      std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    const auto relocation_store =
      std::make_shared<runtime::in_memory_relocation_store_t> ();
    const auto relocation_repository =
      std::make_shared<runtime::provider_relocation_repository_t> (
        *relocation_store);
    owner.bind_relocation_store (
      std::make_shared<runtime::stateful::public_relocation_store_adapter_t> (
        relocation_repository));
    const auto actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"),
      "player", "actor-1", 7);
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    store->snapshot = authority_snapshot_t{
      .store_version = "actor-prepare-v1",
      .payload = runtime::encode_actor_authority_payload (actor, "source-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 19,
      .owner = location_owner_token_t{"source-owner", 29},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "actor-prepare",
                                .node_rid = node_rid_t::from_string ("source-node"),
                                .node_lifecycle_generation = 23,
                                .owner = location_owner_token_t{"source-owner", 29}}}};
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    const auto request = zlink::message_t::from (std::string ("prepare"));
    const auto first = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 13, 19, 23, 29);
    const auto repeated = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 13, 19, 23, 29);
    const auto conflicting = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 17, 19, 23, 29);

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    return first && repeated && first.value ().accepted
           && repeated.value ().accepted
           && first.value ().reply && repeated.value ().reply
           && first.value ().reply->decode<std::string> () == "accepted"
           && repeated.value ().reply->decode<std::string> () == "accepted"
           && !conflicting
           && conflicting.error_kind () == framework_error_kind_t::protocol_error
           && admission_calls == 1;
}

class wire_join_spot_resolver_t final
    : public zlink::framework::runtime::spot_address_resolver_t
{
  public:
    zlink::framework::task_t<std::optional<zlink::framework::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        co_return spot_id == address.spot_id ? std::make_optional (address) : std::nullopt;
    }

    void invalidate_spot_address (std::string_view) override {}

    void invalidate_all_routes_after_store_recovery () override {}

    zlink::framework::runtime::spot_address_t address;
};

// actorJoin(28) receiver admission is APPROVAL-ONLY (spec 15 §478-527):
// admission registers the relocation temporary queue (identity-keyed
// pending admission) with the prepared factory and replies approval — it
// must NOT construct/install the Actor, claim the target location, or
// advance membership; those belong to the later transfer/commit stages.
// A newer wire attempt for the same actor identity evicts a parked older
// attempt (later-attempt-wins, spec 15 §542-546), while a duplicate resend
// of the SAME attempt parks against the existing preparation without
// re-running the application admission callback.
bool verify_wire_actor_join_admission_is_approval_only_and_later_attempt_wins ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> ("wire-join-node");
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("wire-join-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, 64, "wire-join-admission");
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, 64, runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    int admission_calls = 0;
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [&] (void *, std::string_view, const zlink::message_t &,
                          serializer_registry_t &) {
        ++admission_calls;
        return spot_actor_join_result_t::accept (message_t::from (std::string ("approved")));
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    const auto local_rid = zlink::routing_id_t::from (std::string ("wire-join-node"));
    const auto source_rid = zlink::routing_id_t::from (std::string ("wire-join-source"));
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    const auto stored_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("wire-join-source"), "player", "actor-1", 7);
    store->snapshot = authority_snapshot_t{
      .store_version = "wire-join-v1",
      .payload = runtime::encode_actor_authority_payload (stored_actor, "source-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 19,
      .owner = location_owner_token_t{"source-owner", 5},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "wire-join",
                                .node_rid = node_rid_t::from_string ("wire-join-source"),
                                .node_lifecycle_generation = 3,
                                .owner = location_owner_token_t{"source-owner", 5}}}};
    store->spot_snapshot = authority_snapshot_t{
      .store_version = "wire-join-target-v1",
      .object_generation = 9,
      .authority_owner_generation = 21,
      .owner = location_owner_token_t{"target-owner", 22},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::user_spot,
                     .stable_type = "target",
                     .target = {.mesh_name = "wire-join",
                                .node_rid = node_rid_t::from_string ("wire-join-node"),
                                .node_lifecycle_generation = 1,
                                .owner = location_owner_token_t{"target-owner", 22}}}};
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    wire_join_spot_resolver_t resolver;
    resolver.address.node_rid = local_rid;
    resolver.address.spot_id = "target-spot";
    // Deliberately stale: canonical target admission must ignore this
    // cacheable routing projection and read store->spot_snapshot instead.
    resolver.address.spot_generation = 8;
    resolver.address.node_generation = 2;
    resolver.address.authority_owner_generation = 20;
    resolver.address.owner = location_owner_token_t{"stale-owner", 23};
    owner.bind_spot_location_resolver (resolver);

    const auto make_request = [&] (std::uint64_t correlation) {
        return runtime::protocol::actor_join_request_t{
          correlation,
          runtime::protocol::actor_route_fence_t{"actor-1", 7, source_rid.to_bytes (), 3, 19,
                                                 5},
          false,
          runtime::protocol::spot_route_fence_t{"target-spot", 9, local_rid.to_bytes (), 1, 21,
                                                22}};
    };
    const auto transfer_id_for = [&] (std::uint64_t correlation) {
        std::string transfer_id = "wire-actor-join:";
        static constexpr char hex_digits[] = "0123456789abcdef";
        for (const auto byte : source_rid.to_bytes ()) {
            transfer_id += hex_digits[(byte >> 4) & 0x0f];
            transfer_id += hex_digits[byte & 0x0f];
        }
        transfer_id += ":3:";
        transfer_id += std::to_string (correlation);
        return transfer_id;
    };

    const auto first = admit_wire_actor_join (node, local_rid, make_request (4211), std::nullopt);
    const bool first_approved =
      first.join_result == runtime::protocol::actor_join_result_t::accepted && first.spot
      && first.spot->spot_id == "target-spot" && first.spot->object_generation == 9
      // Approval-only: the accepted reply carries the PROPOSED membership
      // epoch (no membership has moved, so current-none + 1 == 1) and the
      // conservative advertised receive chunk limit.
      && first.membership_epoch == 1 && first.receive_chunk_limit_bytes == 32768;
    // No lifecycle side effects during admission: no Actor instance was
    // constructed or installed, no membership was recorded, and no target
    // location/commit state exists — only the temporary-queue registration
    // (target_pending pending admission) on the coordinator.
    const bool approval_only =
      node->actor_instances.empty () && node->actor_spot_ids.empty ()
      && node->core_actor_membership_epochs.empty ()
      && node->actor_transfer_coordinator.admission (transfer_id_for (4211)).has_value ()
      && node->actor_transfer_coordinator.phase ("player:actor-1")
           == actor_move_phase_t::target_pending;

    // A duplicate resend of the same attempt parks against the existing
    // preparation without re-running the application admission callback.
    const auto repeated =
      admit_wire_actor_join (node, local_rid, make_request (4211), std::nullopt);
    const bool duplicate_parked =
      repeated.join_result == runtime::protocol::actor_join_result_t::accepted
      && admission_calls == 1;

    // Later-attempt-wins: a NEWER attempt (fresh correlation → distinct
    // derived transfer identity) evicts the parked older attempt.
    const auto newer = admit_wire_actor_join (node, local_rid, make_request (4213), std::nullopt);
    const bool later_attempt_wins =
      newer.join_result == runtime::protocol::actor_join_result_t::accepted
      && !node->actor_transfer_coordinator.admission (transfer_id_for (4211)).has_value ()
      && node->actor_transfer_coordinator.admission (transfer_id_for (4213)).has_value ()
      && node->actor_transfer_coordinator.is_current ("player:actor-1", transfer_id_for (4213))
      && node->actor_instances.empty () && node->actor_spot_ids.empty ();

    // An identity this node has never created/known is rejected cleanly.
    auto unknown_request = make_request (4215);
    unknown_request.actor.actor_id = "actor-unknown";
    const auto unknown =
      admit_wire_actor_join (node, local_rid, unknown_request, std::nullopt);
    const bool unknown_rejected =
      unknown.join_result == runtime::protocol::actor_join_result_t::rejected && !unknown.spot;

    const auto malformed_terminal = [&] (std::string actor_id, std::string spot_id) {
        auto malformed = make_request (4217);
        malformed.actor.actor_id = std::move (actor_id);
        malformed.target_spot.spot_id = std::move (spot_id);
        const auto result = admit_wire_actor_join (node, local_rid, malformed, std::nullopt);
        return result.terminal_result == 104
               && result.failure_code == static_cast<std::uint32_t> (
                 runtime::protocol::framework_error_code::requestProtocolError);
    };
    const bool malformed_typed =
      malformed_terminal (" \t", "target-spot")
      && malformed_terminal (std::string ("actor\0bad", 9), "target-spot")
      && malformed_terminal ("actor-1", " \n")
      && malformed_terminal ("actor-1", std::string ("spot\0bad", 8));

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    return first_approved && approval_only && duplicate_parked && later_attempt_wins
           && unknown_rejected && malformed_typed;
}

bool verify_target_commit_stages_source_prefix_before_live_dispatch ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player",
      "actor-cutover-order", 7);
    pending_actor_admission_t admission{
      .actor_key = "player:actor-cutover-order",
      .source_actor = source,
      .source_spot_id = "source-spot",
      .target_spot_id = "target-spot",
      .deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30),
      .completion_operation_id_high = 31,
      .completion_operation_id_low = 37};
    if (!coordinator.try_add_admission ("transfer-cutover-order", admission)
        || !coordinator.begin_commit (
          "transfer-cutover-order", source, "target-spot")) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (!coordinator.stage_commit_backlog (
          "transfer-cutover-order", {packet ("B1"), packet ("B2")})
        || coordinator.try_append_backlog (
             "player:actor-cutover-order", packet ("D1"))
             != handoff_append_result_t::appended) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog (
      "transfer-cutover-order", source, "target-spot");
    return replay && replay->size () == 3
           && (*replay)[0].metadata.at ("sequence") == "B1"
           && (*replay)[1].metadata.at ("sequence") == "B2"
           && (*replay)[2].metadata.at ("sequence") == "D1";
}

// Spec 15 §4.2 step 2-4: an arrival for the joining Actor between
// OnActorJoin Accepted (target_pending) and PREPARE (target_committing)
// must park in the relocation temporary queue instead of being dropped, and
// PREPARE must migrate it into the real queue in order — ahead of anything
// that arrives after PREPARE starts. This drives the production admission
// entry points directly (try_add_admission / try_append_backlog /
// begin_commit / complete_commit_and_take_backlog), the same level this
// suite already uses for
// verify_target_commit_stages_source_prefix_before_live_dispatch.
bool verify_actor_join_prewarm_parks_arrival_before_prepare ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-prewarm", 7);
    pending_actor_admission_t admission{
      .actor_key = "player:actor-prewarm",
      .source_actor = source,
      .source_spot_id = "source-spot",
      .target_spot_id = "target-spot",
      .deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30),
      .completion_operation_id_high = 41,
      .completion_operation_id_low = 43};
    if (!coordinator.try_add_admission ("transfer-prewarm", admission)) {
        return false;
    }
    // Accepted has returned to source; PREPARE has not run yet. An arrival
    // for this exact Actor must park here (spec 15 §4.2 "temporary
    // queue가 등록되어 있어도 ... application handler를 실행하지 않는다") —
    // before the fix, target_pending was not in try_append_backlog's
    // allowed-phase set and this call returned not_moving, letting
    // relay_actor_packet fall through to entry-spot auto-join or a
    // not-found failure instead of parking.
    if (coordinator.phase ("player:actor-prewarm") != actor_move_phase_t::target_pending) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (coordinator.try_append_backlog ("player:actor-prewarm", packet ("EARLY"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-prewarm", source, "target-spot")) {
        return false;
    }
    if (coordinator.try_append_backlog (
          "player:actor-prewarm", packet ("AFTER_PREPARE"))
        != handoff_append_result_t::appended) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog (
      "transfer-prewarm", source, "target-spot");
    return replay && replay->size () == 2
           && (*replay)[0].metadata.at ("sequence") == "EARLY"
           && (*replay)[1].metadata.at ("sequence") == "AFTER_PREPARE";
}

// Spec 15 §4.2 "같은 object의 relocation temporary queue는 하나만 존재한다" /
// "나중 attempt가 유효하며": a newer exact identity (different transfer_id)
// for the same object displaces an admission-time placeholder that has not
// started PREPARE yet, and any arrival parked for the displaced identity
// does not leak into the new attempt's replay.
bool verify_actor_join_prewarm_newest_attempt_evicts_placeholder ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-evict", 7);
    pending_actor_admission_t first{
      .actor_key = "player:actor-evict",
      .source_actor = source,
      .source_spot_id = "source-spot",
      .target_spot_id = "target-spot",
      .deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30),
      .completion_operation_id_high = 51,
      .completion_operation_id_low = 53};
    pending_actor_admission_t second = first;
    second.completion_operation_id_low = 59;

    if (!coordinator.try_add_admission ("transfer-evict-1", first)) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (coordinator.try_append_backlog ("player:actor-evict", packet ("STALE"))
        != handoff_append_result_t::appended) {
        return false;
    }
    // A newer exact identity for the same object arrives before the first
    // reaches PREPARE — it must win, and the stale placeholder (including
    // whatever it parked) must not survive.
    if (!coordinator.try_add_admission ("transfer-evict-2", second)) {
        return false;
    }
    if (coordinator.admission ("transfer-evict-1").has_value ()) {
        return false;
    }
    if (coordinator.phase ("player:actor-evict") != actor_move_phase_t::target_pending) {
        return false;
    }
    if (coordinator.try_append_backlog ("player:actor-evict", packet ("FRESH"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-evict-2", source, "target-spot")) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog (
      "transfer-evict-2", source, "target-spot");
    return replay && replay->size () == 1
           && (*replay)[0].metadata.at ("sequence") == "FRESH";
}

// Rejected/expiry/prepare-failure cleanup must release a parked backlog
// exactly once — not strand it under the actor_key for a later, unrelated
// admission to inherit (spec 15 §4.2 cleanup requirement).
bool verify_actor_join_prewarm_fail_commit_clears_parked_backlog ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-fail", 7);
    pending_actor_admission_t admission{
      .actor_key = "player:actor-fail",
      .source_actor = source,
      .source_spot_id = "source-spot",
      .target_spot_id = "target-spot",
      .deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30),
      .completion_operation_id_high = 61,
      .completion_operation_id_low = 63};
    if (!coordinator.try_add_admission ("transfer-fail", admission)) {
        return false;
    }
    handoff_packet_t stale;
    stale.packet_name = "handoff";
    stale.metadata.emplace ("sequence", "STRANDED");
    if (coordinator.try_append_backlog ("player:actor-fail", stale)
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-fail", source, "target-spot")) {
        return false;
    }
    coordinator.fail_commit ("transfer-fail", false);
    if (coordinator.phase ("player:actor-fail").has_value ()) {
        return false;
    }
    // A later, unrelated admission for the same object must start clean.
    pending_actor_admission_t retry = admission;
    if (!coordinator.try_add_admission ("transfer-fail-retry", retry)) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-fail-retry", source, "target-spot")) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog (
      "transfer-fail-retry", source, "target-spot");
    return replay && replay->empty ();
}

// Spec 15 §4.2 newest-attempt-wins parity: a newer exact identity must
// evict an older attempt even after that older attempt is past PREPARE
// (target_committing) — not only the admission-time placeholder
// (target_pending) verify_actor_join_prewarm_newest_attempt_evicts_placeholder
// already covers. Attempt A parks a frame, reaches target_committing (past
// PREPARE) and parks a second frame there; attempt B then arrives with a
// different transfer_id for the same object and must win: A's admission and
// both of its parked frames are gone (failed exactly once — the same single
// erase fail_commit/cleanup_expired already use to reclaim a dead attempt),
// and B parks and replays its own arrival cleanly.
bool verify_actor_join_prewarm_newest_attempt_evicts_live_attempt_past_prepare ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-evict-live", 7);
    const std::string key = "player:actor-evict-live";
    pending_actor_admission_t first{
      .actor_key = key,
      .source_actor = source,
      .source_spot_id = "source-spot",
      .target_spot_id = "target-spot",
      .deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30),
      .completion_operation_id_high = 71,
      .completion_operation_id_low = 73};
    pending_actor_admission_t second = first;
    second.completion_operation_id_low = 79;

    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };

    if (!coordinator.try_add_admission ("transfer-evict-live-A", first)) {
        return false;
    }
    if (coordinator.try_append_backlog (key, packet ("A_PARKED_EARLY"))
        != handoff_append_result_t::appended) {
        return false;
    }
    // Attempt A reaches PREPARE (target_committing) — the "live attempt"
    // case, distinct from the target_pending placeholder the other eviction
    // test covers.
    if (!coordinator.begin_commit ("transfer-evict-live-A", source, "target-spot")
        || coordinator.phase (key) != actor_move_phase_t::target_committing
        || !coordinator.is_current (key, "transfer-evict-live-A")) {
        return false;
    }
    if (coordinator.try_append_backlog (key, packet ("A_PARKED_AFTER_PREPARE"))
        != handoff_append_result_t::appended) {
        return false;
    }

    // A newer exact identity for the same object arrives — it must win even
    // though A is already past PREPARE.
    if (!coordinator.try_add_admission ("transfer-evict-live-B", second)) {
        return false;
    }
    // A is gone: its admission record and both frames it parked (failed
    // exactly once, by the same single erase fail_commit/cleanup_expired
    // use) do not survive the eviction.
    if (coordinator.admission ("transfer-evict-live-A").has_value ()
        || coordinator.is_current (key, "transfer-evict-live-A")
        || coordinator.phase (key) != actor_move_phase_t::target_pending) {
        return false;
    }
    // B starts clean and parks its own arrival.
    if (coordinator.try_append_backlog (key, packet ("B_PARKED"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-evict-live-B", source, "target-spot")) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog (
      "transfer-evict-live-B", source, "target-spot");
    return replay && replay->size () == 1
           && (*replay)[0].metadata.at ("sequence") == "B_PARKED";
}

class actor_cutover_authority_t final
    : public zlink::framework::runtime::stateful::authority_relocation_port_t
{
  public:
    using authority_publish_result_t =
      zlink::framework::runtime::stateful::authority_publish_result_t;
    using authority_relocation_reference_t =
      zlink::framework::runtime::stateful::authority_relocation_reference_t;
    using inventory_digest_t =
      zlink::framework::runtime::stateful::inventory_digest_t;
    using object_kind_t =
      zlink::framework::runtime::stateful::object_kind_t;
    using object_ref_t =
      zlink::framework::runtime::stateful::object_ref_t;
    using authority_publish_status_t =
      zlink::framework::runtime::stateful::authority_publish_status_t;

    authority_publish_result_t publish (
      const object_ref_t &source,
      const object_ref_t &target,
      zlink::framework::location_owner_token_t target_owner,
      zlink::framework::object_creation_target_t,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest,
      std::vector<std::byte> target_application_payload = {}) override
    {
        if (on_publish)
            on_publish ();
        authority_relocation_reference_t reference{
          .source = source,
          .target = target,
          .relocation_reference = std::move (relocation_reference),
          .checksum_crc32c = checksum_crc32c,
          .inventory_digest = inventory_digest,
          .target_owner = std::move (target_owner),
          .application_payload = std::move (target_application_payload)};
        {
            std::lock_guard lock (mutex);
            current = reference;
        }
        return {authority_publish_status_t::published, std::move (reference)};
    }

    std::optional<authority_relocation_reference_t>
    read (object_kind_t, const std::string &) override
    {
        std::lock_guard lock (mutex);
        return current;
    }

    std::function<void ()> on_publish;
    std::mutex mutex;
    std::optional<authority_relocation_reference_t> current;
};

class actor_cutover_probe_t final : public zlink::framework::actor_t
{
  public:
    actor_cutover_probe_t (zlink::framework::actor_context_t context,
    bool target) :
        _context (std::move (context)), _target (target)
    {
    }

    bool target () const noexcept
    {
        return _target;
    }

    zlink::framework::actor_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::actor_context_t &context () const noexcept override
    {
        return _context;
    }

    zlink::framework::task_t<void> on_join_completed (
      const zlink::framework::actor_join_completion_t &completion) override
    {
        const auto *accepted =
          std::get_if<zlink::framework::actor_join_accepted_t> (&completion);
        if (!accepted) {
            failed_completions.fetch_add (1, std::memory_order_release);
            co_return;
        }
        if (_target) {
            target_operation_high.store (
              accepted->operation_id_high, std::memory_order_release);
            target_operation_low.store (
              accepted->operation_id_low, std::memory_order_release);
            target_completions.fetch_add (1, std::memory_order_release);
            target_completion_entered.store (true, std::memory_order_release);
            co_await target_completion_gate->task ();
        }
        else {
            source_operation_high.store (
              accepted->operation_id_high, std::memory_order_release);
            source_operation_low.store (
              accepted->operation_id_low, std::memory_order_release);
            source_completions.fetch_add (1, std::memory_order_release);
        }
    }

    static void reset ()
    {
        source_completions.store (0, std::memory_order_release);
        target_completions.store (0, std::memory_order_release);
        failed_completions.store (0, std::memory_order_release);
        source_operation_high.store (0, std::memory_order_release);
        source_operation_low.store (0, std::memory_order_release);
        target_operation_high.store (0, std::memory_order_release);
        target_operation_low.store (0, std::memory_order_release);
        target_completion_entered.store (false, std::memory_order_release);
        target_joined.store (false, std::memory_order_release);
        source_leave_entered.store (false, std::memory_order_release);
        source_leave_before_target_joined.store (false, std::memory_order_release);
        source_leave_calls.store (0, std::memory_order_release);
        target_completion_gate = std::make_shared<
          zlink::framework::detail::task_completion_source_t<void>> ();
        source_leave_gate = std::make_shared<
          zlink::framework::detail::task_completion_source_t<void>> ();
    }

    static inline std::atomic_int source_completions{0};
    static inline std::atomic_int target_completions{0};
    static inline std::atomic_int failed_completions{0};
    static inline std::atomic_uint64_t source_operation_high{0};
    static inline std::atomic_uint64_t source_operation_low{0};
    static inline std::atomic_uint64_t target_operation_high{0};
    static inline std::atomic_uint64_t target_operation_low{0};
    static inline std::atomic_bool target_completion_entered{false};
    static inline std::atomic_bool target_joined{false};
    static inline std::atomic_bool source_leave_entered{false};
    static inline std::atomic_bool source_leave_before_target_joined{false};
    static inline std::atomic_int source_leave_calls{0};
    static inline std::shared_ptr<
      zlink::framework::detail::task_completion_source_t<void>>
      target_completion_gate = std::make_shared<
        zlink::framework::detail::task_completion_source_t<void>> ();
    static inline std::shared_ptr<
      zlink::framework::detail::task_completion_source_t<void>>
      source_leave_gate = std::make_shared<
        zlink::framework::detail::task_completion_source_t<void>> ();

  private:
    zlink::framework::actor_context_t _context;
    bool _target;
};

class actor_cutover_probe_factory_t final
    : public zlink::framework::actor_factory_t<actor_cutover_probe_t>
{
  public:
    explicit actor_cutover_probe_factory_t (bool target) :
        _target (target)
    {
    }

    zlink::framework::task_t<std::shared_ptr<actor_cutover_probe_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<actor_cutover_probe_t> (
          std::move (context), _target);
    }

  private:
    bool _target;
};

class actor_cutover_probe_spot_t final
    : public zlink::framework::spot_t<actor_cutover_probe_t>
{
  public:
    explicit actor_cutover_probe_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override
    {
        _context.handlers ().add_actor_send<
          &actor_cutover_probe_spot_t::on_probe> (
            "actor.cutover.probe.noop");
    }
    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }
    zlink::framework::task_t<void> on_initialize () override
    {
        co_return;
    }
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (actor_cutover_probe_t &) override
    {
        actor_cutover_probe_t::target_joined.store (
          true, std::memory_order_release);
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (actor_cutover_probe_t &actor) override
    {
        if (!actor.target ()) {
            actor_cutover_probe_t::source_leave_calls.fetch_add (
              1, std::memory_order_release);
            actor_cutover_probe_t::source_leave_before_target_joined.store (
              !actor_cutover_probe_t::target_joined.load (
                std::memory_order_acquire),
              std::memory_order_release);
            actor_cutover_probe_t::source_leave_entered.store (
              true, std::memory_order_release);
            co_await actor_cutover_probe_t::source_leave_gate->task ();
        }
        co_return;
    }

    zlink::framework::task_t<void>
    on_probe (actor_cutover_probe_t &,
              zlink::framework::message_context_t &,
              const zlink::framework::detail::spot_actor_handoff_packet_t &)
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

bool verify_actor_join_finalize_replies_after_target_activation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> (
      "actor-finalize-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (
      1, 64, "actor-finalize");
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    // The single finalize request keeps its original Join deadline while the
    // target finishes lifecycle and retained backlog publication.
    node->channel_runtime->default_request_timeout = std::chrono::milliseconds (200);
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("actor-finalize-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor = node->worker_executor;
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, 1,
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (
      target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    auto join_completion =
      std::make_shared<detail::task_completion_source_t<void>> ();
    std::atomic_bool join_completion_entered{false};
    std::atomic_int join_completion_calls{0};
    std::atomic_uint64_t join_completion_operation_high{0};
    std::atomic_uint64_t join_completion_operation_low{0};
    factory.on_join_completed =
      [join_completion, &join_completion_entered, &join_completion_calls,
       &join_completion_operation_high, &join_completion_operation_low] (
        void *, actor_join_completion_outcome_t outcome,
        std::uint64_t operation_high, std::uint64_t operation_low,
        const actor_ref_t *,
        const std::optional<message_t> &, framework_error_kind_t, bool)
        -> task_t<void> {
          if (outcome != actor_join_completion_outcome_t::accepted)
              throw std::runtime_error ("deferred completion was not accepted");
          ++join_completion_calls;
          join_completion_operation_high.store (
            operation_high, std::memory_order_release);
          join_completion_operation_low.store (
            operation_low, std::memory_order_release);
          join_completion_entered.store (true, std::memory_order_release);
          co_await join_completion->task ();
      };
    node->actor_factories.emplace ("player", std::move (factory));
    spot_actor_admission_callbacks_t callbacks;
    std::atomic_int actor_joined_calls{0};
    std::atomic_bool replay_before_joined{false};
    callbacks.join = [] (void *, std::string_view, const zlink::message_t &,
                         serializer_registry_t &) {
        return spot_actor_join_result_t::accept (
          message_t::from (std::string ("accepted")));
    };
    callbacks.on_actor_joined = [&actor_joined_calls] (void *, void *) -> task_t<void> {
        ++actor_joined_calls;
        co_return;
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    auto native = std::make_shared<runtime::host::public_host_runtime_t> (
      runtime::host::host_options_t{
        .mesh = {
          .descriptor = {
            .mesh_name = "actor-finalize-mesh",
            .node_routing_id =
              zlink::routing_id_t::from ("actor-finalize-node").to_bytes (),
            .lifecycle_generation = 1,
            .descriptor_revision = 1,
            .advertised_endpoint = "tcp://127.0.0.1:0"}},
        .object_stable_types = {"framework.spot"}});
    native->start ();
    owner.attach_native_node (native);
    auto authority = std::make_shared<actor_cutover_authority_t> ();
    owner.bind_relocation_authority (authority);
    const auto actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c2", 7);
    // The private cutover path still enters the same Store-fenced admission
    // boundary as canonical actorJoin(28).  Model its source Authority row
    // rather than reviving the retired local actor-type cache.
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    const auto set_source_authority = [store] (
      const actor_ref_t &source_actor,
      std::uint64_t authority_owner_generation) {
        store->snapshot = authority_snapshot_t{
          .store_version = "actor-cutover-v1",
          .payload = runtime::encode_actor_authority_payload (
            source_actor, "source-spot", 1),
          .object_generation = source_actor.object_generation (),
          .authority_owner_generation = authority_owner_generation,
          .owner = location_owner_token_t{"source-owner", 29},
          .store_now = std::chrono::system_clock::now (),
          .allocation = {.state = placement_allocation_state_t::active,
                         .object_kind = placement_object_kind_t::actor,
                         .stable_type = "player",
                         .target = {.mesh_name = "source-mesh",
                                    .node_rid = node_rid_t::from_string (
                                      std::string (source_actor.node_rid ().value ())),
                                    .node_lifecycle_generation = 1,
                                    .owner = location_owner_token_t{
                                      "source-owner", 29}}}};
    };
    set_source_authority (actor, 19);
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    actor_gateway_runtime_t gateway;
    auto session = gateway.manager ();
    session_actor_manager_access_t::attach (session, stream_t{});
    if (!session.bind (actor).submit ().result ()) {
        return false;
    }
    std::atomic_int replayed{0};
    std::atomic_bool successor_join_reserved{false};
    auto first_replay =
      std::make_shared<detail::task_completion_source_t<void>> ();
    std::atomic_bool first_replay_entered{false};
    std::mutex delivery_order_mutex;
    std::vector<std::string> delivery_order;
    target->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::actor_send, "BacklogPacket", "",
      std::type_index (typeid (int)), std::type_index (typeid (int)),
      std::type_index (typeid (int)), std::type_index (typeid (void))});
    target->handler_invokers.push_back (
      [&owner, actor, &replayed, &successor_join_reserved,
       &actor_joined_calls, &replay_before_joined,
       first_replay, &first_replay_entered,
       &delivery_order_mutex, &delivery_order] (
        void *, void *, service_provider_t &, serializer_registry_t &,
        const zlink::message_t &, const spot_inbound_message_t &metadata)
        -> task_t<zlink::message_t> {
          const auto sequence = metadata.find ("sequence");
          if (!sequence)
              throw std::runtime_error ("handoff replay sequence is missing");
          if (actor_joined_calls.load (std::memory_order_acquire) == 0)
              replay_before_joined.store (true, std::memory_order_release);
          if (*sequence == "B1") {
              first_replay_entered.store (true, std::memory_order_release);
              co_await first_replay->task ();
          }
          ++replayed;
          {
              std::lock_guard lock (delivery_order_mutex);
              delivery_order.push_back (std::string (*sequence));
          }
          if (*sequence == "B1") {
              auto reserved = owner.reserve_actor_join_barrier (actor);
              successor_join_reserved.store (
                static_cast<bool> (reserved), std::memory_order_release);
              if (reserved)
                  reserved.value ()->cancel ();
          }
          co_return zlink::message_t{};
      });

    const std::string transfer_id = "transfer-c2";
    const std::string key = "player:actor-c2";
    const auto admitted = owner.admit_remote_actor_to_spot (
      transfer_id, actor, spot_id_t ("source-spot"), target->spot_id,
        zlink::message_t::from (std::string ("prepare")), 11, 13, 19, 1, 29);
    if (!admitted || !admitted.value ().accepted) {
        return false;
    }
    const auto prepared = owner.prepare_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (actor), true);
    if (!prepared) {
        return false;
    }

    spot_actor_commit_route_request_t cutover{
      .transfer_id = transfer_id,
      .actor_node_rid = "source-node",
      .actor_type = "player",
      .actor_id = "actor-c2",
      .actor_generation = 7,
      .actor_authority_owner_generation = 19,
      .target_spot_id = "target-spot",
      .target_spot_generation = 1,
      .source_mesh_name = "source-mesh",
      .target_mesh_name = "actor-finalize-mesh",
      .target_node_lifecycle_generation =
        native->status ().lifecycle_generation (),
      .target_owner_id = "target-owner",
      .target_owner_lease_generation = 23,
      .source_spot_id = "source-spot",
      .source_spot_generation = 1,
      .handoff_backlog = {
        spot_actor_handoff_packet_t{
          .packet_name_value = "BacklogPacket",
          .payload = {1},
          .content_type = "application/x-test",
          .metadata = {{"sequence", "B1"}}},
        spot_actor_handoff_packet_t{
          .packet_name_value = "BacklogPacket",
          .payload = {2},
          .content_type = "application/x-test",
          .metadata = {{"sequence", "B2"}}}},
      .finalize = true};
    authority->on_publish = [&] {
        const auto appended = node->actor_transfer_coordinator.try_append_backlog (
          key, handoff_packet_t{
                 "BacklogPacket", {3}, "application/x-test",
                 {{"sequence", "D1"}}, false});
        if (appended != handoff_append_result_t::appended)
            throw std::runtime_error (
              "direct packet was not retained at the authority boundary");
    };
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "actor-route";
    header.message_name = spot_actor_commit_route_request_t::packet_name;
    const auto parts = runtime::messaging::envelope_codec_t{}.encode_parts (
      header, cutover, serializers);
    spot_route_internal_dispatcher_t dispatcher (
      owner, gateway, route_client_t{}, serializers);
    if (!dispatcher.can_handle_send (
          spot_actor_commit_route_request_t::packet_name)) {
        return false;
    }
    const auto submitted = dispatcher.dispatch_send (
      route_received_packet_t{
        zlink::routing_id_t::from ("source-node"), 1, parts},
      provider);
    const auto join_completion_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!join_completion_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < join_completion_deadline) {
        std::this_thread::yield ();
    }
    if (!submitted
        || !join_completion_entered.load (std::memory_order_acquire)
        || actor_joined_calls.load (std::memory_order_acquire) != 1
        || replayed.load (std::memory_order_acquire) != 0
        || !node->actor_transfer_coordinator.blocks_dispatch (key)) {
        std::cerr << "actor cutover dispatch diagnostic submitted="
                  << static_cast<bool> (submitted)
                  << " error="
                  << (submitted.error () ? submitted.error ()->what () : "<none>")
                  << " completion-entered=" << join_completion_entered.load ()
                  << " joined=" << actor_joined_calls.load ()
                  << " replayed=" << replayed.load ()
                  << " blocked="
                  << node->actor_transfer_coordinator.blocks_dispatch (key)
                  << '\n';
        return false;
    }
    join_completion->complete (result_t<void>::success ());
    const auto replay_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!first_replay_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < replay_deadline) {
        std::this_thread::yield ();
    }
    if (!first_replay_entered.load (std::memory_order_acquire))
        return false;
    first_replay->complete (result_t<void>::success ());
    const auto finalized_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while ((!owner.completed_remote_actor_commit (
              transfer_id, actor, target->spot_id)
            || replayed.load (std::memory_order_acquire) != 3)
           && std::chrono::steady_clock::now () < finalized_deadline) {
        std::this_thread::yield ();
    }
    std::vector<std::string> observed_delivery_order;
    {
        std::lock_guard lock (delivery_order_mutex);
        observed_delivery_order = delivery_order;
    }
    if (node->actor_transfer_coordinator.blocks_dispatch (key)
        || replayed.load () != 3
        || replay_before_joined.load (std::memory_order_acquire)
        || !join_completion_entered.load (std::memory_order_acquire)
        || join_completion_calls.load () != 1
        || join_completion_operation_high.load (std::memory_order_acquire) != 11
        || join_completion_operation_low.load (std::memory_order_acquire) != 13
        || actor_joined_calls.load (std::memory_order_acquire) != 1
        || !first_replay_entered.load (std::memory_order_acquire)
        || !successor_join_reserved.load (std::memory_order_acquire)
        || observed_delivery_order
             != std::vector<std::string>{"B1", "B2", "D1"}) {
        std::cerr << "actor cutover replay diagnostic blocked="
                  << node->actor_transfer_coordinator.blocks_dispatch (key)
                  << " replayed=" << replayed.load ()
                  << " completion-calls=" << join_completion_calls.load ()
                  << " joined=" << actor_joined_calls.load ()
                  << " order=";
        for (const auto &item : observed_delivery_order)
            std::cerr << item << ',';
        std::cerr << '\n';
        return false;
    }
    if (!owner.completed_remote_actor_commit (transfer_id, actor, target->spot_id)) {
        return false;
    }
    // A duplicate internal finalize cannot reopen a completed target or
    // dispatch its retained backlog twice.
    const auto duplicate = owner.finalize_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, provider, nullptr,
        std::nullopt);
    if (duplicate || node->actor_transfer_coordinator.blocks_dispatch (key)
        || replayed.load () != 3
        || !owner.completed_remote_actor_commit (transfer_id, actor, target->spot_id)) {
        return false;
    }

    // If the deadline wins while the replay owner is still queued, no Join
    // completion, location publication or completed admission may escape.
    const auto queued_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c3", 7);
    const std::string queued_transfer_id = "transfer-c3";
    const std::string queued_key = "player:actor-c3";
    set_source_authority (queued_actor, 29);
    const auto queued_admitted = owner.admit_remote_actor_to_spot (
      queued_transfer_id, queued_actor, spot_id_t ("source-spot"),
      target->spot_id, zlink::message_t::from (std::string ("prepare")),
      21, 23, 29, 1, 29);
    const auto queued_prepared = owner.prepare_remote_actor_to_spot (
      queued_transfer_id, queued_actor, target->spot_id, zlink::message_t{},
      actor_gateway_runtime_t{}.actor_context (queued_actor), true);
    if (!queued_admitted || !queued_admitted.value ().accepted
        || !queued_prepared) {
        return false;
    }

    runtime::serial_execution_queue_options_t queued_options;
    queued_options.application_message_capacity = 2;
    queued_options.application_byte_capacity =
      2 * runtime::serial_execution_queue_t::fixed_work_byte_cost;
    auto queued_actor_queue =
      std::make_shared<runtime::serial_execution_queue_t> (
        *node->worker_executor, queued_options,
        runtime::serial_execution_queue_t::error_handler_t{},
        runtime::serial_lane_policy_t::actor_delivery ());
    node->actor_execution_queues[queued_key] = queued_actor_queue;
    publish_actor_execution_queue_snapshot_unlocked (*node);
    std::mutex queued_gate;
    std::condition_variable queued_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t>
      release_predecessor;
    if (!queued_actor_queue->try_post_async (
          "deadline-predecessor",
          [&] (auto complete) {
              {
                  std::lock_guard lock (queued_gate);
                  release_predecessor.emplace (std::move (complete));
              }
              queued_changed.notify_all ();
          })) {
        return false;
    }
    {
        std::unique_lock lock (queued_gate);
        if (!queued_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return release_predecessor.has_value (); })) {
            queued_actor_queue->cancel_pending ();
            return false;
        }
    }
    const auto completion_calls_before_cancel = join_completion_calls.load ();
    std::mutex cancelled_mutex;
    std::condition_variable cancelled_changed;
    std::optional<result_t<actor_join_reply_t>> cancelled;
    owner.finalize_remote_actor_to_spot_async (
      queued_transfer_id, queued_actor, target->spot_id, provider, nullptr,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (10),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (cancelled_mutex);
              cancelled.emplace (std::move (result));
          }
          cancelled_changed.notify_all ();
      });
    {
        std::unique_lock lock (cancelled_mutex);
        if (!cancelled_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return cancelled.has_value (); })) {
            return false;
        }
    }
    runtime::serial_execution_queue_t::async_completion_t finish_predecessor;
    {
        std::lock_guard lock (queued_gate);
        finish_predecessor = std::move (*release_predecessor);
    }
    finish_predecessor ([] {});
    queued_actor_queue->drain ();
    if (*cancelled
        || cancelled->error_kind ()
             != framework_error_kind_t::deadline_exceeded
        || node->actor_transfer_coordinator.phase (queued_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (
             queued_transfer_id, queued_actor, target->spot_id)
        || join_completion_calls.load () != completion_calls_before_cancel) {
        return false;
    }
    std::weak_ptr<runtime::serial_execution_queue_t> cancelled_queue_owner =
      queued_actor_queue;
    node->actor_execution_queues.erase (queued_key);
    publish_actor_execution_queue_snapshot_unlocked (*node);
    queued_actor_queue.reset ();
    if (!cancelled_queue_owner.expired ()) {
        return false;
    }

    // Destroying the deadline owner must happen after the native timer callback
    // returns. A second timer proves the shared scheduler remains available.
    std::mutex probe_mutex;
    std::condition_variable probe_changed;
    bool probe_fired = false;
    zlink::timer_t probe_timer;
    probe_timer.on_fire ([&] (std::uint64_t) {
        {
            std::lock_guard lock (probe_mutex);
            probe_fired = true;
        }
        probe_changed.notify_all ();
    });
    probe_timer.start (std::chrono::milliseconds (1), 1);
    {
        std::unique_lock lock (probe_mutex);
        if (!probe_changed.wait_for (
              lock, std::chrono::seconds (1), [&] { return probe_fired; })) {
            return false;
        }
    }
    probe_timer.close ();

    // A deadline that expires while on_actor_joined is queued removes that
    // lifecycle submission. The failed transfer must not run the callback
    // later when the predecessor releases the Spot queue.
    const auto lifecycle_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c4", 7);
    const std::string lifecycle_transfer_id = "transfer-c4";
    const std::string lifecycle_key = "player:actor-c4";
    set_source_authority (lifecycle_actor, 41);
    const auto lifecycle_admitted = owner.admit_remote_actor_to_spot (
      lifecycle_transfer_id, lifecycle_actor, spot_id_t ("source-spot"),
      target->spot_id, zlink::message_t::from (std::string ("prepare")),
      31, 37, 41, 1, 29);
    const auto lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      lifecycle_transfer_id, lifecycle_actor, target->spot_id,
      zlink::message_t{}, gateway.actor_context (lifecycle_actor), true);
    if (!lifecycle_admitted || !lifecycle_admitted.value ().accepted
        || !lifecycle_prepared) {
        return false;
    }
    std::mutex lifecycle_gate;
    std::condition_variable lifecycle_gate_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t>
      release_lifecycle_predecessor;
    if (!target->serial_queue->try_post_async (
          "deadline-lifecycle-predecessor",
          [&] (auto complete) {
              {
                  std::lock_guard lock (lifecycle_gate);
                  release_lifecycle_predecessor.emplace (std::move (complete));
              }
              lifecycle_gate_changed.notify_all ();
          },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost})) {
        return false;
    }
    {
        std::unique_lock lock (lifecycle_gate);
        if (!lifecycle_gate_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return release_lifecycle_predecessor.has_value (); })) {
            return false;
        }
    }
    const auto joined_calls_before_lifecycle_cancel =
      actor_joined_calls.load (std::memory_order_acquire);
    std::mutex lifecycle_cancel_mutex;
    std::condition_variable lifecycle_cancel_changed;
    std::optional<result_t<actor_join_reply_t>> lifecycle_cancelled;
    owner.finalize_remote_actor_to_spot_async (
      lifecycle_transfer_id, lifecycle_actor, target->spot_id, provider,
      &gateway,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (10),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (lifecycle_cancel_mutex);
              lifecycle_cancelled.emplace (std::move (result));
          }
          lifecycle_cancel_changed.notify_all ();
      });
    {
        std::unique_lock lock (lifecycle_cancel_mutex);
        if (!lifecycle_cancel_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return lifecycle_cancelled.has_value (); })) {
            return false;
        }
    }
    runtime::serial_execution_queue_t::async_completion_t
      finish_lifecycle_predecessor;
    {
        std::lock_guard lock (lifecycle_gate);
        finish_lifecycle_predecessor =
          std::move (*release_lifecycle_predecessor);
    }
    finish_lifecycle_predecessor ([] {});
    target->serial_queue->drain ();
    if (*lifecycle_cancelled
        || lifecycle_cancelled->error_kind ()
             != framework_error_kind_t::deadline_exceeded
        || actor_joined_calls.load (std::memory_order_acquire)
             != joined_calls_before_lifecycle_cancel
        || node->actor_transfer_coordinator.phase (lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (
             lifecycle_transfer_id, lifecycle_actor, target->spot_id)) {
        return false;
    }

    // If the deadline expires after on_actor_joined has started, the transfer
    // keeps its terminal owner until that callback settles. The callback's
    // terminal is converted into the admitted Join failure completion before
    // the transfer enters reconciliation.
    auto active_lifecycle =
      std::make_shared<detail::task_completion_source_t<void>> ();
    std::atomic_bool active_lifecycle_entered{false};
    std::atomic_int active_lifecycle_failure_calls{0};
    node->actor_factories.at ("player").on_join_completed =
      [&active_lifecycle_failure_calls] (
        void *, actor_join_completion_outcome_t outcome,
        std::uint64_t operation_high, std::uint64_t operation_low,
        const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t error_kind, bool) -> task_t<void> {
          if (outcome != actor_join_completion_outcome_t::failed
              || operation_high != 59 || operation_low != 61
              || error_kind != framework_error_kind_t::deadline_exceeded) {
              throw std::runtime_error (
                "active lifecycle deadline completion lost its OperationId");
          }
          ++active_lifecycle_failure_calls;
          co_return;
      };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [active_lifecycle,
       &active_lifecycle_entered] (void *, void *) -> task_t<void> {
          active_lifecycle_entered.store (true, std::memory_order_release);
          co_await active_lifecycle->task ();
      };
    const auto active_lifecycle_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c4-active", 7);
    const std::string active_lifecycle_transfer_id = "transfer-c4-active";
    const std::string active_lifecycle_key = "player:actor-c4-active";
    set_source_authority (active_lifecycle_actor, 67);
    const auto active_lifecycle_admitted = owner.admit_remote_actor_to_spot (
      active_lifecycle_transfer_id, active_lifecycle_actor,
      spot_id_t ("source-spot"), target->spot_id,
      zlink::message_t::from (std::string ("prepare")), 59, 61, 67, 1, 29);
    const auto active_lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      active_lifecycle_transfer_id, active_lifecycle_actor, target->spot_id,
      zlink::message_t{}, gateway.actor_context (active_lifecycle_actor), true);
    if (!active_lifecycle_admitted
        || !active_lifecycle_admitted.value ().accepted
        || !active_lifecycle_prepared) {
        return false;
    }
    std::mutex active_lifecycle_mutex;
    std::condition_variable active_lifecycle_changed;
    std::optional<result_t<actor_join_reply_t>> active_lifecycle_result;
    owner.finalize_remote_actor_to_spot_async (
      active_lifecycle_transfer_id, active_lifecycle_actor, target->spot_id,
      provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (20),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (active_lifecycle_mutex);
              active_lifecycle_result.emplace (std::move (result));
          }
          active_lifecycle_changed.notify_all ();
      });
    const auto active_lifecycle_start_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!active_lifecycle_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < active_lifecycle_start_deadline) {
        std::this_thread::yield ();
    }
    if (!active_lifecycle_entered.load (std::memory_order_acquire))
        return false;
    {
        std::unique_lock lock (active_lifecycle_mutex);
        if (active_lifecycle_changed.wait_for (
              lock, std::chrono::milliseconds (80),
              [&] { return active_lifecycle_result.has_value (); })) {
            return false;
        }
    }
    if (active_lifecycle_failure_calls.load (std::memory_order_acquire) != 0)
        return false;
    active_lifecycle->complete (result_t<void>::success ());
    {
        std::unique_lock lock (active_lifecycle_mutex);
        if (!active_lifecycle_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return active_lifecycle_result.has_value (); })) {
            return false;
        }
    }
    if (*active_lifecycle_result
        || active_lifecycle_result->error_kind ()
             != framework_error_kind_t::deadline_exceeded
        || active_lifecycle_failure_calls.load (std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator.phase (active_lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (
             active_lifecycle_transfer_id, active_lifecycle_actor,
             target->spot_id)) {
        return false;
    }

    // Host shutdown uses the same cooperative lifecycle cancellation seam but
    // keeps its own terminal reason. It must not be rewritten as a deadline,
    // and the terminal owner remains held until the active callback settles.
    auto shutdown_lifecycle =
      std::make_shared<detail::task_completion_source_t<void>> ();
    std::atomic_bool shutdown_lifecycle_entered{false};
    std::atomic_int shutdown_lifecycle_failure_calls{0};
    node->actor_factories.at ("player").on_join_completed =
      [&shutdown_lifecycle_failure_calls] (
        void *, actor_join_completion_outcome_t outcome,
        std::uint64_t operation_high, std::uint64_t operation_low,
        const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t error_kind, bool) -> task_t<void> {
          if (outcome != actor_join_completion_outcome_t::failed
              || operation_high != 71 || operation_low != 73
              || error_kind != framework_error_kind_t::shutting_down) {
              throw std::runtime_error (
                "active lifecycle shutdown completion lost its terminal reason");
          }
          ++shutdown_lifecycle_failure_calls;
          co_return;
      };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [shutdown_lifecycle,
       &shutdown_lifecycle_entered] (void *, void *) -> task_t<void> {
          shutdown_lifecycle_entered.store (true, std::memory_order_release);
          co_await shutdown_lifecycle->task ();
      };
    const auto shutdown_lifecycle_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c4-shutdown", 7);
    const std::string shutdown_lifecycle_transfer_id = "transfer-c4-shutdown";
    const std::string shutdown_lifecycle_key = "player:actor-c4-shutdown";
    set_source_authority (shutdown_lifecycle_actor, 79);
    const auto shutdown_lifecycle_admitted = owner.admit_remote_actor_to_spot (
      shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor,
      spot_id_t ("source-spot"), target->spot_id,
      zlink::message_t::from (std::string ("prepare")), 71, 73, 79, 1, 29);
    const auto shutdown_lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor,
      target->spot_id, zlink::message_t{},
      gateway.actor_context (shutdown_lifecycle_actor), true);
    if (!shutdown_lifecycle_admitted
        || !shutdown_lifecycle_admitted.value ().accepted
        || !shutdown_lifecycle_prepared) {
        return false;
    }
    std::mutex shutdown_lifecycle_mutex;
    std::condition_variable shutdown_lifecycle_changed;
    std::optional<result_t<actor_join_reply_t>> shutdown_lifecycle_result;
    owner.finalize_remote_actor_to_spot_async (
      shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor,
      target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (shutdown_lifecycle_mutex);
              shutdown_lifecycle_result.emplace (std::move (result));
          }
          shutdown_lifecycle_changed.notify_all ();
      });
    const auto shutdown_lifecycle_start_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!shutdown_lifecycle_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now ()
                < shutdown_lifecycle_start_deadline) {
        std::this_thread::yield ();
    }
    if (!shutdown_lifecycle_entered.load (std::memory_order_acquire))
        return false;
    target->serial_queue->cancel_pending ();
    {
        std::unique_lock lock (shutdown_lifecycle_mutex);
        if (shutdown_lifecycle_changed.wait_for (
              lock, std::chrono::milliseconds (40),
              [&] { return shutdown_lifecycle_result.has_value (); })) {
            return false;
        }
    }
    shutdown_lifecycle->complete (result_t<void>::success ());
    {
        std::unique_lock lock (shutdown_lifecycle_mutex);
        if (!shutdown_lifecycle_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return shutdown_lifecycle_result.has_value (); })) {
            return false;
        }
    }
    if (*shutdown_lifecycle_result
        || shutdown_lifecycle_result->error_kind ()
             != framework_error_kind_t::shutting_down
        || shutdown_lifecycle_failure_calls.load (
             std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator.phase (shutdown_lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (
             shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor,
             target->spot_id)) {
        return false;
    }
    target->serial_queue->drain ();
    target->serial_queue =
      std::make_shared<runtime::serial_execution_queue_t> (
        *target->serial_executor, 1,
        runtime::serial_execution_queue_t::error_handler_t{},
        runtime::serial_lane_policy_t::spot_wide ());

    // An admitted lifecycle failure still owns a Join completion terminal.
    // The source OperationId must observe exactly one failed completion before
    // the target transfer enters reconciliation.
    std::atomic_int failed_join_completion_calls{0};
    node->actor_factories.at ("player").on_join_completed =
      [&failed_join_completion_calls] (
        void *, actor_join_completion_outcome_t outcome,
        std::uint64_t operation_high, std::uint64_t operation_low,
        const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t error_kind, bool) -> task_t<void> {
          if (outcome != actor_join_completion_outcome_t::failed
              || operation_high != 43 || operation_low != 47
              || error_kind != framework_error_kind_t::internal_failure) {
              throw std::runtime_error (
                "lifecycle failure completion did not preserve its OperationId");
          }
          ++failed_join_completion_calls;
          co_return;
      };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [] (void *, void *) -> task_t<void> {
          throw framework_exception_t (
            framework_error_kind_t::internal_failure,
            "deterministic lifecycle failure");
          co_return;
      };
    const auto failed_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c5", 7);
    const std::string failed_transfer_id = "transfer-c5";
    const std::string failed_key = "player:actor-c5";
    set_source_authority (failed_actor, 53);
    const auto failed_admitted = owner.admit_remote_actor_to_spot (
      failed_transfer_id, failed_actor, spot_id_t ("source-spot"),
      target->spot_id, zlink::message_t::from (std::string ("prepare")),
      43, 47, 53, 1, 29);
    const auto failed_prepared = owner.prepare_remote_actor_to_spot (
      failed_transfer_id, failed_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (failed_actor), true);
    if (!failed_admitted || !failed_admitted.value ().accepted
        || !failed_prepared) {
        return false;
    }
    std::mutex failed_mutex;
    std::condition_variable failed_changed;
    std::optional<result_t<actor_join_reply_t>> failed_result;
    owner.finalize_remote_actor_to_spot_async (
      failed_transfer_id, failed_actor, target->spot_id, provider,
      &gateway, std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (failed_mutex);
              failed_result.emplace (std::move (result));
          }
          failed_changed.notify_all ();
      });
    {
        std::unique_lock lock (failed_mutex);
        if (!failed_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return failed_result.has_value (); })) {
            return false;
        }
    }
    if (*failed_result
        || failed_result->error_kind ()
             != framework_error_kind_t::internal_failure
        || failed_join_completion_calls.load (std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator.phase (failed_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (
             failed_transfer_id, failed_actor, target->spot_id)) {
        return false;
    }

    // The target owns the cutover order. A one-way source leave submission
    // terminal may fail, but it is still attempted after OnJoined and before
    // the target publishes Accepted.
    std::atomic_int cutover_order{0};
    std::atomic_int target_joined_order{0};
    std::atomic_int source_leave_submit_order{0};
    std::atomic_int target_accepted_order{0};
    node->actor_factories.at ("player").on_join_completed =
      [&cutover_order, &target_accepted_order] (
        void *, actor_join_completion_outcome_t outcome,
        std::uint64_t operation_high, std::uint64_t operation_low,
        const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t, bool) -> task_t<void> {
          if (outcome != actor_join_completion_outcome_t::accepted
              || operation_high != 83 || operation_low != 89) {
              throw std::runtime_error (
                "source leave ordering lost the target OperationId");
          }
          target_accepted_order.store (
            ++cutover_order, std::memory_order_release);
          co_return;
      };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [&cutover_order, &target_joined_order] (void *, void *) -> task_t<void> {
          target_joined_order.store (
            ++cutover_order, std::memory_order_release);
          co_return;
      };
    const auto leave_order_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c6", 7);
    const std::string leave_order_transfer_id = "transfer-c6";
    set_source_authority (leave_order_actor, 97);
    const auto leave_order_admitted = owner.admit_remote_actor_to_spot (
      leave_order_transfer_id, leave_order_actor, spot_id_t ("source-spot"),
      target->spot_id, zlink::message_t::from (std::string ("prepare")),
      83, 89, 97, 1, 29);
    const auto leave_order_prepared = owner.prepare_remote_actor_to_spot (
      leave_order_transfer_id, leave_order_actor, target->spot_id,
      zlink::message_t{}, gateway.actor_context (leave_order_actor), true);
    if (!leave_order_admitted || !leave_order_admitted.value ().accepted
        || !leave_order_prepared) {
        return false;
    }
    std::mutex leave_order_mutex;
    std::condition_variable leave_order_changed;
    std::optional<result_t<actor_join_reply_t>> leave_order_result;
    owner.finalize_remote_actor_to_spot_async (
      leave_order_transfer_id, leave_order_actor, target->spot_id,
      provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (leave_order_mutex);
              leave_order_result.emplace (std::move (result));
          }
          leave_order_changed.notify_all ();
      },
      [&cutover_order, &source_leave_submit_order] {
          source_leave_submit_order.store (
            ++cutover_order, std::memory_order_release);
          return task_t<void> (result_t<void>::failure (
            framework_error_kind_t::internal_failure,
            "deterministic source leave submit failure"));
      });
    {
        std::unique_lock lock (leave_order_mutex);
        if (!leave_order_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return leave_order_result.has_value (); })) {
            return false;
        }
    }
    if (!*leave_order_result
        || target_joined_order.load (std::memory_order_acquire) != 1
        || source_leave_submit_order.load (std::memory_order_acquire) != 2
        || target_accepted_order.load (std::memory_order_acquire) != 3) {
        return false;
    }

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    return true;
}

bool verify_remote_actor_cutover_completion_is_target_owned ()
{
    using namespace std::chrono_literals;
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    actor_cutover_probe_t::reset ();
    serializer_registry_t serializers;
    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_state = [&] (const std::string &rid, bool target) {
        auto state = std::make_shared<mesh_node_builder_state_t> (
          "actor-cutover-mesh");
        state->core_context = core_context;
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_state->snapshot.routing_id = *state->routing_id;
        state->spot_state->channel_runtime =
          std::make_shared<channel_runtime_state_t> ();
        state->spot_state->channel_runtime->serializers = &serializers;
        state->spot_builder.add_spot_factory<actor_cutover_probe_spot_t> (
          "actor.cutover.spot",
          [] (spot_context_t context) {
              return std::make_shared<actor_cutover_probe_spot_t> (
                std::move (context));
          },
          [] (auto &factory) { factory.recreate_on_relocation (); });
        state->spot_builder.add_actor_factory<
          actor_cutover_probe_t, actor_cutover_probe_factory_t> (
          "actor.cutover.probe",
          std::make_shared<actor_cutover_probe_factory_t> (target),
          [] (auto &factory) { factory.recreate_on_relocation (); });
        return state;
    };

    auto locations =
      std::make_shared<runtime::in_memory_location_repository_t> ();
    const auto source_owner = std::get<owner_lease_claimed_t> (
      locations->claim_owner_lease ("source-owner", 30s)
        .result ().value ()).token;
    const auto target_owner = std::get<owner_lease_claimed_t> (
      locations->claim_owner_lease ("target-owner", 30s)
        .result ().value ()).token;
    auto relocation_store =
      std::make_shared<runtime::in_memory_relocation_store_t> ();
    auto relocation_repository =
      std::make_shared<runtime::provider_relocation_repository_t> (
        *relocation_store);
    auto authority = std::make_shared<
      stateful::public_authority_store_adapter_t> (*locations);
    auto relocations = std::make_shared<
      stateful::public_relocation_store_adapter_t> (
        relocation_repository);
    auto source_state = make_state ("actor-cutover-source", false);
    auto target_state = make_state ("actor-cutover-target", true);
    mesh_node_runtime_t source (source_state);
    mesh_node_runtime_t target (target_state);
    struct spot_route_fixture_t
    {
        zlink::routing_id_t node = zlink::routing_id_t::from (
          std::uint32_t{0});
        std::uint64_t generation = 0;
        runtime::host::route_fence_t fence;
    };
    std::map<std::string, spot_route_fixture_t> spot_routes;
    const auto resolve_spot_route =
      [&spot_routes] (const zlink::routing_id_t &node,
                      std::string_view spot_id,
                      std::uint64_t generation)
        -> std::optional<runtime::host::route_fence_t> {
          const auto found = spot_routes.find (std::string (spot_id));
          if (found == spot_routes.end ()
              || found->second.node != node
              || found->second.generation != generation) {
              return std::nullopt;
          }
          return found->second.fence;
      };
    source.bind_serializers (serializers);
    target.bind_serializers (serializers);
    source.configure_spot_route_fence_resolver (
      resolve_spot_route, 0ms, 0ms);
    target.configure_spot_route_fence_resolver (
      resolve_spot_route, 0ms, 0ms);
    source.configure_user_spot_operations (
      locations,
      [] (const stateful::object_ref_t &, const std::string &,
          const std::vector<std::byte> &) {
          return runtime::host::user_spot_materialize_result_t{
            true, std::nullopt};
      });
    source.configure_relocation_runtime (authority, relocations);
    target.configure_relocation_runtime (authority, relocations);
    source.configure_session_route_owner (
      [source_owner] {
          return std::optional<location_owner_token_t>{source_owner};
      });
    target.configure_session_route_owner (
      [target_owner] {
          return std::optional<location_owner_token_t>{target_owner};
      });
    source.start ();
    target.start ();

    const auto register_node = [&] (
      mesh_node_runtime_t &node,
      const location_owner_token_t &owner) {
        const auto status = node.status ();
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "actor-cutover-mesh";
        descriptor.rid = status.routing_id ();
        descriptor.lifecycle_generation =
          status.lifecycle_generation ();
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = status.local_endpoint ();
        descriptor.application_version = 1;
        descriptor.object_capabilities = {
          {.object_kind = placement_object_kind_t::actor,
           .stable_type = "actor.cutover.probe"},
          {.object_kind = placement_object_kind_t::user_spot,
           .stable_type = "actor.cutover.spot"}};
        descriptor.object_role = object_role_t::server;
        descriptor.capacity = {
          .actors = {.limit = 8}, .spots = {.limit = 8}};
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "actor-cutover-test";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        const auto updated = locations->update_mesh_node (
          std::move (descriptor), location_write_intent_t::new_claim)
          .result ().value ();
        if (updated.status != location_write_status_t::stored)
            std::cerr << "cutover node registration status="
                      << static_cast<int> (updated.status) << '\n';
        return updated.status == location_write_status_t::stored;
    };
    if (!register_node (source, source_owner)
        || !register_node (target, target_owner)) {
        std::cerr << "cutover production setup: node registration failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    spot_node_runtime_t source_spots (source_state->spot_state);
    spot_node_runtime_t target_spots (target_state->spot_state);
    // The two-node command-28 receiver resolves the source Actor type and
    // exact fence from the same authority repository the fixture commits
    // below.  Bind that production dependency before peer traffic starts.
    service_collection_t location_services;
    location_services.add_factory<runtime::live_location_reader_t> (
      [locations] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*locations);
      },
      service_lifetime_t::singleton);
    auto location_provider = location_services.build_provider ();
    source_spots.bind_service_provider (location_provider);
    target_spots.bind_service_provider (location_provider);
    source_spots.bind_relocation_store (relocations);
    source_spots.bind_relocation_authority (authority);
    target_spots.bind_relocation_store (relocations);
    target_spots.bind_relocation_authority (authority);
    const auto source_spot =
      source_spots.create_spot ("actor.cutover.spot");
    const auto target_spot =
      target_spots.create_spot ("actor.cutover.spot");
    auto source_native_spot = source.get_or_create_spot (
      std::string (source_spot.spot_id));
    auto target_native_spot = target.get_or_create_spot (
      std::string (target_spot.spot_id));

    const object_reserve_request_t source_spot_reserve{
      .key = {placement_object_kind_t::user_spot,
              source_native_spot.spot_id ()},
      .intent = {.stable_type = "actor.cutover.spot"},
      .target = {
        .mesh_name = "actor-cutover-mesh",
        .node_rid = node_rid_t::from_string (
          source.status ().routing_id ().to_string ()),
        .node_lifecycle_generation =
          source.status ().lifecycle_generation (),
        .owner = source_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          placement_object_kind_t::user_spot,
          "actor.cutover.spot", 1}}};
    const auto source_spot_reserved_value =
      locations->reserve (source_spot_reserve).result ().value ();
    const auto *source_spot_reserved =
      std::get_if<object_reserved_t> (&source_spot_reserved_value);
    if (!source_spot_reserved
        || !std::holds_alternative<object_committed_t> (
          locations->commit (
            {source_spot_reserve.key, source_spot_reserved->fence, {}})
            .result ().value ())) {
        std::cerr << "cutover production setup: source Spot authority failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    spot_routes.insert_or_assign (
      source_native_spot.spot_id (),
      spot_route_fixture_t{
        source.status ().routing_id (),
        source_native_spot.status ().lifecycle_generation (),
        {source_spot_reserved->fence.authority_owner_generation,
         static_cast<std::uint64_t> (
           source_owner.lease_generation)}});

    const object_reserve_request_t target_spot_reserve{
      .key = {placement_object_kind_t::user_spot,
              target_native_spot.spot_id ()},
      .intent = {.stable_type = "actor.cutover.spot"},
      .target = {
        .mesh_name = "actor-cutover-mesh",
        .node_rid = node_rid_t::from_string (
          target.status ().routing_id ().to_string ()),
        .node_lifecycle_generation =
          target.status ().lifecycle_generation (),
        .owner = target_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          placement_object_kind_t::user_spot,
          "actor.cutover.spot", 1}}};
    const auto target_spot_reserved_value =
      locations->reserve (target_spot_reserve).result ().value ();
    const auto *target_spot_reserved =
      std::get_if<object_reserved_t> (&target_spot_reserved_value);
    if (!target_spot_reserved
        || !std::holds_alternative<object_committed_t> (
          locations->commit (
            {target_spot_reserve.key, target_spot_reserved->fence, {}})
            .result ().value ())) {
        std::cerr << "cutover production setup: target Spot authority failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    spot_routes.insert_or_assign (
      target_native_spot.spot_id (),
      spot_route_fixture_t{
        target.status ().routing_id (),
        target_native_spot.status ().lifecycle_generation (),
        {target_spot_reserved->fence.authority_owner_generation,
         static_cast<std::uint64_t> (
           target_owner.lease_generation)}});

    source.connect_peer (
      target.status ().routing_id (), target.status ().local_endpoint ());
    const auto discard = [] (const auto &, const auto &, auto) {};
    const auto admitted_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (
               target.status ().routing_id (),
               target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now () < admitted_deadline) {
        (void) source.dispatch_ready (discard);
        (void) target.dispatch_ready (discard);
        std::this_thread::sleep_for (1ms);
    }
    if (!source.has_admitted_peer (
          target.status ().routing_id (),
          target.status ().lifecycle_generation ())
        || !target.has_admitted_peer (
          source.status ().routing_id (),
          source.status ().lifecycle_generation ())) {
        std::cerr << "cutover production setup: peer admission failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    const object_reserve_request_t actor_reserve{
      .key = {placement_object_kind_t::actor, "actor-cutover-probe"},
      .intent = {.stable_type = "actor.cutover.probe"},
      .target = {
        .mesh_name = "actor-cutover-mesh",
        .node_rid = node_rid_t::from_string (
          source.status ().routing_id ().to_string ()),
        .node_lifecycle_generation =
          source.status ().lifecycle_generation (),
        .owner = source_owner},
      .capacity_bundle = {.actor_slots = 1}};
    const auto actor_reserved_value =
      locations->reserve (actor_reserve).result ().value ();
    const auto *actor_reserved =
      std::get_if<object_reserved_t> (&actor_reserved_value);
    if (!actor_reserved) {
        std::cerr << "cutover production setup: authority reserve failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto created_actor = source.create_application_actor (
      "actor.cutover.probe", "actor-cutover-probe", std::nullopt,
      actor_reserved->fence.object_generation,
      actor_reserved->fence.authority_owner_generation, 1s);
    if (source_spot.state != spot_create_state_t::created
        || target_spot.state != spot_create_state_t::created
        || !created_actor) {
        std::cerr << "cutover production setup: application materialization failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto actor = created_actor.value ();
    actor_gateway_runtime_t source_actor_gateway;
    {
        std::lock_guard<std::recursive_mutex> lock (
          source_state->spot_state->mutex);
        source_state->spot_state->actor_instances.insert_or_assign (
          "actor.cutover.probe:actor-cutover-probe",
          std::make_shared<actor_cutover_probe_t> (
            source_actor_gateway.actor_context (actor), false));
    }
    auto source_actor_object = source.native_node ().objects ().find (
      stateful::object_kind_t::actor, "actor-cutover-probe");
    auto source_spot_object = source.native_node ().objects ().find (
      stateful::object_kind_t::user_spot,
      source_native_spot.spot_id ());
    if (!source_actor_object || !source_spot_object) {
        std::cerr << "cutover production setup: Core object lookup failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    const auto [join_error, join] =
      source.native_node ().objects ().begin_membership_move (
        *source_actor_object, *source_spot_object);
    const auto [commit_error, joined_actor] =
      source.native_node ().objects ().commit_membership_move (join);
    if (join_error != stateful::stateful_error_t::none
        || commit_error != stateful::stateful_error_t::none) {
        std::cerr << "cutover production setup: Core membership failed\n";
        source.stop ();
        target.stop ();
        return false;
    }
    source_spots.record_actor_spot (
      actor, spot_id_t (source_native_spot.spot_id ()));

    const auto actor_committed_value = locations->commit (
      {actor_reserve.key, actor_reserved->fence,
       runtime::encode_actor_authority_payload (
         actor, source_native_spot.spot_id (),
         source_native_spot.status ().lifecycle_generation ())})
      .result ().value ();
    if (!std::holds_alternative<object_committed_t> (
          actor_committed_value)) {
        std::cerr << "cutover production setup: authority commit failed\n";
        source.stop ();
        target.stop ();
        return false;
    }

    std::atomic_bool stop_dispatch{false};
    source_spots.set_route_client (route_client_t{});
    target_spots.set_route_client (route_client_t{});
    // OnLeave travels node-level (the source Entry Spot is fixed to node
    // lifecycle and is never published into mesh spot routing), mirroring
    // how production (app.cpp) wires actor_leave_notification_sender to
    // application_mesh->send_to_node.
    target_spots.on_actor_leave_notification (
      [&target] (const zlink::routing_id_t &node_rid,
                std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
          co_return co_await target.send_to_node (node_rid, parts);
      });
    service_collection_t source_services;
    source_services.add_singleton<actor_gateway_runtime_t> ();
    auto source_provider = source_services.build_provider ();
    service_collection_t target_services;
    target_services.add_singleton<actor_gateway_runtime_t> ();
    auto target_provider = target_services.build_provider ();
    const auto dispatch_source = [&] {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            (void) source.dispatch_ready (
              [&] (const host::ready_record_t &owner,
                   const host::receive_record_t &record,
                   std::vector<zlink::message_t> parts) {
                  (void) source_spots.dispatch_mesh_record (
                    owner, record, parts, source_provider, serializers);
              });
            std::this_thread::sleep_for (1ms);
        }
    };
    const auto dispatch_target = [&] {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            (void) target.dispatch_ready (
              [&] (const host::ready_record_t &owner,
                   const host::receive_record_t &record,
                   std::vector<zlink::message_t> parts) {
                  (void) target_spots.dispatch_mesh_record (
                    owner, record, parts, target_provider, serializers);
              });
            std::this_thread::sleep_for (1ms);
        }
    };
    std::thread source_dispatch (dispatch_source);
    std::thread target_dispatch (dispatch_target);
    const runtime::spot_address_t target_address{
      .mesh_name = "actor-cutover-mesh",
      .node_rid = target.status ().routing_id (),
      .spot_id = target_native_spot.spot_id (),
      .spot_generation =
        target_native_spot.status ().lifecycle_generation (),
      .object_generation =
        target_native_spot.status ().lifecycle_generation (),
      .authority_owner_generation = 1,
      .owner = target_owner,
      .node_generation = target.status ().lifecycle_generation ()};
    auto joined = std::async (std::launch::async, [&] {
        return std::move (source.join_application_actor_to_spot (
          actor, target_address, zlink::message_t{}, 5s)).result ();
    });
    const auto source_leave_deadline =
      std::chrono::steady_clock::now () + 2s;
    while (!actor_cutover_probe_t::source_leave_entered.load (
             std::memory_order_acquire)
           && std::chrono::steady_clock::now () < source_leave_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto source_leave_started =
      actor_cutover_probe_t::source_leave_entered.load (
        std::memory_order_acquire);
    const auto returned_while_source_leave_blocked =
      joined.wait_for (250ms) == std::future_status::ready;
    const auto target_completion_deadline =
      std::chrono::steady_clock::now () + 250ms;
    while (!actor_cutover_probe_t::target_completion_entered.load (
             std::memory_order_acquire)
           && std::chrono::steady_clock::now ()
                < target_completion_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto target_completion_started_while_source_leave_blocked =
      actor_cutover_probe_t::target_completion_entered.load (
        std::memory_order_acquire);
    actor_cutover_probe_t::source_leave_gate->complete (
      result_t<void>::success ());
    const auto returned_before_target_completion =
      joined.wait_for (2s) == std::future_status::ready;
    const auto target_completion_after_leave_deadline =
      std::chrono::steady_clock::now () + 2s;
    while (!actor_cutover_probe_t::target_completion_entered.load (
             std::memory_order_acquire)
           && std::chrono::steady_clock::now ()
                < target_completion_after_leave_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    const auto target_completion_started =
      actor_cutover_probe_t::target_completion_entered.load (
        std::memory_order_acquire);
    result_t<actor_join_reply_t> joined_result =
      returned_before_target_completion
        ? joined.get ()
        : result_t<actor_join_reply_t>::failure (
            framework_error_kind_t::deadline_exceeded,
            "cutover source awaited target completion");
    const auto source_completions_before_release =
      actor_cutover_probe_t::source_completions.load (
        std::memory_order_acquire);
    actor_cutover_probe_t::target_completion_gate->complete (
      result_t<void>::success ());
    const auto target_completed =
      actor_cutover_probe_t::target_completions.load (
        std::memory_order_acquire) == 1;
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();
    source.stop ();
    target.stop ();
    if (!returned_before_target_completion)
        (void) joined.get ();

    const auto target_operation_high =
      actor_cutover_probe_t::target_operation_high.load (
        std::memory_order_acquire);
    const auto target_operation_low =
      actor_cutover_probe_t::target_operation_low.load (
        std::memory_order_acquire);
    const auto passed = joined_result && joined_result.value ().result_code == 0
      && source_leave_started && returned_while_source_leave_blocked
      && target_completion_started_while_source_leave_blocked
      && !actor_cutover_probe_t::source_leave_before_target_joined.load (
           std::memory_order_acquire)
      && actor_cutover_probe_t::source_leave_calls.load (
           std::memory_order_acquire) == 1
      && target_completion_started && target_completed
      && source_completions_before_release == 0
      && actor_cutover_probe_t::source_completions.load (
           std::memory_order_acquire) == 0
      && actor_cutover_probe_t::target_completions.load (
           std::memory_order_acquire) == 1
      && actor_cutover_probe_t::failed_completions.load (
           std::memory_order_acquire) == 0
      && target_operation_high != 0 && target_operation_low != 0
      && actor_cutover_probe_t::source_operation_high.load (
           std::memory_order_acquire) == 0
      && actor_cutover_probe_t::source_operation_low.load (
           std::memory_order_acquire) == 0;
    if (!passed) {
        std::cerr << "cutover production diagnostic returned="
                  << returned_before_target_completion
                  << " returned-with-leave-blocked="
                  << returned_while_source_leave_blocked
                  << " leave-started=" << source_leave_started
                  << " leave-before-target-joined="
                  << actor_cutover_probe_t::source_leave_before_target_joined.load ()
                  << " target-started-with-leave-blocked="
                  << target_completion_started_while_source_leave_blocked
                  << " joined=" << static_cast<bool> (joined_result)
                  << " result="
                  << (joined_result ? joined_result.value ().result_code : -1)
                  << " error="
                  << (joined_result.error ()
                        ? joined_result.error ()->what () : "<none>")
                  << " target-started=" << target_completion_started
                  << " target-completed=" << target_completed
                  << " source="
                  << actor_cutover_probe_t::source_completions.load ()
                  << " target="
                  << actor_cutover_probe_t::target_completions.load ()
                  << " failed="
                  << actor_cutover_probe_t::failed_completions.load ()
                  << " op=" << target_operation_high << ':'
                  << target_operation_low << " source-op="
                  << actor_cutover_probe_t::source_operation_high.load ()
                  << ':'
                  << actor_cutover_probe_t::source_operation_low.load ()
                  << " source-spot=" << source_native_spot.spot_id ()
                  << ':' << source_native_spot.status ().lifecycle_generation ()
                  << " target-spot=" << target_native_spot.spot_id ()
                  << ':' << target_native_spot.status ().lifecycle_generation ()
                  << '\n';
    }
    return passed;
}

bool verify_remote_actor_completion_keeps_session_ref_until_route_ack ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = std::make_shared<spot_node_builder_state_t> (
      "remote-source-publication-node");
    spot_node_runtime_t spots (node);
    actor_gateway_runtime_t gateway;
    auto session = gateway.manager ();
    session_actor_manager_access_t::attach (session, stream_t{});
    const auto source = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player",
      "remote-source-actor", 7);
    const auto target = actor_ref_access_t::make (
      node_rid_t::from_string ("target-node"), "player",
      "remote-source-actor", 7);
    if (!session.bind (source).submit ().result ())
        return false;

    int publications = 0;
    spots.on_actor_ref_updated ([&] (const actor_ref_t &actor) {
        ++publications;
        return gateway.update_actor_ref (actor);
    });
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "remote-source-actor", 7,
      zlink::routing_id_t::from ("source-node").to_bytes (), 11, 13, 17};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "remote-source-actor", 7,
      zlink::routing_id_t::from ("target-node").to_bytes (), 12, 14, 18};
    try {
        std::move (spots.complete_remote_actor_transfer (
          source, target,
          spot_route_t{node_rid_t::from_string ("target-node"),
                       spot_id_t ("target-spot"), "game"},
          source_fence, target_fence, "remote-source-transfer"))
          .result ().value ();
    }
    catch (...) {
        return false;
    }
    const auto current = session.find ("remote-source-actor");
    if (publications != 0 || !current
        || current->ref ().node_rid ().value () != "source-node") {
        return false;
    }
    const std::lock_guard<std::recursive_mutex> lock (node->mutex);
    const auto route = node->actor_routes.find ("player:remote-source-actor");
    return route != node->actor_routes.end ()
           && route->second.node_rid.value () == "target-node"
           && route->second.spot_id == "target-spot";
}

} // namespace

int main ()
{
    {
        using queue_t =
          zlink::framework::runtime::application_job_queue_t;
        queue_t queue ({
          zlink::framework::application_job_queue_profile_t::balanced,
          std::uint32_t{1}, 4, 1});

        auto first = queue.try_reserve_supply ();
        if (!first) {
            return 100;
        }
        first->mark_queued ();

        std::mutex grants_mutex;
        std::vector<int> grants;
        std::optional<queue_t::permit_t> second;
        std::optional<queue_t::permit_t> third;
        auto cancelled = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              if (permit) {
                  std::lock_guard lock (grants_mutex);
                  grants.push_back (1);
              }
          });
        auto second_waiter = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              std::lock_guard lock (grants_mutex);
              if (permit) {
                  grants.push_back (2);
                  second.emplace (std::move (*permit));
              }
          });
        auto third_waiter = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              std::lock_guard lock (grants_mutex);
              if (permit) {
                  grants.push_back (3);
                  third.emplace (std::move (*permit));
              }
          });
        if (!cancelled.cancel ()) {
            return 101;
        }

        first->release_for_handler_entry ();
        {
            std::lock_guard lock (grants_mutex);
            if (grants != std::vector<int>{2} || !second) {
                return 102;
            }
        }
        second->mark_queued ();
        second->release_for_handler_entry ();
        second.reset ();
        {
            std::lock_guard lock (grants_mutex);
            if (grants != std::vector<int>({2, 3}) || !third) {
                return 103;
            }
        }
        third->release_without_handler ();
        third.reset ();

        const auto before_reset = queue.snapshot ();
        if (before_reset.reserved_supply_permits != 0
            || before_reset.queued_application_jobs != 0
            || before_reset.permits_in_use != 0
            || before_reset.peak_permits_in_use != 1
            || before_reset.capacity_waiters != 0
            || before_reset.capacity_wait_count != 3) {
            return 104;
        }
        queue.reset_metrics ();
        const auto after_reset = queue.snapshot ();
        if (after_reset.peak_permits_in_use
              != after_reset.permits_in_use
            || after_reset.capacity_wait_count != 0
            || after_reset.capacity_wait_duration
                 != std::chrono::nanoseconds::zero ()) {
            return 105;
        }
        (void) second_waiter;
        (void) third_waiter;
    }

    {
        using namespace zlink::framework;
        using namespace zlink::framework::runtime;

        const application_job_queue_processor_limits_t constrained{
          std::uint32_t{16}, std::uint32_t{8}, std::uint32_t{4},
          std::uint32_t{6}, std::uint32_t{12}};
        if (effective_application_job_processors (constrained) != 4) {
            return 106;
        }

        const std::array profiles{
          application_job_queue_profile_t::compact,
          application_job_queue_profile_t::low_latency,
          application_job_queue_profile_t::balanced,
          application_job_queue_profile_t::throughput};
        const std::array<std::uint32_t, 4> multipliers{32, 64, 128, 256};
        for (const auto processors : {4u, 8u, 16u}) {
            for (std::size_t index = 0; index < profiles.size (); ++index) {
                const auto resolved = resolve_application_job_queue_configuration (
                  profiles[index], std::nullopt,
                  {processors, std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt});
                if (resolved.effective_processor_count != processors
                    || resolved.effective_max_queued_application_jobs
                         != multipliers[index] * processors) {
                    return 107;
                }
            }
        }

        for (const auto manual : {
               std::uint32_t{1},
               static_cast<std::uint32_t> (
                 std::numeric_limits<std::int32_t>::max ())}) {
            const auto resolved = resolve_application_job_queue_configuration (
              application_job_queue_profile_t::balanced, manual, constrained);
            if (resolved.configured_manual_max != manual
                || resolved.effective_max_queued_application_jobs != manual) {
                return 108;
            }
        }

        bool zero_rejected = false;
        try {
            (void) resolve_application_job_queue_configuration (
              application_job_queue_profile_t::balanced, std::uint32_t{0},
              constrained);
        }
        catch (const framework_exception_t &) {
            zero_rejected = true;
        }
        bool overflow_rejected = false;
        try {
            (void) calculate_application_job_queue_limit (
              application_job_queue_profile_t::throughput,
              std::numeric_limits<std::uint32_t>::max ());
        }
        catch (const framework_exception_t &) {
            overflow_rejected = true;
        }
        if (!zero_rejected || !overflow_rejected) {
            return 109;
        }

        const std::array<std::string_view, 10> expected_metric_names{
          "zlink.host.core_hwm.effective_budget",
          "zlink.host.core_hwm.applied",
          "zlink.host.core_hwm.accounted",
          "zlink.host.core_hwm.completion_accounted",
          "zlink.host.core_hwm.blocked_ratio",
          "zlink.host.application_job_queue.limit",
          "zlink.host.application_job_queue.jobs",
          "zlink.host.application_job_queue.capacity_waiters",
          "zlink.host.application_job_queue.capacity_waits",
          "zlink.host.application_job_queue.capacity_wait_duration"};
        for (std::size_t index = 0; index < expected_metric_names.size ();
             ++index) {
            if (host_capacity_metric_catalog[index].name
                  != expected_metric_names[index]) {
                return 110;
            }
        }
        if (host_capacity_metric_catalog[2].unit != "By"
            || !host_capacity_metric_catalog[2].state_label
            || !host_capacity_metric_catalog[3].state_label
            || host_capacity_metric_catalog[4].unit != "{ppm}"
            || host_capacity_metric_catalog[6].unit != "{job}"
            || !host_capacity_metric_catalog[6].state_label
            || host_capacity_metric_catalog[8].kind
                 != detail::metric_instrument_kind_t::counter
            || host_capacity_metric_catalog[8].unit != "{wait}"
            || host_capacity_metric_catalog[9].kind
                 != detail::metric_instrument_kind_t::counter
            || host_capacity_metric_catalog[9].unit != "s") {
            return 111;
        }
    }

    {
        auto state = std::make_shared<
          zlink::framework::detail::spot_node_builder_state_t> (
          "logical-multicast-observation");
        std::atomic_bool observed{false};
        zlink::framework::detail::dispatch_options_access_t::
          set_dispatch_error_observer_for_tests (
            state->dispatch,
            [&] (const zlink::framework::message_dispatch_error_event_t &event) {
              if (event.surface
                    == zlink::framework::dispatch_error_surface_t::route_mesh_channel
                  && event.message_kind
                    == zlink::framework::dispatch_message_kind_t::publish
                  && event.reason
                    == zlink::framework::dispatch_error_reason_t::backpressure
                  && event.action
                    == zlink::framework::dispatch_error_action_t::drop
                  && event.packet_name
                  && *event.packet_name == "PlayerMoved"
                  && event.channel_name
                  && *event.channel_name == "world"
                  && event.topic && *event.topic == "players") {
                  observed.store (true, std::memory_order_release);
              }
          });
        const zlink::framework::framework_exception_t failure (
          zlink::framework::framework_error_kind_t::capacity_exceeded,
          "logical multicast source queue is full");
        zlink::framework::detail::report_logical_multicast_failure (
          state, "world", "players", "PlayerMoved", failure);
        if (!wait_until ([&] {
              return observed.load (std::memory_order_acquire);
            })) {
            return 90;
        }
    }

    zlink::framework::worker_options_t worker_options;
    if (worker_options.min_threads () > worker_options.max_threads ()
        || worker_options.max_threads () == 0
        || worker_options.idle_timeout () < std::chrono::milliseconds::zero ()
        || worker_options.max_queue_length () == 0) {
        return 42;
    }
    worker_options.min_threads (2)
      .max_threads (3)
      .idle_timeout (std::chrono::milliseconds (7))
      .max_queue_length (11);
    if (worker_options.min_threads () != 2
        || worker_options.max_threads () != 3
        || worker_options.idle_timeout () != std::chrono::milliseconds (7)
        || worker_options.max_queue_length () != 11) {
        return 43;
    }
    bool invalid_worker_options_rejected = false;
    try {
        worker_options.max_threads (1);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_worker_options_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!invalid_worker_options_rejected) {
        return 44;
    }

    std::atomic_bool abandoned_deadline_fired = false;
    const auto deadline_owner_start = std::chrono::steady_clock::now ();
    {
        auto control = std::make_shared<zlink::framework::detail::worker_control_t> (
          std::stop_token{});
        control->arm_deadline (
          std::chrono::hours (1),
          [&] { abandoned_deadline_fired.store (true); });
    }
    const auto deadline_owner_elapsed =
      std::chrono::steady_clock::now () - deadline_owner_start;
    if (abandoned_deadline_fired.load ()
        || deadline_owner_elapsed > std::chrono::milliseconds (250)) {
        return 45;
    }

    {
        zlink::framework::runtime::offload_executor_t saturated_executor (
          1, 1, 1, std::chrono::milliseconds (0));
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        bool release = false;
        std::atomic_int queued_runs = 0;
        if (!saturated_executor.try_submit ([&] {
                std::unique_lock lock (state_mutex);
                entered = true;
                state_changed.notify_all ();
                state_changed.wait (lock, [&] { return release; });
            })) {
            return 54;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 55;
            }
        }
        if (!saturated_executor.try_submit ([&] { ++queued_runs; })) {
            return 56;
        }
        if (saturated_executor.try_submit ([] {})) {
            return 57;
        }
        {
            std::lock_guard lock (state_mutex);
            release = true;
        }
        state_changed.notify_all ();
        saturated_executor.drain ();
        if (queued_runs.load () != 1) {
            return 58;
        }
    }

    if (!verify_timer_handler_activation_lifetime ()) {
        return 40;
    }
    if (!verify_close_waits_for_timer_callback_barrier ()) {
        return 41;
    }

    if (!verify_request_turn_mode (false, {1, 3, 2})) {
        return 25;
    }
    if (!verify_request_turn_mode (true, {1, 2, 3})) {
        return 26;
    }
    if (!verify_serial_resume_capacity_failure_is_terminal_and_deferred ()) {
        return 54;
    }
    if (!verify_serial_queue_lanes_and_byte_budget ()) {
        return 50;
    }
    if (!verify_host_reserved_application_bypasses_owner_capacity ()) {
        return 112;
    }
    if (!verify_serial_queue_owner_time_budget ()) {
        return 56;
    }
    if (!verify_cancellable_serial_submission_lifecycle ()) {
        return 92;
    }
    if (!verify_spot_serial_task_async_shutdown_settlement ()) {
        return 93;
    }
    if (!verify_common_dispatch_limits ()) {
        return 55;
    }
    if (!verify_fixture_accounting_boundaries ()) {
        return 60;
    }
    if (!verify_fixture_arbitration_and_owner_isolation ()) {
        return 61;
    }
    if (!verify_serial_lane_policies ()) {
        return 57;
    }
    if (!verify_runtime_observation_loss_and_terminal_retention ()) {
        return 52;
    }
    if (!verify_idle_instance_spot_eviction_closes_local_context ()) {
        return 53;
    }
    if (!verify_explicit_instance_spot_close_releases_authority_after_callback ()) {
        return 91;
    }
    if (!verify_remote_actor_prepare_is_idempotent ()) {
        return 58;
    }
    if (!verify_wire_actor_join_admission_is_approval_only_and_later_attempt_wins ()) {
        std::cerr << "wire actor Join approval-only admission regression failed\n";
        return 117;
    }
    if (!verify_target_commit_stages_source_prefix_before_live_dispatch ()) {
        return 94;
    }
    if (!verify_actor_join_prewarm_parks_arrival_before_prepare ()) {
        std::cerr << "actor Join prewarm parking regression failed\n";
        return 113;
    }
    if (!verify_actor_join_prewarm_newest_attempt_evicts_placeholder ()) {
        std::cerr << "actor Join prewarm newest-attempt eviction regression failed\n";
        return 114;
    }
    if (!verify_actor_join_prewarm_fail_commit_clears_parked_backlog ()) {
        std::cerr << "actor Join prewarm fail_commit backlog cleanup regression failed\n";
        return 115;
    }
    if (!verify_actor_join_prewarm_newest_attempt_evicts_live_attempt_past_prepare ()) {
        std::cerr << "actor Join prewarm live-attempt (past PREPARE) eviction regression failed\n";
        return 116;
    }
    if (!verify_actor_join_finalize_replies_after_target_activation ()) {
        std::cerr << "actor cutover dispatcher regression failed\n";
        return 59;
    }
    if (!verify_remote_actor_cutover_completion_is_target_owned ()) {
        std::cerr << "actor cutover production ownership regression failed\n";
        return 95;
    }
    if (!verify_remote_actor_completion_keeps_session_ref_until_route_ack ()) {
        return 62;
    }

    std::atomic_int unsupported_submit_count = 0;
    zlink::framework::request_call_t<int> unsupported_yield (
      "UnsupportedYield",
      [&unsupported_submit_count] (const auto &, auto, const auto &) {
          unsupported_submit_count.fetch_add (1);
          return zlink::framework::task_t<int> (
            zlink::framework::result_t<int>::success (1));
      });
    const auto unsupported_yield_result = unsupported_yield.yield ().result ();
    if (unsupported_yield_result
        || unsupported_yield_result.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured
        || unsupported_submit_count.load () != 0) {
        return 30;
    }

    zlink::framework::runtime::offload_executor_t executor (2);

    std::vector<int> order;
    std::string failed_item;
    bool error_seen = false;
    zlink::framework::runtime::serial_execution_queue_t queue (
      executor, 4, [&] (const std::string &name, const std::exception_ptr &error) {
          failed_item = name;
          try {
              if (error) {
                  std::rethrow_exception (error);
              }
          }
          catch (const std::runtime_error &ex) {
              error_seen = std::string (ex.what ()) == "boom";
          }
      });

    const bool first_queued = queue.try_post ("first", [&] { order.push_back (1); });
    const bool second_queued = queue.try_post ("second", [&] { order.push_back (2); });
    const bool third_queued = queue.try_post ("third", [&] { order.push_back (3); });
    if (!first_queued || !second_queued || !third_queued) {
        std::cerr << "serial initial admission first=" << first_queued
                  << " second=" << second_queued
                  << " third=" << third_queued << '\n';
        return 1;
    }
    queue.drain ();
    if (order != std::vector<int>{1, 2, 3} || queue.pending_count () != 0) {
        return 2;
    }

    queue.post ("fail", [] { throw std::runtime_error ("boom"); });
    queue.post ("after-fail", [&] { order.push_back (4); });
    queue.drain ();
    if (!error_seen || failed_item != "fail" || order != std::vector<int>{1, 2, 3, 4}) {
        return 3;
    }

    queue.close ();
    if (!queue.closed () || queue.try_post ("closed", [] {}) || queue.pending_count () != 0) {
        return 4;
    }

    bool capacity_error = false;
    try {
        zlink::framework::runtime::serial_execution_queue_t invalid (executor, 0);
    }
    catch (const std::invalid_argument &) {
        capacity_error = true;
    }
    if (!capacity_error) {
        return 5;
    }

    auto context =
      zlink::framework::detail::spot_context_access_t::create ();
    auto async_call = context.run_cpu_worker ([] { return 1; });
    auto async_result = async_call.submit ().result ();
    if (async_result
        || async_result.error_kind () != zlink::framework::framework_error_kind_t::internal_failure) {
        return 6;
    }
    auto duplicate_async = async_call.submit ().result ();
    if (duplicate_async
        || duplicate_async.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 7;
    }

    auto callback_call = context.run_cpu_worker ([] { return 2; });
    const auto unconfigured_result = callback_call.submit ().result ();
    if (unconfigured_result
        || unconfigured_result.error_kind ()
             != zlink::framework::framework_error_kind_t::internal_failure) {
        return 8;
    }
    const auto duplicate_result = callback_call.submit ().result ();
    if (duplicate_result
        || duplicate_result.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 9;
    }

    auto scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto runtime_context = context_with_scheduler (scheduler);
    auto worker_thread = std::thread::id{};
    auto submit_call = runtime_context.run_cpu_worker ([&] {
        worker_thread = std::this_thread::get_id ();
        return 42;
    });
    auto submit_task = submit_call.submit ();
    if (scheduler->worker_job_count () != 1 || scheduler->owner_job_count () != 0) {
        return 10;
    }
    scheduler->run_worker_job ();
    if (scheduler->owner_job_count () != 1) {
        return 11;
    }
    scheduler->run_owner_job ();
    const auto submit_result = submit_task.result ();
    if (worker_thread == std::thread::id{} || !submit_result || submit_result.value () != 42) {
        return 12;
    }

    auto async_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto async_context = context_with_scheduler (async_scheduler);
    auto worker_call = async_context.run_cpu_worker ([] { return 7; });
    auto worker_task = worker_call.submit ();
    async_scheduler->run_worker_job ();
    async_scheduler->run_owner_job ();
    const auto worker_result = worker_task.result ();
    if (!worker_result || worker_result.value () != 7) {
        return 13;
    }

    auto full_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    full_scheduler->queue_full = true;
    auto full_context = context_with_scheduler (full_scheduler);
    auto full_call = full_context.run_cpu_worker ([] { return 3; });
    auto full_task = full_call.submit ();
    if (full_scheduler->worker_job_count () != 0 || full_scheduler->owner_job_count () != 1) {
        return 14;
    }
    full_scheduler->run_owner_job ();
    const auto full_result = full_task.result ();
    if (full_result
        || full_result.error_kind ()
             != zlink::framework::framework_error_kind_t::capacity_exceeded) {
        return 15;
    }

    auto timeout_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto timeout_context = context_with_scheduler (timeout_scheduler);
    std::atomic_bool timeout_saw_cancellation = false;
    auto timeout_call = timeout_context.run_cpu_worker (
      [&] (std::stop_token cancellation) {
          timeout_saw_cancellation.store (cancellation.stop_requested ());
          return 9;
      });
    auto timeout_task = timeout_call.timeout (std::chrono::milliseconds (5)).submit ();
    for (int attempt = 0; attempt < 50 && !timeout_task.await_ready (); ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    if (timeout_scheduler->worker_job_count () != 1 || !timeout_task.await_ready ()) {
        return 18;
    }
    const auto timeout_result = timeout_task.result ();
    if (timeout_result
        || timeout_result.error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded) {
        return 19;
    }
    timeout_scheduler->run_worker_job ();
    if (timeout_scheduler->owner_job_count () != 0
        || !timeout_saw_cancellation.load ()) {
        return 20;
    }

    auto shutdown_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto shutdown_context = context_with_scheduler (shutdown_scheduler);
    std::atomic_bool shutdown_saw_cancellation = false;
    auto shutdown_call = shutdown_context.run_cpu_worker (
      [&] (std::stop_token cancellation) {
          shutdown_saw_cancellation.store (cancellation.stop_requested ());
          return 11;
      });
    auto shutdown_task = shutdown_call.submit ();
    shutdown_scheduler->request_stop ();
    if (!shutdown_task.await_ready ()) {
        return 21;
    }
    const auto shutdown_result = shutdown_task.result ();
    if (shutdown_result
        || shutdown_result.error_kind ()
             != zlink::framework::framework_error_kind_t::shutting_down) {
        return 22;
    }
    shutdown_scheduler->run_worker_job ();
    if (!shutdown_saw_cancellation.load ()
        || shutdown_scheduler->owner_job_count () != 0) {
        return 23;
    }

    auto io_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto io_context = context_with_scheduler (io_scheduler);
    std::vector<std::shared_ptr<zlink::framework::detail::task_completion_source_t<int>>>
      io_sources;
    std::vector<zlink::framework::task_t<int>> io_tasks;
    for (int value = 0; value < 8; ++value) {
        auto source =
          std::make_shared<zlink::framework::detail::task_completion_source_t<int>> ();
        auto call = io_context.run_io_worker ([source] { return source->task (); });
        io_tasks.push_back (call.submit ());
        io_sources.push_back (std::move (source));
    }
    if (io_scheduler->worker_job_count () != 8 || io_scheduler->owner_job_count () != 0) {
        return 27;
    }
    for (int value = 0; value < 8; ++value) {
        io_scheduler->run_worker_job ();
    }
    for (int value = 0; value < 8; ++value) {
        io_sources[static_cast<std::size_t> (value)]->complete (
          zlink::framework::result_t<int>::success (value));
    }
    for (int value = 0; value < 8; ++value) {
        const auto io_result = io_tasks[static_cast<std::size_t> (value)].result ();
        if (!io_result || io_result.value () != value) {
            return 28;
        }
    }

    auto io_thread_source =
      std::make_shared<zlink::framework::detail::task_completion_source_t<int>> ();
    std::thread::id io_thread;
    auto io_thread_call = io_context.run_io_worker (
      [io_thread_source, &io_thread] (std::stop_token) {
          io_thread = std::this_thread::get_id ();
          return io_thread_source->task ();
      });
    auto io_thread_task = io_thread_call.submit ();
    io_scheduler->run_worker_job ();
    if (io_thread == std::thread::id{}) {
        return 24;
    }
    io_thread_source->complete (zlink::framework::result_t<int>::success (99));
    const auto io_thread_result = io_thread_task.result ();
    if (!io_thread_result || io_thread_result.value () != 99) {
        return 25;
    }

    auto io_timeout_source =
      std::make_shared<zlink::framework::detail::task_completion_source_t<int>> ();
    auto io_timeout_call = io_context.run_io_worker (
      [io_timeout_source] { return io_timeout_source->task (); });
    auto io_timeout_task =
      io_timeout_call.timeout (std::chrono::milliseconds (5)).submit ();
    if (io_scheduler->worker_job_count () != 1) {
        return 29;
    }
    io_scheduler->run_worker_job ();
    for (int attempt = 0; attempt < 50 && !io_timeout_task.await_ready (); ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    const auto io_timeout_result = io_timeout_task.result ();
    if (io_timeout_result
        || io_timeout_result.error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded
        || !io_timeout_task.await_ready ()) {
        std::cerr << "io timeout mismatch: success="
                  << static_cast<bool> (io_timeout_result)
                  << " error=" << static_cast<int> (io_timeout_result.error_kind ())
                  << " ready=" << io_timeout_task.await_ready () << '\n';
        return 30;
    }

    {
        zlink::framework::runtime::offload_executor_t deferred_executor (1);
        zlink::framework::runtime::serial_execution_queue_t deferred_queue (
          deferred_executor, 16);
        std::vector<std::string> events;
        deferred_queue.run ("deferred-actor-join", [&] {
            zlink::framework::actor_join_call_t ([&] (std::chrono::milliseconds) {
                events.push_back ("join");
            }).defer ();
            events.push_back ("handler");
        });
        if (events != std::vector<std::string>{"handler", "join"}) {
            return 30;
        }

        bool detached_rejected = false;
        try {
            zlink::framework::actor_join_call_t (
              [] (std::chrono::milliseconds) {}).defer ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            detached_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::not_configured;
        }
        if (!detached_rejected) {
            return 31;
        }

        events.clear ();
        deferred_queue.run ("failed-handler", [&] {
            zlink::framework::actor_join_call_t ([&] (std::chrono::milliseconds) {
                events.push_back ("must-not-run");
            }).defer ();
            throw std::runtime_error ("handler failed");
        });
        if (!events.empty ()) {
            return 32;
        }

        const auto rejected =
          zlink::framework::detail::actor_join_completion_from_erased (
            zlink::framework::detail::actor_join_completion_outcome_t::rejected,
            17, 19, nullptr,
            std::make_optional (zlink::framework::message_t::from (
              std::string ("no"))),
            zlink::framework::framework_error_kind_t::internal_failure);
        const auto *rejected_result =
          std::get_if<zlink::framework::actor_join_rejected_t> (
            &rejected);
        if (rejected_result == nullptr
            || rejected_result->operation_id_high != 17
            || rejected_result->operation_id_low != 19
            || !rejected_result->reply
            || rejected_result->reply->decode<std::string> () != "no") {
            return 33;
        }

        const auto failed =
          zlink::framework::detail::actor_join_completion_from_erased (
            zlink::framework::detail::actor_join_completion_outcome_t::failed,
            23, 29, nullptr, std::nullopt,
            zlink::framework::framework_error_kind_t::internal_failure);
        const auto *failed_result =
          std::get_if<zlink::framework::actor_join_failed_t> (&failed);
        if (failed_result == nullptr
            || failed_result->operation_id_high != 23
            || failed_result->operation_id_low != 29
            || failed_result->error_kind
                 != zlink::framework::framework_error_kind_t::internal_failure) {
            return 34;
        }

        auto completion_state =
          std::make_shared<
            zlink::framework::detail::spot_node_builder_state_t> (
            "completion-node");
        std::vector<zlink::framework::message_flow_event_t>
          completion_failure_events;
        completion_state->dispatch.message_flow (
          zlink::framework::message_flow_log_mode_t::errors);
        zlink::framework::detail::dispatch_options_access_t::
          set_observer_for_tests (
            completion_state->dispatch,
            [&completion_failure_events] (
              const zlink::framework::message_flow_event_t &event) {
                if (event.packet_name == "JoinSpot"
                    && event.error_reason.has_value ()) {
                    completion_failure_events.push_back (event);
                }
            });
        zlink::framework::detail::spot_node_runtime_t completion_runtime (
          completion_state);
        const zlink::framework::actor_ref_t completion_actor =
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("completion-node"),
            "player", "completion-actor", 7);
        const std::string completion_key = "player:completion-actor";
        auto completion_instance = std::make_shared<int> (42);
        completion_state->actor_instances.emplace (
          completion_key, completion_instance);
        completion_state->actor_generations.emplace (
          completion_key, completion_actor.object_generation ());
        completion_state->actor_spot_ids.emplace (
          completion_key,
          zlink::framework::spot_id_t ("source-spot"));

        int completion_callback_count = 0;
        bool fail_completion_once = false;
        auto completion_error_kind =
          zlink::framework::framework_error_kind_t::internal_failure;
        bool completion_retryable = true;
        std::vector<
          zlink::framework::detail::actor_join_completion_outcome_t>
          completion_outcomes;
        zlink::framework::detail::spot_node_builder_state_t::
          actor_factory_registration_t completion_factory;
        completion_factory.actor_type = std::type_index (typeid (int));
        completion_factory.on_join_completed =
          [&] (void *actor,
               zlink::framework::detail::actor_join_completion_outcome_t outcome,
               std::uint64_t,
               std::uint64_t,
               const zlink::framework::actor_ref_t *,
               const std::optional<zlink::framework::message_t> &,
               zlink::framework::framework_error_kind_t error_kind,
               bool retryable) {
              if (actor != completion_instance.get ()) {
                  return zlink::framework::task_t<void> (
                    zlink::framework::result_t<void>::failure (
                      zlink::framework::framework_error_kind_t::not_configured,
                      "completion callback received another Actor"));
              }
              ++completion_callback_count;
              completion_outcomes.push_back (outcome);
              completion_error_kind = error_kind;
              completion_retryable = retryable;
              if (fail_completion_once) {
                  fail_completion_once = false;
                  return zlink::framework::task_t<void> (
                    zlink::framework::result_t<void>::failure (
                      zlink::framework::framework_error_kind_t::internal_failure,
                      "completion callback failed"));
              }
              return zlink::framework::task_t<void> (
                zlink::framework::result_t<void>::success ());
          };
        completion_state->actor_factories.emplace (
          "player", std::move (completion_factory));

        const auto first_operation =
          completion_runtime.actor_join_operation_id ("transfer-1");
        const auto repeated_operation =
          completion_runtime.actor_join_operation_id ("transfer-1");
        const auto second_operation =
          completion_runtime.actor_join_operation_id ("transfer-2");
        if ((first_operation.first == 0 && first_operation.second == 0)
            || first_operation != repeated_operation
            || first_operation == second_operation) {
            return 70;
        }

        const zlink::framework::actor_join_completion_t rejected_completion =
          zlink::framework::actor_join_rejected_t{
            first_operation.first, first_operation.second,
            zlink::framework::message_t::from (
              std::string ("rejected"))};
        if (!completion_runtime.deliver_actor_join_completion (
              completion_actor, rejected_completion,
              zlink::framework::spot_id_t ("source-spot"))
            || !completion_runtime.deliver_actor_join_completion (
              completion_actor, rejected_completion,
              zlink::framework::spot_id_t ("source-spot"))
            || completion_callback_count != 1
            || completion_outcomes
                 != std::vector{
                   zlink::framework::detail::
                     actor_join_completion_outcome_t::rejected}) {
            return 71;
        }

        fail_completion_once = true;
        const zlink::framework::actor_join_completion_t failed_completion =
          zlink::framework::actor_join_failed_t{
            second_operation.first, second_operation.second,
            zlink::framework::framework_error_kind_t::internal_failure};
        if (completion_runtime.deliver_actor_join_completion (
              completion_actor, failed_completion,
              zlink::framework::spot_id_t ("source-spot"))
            || !completion_runtime.deliver_actor_join_completion (
              completion_actor, failed_completion,
              zlink::framework::spot_id_t ("source-spot"))
            || completion_callback_count != 3
            || completion_outcomes.back ()
                 != zlink::framework::detail::
                      actor_join_completion_outcome_t::failed
            || completion_error_kind
                 != zlink::framework::framework_error_kind_t::internal_failure
            || completion_retryable
            || completion_failure_events.size () != 2
            || completion_failure_events.back ().actor_id
                 != "completion-actor"
            || completion_failure_events.back ().error_reason
                 != zlink::framework::dispatch_error_reason_t::handler_exception
            || !completion_failure_events.back ().exception) {
            return 72;
        }
        try {
            std::rethrow_exception (
              completion_failure_events.back ().exception);
            return 73;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            if (error.kind ()
                != zlink::framework::framework_error_kind_t::internal_failure)
                return 73;
        }

        zlink::framework::runtime::offload_executor_t target_executor (1);
        zlink::framework::runtime::serial_execution_queue_t target_queue (
          target_executor, 16);
        std::mutex barrier_events_mutex;
        std::vector<std::string> barrier_events;
        auto record_barrier_event = [&] (std::string event) {
            std::lock_guard lock (barrier_events_mutex);
            barrier_events.push_back (std::move (event));
        };

        deferred_queue.run ("cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next (
              "reserved-join");
            if (!reserved)
                throw std::runtime_error ("target barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred =
              zlink::framework::detail::defer_current_serial_turn (
                [&, barrier] {
                    const auto activated = barrier->activate (
                      [&] { record_barrier_event ("join"); });
                    if (!activated)
                        throw std::runtime_error (
                          "target barrier was not activated");
                },
                [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error ("Join activation was not deferred");
            if (!target_queue.try_post (
                  "queued-actor-turn",
                  [&] { record_barrier_event ("queued"); })) {
                throw std::runtime_error (
                  "target Actor turn was not queued");
            }
            record_barrier_event ("handler");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"handler", "join", "queued"}) {
                return 35;
            }
            barrier_events.clear ();
        }

        deferred_queue.run ("failed-cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next (
              "cancelled-join");
            if (!reserved)
                throw std::runtime_error ("cancelled barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred =
              zlink::framework::detail::defer_current_serial_turn (
                [&, barrier] {
                    const auto activated = barrier->activate ([&] {
                        record_barrier_event ("must-not-run");
                    });
                    if (!activated)
                        throw std::runtime_error (
                          "cancelled barrier unexpectedly failed activation");
                },
                [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error (
                  "cancelled Join activation was not deferred");
            if (!target_queue.try_post (
                  "turn-after-cancelled-join",
                  [&] { record_barrier_event ("after-cancel"); })) {
                throw std::runtime_error (
                  "target Actor turn after cancelled Join was not queued");
            }
            throw std::runtime_error ("handler failed after Join defer");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"after-cancel"}) {
                return 36;
            }
            barrier_events.clear ();
        }

        target_queue.run ("handoff-barrier-source-turn", [&] {
            if (!target_queue.try_post (
                  "accepted-before-handoff",
                  [&] { record_barrier_event ("accepted-before-handoff"); })) {
                throw std::runtime_error (
                  "Actor turn before handoff was not queued");
            }
            auto reserved = target_queue.reserve_handoff_barrier (
              "handoff-after-accepted-turns");
            if (!reserved)
                throw std::runtime_error ("handoff barrier was not reserved");
            const auto activated = reserved.value ()->activate (
              [&] { record_barrier_event ("handoff"); });
            if (!activated)
                throw std::runtime_error ("handoff barrier was not activated");
            record_barrier_event ("source-turn");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"source-turn",
                                            "accepted-before-handoff",
                                            "handoff"}) {
                return 73;
            }
            barrier_events.clear ();
        }

        zlink::framework::detail::actor_gateway_runtime_t actor_gateway;
        zlink::framework::serializer_registry_t actor_serializers;
        actor_gateway.bind_serializers (actor_serializers);
        const zlink::framework::actor_ref_t barrier_actor =
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("barrier-node"),
            "player", "barrier-actor", 1);
        actor_gateway.on_join_spot (
          [&] (const auto &actor, auto, const auto &, auto)
            -> zlink::framework::task_t<
              zlink::framework::detail::actor_join_reply_t> {
              record_barrier_event ("production-join");
              co_return zlink::framework::result_t<
                zlink::framework::detail::actor_join_reply_t>::success (
                  {1, actor, zlink::message_t{}});
          });
        actor_gateway.on_join_barrier (
          [&] (const auto &) {
              return target_queue.reserve_barrier_next (
                "production-join-barrier");
        });
        auto actor_context = actor_gateway.actor_context (barrier_actor);
        try {
            auto serializer_bound_join =
              actor_context.join_spot (
                "serializer-target",
                zlink::framework::message_t::from (
                  std::string ("serializer-probe")));
            (void) serializer_bound_join;
        }
        catch (...) {
            return 37;
        }
        deferred_queue.run ("production-cross-actor-barrier", [&] {
            actor_context.join_spot ("target-spot").defer ();
            if (!target_queue.try_post (
                  "production-queued-turn",
                  [&] { record_barrier_event ("production-queued"); })) {
                throw std::runtime_error (
                  "production target Actor turn was not queued");
            }
            record_barrier_event ("production-handler");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"production-handler",
                                            "production-join",
                                            "production-queued"}) {
                return 37;
            }
            barrier_events.clear ();
        }

        deferred_queue.run ("failed-production-cross-actor-barrier", [&] {
            actor_context.join_spot ("target-spot").defer ();
            if (!target_queue.try_post (
                  "production-turn-after-cancel",
                  [&] { record_barrier_event ("production-after-cancel"); })) {
                throw std::runtime_error (
                  "production post-cancel turn was not queued");
            }
            throw std::runtime_error (
              "production handler failed after Join defer");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"production-after-cancel"}) {
                return 38;
            }
            barrier_events.clear ();
        }

        zlink::framework::runtime::serial_execution_queue_t closing_source_queue (
          deferred_executor, 16);
        closing_source_queue.run ("closing-source-cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next (
              "source-close-cancelled-join");
            if (!reserved)
                throw std::runtime_error (
                  "source-close barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred =
              zlink::framework::detail::defer_current_serial_turn (
                [&, barrier] {
                    const auto activated = barrier->activate ([&] {
                        record_barrier_event ("source-close-must-not-run");
                    });
                    if (!activated)
                        throw std::runtime_error (
                          "source-close barrier activation failed");
                },
                [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error (
                  "source-close Join activation was not deferred");
            if (!target_queue.try_post (
                  "turn-after-source-close",
                  [&] { record_barrier_event ("after-source-close"); })) {
                throw std::runtime_error (
                  "target turn after source close was not queued");
            }
            closing_source_queue.close ();
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"after-source-close"}) {
                return 39;
            }
        }
    }

    {
        zlink::framework::runtime::offload_executor_t bounded_executor (1);
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        if (!bounded_executor.try_submit_cancellable (
              [&] (std::stop_token token) {
                  std::unique_lock lock (state_mutex);
                  entered = true;
                  state_changed.notify_all ();
                  std::stop_callback notify_stop (token, [&] {
                      state_changed.notify_all ();
                  });
                  state_changed.wait (lock, [&] {
                      return token.stop_requested ();
                  });
              })) {
            return 46;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 47;
            }
        }
        if (!bounded_executor.drain_until (
              std::chrono::steady_clock::now () + std::chrono::milliseconds (250))) {
            return 48;
        }
        if (!bounded_executor.drained ()
            || bounded_executor.live_worker_count () != 0) {
            return 49;
        }
    }

    {
        zlink::framework::runtime::offload_executor_t bounded_executor (1);
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        bool release = false;
        if (!bounded_executor.try_submit_cancellable (
              [&] (std::stop_token) {
                  std::unique_lock lock (state_mutex);
                  entered = true;
                  state_changed.notify_all ();
                  state_changed.wait (lock, [&] { return release; });
              })) {
            return 50;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 51;
            }
        }
        if (bounded_executor.drain_until (
              std::chrono::steady_clock::now () + std::chrono::milliseconds (20))) {
            return 52;
        }
        {
            std::lock_guard lock (state_mutex);
            release = true;
        }
        state_changed.notify_all ();
        bounded_executor.drain ();
        if (!bounded_executor.drained ()
            || bounded_executor.live_worker_count () != 0) {
            std::cerr << "bounded executor final drain mismatch: drained="
                      << bounded_executor.drained ()
                      << " live=" << bounded_executor.live_worker_count ()
                      << '\n';
            return 53;
        }
    }

    {
        zlink::framework::runtime::offload_executor_t fixed_min_executor (
          2, 4, 8, std::chrono::milliseconds (5));
        std::mutex state_mutex;
        std::condition_variable state_changed;
        int primed = 0;
        bool release_priming = false;
        std::atomic_int priming_completed{0};
        for (int index = 0; index < 2; ++index) {
            if (!fixed_min_executor.try_submit ([&] {
                    {
                        std::unique_lock lock (state_mutex);
                        ++primed;
                        state_changed.notify_all ();
                        state_changed.wait (
                          lock, [&] { return release_priming; });
                    }
                    priming_completed.fetch_add (
                      1, std::memory_order_release);
                    state_changed.notify_all ();
                })) {
                return 62;
            }
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (
                  lock, std::chrono::seconds (1), [&] { return primed == 2; })) {
                return 63;
            }
            release_priming = true;
        }
        state_changed.notify_all ();
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (
                  lock, std::chrono::seconds (1), [&] {
                      return priming_completed.load (
                               std::memory_order_acquire)
                             == 2;
                  })) {
                return 64;
            }
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        if (fixed_min_executor.live_worker_count () != 2
            || !fixed_min_executor.drained ()) {
            return 65;
        }

        std::atomic_int post_timeout_completed{0};
        for (int index = 0; index < 4; ++index) {
            if (!fixed_min_executor.try_submit ([&] {
                    post_timeout_completed.fetch_add (
                      1, std::memory_order_release);
                })) {
                return 66;
            }
        }
        for (int attempt = 0;
             attempt < 100
             && post_timeout_completed.load (std::memory_order_acquire) != 4;
             ++attempt) {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        if (post_timeout_completed.load (std::memory_order_acquire) != 4) {
            return 67;
        }
        fixed_min_executor.drain ();
        if (!fixed_min_executor.drained ()
            || fixed_min_executor.live_worker_count () != 0) {
            return 68;
        }
    }

    zlink::framework::runtime::offload_executor_t elastic_executor (
      0, 2, 8, std::chrono::milliseconds (5));
    if (elastic_executor.live_worker_count () != 0) {
        return 21;
    }
    std::atomic_bool elastic_work_ran = false;
    if (!elastic_executor.try_submit ([&] { elastic_work_ran.store (true); })) {
        return 22;
    }
    for (int attempt = 0; attempt < 50 && !elastic_work_ran.load (); ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    if (!elastic_work_ran.load ()) {
        return 23;
    }
    for (int attempt = 0; attempt < 50 && elastic_executor.live_worker_count () != 0; ++attempt) {
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    if (elastic_executor.live_worker_count () != 0) {
        return 24;
    }

    return 0;
}

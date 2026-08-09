/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/inbound_dispatch_budget.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/timers/timer_runtime.hpp"

#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{

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

bool verify_inbound_budget_atomic_pending_and_observations ()
{
    zlink::framework::runtime::inbound_dispatch_budget_t budget (100);
    if (!budget.can_start_application_receive ()) {
        return false;
    }

    budget.received (60);
    budget.handler_started (40);
    auto snapshot = budget.snapshot ();
    if (snapshot.pending_payload_bytes != 60
        || snapshot.active_payload_bytes != 40
        || snapshot.queued_payload_bytes != 20
        || snapshot.application_receive_paused) {
        return false;
    }

    budget.received (50);
    snapshot = budget.snapshot ();
    if (snapshot.pending_payload_bytes != 110
        || snapshot.active_payload_bytes != 40
        || !snapshot.application_receive_paused
        || budget.can_start_application_receive ()) {
        return false;
    }

    budget.completed (40, true);
    snapshot = budget.snapshot ();
    if (snapshot.pending_payload_bytes != 70
        || snapshot.active_payload_bytes != 0
        || snapshot.queued_payload_bytes != 70
        || snapshot.application_receive_paused
        || !budget.can_start_application_receive ()) {
        return false;
    }

    budget.completed (70, false);
    snapshot = budget.snapshot ();
    return snapshot.pending_payload_bytes == 0
           && snapshot.queued_payload_bytes == 0
           && snapshot.active_payload_bytes == 0;
}

bool verify_common_dispatch_limits ()
{
    using namespace zlink::framework::runtime;
    const serial_execution_queue_options_t queue_options;
    const receive_batch_budget_t receive_options;
    return queue_options.application_message_capacity
             == dispatch_limits::application_mailbox_messages
           && queue_options.application_byte_capacity
                == dispatch_limits::application_mailbox_bytes
           && queue_options.lifecycle_message_capacity
                == dispatch_limits::control_mailbox_messages
           && queue_options.lifecycle_byte_capacity
                == dispatch_limits::control_mailbox_bytes
           && queue_options.owner_time_budget
                == dispatch_limits::owner_time_budget
           && queue_options.lifecycle_burst_limit
                == dispatch_limits::lifecycle_burst_limit
           && serial_execution_queue_t::fixed_work_byte_cost
                == dispatch_limits::fixed_work_byte_cost
           && receive_options.max_messages
                == dispatch_limits::receive_batch_messages
           && receive_options.max_bytes
                == dispatch_limits::receive_batch_bytes
           && receive_options.max_elapsed
                == dispatch_limits::receive_batch_time;
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
    observer->enqueue (probe_status_t{1});
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                              [&] { return first_entered; })) {
            return false;
        }
    }
    observer->enqueue (probe_status_t{2});
    observer->enqueue (probe_status_t{3});
    observer->enqueue (probe_status_t{4}, true);
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
    terminal_observer->enqueue (probe_status_t{10});
    {
        std::unique_lock lock (terminal_gate);
        if (!terminal_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return terminal_first_entered; })) {
            return false;
        }
    }
    terminal_observer->enqueue (probe_status_t{11}, true);
    terminal_observer->enqueue (probe_status_t{12}, true);
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
    context->kind = detail::spot_runtime_kind_t::instance;
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
    const auto request = zlink::message_t::from (std::string ("prepare"));
    const auto first = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 13, 19);
    const auto repeated = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 13, 19);
    const auto conflicting = owner.admit_remote_actor_to_spot (
      "transfer-1", actor, spot_id_t ("source-spot"),
      target->spot_id, request, 11, 17, 19);

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

class scripted_authority_port_t final
    : public zlink::framework::runtime::stateful::authority_relocation_port_t
{
  public:
    using authority_publish_result_t =
      zlink::framework::runtime::stateful::authority_publish_result_t;
    using authority_publish_status_t =
      zlink::framework::runtime::stateful::authority_publish_status_t;
    using authority_relocation_reference_t =
      zlink::framework::runtime::stateful::authority_relocation_reference_t;
    using object_kind_t = zlink::framework::runtime::stateful::object_kind_t;
    using object_ref_t = zlink::framework::runtime::stateful::object_ref_t;

    authority_publish_result_t publish (
      const object_ref_t &,
      const object_ref_t &,
      zlink::framework::location_owner_token_t,
      zlink::framework::relocation_capacity_fence_t,
      std::string,
      std::uint32_t,
      zlink::framework::runtime::stateful::inventory_digest_t,
      std::vector<std::byte>) override
    {
        return {};
    }

    std::optional<authority_relocation_reference_t>
    read (object_kind_t, const std::string &) override
    {
        std::lock_guard lock (mutex);
        if (throw_on_read) {
            throw std::runtime_error ("authority store is unavailable");
        }
        return current;
    }

    authority_publish_result_t replace_completion (object_kind_t,
                                                   const std::string &,
                                                   std::uint64_t,
                                                   const std::string &old_reference,
                                                   std::uint32_t,
                                                   std::string new_reference,
                                                   std::uint32_t new_checksum) override
    {
        std::lock_guard lock (mutex);
        if (!current || current->relocation_reference != old_reference) {
            return {authority_publish_status_t::failed, current};
        }
        current->relocation_reference = std::move (new_reference);
        current->checksum_crc32c = new_checksum;
        return {authority_publish_status_t::published, current};
    }

    bool release_completion (object_kind_t,
                             const std::string &,
                             std::uint64_t,
                             const std::string &reference,
                             std::uint32_t) override
    {
        std::lock_guard lock (mutex);
        return current && current->relocation_reference == reference;
    }

    std::mutex mutex;
    bool throw_on_read = false;
    std::optional<authority_relocation_reference_t> current;
};

bool verify_deferred_actor_join_completion_converges_from_durable_state ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> (
      "deferred-completion-node");
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    // The join window: the completion-only leg must arrive within it before
    // the target converges from the durable owner state.
    node->channel_runtime->default_request_timeout = std::chrono::milliseconds (200);
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("deferred-completion-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor = std::make_shared<runtime::offload_executor_t> (
      2, 64, "deferred-completion");
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, 64,
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (
      target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [] (void *, std::string_view, const zlink::message_t &,
                         serializer_registry_t &) {
        return spot_actor_join_result_t::accept (
          message_t::from (std::string ("accepted")));
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    std::atomic_int replayed{0};
    target->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::actor_send, "BacklogPacket", "",
      std::type_index (typeid (int)), std::type_index (typeid (int)),
      std::type_index (typeid (int)), std::type_index (typeid (void))});
    target->handler_invokers.push_back (
      [&replayed] (void *, void *, service_provider_t &, serializer_registry_t &,
                   const zlink::message_t &, const spot_inbound_message_t &)
        -> task_t<zlink::message_t> {
          ++replayed;
          co_return zlink::message_t{};
      });

    spot_node_runtime_t owner (node);
    const auto relocation_store =
      std::make_shared<runtime::in_memory_relocation_store_t> ();
    const auto relocation_repository =
      std::make_shared<runtime::provider_relocation_repository_t> (*relocation_store);
    const auto relocation_port =
      std::make_shared<runtime::stateful::public_relocation_store_adapter_t> (
        relocation_repository);
    owner.bind_relocation_store (relocation_port);
    const auto authority = std::make_shared<scripted_authority_port_t> ();
    owner.bind_relocation_authority (authority);

    const auto actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c2", 7);
    const std::string transfer_id = "transfer-c2";
    const std::string key = "player:actor-c2";
    const auto admitted = owner.admit_remote_actor_to_spot (
      transfer_id, actor, spot_id_t ("source-spot"), target->spot_id,
      zlink::message_t::from (std::string ("prepare")), 11, 13, 19);
    if (!admitted || !admitted.value ().accepted) {
        return false;
    }
    const auto prepared = owner.prepare_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, zlink::message_t{},
      actor_gateway_runtime_t{}.actor_context (actor), true);
    if (!prepared) {
        return false;
    }

    // Simulate the durable effects of the finalize leg's authority commit:
    // the completion root moves to the committed cursor and the durable owner
    // row carries it for this target.
    const auto prepared_root = owner.pending_join_completion_root (transfer_id);
    if (!prepared_root) {
        return false;
    }
    runtime::stateful::durable_join_completion_store_t completion_store (relocation_port);
    const auto committed_root = completion_store.commit (*prepared_root, false);
    const auto committed_record = completion_store.recover (committed_root);
    if (!committed_record
        || committed_record->cursor != runtime::stateful::join_completion_cursor_t::committed) {
        return false;
    }
    if (!node->actor_transfer_coordinator.update_completion_root (
          transfer_id, committed_root.reference, committed_root.checksum_crc32c)) {
        return false;
    }
    {
        auto source_ref = committed_record->actor;
        source_ref.authority_owner_generation = 19;
        source_ref.node_id = "source-node";
        std::lock_guard lock (authority->mutex);
        authority->current = runtime::stateful::authority_relocation_reference_t{
          source_ref, committed_record->actor, committed_root.reference,
          committed_root.checksum_crc32c, {}, location_owner_token_t{"owner-1", 3}, {}};
    }

    service_collection_t services;
    auto provider = services.build_provider ();
    std::vector<handoff_packet_t> backlog;
    backlog.push_back (handoff_packet_t{
      "BacklogPacket", {1, 2, 3}, "application/x-test", {}, false});
    // The deferred finalize commits and stages the backlog; the
    // completion-only leg is withheld (the source died after the commit).
    const auto finalized = owner.finalize_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, std::move (backlog), provider, nullptr,
      std::nullopt, true, false);
    if (!finalized || !node->actor_transfer_coordinator.blocks_dispatch (key)) {
        return false;
    }

    // Inside the join window the poll must not open admission.
    if (owner.poll_deferred_actor_join_completions (provider) != 0
        || !node->actor_transfer_coordinator.blocks_dispatch (key)) {
        return false;
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (250));
    // Fail closed: an unreadable authority store keeps admission closed.
    {
        std::lock_guard lock (authority->mutex);
        authority->throw_on_read = true;
    }
    if (owner.poll_deferred_actor_join_completions (provider) != 0
        || !node->actor_transfer_coordinator.blocks_dispatch (key)) {
        return false;
    }
    {
        std::lock_guard lock (authority->mutex);
        authority->throw_on_read = false;
    }
    // The re-poll converges from the durable owner state once it proves the
    // commit: admission opens and the staged backlog drains exactly once.
    std::size_t converged = 0;
    for (int attempt = 0; attempt < 100 && converged == 0; ++attempt) {
        converged = owner.poll_deferred_actor_join_completions (provider);
        if (converged == 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    }
    if (converged != 1 || node->actor_transfer_coordinator.blocks_dispatch (key)
        || replayed.load () != 1) {
        return false;
    }
    if (!owner.completed_remote_actor_commit (transfer_id, actor, target->spot_id)) {
        return false;
    }
    // A late completion-only leg is idempotent: the completed commit stays
    // visible and the duplicate does not double-dispatch or close admission.
    const auto duplicate = owner.finalize_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, {}, provider, nullptr,
      std::nullopt, false, true);
    if (duplicate || node->actor_transfer_coordinator.blocks_dispatch (key)
        || replayed.load () != 1
        || !owner.completed_remote_actor_commit (transfer_id, actor, target->spot_id)) {
        return false;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    if (owner.poll_deferred_actor_join_completions (provider) != 0) {
        return false;
    }

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    return true;
}

} // namespace

int main ()
{
    {
        auto state = std::make_shared<
          zlink::framework::detail::spot_node_builder_state_t> (
          "logical-multicast-observation");
        std::atomic_bool observed{false};
        zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
          state->dispatch,
          [&] (const zlink::framework::message_flow_event_t &event) {
              if (event.outcome
                    == zlink::framework::message_flow_outcome_t::error
                  && event.surface
                    == zlink::framework::dispatch_error_surface_t::route_mesh_channel
                  && event.message_kind
                    == zlink::framework::dispatch_message_kind_t::publish
                  && event.error_reason
                    == zlink::framework::dispatch_error_reason_t::handler_exception
                  && event.error_action
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
    if (!verify_serial_queue_owner_time_budget ()) {
        return 56;
    }
    if (!verify_inbound_budget_atomic_pending_and_observations ()) {
        return 51;
    }
    if (!verify_common_dispatch_limits ()) {
        return 55;
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
    if (!verify_remote_actor_prepare_is_idempotent ()) {
        return 58;
    }
    if (!verify_deferred_actor_join_completion_converges_from_durable_state ()) {
        return 59;
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

    if (!queue.try_post ("first", [&] { order.push_back (1); })
        || !queue.try_post ("second", [&] { order.push_back (2); })
        || !queue.try_post ("third", [&] { order.push_back (3); })) {
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
            || completion_retryable) {
            return 72;
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
          [&] (const auto &actor, auto, const auto &, auto) {
              record_barrier_event ("production-join");
              return zlink::framework::result_t<
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

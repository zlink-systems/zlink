/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/timers/async_delay.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_delay_owner_final_join_closes_accepted_registration (test_context_t &test)
{
    namespace framework = zlink::framework;
    std::mutex owner_mutex;
    framework::detail::delay_owner_t delays (owner_mutex);
    const auto duration = framework::runtime::dispatch_limits::management_retry_interval;
    delays.request_stop (std::chrono::steady_clock::time_point::max ());
    auto pending = delays.start (duration, true);
    delays.stop_and_join ();
    const auto completed = pending.result ();
    const auto rejected = delays.start (duration, true).result ();
    test.require (
      (completed || completed.error_kind () == framework::framework_error_kind_t::shutting_down)
        && !rejected && rejected.error_kind () == framework::framework_error_kind_t::shutting_down
        && delays.worker_count () == 0,
      "final teardown must complete existing accepted waits and reject accepted "
      "registration after its final fence");
}

void test_source_completion_wait_scope (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    std::promise<bool> terminal;
    auto delivered = terminal.get_future ();
    detail::actor_transfer_coordinator_t coordinator;
    const std::string key = "player:source-completion";
    test.require (coordinator.try_begin_source_remote (key, "source-completion").has_value (),
                  "source completion fixture must begin its source move");
    auto waiting = coordinator.wait_for_source_completion (key);
    const bool suspended = !waiting.await_ready ();
    detail::observe_task_terminal (waiting, [&] (const auto &) {
        terminal.set_value (zlink::framework::runtime::state_lane_t::current () == nullptr);
    });
    coordinator.cancel_move (key);
    const bool outside_lane = delivered.get ();
    waiting.result ().value ();
    test.require (suspended && outside_lane,
                  "source cancellation must release its async waiter outside the state lane");

    coordinator.mark_reconcile (key, std::chrono::seconds (30));
    auto target_recovery = coordinator.wait_for_source_completion (key);
    const bool no_source_wait = target_recovery.await_ready ();
    coordinator.cancel_move (key);
    target_recovery.result ().value ();
    test.require (no_source_wait,
                  "target reconciliation must not wait for its own materialization to finish");
}

void test_pending_actor_turn_survives_queue_removal (test_context_t &test, bool cancel_pending)
{
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (1);
    std::atomic_int retired{0};
    std::promise<void> retirement;
    auto retirement_ready = retirement.get_future ();
    auto *raw = new runtime::serial_execution_queue_t (executor);
    // Observe premature retirement without blocking the state lane in drain.
    // The test deletes the queue after releasing and joining its accepted work.
    auto queue = std::shared_ptr<runtime::serial_execution_queue_t> (raw, [&] (auto *) {
        retired.fetch_add (1);
        retirement.set_value ();
    });
    std::weak_ptr<runtime::serial_execution_queue_t> weak = queue;
    std::promise<runtime::serial_execution_queue_t::async_completion_t> started;
    auto ready = started.get_future ();
    queue->post_async ("actor-join-awaiting-source-lane",
                       [&] (auto complete) { started.set_value (std::move (complete)); });
    auto finish = ready.get ();
    std::atomic_int cancelled_work_ran{0};
    if (cancel_pending) {
        queue->post_async ("cancelled-actor-work", [&] (auto complete) {
            ++cancelled_work_ran;
            complete ([] {});
        });
        queue->cancel_pending ();
    }
    queue.reset ();
    const bool retained = !weak.expired ();
    finish ([] {});
    std::promise<void> completed;
    auto completion_ready = completed.get_future ();
    executor.submit ([&] { completed.set_value (); });
    completion_ready.get ();
    const bool released_on_completion = weak.expired ();
    finish = {};
    retirement_ready.get ();
    executor.drain ();
    const auto retirements = retired.load ();
    delete raw;
    test.require (retained && released_on_completion && retirements == 1 && cancelled_work_ran == 0,
                  "an accepted Actor turn must retain its queue until completion after removal");
}

} // namespace m6c_test

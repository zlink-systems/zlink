/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/execution_signal.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
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

    std::size_t worker_job_count () const
    {
        std::lock_guard lock (mutex);
        return worker_jobs.size ();
    }

    std::stop_token stop_token () const noexcept override { return cancellation.get_token (); }

    void request_stop () noexcept { cancellation.request_stop (); }

    bool queue_full = false;
    mutable std::mutex mutex;
    std::queue<std::function<void (std::stop_token)>> worker_jobs;
    std::stop_source cancellation;
};
zlink::framework::spot_context_t
context_with_scheduler (const std::shared_ptr<controlled_worker_scheduler_t> &scheduler)
{
    auto state = std::make_shared<zlink::framework::detail::spot_context_state_t> ();
    state->worker_scheduler = scheduler;
    return zlink::framework::detail::spot_context_access_t::create (state);
}
bool verify_cpu_worker_completes_in_ordinary_turn ()
{
    using namespace zlink::framework;
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor, {}, {},
                                             runtime::serial_lane_policy_t::spot_wide ());
    auto scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto context = context_with_scheduler (scheduler);
    serial_test_signal_t submitted;
    std::atomic_bool follower_ran = false;
    std::atomic_bool succeeded = false;
    auto handler = [&] () -> task_t<void> {
        const auto turn = detail::capture_current_serial_turn ();
        auto call = context.run_cpu_worker ([] { return 42; });
        auto pending = call.timeout (std::chrono::seconds (1)).async ();
        submitted.set ();
        const auto result = co_await pending;
        succeeded = result == 42 && !follower_ran.load ()
                    && detail::capture_current_serial_turn () == turn && !turn->released ();
        co_return;
    };
    // Runs the worker jobs the suspended handler waits for, then drains, before the locals
    // above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        auto drained = std::async (std::launch::async, [&] { queue.drain (); });
        while (drained.wait_for (std::chrono::milliseconds (2)) != std::future_status::ready) {
            while (scheduler->worker_job_count () != 0)
                scheduler->run_worker_job ();
        }
    });
    if (!queue.try_post_async ("ordinary-worker-handler",
                               [&] (auto complete) {
                                   auto task = handler ();
                                   detail::observe_task_completion (
                                     task, [complete] (const result_t<void> &) mutable {
                                         complete ([] {});
                                     });
                               })
        || !submitted.wait_for ()) {
        queue.cancel_pending ();
        return false;
    }
    if (!queue.try_post ("ordinary-worker-follower", [&] { follower_ran = true; })) {
        queue.cancel_pending ();
        return false;
    }
    scheduler->run_worker_job ();
    queue.drain ();
    return succeeded.load () && follower_ran.load ();
}

int verify_workers_and_spot_gate ()
{
    std::atomic_int unsupported_submit_count = 0;
    zlink::framework::request_call_t<int> unsupported_yield (
      "UnsupportedYield", [&unsupported_submit_count] (const auto &, auto, const auto &) {
          unsupported_submit_count.fetch_add (1);
          return zlink::framework::request_task_t<int> (
            zlink::framework::result_t<int>::success (1));
      });
    const auto unsupported_yield_result = unsupported_yield.yield ().result ();
    if (unsupported_yield_result
        || unsupported_yield_result.error_kind ()
             != zlink::framework::framework_error_kind_t::invalid_operation
        || unsupported_submit_count.load () != 0) {
        return 30;
    }

    zlink::framework::runtime::offload_executor_t executor (2);

    std::vector<int> order;
    std::string failed_item;
    bool error_seen = false;
    zlink::framework::runtime::serial_execution_queue_t queue (
      executor, {}, [&] (const std::string &name, const std::exception_ptr &error) {
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
                  << " second=" << second_queued << " third=" << third_queued << '\n';
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
        zlink::framework::runtime::serial_execution_queue_t invalid (
          executor,
          zlink::framework::runtime::serial_execution_queue_options_t{.lifecycle_burst_limit = 0});
    }
    catch (const std::invalid_argument &) {
        capacity_error = true;
    }
    if (!capacity_error) {
        return 5;
    }

    auto context = zlink::framework::detail::spot_context_access_t::create ();
    auto async_call = context.run_cpu_worker ([] { return 1; });
    auto async_result = async_call.async ().result ();
    if (async_result
        || async_result.error_kind ()
             != zlink::framework::framework_error_kind_t::internal_failure) {
        return 6;
    }
    auto duplicate_async = async_call.async ().result ();
    if (duplicate_async
        || duplicate_async.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 7;
    }

    auto callback_call = context.run_cpu_worker ([] { return 2; });
    const auto unconfigured_result = callback_call.async ().result ();
    if (unconfigured_result
        || unconfigured_result.error_kind ()
             != zlink::framework::framework_error_kind_t::internal_failure) {
        return 8;
    }
    const auto duplicate_result = callback_call.async ().result ();
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
    auto submit_task = submit_call.async ();
    if (scheduler->worker_job_count () != 1 || submit_task.await_ready ()) {
        return 10;
    }
    scheduler->run_worker_job ();
    const auto submit_result = submit_task.result ();
    if (!submit_task.await_ready ()) {
        return 11;
    }
    if (worker_thread == std::thread::id{} || !submit_result || submit_result.value () != 42) {
        return 12;
    }

    auto async_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto async_context = context_with_scheduler (async_scheduler);
    auto worker_call = async_context.run_cpu_worker ([] { return 7; });
    auto worker_task = worker_call.async ();
    async_scheduler->run_worker_job ();
    const auto worker_result = worker_task.result ();
    if (!worker_result || worker_result.value () != 7) {
        return 13;
    }

    auto full_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    full_scheduler->queue_full = true;
    auto full_context = context_with_scheduler (full_scheduler);
    auto full_call = full_context.run_cpu_worker ([] { return 3; });
    auto full_task = full_call.async ();
    if (full_scheduler->worker_job_count () != 0 || !full_task.await_ready ()) {
        return 14;
    }
    const auto full_result = full_task.result ();
    if (full_result
        || full_result.error_kind () != zlink::framework::framework_error_kind_t::shutting_down) {
        return 15;
    }
    auto full_io_call = full_context.run_io_worker (
      [] { return zlink::framework::task_t<int> (zlink::framework::result_t<int>::success (3)); });
    auto full_io_task = full_io_call.async ();
    if (full_scheduler->worker_job_count () != 0 || !full_io_task.await_ready ()) {
        return 16;
    }
    const auto full_io_result = full_io_task.result ();
    if (full_io_result
        || full_io_result.error_kind ()
             != zlink::framework::framework_error_kind_t::shutting_down) {
        return 17;
    }

    auto timeout_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto timeout_context = context_with_scheduler (timeout_scheduler);
    std::atomic_bool timeout_saw_cancellation = false;
    auto timeout_call = timeout_context.run_cpu_worker ([&] (std::stop_token cancellation) {
        timeout_saw_cancellation.store (cancellation.stop_requested ());
        return 9;
    });
    auto timeout_task = timeout_call.timeout (std::chrono::milliseconds (5)).async ();
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
    if (!timeout_saw_cancellation.load ()
        || timeout_task.result ().error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded) {
        return 20;
    }

    auto shutdown_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto shutdown_context = context_with_scheduler (shutdown_scheduler);
    std::atomic_bool shutdown_saw_cancellation = false;
    auto shutdown_call = shutdown_context.run_cpu_worker ([&] (std::stop_token cancellation) {
        shutdown_saw_cancellation.store (cancellation.stop_requested ());
        return 11;
    });
    auto shutdown_task = shutdown_call.async ();
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
        || shutdown_task.result ().error_kind ()
             != zlink::framework::framework_error_kind_t::shutting_down) {
        return 23;
    }

    auto io_scheduler = std::make_shared<controlled_worker_scheduler_t> ();
    auto io_context = context_with_scheduler (io_scheduler);
    std::vector<std::shared_ptr<zlink::framework::task_completion_source_t<int>>> io_sources;
    std::vector<zlink::framework::task_t<int>> io_tasks;
    for (int value = 0; value < 8; ++value) {
        auto source = std::make_shared<zlink::framework::task_completion_source_t<int>> ();
        auto call = io_context.run_io_worker ([source] { return source->task (); });
        io_tasks.push_back (call.async ());
        io_sources.push_back (std::move (source));
    }
    if (io_scheduler->worker_job_count () != 8) {
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

    auto io_thread_source = std::make_shared<zlink::framework::task_completion_source_t<int>> ();
    std::thread::id io_thread;
    auto io_thread_call =
      io_context.run_io_worker ([io_thread_source, &io_thread] (std::stop_token) {
          io_thread = std::this_thread::get_id ();
          return io_thread_source->task ();
      });
    auto io_thread_task = io_thread_call.async ();
    io_scheduler->run_worker_job ();
    if (io_thread == std::thread::id{}) {
        return 24;
    }
    io_thread_source->complete (zlink::framework::result_t<int>::success (99));
    const auto io_thread_result = io_thread_task.result ();
    if (!io_thread_result || io_thread_result.value () != 99) {
        return 25;
    }

    auto io_timeout_source = std::make_shared<zlink::framework::task_completion_source_t<int>> ();
    auto io_timeout_call =
      io_context.run_io_worker ([io_timeout_source] { return io_timeout_source->task (); });
    auto io_timeout_task = io_timeout_call.timeout (std::chrono::milliseconds (5)).async ();
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
        std::cerr << "io timeout mismatch: success=" << static_cast<bool> (io_timeout_result)
                  << " error=" << static_cast<int> (io_timeout_result.error_kind ())
                  << " ready=" << io_timeout_task.await_ready () << '\n';
        return 30;
    }

    {
        zlink::framework::runtime::offload_executor_t deferred_executor (1);
        zlink::framework::runtime::serial_execution_queue_t deferred_queue (deferred_executor);
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
            zlink::framework::actor_join_call_t ([] (std::chrono::milliseconds) {}).defer ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            detached_rejected =
              error.kind () == zlink::framework::framework_error_kind_t::not_configured;
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

        const auto rejected = zlink::framework::detail::actor_join_completion_from_erased (
          zlink::framework::detail::actor_join_completion_outcome_t::rejected, 17, 19, nullptr,
          std::make_optional (zlink::framework::message_t::from (std::string ("no"))),
          zlink::framework::framework_error_kind_t::internal_failure);
        const auto *rejected_result =
          std::get_if<zlink::framework::actor_join_rejected_t> (&rejected);
        if (rejected_result == nullptr || rejected_result->operation_id_high != 17
            || rejected_result->operation_id_low != 19 || !rejected_result->reply
            || rejected_result->reply->decode<std::string> () != "no") {
            return 33;
        }

        const auto failed = zlink::framework::detail::actor_join_completion_from_erased (
          zlink::framework::detail::actor_join_completion_outcome_t::failed, 23, 29, nullptr,
          std::nullopt, zlink::framework::framework_error_kind_t::internal_failure);
        const auto *failed_result = std::get_if<zlink::framework::actor_join_failed_t> (&failed);
        if (failed_result == nullptr || failed_result->operation_id_high != 23
            || failed_result->operation_id_low != 29
            || failed_result->error_kind
                 != zlink::framework::framework_error_kind_t::internal_failure) {
            return 34;
        }

        if (const auto result = verify_actor_join_completion_observations (); result != 0)
            return result;

        zlink::framework::runtime::offload_executor_t target_executor (1);
        zlink::framework::runtime::serial_execution_queue_t target_queue (target_executor);
        std::mutex barrier_events_mutex;
        std::vector<std::string> barrier_events;
        auto record_barrier_event = [&] (std::string event) {
            std::lock_guard lock (barrier_events_mutex);
            barrier_events.push_back (std::move (event));
        };

        // These Join barriers hold the target Actor FIFO while they wait, as the
        // production Actor handoff barrier does (message ordering behind Join).
        deferred_queue.run ("cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next ("reserved-join", true);
            if (!reserved)
                throw std::runtime_error ("target barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred = zlink::framework::detail::defer_current_serial_turn (
              [&, barrier] {
                  const auto activated = barrier->activate ([&] { record_barrier_event ("join"); });
                  if (!activated)
                      throw std::runtime_error ("target barrier was not activated");
                  return barrier->terminal ();
              },
              [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error ("Join activation was not deferred");
            if (!target_queue.try_post ("queued-actor-turn",
                                        [&] { record_barrier_event ("queued"); })) {
                throw std::runtime_error ("target Actor turn was not queued");
            }
            record_barrier_event ("handler");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events != std::vector<std::string>{"handler", "join", "queued"}) {
                return 35;
            }
            barrier_events.clear ();
        }

        deferred_queue.run ("failed-cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next ("cancelled-join", true);
            if (!reserved)
                throw std::runtime_error ("cancelled barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred = zlink::framework::detail::defer_current_serial_turn (
              [&, barrier] {
                  const auto activated =
                    barrier->activate ([&] { record_barrier_event ("must-not-run"); });
                  if (!activated)
                      throw std::runtime_error ("cancelled barrier unexpectedly failed activation");
                  return barrier->terminal ();
              },
              [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error ("cancelled Join activation was not deferred");
            if (!target_queue.try_post ("turn-after-cancelled-join",
                                        [&] { record_barrier_event ("after-cancel"); })) {
                throw std::runtime_error ("target Actor turn after cancelled Join was not queued");
            }
            throw std::runtime_error ("handler failed after Join defer");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events != std::vector<std::string>{"after-cancel"}) {
                return 36;
            }
            barrier_events.clear ();
        }

        target_queue.run ("handoff-barrier-source-turn", [&] {
            // The predecessor and barrier share the lifecycle FIFO (§7).
            if (!target_queue.try_post (
                  "accepted-before-handoff",
                  [&] { record_barrier_event ("accepted-before-handoff"); },
                  zlink::framework::runtime::serial_work_options_t{
                    zlink::framework::runtime::serial_work_lane_t::lifecycle})) {
                throw std::runtime_error ("Actor turn before handoff was not queued");
            }
            auto reserved =
              target_queue.reserve_barrier_next ("handoff-after-accepted-turns", true);
            if (!reserved)
                throw std::runtime_error ("handoff barrier was not reserved");
            const auto activated =
              reserved.value ()->activate ([&] { record_barrier_event ("handoff"); });
            if (!activated)
                throw std::runtime_error ("handoff barrier was not activated");
            record_barrier_event ("source-turn");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"source-turn", "accepted-before-handoff", "handoff"}) {
                return 73;
            }
            barrier_events.clear ();
        }

        {
            zlink::framework::runtime::offload_executor_t handoff_executor (1);
            zlink::framework::runtime::serial_execution_queue_t handoff_queue (handoff_executor);
            auto reserved = handoff_queue.reserve_barrier_next ("handoff-after-queue-head", true);
            if (!reserved)
                return 74;
            std::promise<void> queue_head;
            auto queue_head_done = queue_head.get_future ();
            if (!handoff_executor.try_submit_internal ([&] { queue_head.set_value (); }))
                return 74;
            queue_head_done.get ();

            bool join_started = false;
            const auto activated = reserved.value ()->activate_async ([&] (auto complete) {
                join_started = true;
                complete (zlink::framework::result_t<void>::success ());
            });
            if (!activated)
                return 74;
            handoff_queue.drain ();
            if (!join_started)
                return 74;
        }

        zlink::framework::detail::actor_gateway_runtime_t actor_gateway;
        zlink::framework::serializer_registry_t actor_serializers;
        actor_gateway.bind_serializers (actor_serializers);
        const zlink::framework::actor_ref_t barrier_actor =
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("barrier-node"), "player", "barrier-actor",
            1);
        actor_gateway.on_join_spot (
          [&] (const auto &actor, auto, const auto &, auto, auto,
               auto) -> zlink::framework::task_t<zlink::framework::detail::actor_join_reply_t> {
              record_barrier_event ("production-join");
              co_return zlink::framework::result_t<
                zlink::framework::detail::actor_join_reply_t>::success ({1, actor,
                                                                         zlink::message_t{}});
          });
        actor_gateway.on_join_barrier ([&] (const auto &) {
            return target_queue.reserve_barrier_next ("production-join-barrier", true);
        });
        auto actor_context = actor_gateway.actor_context (barrier_actor);
        try {
            auto serializer_bound_join = actor_context.join_spot (
              "serializer-target",
              zlink::framework::message_t::from (std::string ("serializer-probe")));
            (void) serializer_bound_join;
        }
        catch (...) {
            return 37;
        }
        deferred_queue.run ("production-cross-actor-barrier", [&] {
            actor_context.join_spot ("target-spot").defer ();
            if (!target_queue.try_post ("production-queued-turn",
                                        [&] { record_barrier_event ("production-queued"); })) {
                throw std::runtime_error ("production target Actor turn was not queued");
            }
            record_barrier_event ("production-handler");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events
                != std::vector<std::string>{"production-handler", "production-join",
                                            "production-queued"}) {
                return 37;
            }
            barrier_events.clear ();
        }

        if (!verify_bulk_deferred_actor_joins ())
            return 74;

        deferred_queue.run ("failed-production-cross-actor-barrier", [&] {
            actor_context.join_spot ("target-spot").defer ();
            if (!target_queue.try_post ("production-turn-after-cancel", [&] {
                    record_barrier_event ("production-after-cancel");
                })) {
                throw std::runtime_error ("production post-cancel turn was not queued");
            }
            throw std::runtime_error ("production handler failed after Join defer");
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events != std::vector<std::string>{"production-after-cancel"}) {
                return 38;
            }
            barrier_events.clear ();
        }

        zlink::framework::runtime::serial_execution_queue_t closing_source_queue (
          deferred_executor);
        closing_source_queue.run ("closing-source-cross-actor-barrier", [&] {
            auto reserved = target_queue.reserve_barrier_next ("source-close-cancelled-join");
            if (!reserved)
                throw std::runtime_error ("source-close barrier was not reserved");
            const auto barrier = reserved.value ();
            const auto deferred = zlink::framework::detail::defer_current_serial_turn (
              [&, barrier] {
                  const auto activated =
                    barrier->activate ([&] { record_barrier_event ("source-close-must-not-run"); });
                  if (!activated)
                      throw std::runtime_error ("source-close barrier activation failed");
                  return barrier->terminal ();
              },
              [barrier] { barrier->cancel (); });
            if (!deferred)
                throw std::runtime_error ("source-close Join activation was not deferred");
            if (!target_queue.try_post ("turn-after-source-close",
                                        [&] { record_barrier_event ("after-source-close"); })) {
                throw std::runtime_error ("target turn after source close was not queued");
            }
            closing_source_queue.close ();
        });
        target_queue.drain ();
        {
            std::lock_guard lock (barrier_events_mutex);
            if (barrier_events != std::vector<std::string>{"after-source-close"}) {
                return 39;
            }
        }
    }

    {
        // Declared before the executor so the cancellable job never outlives them.
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        zlink::framework::runtime::offload_executor_t bounded_executor (1);
        if (!bounded_executor.try_submit_cancellable ([&] (std::stop_token token) {
                std::unique_lock lock (state_mutex);
                entered = true;
                state_changed.notify_all ();
                std::stop_callback notify_stop (token, [&] { state_changed.notify_all (); });
                state_changed.wait (lock, [&] { return token.stop_requested (); });
            })) {
            return 46;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 47;
            }
        }
        if (!bounded_executor.drain_until (std::chrono::steady_clock::now ()
                                           + std::chrono::milliseconds (250))) {
            return 48;
        }
        if (!bounded_executor.drained () || bounded_executor.live_worker_count () != 0) {
            return 49;
        }
    }

    {
        zlink::framework::runtime::offload_executor_t bounded_executor (1);
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        bool release = false;
        // Releases the blocking job and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            {
                std::lock_guard lock (state_mutex);
                release = true;
            }
            state_changed.notify_all ();
            bounded_executor.drain ();
        });
        if (!bounded_executor.try_submit_cancellable ([&] (std::stop_token) {
                std::unique_lock lock (state_mutex);
                entered = true;
                state_changed.notify_all ();
                state_changed.wait (lock, [&] { return release; });
            })) {
            return 50;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 51;
            }
        }
        if (bounded_executor.drain_until (std::chrono::steady_clock::now ()
                                          + std::chrono::milliseconds (20))) {
            return 52;
        }
        {
            std::lock_guard lock (state_mutex);
            release = true;
        }
        state_changed.notify_all ();
        bounded_executor.drain ();
        if (!bounded_executor.drained () || bounded_executor.live_worker_count () != 0) {
            std::cerr << "bounded executor final drain mismatch: drained="
                      << bounded_executor.drained ()
                      << " live=" << bounded_executor.live_worker_count () << '\n';
            return 53;
        }
    }

    {
        zlink::framework::runtime::offload_executor_t fixed_min_executor (
          2, 4, std::chrono::milliseconds (5));
        std::mutex state_mutex;
        std::condition_variable state_changed;
        int primed = 0;
        bool release_priming = false;
        std::atomic_int priming_completed{0};
        std::atomic_int post_timeout_completed{0};
        // Releases the priming jobs and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            {
                std::lock_guard lock (state_mutex);
                release_priming = true;
            }
            state_changed.notify_all ();
            fixed_min_executor.drain ();
        });
        for (int index = 0; index < 2; ++index) {
            if (!fixed_min_executor.try_submit ([&] {
                    {
                        std::unique_lock lock (state_mutex);
                        ++primed;
                        state_changed.notify_all ();
                        state_changed.wait (lock, [&] { return release_priming; });
                    }
                    priming_completed.fetch_add (1, std::memory_order_release);
                    state_changed.notify_all ();
                })) {
                return 62;
            }
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (lock, std::chrono::seconds (1),
                                         [&] { return primed == 2; })) {
                return 63;
            }
            release_priming = true;
        }
        state_changed.notify_all ();
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (lock, std::chrono::seconds (1), [&] {
                    return priming_completed.load (std::memory_order_acquire) == 2;
                })) {
                return 64;
            }
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        if (fixed_min_executor.live_worker_count () != 2 || !fixed_min_executor.drained ()) {
            return 65;
        }

        for (int index = 0; index < 4; ++index) {
            if (!fixed_min_executor.try_submit (
                  [&] { post_timeout_completed.fetch_add (1, std::memory_order_release); })) {
                return 66;
            }
        }
        for (int attempt = 0;
             attempt < 100 && post_timeout_completed.load (std::memory_order_acquire) != 4;
             ++attempt) {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        if (post_timeout_completed.load (std::memory_order_acquire) != 4) {
            return 67;
        }
        fixed_min_executor.drain ();
        if (!fixed_min_executor.drained () || fixed_min_executor.live_worker_count () != 0) {
            return 68;
        }
    }

    // Declared before the executor so its job never outlives it.
    std::atomic_bool elastic_work_ran = false;
    zlink::framework::runtime::offload_executor_t elastic_executor (0, 2,
                                                                    std::chrono::milliseconds (5));
    if (elastic_executor.live_worker_count () != 0) {
        return 21;
    }
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

    {
        // A worker that returns on idle timeout is joined by its owner before
        // the next worker starts, so returned threads do not accumulate.
        constexpr int restart_rounds = 8;
        // Declared before the executor so its job never outlives it.
        std::atomic_bool ran = false;
        zlink::framework::runtime::offload_executor_t restarting_executor (
          0, 1, std::chrono::milliseconds (1));
        for (int round = 0; round < restart_rounds; ++round) {
            ran = false;
            if (!restarting_executor.try_submit ([&] { ran.store (true); })) {
                return 76;
            }
            for (int attempt = 0;
                 attempt < 2500 && (!ran.load () || restarting_executor.live_worker_count () != 0);
                 ++attempt) {
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
            if (!ran.load () || restarting_executor.live_worker_count () != 0) {
                return 77;
            }
        }
        if (restarting_executor.owned_thread_count () > 1) {
            std::cerr << "offload executor kept returned workers: "
                      << restarting_executor.owned_thread_count () << '\n';
            return 78;
        }
    }

    if (!verify_spot_wide_consumer_waits_for_shared_gate ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_cancel_removes_unclaimed_actor_head ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_stop_settles_queued_actor_rejection ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_rejects_actor_when_gate_scheduler_stops ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_actor_ready_lane_tracks_lifecycle ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_handoff_allows_spot_control ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_terminal_retains_gate_lifetime ()) {
        return EXIT_FAILURE;
    }
    if (!verify_spot_wide_control_returns_without_waiting_for_gate ()) {
        return EXIT_FAILURE;
    }
    return 0;
}
}

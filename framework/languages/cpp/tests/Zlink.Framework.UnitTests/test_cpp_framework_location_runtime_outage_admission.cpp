/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>
#include "../support/location_runtime_outage_fixture.hpp"

namespace zlink::framework::runtime
{
struct serial_execution_queue_test_access_t
{
    static void select_then (serial_execution_queue_t &queue, std::function<void ()> close)
    {
        auto selected = [&queue, close = std::move (close)] {
            serial_execution_queue_t::work_item_t item;
            {
                std::lock_guard lock (queue._mutex);
                item = queue.take_next_locked ();
                ++queue._active;
                queue._active_lane = item.lane;
                queue._active_bytes = item.byte_cost;
            }
            close ();
            if (item.actor_mailbox)
                queue.execute_actor_head (std::move (item));
            else
                queue.execute_item (std::move (item));
        };
        if (queue.owns_shared_gate ())
            queue.notify_control ("selected-before-pause", std::move (selected));
        else
            selected ();
    }
};

namespace
{
TEST_P (StoreOutageRetention, NormalTurnsAndIneligibleObjectsDoNotReadExecutionClock)
{
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options,
                                        "clock-owner-" + std::to_string (retained)
                                          + std::to_string (shared));
            int reads = 0;
            lease_access::clock (runtime, [&] {
                ++reads;
                return clock_now.load ();
            });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            int calls = 0;
            reads = 0;
            ASSERT_TRUE (actor->try_post ("without-location-policy", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            spot->set_execution_policy (runtime.execution_policy ());
            actor->set_execution_policy (runtime.execution_policy ());
            actor->confirm_execution_ready (clock_now.load ());
            scheduler.run_all ();
            reads = 0;
            ASSERT_TRUE (actor->try_post ("without-outage", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            advance (5s);
            lease_access::communication_failure (runtime);
            reads = 0;
            // No Ready evidence.
            EXPECT_TRUE (spot->execution_allowed ());
            spot->confirm_execution_ready (clock_now.load ()); // Ready at observation.
            EXPECT_TRUE (spot->execution_allowed ());
            EXPECT_TRUE (actor->execution_allowed ());
            EXPECT_EQ (2, calls);
            const auto snapshot = lease_access::snapshot (runtime);
            advance (1s);
            lease_access::accept (runtime, *snapshot->token, *snapshot->renewed_at + 30s,
                                  *snapshot->renewed_at, clock_now.load (), true, snapshot->outage);
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            reads = 0;
            ASSERT_TRUE (actor->try_post ("after-outage-renewal", [&] { ++calls; }));
            scheduler.run_all ();
            EXPECT_EQ (0, reads);
            EXPECT_EQ (3, calls);
            spot->invalidate_execution_ready ();
            lease_access::communication_failure (runtime);
            move_to (*lease_access::snapshot (runtime)->admission_deadline + 1ms);
            EXPECT_FALSE (spot->execution_allowed ());
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
        }
}

TEST_P (StoreOutageRetention, AdmissionDeadlineLatchesWithoutHeartbeatAndRenewalReopens)
{
    for (bool retained : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = retained;
            location_runtime_t runtime (store (), options,
                                        "deadline-owner-" + std::to_string (retained)
                                          + std::to_string (shared));
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
            auto spot = std::make_shared<serial_execution_queue_t> (
              *scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (*scheduler);
            auto timer_queue = std::make_shared<serial_execution_queue_t> (*scheduler);
            actor->attach_spot_gate (spot);
            timer_queue->attach_spot_gate (spot);
            for (auto queue : {spot, actor, timer_queue}) {
                queue->set_execution_policy (runtime.execution_policy ());
                queue->confirm_execution_ready (clock_now.load ());
            }
            runtime
              .on_execution_discard (
                [] (auto) {}, {}, {},
                [actor = std::weak_ptr (actor), timer = std::weak_ptr (timer_queue)] {
                    if (auto queue = actor.lock ())
                        queue->execution_operation_changed ();
                    if (auto queue = timer.lock ())
                        queue->execution_operation_changed ();
                })
              .result ()
              .value ();
            auto context = std::make_shared<detail::spot_context_state_t> ();
            context->serial_executor = scheduler;
            context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
            serializer_registry_t serializers;
            context->channel_runtime->serializers = &serializers;
            auto handler = std::make_shared<int> (0);
            auto timer = std::make_shared<detail::timer_state_t> ();
            timer->name = "deadline-timer";
            timer->serial_queue = timer_queue;
            timer->handler_instance = handler;
            int timer_calls = 0, actor_calls = 0;
            timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                          const timer_tick_t &) {
                ++timer_calls;
                return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
            };
            scheduler->run_all ();
            const auto first = lease_access::snapshot (runtime);
            const auto deadline = *first->admission_deadline;
            move_to (deadline - 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (actor->try_post ("before-deadline", [&] { ++actor_calls; }));
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            scheduler->run_all ();
            EXPECT_EQ (1, actor_calls);
            EXPECT_EQ (1, timer_calls);
            // Callbacks queued before the deadline must not start once the deadline reservation runs.
            ASSERT_TRUE (actor->try_post ("queued-before-deadline", [&] { ++actor_calls; }));
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            move_to (deadline + 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            scheduler->run_all ();
            EXPECT_EQ (1, actor_calls);
            EXPECT_EQ (1, timer_calls);
            EXPECT_NE (serial_execution_queue_t::execution_status_t::allowed,
                       actor->execution_status ());
            EXPECT_NE (serial_execution_queue_t::execution_status_t::allowed,
                       timer_queue->execution_status ());
            lease_access::accept (runtime, *first->token, *first->renewed_at + 30s,
                                  *first->renewed_at, clock_now.load (), true);
            ASSERT_FALSE (lease_access::snapshot (runtime)->outage);
            scheduler->run_all ();
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                       actor->execution_status ());
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed,
                       timer_queue->execution_status ());
            EXPECT_EQ (2, actor_calls);
            EXPECT_EQ (2, timer_calls);
            ASSERT_TRUE (actor->try_post ("renewed", [&] { ++actor_calls; }));
            ASSERT_TRUE (timer_queue->try_post ("renewed-timer-lane", [] {}));
            scheduler->run_all ();
            EXPECT_EQ (3, actor_calls);
            EXPECT_EQ (2, timer_calls);
            detail::timer_runtime_t::post_fire_count (context, timer, timer_queue, 1);
            scheduler->run_all ();
            EXPECT_EQ (3, timer_calls);
            actor->cancel_pending ();
            timer_queue->cancel_pending ();
            spot->cancel_pending ();
            scheduler->run_all ();
        }
}

TEST_P (StoreOutageRetention, SecondOutageUsesNewSuccessfulLease)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "second-outage-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        std::vector<std::shared_ptr<serial_execution_queue_t>> queues;
        runtime
          .on_execution_discard ([&] (auto policy) {
              for (const auto &queue : queues)
                  if (policy (*queue)
                      == serial_execution_queue_t::execution_status_t::ownership_lost)
                      queue->cancel_pending (
                        serial_execution_queue_t::execution_status_t::ownership_lost);
          })
          .result ()
          .value ();
        const auto add = [&] {
            auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
            queue->set_execution_policy (runtime.execution_policy ());
            queue->confirm_execution_ready (clock_now.load ());
            queues.push_back (queue);
            scheduler.run_all ();
            return queue;
        };
        auto first = add ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto old = lease_access::snapshot (runtime)->outage;
        move_to (old->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        ASSERT_TRUE (old->discarded.load ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   first->execution_status ());
        store ().release_owner_lease (*lease_access::snapshot (runtime)->token).result ().value ();
        move_to (std::max (clock_now.load (),
                           lease_access::snapshot (runtime)->last_successful_lease->started_at
                             + options.owner_lease_ttl + options.owner_lease_fencing_margin));
        lease_access::heartbeat (runtime, clock_now.load ());
        const auto success = lease_access::snapshot (runtime)->last_successful_lease;
        ASSERT_NE (old->token.lease_generation, success->token.lease_generation);
        advance (1ms);
        auto second = add ();
        advance (1ms);
        lease_access::communication_failure (runtime);
        const auto current = lease_access::snapshot (runtime)->outage;
        ASSERT_NE (old, current);
        EXPECT_EQ (clock_now.load (), current->observed_at);
        EXPECT_EQ (retained ? success->started_at + options.owner_lease_ttl
                                + 2 * options.owner_lease_fencing_margin
                            : success->admission_deadline,
                   current->cutoff);
        move_to (current->cutoff - 1ms);
        int calls = 0;
        ASSERT_TRUE (second->try_post ("second-retained", [&] { ++calls; }));
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        move_to (current->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        EXPECT_TRUE (current->discarded.load ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   second->execution_status ());
        EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                   first->execution_status ());
        EXPECT_FALSE (first->try_post ("old-object", [&] { ++calls; }));
        EXPECT_FALSE (second->try_post ("discarded-second", [&] { ++calls; }));
        EXPECT_EQ (1, calls);
    }
}

TEST_P (StoreOutageRetention, PausedAdmissionRejectsNewOperationAndTimerTick)
{
    for (const auto policy :
         {timer_overrun_policy_t::skip_late_ticks, timer_overrun_policy_t::catch_up_bounded,
          timer_overrun_policy_t::delay_next_tick})
        for (bool stop : {false, true})
            for (bool recovery : {false, true}) {
                location_runtime_t runtime (store (), options,
                                            "paused-admission-" + std::to_string (int (policy))
                                              + std::to_string (stop) + std::to_string (recovery));
                lease_access::clock (runtime, [&] { return clock_now.load (); });
                claim (runtime);
                auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
                auto queue = std::make_shared<serial_execution_queue_t> (*scheduler);
                queue->set_execution_policy (runtime.execution_policy ());
                queue->confirm_execution_ready (clock_now.load ());
                scheduler->run_all ();
                auto context = std::make_shared<detail::spot_context_state_t> ();
                context->serial_executor = scheduler;
                context->serial_queue = queue;
                context->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
                serializer_registry_t serializers;
                context->channel_runtime->serializers = &serializers;
                auto timer = std::make_shared<detail::timer_state_t> ();
                timer->name = "paused-tick";
                timer->options.overrun_policy = policy;
                timer->options.stop_on_unhandled_exception = stop;
                timer->serial_queue = queue;
                auto timer_handler = std::make_shared<int> (0);
                timer->handler_instance = timer_handler;
                int calls = 0;
                timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                              const timer_tick_t &) {
                    ++calls;
                    return task_t<zlink::message_t> (result_t<zlink::message_t>::success ({}));
                };
                const auto lease = lease_access::snapshot (runtime);
                task_completion_source_t<serial_execution_queue_t::execution_operation_t> recovered;
                if (recovery)
                    queue->set_execution_operation (recovered.task ());
                else {
                    move_to (*lease->admission_deadline);
                    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
                }
                auto operation = queue->try_post_cancellable_async (
                  "new-paused", [] (auto complete) { complete ([] {}); }, [] {});
                EXPECT_FALSE (operation);
                if (!operation)
                    EXPECT_EQ (framework_error_kind_t::shutting_down, operation.error_kind ());
                EXPECT_FALSE (queue->try_post ("new-paused-raw", [] {}));
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
                scheduler->run_all ();
                EXPECT_EQ (0, queue->pending_count (serial_work_lane_t::application));
                EXPECT_EQ (1, timer->pending_fire_count);
                EXPECT_FALSE (timer->pending_fire);
                EXPECT_EQ (0, timer->delivery_index);
                EXPECT_TRUE (timer->failure_events.empty ());
                EXPECT_FALSE (timer->disposed);
                if (recovery)
                    recovered.complete (
                      result_t<serial_execution_queue_t::execution_operation_t>::success (
                        {serial_execution_queue_t::execution_status_t::allowed,
                         clock_now.load ()}));
                else
                    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s,
                                          *lease->renewed_at, clock_now.load (), true);
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 0);
                scheduler->run_all ();
                EXPECT_EQ (1, calls);
                EXPECT_EQ (1, timer->delivery_index);
                timer->handler_invoker = [] (void *, void *, serializer_registry_t &,
                                             const timer_tick_t &) -> task_t<zlink::message_t> {
                    throw std::runtime_error ("actual timer handler failure");
                };
                detail::timer_runtime_t::post_fire_count (context, timer, queue, 1);
                scheduler->run_all ();
                ASSERT_EQ (1, timer->failure_events.size ());
                EXPECT_EQ (stop, timer->disposed.load ());
                EXPECT_EQ (stop, timer->failure_events.front ().stopped);
                queue->cancel_pending ();
                scheduler->run_all ();
            }
}

TEST_P (StoreOutageRetention, SelectedApplicationReturnsToHeadWhenPaused)
{
    for (int layout : {0, 1, 2}) {
        const bool shared = layout != 0;
        location_runtime_t runtime (store (), options,
                                    "selected-paused-" + std::to_string (layout));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::actor_delivery ());
        auto owner = queue;
        if (layout == 2) {
            queue = std::make_shared<serial_execution_queue_t> (scheduler);
            queue->attach_spot_gate (owner);
            owner->set_execution_policy (runtime.execution_policy ());
            runtime
              .on_execution_discard ([] (auto) {}, {}, {},
                                     [weak = std::weak_ptr (owner)] {
                                         if (auto queue = weak.lock ())
                                             queue->execution_operation_changed ();
                                     })
              .result ()
              .value ();
        }
        queue->set_execution_policy (runtime.execution_policy ());
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        std::vector<int> calls;
        ASSERT_TRUE (queue->try_post ("selected", [&] { calls.push_back (1); }));
        ASSERT_TRUE (queue->try_post ("following", [&] { calls.push_back (2); }));
        const auto lease = lease_access::snapshot (runtime);
        serial_execution_queue_test_access_t::select_then (*owner, [&] {
            move_to (*lease->admission_deadline);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        });
        scheduler.run_all ();
        EXPECT_TRUE (calls.empty ());
        EXPECT_EQ (2, queue->pending_count (serial_work_lane_t::application));
        lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                              clock_now.load (), true);
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        EXPECT_EQ ((std::vector<int>{1, 2}), calls);
        queue->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, YieldedResumeWaitsThroughTemporaryClosure)
{
    location_runtime_t runtime (store (), options, "yield-paused");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    int errors = 0, resumes = 0;
    auto queue = std::make_shared<serial_execution_queue_t> (
      scheduler, serial_execution_queue_options_t{},
      [&] (const std::string &, std::exception_ptr error) {
          ++errors;
          EXPECT_THROW (std::rethrow_exception (error), std::runtime_error);
      },
      serial_lane_policy_t::spot_wide ());
    queue->set_execution_policy (runtime.execution_policy ());
    runtime
      .on_execution_discard ([] (auto) {}, {}, {},
                             [weak = std::weak_ptr (queue)] {
                                 if (auto queue = weak.lock ())
                                     queue->execution_operation_changed ();
                             })
      .result ()
      .value ();
    scheduler.run_all ();
    std::shared_ptr<detail::serial_turn_t> handler;
    ASSERT_TRUE (queue->try_post_async (
      "yield-before-pause", [&] (auto) { handler = detail::capture_current_serial_turn (); }));
    scheduler.run_all ();
    ASSERT_TRUE (handler);
    ASSERT_TRUE (handler->release ());
    scheduler.run_all ();
    const auto lease = lease_access::snapshot (runtime);
    move_to (*lease->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    handler->resume_scheduler () ([&] {
        ++resumes;
        handler->finish_handler ([] {}, true);
    });
    scheduler.run_all ();
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    const auto renewed = lease_access::snapshot (runtime);
    serial_execution_queue_test_access_t::select_then (*queue, [&] {
        move_to (*renewed->admission_deadline);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    });
    scheduler.run_all ();
    auto observed = queue->observe_handler_terminal ();
    ASSERT_TRUE (observed);
    scheduler.run_all ();
    auto terminal = observed.value ()->run ();
    EXPECT_FALSE (terminal.await_ready ());
    EXPECT_EQ (0, resumes);
    EXPECT_EQ (0, errors);
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    scheduler.run_all ();
    EXPECT_EQ (1, resumes);
    scheduler.run_until (terminal);
    EXPECT_TRUE (terminal.await_ready ());
    observed.value ()->cancel ();
    ASSERT_TRUE (
      queue->try_post ("actual-handler-failure", [] { throw std::runtime_error ("handler"); }));
    scheduler.run_all ();
    EXPECT_EQ (1, errors);
    queue->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, PausedCancellationIsTerminalBeforeReopening)
{
    for (int stage : {0, 1, 2}) {
        location_runtime_t runtime (store (), options, "cancel-paused-" + std::to_string (stage));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
        queue->set_execution_policy (runtime.execution_policy ());
        runtime
          .on_execution_discard ([] (auto) {}, {}, {},
                                 [weak = std::weak_ptr (queue)] {
                                     if (auto queue = weak.lock ())
                                         queue->execution_operation_changed ();
                                 })
          .result ()
          .value ();
        scheduler.run_all ();
        int calls = 0, cancellations = 0;
        auto submission = queue->try_post_cancellable_async (
          "cancel-paused",
          [&] (auto complete) {
              ++calls;
              complete ([] {});
          },
          [&] { ++cancellations; });
        ASSERT_TRUE (submission);
        const auto lease = lease_access::snapshot (runtime);
        const auto close = [&] {
            move_to (*lease->admission_deadline);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        };
        if (stage != 0)
            serial_execution_queue_test_access_t::select_then (*queue, [&] {
                close ();
                if (stage == 2) {
                    auto active = queue->cancel_submission (submission.value ());
                    scheduler.run_until (active);
                    EXPECT_EQ (serial_cancel_submission_outcome_t::active_cancel_requested,
                               active.result ().value ());
                }
            });
        else
            close ();
        scheduler.run_all ();
        auto cancelled = queue->cancel_submission (submission.value ());
        scheduler.run_all ();
        EXPECT_EQ (stage == 2 ? serial_cancel_submission_outcome_t::already_terminal
                              : serial_cancel_submission_outcome_t::queued_cancelled,
                   cancelled.result ().value ());
        EXPECT_EQ (1, cancellations);
        lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                              clock_now.load (), true);
        scheduler.run_all ();
        EXPECT_EQ (0, calls);
        auto again = queue->cancel_submission (submission.value ());
        scheduler.run_all ();
        EXPECT_EQ (serial_cancel_submission_outcome_t::already_terminal, again.result ().value ());
        EXPECT_EQ (1, cancellations);
        queue->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, PausedDeadlineEndsOnceWithoutExecutingAfterRenewal)
{
    location_runtime_t runtime (store (), options, "deadline-paused");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    queue->set_execution_policy (runtime.execution_policy ());
    runtime
      .on_execution_discard ([] (auto) {}, {}, {},
                             [weak = std::weak_ptr (queue)] {
                                 if (auto queue = weak.lock ())
                                     queue->execution_operation_changed ();
                             })
      .result ()
      .value ();
    scheduler.run_all ();
    int calls = 0, terminals = 0;
    task_completion_source_t<void> terminal;
    const auto lease = lease_access::snapshot (runtime);
    auto submission = queue->try_post_cancellable_async (
      "deadline-pending",
      [&] (auto complete) {
          ++calls;
          complete ([] {});
      },
      [&] {
          ++terminals;
          terminal.complete (result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                                      "operation deadline"));
      });
    ASSERT_TRUE (submission);
    move_to (*lease->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    scheduler.run_all ();
    auto deadline = std::make_shared<detail::deadline_state_t> ();
    std::optional<task_t<serial_cancel_submission_outcome_t>> cancellation;
    detail::deadline_scheduler_t::instance ().schedule (clock_now.load () + 1ms, deadline, [&] {
        cancellation.emplace (queue->cancel_submission (submission.value ()));
    });
    advance (1ms);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    scheduler.run_all ();
    ASSERT_TRUE (cancellation);
    EXPECT_EQ (serial_cancel_submission_outcome_t::queued_cancelled,
               cancellation->result ().value ());
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, terminal.task ().result ().error_kind ());
    lease_access::accept (runtime, *lease->token, *lease->renewed_at + 30s, *lease->renewed_at,
                          clock_now.load (), true);
    scheduler.run_all ();
    EXPECT_EQ (0, calls);
    EXPECT_EQ (1, terminals);
    queue->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, RetiredEpisodeFencesUntilCleanupAndNewDeadlineStillCloses)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "retired-fence");
    int clock_reads = 0;
    lease_access::clock (runtime, [&] {
        ++clock_reads;
        return clock_now.load ();
    });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    auto old = std::make_shared<serial_execution_queue_t> (scheduler);
    old->set_execution_policy (runtime.execution_policy ());
    old->confirm_execution_ready (clock_now.load ());
    serial_execution_queue_t::execution_admission_t cleanup;
    runtime.on_execution_discard ([&] (auto fence) { cleanup = std::move (fence); })
      .result ()
      .value ();
    scheduler.run_all ();
    advance (5s);
    lease_access::communication_failure (runtime);
    auto episode = lease_access::snapshot (runtime)->outage;
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    ASSERT_TRUE (cleanup);
    ASSERT_FALSE (old->closed ());
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto recovered = lease_access::snapshot (runtime);
    ASSERT_TRUE (recovered->token);
    EXPECT_FALSE (recovered->outage);
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    auto current = std::make_shared<serial_execution_queue_t> (scheduler);
    current->set_execution_policy (runtime.execution_policy ());
    current->confirm_execution_ready (clock_now.load ());
    scheduler.run_all ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, current->execution_status ());
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> verification;
    current->set_execution_operation (verification.task ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::stopped, current->execution_status ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    verification.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, clock_now.load ()}));
    scheduler.run_all ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, current->execution_status ());
    move_to (*recovered->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (current->execution_paused ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    EXPECT_FALSE (current->try_post ("expired-new-lease", [] {}));
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost, cleanup (*old));
    old->cancel_pending (serial_execution_queue_t::execution_status_t::ownership_lost);
    scheduler.run_all ();
    cleanup = {};
    episode.reset ();
    lease_access::accept (runtime, *recovered->token, *recovered->renewed_at + 30s,
                          *recovered->renewed_at, clock_now.load (), true);
    EXPECT_TRUE (lease_access::snapshot (runtime)->discarded_outages.empty ());
    clock_reads = 0;
    EXPECT_TRUE (current->execution_allowed ());
    EXPECT_EQ (0, clock_reads);
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
               old->execution_status ());
    current->cancel_pending ();
    scheduler.run_all ();
}

TEST_P (StoreOutageRetention, DelayedAdmissionCallbackDoesNotCloseRenewedLease)
{
    location_runtime_t runtime (store (), options, "delayed-deadline-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    test::manual_internal_scheduler_t scheduler;
    serial_execution_queue_t queue (scheduler);
    const auto policy = runtime.execution_policy ();
    const auto first = lease_access::snapshot (runtime);
    move_to (*first->admission_deadline);
    auto callbacks = detail::deadline_scheduler_test_access_t::take_due (clock_now.load ());
    ASSERT_FALSE (callbacks.empty ());
    lease_access::accept (runtime, *first->token, *first->renewed_at + 30s, *first->renewed_at,
                          clock_now.load (), true);
    const auto renewed = lease_access::snapshot (runtime);
    ASSERT_GT (*renewed->admission_deadline, clock_now.load ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, policy (queue));
    for (auto &callback : callbacks)
        callback ();
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::allowed, policy (queue));
    move_to (*renewed->admission_deadline);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_EQ (serial_execution_queue_t::execution_status_t::stopped, policy (queue));
}

TEST_P (StoreOutageRetention, ExecutionGateConsumesOutageLatchInBothExecutionModes)
{
    for (bool enabled : {false, true})
        for (bool shared : {false, true}) {
            options.store_outage_retention = enabled;
            location_runtime_t runtime (store (), options,
                                        "gate-owner-" + std::to_string (enabled)
                                          + std::to_string (shared));
            lease_access::clock (runtime, [&] { return clock_now.load (); });
            claim (runtime);
            test::manual_internal_scheduler_t scheduler;
            auto spot = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              shared ? serial_lane_policy_t::spot_wide ()
                     : serial_lane_policy_t::per_actor_spot ());
            auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
            actor->attach_spot_gate (spot);
            // Supply the Location latch to the gate without duplicating its deadline policy.
            const auto policy = [&runtime] (const serial_execution_queue_t &) {
                const auto episode = lease_access::snapshot (runtime)->outage;
                return !episode || !episode->discarded.load (std::memory_order_acquire)
                         ? serial_execution_queue_t::execution_status_t::allowed
                         : serial_execution_queue_t::execution_status_t::ownership_lost;
            };
            spot->set_execution_policy (policy);
            actor->set_execution_policy (policy);
            scheduler.run_all ();
            advance (5s);
            lease_access::communication_failure (runtime);
            const auto episode = lease_access::snapshot (runtime)->outage;
            int messages = 0, spot_calls = 0, completions = 0;
            move_to (episode->cutoff - 1ms);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (actor->try_post ("cached-actor", [&] { ++messages; }));
            ASSERT_TRUE (spot->try_post ("cached-spot", [&] { ++spot_calls; }));
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            serial_work_options_t queued;
            queued.completion = [&] (result_t<void> result) {
                EXPECT_FALSE (result);
                EXPECT_EQ (framework_error_kind_t::not_found, result.error_kind ());
                ++completions;
            };
            ASSERT_TRUE (actor->try_post ("queued-actor", [&] { ++messages; }, queued));
            ASSERT_TRUE (spot->try_post ("queued-spot", [&] { ++spot_calls; }, queued));
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            ASSERT_TRUE (episode->discarded.load ());
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            EXPECT_EQ (0, completions);
            const auto lease = lease_access::snapshot (runtime);
            auto new_token = *lease->token;
            ++new_token.lease_generation;
            lease_access::accept (runtime, new_token, *lease->renewed_at + 30s, *lease->renewed_at,
                                  clock_now.load (), false);
            ASSERT_TRUE (actor->try_post ("new-claim", [&] { ++messages; }, queued));
            scheduler.run_all ();
            EXPECT_EQ (1, messages);
            EXPECT_EQ (1, spot_calls);
            actor->cancel_pending ();
            spot->cancel_pending ();
            scheduler.run_all ();
            EXPECT_EQ (3, completions);
            EXPECT_EQ (0u, actor->pending_count ());
            EXPECT_EQ (0u, spot->pending_count ());
        }
}

TEST (ZLinkFrameworkLocationExecutionGate, MemberUsesItsExistingSpotGatePolicyInBothExecutionModes)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        auto other = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        other->attach_spot_gate (spot);
        auto parent = std::make_shared<const int> (1);
        auto member = std::make_shared<const int> (2);
        auto other_member = std::make_shared<const int> (3);
        const auto policy = [] (const auto &object) {
            return [weak = std::weak_ptr (object)] { return !weak.expired (); };
        };
        spot->set_execution_policy (policy (parent));
        actor->set_execution_policy (policy (member));
        other->set_execution_policy (policy (other_member));
        if (shared)
            spot->set_application_start_policy ([] (const auto &) { return false; });
        scheduler.run_all ();
        int actor_calls = 0, other_calls = 0, spot_calls = 0;
        ASSERT_TRUE (actor->try_post ("member", [&] { ++actor_calls; }));
        ASSERT_TRUE (other->try_post ("other-member", [&] { ++other_calls; }));
        ASSERT_TRUE (spot->try_post ("spot", [&] { ++spot_calls; }));
        if (shared) {
            scheduler.run_all ();
            EXPECT_EQ (0, actor_calls);
            EXPECT_EQ (0, other_calls);
            EXPECT_EQ (0, spot_calls);
        }
        member.reset ();
        spot->set_application_start_policy ([] (const auto &) { return true; });
        scheduler.run_all ();
        EXPECT_EQ (0, actor_calls);
        EXPECT_EQ (1, other_calls);
        EXPECT_EQ (1, spot_calls);
        ASSERT_TRUE (other->try_post ("parent-discarded", [&] { ++other_calls; }));
        parent.reset ();
        scheduler.run_all ();
        EXPECT_EQ (1, other_calls);
        EXPECT_TRUE (policy (other_member) ());
        actor->cancel_pending ();
        other->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0u, actor->pending_count ());
        EXPECT_EQ (0u, other->pending_count ());
        EXPECT_EQ (0u, spot->pending_count ());
    }
}

TEST (ZLinkFrameworkLocationExecutionGate, ExecutionPolicyPreservesApplicationStartPolicy)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        auto object = std::make_shared<const int> (1);
        actor->set_application_start_policy ([] (const auto &) { return false; });
        actor->set_execution_policy ([weak = std::weak_ptr (object)] { return !weak.expired (); });
        scheduler.run_all ();
        int calls = 0;
        ASSERT_TRUE (actor->try_post ("preparation-held", [&] { ++calls; }));
        scheduler.run_all ();
        EXPECT_EQ (0, calls);
        actor->set_application_start_policy ([] (const auto &) { return true; });
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        ASSERT_TRUE (actor->try_post ("execution-discarded", [&] { ++calls; }));
        object.reset ();
        scheduler.run_all ();
        EXPECT_EQ (1, calls);
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST_P (StoreOutageRetention, OutagePolicyAllowsLifecycleAndLateCompletionWithoutNewHandler)
{
    for (bool shared : {false, true}) {
        options.store_outage_retention = true;
        location_runtime_t runtime (
          store (), options, shared ? "shared-completion-owner" : "per-actor-completion-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto spot = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto actor = std::make_shared<serial_execution_queue_t> (scheduler);
        actor->attach_spot_gate (spot);
        const auto policy = [&runtime] {
            const auto episode = lease_access::snapshot (runtime)->outage;
            return !episode || !episode->discarded.load (std::memory_order_acquire);
        };
        spot->set_execution_policy (policy);
        actor->set_execution_policy (policy);
        scheduler.run_all ();
        serial_execution_queue_t::async_completion_t finish;
        int started = 0, settled = 0, lifecycle = 0;
        ASSERT_TRUE (actor->try_post_async ("inflight", [&] (auto complete) {
            ++started;
            finish = std::move (complete);
        }));
        scheduler.run_all ();
        ASSERT_EQ (1, started);
        ASSERT_TRUE (finish);
        ASSERT_TRUE (actor->try_post ("unstarted", [&] { ++started; }));
        advance (5s);
        lease_access::communication_failure (runtime);
        move_to (lease_access::snapshot (runtime)->outage->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        serial_work_options_t infrastructure;
        infrastructure.lane = serial_work_lane_t::lifecycle;
        ASSERT_TRUE (spot->try_post ("cleanup", [&] { ++lifecycle; }, infrastructure));
        std::exchange (finish, {}) ([&] { ++settled; });
        scheduler.run_all ();
        EXPECT_EQ (1, started);
        EXPECT_EQ (1, settled);
        EXPECT_EQ (1, lifecycle);
        actor->cancel_pending ();
        spot->cancel_pending ();
        scheduler.run_all ();
        EXPECT_EQ (0u, actor->pending_count ());
        EXPECT_EQ (0u, spot->pending_count ());
    }
}
} // namespace
} // namespace zlink::framework::runtime

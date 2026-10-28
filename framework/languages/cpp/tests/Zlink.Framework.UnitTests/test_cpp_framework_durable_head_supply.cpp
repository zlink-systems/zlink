/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include <gtest/gtest.h>
#include <latch>
#include <thread>

namespace
{
namespace rt = zlink::framework::runtime;

void occupied_handler_capacity (bool durable)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto spot = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (spot);
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{});
    auto reserved = jobs->try_reserve_supply ();
    ASSERT_TRUE (reserved);
    auto initial = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*reserved));
    initial->mark_queued ();
    auto prefix = std::make_shared<rt::serial_application_record_t> ();
    prefix->admission.jobs = jobs;
    auto ordinary = std::make_shared<rt::serial_application_record_t> ();
    ordinary->admission.jobs = jobs;
    ordinary->admission.retain_initial ([initial] { initial->release_without_handler (); });
    std::latch prefix_ready (1), producer_done (1);
    rt::serial_execution_queue_t::async_completion_t finish;
    std::optional<rt::application_job_queue_t::waiter_t> waiting;
    bool published = false, adopted = false, handler_terminal = false, supplied = false;
    bool early_handler = false;
    std::uint32_t before_import = 0;
    int calls = 0;
    std::thread producer ([&] {
        prefix_ready.wait ();
        rt::serial_work_options_t options;
        options.application_record = ordinary;
        published = actor->try_post (
          "ordinary",
          [&] {
              early_handler |= !handler_terminal;
              ordinary->admission.enter_handler ();
              ++calls;
          },
          options);
        producer_done.count_down ();
    });
    const bool posted = spot->try_post_async ("occupied-handler", [&] (auto complete) {
        finish = std::move (complete);
        if (durable) {
            rt::serial_prefix_work_t work;
            work.name = "durable-prefix";
            work.options.application_record = prefix;
            work.work = [&] (auto done) {
                early_handler |= !handler_terminal;
                prefix->admission.enter_handler ();
                ++calls;
                done ([] {});
            };
            auto result = actor
                            ->adopt_application_prefix (
                              [work] { return std::vector<rt::serial_prefix_work_t>{work}; })
                            .result ();
            adopted = bool (result) && result.value ();
        }
        prefix_ready.count_down ();
        producer_done.wait ();
        before_import = jobs->snapshot ().permits_in_use;
        waiting.emplace (jobs->wait_for_supply (
          [&] (auto permit) {
              if (permit) {
                  supplied = true;
                  permit.reset ();
                  handler_terminal = true;
                  finish ([] {});
              }
          },
          rt::application_job_queue_t::origin_t::local));
        // The handler retains its turn until capacity arrives; it does not Yield.
    });
    if (!posted)
        prefix_ready.count_down ();
    worker.run_all ();
    producer.join ();
    EXPECT_TRUE (posted);
    EXPECT_TRUE (published);
    EXPECT_EQ (before_import, 1u);
    EXPECT_FALSE (early_handler);
    if (durable) {
        EXPECT_TRUE (adopted);
        EXPECT_FALSE (bool (*initial));
        EXPECT_TRUE (ordinary->admission.durable ());
        EXPECT_TRUE (supplied) << "DURABLE_CAPACITY_CYCLE_REPRODUCED";
        EXPECT_TRUE (handler_terminal);
        EXPECT_EQ (calls, 2);
        EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    } else {
        EXPECT_FALSE (supplied);
        EXPECT_FALSE (handler_terminal);
        EXPECT_TRUE (bool (*initial));
        EXPECT_FALSE (ordinary->admission.durable ());
        EXPECT_EQ (calls, 0);
        EXPECT_EQ (jobs->snapshot ().capacity_waiters, 1u);
    }
    // Cleanup also runs on RED, after observing the unassisted transition.
    waiting.reset ();
    if (!handler_terminal && finish) {
        handler_terminal = true;
        finish ([] {});
    }
    worker.run_all ();
    EXPECT_EQ (calls, durable ? 2 : 1);
    actor->close ();
    worker.run_all ();
    actor->drain ();
    spot->close ();
    worker.run_all ();
    spot->drain ();
    worker.retire ();
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
}

TEST (DurableHeadSupply, OccupiedSharedGateDoesNotReserveActorSupply)
{
    occupied_handler_capacity (true);
}

TEST (DurableHeadSupply, OrdinarySuffixKeepsInitialSupplyUntilHandlerEntry)
{
    occupied_handler_capacity (false);
}
TEST (DurableHeadSupply, PendingActorTicketReleasesGateAndReconnectsOnSupply)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto spot = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (spot);
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{});
    auto held = jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->admission.jobs = jobs;
    int calls = 0;
    rt::serial_prefix_work_t work;
    work.name = "waiting-actor";
    work.options.application_record = record;
    work.work = [&] (auto complete) {
        record->admission.enter_handler ();
        ++calls;
        complete ([] {});
    };
    auto adopted = actor->adopt_application_prefix (
      [work] { return std::vector<rt::serial_prefix_work_t>{work}; });
    worker.run_all ();
    EXPECT_TRUE (adopted.result ().value ());
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 1u);
    EXPECT_EQ (calls, 0);
    int unrelated_calls = 0;
    EXPECT_TRUE (spot->try_post ("other-spot-work", [&] { ++unrelated_calls; }));
    rt::serial_work_options_t lifecycle;
    lifecycle.lane = rt::serial_work_lane_t::lifecycle;
    EXPECT_TRUE (actor->try_post ("actor-lifecycle", [&] { ++unrelated_calls; }, lifecycle));
    worker.run_all ();
    EXPECT_EQ (unrelated_calls, 2);
    EXPECT_EQ (calls, 0);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 1u);
    held.reset ();
    worker.run_all ();
    EXPECT_EQ (calls, 1);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    actor->close ();
    worker.run_all ();
    actor->drain ();
    spot->close ();
    worker.run_all ();
    spot->drain ();
    worker.retire ();
}

} // namespace

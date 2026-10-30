/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include <gtest/gtest.h>
#include <latch>
#include <string>
#include <vector>
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
    int calls = 0;
    std::vector<std::string> callback_order;
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
              callback_order.emplace_back ("ordinary");
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
                callback_order.emplace_back ("prefix");
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
    EXPECT_FALSE (early_handler);
    if (durable) {
        EXPECT_TRUE (adopted);
        EXPECT_FALSE (bool (*initial));
        EXPECT_TRUE (ordinary->admission.durable ());
        EXPECT_TRUE (supplied) << "DURABLE_CAPACITY_CYCLE_REPRODUCED";
        EXPECT_TRUE (handler_terminal);
        EXPECT_EQ (calls, 2);
        EXPECT_EQ (callback_order, (std::vector<std::string>{"prefix", "ordinary"}));
        EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    } else {
        EXPECT_FALSE (supplied);
        EXPECT_FALSE (handler_terminal);
        EXPECT_TRUE (bool (*initial));
        EXPECT_FALSE (ordinary->admission.durable ());
        EXPECT_EQ (calls, 0);
        EXPECT_TRUE (callback_order.empty ());
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


// A blocked owner callback prevents every queued import from running. The
// producer returns on the test thread, outside that owner's execution context.
void acceptance_before_gate_import (bool durable)
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
    int initial_releases = 0, native_releases = 0, owner_transfers = 0;
    int prefix_native_releases = 0, prefix_terminals = 0, ordinary_terminals = 0;
    int retained_releases = 0;
    auto retained = std::shared_ptr<const void> (new int (1), [&] (const void *value) {
        delete static_cast<const int *> (value);
        ++retained_releases;
    });
    std::weak_ptr<const void> retained_weak = retained;
    auto prefix = std::make_shared<rt::serial_application_record_t> ();
    prefix->admission.jobs = jobs;
    prefix->transferred = [&] { ++prefix_native_releases; };
    auto ordinary = std::make_shared<rt::serial_application_record_t> ();
    ordinary->admission.jobs = jobs;
    ordinary->admission.retain_initial ([&] {
        ++initial_releases;
        initial->release_without_handler ();
    });
    ordinary->transferred = [&] { ++native_releases; };
    std::latch import_blocked (1), allow_import (1);
    bool adopted = !durable;
    std::vector<std::string> callback_order;
    ASSERT_TRUE (spot->try_post_async ("block-actor-import", [&] (auto complete) {
        if (durable) {
            rt::serial_prefix_work_t work;
            work.name = "accepted-durable-prefix";
            work.options.application_record = prefix;
            work.work = [&] (auto done) {
                prefix->admission.enter_handler ();
                callback_order.emplace_back ("prefix");
                done ([&] { ++prefix_terminals; });
            };
            auto result = actor
                            ->adopt_application_prefix (
                              [work] { return std::vector<rt::serial_prefix_work_t>{work}; })
                            .result ();
            adopted = bool (result) && result.value ();
        }
        // The prefix is imported while this Spot turn is active. No Actor
        // handler or subsequent publication import can pass the second latch.
        import_blocked.count_down ();
        allow_import.wait ();
        complete ([] {});
    }));
    std::thread gate ([&] { worker.run_all (); });
    import_blocked.wait ();
    rt::serial_work_options_t options;
    options.application_record = ordinary;
    options.retained_message = std::move (retained);
    options.transfer_owner_reservation = [&] { ++owner_transfers; };
    const bool accepted = actor->try_post_async (
      "accepted-ordinary-suffix",
      [&] (auto complete) {
          ordinary->admission.enter_handler ();
          callback_order.emplace_back ("ordinary");
          complete ([&] { ++ordinary_terminals; });
      },
      std::move (options));
    // This is the acceptance boundary, before releasing the gate-import latch.
    // S 04:153-158 requires the durable suffix to return its initial permit now.
    EXPECT_TRUE (accepted);
    EXPECT_TRUE (adopted);
    EXPECT_TRUE (callback_order.empty ());
    EXPECT_EQ (owner_transfers, 1);
    EXPECT_EQ (initial_releases, durable ? 1 : 0);
    EXPECT_EQ (bool (*initial), !durable);
    EXPECT_EQ (jobs->snapshot ().permits_in_use, durable ? 0u : 1u);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (ordinary_terminals, 0);
    EXPECT_EQ (retained_releases, 0);
    EXPECT_FALSE (retained_weak.expired ());
    // Always release the gate on RED too, so a failed boundary assertion does
    // not prevent the ordering and exact-once cleanup observations below.
    allow_import.count_down ();
    gate.join ();
    worker.run_all ();
    EXPECT_EQ (callback_order, durable ? (std::vector<std::string>{"prefix", "ordinary"})
                                       : (std::vector<std::string>{"ordinary"}));
    EXPECT_EQ (initial_releases, 1);
    EXPECT_FALSE (bool (*initial));
    EXPECT_EQ (native_releases, 1);
    EXPECT_EQ (owner_transfers, 1);
    EXPECT_EQ (ordinary_terminals, 1);
    EXPECT_EQ (prefix_native_releases, durable ? 1 : 0);
    EXPECT_EQ (prefix_terminals, durable ? 1 : 0);
    actor->close ();
    worker.run_all ();
    actor->drain ();
    spot->close ();
    worker.run_all ();
    spot->drain ();
    worker.retire ();
    EXPECT_EQ (actor->pending_count (), 0u);
    EXPECT_EQ (spot->pending_count (), 0u);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    EXPECT_EQ (retained_releases, 1);
    EXPECT_TRUE (retained_weak.expired ());
    // Recheck after shutdown: no terminal or lease may settle a second time.
    EXPECT_EQ (initial_releases, 1);
    EXPECT_EQ (native_releases, 1);
    EXPECT_EQ (owner_transfers, 1);
    EXPECT_EQ (ordinary_terminals, 1);
    EXPECT_EQ (prefix_native_releases, durable ? 1 : 0);
    EXPECT_EQ (prefix_terminals, durable ? 1 : 0);
}

TEST (DurableHeadSupply, DurableSuffixReturnsInitialSupplyBeforeGateImport)
{
    acceptance_before_gate_import (true);
}

TEST (DurableHeadSupply, OrdinaryAcceptanceRetainsInitialSupplyBeforeGateImport)
{
    acceptance_before_gate_import (false);
}

TEST (DurableHeadSupply, RegistrationCallbackReplacesActorHeadBeforeSupplyCommit)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto spot = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    auto actor = std::make_shared<rt::serial_execution_queue_t> (worker);
    actor->attach_spot_gate (spot);
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{});
    auto original = std::make_shared<rt::serial_application_record_t> ();
    auto prefix = std::make_shared<rt::serial_application_record_t> ();
    original->admission.jobs = prefix->admission.jobs = jobs;
    int original_native = 0, prefix_native = 0;
    original->transferred = [&] { ++original_native; };
    prefix->transferred = [&] { ++prefix_native; };
    std::vector<std::string> order;
    const auto make_work = [&] (const auto &record, std::string name) {
        rt::serial_prefix_work_t work;
        work.name = name;
        work.options.application_record = record;
        work.work = [&, record, name = std::move (name)] (auto complete) {
            record->admission.enter_handler ();
            order.push_back (name);
            complete ([] {});
        };
        return work;
    };
    bool replaced = false, prefix_adopted = false;
    auto flow = jobs->register_receive_flow_socket ([&] (auto state) {
        if (state == zlink::framework::application_job_queue_pressure_state_t::paused
            && !replaced) {
            replaced = true;
            // register_supply calls this setter synchronously after reserving
            // the original head's permit, before its reservation is installed.
            const auto adopted =
              actor
                ->adopt_application_prefix ([&] {
                    return std::vector<rt::serial_prefix_work_t>{make_work (prefix, "prefix")};
                })
                .result ();
            prefix_adopted = bool (adopted) && adopted.value ();
        }
        return rt::receive_flow_state_apply_result_t::applied;
    });
    auto accepted = actor->adopt_application_prefix (
      [&] { return std::vector<rt::serial_prefix_work_t>{make_work (original, "original")}; });
    worker.run_all ();
    const auto adopted = accepted.result ();
    EXPECT_TRUE (bool (adopted) && adopted.value ());
    EXPECT_TRUE (replaced);
    EXPECT_TRUE (prefix_adopted);
    EXPECT_EQ (order, (std::vector<std::string>{"prefix", "original"}));
    EXPECT_EQ (original_native, 1);
    EXPECT_EQ (prefix_native, 1);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    // Clear unexecuted records on RED after observing the stalled selection.
    actor->cancel_pending ();
    worker.run_all ();
    actor->drain ();
    spot->close ();
    worker.run_all ();
    spot->drain ();
    flow.close ();
    worker.retire ();
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    EXPECT_EQ (original_native, 1);
    EXPECT_EQ (prefix_native, 1);
}

} // namespace

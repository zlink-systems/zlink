/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/location_runtime_auto_connect_fixture.hpp"
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
namespace
{
TEST_P (StoreOutageRetention, OrdinaryPublicationTransfersCallbackWithoutStart)
{
    for (const bool confirmed : {false, true}) {
        location_runtime_t runtime (store (), options,
                                    confirmed ? "callback-confirmed" : "callback-unconfirmed");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto builder = test::runtime_failure_builder ();
        handler_registry_t handlers;
        serializer_registry_t serializers;
        auto failures = std::make_shared<runtime_failure_collector_t> ();
        unsigned publications = 0;
        location_auto_connect_host_service_t service (
          failures, builder.message_bus (), {}, handlers, serializers,
          [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
          [&] (auto intent, auto, auto) {
              ++publications;
              EXPECT_EQ (location_write_intent_t::renew, intent);
              return task_t<bool> (result_t<bool>::success (confirmed));
          });
        EXPECT_EQ (confirmed,
                   location_auto_connect_test_access_t::publish_without_start (service, runtime)
                     .result ()
                     .value ());
        EXPECT_EQ (1u, publications);
        EXPECT_EQ (confirmed,
                   runtime
                     .republish_descriptors (location_write_intent_t::renew, {},
                                             clock_now.load () + options.owner_lease_renew_timeout)
                     .result ()
                     .value ());
        EXPECT_EQ (2u, publications);
        EXPECT_EQ (confirmed,
                   location_auto_connect_test_access_t::publish_without_start (service, runtime)
                     .result ()
                     .value ());
        EXPECT_EQ (3u, publications);
        service.stop ();
        EXPECT_FALSE (
          runtime
            .republish_descriptors (location_write_intent_t::renew, {},
                                    clock_now.load () + options.owner_lease_renew_timeout)
            .result ()
            .value ());
        EXPECT_EQ (3u, publications);
        failures->rethrow_if_failed ();
    }
}

TEST_P (StoreOutageRetention, FirstPublicationWaitsForRenewalObservation)
{
    location_runtime_t runtime (store (), options, "first-publication-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    serializer_registry_t serializers;
    mesh_node_host_service_t host (std::make_shared<runtime_failure_collector_t> (), {},
                                   serializers);
    mesh_node_host_service_test_access_t::configure (
      host, std::shared_ptr<location_repository_t> (&store (), [] (auto *) {}), runtime);
    lease_access::publisher (runtime, [&] (auto intent, auto cancellation, auto deadline) {
        return host.republish_after_store_recovery (intent, cancellation, deadline);
    });
    const auto observed = runtime.owner_lease_observation ();
    const auto [unchanged, ordinary] = runtime.descriptor_publication_observation (observed);
    EXPECT_EQ (observed, unchanged);
    EXPECT_FALSE (
      runtime
        .republish_descriptors (ordinary, {}, clock_now.load () + options.owner_lease_renew_timeout)
        .result ()
        .value ());
    EXPECT_TRUE (store ().list_mesh_nodes ("publication-mesh").result ().value ().items.empty ());
    advance (1ms);
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto [renewed, retry] = runtime.descriptor_publication_observation (observed);
    EXPECT_NE (observed, renewed);
    EXPECT_TRUE (
      runtime
        .republish_descriptors (retry, {}, clock_now.load () + options.owner_lease_renew_timeout)
        .result ()
        .value ());
    EXPECT_EQ (1u, store ().list_mesh_nodes ("publication-mesh").result ().value ().items.size ());
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    EXPECT_EQ (0u, provider.preserve_writes.load ());
    EXPECT_EQ (0u, memory.preserve_writes.load ());
}

TEST_P (StoreOutageRetention, InitialClaimAndOrdinaryPublicationKeepStartupOwnership)
{
    for (const bool confirmed : {false, true}) {
        location_runtime_t runtime (store (), options,
                                    confirmed ? "startup-confirmed" : "startup-waiting");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        unsigned publications = 0, recoveries = 0;
        lease_access::publisher (runtime, [&] (auto intent, auto, auto) {
            ++publications;
            EXPECT_EQ (location_write_intent_t::renew, intent);
            return task_t<bool> (result_t<bool>::success (confirmed));
        });
        runtime
          .on_execution_discard ([] (auto) {}, {},
                                 [&] (location_runtime_t::execution_recovery_t, monotonic_time) {
                                     ++recoveries;
                                     return task_t<void> (result_t<void>::success ());
                                 })
          .result ()
          .value ();
        claim (runtime);
        const auto initial = *lease_access::snapshot (runtime)->token;
        EXPECT_EQ (0u, publications);
        advance (1ms);
        lease_access::heartbeat (runtime, clock_now.load ());
        EXPECT_EQ (initial.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_EQ (0u, publications);
        EXPECT_EQ (0u, recoveries);
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (confirmed,
                   runtime
                     .republish_descriptors (location_write_intent_t::renew, {},
                                             clock_now.load () + options.owner_lease_renew_timeout)
                     .result ()
                     .value ());
        EXPECT_EQ (1u, publications);
        EXPECT_EQ (0u, recoveries);
        EXPECT_TRUE (runtime.owner_lease_healthy ());
        EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (0u, provider.preserve_writes.load ());
        EXPECT_EQ (0u, memory.preserve_writes.load ());
    }
}

TEST_P (StoreOutageRetention, StaleLeaseKeepsObjectsAndOriginalEpisodeUntilCutoff)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options, "stale-owner-" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        test::manual_internal_scheduler_t scheduler;
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy (runtime.execution_policy ());
        gate->confirm_execution_ready (clock_now.load ());
        int discarded = 0;
        runtime
          .on_execution_discard ([&] (auto allowed) {
              if (allowed (*gate) != serial_execution_queue_t::execution_status_t::allowed) {
                  ++discarded;
                  gate->cancel_pending ();
              }
          })
          .result ()
          .value ();
        scheduler.run_all ();
        advance (5s);
        lease_access::communication_failure (runtime);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        const auto old_token = *lease_access::snapshot (runtime)->token;
        store ().release_owner_lease (old_token).result ().value ();
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_EQ (old_token.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        EXPECT_TRUE (std::holds_alternative<owner_lease_missing_t> (
          store ().read_owner_lease (old_token.owner_id).result ().value ()));
        EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (0, discarded);
        int handlers = 0;
        ASSERT_TRUE (gate->try_post ("after-stale-before-expiry", [&] { ++handlers; }));
        scheduler.run_all ();
        EXPECT_EQ (1, handlers);
        const auto normal_expiry =
          lease_access::snapshot (runtime)->last_successful_lease->started_at
          + options.owner_lease_ttl + options.owner_lease_fencing_margin;
        // The default cutoff precedes normal expiry; retained execution lasts until T.
        if (episode->cutoff <= normal_expiry) {
            move_to (episode->cutoff);
            detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
            scheduler.run_all ();
            EXPECT_EQ (1, discarded);
        }
        move_to (normal_expiry);
        lease_access::heartbeat (runtime, clock_now.load ());
        ASSERT_TRUE (lease_access::snapshot (runtime)->token);
        EXPECT_NE (old_token.lease_generation,
                   lease_access::snapshot (runtime)->token->lease_generation);
        if (episode->discarded.load ()) {
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                       gate->execution_status ());
        } else
            EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        EXPECT_EQ (retained ? 0 : 1, discarded);
        // A renew of the newly claimed token cannot extend the old episode.
        advance (1ms);
        lease_access::heartbeat (runtime, clock_now.load ());
        if (episode->discarded.load ()) {
            EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
            EXPECT_EQ (serial_execution_queue_t::execution_status_t::ownership_lost,
                       gate->execution_status ());
        } else
            EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
        if (clock_now.load () < episode->cutoff)
            move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        scheduler.run_all ();
        EXPECT_EQ (1, discarded);
        EXPECT_TRUE (gate->closed ());
        EXPECT_FALSE (gate->try_post ("after-cutoff", [&] { ++handlers; }));
    }
}

TEST_P (StoreOutageRetention, CachedBoundSpotRoutePerActorDefault)
{
    registered_owner (false, false, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRoutePerActorRetention)
{
    registered_owner (false, true, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRouteSpotWideDefault)
{
    registered_owner (true, false, -1);
}

TEST_P (StoreOutageRetention, CachedBoundSpotRouteSpotWideRetention)
{
    registered_owner (true, true, -1);
}

TEST (ZLinkFrameworkLocationRuntime, ParentRecoveryNotifiesChildOnceForThousandEnqueues)
{
    for (bool shared : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto parent = std::make_shared<serial_execution_queue_t> (
          scheduler, serial_execution_queue_options_t{},
          serial_execution_queue_t::error_handler_t{},
          shared ? serial_lane_policy_t::spot_wide () : serial_lane_policy_t::per_actor_spot ());
        auto child = std::make_shared<serial_execution_queue_t> (scheduler);
        parent->set_execution_policy ([] { return true; });
        child->set_execution_policy ([] { return true; });
        child->attach_spot_gate (parent);
        const auto at = monotonic_time{std::chrono::hours (1000000)};
        parent->confirm_execution_ready (at);
        child->confirm_execution_ready (at);
        scheduler.run_all ();
        task_completion_source_t<serial_execution_queue_t::execution_operation_t> pending;
        unsigned calls = 0;
        for (unsigned i = 0; i != 1000; ++i)
            ASSERT_TRUE (child->try_post ("retained", [&] { ++calls; }));
        parent->set_execution_operation (pending.task ());
        EXPECT_FALSE (child->try_post ("new-while-paused", [] {}));
        scheduler.run_all ();
        EXPECT_EQ (0u, calls);
        pending.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
          {serial_execution_queue_t::execution_status_t::allowed, at + 1s}));
        // A PerActor Spot has its own control turn before the child mailbox drain.
        // Observe the child's one resume only after that parent control turn completes.
        if (!shared) {
            ASSERT_TRUE (scheduler.run_one ());
            EXPECT_EQ (0u, calls);
        }
        EXPECT_EQ (1u, scheduler.pending_work ());
        scheduler.run_all ();
        EXPECT_EQ (1000u, calls);
        EXPECT_EQ (0u, scheduler.pending_work ());
        child->cancel_pending ();
        parent->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST (ZLinkFrameworkLocationRuntime, RecoveryOperationPublishesAllowedLostAndReadFailure)
{
    using status_t = serial_execution_queue_t::execution_status_t;
    using operation_t = serial_execution_queue_t::execution_operation_t;
    for (auto terminal : {status_t::allowed, status_t::ownership_lost, status_t::stopped}) {
        test::manual_internal_scheduler_t scheduler;
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy ([] { return true; });
        const auto at = monotonic_time{std::chrono::hours (1000000)};
        gate->confirm_execution_ready (at);
        scheduler.run_all ();
        EXPECT_EQ (status_t::allowed, gate->execution_status ());
        task_completion_source_t<operation_t> superseded;
        gate->set_execution_operation (superseded.task ());
        task_completion_source_t<operation_t> pending;
        gate->set_execution_operation (pending.task ());
        superseded.complete (result_t<operation_t>::success ({status_t::allowed, at + 3s}));
        EXPECT_EQ (status_t::stopped, gate->execution_status ());
        EXPECT_FALSE (gate->execution_allowed ());
        // A paused object retains eligibility for the original episode's T latch.
        ASSERT_TRUE (gate->execution_ready_at ());
        EXPECT_LT (*gate->execution_ready_at (), at + 1s);
        if (terminal == status_t::stopped)
            pending.complete (result_t<operation_t>::failure (framework_error_kind_t::unavailable,
                                                              "injected exact read failure"));
        else
            pending.complete (result_t<operation_t>::success ({terminal, at + 2s}));
        scheduler.run_all ();
        EXPECT_EQ (terminal, gate->execution_status ());
        EXPECT_EQ (terminal == status_t::allowed, gate->execution_allowed ());
        EXPECT_EQ (0u, tests::count_allocations ([&] {
                       for (unsigned i = 0; i != 1000; ++i) {
                           (void) gate->execution_status ();
                           (void) gate->execution_allowed ();
                           (void) gate->execution_ready_at ();
                       }
                   }));
        gate->cancel_pending ();
        scheduler.run_all ();
    }
}

TEST (ZLinkFrameworkLocationRuntime, LocationRecoveryCannotRestoreInvalidatedReadyProof)
{
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy ([] { return true; });
    const auto at = monotonic_time{std::chrono::hours (1000000)};
    gate->confirm_execution_ready (at);
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> pending;
    gate->set_execution_operation (pending.task ());
    gate->invalidate_execution_ready ();
    pending.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, at + 1s}));
    scheduler.run_all ();
    EXPECT_FALSE (gate->execution_ready_at ());
    task_completion_source_t<serial_execution_queue_t::execution_operation_t> stale;
    gate->set_execution_operation (stale.task ());
    stale.complete (result_t<serial_execution_queue_t::execution_operation_t>::success (
      {serial_execution_queue_t::execution_status_t::allowed, at + 2s}));
    scheduler.run_all ();
    EXPECT_FALSE (gate->execution_ready_at ());
    gate->cancel_pending ();
    scheduler.run_all ();
}

TEST (ZLinkFrameworkLocationRuntime, PreservedRegistryDoesNotParticipateInNormalTurnAdmission)
{
    in_memory_location_repository_t repository;
    location_runtime_t runtime (repository);
    int clocks = 0;
    const auto at = monotonic_time{std::chrono::hours (1000000)};
    lease_access::clock (runtime, [&] {
        ++clocks;
        return at;
    });
    auto object_owner = std::make_shared<stateful::stateful_object_runtime_t> ();
    auto &objects = *object_owner;
    test::manual_internal_scheduler_t scheduler;
    std::vector<std::shared_ptr<serial_execution_queue_t>> gates;
    for (unsigned i = 0; i != 5000; ++i) {
        stateful::object_ref_t object{
          stateful::object_kind_t::actor, "preserved-" + std::to_string (i), 1, 1, "mesh", "node"};
        const auto created =
          objects.begin_reserved_object_async (object, "player", {}).result ().value ();
        ASSERT_EQ (stateful::stateful_error_t::none, created.error);
        ASSERT_EQ (stateful::stateful_error_t::none, objects.commit_create (created.attempt));
        auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
        gate->set_execution_policy (runtime.execution_policy ());
        objects.bind_execution_gate (object, gate).result ().value ();
        authority_snapshot_t snapshot;
        snapshot.store_version = "confirmed-" + std::to_string (i);
        snapshot.object_generation = snapshot.authority_owner_generation = 1;
        objects.confirm_location_ready (object, snapshot, gate->ready_confirmation (), at)
          .result ()
          .value ();
        gates.push_back (std::move (gate));
    }
    unsigned enumerations = 0, validations = 0;
    const auto recover = [&] {
        ++enumerations;
        return stateful::stateful_object_runtime_t::recover_location_execution (
          object_owner,
          [&] (authority_key_t, authority_snapshot_t snapshot,
               std::function<task_t<void> (authority_snapshot_t)>) {
              ++validations;
              EXPECT_TRUE (snapshot.store_version.starts_with ("confirmed-"));
              return task_t<serial_execution_queue_t::execution_operation_t> (
                result_t<serial_execution_queue_t::execution_operation_t>::success (
                  {serial_execution_queue_t::execution_status_t::allowed, at + 2s}));
          },
          at + 1s);
    };
    recover ().result ().value ();
    scheduler.run_all ();
    ASSERT_EQ (5000u, validations);
    ASSERT_EQ (1u, enumerations);
    clocks = 0;
    std::size_t allowed = 0;
    const auto allocations = tests::count_allocations ([&] {
        for (unsigned turn = 0; turn != 32; ++turn)
            for (const auto &gate : gates)
                allowed += gate->execution_allowed ();
    });
    EXPECT_EQ (160000u, allowed);
    EXPECT_EQ (0u, allocations);
    EXPECT_EQ (0, clocks);
    EXPECT_EQ (5000u, validations);
    EXPECT_EQ (1u, enumerations);
    recover ().result ().value ();
    scheduler.run_all ();
    EXPECT_EQ (2u, enumerations);
    EXPECT_EQ (5000u, validations); // The same episode cannot pause recovered objects again.
    // Positive control: the same counter must detect a deliberately allocated object.
    EXPECT_GT (tests::count_allocations ([] {
                   auto value = std::make_shared<int> (1);
                   EXPECT_EQ (1, *value);
               }),
               0u);
    for (auto &gate : gates)
        gate->cancel_pending ();
    scheduler.run_all ();
}

INSTANTIATE_TEST_SUITE_P (BothRepositories, StoreOutageRetention, ::testing::Bool ());

TEST (ZLinkFrameworkLocationRuntime, StoreOutageRetentionIsStartupCopyWithFalseDefault)
{
    location_options_t positional{7s};
    EXPECT_EQ (7s, positional.owner_lease_renew_interval);
    EXPECT_FALSE (positional.store_outage_retention);
    location_options_t defaults;
    EXPECT_FALSE (defaults.store_outage_retention);
    detail::validate_location_options (defaults);
    app_t app;
    auto &builder = app.add_zlink_framework ();
    EXPECT_FALSE (builder.location_options ().store_outage_retention);
    builder.configure_locations ().store_outage_retention = true;
    auto options = builder.location_options ();
    detail::validate_location_options (options);
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, options, "startup-owner");
    lease_access::clock (runtime, [] { return monotonic_time{std::chrono::hours (1000000)}; });
    lease_access::heartbeat (runtime, monotonic_time{std::chrono::hours (1000000)});
    options.store_outage_retention = false;
    builder.configure_locations ().store_outage_retention = false;
    EXPECT_TRUE (runtime.options ().store_outage_retention);
    static_assert (std::is_const_v<std::remove_reference_t<decltype (runtime.options ())>>);
}
} // namespace
} // namespace zlink::framework::runtime

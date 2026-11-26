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
#include <gtest/gtest-spi.h>

namespace zlink::framework::runtime
{
namespace
{
TEST (StoreOutageObservation, AuthorityKeyLookupRejectsUnknownKey)
{
    const std::array keys{actor_authority_key ("early").value, actor_authority_key ("late").value,
                          spot_authority_key ("registered-spot").value};
    for (std::size_t i = 0; i < keys.size (); ++i)
        EXPECT_EQ (1u << i, outage_authority_bit (keys[i], keys));
    EXPECT_NONFATAL_FAILURE (outage_authority_bit ("unexpected-key", keys),
                             "CAS submission: unknown authority key \"unexpected-key\"");
}

TEST (StoreOutageObservation, OperationWaitReportsFirstPendingQueueForEachLoss)
{
    using operation_t = serial_execution_queue_t::execution_operation_t;
    using status_t = serial_execution_queue_t::execution_status_t;
    for (const auto loss : {-34, -42}) {
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
        queue->cancel_pending (status_t::ownership_lost);
        scheduler.run_all ();
        task_completion_source_t<operation_t> pending;
        const auto completed = result_t<operation_t>::success ({status_t::allowed, {}});
        std::vector<outage_operation_t> operations;
        operations.push_back ({"early Actor", queue, task_t<operation_t> (completed)});
        operations.push_back ({"late Actor", queue, pending.task ()});
        operations.push_back ({"Spot", queue, pending.task ()});
        const char *stage = loss == -42 ? "renew recovery operation" : "recovery operation";
        const auto result = wait_outage_operations (operations, loss, stage, 0ms);
        EXPECT_FALSE (result);
        const std::string message = result.message ();
        EXPECT_NE (std::string::npos, message.find (std::string (stage) + ": late Actor"));
        EXPECT_NE (std::string::npos, message.find ("execution_status=ownership_lost"));
        EXPECT_NE (std::string::npos, message.find ("loss=" + std::to_string (loss)));
        pending.complete (completed);
        EXPECT_TRUE (wait_outage_operations (operations, loss, stage, 0ms));
    }
}

TEST_P (StoreOutageRetention, NewClaimRecoveryPreservesAfterPublishAndCasPerActor)
{
    registered_owner (false, true, -13);
}
TEST_P (StoreOutageRetention, NewClaimRecoveryPreservesAfterPublishAndCasSpotWide)
{
    registered_owner (true, true, -13);
}

TEST_P (StoreOutageRetention, NewClaimPublicationRejectsAllPerActor)
{
    registered_owner (false, true, -31);
}

TEST_P (StoreOutageRetention, NewClaimPublicationRejectsAllSpotWide)
{
    registered_owner (true, true, -31);
}

TEST_P (StoreOutageRetention, NewClaimCasRejectsOnlyItsObjectPerActor)
{
    registered_owner (false, true, -32);
}

TEST_P (StoreOutageRetention, NewClaimCasRejectsOnlyItsObjectSpotWide)
{
    registered_owner (true, true, -32);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineEqualsCutoffSubmitsPerActor)
{
    registered_owner (false, true, -33);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineEqualsCutoffSubmitsSpotWide)
{
    registered_owner (true, true, -33);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineExceedsCutoffDoesNotSubmitPerActor)
{
    registered_owner (false, true, -34);
}

TEST_P (StoreOutageRetention, NewClaimCasDeadlineExceedsCutoffDoesNotSubmitSpotWide)
{
    registered_owner (true, true, -34);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationBeforeCutoffPerActor)
{
    registered_owner (false, true, -35);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationBeforeCutoffSpotWide)
{
    registered_owner (true, true, -35);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAtCutoffPerActor)
{
    registered_owner (false, true, -36);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAtCutoffSpotWide)
{
    registered_owner (true, true, -36);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAfterCutoffPerActor)
{
    registered_owner (false, true, -37);
}

TEST_P (StoreOutageRetention, NewClaimConfirmationAfterCutoffSpotWide)
{
    registered_owner (true, true, -37);
}

TEST_P (StoreOutageRetention, NewClaimStopDoesNotSubmitCasPerActor)
{
    registered_owner (false, true, -38);
}

TEST_P (StoreOutageRetention, NewClaimStopDoesNotSubmitCasSpotWide)
{
    registered_owner (true, true, -38);
}

TEST_P (StoreOutageRetention, NewClaimUnconfirmedPublicationDoesNotResumePerActor)
{
    registered_owner (false, true, -39);
}

TEST_P (StoreOutageRetention, NewClaimUnconfirmedPublicationDoesNotResumeSpotWide)
{
    registered_owner (true, true, -39);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationWaitsForPublishPerActor)
{
    registered_owner (false, true, -40);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationWaitsForPublishSpotWide)
{
    registered_owner (true, true, -40);
}

TEST_P (StoreOutageRetention, NewClaimPublishRejectsWhileCasPendingPerActor)
{
    registered_owner (false, true, -41);
}

TEST_P (StoreOutageRetention, NewClaimPublishRejectsWhileCasPendingSpotWide)
{
    registered_owner (true, true, -41);
}

TEST_P (StoreOutageRetention, NewClaimCasFailureRetriesOnRenewPerActor)
{
    registered_owner (false, true, -42);
}

TEST_P (StoreOutageRetention, NewClaimCasFailureRetriesOnRenewSpotWide)
{
    registered_owner (true, true, -42);
}

TEST_P (StoreOutageRetention, NewClaimCasTimeoutRemainsFailedPerActor)
{
    registered_owner (false, true, -43);
}

TEST_P (StoreOutageRetention, NewClaimCasResponseBeforeTimeoutResumesPerActor)
{
    registered_owner (false, true, -40);
}

TEST_P (StoreOutageRetention, NewClaimCasTimeoutRemainsFailedSpotWide)
{
    registered_owner (true, true, -43);
}

TEST_P (StoreOutageRetention, NewClaimSuccessfulCasUpdatesPreservedFencePerActor)
{
    registered_owner (false, true, -44);
}

TEST_P (StoreOutageRetention, NewClaimSuccessfulCasUpdatesPreservedFenceSpotWide)
{
    registered_owner (true, true, -44);
}

TEST_P (StoreOutageRetention, NewClaimRenewDoesNotWaitForCasPerActor)
{
    registered_owner (false, true, -45);
}

TEST_P (StoreOutageRetention, NewClaimRenewDoesNotWaitForCasSpotWide)
{
    registered_owner (true, true, -45);
}

TEST_P (StoreOutageRetention, NewClaimConcurrentConfirmationsPerActor)
{
    registered_owner (false, true, -46);
}

TEST_P (StoreOutageRetention, NewClaimConcurrentConfirmationsSpotWide)
{
    registered_owner (true, true, -46);
}

TEST_P (StoreOutageRetention, NewClaimCombinesIndependentOwnersPerActor)
{
    registered_owner (false, true, -47);
}

TEST_P (StoreOutageRetention, NewClaimCombinesIndependentOwnersSpotWide)
{
    registered_owner (true, true, -47);
}

TEST_P (StoreOutageRetention, NewClaimHostSubmitsDescriptorsBeforeCasPerActor)
{
    registered_owner (false, true, -48);
}

TEST_P (StoreOutageRetention, NewClaimHostSubmitsDescriptorsBeforeCasSpotWide)
{
    registered_owner (true, true, -48);
}

TEST_P (StoreOutageRetention, NewClaimHostRejectionIsTerminalPerActor)
{
    registered_owner (false, true, -49);
}

TEST_P (StoreOutageRetention, NewClaimHostRejectionIsTerminalSpotWide)
{
    registered_owner (true, true, -49);
}

TEST_P (StoreOutageRetention, NewClaimSpotCasRejectionDiscardsLocalMembersPerActor)
{
    registered_owner (false, true, -50);
}

TEST_P (StoreOutageRetention, NewClaimSpotCasRejectionDiscardsLocalMembersSpotWide)
{
    registered_owner (true, true, -50);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationBeforeCutoffPerActor)
{
    registered_owner (false, true, -52);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationBeforeCutoffSpotWide)
{
    registered_owner (true, true, -52);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAtCutoffPerActor)
{
    registered_owner (false, true, -53);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAtCutoffSpotWide)
{
    registered_owner (true, true, -53);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAfterCutoffPerActor)
{
    registered_owner (false, true, -54);
}

TEST_P (StoreOutageRetention, NewClaimCasConfirmationAfterCutoffSpotWide)
{
    registered_owner (true, true, -54);
}

TEST_P (StoreOutageRetention, SameTokenExactMatchPerActor)
{
    registered_owner (false, true, -10);
}

TEST_P (StoreOutageRetention, SameTokenExactMatchSpotWide)
{
    registered_owner (true, true, -10);
}

TEST_P (StoreOutageRetention, SameTokenExactMissingIsObjectLocalPerActor)
{
    registered_owner (false, true, -11);
}

TEST_P (StoreOutageRetention, SameTokenExactMissingIsObjectLocalSpotWide)
{
    registered_owner (true, true, -11);
}

TEST_P (StoreOutageRetention, SameTokenMemberWaitsForSpotPerActor)
{
    registered_owner (false, true, -12);
}

TEST_P (StoreOutageRetention, SameTokenMemberWaitsForSpotSpotWide)
{
    registered_owner (true, true, -12);
}

TEST_P (StoreOutageRetention, RuntimeStopPreventsQueuedOwnerCleanupPerActor)
{
    registered_owner (false, true, -3);
}
TEST_P (StoreOutageRetention, RuntimeStopPreventsQueuedOwnerCleanupSpotWide)
{
    registered_owner (true, true, -3);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerPerActorRetention)
{
    registered_owner (false, true);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerSpotWideRetention)
{
    registered_owner (true, true);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerPerActorDefault)
{
    registered_owner (false, false);
}

TEST_P (StoreOutageRetention, RegisteredObjectOwnerSpotWideDefault)
{
    registered_owner (true, false);
}

TEST_P (StoreOutageRetention, ReviewMembershipCasRefreshesPreservedFence)
{
    registered_owner (false, true, -24);
}
TEST_P (StoreOutageRetention, ReviewMembershipCasDoesNotAcceptSupersedingFence)
{
    registered_owner (false, true, -25);
}

TEST_P (StoreOutageRetention, ReviewAdoptionExactFenceMatches)
{
    registered_owner (false, true, -20);
}
TEST_P (StoreOutageRetention, ReviewAdoptionExactFenceMismatchIsObjectLocal)
{
    registered_owner (false, true, -21);
}
TEST_P (StoreOutageRetention, ReviewDuplicateTerminalExactFenceMatches)
{
    registered_owner (false, true, -22);
}
TEST_P (StoreOutageRetention, ReviewDuplicateTerminalExactFenceMismatchIsObjectLocal)
{
    registered_owner (false, true, -23);
}

TEST_P (StoreOutageRetention, ReviewHeartbeatDoesNotWaitOrRestartExactRead)
{
    registered_owner (false, true, -14);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryPreserveCas)
{
    registered_owner (false, true, -55);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryExactRead)
{
    registered_owner (false, true, -56);
}
TEST_P (StoreOutageRetention, RuntimeStopDrainsRecoveryReadAfterCutoff)
{
    registered_owner (false, true, -57);
}

TEST_P (StoreOutageRetention, ReviewCutoffTerminatesReadBeforeEpisodeRelease)
{
    registered_owner (false, true, -15);
}
TEST_P (StoreOutageRetention, ReviewOnlyReadErrorsRetryOnNextRenew)
{
    registered_owner (false, true, -16);
}

TEST_P (StoreOutageRetention, ContendedOwnerSnapshotCannotDelayCutoff)
{
    for (bool retained : {false, true}) {
        options.store_outage_retention = retained;
        location_runtime_t runtime (store (), options,
                                    "contended-snapshot" + std::to_string (retained));
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
        auto node = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("contended");
        node->worker_executor = scheduler;
        detail::spot_node_runtime_t owner (node);
        location_lifecycle_t lifecycle (runtime);
        owner.bind_location_lifecycle (lifecycle);
        auto context = std::make_shared<detail::spot_context_state_t> ();
        context->node = node;
        context->lane_owner = node;
        context->serial_executor = scheduler;
        context->spot_id = spot_id_t ("contended-spot");
        context->mesh_name = "contended";
        context->node_rid = node_rid_t::from_string ("contended-node");
        node->spot_contexts_by_id.emplace ("contended-spot",
                                           detail::spot_context_access_t::create (context));
        const auto confirmed_at = clock_now.load ();
        auto writer = owner
                        .prepare_store_ready ({stateful::object_kind_t::user_spot, "contended-spot",
                                               1, 1, "contended", "contended-node"})
                        .result ()
                        .value ();
        ASSERT_TRUE (writer);
        auto gate = context->ensure_spot_serial_executor ()->spot_queue ();
        const auto cutoff = retained ? confirmed_at + 40s : confirmed_at + 25s;
        std::latch snapshot_entered (1), snapshot_released (1);
        auto source = std::async (std::launch::async, [&] {
            node->lane.run_sync ([&] {
                snapshot_entered.count_down ();
                snapshot_released.wait ();
            });
        });
        snapshot_entered.wait ();
        writer (confirmed_at);
        advance (5s);
        auto opening =
          std::async (std::launch::async, [&] { lease_access::communication_failure (runtime); });
        opening.get ();
        move_to (cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        int calls = 0;
        EXPECT_TRUE (gate->try_post ("while-snapshot-owner-is-contended", [&] { ++calls; }));
        scheduler->run_all ();
        snapshot_released.count_down ();
        source.get ();
        EXPECT_EQ (0, calls);
        const auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_TRUE (episode->discarded.load ());
        gate->cancel_pending ();
        scheduler->run_all ();
        node->spot_contexts_by_id.clear ();
        context->node.reset ();
    }
}

TEST_P (StoreOutageRetention, ReadyPolicyAndLateWriterRetainTheirOwnedPublication)
{
    auto runtime = std::make_unique<location_runtime_t> (store (), options, "policy-lifetime");
    lease_access::clock (*runtime, [&] { return clock_now.load (); });
    claim (*runtime);
    test::manual_internal_scheduler_t scheduler;
    auto gate = std::make_shared<serial_execution_queue_t> (scheduler);
    gate->set_execution_policy (runtime->execution_policy ());
    auto writer = gate->ready_confirmation ();
    ASSERT_TRUE (writer);
    writer (clock_now.load ());
    advance (5s);
    lease_access::communication_failure (*runtime);
    const auto cutoff = lease_access::snapshot (*runtime)->outage->cutoff;
    int calls = 0;
    ASSERT_TRUE (gate->try_post ("queued-before-location-owner-destruction", [&] { ++calls; }));
    runtime.reset ();
    move_to (cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (gate->try_post ("after-location-owner-destruction", [&] { ++calls; }));
    const auto rejected = gate->try_post_cancellable_async (
      "after-owner-cancellable", [] (auto complete) { complete ([] {}); }, [] {});
    ASSERT_FALSE (rejected);
    EXPECT_EQ (framework_error_kind_t::shutting_down, rejected.error_kind ());
    scheduler.run_all ();
    EXPECT_EQ (0, calls);
    gate->cancel_pending ();
    scheduler.run_all ();
    gate.reset ();
    writer (clock_now.load ());
}

TEST_P (StoreOutageRetention, CutoffAndScheduledLatchDoNotChangeAdmission)
{
    for (bool enabled : {false, true}) {
        options.store_outage_retention = enabled;
        location_runtime_t runtime (store (), options, enabled ? "enabled-owner" : "default-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        auto initial = lease_access::snapshot (runtime);
        advance (5s);
        lease_access::communication_failure (runtime);
        auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_EQ (clock_now.load (), episode->observed_at);
        EXPECT_EQ (enabled ? initial->last_successful_lease->started_at + 40s
                           : *initial->admission_deadline,
                   episode->cutoff);
        EXPECT_EQ (initial->admission_deadline,
                   lease_access::snapshot (runtime)->admission_deadline);
        move_to (episode->cutoff - 1ms);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_FALSE (episode->discarded.load ());
        EXPECT_EQ (lease_access::usable_at (runtime, clock_now.load ()),
                   clock_now.load () < *initial->admission_deadline);
        advance (1ms);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_TRUE (episode->discarded.load ());
        EXPECT_EQ (initial->admission_deadline,
                   lease_access::snapshot (runtime)->admission_deadline);
        EXPECT_TRUE (lease_access::outage_discarded (runtime));
    }
}

TEST_P (StoreOutageRetention, NewClaimAndLateRenewCannotChangeOriginalEpisode)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto original = lease_access::snapshot (runtime);
    advance (5s);
    lease_access::communication_failure (runtime);
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    auto token = *original->token;
    const auto store_now = *original->renewed_at;
    ++token.lease_generation;
    lease_access::accept (runtime, token, store_now + 30s, store_now, clock_now.load (), false);
    advance (1s);
    lease_access::accept (runtime, *original->token, store_now + 30s, store_now,
                          original->last_successful_lease->started_at + 1s, true);
    EXPECT_EQ (episode, lease_access::snapshot (runtime)->outage);
    EXPECT_EQ (original->last_successful_lease->started_at + 40s, episode->cutoff);
    move_to (episode->cutoff);
    // Even a recovery response delivered before the due callback cannot erase the latch.
    lease_access::accept (runtime, *original->token, store_now + 30s, store_now,
                          clock_now.load () - 1s, true, episode);
    EXPECT_TRUE (episode->discarded.load ());
    const auto retired = lease_access::snapshot (runtime);
    EXPECT_FALSE (retired->outage);
    ASSERT_EQ (1u, retired->discarded_outages.size ());
    EXPECT_EQ (episode, retired->discarded_outages.front ().lock ());
    EXPECT_EQ (original->last_successful_lease->started_at + 40s, episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ClaimAdvancesBaselineAndLateSuccessCannotMoveItBack)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    const auto initial = lease_access::snapshot (runtime);
    advance (5s);
    auto new_token = *initial->token;
    ++new_token.lease_generation;
    const auto claim_started_at = clock_now.load ();
    const auto store_now = *initial->renewed_at;
    lease_access::accept (runtime, new_token, store_now + 30s, store_now, claim_started_at, false);
    EXPECT_EQ (claim_started_at,
               lease_access::snapshot (runtime)->last_successful_lease->started_at);

    lease_access::accept (runtime, new_token, store_now + 30s, store_now,
                          initial->last_successful_lease->started_at, true);
    EXPECT_EQ (claim_started_at,
               lease_access::snapshot (runtime)->last_successful_lease->started_at);
    advance (1s);
    lease_access::communication_failure (runtime);
    const auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (claim_started_at + 40s, episode->cutoff);
}

TEST_P (StoreOutageRetention, TimelySameOwnerRenewClosesEpisodeAndCancelsLatch)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    lease_access::communication_failure (runtime);
    auto snapshot = lease_access::snapshot (runtime);
    auto episode = snapshot->outage;
    ASSERT_TRUE (episode);
    advance (1s);
    lease_access::accept (runtime, *snapshot->token, *snapshot->renewed_at + 30s,
                          *snapshot->renewed_at, clock_now.load (), true, episode);
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (episode->discarded.load ());
    // A second episode uses the last successful renewal request, not the initial claim.
    lease_access::communication_failure (runtime);
    EXPECT_EQ (snapshot->outage->observed_at + 1s + 40s,
               lease_access::snapshot (runtime)->outage->cutoff);
}

TEST_P (StoreOutageRetention, ProviderErrorsOpenEpisodeAndActualRenewClosesIt)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    provider.fail = memory.fail = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (clock_now.load (), episode->observed_at);
    provider.fail = memory.fail = false;
    advance (1s);
    lease_access::heartbeat (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_FALSE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ConfirmationReadDoesNotRenewRetentionBaseline)
{
    options.store_outage_retention = true;
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    auto initial = lease_access::snapshot (runtime);
    advance (5s);
    provider.fail_next_read = memory.fail_renew = true;
    lease_access::heartbeat (runtime, clock_now.load ());
    const auto snapshot = lease_access::snapshot (runtime);
    ASSERT_TRUE (snapshot->outage);
    EXPECT_TRUE (snapshot->healthy);
    EXPECT_EQ (initial->last_successful_lease->started_at,
               snapshot->last_successful_lease->started_at);
    EXPECT_EQ (initial->last_successful_lease->started_at + 40s, snapshot->outage->cutoff);
    move_to (snapshot->outage->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (snapshot->outage->discarded.load ());
}

TEST_P (StoreOutageRetention, RenewTimeoutOpensEpisode)
{
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    advance (5s);
    provider.hold_read = memory.hold_renew = true;
    lease_access::heartbeat (runtime, clock_now.load (), [&] {
        advance (options.owner_lease_renew_timeout);
        provider.resume.complete (result_t<void>::success ());
        memory.resume.complete (result_t<void>::success ());
    });
    auto episode = lease_access::snapshot (runtime)->outage;
    ASSERT_TRUE (episode);
    EXPECT_EQ (clock_now.load (), episode->observed_at);
    move_to (episode->cutoff);
    detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
    EXPECT_TRUE (episode->discarded.load ());
}

TEST_P (StoreOutageRetention, ReadErrorAndReadTimeoutOpenEpisode)
{
    for (bool timeout : {false, true}) {
        location_runtime_t runtime (store (), options, timeout ? "timeout-owner" : "error-owner");
        lease_access::clock (runtime, [&] { return clock_now.load (); });
        claim (runtime);
        advance (5s);
        if (timeout) {
            provider.hold_read = memory.hold_read = true;
        } else {
            provider.fail = memory.fail = true;
        }
        lease_access::confirm (runtime, clock_now.load (), [&] {
            if (timeout) {
                advance (options.owner_lease_renew_timeout);
                provider.resume.complete (result_t<void>::success ());
                memory.resume.complete (result_t<void>::success ());
            }
        });
        auto episode = lease_access::snapshot (runtime)->outage;
        ASSERT_TRUE (episode);
        EXPECT_EQ (clock_now.load (), episode->observed_at);
        provider.hold_read = memory.hold_read = false;
        provider.fail = memory.fail = false;
        move_to (episode->cutoff);
        detail::deadline_scheduler_test_access_t::advance_to (clock_now.load ());
        EXPECT_TRUE (episode->discarded.load ());
    }
}

TEST_P (StoreOutageRetention, ConflictMissingAndStaleDoNotOpenEpisode)
{
    location_runtime_t runtime (store (), options, "outage-owner");
    lease_access::clock (runtime, [&] { return clock_now.load (); });
    claim (runtime);
    location_runtime_t conflict (store (), options, runtime.owner_id ());
    lease_access::clock (conflict, [&] { return clock_now.load (); });
    lease_access::heartbeat (conflict, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (conflict)->outage);
    const auto token = *lease_access::snapshot (runtime)->token;
    store ().release_owner_lease (token).result ().value ();
    lease_access::confirm (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
    lease_access::heartbeat (runtime, clock_now.load ());
    EXPECT_FALSE (lease_access::snapshot (runtime)->outage);
}
} // namespace
} // namespace zlink::framework::runtime

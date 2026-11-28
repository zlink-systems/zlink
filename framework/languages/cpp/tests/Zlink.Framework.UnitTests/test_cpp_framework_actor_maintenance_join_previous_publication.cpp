/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_authority_fixture.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/authority_aggregate_relocation.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>

namespace zlink::framework::test::actor_maintenance_join
{
enum class previous_target_case_t
{
    absent,
    completes,
    deadline,
    captured_request,
    host
};
void join_during_previous_publication (previous_target_case_t scenario)
{
    namespace aggregate = fw::runtime::authority_aggregate_codec;
    auto evidence = std::make_shared<observations_t> ();
    if (scenario == previous_target_case_t::deadline
        || scenario == previous_target_case_t::captured_request)
        evidence->join_timeout = 1s;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    evidence->hold_join_admission = scenario == previous_target_case_t::captured_request;
    host_t source, destination;
    const release_callbacks_t release_callbacks{*evidence};
    configure (source, evidence, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    configure (destination, evidence, second_rid, store, relocations);
    destination.start ();
    ASSERT_TRUE (ready (destination));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto target_services = destination.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &target_services));
    const auto created = target_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (local_spot), room_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_EQ (created.value ().spot.node_rid ().value (), second_rid);

    auto &repository = services.get_required<fw::location_repository_t> ();
    const auto key = fw::runtime::actor_authority_key (actor_id);
    const auto original =
      std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
    const aggregate::json_t source_fence{
      {"nodeRid", zlink::routing_id_t::from ("previous-source").to_hex ()},
      {"nodeGeneration", "1"},
      {"ownerId", "previous-owner"},
      {"ownerLeaseGeneration", "1"}};
    auto coordinator = source_fence;
    coordinator["expectedStoreVersion"] = "previous-source-version";
    const auto previous_publication = aggregate::replace (
      original.aggregate,
      {{"relocationId", aggregate::wire_uuid (31, 7)},
       {"phase", scenario == previous_target_case_t::absent ? "activated" : "cleaning"},
       {"source", source_fence},
       {"coordinator", coordinator},
       {"target",
        {{"nodeRid",
          zlink::routing_id_t::from (std::string (original.allocation.target.node_rid.value ()))
            .to_hex ()},
         {"nodeGeneration", std::to_string (original.allocation.target.node_lifecycle_generation)},
         {"ownerId", original.owner.owner_id},
         {"ownerLeaseGeneration", std::to_string (original.owner.lease_generation)},
         {"attemptGeneration", "1"}}},
       {"sourceInventorySha256", std::string (64, 'a')},
       {"applicationVersion", "1"}});
    const auto published =
      std::get<fw::authority_stored_t> (
        repository
          .compare_exchange_authority (
            key, original.store_version,
            fw::authority_put_t{original.payload, {}, {}, previous_publication})
          .result ()
          .value ())
        .snapshot;

    struct target_terminal_guard_t
    {
        std::shared_ptr<fw::task_completion_source_t<void>> terminal;
        std::shared_ptr<fw::task_completion_source_t<void>> delivery;
        ~target_terminal_guard_t ()
        {
            if (terminal)
                terminal->complete (fw::result_t<void>::success ());
        }
    } previous;
    if (scenario != previous_target_case_t::absent) {
        const auto registration =
          fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ()).front ();
        const auto native =
          fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
        const auto object =
          native->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id);
        ASSERT_TRUE (object);
        auto [terminal, delivery] =
          fw::runtime::host::public_host_runtime_test_access_t::track_previous_target (
            *native, *object, scenario == previous_target_case_t::captured_request);
        previous.terminal = std::move (terminal);
        previous.delivery = delivery;
        if (delivery)
            evidence->after_move_deferred = [delivery] {
                delivery->complete (fw::result_t<void>::success ());
            };
    }

    auto &client = services.get_required<fw::actor_client_t> ();
    if (scenario == previous_target_case_t::host) {
        const auto relocated = source.app
                                 .relocate ({.mode = fw::relocation_mode_t::planned_maintenance,
                                             .deadline = wait_budget})
                                 .result ();
        ASSERT_TRUE (relocated);
        EXPECT_EQ (relocated.value ().outcome, fw::relocation_outcome_t::blocked);
        EXPECT_EQ (relocated.value ().reason, fw::relocation_reason_t::operation_in_progress);
    } else if (scenario == previous_target_case_t::completes) {
        const auto probe = client.request (fw::actor_id_t (actor_id), probe_t{71})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
        ASSERT_TRUE (probe);
        ASSERT_EQ (probe.value ().value, 71);
        ASSERT_TRUE (
          client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
        {
            std::unique_lock lock (evidence->mutex);
            EXPECT_FALSE (evidence->changed.wait_for (
              lock, 150ms, [&] { return !evidence->completions.empty (); }));
        }
        const auto release = repository
                               .compare_exchange_authority (
                                 key, published.store_version,
                                 fw::authority_put_t{original.payload, {}, {}, original.aggregate})
                               .result ();
        ASSERT_TRUE (release);
        ASSERT_TRUE (std::holds_alternative<fw::authority_stored_t> (release.value ()));
        previous.terminal->complete (fw::result_t<void>::success ());
        std::unique_lock lock (evidence->mutex);
        ASSERT_TRUE (evidence->changed.wait_for (lock, wait_budget,
                                                 [&] { return !evidence->completions.empty (); }));
        ASSERT_EQ (evidence->completions.size (), 1u);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence->completions.front ()));
        return;
    } else {
        ASSERT_TRUE (move_to (client, *evidence, local_spot, 1));
        std::lock_guard lock (evidence->mutex);
        ASSERT_EQ (evidence->completions.size (), 1u);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&evidence->completions.front ());
        ASSERT_NE (failed, nullptr);
        if (scenario == previous_target_case_t::captured_request) {
            ASSERT_TRUE (previous.delivery);
            EXPECT_TRUE (previous.delivery->task ().await_ready ());
            EXPECT_FALSE (evidence->join_admission_entered.task ().await_ready ());
        }
        EXPECT_EQ (failed->error_kind, scenario == previous_target_case_t::deadline
                                         ? fw::framework_error_kind_t::deadline_exceeded
                                         : fw::framework_error_kind_t::unavailable);
    }
    const auto after =
      std::get<fw::authority_snapshot_t> (repository.read_authority (key).result ().value ());
    EXPECT_EQ (after.store_version, published.store_version);
    EXPECT_EQ (after.payload, published.payload);
}

TEST (ActorMaintenanceJoin, ForeignActivatedPublicationWithoutLocalTargetIsUnavailable)
{
    join_during_previous_publication (previous_target_case_t::absent);
}

TEST (ActorMaintenanceJoin, ActorJoinWaitsForPreviousTargetPublicationAndRereadsAuthority)
{
    join_during_previous_publication (previous_target_case_t::completes);
}

TEST (ActorMaintenanceJoin, ActorJoinDeadlineExpiresWhilePreviousTargetIsCleaning)
{
    join_during_previous_publication (previous_target_case_t::deadline);
}

TEST (ActorMaintenanceJoin, SameActorDeferredJoinCannotWaitForItsCapturedRequest)
{
    join_during_previous_publication (previous_target_case_t::captured_request);
}

TEST (ActorMaintenanceJoin, HostRelocateRejectsPreviousTargetStillCleaning)
{
    join_during_previous_publication (previous_target_case_t::host);
}

TEST (ActorMaintenanceJoin, ReplayedSendStartsDeferredJoinAfterPreviousTargetTerminal)
{
    auto evidence = std::make_shared<observations_t> ();
    evidence->joined_payloads = {1};
    evidence->transfer_suffix = true;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target, final_target;
    release_callbacks_t release{*evidence};

    configure (source, evidence, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto source_services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (source_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (source_services, 0));

    configure (target, evidence, second_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (target_services, 0));

    configure (final_target, evidence, third_rid, store, relocations);
    final_target.start ();
    ASSERT_TRUE (ready (final_target));
    auto final_services = final_target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (final_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (suffix_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (
      peer_ready (source_services.get_required<fw::route_mesh_runtime_t> (), second_rid, 2));
    ASSERT_TRUE (
      peer_ready (target_services.get_required<fw::route_mesh_runtime_t> (), third_rid, 2));
    auto &sender = source_services.get_required<fw::actor_client_t> ();
    evidence->joined_sender = &sender;
    ASSERT_TRUE (move_to (sender, *evidence, local_spot, 2));
    std::lock_guard lock (evidence->mutex);
    ASSERT_EQ (evidence->completions.size (), 2u);
    EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence->completions[0]));
    EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence->completions[1]));
    EXPECT_EQ (evidence->completion_owners, (std::vector<std::string>{second_rid, third_rid}));
    EXPECT_EQ (evidence->handled_payloads, std::vector<int>{1});
}

} // namespace zlink::framework::test::actor_maintenance_join

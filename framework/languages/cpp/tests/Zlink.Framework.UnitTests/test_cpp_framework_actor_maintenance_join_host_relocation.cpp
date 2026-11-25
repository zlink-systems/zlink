/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
constexpr std::size_t status_observation_capacity = 32;

TEST (ActorMaintenanceJoin, HostShutdownClosesOccupiedUserSpot)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t host;
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    for (const auto &registration :
         fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())) {
        auto state = registration->spot_state;
        state->lane
          .run ([&] {
              const auto original = state->destroy_actor_registry;
              state->destroy_actor_registry = [original, &evidence] (const auto &actor) {
                  {
                      std::lock_guard lock (evidence.mutex);
                      ++evidence.destroy_callbacks;
                  }
                  return original (actor);
              };
          })
          .get ();
    }
    const auto created = services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (local_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    const auto explicit_close =
      services.get_required<fw::spot_manager_t> ().close (created.value ().spot).result ();
    ASSERT_TRUE (explicit_close);
    EXPECT_FALSE (explicit_close.value ());
    auto &repository = services.get_required<fw::location_repository_t> ();
    const auto before =
      repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (before));
    const auto mesh_name = std::get<fw::authority_snapshot_t> (before).allocation.target.mesh_name;
    ASSERT_EQ (1u, repository.list_mesh_nodes (mesh_name).result ().value ().items.size ());
    const auto stopped = host.app.shutdown (wait_budget).result ().value ();
    EXPECT_EQ (stopped.outcome, fw::termination_outcome_t::stopped);
    EXPECT_EQ (stopped.reason, fw::termination_reason_t::none);
    {
        std::lock_guard callback_lock (evidence.mutex);
        EXPECT_EQ (evidence.destroy_callbacks, 0);
    }
    const auto after =
      repository.read_authority (fw::runtime::actor_authority_key (actor_id)).result ().value ();
    EXPECT_TRUE (std::holds_alternative<fw::authority_missing_t> (after));
    EXPECT_TRUE (repository.list_mesh_nodes (mesh_name).result ().value ().items.empty ());
    std::lock_guard lock (evidence.mutex);
    ASSERT_EQ (evidence.closings.size (), 1);
    EXPECT_EQ (evidence.closings.front ().first, fw::spot_close_reason_t::host_shutdown);
    EXPECT_EQ (evidence.closings.front ().second, 0);
    EXPECT_EQ (evidence.destroyed.size (), 1);
    EXPECT_EQ (evidence.destroy_callbacks, 0);
}

TEST (ActorMaintenanceJoin, RelocationPublishesSafeToShutdown)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, evidence_owner, first_rid, store, relocations, source_follow_duration);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, evidence_owner, second_rid, store, relocations, source_follow_duration);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<fw::framework_runtime_status_t> statuses;
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto initial_callbacks = [&] {
        std::lock_guard lock (evidence.mutex);
        return std::pair{evidence.entry_joined, evidence.entry_left};
    }();
    auto observation = runtime.observe (status_observation_capacity, [&] (const auto &event) {
        {
            std::lock_guard lock (mutex);
            statuses.push_back (event.status);
        }
        changed.notify_all ();
    });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ()
        .value ();
    ASSERT_EQ (relocated.outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (relocated.reason);
    EXPECT_FALSE (runtime.status ().safe_to_shutdown);
    std::unique_lock lock (mutex);
    EXPECT_TRUE (changed.wait_for (lock, wait_budget, [&] {
        return std::ranges::any_of (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && status.safe_to_shutdown;
        });
    }));
    lock.unlock ();
    EXPECT_TRUE (runtime.status ().safe_to_shutdown);
    {
        std::lock_guard status_lock (mutex);
        const auto unsafe = std::ranges::find_if (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && !status.safe_to_shutdown;
        });
        const auto safe = std::ranges::find_if (statuses, [] (const auto &status) {
            return status.state == fw::framework_runtime_state_t::relocated
                   && status.safe_to_shutdown;
        });
        ASSERT_NE (unsafe, statuses.end ());
        ASSERT_NE (safe, statuses.end ());
        EXPECT_LT (unsafe->sequence, safe->sequence);
    }
    observation->close ();
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &client = target_services.get_required<fw::actor_client_t> ();
    const auto reply = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 2);
    {
        std::lock_guard evidence_lock (evidence.mutex);
        EXPECT_EQ (evidence.entry_joined, initial_callbacks.first);
        EXPECT_EQ (evidence.entry_left, initial_callbacks.second);
    }
}

TEST (ActorMaintenanceJoin, ApplicationSignaledSpotCaptureCompletes)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, evidence_owner, first_rid, store, relocations, source_follow_duration, 0,
               true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (target, evidence_owner, second_rid, store, relocations, source_follow_duration, 0,
               true);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
}

class ActorHostRelocation : public testing::TestWithParam<bool>
{
};

TEST_P (ActorHostRelocation, AfterRemoteActorJoin)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t entry, source, target;
    configure (entry, evidence_owner, first_rid, store, relocations, source_follow_duration, 0,
               true);
    entry.start ();
    ASSERT_TRUE (ready (entry));
    auto entry_services = entry.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (entry_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &entry_options = entry_services.get_required<fw::route_mesh_runtime_options_t> ();
    const auto entry_weight = entry_options.mesh (mesh_name).placement_weight ();
    ASSERT_TRUE (set_published_placement_weight (entry_services, 0));
    configure (source, evidence_owner, second_rid, store, relocations, source_follow_duration, 0,
               true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (entry_services.get_required<fw::route_mesh_runtime_t> ()));
    ASSERT_TRUE (
      move_to (entry_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    if (GetParam ()) {
        entry_options.mesh (mesh_name).placement_weight (entry_weight);
    } else {
        configure (target, evidence_owner, "join-third", store, relocations, source_follow_duration,
                   0, true);
        target.start ();
        ASSERT_TRUE (ready (target));
        ASSERT_TRUE (
          peer_ready (entry_services.get_required<fw::route_mesh_runtime_t> (), third_rid));
    }
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    ASSERT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    // Relocation flow §4.6: once the aggregate relocation commits, a request to the
    // relocated Actor reaches the target activation and replies.
    const auto reply = entry_services.get_required<fw::actor_client_t> ()
                         .request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
    ASSERT_TRUE (reply) << "kind=" << static_cast<int> (reply.error_kind ())
                        << " error=" << (reply.error () ? reply.error ()->what () : "none");
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (reply.value ().value, evidence.instances);
    }
    // The relocated User Spot stays addressable by its public Id.
    const auto spot = entry_services.get_required<fw::spot_manager_t> ()
                        .get_or_create (fw::spot_id_t (local_spot), room_type)
                        .timeout (wait_budget)
                        .async ()
                        .result ();
    ASSERT_TRUE (spot) << "kind=" << static_cast<int> (spot.error_kind ());
    EXPECT_EQ (spot.value ().state, fw::spot_create_state_t::existing);
}

// Relocation flow §4.6 and Session Actor binding §8.1: after the aggregate
// relocation commits, the Session keeps its physical connection and its bound
// Actor; the next packet on that connection reaches the target activation.
TEST_P (ActorHostRelocation, ExistingSessionRelaysAfterRelocation)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t entry, source, target, session;
    configure (entry, evidence_owner, first_rid, store, relocations, source_follow_duration, 0,
               true);
    entry.start ();
    ASSERT_TRUE (ready (entry));
    auto entry_services = entry.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (entry_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &entry_options = entry_services.get_required<fw::route_mesh_runtime_options_t> ();
    const auto entry_weight = entry_options.mesh (mesh_name).placement_weight ();
    ASSERT_TRUE (set_published_placement_weight (entry_services, 0));
    configure (source, evidence_owner, second_rid, store, relocations, source_follow_duration, 0,
               true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    configure_session (session, evidence_owner, store, relocations);
    session.start ();
    ASSERT_TRUE (ready (session));
    auto session_services = session.app.advanced ().services ().build_provider ();
    auto &session_routes = session_services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (session_routes, first_rid));
    ASSERT_TRUE (peer_ready (session_routes, second_rid));
    ASSERT_TRUE (
      peer_ready (entry_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    view_client_t client (session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint);
    ASSERT_TRUE (client.connect ());
    ASSERT_EQ (client.view (), std::optional<int>{1}) << "bound Entry Spot Actor did not reply";
    ASSERT_TRUE (
      move_to (entry_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    int joined_instance = 0;
    {
        std::lock_guard lock (evidence.mutex);
        joined_instance = evidence.instances;
    }
    ASSERT_EQ (client.view (), std::optional<int>{joined_instance})
      << "joined Actor did not reply on the existing Session";
    if (GetParam ()) {
        entry_options.mesh (mesh_name).placement_weight (entry_weight);
    } else {
        configure (target, evidence_owner, "join-third", store, relocations, source_follow_duration,
                   0, true);
        target.start ();
        ASSERT_TRUE (ready (target));
        ASSERT_TRUE (peer_ready (session_routes, third_rid));
    }
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto route = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (route.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    ASSERT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    int relocated_instance = 0;
    {
        std::lock_guard lock (evidence.mutex);
        relocated_instance = evidence.instances;
    }
    ASSERT_GT (relocated_instance, joined_instance);
    EXPECT_EQ (client.view (), std::optional<int>{relocated_instance})
      << "relocated Actor did not reply on the existing Session";
}

INSTANTIATE_TEST_SUITE_P (FreshAndPreviousOwner, ActorHostRelocation, testing::Bool ());
} // namespace zlink::framework::test::actor_maintenance_join

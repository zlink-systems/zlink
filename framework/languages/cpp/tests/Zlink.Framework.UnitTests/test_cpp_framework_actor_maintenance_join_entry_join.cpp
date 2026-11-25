/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
class creation_store_t final : public fw::location_store_t
{
  public:
    std::atomic<bool> fail_next_write{false};
    std::atomic<int> failed_writes{0};
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        const auto terminal_write = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<fw::store_put_t> (&mutation);
              return put && put->key.value.starts_with (std::string ("creation-terminal") + '\0');
          });
        if (terminal_write && fail_next_write.exchange (false)) {
            ++failed_writes;
            throw fw::framework_exception_t (fw::framework_error_kind_t::internal_failure,
                                             "creation terminal publication failed");
        }
        return inner.write (std::move (request));
    }

  private:
    fw::runtime::in_memory_location_store_t inner;
};
class ActorCreationHandoff : public testing::TestWithParam<creation_case_t>
{
};
TEST_P (ActorCreationHandoff, TerminalReleasesMoveWithoutJoinLifecycle)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.creation_case = GetParam ();
    auto store = std::make_shared<creation_store_t> ();
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    if (GetParam () == creation_case_t::publication_failed)
        store->fail_next_write = true;
    auto services = host.app.advanced ().services ().build_provider ();
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    if (GetParam () == creation_case_t::accepted) {
        ASSERT_TRUE (created);
        const auto first = services.get_required<fw::actor_client_t> ()
                             .request (fw::actor_id_t (actor_id), probe_t{17})
                             .async<probe_t> ()
                             .result ();
        ASSERT_TRUE (first);
        EXPECT_EQ (first.value ().value, 17);
    } else {
        EXPECT_FALSE (created);
    }
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    ASSERT_TRUE (state->worker_executor);
    state->worker_executor->drain ();
    EXPECT_FALSE (
      state->actor_transfer_coordinator->phase (std::string (actor_type) + ":" + actor_id));
    EXPECT_EQ (store->failed_writes.load (),
               GetParam () == creation_case_t::publication_failed ? 1 : 0);
    std::lock_guard lock (evidence.mutex);
    EXPECT_EQ (evidence.creation_callbacks, 1);
    EXPECT_EQ (evidence.entry_joined, 0);
    EXPECT_TRUE (evidence.completions.empty ());
}
INSTANTIATE_TEST_SUITE_P (AcceptedRejectedFailedPublication,
                          ActorCreationHandoff,
                          testing::Values (creation_case_t::accepted,
                                           creation_case_t::rejected,
                                           creation_case_t::failed,
                                           creation_case_t::publication_failed));
TEST (ActorMaintenanceJoin, PublicEntryJoinCompletionPrecedesFirstCommand)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, evidence, local_spot, 1));
    ASSERT_TRUE (move_to (client, evidence, entry_target, 2));
    const auto first =
      client.request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ().result ();
    ASSERT_TRUE (first);
    EXPECT_EQ (first.value ().value, 17);
    std::lock_guard lock (evidence.mutex);
    EXPECT_EQ (evidence.creation_callbacks, 1);
    EXPECT_EQ (evidence.entry_joined, 1);
    ASSERT_EQ (evidence.completions.size (), 2u);
    const auto *accepted = std::get_if<fw::actor_join_accepted_t> (&evidence.completions.back ());
    ASSERT_NE (accepted, nullptr);
    EXPECT_TRUE (accepted->operation_id_high != 0 || accepted->operation_id_low != 0);
}

TEST (ActorMaintenanceJoin, RemoteEntryJoinMovesExistingActorToCompatibleNode)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (services, 0));
    configure (target, evidence_owner, second_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, entry_target, 1));
    std::unique_lock lock (evidence.mutex);
    ASSERT_TRUE (
      evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.entry_left == 1; }));
    ASSERT_EQ (evidence.completions.size (), 1u);
    const auto *accepted = std::get_if<fw::actor_join_accepted_t> (&evidence.completions.back ());
    ASSERT_NE (accepted, nullptr);
    EXPECT_EQ (accepted->actor.node_rid ().value (), second_rid);
    EXPECT_EQ (evidence.creation_callbacks, 1);
    EXPECT_EQ (evidence.entry_joined, 1);
    EXPECT_EQ (evidence.entry_left, 1);
}
} // namespace zlink::framework::test::actor_maintenance_join

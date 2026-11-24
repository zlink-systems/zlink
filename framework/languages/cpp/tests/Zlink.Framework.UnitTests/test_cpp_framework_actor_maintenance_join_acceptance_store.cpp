/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <tuple>

namespace zlink::framework::test::actor_maintenance_join
{
class NativeIntakePendingStore : public testing::TestWithParam<std::tuple<bool, int>>
{
};
TEST_P (NativeIntakePendingStore, ProductionResolverUsesNoStoreDuringAcceptance)
{
    const auto [remote, failure_stage] = GetParam ();
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<intake_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host, peer;
    release_callbacks_t callbacks{evidence};
    configure (host, evidence_owner, first_rid, store, relocations);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    constexpr auto independent_actor = "intake-independent";
    ASSERT_TRUE (
      actors.get_or_create (fw::actor_id_t (independent_actor), actor_type).async ().result ());
    configure (peer, evidence_owner, second_rid, store, relocations);
    peer.start ();
    ASSERT_TRUE (ready (peer));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto peer_services = peer.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (peer_services.get_required<fw::route_mesh_runtime_t> (), first_rid));
    auto &local = services.get_required<fw::actor_client_t> ();
    auto &native = peer_services.get_required<fw::actor_client_t> ();
    auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    if (failure_stage >= 0)
        store->release (failure_stage, true);
    store->stage.store (1);
    auto pending =
      (remote ? native : local).request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ();
    auto terminals = std::make_shared<std::atomic<int>> (0);
    auto terminal_seen = std::make_shared<std::promise<void>> ();
    auto terminal_ready = terminal_seen->get_future ();
    fw::detail::observe_task_completion (pending, [terminals, terminal_seen] (const auto &) {
        if (++*terminals == 1) {
            terminal_seen->set_value ();
        }
    });
    store->release_all ();
    const auto result = pending.result ();
    EXPECT_TRUE (result) << (result.error () ? result.error ()->what () : "request failed");
    if (result)
        EXPECT_EQ (result.value ().value, 17);
    for (int index = 0; index != 3; ++index)
        EXPECT_FALSE (store->reached[index].load ()) << "unexpected Store read=" << index;
    store->release_all ();
    terminal_ready.get ();
    EXPECT_EQ (terminals->load (), 1);
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (std::count (evidence.intake_probe_values.begin (),
                               evidence.intake_probe_values.end (), 17),
                   1);
    }
    constexpr int following_payload = 117;
    const auto following = (remote ? native : local)
                             .request (fw::actor_id_t (actor_id), probe_t{following_payload})
                             .async<probe_t> ()
                             .result ();
    EXPECT_TRUE (following);
    if (following)
        EXPECT_EQ (following.value ().value, following_payload);
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (std::count (evidence.intake_probe_values.begin (),
                               evidence.intake_probe_values.end (), following_payload),
                   1);
    }

    auto jobs = registration->application_jobs;
    const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    host.app.request_stop ();
    peer.app.request_stop ();
    host.thread.join ();
    peer.thread.join ();
    EXPECT_EQ (jobs->snapshot ().permits_in_use, 0u);
    EXPECT_EQ (jobs->snapshot ().capacity_waiters, 0u);
    EXPECT_EQ (node->transport ().mailbox ().pending_messages (
                 fw::runtime::mesh::service_mailbox_domain_t::application),
               0u);
    EXPECT_EQ (node->transport ().mailbox ().pending_bytes (
                 fw::runtime::mesh::service_mailbox_domain_t::application),
               0u);
}
INSTANTIATE_TEST_SUITE_P (LocalAndNative,
                          NativeIntakePendingStore,
                          testing::Combine (testing::Bool (), testing::Values (-1, 0, 1, 2)));
} // namespace zlink::framework::test::actor_maintenance_join

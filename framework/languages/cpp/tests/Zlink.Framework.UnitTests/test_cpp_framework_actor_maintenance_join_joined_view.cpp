/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
void joined_first_view_reaches_client (bool remote)
{
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    configure (source, evidence, first_rid, store, relocations);
    source.app.advanced ().services ().add_factory<relay_session_t> (
      [evidence] (fw::service_provider_t &provider) {
          return std::make_unique<relay_session_t> (provider.get_required<fw::actor_manager_t> (),
                                                    evidence);
      },
      fw::service_lifetime_t::scoped);
    source.app.add_zlink_framework ()
      .add_stream_node (stream_name)
      .bind ("tcp://127.0.0.1:0")
      .register_session<relay_session_t> ();
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    view_client_t connection (services.get_required<fw::framework_runtime_t> ()
                                .listener_status (fw::listener_kind_t::stream, stream_name)
                                .endpoint);
    ASSERT_TRUE (connection.connect ());
    ASSERT_TRUE (connection.view ().has_value ());

    std::optional<fw::service_provider_t> target_services;
    auto *destination = &services;
    if (remote) {
        configure (target, evidence, second_rid, store, relocations);
        target.start ();
        ASSERT_TRUE (ready (target));
        target_services.emplace (target.app.advanced ().services ().build_provider ());
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
        ASSERT_TRUE (
          peer_ready (target_services->get_required<fw::route_mesh_runtime_t> (), first_rid));
        ASSERT_TRUE (set_published_placement_weight (services, 0, &*target_services));
        destination = &*target_services;
    }
    ASSERT_TRUE (destination->get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());

    auto submitted = std::make_shared<fw::task_completion_source_t<void>> ();
    auto release = std::make_shared<fw::task_completion_source_t<void>> ();
    auto *sender = &destination->get_required<fw::actor_client_t> ();
    evidence->joined_view = [sender, submitted, release] (fw::actor_id_t id) -> fw::task_t<void> {
        // The same public path as Table::publish -> deliver -> bound_session.send.
        co_await sender->send (std::move (id), view_request_t{}).async ();
        submitted->complete (fw::result_t<void>::success ());
        co_await release->task ();
    };
    struct release_joined_t
    {
        std::shared_ptr<fw::task_completion_source_t<void>> release;
        ~release_joined_t () { release->complete (fw::result_t<void>::success ()); }
    } release_joined{release};
    ASSERT_TRUE (services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t (actor_id), move_t{local_spot})
                   .async ()
                   .result ());
    const auto admitted = submitted->task ().result_for (wait_budget);
    ASSERT_TRUE (admitted.has_value ());
    ASSERT_TRUE (*admitted);
    // Membership §4: payload dispatch waits for the joined callback and completion.
    EXPECT_EQ (connection.received_views (), 1u);
    {
        std::lock_guard lock (evidence->mutex);
        EXPECT_TRUE (evidence->completions.empty ());
    }
    release->complete (fw::result_t<void>::success ());
    EXPECT_TRUE (connection.wait_view (1).has_value ());
    {
        std::lock_guard lock (evidence->mutex);
        ASSERT_EQ (evidence->completions.size (), 1u);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence->completions.front ()));
        EXPECT_EQ (evidence->completion_owners.front (), remote ? second_rid : first_rid);
        EXPECT_EQ (evidence->completion_spots.front (), fw::spot_id_t (local_spot));
    }
}

TEST (ActorMaintenanceJoin, JoinedFirstViewReachesClientLocal)
{
    joined_first_view_reaches_client (false);
}
TEST (ActorMaintenanceJoin, JoinedFirstViewReachesClientRemote)
{
    joined_first_view_reaches_client (true);
}
} // namespace zlink::framework::test::actor_maintenance_join

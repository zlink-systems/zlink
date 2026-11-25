/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_actors.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
class consecutive_join_state_t final : public fw::actor_relocation_adapter_t<actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (actor_t &actor, std::stop_token) override
    {
        const auto observations = actor.observations;
        if (actor.context ().actor_id ().value () == actor_id
            && actor.context ().actor_ref ().node_rid ().value () == first_rid) {
            {
                std::lock_guard lock (observations->mutex);
                EXPECT_TRUE (observations->first_admission_accepted);
                observations->first_capture_pending = true;
            }
            observations->changed.notify_all ();
            co_await observations->release_completion.task ();
            {
                std::lock_guard lock (observations->mutex);
                observations->first_capture_pending = false;
            }
        }
        co_return std::vector<std::byte>{std::byte{1}};
    }
    fw::task_t<void> restore (actor_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};

TEST (ActorMaintenanceJoin, HandlerInvocationCallbackStopsBothPhasesWithTracingOff)
{
    auto evidence = std::make_shared<observations_t> ();
    evidence->joined_payloads = {17};
    host_t host;
    configure (host, evidence, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    struct boundary_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        int entered = 0;
        int returned = 0;
        int released = 0;
    };
    auto boundary = std::make_shared<boundary_t> ();
    std::atomic<int> telemetry_events{0};
    auto &dispatch = host.app.add_zlink_framework ().configure_dispatch ();
    dispatch.message_flow (fw::message_flow_log_mode_t::off);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch, [&] (const auto &) { ++telemetry_events; });
    const auto node = fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
                        .front ()
                        ->spot_state;
    node->handler_invocation_for_tests = [boundary] (fw::detail::handler_invocation_phase_t phase,
                                                     std::string_view packet, std::string_view spot,
                                                     std::string_view actor) {
        if (packet != probe_t::packet_name || actor != actor_id)
            return;
        EXPECT_FALSE (spot.empty ());
        std::unique_lock lock (boundary->mutex);
        const int step = phase == fw::detail::handler_invocation_phase_t::before_admission ? 1 : 2;
        boundary->entered = step;
        boundary->changed.notify_all ();
        if (!boundary->changed.wait_for (lock, wait_budget,
                                         [&] { return boundary->released >= step; })) {
            ADD_FAILURE () << "handler invocation callback release timed out";
            return;
        }
        ++boundary->returned;
    };
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t (actor_id), move_t{local_spot})
                   .async ()
                   .result ());
    {
        std::unique_lock lock (evidence->mutex);
        ASSERT_TRUE (evidence->changed.wait_for (lock, wait_budget,
                                                 [&] { return !evidence->completions.empty (); }));
    }
    auto request = std::async (std::launch::async, [&] {
        return services.get_required<fw::actor_client_t> ()
          .request (fw::actor_id_t (actor_id), probe_t{17})
          .async<probe_t> ()
          .result ();
    });
    {
        std::unique_lock lock (boundary->mutex);
        EXPECT_TRUE (
          boundary->changed.wait_for (lock, wait_budget, [&] { return boundary->entered == 1; }));
        EXPECT_EQ (boundary->returned, 0);
        {
            std::lock_guard evidence_lock (evidence->mutex);
            EXPECT_TRUE (evidence->handled_payloads.empty ());
        }
        boundary->released = 1;
        boundary->changed.notify_all ();
        EXPECT_TRUE (
          boundary->changed.wait_for (lock, wait_budget, [&] { return boundary->entered == 2; }));
        EXPECT_EQ (boundary->returned, 1);
        boundary->released = 2;
        boundary->changed.notify_all ();
    }
    const auto reply = request.get ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 17);
    EXPECT_EQ (telemetry_events.load (), 0);
    host.app.request_stop ();
    host.thread.join ();
    node->handler_invocation_for_tests = {};
}

TEST (ActorMaintenanceJoin, ConsecutiveDeferredJoinsDoNotHoldTargetSpotDuringCapture)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.consecutive_join_probe = true;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, other_source, target;
    release_callbacks_t release{evidence};
    // Target admission accepts P1 before its source Capture waits. The
    // target Host and its Spot callback gate remain available for P2.
    const auto configure_public = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        mesh.add_entry_spot<entry_t> (
          [evidence = evidence_owner] (fw::entry_spot_context_t context) {
              return std::make_shared<entry_t> (std::move (context), evidence);
          });
        mesh
          .add_spot_factory<room_t> (
            room_type,
            [evidence = evidence_owner] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context), evidence);
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<actor_t, factory_t> (
            actor_type, std::make_shared<factory_t> (evidence_owner), [] (auto &factory) {
                factory.template preserve_state_with<consecutive_join_state_t> ();
            });
    };
    configure_public (source, first_rid);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    ASSERT_TRUE (set_published_placement_weight (services, 0));
    // Source Capture may occupy P1's Host. P2 has an independent source
    // so this test isolates the target Spot FIFO.
    configure_public (other_source, third_rid);
    other_source.start ();
    ASSERT_TRUE (ready (other_source));
    auto other_services = other_source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (other_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t ("second-member"), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (other_services, 0));
    configure_public (target, second_rid);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (peer_ready (other_services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.first_capture_pending; }));
    }
    ASSERT_TRUE (other_services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t ("second-member"), move_t{current_spot})
                   .async ()
                   .result ());
    {
        std::unique_lock lock (evidence.mutex);
        // The watchdog bounds failure cleanup; callback signals define ordering.
        const bool admitted = evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.second_admission_during_capture; });
        EXPECT_TRUE (admitted);
        EXPECT_TRUE (evidence.first_capture_pending);
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () == 2; }));
        for (const auto &completion : evidence.completions) {
            const auto *failed = std::get_if<fw::actor_join_failed_t> (&completion);
            EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (completion))
              << "completion_variant=" << completion.index ()
              << " failed_kind=" << (failed ? static_cast<int> (failed->error_kind) : -1);
        }
    }
}
} // namespace zlink::framework::test::actor_maintenance_join

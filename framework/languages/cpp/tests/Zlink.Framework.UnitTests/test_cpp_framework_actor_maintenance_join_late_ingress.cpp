/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "../support/scope_exit.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorMaintenanceJoin, LateRequestRequiresSuccessfulJoinCompletion)
{
    for (const bool fail_completion : {false, true}) {
        SCOPED_TRACE (fail_completion ? "failed completion" : "successful completion");
        constexpr int late_payload = 17;
        auto dropped_one_way = std::make_shared<int> (0);
        auto evidence = std::make_shared<observations_t> ();
        evidence->fail_completion = fail_completion;
        // Enable the room fixture's handler observations without sending a lifecycle batch.
        evidence->joined_payloads = {late_payload};
        host_t host;
        configure (host, evidence, fail_completion ? second_rid : first_rid,
                   std::make_shared<fw::runtime::in_memory_location_store_t> (),
                   std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        host.app.advanced ().services ().add_factory<relay_session_t> (
          [evidence] (fw::service_provider_t &provider) {
              return std::make_unique<relay_session_t> (
                provider.get_required<fw::actor_manager_t> (), evidence);
          },
          fw::service_lifetime_t::scoped);
        auto &options = host.app.add_zlink_framework ();
        options.add_stream_node (stream_name)
          .bind ("tcp://127.0.0.1:0")
          .register_session<relay_session_t> ();
        fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options.configure_dispatch (),
          [evidence, dropped_one_way] (const fw::message_dispatch_error_event_t &event) {
              if (event.actor_id != actor_id || event.packet_name != probe_t::packet_name
                  || event.message_kind != fw::dispatch_message_kind_t::actor_send
                  || event.action != fw::dispatch_error_action_t::drop)
                  return;
              {
                  std::lock_guard lock (evidence->mutex);
                  ++*dropped_one_way;
              }
              evidence->changed.notify_all ();
          });
        host.start ();
        ASSERT_TRUE (ready (host));
        auto services = host.app.advanced ().services ().build_provider ();
        const auto id = fw::actor_id_t (actor_id);
        ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                       .get_or_create (fw::spot_id_t (local_spot), room_type)
                       .timeout (wait_budget)
                       .async ()
                       .result ());
        ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                       .get_or_create (id, actor_type)
                       .timeout (wait_budget)
                       .async ()
                       .result ());
        auto &client = services.get_required<fw::actor_client_t> ();
        ASSERT_TRUE (client.request (id, probe_t{instance_probe})
                       .timeout (wait_budget)
                       .async<probe_t> ()
                       .result ());
        zlink::stream_connector::connector_options_t stream_options;
        stream_options.endpoint = services.get_required<fw::framework_runtime_t> ()
                                    .listener_status (fw::listener_kind_t::stream, stream_name)
                                    .endpoint;
        stream_options.reconnect.enabled = false;
        auto stream = zlink::stream_connector::connector_factory_t::create (stream_options);
        ASSERT_TRUE (stream.connect ());
        stream.send (record_t{instance_probe}).submit ();
        {
            std::unique_lock lock (evidence->mutex);
            ASSERT_TRUE (evidence->changed.wait_for (lock, wait_budget, [&] {
                return evidence->relay_completed && !evidence->recorded.empty ();
            }));
        }
        const auto state =
          fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
            .front ()
            ->spot_state;
        const auto key = std::string (actor_type) + ":" + actor_id;
        const auto source_queue =
          state->lane
            .run ([&] {
                const auto &source = state->actor_spot_ids.at (key);
                const auto context = fw::detail::spot_context_access_t::state (
                  state->spot_contexts_by_id.at (std::string (source)));
                return context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
            })
            .get ();
        ASSERT_TRUE (source_queue);
        ASSERT_TRUE (move_to (client, *evidence, local_spot, 1));
        // The completion observation is recorded inside the callback. Drain the
        // source Join barrier so the request starts after completion settlement.
        source_queue->drain ();

        const auto completion_before_request = [&] {
            std::lock_guard lock (evidence->mutex);
            return evidence->completions.front ();
        }();
        const auto *committed = std::get_if<fw::actor_join_accepted_t> (&completion_before_request);
        ASSERT_NE (committed, nullptr);
        // Release the failed Join seal before graceful host teardown.
        scope_exit_t release_join_seal ([&] {
            if (fail_completion)
                fw::detail::spot_node_runtime_t (state).cancel_local_actor_handoff (
                  committed->actor);
        });
        const auto started = std::chrono::steady_clock::now ();
        const auto reply = client.request (id, probe_t{late_payload})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
        const auto elapsed = std::chrono::steady_clock::now () - started;
        if (fail_completion) {
            EXPECT_EQ (reply.error_kind (), fw::framework_error_kind_t::unavailable);
            EXPECT_LT (elapsed, std::chrono::seconds (1));
            RecordProperty (
              "failed_completion_request_ms",
              static_cast<int> (
                std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count ()));
        } else {
            ASSERT_TRUE (reply);
            EXPECT_EQ (reply.value ().value, late_payload);
        }

        const auto wait_for_one_way = [&] (int count) {
            std::unique_lock lock (evidence->mutex);
            return evidence->changed.wait_for (lock, wait_budget, [&] {
                return fail_completion ? *dropped_one_way >= count
                                       : evidence->handled_payloads.size ()
                                           >= static_cast<std::size_t> (count + 1);
            });
        };
        ASSERT_TRUE (client.send (id, probe_t{late_payload + 1}).async ().result ());
        EXPECT_TRUE (wait_for_one_way (1)) << "ActorClient send";
        stream.send (probe_t{late_payload + 2}).submit ();
        EXPECT_TRUE (wait_for_one_way (2)) << "one-way Session relay";
        {
            std::lock_guard lock (evidence->mutex);
            EXPECT_EQ (*dropped_one_way, fail_completion ? 2 : 0);
            RecordProperty (fail_completion ? "failed_completion_handler_calls"
                                            : "successful_completion_handler_calls",
                            static_cast<int> (evidence->handled_payloads.size ()));
            if (fail_completion)
                EXPECT_TRUE (evidence->handled_payloads.empty ());
            else
                EXPECT_EQ (evidence->handled_payloads,
                           (std::vector<int>{late_payload, late_payload + 1, late_payload + 2}));
            EXPECT_EQ (evidence->intake_probe_values, std::vector<int>{instance_probe});
            ASSERT_EQ (evidence->completions.size (), 1u);
            const auto *accepted =
              std::get_if<fw::actor_join_accepted_t> (&evidence->completions.front ());
            ASSERT_NE (accepted, nullptr);
            EXPECT_EQ (accepted->actor.actor_id (), id);
            EXPECT_EQ (evidence->completion_spots.front (), fw::spot_id_t (local_spot));
        }
        EXPECT_EQ (state->lane.run ([&] { return state->actor_spot_ids.at (key); }).get (),
                   fw::spot_id_t (local_spot));
    }
}
} // namespace zlink::framework::test::actor_maintenance_join

/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/scope_exit.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorMaintenanceJoin, LateRequestRequiresSuccessfulJoinCompletion)
{
    for (const bool fail_completion : {false, true}) {
        SCOPED_TRACE (fail_completion ? "failed completion" : "successful completion");
        constexpr int late_payload = 17;
        auto evidence = std::make_shared<observations_t> ();
        evidence->fail_completion = fail_completion;
        // Enable the room fixture's handler observations without sending a lifecycle batch.
        evidence->joined_payloads = {late_payload};
        host_t host;
        configure (host, evidence, fail_completion ? second_rid : first_rid,
                   std::make_shared<fw::runtime::in_memory_location_store_t> (),
                   std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
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

        const auto [completion_before_request, admitted_before_request] = [&] {
            std::lock_guard lock (evidence->mutex);
            return std::pair{evidence->completions.front (), evidence->ordinary_posts};
        }();
        const auto *committed = std::get_if<fw::actor_join_accepted_t> (&completion_before_request);
        ASSERT_NE (committed, nullptr);
        // Release the retained request after observing the seal so host teardown can close the Spot.
        scope_exit_t discard_retained_request ([&] {
            if (fail_completion)
                fw::detail::spot_node_runtime_t (state).cancel_local_actor_handoff (
                  committed->actor);
        });
        const auto reply = client.request (id, probe_t{late_payload})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
        {
            std::unique_lock lock (evidence->mutex);
            EXPECT_TRUE (evidence->changed.wait_for (lock, wait_budget, [&] {
                return evidence->ordinary_posts > admitted_before_request;
            }));
            RecordProperty (fail_completion ? "failed_completion_handler_calls"
                                            : "successful_completion_handler_calls",
                            static_cast<int> (evidence->handled_payloads.size ()));
            EXPECT_EQ (evidence->handled_payloads.size (), fail_completion ? 0u : 1u);
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
        if (fail_completion) {
            EXPECT_FALSE (reply);
            if (const auto *error = reply.error ()) {
                RecordProperty ("failed_completion_request_error", error->what ());
                RecordProperty ("failed_completion_request_kind",
                                static_cast<int> (error->kind ()));
                RecordProperty ("failed_completion_request_code", error->code ().message ());
            }
        } else {
            ASSERT_TRUE (reply);
            EXPECT_EQ (reply.value ().value, late_payload);
        }
    }
}
} // namespace zlink::framework::test::actor_maintenance_join

/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_replay_access.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
enum class backlog_case_t
{
    ordinary_order,
    completion_failure,
    replay_failure
};
void local_join_backlog (backlog_case_t scenario)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.joined_payloads =
      scenario == backlog_case_t::replay_failure ? std::vector<int>{-1} : std::vector<int>{1, 2};
    evidence.fail_completion = scenario == backlog_case_t::completion_failure;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::actor_client_t> ();
    if (scenario != backlog_case_t::replay_failure)
        evidence.joined_sender = &client;
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    const auto ingress_ready = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                                 .async<probe_t> ()
                                 .result ();
    ASSERT_TRUE (ingress_ready);
    // Creation can retain the readiness probe itself; count only this Join's batch.
    const auto backlog_before_join = [&] {
        std::lock_guard lock (evidence.mutex);
        return evidence.backlog_appends;
    }();
    evidence.hold_replay_prefix = scenario == backlog_case_t::ordinary_order;
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto key = std::string (actor_type) + ":" + actor_id;
    auto actor_queue = [&] (fw::spot_id_t spot) {
        auto context = state->lane
                         .run ([&] {
                             return fw::detail::spot_context_access_t::state (
                               state->spot_contexts_by_id.at (std::string (spot)));
                         })
                         .get ();
        return context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
    };
    const auto source = state->lane.run ([&] { return state->actor_spot_ids.at (key); }).get ();
    auto source_queue = actor_queue (source);
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return !evidence.completions.empty (); }));
    }
    if (scenario == backlog_case_t::ordinary_order) {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.replay_prefix_entered; }));
    } else {
        source_queue->drain ();
    }
    if (scenario == backlog_case_t::replay_failure) {
        // Native acceptance can precede or follow the source handoff terminal.
        // Submit an actual retained record to replay so this test always exercises
        // replay failure rather than an ordinary post-Join send failure.
        const auto actor = state->lane
                             .run ([&] {
                                 return fw::detail::actor_ref_access_t::make (
                                   fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
                                   state->actor_generations.at (key));
                             })
                             .get ();
        fw::runtime::messaging::envelope_header_t header;
        header.kind = fw::runtime::messaging::message_kind_t::command;
        header.message_name = probe_t::packet_name;
        header.content_type = "application/json";
        auto encoded = fw::detail::encode_actor_packet (
          header, zlink::message_t::from (zlink::detail::json_profile::encode (probe_t{-1})));
        namespace wire = fw::runtime::protocol;
        wire::frozen_application_record_t frozen;
        frozen.kind = wire::frozen_record_kind_t::actor_send;
        frozen.body = wire::frozen_actor_application_body_t{
          {}, wire::application_payload_t::from_parts (std::move (encoded).take_items ())};
        auto record = std::make_shared<fw::runtime::serial_application_record_t> ();
        record->payload ().emplace ();
        record->payload ()->application_record = std::move (frozen);
        auto terminals = std::make_shared<std::atomic<int>> (0);
        record->native_reply = [terminals] (auto) {
            ++*terminals;
            return true;
        };
        fw::detail::handoff_packet_t packet;
        packet.packet_name = probe_t::packet_name;
        packet.content_type = header.content_type;
        packet.application_record = record;
        fw::detail::spot_node_runtime_t runtime (state);
        ASSERT_TRUE (fw::detail::spot_node_runtime_test_access_t::replay (
                       runtime, actor, {std::move (packet)}, services)
                       .result ());
        EXPECT_EQ (terminals->load (), 1);
        EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{-1}));
    }
    if (scenario == backlog_case_t::completion_failure) {
        auto sealed_queue = actor_queue (fw::spot_id_t (local_spot));
        ASSERT_TRUE (sealed_queue);
        (void) sealed_queue->pending_messages ().result ().value ();
        EXPECT_EQ (state->actor_transfer_coordinator->phase (key),
                   fw::detail::actor_move_phase_t::local);
        {
            std::lock_guard lock (evidence.mutex);
            // This later one-record batch owns its own admission future.
            evidence.joined_send_captured = fw::task_completion_source_t<void>{};
            auto *source = &evidence.joined_send_captured;
            evidence.joined_admission_completion = observations_t::joined_admission_batch_t{
              1, [source] { source->complete (fw::result_t<void>::success ()); }};
        }
        ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), probe_t{3}).async ().result ());
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.backlog_appends == backlog_before_join + 3; }));
        const auto admission = evidence.joined_send_captured.task ();
        lock.unlock ();
        // Backlog retention and native admission publish on independent owners.
        // Observe this batch's completion instead of assuming their callback order.
        const auto captured = admission.result_for (wait_budget);
        ASSERT_TRUE (captured);
        ASSERT_TRUE (*captured);
        lock.lock ();
        EXPECT_TRUE (evidence.joined_send_captured.task ().await_ready ());
        EXPECT_TRUE (evidence.handled_payloads.empty ());
        EXPECT_EQ (evidence.completions.size (), 1u);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.front ()));
        lock.unlock ();
        // The queue control future observes published work while keeping the
        // host executor available for the later canonical ingress.
        (void) sealed_queue->pending_messages ().result ().value ();
        const auto actor = state->lane
                             .run ([&] {
                                 return fw::detail::actor_ref_access_t::make (
                                   fw::node_rid_t::from_string (first_rid), actor_type, actor_id,
                                   state->actor_generations.at (key));
                             })
                             .get ();
        fw::detail::spot_node_runtime_t runtime (state);
        runtime.seal_actor_application_suffix (actor, {}).result ().value ();
        auto backlog = state->actor_transfer_coordinator->cancel_move (key);
        EXPECT_EQ (backlog.size (), 3u);
        std::vector<int> durable_payloads;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> durable_operations;
        for (const auto &packet : backlog) {
            ASSERT_TRUE (packet.application_record);
            ASSERT_TRUE (packet.application_record->payload ());
            EXPECT_TRUE (packet.application_record->saved_work ());
            const auto &turn = *packet.application_record->payload ();
            ASSERT_TRUE (turn.application_record || turn.frozen_record);
            const auto canonical = turn.application_record
                                     ? fw::runtime::protocol::summarize_frozen_application_record (
                                         *turn.application_record)
                                     : *turn.frozen_record;
            EXPECT_EQ (canonical.kind, fw::runtime::protocol::frozen_record_kind_t::actor_send);
            EXPECT_EQ (canonical.source.node_routing_id,
                       zlink::routing_id_t::from (first_rid).to_bytes ());
            EXPECT_TRUE (canonical.operation.high != 0 || canonical.operation.low != 0);
            durable_operations.emplace_back (canonical.operation.high, canonical.operation.low);
            fw::runtime::stateful::with_accepted_application (
              turn, [&] (auto, const auto &payload) {
                  fw::runtime::messaging::message_parts_t parts (
                    fw::runtime::protocol::decode_application_parts (payload));
                  const auto body = fw::runtime::messaging::envelope_codec_t{}.decode_body (parts);
                  ASSERT_TRUE (body);
                  durable_payloads.push_back (zlink::detail::json_profile::decode_value<probe_t> (
                                                nlohmann::json::parse (body.value ().to_string ()))
                                                .value);
              });
        }
        std::sort (durable_payloads.begin (), durable_payloads.end ());
        EXPECT_EQ (durable_payloads, (std::vector<int>{1, 2, 3}));
        std::sort (durable_operations.begin (), durable_operations.end ());
        EXPECT_EQ (std::adjacent_find (durable_operations.begin (), durable_operations.end ()),
                   durable_operations.end ());
        EXPECT_TRUE (runtime.take_actor_application_suffix (actor, {}).result ().value ().empty ());
        {
            std::lock_guard guard (evidence.mutex);
            EXPECT_TRUE (evidence.handled_payloads.empty ());
        }
        return;
    }
    if (scenario == backlog_case_t::ordinary_order) {
        EXPECT_FALSE (state->actor_transfer_coordinator->phase (key));
        const auto posts_before_request = [&] {
            std::lock_guard lock (evidence.mutex);
            return evidence.ordinary_posts;
        }();
        auto ordinary = client.request (fw::actor_id_t (actor_id), probe_t{3}).async<probe_t> ();
        {
            std::unique_lock lock (evidence.mutex);
            ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
                return evidence.ordinary_posts == posts_before_request + 1;
            }));
            EXPECT_TRUE (evidence.handled_payloads.empty ());
        }
        {
            std::lock_guard lock (evidence.mutex);
            evidence.release_replay_prefix = true;
        }
        evidence.changed.notify_all ();
        const auto replied = ordinary.result ();
        ASSERT_TRUE (replied);
        EXPECT_EQ (replied.value ().value, 3);
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.handled_payloads.size () == 3; }));
        EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1, 2, 3}));
    } else {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return !evidence.replay_failures.empty (); }));
        EXPECT_FALSE (state->actor_transfer_coordinator->phase (key));
        const auto &event = evidence.replay_failures.front ();
        EXPECT_EQ (event.result, fw::message_flow_result_t::failed);
        ASSERT_TRUE (event.exception);
        EXPECT_EQ (event.flow_id, event.correlation_id);
        EXPECT_FALSE (event.flow_id.value_or ("").empty ());
        try {
            std::rethrow_exception (event.exception);
        }
        catch (const std::exception &error) {
            EXPECT_STREQ (error.what (), "replayed payload failed");
        }
    }
}
TEST (ActorMaintenanceJoin, LocalJoinPlaceholderKeepsOrdinaryIngressBehindBacklog)
{
    local_join_backlog (backlog_case_t::ordinary_order);
}
TEST (ActorMaintenanceJoin, LocalJoinCompletionFailureKeepsBacklogSealed)
{
    local_join_backlog (backlog_case_t::completion_failure);
}
TEST (ActorMaintenanceJoin, ReplayFailureHasFailedOutcomeAndException)
{
    local_join_backlog (backlog_case_t::replay_failure);
}
} // namespace zlink::framework::test::actor_maintenance_join

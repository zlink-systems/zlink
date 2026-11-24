/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/telemetry_observation.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
int relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto source = test_actor_ref ("source-node", "player", "relocation-prewarm", 7);
    const auto target = test_actor_ref ("target-node", "player", "relocation-prewarm", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    // The gateway state outlives this scope, so the callbacks own what they write.
    auto sends = std::make_shared<std::atomic_int> (0);
    auto sent_node = std::make_shared<std::string> ();
    // The gateway state outlives this scope, so the observer owns its flag.
    auto saw_stale_target = std::make_shared<std::atomic_bool> (false);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options, [saw_stale_target] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::dropped
              && event.reason == message_flow_reason_t::stale_target
              && event.detail_stage == std::optional<std::string> ("route_fence")) {
              saw_stale_target->store (true, std::memory_order_release);
          }
      });
    gateway.set_dispatch (std::move (trace_options));
    gateway.on_bound_session_send (
      [sends, sent_node] (const actor_ref_t &actor, std::uint64_t binding_generation,
                          const stream_header_t &,
                          const zlink::message_t &) -> task_t<result_t<void>> {
          if (binding_generation == 23)
              *sent_node = std::string (actor.node_rid ().value ());
          ++*sends;
          co_return result_t<void>::success ();
      });
    // The target can retain a lower cache generation when it did not own an
    // intervening relocation. Post-CAS prewarm must advance that cache to the
    // Store-confirmed route rather than require it to equal the previous fence.
    if (!gateway.record_bound_session_route (source, session_owner, session_rid, 11, 15, 19, 23, 0,
                                             1)) {
        return 1;
    }
    const auto staged_source_sink =
      state->sync ([&] { return state->bound_session_sinks.at ("relocation-prewarm"); });

    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"relocation-prewarm", 7},
      .session_owner_node_routing_id = session_owner.to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "session-owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = session_rid.to_bytes (),
      .binding_generation = 23,
      .route = {protocol::session_relocation_route_action_t::commit, 17, 29,
                zlink::routing_id_t::from (std::string ("target-node")).to_bytes (), 31, 0, 37}};
    if (!gateway.prepare_session_relocation_target_route (route, 37)) {
        return 2;
    }
    const auto prewarmed_route_matches = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("relocation-prewarm");
        return record.ref.node_rid ().value () == target.node_rid ().value ()
               && record.bound_session_route
               && record.bound_session_route->authority_owner_generation == 29
               && record.bound_session_route->owner_lease_generation == 37
               && record.bound_session_route->session_sequence == 0
               && record.bound_session_route->session_sequence_baseline_unknown;
    });
    if (!prewarmed_route_matches)
        return 3;

    const auto staged_push =
      (*staged_source_sink) ("RelocationStagedPush", stream_codec_t::message_pack,
                             zlink::message_t::from ("staged"))
        .result ();
    if (!staged_push || sends->load (std::memory_order_acquire) != 1
        || *sent_node != "target-node") {
        return 4;
    }
    sends->store (0, std::memory_order_release);
    sent_node->clear ();

    const protocol::actor_route_fence_t target_fence{"relocation-prewarm",
                                                     7,
                                                     route.route.target_node_routing_id,
                                                     route.route.target_node_generation,
                                                     route.route.target_authority_owner_generation,
                                                     37};
    if (!gateway.admit_session_relay (target, session_owner, session_rid, 23, 25, &target_fence)
        || !gateway.prepare_session_relocation_target_route (route, 37)) {
        return 5;
    }
    const auto prewarmed_sequence_is_admitted = state->sync ([&] {
        const auto &prepared = *state->actors_by_id.at ("relocation-prewarm").bound_session_route;
        return prepared.session_sequence == 25 && !prepared.session_sequence_baseline_unknown;
    });
    if (!prewarmed_sequence_is_admitted)
        return 6;

    const auto submitted = gateway.actor_context (target)
                             .bound_session ()
                             .send (std::string ("RelocationReady"))
                             .async ()
                             .result ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (sends->load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (!submitted || sends->load () != 1 || *sent_node != "target-node")
        return 7;

    auto stale = route;
    stale.route.previous_authority_owner_generation = 16;
    stale.route.target_authority_owner_generation = 30;
    if (gateway.prepare_session_relocation_target_route (stale, 41))
        return 8;

    auto replacement = *gateway.bound_session_route_async (target).result ().value ();
    ++replacement.binding_generation;
    if (!gateway.record_bound_session_route_transition (target, replacement))
        return 9;
    const auto stale_binding_push =
      (*staged_source_sink) ("StaleBindingPush", stream_codec_t::message_pack,
                             zlink::message_t::from ("stale"))
        .result ();
    test::acknowledge_delivery ();
    if (stale_binding_push || !saw_stale_target->load (std::memory_order_acquire))
        return 10;

    auto fresh_state = std::make_shared<actor_gateway_state_t> ();
    fresh_state->serializers = &serializers;
    actor_gateway_runtime_t fresh_gateway (fresh_state);
    fresh_gateway.on_bound_session_send ([] (const actor_ref_t &, std::uint64_t,
                                             const stream_header_t &,
                                             const zlink::message_t &) -> task_t<result_t<void>> {
        co_return result_t<void>::success ();
    });
    if (!fresh_gateway.prepare_session_relocation_target_route (route, 37))
        return 11;
    const auto fresh_route_is_prewarmed = fresh_state->sync ([&] {
        const auto found = fresh_state->actors_by_id.find ("relocation-prewarm");
        return found != fresh_state->actors_by_id.end () && found->second.bound
               && !found->second.disconnected
               && found->second.ref.node_rid ().value () == target.node_rid ().value ()
               && found->second.bound_session_route
               && found->second.bound_session_route->node_rid.to_bytes ()
                    == session_owner.to_bytes ()
               && found->second.bound_session_route->session_rid == session_rid
               && found->second.bound_session_route->authority_owner_generation == 29
               && found->second.bound_session_route->owner_lease_generation == 37
               && found->second.bound_session_route->session_sequence_baseline_unknown
               && fresh_state->bound_session_sinks.contains ("relocation-prewarm");
    });
    if (!fresh_route_is_prewarmed)
        return 12;
    return 0;
}

int stale_binding_registration_and_relocation_prepare_preserve_current_tenure ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("source-node", "player", "stale-binding", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid, 11, 17, 19, 24, 0,
                                             1, false, "session-owner-id", 13))
        return 1;

    auto stale = *gateway.bound_session_route_async (actor).result ().value ();
    stale.binding_generation = 23;
    stale.authority_owner_generation = 29;
    stale.owner_lease_generation = 37;
    const auto registered = gateway.record_bound_session_route_transition (actor, stale);
    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"stale-binding", 7},
      .session_owner_node_routing_id = session_owner.to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "session-owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = session_rid.to_bytes (),
      .binding_generation = 23,
      .route = {protocol::session_relocation_route_action_t::commit, 17, 29,
                zlink::routing_id_t::from (std::string ("target-node")).to_bytes (), 31, 0, 37}};
    const bool prepared = gateway.prepare_session_relocation_target_route (route, 37);
    const bool unchanged = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("stale-binding");
        const auto &current = *record.bound_session_route;
        return record.ref.node_rid ().value () == "source-node" && current.binding_generation == 24
               && current.authority_owner_generation == 17 && current.owner_lease_generation == 19
               && current.session_sequence == 1
               && current.node_rid.to_bytes () == session_owner.to_bytes ()
               && current.session_rid == session_rid;
    });
    if (registered || !registered.error ()
        || registered.error_kind () != framework_error_kind_t::unavailable || prepared
        || !unchanged) {
        std::cerr << "stale binding: registered=" << static_cast<bool> (registered)
                  << " prepared=" << prepared << " current_unchanged=" << unchanged << '\n';
        return 2;
    }
    return 0;
}

int relocation_prepare_preserves_runtime_failure ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"actor", 7},
      .session_owner_node_routing_id = zlink::routing_id_t::from ("owner").to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = zlink::routing_id_t::from ("session").to_bytes (),
      .binding_generation = 17,
      .route = {protocol::session_relocation_route_action_t::commit, 19, 23,
                zlink::routing_id_t::from ("target").to_bytes (), 29, 0, 31}};
    for (int field = 0; field < 3; ++field) {
        auto malformed = route;
        auto &rid = field == 0   ? malformed.session_owner_node_routing_id
                    : field == 1 ? malformed.session_routing_id
                                 : malformed.route.target_node_routing_id;
        rid.assign (protocol::ridBytes + 1, 1);
        if (gateway.prepare_session_relocation_target_route (malformed, 31))
            return 1;
        rid.clear ();
        if (gateway.prepare_session_relocation_target_route (malformed, 31))
            return 2;
    }
    state->lane.close ();
    try {
        (void) gateway.prepare_session_relocation_target_route (route, 31);
    }
    catch (const std::runtime_error &error) {
        return std::string_view (error.what ()) == "state lane is closed" ? 0 : 3;
    }
    return 4;
}
} // namespace zlink::framework::test::actor_gateway

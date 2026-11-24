/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/request_admission_fixture.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int bound_session_actor_dispatch_uses_current_binding_without_location_reread ();

int bound_session_relay_admission_is_exact_and_monotonic ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "game.actor", "relay-actor", 3);
    if (!gateway.bind_session_sink (actor,
                                    [] (std::string, stream_codec_t, const zlink::message_t &) {
                                        return task_t<void> (result_t<void>::success ());
                                    })) {
        return 1;
    }
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid, 7, 11, 13, 17, 0,
                                             0)) {
        return 2;
    }
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1))
        return 4;
    if (gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1)
        || gateway.admit_session_relay (
          actor, zlink::routing_id_t::from (std::string ("other-owner")), session_rid, 17, 2)) {
        return 7;
    }
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 2))
        return 9;
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    if (!route || route->session_sequence != 2)
        return 11;
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 4)
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 3)
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 4))
        return 22;
    const auto after_gap = gateway.bound_session_route_async (actor).result ().value ();
    if (!after_gap || after_gap->session_sequence != 4)
        return 23;

    namespace stateful = runtime::stateful;
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "relay-actor", 3, 11, "relay-mesh", "actor-owner"};
    auto other_owner = owner;
    other_owner.key = "other-actor";
    stateful::stream_session_registry_t sessions (
      [&] (const std::string &key) -> std::optional<stateful::object_ref_t> {
          if (key == owner.key)
              return owner;
          if (key == other_owner.key)
              return other_owner;
          return std::nullopt;
      });
    const auto connection = sessions.open (session_rid.to_hex (), {}, session_owner, 7);
    const auto [binding_error, binding] = sessions.bind (connection, owner, 7, 13);
    if (binding_error != stateful::stateful_error_t::none)
        return 12;
    const auto [first_error, first] = sessions.admit_inbound (binding);
    if (first_error != stateful::stateful_error_t::none || !first || first->inbound_sequence != 1)
        return 13;
    if (sessions.complete_inbound (*first) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*first) == stateful::stateful_error_t::none)
        return 14;
    const auto [second_error, second] = sessions.admit_inbound (binding);
    if (second_error != stateful::stateful_error_t::none || !second
        || second->inbound_sequence != 2)
        return 15;
    auto wrong_binding = *second;
    ++wrong_binding.binding.binding_generation;
    auto wrong_sequence = *second;
    wrong_sequence.inbound_sequence += 2;
    if (sessions.complete_inbound (wrong_binding) == stateful::stateful_error_t::none
        || sessions.complete_inbound (wrong_sequence) == stateful::stateful_error_t::none
        || sessions.complete_inbound (*second) != stateful::stateful_error_t::none)
        return 16;

    const auto [third_error, third] = sessions.admit_inbound (binding);
    const auto other_connection = sessions.open ("other-session", {}, session_owner, 7);
    const auto [other_binding_error, other_binding] =
      sessions.bind (other_connection, other_owner, 7, 13);
    const auto [other_error, other] = sessions.admit_inbound (other_binding);
    if (third_error != stateful::stateful_error_t::none || !third || third->inbound_sequence != 3
        || other_binding_error != stateful::stateful_error_t::none
        || other_error != stateful::stateful_error_t::none || !other)
        return 17;
    auto unaccepted_other_token = *third;
    unaccepted_other_token.drain = other->drain;
    if (sessions.complete_inbound (unaccepted_other_token) == stateful::stateful_error_t::none
        || !gateway.destroy_actor (actor)
        || sessions.try_seal_actor (owner).first != stateful::stateful_error_t::backpressured
        || sessions.try_seal_actor (other_owner).first != stateful::stateful_error_t::backpressured)
        return 18;
    if (sessions.complete_inbound (*third) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*third) == stateful::stateful_error_t::none
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 3)
        || sessions.complete_inbound (*other) != stateful::stateful_error_t::none)
        return 19;
    const auto [seal_error, seal] = sessions.try_seal_actor (owner);
    if (seal_error != stateful::stateful_error_t::none
        || sessions.abort_barrier (seal) != stateful::stateful_error_t::none)
        return 20;

    // A later accepted ingress does not invalidate an earlier completion token.
    const auto [fourth_error, fourth] = sessions.admit_inbound (binding);
    const auto [fifth_error, fifth] = sessions.admit_inbound (binding);
    if (fourth_error != stateful::stateful_error_t::none || !fourth
        || fifth_error != stateful::stateful_error_t::none || !fifth
        || fourth->inbound_sequence != 4 || fifth->inbound_sequence != 5
        || sessions.complete_inbound (*fourth) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*fifth) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*fourth) == stateful::stateful_error_t::none)
        return 21;
    return bound_session_actor_dispatch_uses_current_binding_without_location_reread ();
}

int bound_session_actor_dispatch_uses_current_binding_without_location_reread ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;
    namespace protocol = zlink::framework::runtime::protocol;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("bound-session-target");
    auto errors =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    auto flows = std::make_shared<test::telemetry_observations_t<message_flow_event_t>> ();
    node->dispatch.message_flow (message_flow_log_mode_t::normal);
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      node->dispatch, [errors] (const auto &event) { errors->record (event); });
    dispatch_options_access_t::set_observer_for_tests (node->dispatch, [flows] (const auto &event) {
        if (event.outcome == message_flow_outcome_t::dropped)
            flows->record (event);
    });
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "bound-session-target");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    const auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor =
                 {.mesh_name = "bound-session-dispatch",
                  .node_routing_id =
                    zlink::routing_id_t::from (std::string ("bound-session-target")).to_bytes (),
                  .lifecycle_generation = 7,
                  .descriptor_revision = 1,
                  .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    node->native_node = native;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("bound-session-target");
    spot->spot_id = spot_id_t ("bound-session-spot");
    spot->spot_name = "bound-session-spot";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("bound-session-target", "player", "bound-player", 3);
    const auto actor_key = std::string ("player:bound-player");
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 3);
    node->actor_types_by_id.emplace ("bound-player", "player");

    std::atomic_int handler_calls{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "MoveMsg", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&handler_calls] (void *, void *, service_provider_t &, serializer_registry_t &,
                        const zlink::message_t &,
                        const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid,
                                             /*session owner generation=*/11,
                                             /*authority owner generation=*/13,
                                             /*target owner lease generation=*/17,
                                             /*binding generation=*/19, 0, 0)) {
        return 1;
    }

    std::atomic_int location_reads{0};
    node->actor_route_admission = [&location_reads] (const protocol::actor_route_fence_t &) {
        location_reads.fetch_add (1, std::memory_order_acq_rel);
        return false;
    };

    const auto exact_target =
      protocol::actor_route_fence_t{"bound-player",
                                    3,
                                    native->status ().routing_id ().to_bytes (),
                                    native->status ().lifecycle_generation (),
                                    13,
                                    17};
    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string (spot->spot_id),
                                     .actor = actor};
    spot_node_runtime_t spots (node);

    const auto dispatch = [&] (protocol::actor_route_fence_t target, std::uint64_t sequence,
                               bool request = false, bool fenced = true) {
        host::receive_record_t record{.kind = request ? host::record_kind_t::actor_request
                                                      : host::record_kind_t::actor_send,
                                      .domain = host::ready_domain_t::application};
        record.source_node_rid = session_owner;
        record.source_session_rid = session_rid;
        record.source_binding_generation = 19;
        record.source_session_sequence = sequence;
        if (fenced)
            record.actor_route = std::move (target);
        auto encoded = zlink::framework::detail::encode_actor_packet (
          messaging::envelope_header_t{.kind = request ? messaging::message_kind_t::request
                                                       : messaging::message_kind_t::command,
                                       .channel_name = "actor",
                                       .message_name = "MoveMsg",
                                       .correlation_id = "relay-sequence"},
          zlink::message_t::from (std::string ("move")));
        auto parts = std::move (encoded).take_items ();
        return spots.dispatch_mesh_record (
          owner, record, parts,
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
          provider, serializers);
    };
    const auto wait_for_calls = [&] (int expected) {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (handler_calls.load (std::memory_order_acquire) != expected
               && std::chrono::steady_clock::now () < deadline) {
            std::this_thread::yield ();
        }
        return handler_calls.load (std::memory_order_acquire) == expected;
    };

    (void) dispatch (exact_target, 1);
    if (!wait_for_calls (1)) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 5;
    }
    if (location_reads.load (std::memory_order_acquire) != 0) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 6;
    }

    auto stale_target = exact_target;
    ++stale_target.authority_owner_generation;
    (void) dispatch (std::move (stale_target), 2);
    if (!wait_for_calls (2) || location_reads.load (std::memory_order_acquire) != 0) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 4;
    }
    (void) dispatch (exact_target, 3);
    if (!wait_for_calls (3)) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 7;
    }
    node->worker_executor->drain ();
    // Duplicate and lower sequences share the existing Actor owner admission.
    // Neither kind may reach the handler; each rejection must be observable.
    for (const auto fenced : {true, false}) {
        for (const auto sequence : {3U, 2U}) {
            (void) dispatch (exact_target, sequence, false, fenced);
            (void) dispatch (exact_target, sequence, true, fenced);
        }
    }
    test::acknowledge_delivery ();
    const auto rejected_requests = errors->snapshot ();
    const auto rejected_sends = flows->snapshot ();
    node->worker_executor->drain ();
    node->worker_executor.reset ();
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    if (handler_calls.load () != 3 || rejected_requests.size () != 4
        || rejected_sends.size () != 4) {
        std::cerr << "relay rejection records: requests=" << rejected_requests.size ()
                  << " sends=" << rejected_sends.size () << " handlers=" << handler_calls.load ()
                  << '\n';
        return 8;
    }
    for (const auto &event : rejected_requests) {
        if (event.surface != dispatch_error_surface_t::stream_session
            || event.message_kind != dispatch_message_kind_t::request
            || event.reason != dispatch_error_reason_t::stale_target
            || event.action != dispatch_error_action_t::reply_error
            || event.packet_name != "MoveMsg" || event.actor_id != "bound-player"
            || event.source_rid != session_owner.to_hex ()
            || event.correlation_id != "relay-sequence" || !event.exception)
            return 9;
        try {
            std::rethrow_exception (event.exception);
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::invalid_operation)
                return 10;
        }
    }
    for (const auto &event : rejected_sends) {
        if (event.surface != dispatch_error_surface_t::stream_session
            || event.message_kind != dispatch_message_kind_t::send
            || event.outcome != message_flow_outcome_t::dropped
            || event.reason != message_flow_reason_t::stale_target || event.packet_name != "MoveMsg"
            || event.actor_id != "bound-player" || event.source_rid != session_owner.to_hex ()
            || event.stream_session_id != session_rid.to_hex ()
            || event.correlation_id != "relay-sequence")
            return 11;
    }
    return 0;
}

int session_relay_waiter_accepts_beyond_former_capacity ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    constexpr std::size_t relay_count = 1026;
    using reply_source_t = task_completion_source_t<std::optional<zlink::message_t>>;
    struct relay_fixture_t
    {
        std::vector<std::shared_ptr<reply_source_t>> sources;
        std::vector<std::promise<void>> arrived;
        std::vector<std::future<void>> arrivals;
        std::vector<std::string> packet_names;
        std::size_t dispatch_count = 0;
        std::promise<std::size_t> &released;

        relay_fixture_t (std::size_t count, std::promise<std::size_t> &released) :
            sources (count), arrived (count), packet_names (count), released (released)
        {
            arrivals.reserve (count);
            for (std::size_t index = 0; index != count; ++index) {
                sources[index] = std::make_shared<reply_source_t> ();
                arrivals.push_back (arrived[index].get_future ());
            }
        }

        ~relay_fixture_t () { released.set_value (dispatch_count); }
    };

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t actor = test_actor_ref ("actor-node-a", "player", "actor-capacity", 1);
    auto binding = manager.bind (actor).async ().result ().value ();
    std::promise<std::size_t> released;
    auto release = released.get_future ();
    {
        auto fixture = std::make_shared<relay_fixture_t> (relay_count, released);
        std::vector<task_t<void>> accepted;
        accepted.reserve (relay_count);
        struct relay_scope_t
        {
            actor_gateway_runtime_t &gateway;
            std::shared_ptr<relay_fixture_t> &fixture;
            std::vector<task_t<void>> &accepted;
            std::future<std::size_t> &release;
            ~relay_scope_t ()
            {
                for (const auto &source : fixture->sources)
                    source->complete (
                      result_t<std::optional<zlink::message_t>>::success (std::nullopt));
                for (auto &task : accepted)
                    (void) task.result ();
                gateway.on_relay ({});
                fixture.reset ();
                // The final drain observer owns its dispatch closure until it returns.
                release.wait ();
            }
        } relay_scope{gateway, fixture, accepted, release};
        gateway.on_relay ([fixture] (const actor_ref_t &, const actor_context_t &,
                                     const stream_header_t &header, const zlink::message_t &,
                                     std::optional<bound_session_relay_source_t>,
                                     std::chrono::milliseconds) {
            const auto index = fixture->dispatch_count++;
            fixture->packet_names.at (index) = std::string (header.packet_name ());
            const auto source = fixture->sources.at (index);
            fixture->arrived.at (index).set_value ();
            return test::accepted_request_task (source->task ());
        });

        /* One relay occupies the drain turn; the rest cross the former boundary. */
        for (std::size_t index = 0; index != relay_count; ++index)
            accepted.push_back (
              binding.relay (std::to_string (index), zlink::framework::message_t{}));
        auto completed = std::make_shared<std::promise<void>> ();
        auto completion = completed->get_future ();
        observe_task_completion (accepted.back (),
                                 [completed] (const auto &) { completed->set_value (); });

        /* Each arrival is observed before its source is released; order remains FIFO. */
        for (std::size_t index = 0; index != relay_count; ++index) {
            fixture->arrivals[index].get ();
            if (fixture->packet_names[index] != std::to_string (index))
                return 5;
            fixture->sources[index]->complete (
              result_t<std::optional<zlink::message_t>>::success (std::nullopt));
        }
        // Result delivery precedes the final drain frame's destruction.
        completion.get ();
        for (auto &admitted : accepted) {
            if (!admitted.await_ready () || !admitted.result ())
                return 4;
        }
    }
    if (release.get () != relay_count)
        return 3;
    return 0;
}
} // namespace zlink::framework::test::actor_gateway

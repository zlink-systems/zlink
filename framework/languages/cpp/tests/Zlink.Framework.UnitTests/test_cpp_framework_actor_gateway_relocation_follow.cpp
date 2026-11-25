/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int old_owner_forwards_cold_probe_via_active_message_follow_route (bool disconnect)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // Regression pin: a client that probes an Actor's former owner directly
    // (no Message Follow fence on the wire, hop_count 0 -- e.g. a stale
    // cached location) must be forwarded to the real current owner instead
    // of failing with "actor spot context is not registered" and, worse,
    // materializing a duplicate local Actor instance on the old owner.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "actor-a-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    // The factory and the relay handler can run after this scope ends (the 500 ms bounded wait
    // may return first), so they own what they write and capture read-only values by value.
    auto local_materializations = std::make_shared<std::atomic_int> (0);
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [local_materializations] (std::string) {
        local_materializations->fetch_add (1, std::memory_order_acq_rel);
        return std::make_shared<int> (7);
    };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "actor-remote-ok", 21);
    const auto key = std::string ("player:actor-remote-ok");

    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "actor-remote-ok", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "actor-remote-ok", 21, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 1, 1};
    const auto target_actor = test_actor_ref ("actor-b", "player", "actor-remote-ok", 21);
    const spot_route_t target_route{node_rid_t::from_string ("actor-b"),
                                    spot_id_t ("spot-remote-ok-0"), "game"};

    // Mirrors spot_node_runtime_t::complete_remote_actor_transfer: it retains
    // the Message Follow route for the old source fence and (via
    // record_actor_route_unlocked) records the remote target's location,
    // which is not a locally registered Spot on this node.
    node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, target_actor, target_route, target_fence,
      std::chrono::steady_clock::now () + std::chrono::seconds (30), "relocation-1");
    record_actor_route_unlocked (*node, key, target_route, target_actor.object_generation ());

    // The caller's own identity (spec 28.en:584-591): a synthesized-fence
    // relay must still forward these to the Message Follow relay, not the
    // zero/empty placeholders that would silently skip the stale-cache
    // notice back to whoever actually sent this probe.
    const auto inbound_source_node_rid = zlink::routing_id_t::from ("probing-client");
    const auto inbound_operation = runtime::protocol::wire_operation_id_t{111, 222};
    constexpr std::uint64_t inbound_reply_route_id = 333;
    const std::string inbound_deadline = "2030-01-02T03:04:05Z";

    const auto packet_name =
      disconnect ? runtime::protocol::session_disconnected_packet_name : "after-transfer";
    const auto payload = disconnect
                           ? runtime::protocol::encode_session_disconnected (
                               {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7})
                           : zlink::message_t::from (std::string ("probe")).to_bytes ();
    auto follow_terminal =
      std::make_shared<task_completion_source_t<std::optional<zlink::message_t>>> ();
    std::promise<bool> follow_entered;
    auto follow_started = follow_entered.get_future ();
    auto follow_entered_once = std::make_shared<std::atomic_bool> (false);
    auto relay_calls = std::make_shared<std::atomic_int> (0);
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [relay_calls, follow_terminal, follow_entered_once, &follow_entered, disconnect, source_fence,
       packet_name, payload, inbound_deadline, inbound_source_node_rid, inbound_operation] (
        const actor_ref_t &relayed_actor, const runtime::messaging::envelope_header_t &header,
        const zlink::message_t &body, std::chrono::milliseconds relay_timeout,
        const zlink::routing_id_t &source_node, const runtime::protocol::actor_route_fence_t &route,
        std::uint8_t hop_count, const runtime::protocol::wire_operation_id_t &operation,
        std::uint64_t reply_route_id) -> task_t<std::optional<zlink::message_t>> {
          relay_calls->fetch_add (1, std::memory_order_acq_rel);
          if (disconnect && !follow_entered_once->exchange (true))
              follow_entered.set_value (true);
          if (relayed_actor.actor_id ().value () != "actor-remote-ok" || route != source_fence
              || hop_count != 0 || header.message_name != packet_name || body.to_bytes () != payload
              || header.deadline != inbound_deadline || relay_timeout != std::chrono::seconds (30)
              || source_node.to_bytes () != inbound_source_node_rid.to_bytes ()
              || operation.high != inbound_operation.high || operation.low != inbound_operation.low
              || reply_route_id != inbound_reply_route_id) {
              co_return result_t<std::optional<zlink::message_t>>::failure (
                framework_error_kind_t::internal_failure, "unexpected relay arguments");
          }
          if (disconnect)
              co_return co_await follow_terminal->task ();
          co_return result_t<std::optional<zlink::message_t>>::success (
            std::make_optional (zlink::message_t::from (std::string ("relayed"))));
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    auto relayed = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request, packet_name,
      zlink::message_t::from (payload), provider, serializers, std::move (metadata), nullptr,
      std::function<void ()>{}, std::function<void ()>{}, inbound_source_node_rid,
      inbound_operation, inbound_reply_route_id, inbound_deadline);

    bool premature = false;
    if (disconnect) {
        detail::observe_task_completion (relayed,
                                         [&follow_entered, follow_entered_once] (const auto &) {
                                             if (!follow_entered_once->exchange (true))
                                                 follow_entered.set_value (false);
                                         });
        const bool began = follow_started.get ();
        premature = !began || relayed.await_ready ();
        follow_terminal->complete (result_t<std::optional<zlink::message_t>>::success (
          std::make_optional (zlink::message_t{})));
    }
    const auto outcome = disconnect ? std::make_optional (relayed.result ())
                                    : finite_task_result (std::move (relayed));
    if (!outcome || !*outcome)
        return 1;
    if (relay_calls->load (std::memory_order_acquire) != 1)
        return 2;
    if (disconnect && (premature || !outcome->value () || !outcome->value ()->is_empty ()))
        return 4;
    if (local_materializations->load (std::memory_order_acquire) != 0)
        return 3;
    return 0;
}

int returned_owner_serves_locally_past_retained_message_follow_route ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // A->B->A keeps one Actor incarnation. When A becomes the Store-confirmed
    // owner again, its newer authority fence must win over the old A->B
    // Message Follow route that remains for the retransmission window. A
    // locally originated packet carries no follow fence, so relay_actor_packet
    // synthesizes the retained source fence; that must not reopen the old edge.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "returned-owner-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-returned");
    spot->spot_name = "returned";
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

    const auto actor = test_actor_ref ("actor-a", "player", "returned-actor", 21);
    const auto key = std::string ("player:returned-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_routes.emplace (
      key, spot_route_t{node_rid_t::from_string ("actor-a"), spot->spot_id, "returned"});
    node->actor_generations.emplace (key, actor.object_generation ());

    const auto old_source_fence = runtime::protocol::actor_route_fence_t{
      "returned-actor", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 3, 5};
    const auto old_target_fence = runtime::protocol::actor_route_fence_t{
      "returned-actor", 21, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 4, 7};
    node->actor_transfer_coordinator->activate_message_follow (
      key, old_source_fence, test_actor_ref ("actor-b", "player", "returned-actor", 21),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-away"), "away"},
      old_target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
      "relocation-away");
    node->actor_authority_fences.emplace (
      key, runtime::protocol::actor_route_fence_t{
             "returned-actor", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 5, 11});

    std::atomic_int local_calls{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "ReturnedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&local_calls] (void *, void *, service_provider_t &, serializer_registry_t &,
                      const zlink::message_t &,
                      const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          local_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    std::atomic_int relay_calls{0};
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [&relay_calls] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                      const zlink::message_t &, std::chrono::milliseconds,
                      const zlink::routing_id_t &, const runtime::protocol::actor_route_fence_t &,
                      std::uint8_t, const runtime::protocol::wire_operation_id_t &,
                      std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;
    auto sent =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReturnedProbe", zlink::message_t::from (std::string ("probe")),
                                provider, serializers, std::move (metadata), nullptr);
    const auto outcome = finite_task_result (std::move (sent));
    node->worker_executor->drain ();
    node->worker_executor.reset ();
    if (!outcome || !*outcome)
        return 1;
    if (local_calls.load (std::memory_order_acquire) != 1)
        return 2;
    const auto passed = relay_calls.load (std::memory_order_acquire) == 0 ? 0 : 3;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
} // namespace zlink::framework::test::actor_gateway

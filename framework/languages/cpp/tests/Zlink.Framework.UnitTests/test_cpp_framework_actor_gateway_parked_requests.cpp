/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
/* Actor requests parked during relocation retain their original reply route.
 * The same production dispatch fixture pins that a fresh request still enters
 * the backlog beyond the former 1024 pending-reply boundary. */
int parked_request_reply_case (const std::string &requester_rid, parked_request_case_t test_case)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    // Regression pin (SF-F2 hang / SF-F7 cross-node timeout): an
    // actor_request that arrives while the actor's transfer is still open
    // parks in the handoff backlog and is replayed with a handoff terminal
    // route. The pending entry that maps the terminal back to the original
    // reply token must be recorded even when the requester attached no
    // actor_route fence, and the terminal must return to THIS parking node
    // even when the requester lives on another node — before the fix the
    // terminal route named the requester, so a cross-node requester's
    // terminal never found the pending entry and the reply was lost.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "parked-replay-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-user");
    spot->spot_name = "user";
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

    const auto actor = test_actor_ref ("actor-a", "player", "parked-actor", 1);
    const auto key = std::string ("player:parked-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("parked-actor", "player");

    // The handler may run after an early return; it owns the flag.
    const auto handler_ran_owner = std::make_shared<std::atomic_bool> (false);
    auto &handler_ran = *handler_ran_owner;
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "ParkedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_ran_owner] (void *, void *, service_provider_t &, serializer_registry_t &,
                           const zlink::message_t &,
                           const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_ran_owner->store (true, std::memory_order_release);
          co_return zlink::message_t::from (std::string ("pong"));
      });

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    services.add_singleton<serializer_registry_t> ();
    node->root_services = services.build_provider ();
    auto provider = services.build_provider ();

    // The transfer is open when the request arrives: it must park.
    if (!node->actor_transfer_coordinator->try_begin_local (key))
        return 1;
    spot_node_runtime_t spots (node);

    constexpr std::size_t pending_handoff_capacity = 1024;
    const auto capacity_sentinel_source =
      zlink::routing_id_t::from ("pending-capacity-holder").to_hex ();
    if (test_case == parked_request_case_t::former_pending_capacity) {
        const auto sentinel_deadline = std::chrono::steady_clock::now () + std::chrono::hours (1);
        const auto filled =
          node->lane
            .run ([&] {
                for (std::uint64_t index = 1; index <= pending_handoff_capacity; ++index) {
                    const spot_node_builder_state_t::pending_handoff_request_key_t sentinel_key{
                      capacity_sentinel_source, 900, index, {}};
                    const auto [_, inserted] = node->pending_handoff_requests.emplace (
                      sentinel_key, spot_node_builder_state_t::pending_handoff_request_t{
                                      actor, {}, 10'000 + index, {}, {}, sentinel_deadline});
                    if (!inserted)
                        return false;
                }
                return node->pending_handoff_requests.size () == pending_handoff_capacity;
            })
            .get ();
        if (!filled) {
            spots.fail_remote_actor_transfer (actor, false, std::nullopt);
            return 9;
        }
    }

    // The reply callback may run after an early return; it owns its state.
    struct reply_state_t
    {
        std::mutex mutex;
        std::vector<zlink::message_t> parts;
        std::atomic_int calls{0};
    };
    const auto reply_state = std::make_shared<reply_state_t> ();
    auto &reply_mutex = reply_state->mutex;
    auto &reply_parts = reply_state->parts;
    const auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "parked-replay",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                  .domain = host::ready_domain_t::application};
    record.operation_id = host::call_id_t{41, 43};
    record.source_node_rid = zlink::routing_id_t::from (requester_rid);
    record.reply_route_id = 57;
    record.reply_token.host = reply_host;
    record.reply_token.local_reply = [reply_state] (const std::vector<zlink::message_t> &parts) {
        const std::lock_guard lock (reply_state->mutex);
        reply_state->parts = parts;
        reply_state->calls.fetch_add (1, std::memory_order_release);
        return true;
    };
    // Deliberately NO record.actor_route: the requester attached no fence.

    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string (spot->spot_id),
                                     .actor = actor};

    auto encoded = zlink::framework::detail::encode_actor_packet (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "actor",
                                   .message_name = "ParkedProbe",
                                   .correlation_id = "parked-request-1"},
      zlink::message_t::from (std::string ("ping")));
    auto request_parts = std::move (encoded).take_items ();

    auto parked = std::make_shared<std::promise<void>> ();
    auto parking = parked->get_future ();
    auto replied = std::make_shared<std::promise<void>> ();
    auto reply_ready = replied->get_future ();
    const auto original_reply = record.reply_token.local_reply;
    record.reply_token.local_reply = [original_reply, replied] (const auto &parts) {
        const auto accepted = original_reply (parts);
        replied->set_value ();
        return accepted;
    };
    bool terminal_deferred = false;
    if (!spots.dispatch_mesh_record (
          owner, record, request_parts,
          zlink::framework::runtime::decode_mesh_record_header (record, request_parts, false)
            .value (),
          provider, serializers, [parked] { parked->set_value (); }, &terminal_deferred)
        || !terminal_deferred
        || parking.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 2;
    const auto pending_count =
      node->lane.run_checked ([&] { return node->pending_handoff_requests.size (); }).get ();
    if (pending_count
          != (test_case == parked_request_case_t::former_pending_capacity
                ? pending_handoff_capacity + 1
                : 1)
        || handler_ran.load (std::memory_order_acquire)
        || reply_ready.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready)
        return 3;
    spots.fail_remote_actor_transfer (actor, false, std::nullopt);
    if (reply_ready.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 4;
    if (!handler_ran.load (std::memory_order_acquire))
        return 5;
    std::vector<zlink::message_t> delivered;
    {
        const std::lock_guard lock (reply_mutex);
        delivered = reply_parts;
    }
    messaging::message_parts_t reply_envelope (std::move (delivered));
    const auto reply_body = zlink::framework::detail::decode_actor_reply (reply_envelope);
    if (!reply_body || !reply_body.value () || reply_body.value ()->to_string () != "pong")
        return 9;
    const auto pending_settled =
      node->lane
        .run_checked ([&] {
            return test_case != parked_request_case_t::former_pending_capacity
                     ? node->pending_handoff_requests.empty ()
                     : node->pending_handoff_requests.size () == pending_handoff_capacity;
        })
        .get ();
    if (!pending_settled || reply_state->calls.load (std::memory_order_acquire) != 1)
        return 8;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return 0;
}

int parked_request_without_route_fence_receives_reply_after_replay ()
{
    // Requester local to the parking node (the SF-F2 shape).
    return parked_request_reply_case ("actor-a");
}

int pending_handoff_accepts_beyond_former_capacity ()
{
    return parked_request_reply_case ("actor-a", parked_request_case_t::former_pending_capacity);
}

int same_operation_from_distinct_source_lifecycles_has_distinct_pending_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("parking-node");
    const auto actor = test_actor_ref ("parking-node", "player", "same-operation", 1);
    const auto first_fence = runtime::protocol::actor_route_fence_t{
      "same-operation", 1, zlink::routing_id_t::from ("owner-a").to_bytes (), 4, 9, 12};
    const auto second_fence = runtime::protocol::actor_route_fence_t{
      "same-operation", 1, zlink::routing_id_t::from ("owner-b").to_bytes (), 5, 10, 13};
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    const spot_node_builder_state_t::pending_handoff_request_key_t first{
      zlink::routing_id_t::from ("source-lifecycle-a").to_hex (), 71, 73, first_fence};
    const spot_node_builder_state_t::pending_handoff_request_key_t second{
      zlink::routing_id_t::from ("source-lifecycle-b").to_hex (), 71, 73, second_fence};
    node->pending_handoff_requests.emplace (first,
                                            spot_node_builder_state_t::pending_handoff_request_t{
                                              actor, first_fence, 101, {}, {}, deadline});
    node->pending_handoff_requests.emplace (second,
                                            spot_node_builder_state_t::pending_handoff_request_t{
                                              actor, second_fence, 202, {}, {}, deadline});

    // A terminal reconstructing each source lifecycle identity reaches only
    // its own reply route, despite sharing the exact OperationId pair.
    const auto first_terminal = node->pending_handoff_requests.find (first);
    const auto second_terminal = node->pending_handoff_requests.find (second);
    if (node->pending_handoff_requests.size () != 2
        || first_terminal == node->pending_handoff_requests.end ()
        || second_terminal == node->pending_handoff_requests.end ()
        || first_terminal->second.reply_route_id != 101
        || second_terminal->second.reply_route_id != 202) {
        return 1;
    }
    node->pending_handoff_requests.erase (first_terminal);
    return node->pending_handoff_requests.size () == 1
               && node->pending_handoff_requests.contains (second)
             ? 0
             : 2;
}

int parked_cross_node_request_terminal_returns_to_parking_node ()
{
    // Requester on a different node (the SF-F7/SF-F11 shape): the handoff
    // terminal must be routed by the parking-node key, not the requester's
    // source-node key, or the pending reply token is never answered.
    return parked_request_reply_case ("actor-b");
}

int relayed_request_without_deferred_terminal_receives_follow_reply ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    // Regression pin (SF-F7/SF-F11 timeout): a cross-node actor_request that
    // reaches the committed Message Follow source right after the relocation
    // commit is relayed to the new owner and the relayed reply comes back
    // inline. The reply must reach the original reply token even when the
    // record was dispatched without a deferred terminal (the direct
    // transfer-window dispatch path passes none). Before the fix
    // dispatch_mesh_record returned before observing the relay task, so the
    // relayed reply was discarded and the requester's wire operation timed
    // out.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "follow-relay-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    node->root_services = services.build_provider ();
    auto provider = services.build_provider ();

    const auto local_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "follow-relay",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    node->native_node = local_host;

    const auto actor = test_actor_ref ("actor-a", "player", "moved-actor", 1);
    const auto key = std::string ("player:moved-actor");
    node->actor_types_by_id.emplace ("moved-actor", "player");

    // The requester's stale route names this node as the committed source.
    const runtime::protocol::actor_route_fence_t source_fence{
      "moved-actor",
      1,
      zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
      local_host->status ().lifecycle_generation (),
      3,
      5};
    const runtime::protocol::actor_route_fence_t target_fence{
      "moved-actor", 1, zlink::routing_id_t::from (std::string ("actor-b")).to_bytes (), 1, 4, 6};
    node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, test_actor_ref ("actor-b", "player", "moved-actor", 1),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-user"), "user"},
      target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30));

    spot_node_runtime_t spots (node);
    // The relay handler and the reply sink can run after an early return, so they own their state.
    auto relay_ran = std::make_shared<std::atomic_bool> (false);
    spots.on_actor_message_follow (
      [relay_ran] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                   const zlink::message_t &, std::chrono::milliseconds, const zlink::routing_id_t &,
                   const runtime::protocol::actor_route_fence_t &, std::uint8_t,
                   const runtime::protocol::wire_operation_id_t &,
                   std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_ran->store (true, std::memory_order_release);
          co_return result_t<std::optional<zlink::message_t>>::success (
            std::make_optional (zlink::message_t::from (std::string ("relayed-pong"))));
      });

    struct reply_state_t
    {
        std::mutex mutex;
        std::vector<zlink::message_t> parts;
    };
    auto reply = std::make_shared<reply_state_t> ();
    host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                  .domain = host::ready_domain_t::application};
    record.operation_id = host::call_id_t{11, 13};
    record.source_node_rid = zlink::routing_id_t::from (std::string ("actor-b"));
    record.actor_route = source_fence;
    record.reply_route_id = 17;
    record.reply_token.host = local_host;
    record.reply_token.local_reply = [reply] (const std::vector<zlink::message_t> &parts) {
        const std::lock_guard lock (reply->mutex);
        reply->parts = parts;
        return true;
    };

    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string ("actor-a-user"),
                                     .actor = actor};

    auto encoded = zlink::framework::detail::encode_actor_packet (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "actor",
                                   .message_name = "FollowProbe",
                                   .correlation_id = "follow-request-1"},
      zlink::message_t::from (std::string ("ping")));
    auto request_parts = std::move (encoded).take_items ();

    // Deliberately the deferred-terminal-less overload: this is the direct
    // dispatch shape used while the transfer window is still open.
    (void) spots.dispatch_mesh_record (
      owner, record, request_parts,
      zlink::framework::runtime::decode_mesh_record_header (record, request_parts, false).value (),
      provider, serializers);

    const auto reply_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    for (;;) {
        {
            const std::lock_guard lock (reply->mutex);
            if (!reply->parts.empty ())
                break;
        }
        if (std::chrono::steady_clock::now () >= reply_deadline)
            return 1;
        std::this_thread::yield ();
    }
    if (!relay_ran->load (std::memory_order_acquire))
        return 2;
    std::vector<zlink::message_t> delivered;
    {
        const std::lock_guard lock (reply->mutex);
        delivered = reply->parts;
    }
    messaging::message_parts_t reply_envelope (std::move (delivered));
    const auto reply_body = zlink::framework::detail::decode_actor_reply (reply_envelope);
    if (!reply_body || !reply_body.value () || reply_body.value ()->to_string () != "relayed-pong")
        return 5;
    return 0;
}
} // namespace zlink::framework::test::actor_gateway

/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/configuration/logging.hpp>
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
// All relay cases enter through the native receive-record path and observe
// its original reply token. The terminal callback marks parking completion.
struct native_actor_relay_fixture_t
{
    using host_t = zlink::framework::runtime::host::public_host_runtime_t;
    zlink::framework::logging_builder_t logging;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_provider_t provider;
    std::shared_ptr<host_t> reply_host;
    zlink::framework::runtime::host::ready_record_t owner;
    struct reply_probe_t
    {
        std::promise<std::vector<zlink::message_t>> replied;
        std::atomic_int calls{0};
    };
    std::shared_ptr<reply_probe_t> probe = std::make_shared<reply_probe_t> ();
    std::future<std::vector<zlink::message_t>> reply = probe->replied.get_future ();

    native_actor_relay_fixture_t (
      const std::shared_ptr<zlink::framework::detail::spot_node_builder_state_t> &node,
      const zlink::framework::actor_ref_t &actor) :
        owner{.owner_kind = zlink::framework::runtime::host::owner_kind_t::actor,
              .domain = zlink::framework::runtime::host::ready_domain_t::application,
              .actor = actor}
    {
        logging.use_file ("native-relay.flow").set_min_level (zlink::framework::log_level_t::debug);
        auto dispatch = node->dispatch;
        dispatch.message_flow (zlink::framework::message_flow_log_mode_t::detailed);
        zlink::framework::detail::dispatch_options_access_t::set_logger (
          dispatch, logging.create_logger ("native-relay"));
        zlink::framework::runtime::host::host_options_t options{
          .mesh = {.descriptor = {.mesh_name = "native-relay",
                                  .node_routing_id =
                                    zlink::routing_id_t::from ("actor-a").to_bytes (),
                                  .lifecycle_generation = 2,
                                  .descriptor_revision = 1,
                                  .advertised_endpoint = "tcp://127.0.0.1:0"},
                   .runtime_failures =
                     std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
          .object_stable_types = {"player"}};

        options.mesh.dispatch = dispatch;
        reply_host = std::make_shared<host_t> (std::move (options));
        node->dispatch = dispatch;
        zlink::framework::service_collection_t services;
        services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
        services.add_singleton<zlink::framework::serializer_registry_t> ();
        provider = services.build_provider ();
        node->root_services = provider;
        node->actor_types_by_id.emplace (std::string (actor.actor_id ().value ()), "player");
        node->native_node = reply_host;
    }

    ~native_actor_relay_fixture_t () { reply_host->close (); }

    void observe_reply (zlink::framework::runtime::host::reply_token_t &token)
    {
        auto original = std::move (token.local_reply);
        token.local_reply = [probe = probe, original = std::move (original)] (const auto &parts) {
            const auto invocation = probe->calls.fetch_add (1, std::memory_order_acq_rel);
            const auto accepted = !original || original (parts);
            if (invocation == 0)
                probe->replied.set_value (parts);
            return accepted;
        };
    }

    zlink::framework::runtime::host::receive_record_t
    request_record (const zlink::routing_id_t &source,
                    zlink::framework::runtime::host::call_id_t operation,
                    std::uint64_t reply_route)
    {
        namespace host = zlink::framework::runtime::host;
        host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                      .domain = host::ready_domain_t::application};
        record.operation_id = operation;
        record.source_node_rid = source;
        record.reply_route_id = reply_route;
        record.reply_token.host = reply_host;
        observe_reply (record.reply_token);
        return record;
    }

    bool dispatch_parts (zlink::framework::detail::spot_node_runtime_t &spots,
                         const zlink::framework::runtime::host::receive_record_t &record,
                         std::vector<zlink::message_t> &parts)
    {
        auto parked = std::make_shared<std::promise<void>> ();
        auto parking = parked->get_future ();
        bool deferred = false;
        const auto header =
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false);
        if (!header
            || !spots.dispatch_mesh_record (
              owner, record, parts, header.value (), provider, serializers,
              [parked] { parked->set_value (); }, &deferred))
            return false;
        return !deferred
               || parking.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    }

    bool dispatch (zlink::framework::detail::spot_node_runtime_t &spots,
                   const zlink::framework::runtime::host::receive_record_t &record,
                   const zlink::framework::runtime::messaging::envelope_header_t &header,
                   std::string payload)
    {
        auto encoded =
          zlink::framework::detail::encode_actor_packet (header, zlink::message_t::from (payload));
        auto parts = std::move (encoded).take_items ();
        return dispatch_parts (spots, record, parts);
    }
};

int local_relay_failure_returns_without_waiting_for_handoff_deadline ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    auto node =
      zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> ("actor-a");
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("actor-a", "player", "missing-actor", 1);
    spot_node_runtime_t spots (node);
    native_actor_relay_fixture_t fixture (node, actor);
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::request;
    header.message_name = "MissingActorProbe";
    auto record = fixture.request_record (zlink::routing_id_t::from ("actor-a"), {41, 43}, 47);
    if (!fixture.dispatch (spots, record, header, "ping")
        || fixture.reply.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 1;
    const auto failed =
      decode_actor_reply (runtime::messaging::message_parts_t (fixture.reply.get ()));
    const auto pending_empty =
      node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ();
    return !failed && failed.error_kind () == framework_error_kind_t::not_found
               && fixture.probe->calls.load (std::memory_order_acquire) == 1 && pending_empty
             ? 0
             : 2;
}

int local_relay_preserves_handoff_request_route (bool has_original_operation, bool expire_native)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "local-relay-route-worker");
    const auto actor = test_actor_ref ("actor-a", "player", "local-relay-actor", 1);
    const auto key = std::string ("player:local-relay-actor");
    if (!node->actor_transfer_coordinator->try_begin_local (key))
        return 1;

    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::request;
    header.message_name = "LocalRelayProbe";
    header.content_type = "application/octet-stream";
    header.correlation_id = "local-relay-correlation";
    const auto source = has_original_operation ? zlink::routing_id_t::from ("upstream-node")
                                               : zlink::routing_id_t::from (std::uint32_t{0});
    const auto operation = has_original_operation ? runtime::protocol::wire_operation_id_t{41, 43}
                                                  : runtime::protocol::wire_operation_id_t{};
    const auto source_fence =
      has_original_operation
        ? runtime::protocol::actor_route_fence_t{"local-relay-actor",
                                                 1,
                                                 zlink::routing_id_t::from ("actor-a").to_bytes (),
                                                 2,
                                                 3,
                                                 5}
        : runtime::protocol::actor_route_fence_t{};
    spot_node_runtime_t spots (node);
    native_actor_relay_fixture_t fixture (node, actor);
    runtime::host::pending_operation_t native_operation;
    auto native_completion = native_operation.completion->task ();
    const auto native_terminals = std::make_shared<std::atomic_int> (0);
    const auto native_terminal_observed =
      std::make_shared<task_completion_source_t<runtime::host::operation_completion_t>> ();
    detail::observe_task_terminal (
      native_completion, [native_terminals, native_terminal_observed] (const auto &result) {
          native_terminals->fetch_add (1, std::memory_order_acq_rel);
          native_terminal_observed->complete (result);
      });
    using operation_clock_t = runtime::foundation::operation_registry_t::clock_t;
    operation_clock_t::time_point registered_before;
    operation_clock_t::time_point registered_after;
    auto record = fixture.request_record (source, {operation.high, operation.low},
                                          has_original_operation ? 47 : 0);
    if (has_original_operation) {
        record.actor_route = source_fence;
        record.message_follow_hop_count = 3;
        if (!fixture.dispatch (spots, record, header, "payload"))
            return 6;
    } else {
        // The caller provides no operation identity. Use the real local host
        // registry and retain its original completion from before submission.
        fixture.reply_host->configure_stateful_dispatch (
          [] (const runtime::stateful::accepted_record_authority_query_t &query)
            -> task_t<std::optional<runtime::stateful::accepted_record_authority_t>> {
              co_return runtime::stateful::accepted_record_authority_t{
                {"native-relay-source-owner", 17, query.source_node_routing_id,
                 query.source_node_generation},
                37};
          });
        fixture.reply_host->start ();
        const auto native_actor = fixture.reply_host->create_actor ("player", "local-relay-actor");
        auto encoded =
          encode_actor_packet (header, zlink::message_t::from (std::string ("payload")));
        registered_before = operation_clock_t::now ();
        const auto submitted = finite_task_result (fixture.reply_host->request_to_actor (
          native_actor.ref (), encoded.items (), native_operation, std::chrono::seconds (30)));
        registered_after = operation_clock_t::now ();
        if (!submitted || !*submitted || submitted->value () != zlink::submit_result_t::ok)
            return 7;
        bool received = false;
        const auto dispatched = finite_task_result (fixture.reply_host->dispatch_ready (
          [&] (const auto &, const auto &incoming, auto parts, auto) {
              if (incoming.kind != runtime::host::record_kind_t::actor_request)
                  return;
              record = incoming;
              fixture.observe_reply (record.reply_token);
              received = fixture.dispatch_parts (spots, record, parts);
          }));
        if (!dispatched || !*dispatched || !received
            || native_operation.id == runtime::host::call_id_t{}
            || record.operation_id != native_operation.id || record.reply_route_id == 0)
            return 8;
    }
    auto backlog = spots.take_actor_handoff_backlog (actor);
    if (backlog.size () != 1 || !backlog.front ().is_request
        || backlog.front ().packet_name != "LocalRelayProbe"
        || backlog.front ().metadata.at ("__zlink.actorRequestId") != header.correlation_id
        // Relocation flow §9 keeps the absolute deadline at the client.
        || backlog.front ().metadata.contains (std::string (actor_handoff_deadline_key)))
        return 2;
    const auto &metadata = backlog.front ().metadata;
    if (has_original_operation
          ? metadata.at (std::string (actor_handoff_source_node_key)) != source.to_hex ()
              || metadata.at (std::string (actor_handoff_operation_high_key)) != "41"
              || metadata.at (std::string (actor_handoff_operation_low_key)) != "43"
              || metadata.at (std::string (actor_handoff_reply_route_key)) != "47"
              || metadata.at (std::string (actor_handoff_route_actor_id_key))
                   != source_fence.actor_id
              || metadata.at (std::string (actor_handoff_route_lease_generation_key)) != "5"
              || metadata.at (std::string (actor_handoff_hop_count_key)) != "3"
          : metadata.at (std::string (actor_handoff_source_node_key))
                != metadata.at (std::string (actor_handoff_parking_node_key))
              || metadata.at (std::string (actor_handoff_operation_high_key))
                   != std::to_string (record.operation_id.high)
              || metadata.at (std::string (actor_handoff_operation_low_key))
                   != std::to_string (record.operation_id.low)
              || metadata.at (std::string (actor_handoff_reply_route_key))
                   != std::to_string (record.reply_route_id))
        return 3;
    if (fixture.reply.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready
        || (!has_original_operation && native_completion.await_ready ()))
        return 10;
    if (has_original_operation) {
        auto send_header = header;
        send_header.kind = runtime::messaging::message_kind_t::command;
        send_header.message_name = "LocalRelaySendProbe";
        auto send_record = record;
        send_record.kind = runtime::host::record_kind_t::actor_send;
        send_record.operation_id = {};
        send_record.reply_route_id = 0;
        send_record.reply_token = {};
        if (!fixture.dispatch (spots, send_record, send_header, "send-payload"))
            return 9;
        // The follow handler can run after an early return, so it owns its flag and fence.
        auto replay_preserved_route = std::make_shared<std::atomic_bool> (false);
        spots.on_actor_message_follow (
          [replay_preserved_route, source_fence] (
            const actor_ref_t &, const runtime::messaging::envelope_header_t &replay_header,
            const zlink::message_t &, std::chrono::milliseconds, const zlink::routing_id_t &,
            const runtime::protocol::actor_route_fence_t &route, std::uint8_t hop,
            const runtime::protocol::wire_operation_id_t &,
            std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
              replay_preserved_route->store (replay_header.message_name == "LocalRelaySendProbe"
                                               && route == source_fence && hop == 3,
                                             std::memory_order_release);
              co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
          });
        const auto target = test_actor_ref ("actor-b", "player", "local-relay-actor", 1);
        const runtime::protocol::actor_route_fence_t target_fence{
          "local-relay-actor", 1, zlink::routing_id_t::from ("actor-b").to_bytes (), 2, 4, 6};
        const auto completed_move = finite_task_result (spots.complete_remote_actor_transfer (
          actor, target,
          spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-user"), "user"},
          source_fence,
          {target_fence.target_node_routing_id,
           target_fence.target_node_generation,
           {},
           target_fence.owner_lease_generation},
          target_fence.authority_owner_generation, "local-relay-hop"));
        if (!completed_move || !*completed_move
            || !replay_preserved_route->load (std::memory_order_acquire))
            return 11;
    }
    if (!has_original_operation) {
        // The real native registry owns this request's terminal; cancellation
        // must use that owner without a second Framework reply reservation.
        const auto native_record = backlog.front ().application_record;
        if (!native_record || !native_record->native_reply
            || !node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ())
            return 4;
        node->actor_transfer_coordinator->prepend_source_suffix (key, std::move (backlog));
        if (expire_native) {
            const auto original_operation = native_operation.id;
            const auto original_reply_route = record.reply_route_id;
            if (fixture.reply_host->transport ()
                    .expire_requests (registered_before + std::chrono::seconds (30)
                                      - operation_clock_t::duration (1))
                    .result ()
                    .value ()
                  != 0
                || native_completion.await_ready ())
                return 14;
            // Only the observation includes the wheel's 1ms rounding bound;
            // the request's registered timeout remains 30s.
            const auto expiry_time =
              registered_after + std::chrono::seconds (30) + std::chrono::milliseconds (1);
            const auto expiry_count =
              fixture.reply_host->transport ().expire_requests (expiry_time).result ().value ();
            if (expiry_count != 1) {
                std::cerr << "native deadline expiry count=" << expiry_count
                          << " registration_span_ns="
                          << std::chrono::duration_cast<std::chrono::nanoseconds> (
                               registered_after - registered_before)
                               .count ()
                          << " observation_ticks=" << expiry_time.time_since_epoch ().count ()
                          << " elapsed_since_registration_ns="
                          << std::chrono::duration_cast<std::chrono::nanoseconds> (
                               operation_clock_t::now () - registered_after)
                               .count ()
                          << '\n';
                return 15;
            }
            const auto terminal = finite_task_result (native_terminal_observed->task ());
            if (!terminal || *terminal
                || terminal->error_kind () != framework_error_kind_t::deadline_exceeded
                || native_operation.id != original_operation
                || record.operation_id != original_operation
                || record.reply_route_id != original_reply_route
                || native_terminals->load (std::memory_order_acquire) != 1)
                return 16;
            spots.cancel_local_actor_handoff (actor);
            const auto expired_at =
              std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
            return !native_record->native_reply && !native_record->complete_native (std::nullopt)
                       && native_terminals->load (std::memory_order_acquire) == 1
                       && fixture.probe->calls.load (std::memory_order_acquire) == 0
                       && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
                       && fixture.reply_host->transport ()
                              .expire_requests (expiry_time)
                              .result ()
                              .value ()
                            == 0
                       && node->lane.run ([&] { return node->pending_handoff_requests.empty (); })
                            .get ()
                     ? 0
                     : 17;
        }
        spots.cancel_local_actor_handoff (actor);
        const auto terminal = finite_task_result (native_terminal_observed->task ());
        if (!terminal || !*terminal
            || terminal->value ().record.operation_id != native_operation.id)
            return 12;
        const auto native_reply =
          decode_actor_reply (runtime::messaging::message_parts_t (terminal->value ().parts));
        if (native_reply || native_reply.error_kind () != framework_error_kind_t::unavailable
            || native_terminals->load (std::memory_order_acquire) != 1)
            return 13;
        const auto expired_at =
          std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
        return !native_record->native_reply && !native_record->complete_native (std::nullopt)
                   && fixture.probe->calls.load (std::memory_order_acquire) == 0
                   && native_terminals->load (std::memory_order_acquire) == 1
                   && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
                   && node->lane.run ([&] { return node->pending_handoff_requests.empty (); })
                        .get ()
                 ? 0
                 : 8;
    }
    const auto recorded =
      node->lane
        .run ([&] {
            if (node->pending_handoff_requests.size () != 1)
                return false;
            const auto &pending = *node->pending_handoff_requests.begin ();
            return pending.first.source_fence
                     == record.actor_route.value_or (runtime::protocol::actor_route_fence_t{})
                   && pending.first.operation_high == record.operation_id.high
                   && pending.first.operation_low == record.operation_id.low
                   && pending.second.reply_route_id == record.reply_route_id
                   && pending.second.reply_token.has_value ()
                   && pending.second.request_header.correlation_id == header.correlation_id
                   && pending.second.request_header.message_name == header.message_name;
        })
        .get ();
    if (fixture.reply.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready)
        return 10;
    if (!recorded)
        return 4;
    const auto expired_at = std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
    if (spots.cleanup_expired_actor_admissions_at (expired_at) == 0)
        return 5;
    if (fixture.reply.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 8;
    auto completed = fixture.reply.get ();
    const auto reply = decode_actor_reply (runtime::messaging::message_parts_t (completed));

    return !reply && reply.error_kind () == framework_error_kind_t::unavailable
               && fixture.probe->calls.load (std::memory_order_acquire) == 1
               && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
               && fixture.probe->calls.load (std::memory_order_acquire) == 1
               && node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ()
             ? 0
             : 8;
}
} // namespace zlink::framework::test::actor_gateway

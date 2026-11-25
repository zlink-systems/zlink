/* SPDX-License-Identifier: Elastic-2.0 */

#include "mailbox_claim_fixture.hpp"
#include <service_wire_codec.hpp>
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
void verify_logical_multicast_continues_after_one_target_failure ()
{
    auto source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("multicast-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"}});
    auto target_descriptor = descriptor ("z-multicast-target");
    target_descriptor.channels = {{"framework.spot", 100}};
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (target_descriptor),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->start ();
    target->start ();
    const std::string source_spot_id = source->entry_spot ().spot_id ();
    assert (zlink::framework::detail::is_framework_entry_spot_id (source_spot_id));

    const auto target_status = target->status ();
    const auto source_status = source->status ();
    assert (target->connect_peer (source_status.local_endpoint (), source_status.routing_id ()));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (
      (!ready_peer (source->transport ().topology (), target_status.routing_id ().to_bytes ())
       || !ready_peer (target->transport ().topology (), source_status.routing_id ().to_bytes ()))
      && mesh::service_liveness_registry_t::clock_t::now () < connect_deadline) {
        (void) source->dispatch_ready (noop_dispatch);
        (void) target->dispatch_ready (noop_dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (target_status.routing_id ().to_bytes ()));

    auto unavailable = descriptor ("a-unavailable-target");
    unavailable.state = mesh::service_node_state_t::serving;
    unavailable.channels = {{"framework.spot", 100}};
    assert (source->transport ().admit_peer (unavailable, bytes ("unavailable-connection"),
                                             mesh::service_liveness_registry_t::clock_t::now ())
            == mesh::peer_admission_result_t::admitted);
    const auto fanout_targets = source->transport ().topology ().peers ();
    assert (fanout_targets.size () >= 2);
    assert (fanout_targets.front ().descriptor.node_routing_id == bytes ("a-unavailable-target"));

    auto publisher_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("m6b-mesh");
    std::atomic_bool observed_failure{false};
    std::atomic_int dispatch_error_count{0};
    std::atomic_int empty_target_count{0};
    zlink::framework::logging_builder_t logging;
    logging.use_provider (
      "logical-multicast-dispatch-error", [&] (const zlink::framework::log_record_t &record) {
          const auto field = [&] (std::string_view key) -> std::optional<std::string_view> {
              for (const auto &candidate : record.fields) {
                  if (candidate.key == key)
                      return candidate.value;
              }
              return std::nullopt;
          };
          if (field ("event_id") != "zlink.dispatch_error" || field ("surface") != "spot"
              || field ("topic") != "reward" || field ("channel") != "framework.spot"
              || field ("mesh") != "m6b-mesh")
              return;
          dispatch_error_count.fetch_add (1, std::memory_order_relaxed);
          if (!field ("target_rid") || field ("target_rid")->empty ())
              empty_target_count.fetch_add (1, std::memory_order_relaxed);
          if (field ("kind") == "send" && field ("outcome") == "failed"
              && field ("action") == "drop" && field ("reason") == "stale_target"
              && field ("target_rid") == "a-unavailable-target" && field ("topic") == "reward"
              && field ("channel") == "framework.spot" && field ("mesh") == "m6b-mesh"
              && !field ("phase") && !field ("source_rid") && !field ("packet")
              && !field ("exception") && !field ("error_type") && !field ("error_message")) {
              observed_failure.store (true, std::memory_order_release);
          }
      });
    zlink::framework::detail::dispatch_options_access_t::set_logger (
      publisher_state->dispatch, logging.create_logger ("logical-multicast-dispatch-error"));
    zlink::framework::detail::spot_node_runtime_t publisher_runtime (publisher_state);
    publisher_runtime.attach_native_node (source);
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::spot_publisher_client_t publisher (publisher_runtime.manager (), serializers);
    const auto publish_terminal =
      publisher.publish<std::string> ("framework.spot", "reward", "payload")
        .metadata ("trace-key", "multicast-snapshot")
        .async ()
        .result ();
    assert (publish_terminal);

    const auto diagnostic_deadline = std::chrono::steady_clock::now () + 5s;
    while (!observed_failure.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < diagnostic_deadline) {
        std::this_thread::sleep_for (1ms);
    }
    assert (observed_failure.load (std::memory_order_acquire));
    assert (dispatch_error_count.load (std::memory_order_acquire) == 1);
    assert (empty_target_count.load (std::memory_order_acquire) == 0);

    const auto receive_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
        == 0
      && mesh::service_liveness_registry_t::clock_t::now () < receive_deadline) {
        const auto pumped = target->transport ()
                              .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                              .result ()
                              .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped == mesh::raw_mesh_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == 1);

    auto claim = zlink::framework::test::claim_mailbox (
      target->transport ().mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
    assert (claim && claim->records.size () == 1);
    const auto &received = (*claim->records.front ());
    assert (received.parts ().size () == 3);
    namespace wire = zlink::framework::runtime::protocol::generated::detail;
    wire::logicalMulticast_23_t header;
    wire::reader_t reader (protocol::message_bytes (received.parts ().front ()));
    assert (wire::decode_head_logicalMulticast_23 (reader, header) == wire::error_code::ok);
    assert (header.flags == static_cast<std::uint8_t> (protocol::flag::metadata));
    assert (header.channelName.value == "framework.spot");
    assert (header.topic.value == "reward");
    assert (header.sourceSpotId.value == source_spot_id);
    std::map<std::string, std::string> metadata;
    assert (zlink::framework::detail::mesh_metadata_codec_t::decode (
      received.parts ()[1].to_bytes (), metadata));
    assert ((metadata == std::map<std::string, std::string>{{"trace-key", "multicast-snapshot"}}));
    const auto payload =
      protocol::decode_application_payload (protocol::message_bytes (received.parts ()[2]));
    const auto payload_parts = protocol::decode_application_parts (payload);
    assert (payload_parts.size () == 1);
    assert (payload_parts.front ().size () != 0);
    assert (target->transport ().mailbox ().release (*claim));

    source->close ();
    target->close ();
}
void verify_relocated_source_reply_failure_keeps_terminal_record ()
{
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relocated-reply-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"m6b-mesh", "relocated-reply-target", {"actor"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (objects, {stateful::object_kind_t::actor,
                                               "relocated-reply-actor",
                                               "actor",
                                               std::nullopt,
                                               {},
                                               false,
                                               false});
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [] (stateful::accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{
            {"source-owner", 1, query.source_node_routing_id, query.source_node_generation}, 1};
      });
    const protocol::actor_route_fence_t actor_fence{
      "relocated-reply-actor",           actor.object_generation,
      target_descriptor.node_routing_id, target_descriptor.lifecycle_generation,
      actor.authority_owner_generation,  1};
    const protocol::wire_operation_id_t operation{79, 90};
    const auto unreachable_source = bytes ("unreachable-source");
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "actor:relocated-reply-actor", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_actor_message_header (protocol::command::actorRequest, std::nullopt,
                                                actor_fence, operation, 99),
         protocol::encode_application_payload (
           {"ActorPacket", "application/json", bytes ("request")})}),
      unreachable_source, 99, 1, std::make_pair (operation.high, operation.low))));
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);

    const protocol::reply_relay_t relay{operation,
                                        99,
                                        {0x101, 0x202},
                                        3,
                                        {"coordinator", 4, bytes ("coordinator-rid"), 5, "store"},
                                        1,
                                        1,
                                        0,
                                        protocol::framework_error_code::none};
    const protocol::application_payload_t reply{"ActorReply", "application/json", bytes ("reply")};
    target.close ();
    assert (!dispatch.complete_relocated_source_async (actor, 1, relay, reply).result ().value ());

    const auto [claim_error, delivery] = dispatch.try_claim (actor);
    assert (claim_error == stateful::stateful_error_t::none && delivery);
    stateful::with_accepted_application (delivery->turn, [] (auto, const auto &application) {
        assert (application.payload_bytes () == bytes ("request"));
    });
    assert (objects.complete_claim (actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
    target.close ();
}
void verify_raw_terminal_reply_relay ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relay-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relay-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!ready_peer (source.topology (), target_descriptor.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));

    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 7, target_descriptor.node_routing_id, target_descriptor.lifecycle_generation,
      "store-3"};
    const protocol::reply_relay_t relay{
      {1, 2}, 3, {4, 5}, 6, coordinator, 8, 9, 0, protocol::framework_error_code::none};
    const protocol::application_payload_t payload{"ReplyPacket", "application/json",
                                                  bytes ("relay")};
    assert (target.send_reply_relay (source_descriptor.node_routing_id, relay, payload)
              .result ()
              .value ());

    bool received = false;
    while (!received && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pumped =
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = source.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                  16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &slot : claim->records) {
            const auto &record = *slot;
            if (protocol::decode_header (protocol::message_bytes (record.parts ().front ())).kind
                != protocol::command::replyRelay)
                continue;
            assert (
              protocol::decode_reply_relay (protocol::message_bytes (record.parts ().front ()))
              == relay);
            assert (protocol::decode_application_payload (
                      protocol::message_bytes (record.parts ().back ()))
                    == payload);
            received = true;
        }
        assert (source.mailbox ().release (*claim));
    }
    assert (received);
    source.close ();
    target.close ();
}
void verify_durable_reply_relay_single_winner ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("terminal-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("terminal-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    auto deadline = std::chrono::steady_clock::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!ready_peer (source.topology (), target_descriptor.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::relocation_id_t relocation{301, 302};
    const protocol::wire_operation_id_t operation{401, 402};
    const protocol::relocation_coordinator_fence_t coordinator{
      "terminal-coordinator", 9, target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation, "store-terminal"};
    const protocol::request_source_fence_t request_source{"request-owner", 10,
                                                          source_descriptor.node_routing_id,
                                                          source_descriptor.lifecycle_generation};
    const protocol::reply_relay_t relay{
      operation, 501, relocation, 6, coordinator, 7, 8, 0, protocol::framework_error_code::none};
    const protocol::application_payload_t reply{"TerminalReply", "application/json",
                                                bytes ("completed")};

    stateful::raw_relocation_replay_coordinator_t source_coordinator (source, 1ms, 1ms);
    stateful::raw_relocation_replay_coordinator_t target_coordinator (target, 1ms, 1ms);
    assert (!source_coordinator.next_activity ());
    assert (!target_coordinator.next_activity ());
    int completion_count = 0;
    assert (source_coordinator.register_terminal_source (
      {relocation, coordinator, operation, request_source, target_descriptor.node_routing_id,
       target_descriptor.lifecycle_generation, 6, 7, 8, 501,
       [&] (const protocol::reply_relay_t &received,
            const std::optional<protocol::application_payload_t> &payload) {
           ++completion_count;
           return received == relay && payload && *payload == reply;
       }}));
    int persisted_ack_count = 0;
    protocol::reply_relay_ack_status_t persisted_status{};
    assert (target_coordinator.register_terminal_target (
      {relay, request_source, reply,
       [&] (protocol::reply_relay_ack_status_t status) {
           ++persisted_ack_count;
           persisted_status = status;
           return true;
       },
       [] { return true; }}));
    assert (target_coordinator.pending_terminal_relays () == 1);
    assert (target_coordinator.next_activity ()
            == stateful::raw_relocation_replay_coordinator_t::clock_t::time_point::min ());
    assert (target_coordinator.terminal_retained_bytes () > 0);
    const protocol::reply_relay_ack_t stale_ack{
      relocation,           coordinator,    operation,
      relay.reply_route_id, request_source, protocol::reply_relay_ack_status_t::terminal_received};
    auto stale_ack_record = mesh::service_mailbox_record_t::from_parts (
      "target", mesh::service_mailbox_domain_t::infrastructure,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_reply_relay_ack (stale_ack)}),
      source_descriptor.node_routing_id, std::nullopt, source_descriptor.lifecycle_generation + 1);
    assert (await_task (target_coordinator.process (stale_ack_record))
            == stateful::raw_relocation_replay_result_t::stale_fence);
    auto wrong_route_ack = stale_ack;
    ++wrong_route_ack.reply_route_id;
    auto wrong_route_ack_record = mesh::service_mailbox_record_t::from_parts (
      "target", mesh::service_mailbox_domain_t::infrastructure,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_reply_relay_ack (wrong_route_ack)}),
      source_descriptor.node_routing_id, std::nullopt, source_descriptor.lifecycle_generation);
    assert (await_task (target_coordinator.process (wrong_route_ack_record))
            == stateful::raw_relocation_replay_result_t::stale_fence);

    const auto receive = [&] (mesh::raw_mesh_node_owner_t &node,
                              stateful::raw_relocation_replay_coordinator_t &coordinator) {
        const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
        while (std::chrono::steady_clock::now () < receive_deadline) {
            const auto pumped =
              node.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
            const auto result = await_task (coordinator.pump_one ());
            if (result != stateful::raw_relocation_replay_result_t::no_data)
                return result;
        }
        return stateful::raw_relocation_replay_result_t::no_data;
    };

    const auto first_send_time = stateful::raw_relocation_replay_coordinator_t::clock_t::now ();
    assert (await_task (target_coordinator.retry_terminal_relays (first_send_time)) == 1);
    assert (target_coordinator.next_activity () == first_send_time + 1ms);
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::terminal_received);
    assert (completion_count == 1);
    assert (source_coordinator.next_activity ());

    bool first_ack_dropped = false;
    deadline = std::chrono::steady_clock::now () + 2s;
    while (!first_ack_dropped && std::chrono::steady_clock::now () < deadline) {
        const auto pumped =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                  16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &slot : claim->records) {
            const auto &record = *slot;
            if (protocol::decode_header (protocol::message_bytes (record.parts ().front ())).kind
                == protocol::command::replyRelayAck) {
                assert (protocol::decode_reply_relay_ack (
                          protocol::message_bytes (record.parts ().front ()))
                          .status
                        == protocol::reply_relay_ack_status_t::terminal_received);
                first_ack_dropped = true;
            }
        }
        assert (target.mailbox ().release (*claim));
    }
    assert (first_ack_dropped);
    assert (target_coordinator.pending_terminal_relays () == 1);

    auto conflicting_reply = reply;
    conflicting_reply.payload_bytes () = bytes ("conflicting");
    assert (target.send_reply_relay (source_descriptor.node_routing_id, relay, conflicting_reply)
              .result ()
              .value ());
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::conflicting_duplicate);
    assert (completion_count == 1);

    assert (await_task (target_coordinator.retry_terminal_relays (first_send_time + 2ms)) == 1);
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::terminal_duplicate);
    assert (completion_count == 1);
    assert (receive (target, target_coordinator)
            == stateful::raw_relocation_replay_result_t::relay_acknowledged);
    assert (persisted_ack_count == 1);
    assert (persisted_status == protocol::reply_relay_ack_status_t::already_terminal);
    assert (target_coordinator.pending_terminal_relays () == 0);
    assert (!target_coordinator.next_activity ());
    assert (target_coordinator.terminal_retained_bytes () == 0);

    auto expiry_relay = relay;
    expiry_relay.operation = {403, 404};
    expiry_relay.reply_route_id = 502;
    int expiry_persisted = 0;
    assert (target_coordinator.register_terminal_target (
      {expiry_relay, request_source, std::nullopt,
       [] (protocol::reply_relay_ack_status_t) { return true; },
       [&] {
           ++expiry_persisted;
           return true;
       }}));
    auto wrong_source = request_source;
    ++wrong_source.lease_generation;
    assert (!target_coordinator.confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, wrong_source));
    assert (target_coordinator.pending_terminal_relays () == 1);
    assert (target_coordinator.confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, request_source));
    assert (expiry_persisted == 1);
    assert (target_coordinator.pending_terminal_relays () == 0);
    assert (target_coordinator.terminal_retained_bytes () == 0);

    assert (source_coordinator.reap_terminal_tombstones (
              stateful::raw_relocation_replay_coordinator_t::clock_t::now () + 2ms)
            == 1);
    assert (!source_coordinator.next_activity ());
    assert (
      target.send_reply_relay (source_descriptor.node_routing_id, relay, reply).result ().value ());
    assert (receive (source, source_coordinator)
            == stateful::raw_relocation_replay_result_t::not_registered);

    stateful::raw_relocation_replay_coordinator_t unbounded_target (target, 1ms, 1ms);
    assert (unbounded_target.register_terminal_target (
      {relay, request_source, reply, [] (protocol::reply_relay_ack_status_t) { return true; },
       [] { return true; }}));
    auto non_ok = relay;
    non_ok.terminal_result = 101;
    non_ok.failure_code = protocol::framework_error_code::requestFailed;
    assert (!target_coordinator.register_terminal_target (
      {non_ok, request_source, reply, [] (protocol::reply_relay_ack_status_t) { return true; },
       [] { return true; }}));
    source.close ();
    target.close ();
}

void verify_aggregate_saved_request_terminal_identity ()
{
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("aggregate-terminal-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    target->objects ().configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    target->configure_stateful_dispatch (
      [] (stateful::accepted_record_authority_query_t)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      });
    const protocol::relocation_coordinator_fence_t coordinator{"source-owner", 1, {'s'}, 1, "v1"};
    const protocol::request_source_fence_t source{"source-owner", 1, {'s'}, 1};
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_repository_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<zlink::framework::owner_lease_claimed_t> (
                         store->claim_owner_lease (source.owner_id, 15s).result ().value ())
                         .token;
    assert (owner.lease_generation == static_cast<std::int64_t> (source.lease_generation));
    const zlink::framework::mesh_node_descriptor_t source_node{
      .mesh_name = "mesh",
      .rid = zlink::routing_id_t::from (source.node_routing_id),
      .lifecycle_generation = source.node_generation,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .state = zlink::framework::framework_runtime_state_t::serving,
      .security_identity = "test",
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
    assert (
      store->update_mesh_node (source_node, zlink::framework::location_write_intent_t::new_claim)
        .result ()
        .value ()
        .status
      == zlink::framework::location_write_status_t::stored);
    target->configure_user_spot_operations (
      store,
      [] (const auto &, const auto &, const auto &)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          co_return host::user_spot_materialize_result_t{};
      });
    protocol::relocation_prepare_t prepare;
    prepare.relocation = {0x1786, 3};
    prepare.target_attempt_generation = 1;
    prepare.coordinator = coordinator;
    std::vector<stateful::frozen_object_state_t> frozen;
    std::vector<stateful::object_ref_t> targets;
    std::vector<protocol::frozen_record_t> requests;
    for (std::uint64_t index = 0; index != 2; ++index) {
        const stateful::object_ref_t owner{stateful::object_kind_t::actor,
                                           "aggregate-" + std::to_string (index),
                                           1,
                                           1,
                                           "mesh",
                                           "source"};
        auto relocated = owner;
        relocated.node_id = "aggregate-terminal-target";
        ++relocated.authority_owner_generation;
        targets.push_back (relocated);
        protocol::frozen_application_record_t request;
        request.kind = protocol::frozen_record_kind_t::actor_request;
        request.source_kind = protocol::frozen_source_kind_t::node;
        request.source = source;
        // Exercise both halves of the existing terminal sequence mapping.
        request.operation = index == 0 ? protocol::wire_operation_id_t{0x1786, 0}
                                       : protocol::wire_operation_id_t{0x1786, 0x1771};
        request.operation_kind = 4;
        request.reply_route_id = index + 77;
        request.body = protocol::frozen_actor_application_body_t{
          {owner.key, 1, {'s'}, 1, 1, 1}, {"Request", "application/json", {0x41}}};
        requests.push_back (protocol::encode_frozen_application_record (request));
        frozen.push_back ({owner, "actor", {}, {{index + 1, {}, 0, {}, requests.back ()}}, {}});
    }
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "aggregate-spot", 1, 1, "mesh", "source"};
    auto target_spot = spot;
    target_spot.node_id = "aggregate-terminal-target";
    ++target_spot.authority_owner_generation;
    frozen.push_back ({spot, "spot", {}, {}, {}});
    targets.push_back (target_spot);
    const stateful::relocation_restore_identity_t identity{"aggregate-terminal"};
    assert (host::public_host_runtime_test_access_t::restore_saved (*target, prepare, frozen,
                                                                    targets, identity));
    const protocol::application_payload_t reply{"Reply", "application/json", {0x42}};
    for (std::size_t index = 0; index != requests.size (); ++index) {
        assert (target->objects ().commit_relocation_restore (targets[index], identity)
                == stateful::stateful_error_t::none);
        assert (
          host::public_host_runtime_test_access_t::complete_saved (*target, targets[index], reply));
        const auto &request = requests[index];
        const auto sequence = index == 0 ? 0x1786u : 0x1771u;
        const protocol::reply_relay_t expected{request.operation,
                                               *request.reply_route_id,
                                               prepare.relocation,
                                               1,
                                               coordinator,
                                               index + 1,
                                               sequence,
                                               0,
                                               protocol::framework_error_code::none};
        const bool same_terminal = target->relocation_wire ().register_terminal_target (
          {expected, source, reply, [] (protocol::reply_relay_ack_status_t) { return true; },
           [] { return true; }});
        std::cerr << "aggregate-terminal participant=" << index + 1
                  << " expected-sequence=" << sequence << " matches=" << same_terminal << '\n';
        assert (same_terminal);
    }
    assert (target->relocation_wire ().pending_terminal_relays () == 2);
}

void verify_restored_request_completes_local_native_alias ()
{
    using namespace zlink::framework;
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("alias-source-target"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    mesh::raw_mesh_node_owner_t coordinator_node (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("alias-coordinator"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    auto lease = std::make_shared<const location_owner_token_t> (
      location_owner_token_t{"alias-source-owner", 1});
    target->transport ().configure_request_sources (nullptr, [lease] { return lease; });
    target->objects ().configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token) -> task_t<stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    target->configure_stateful_dispatch (
      [] (stateful::accepted_record_authority_query_t)
        -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      });
    target->start ();
    coordinator_node.start ();
    const auto local = target->transport ().topology ().local_descriptor ();
    const auto remote = coordinator_node.topology ().local_descriptor ();
    assert (target->transport ().connect_peer (coordinator_node.endpoint (), remote));
    const auto admission_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!ready_peer (target->transport ().topology (), remote.node_routing_id)
            || !ready_peer (coordinator_node.topology (), local.node_routing_id))
           && std::chrono::steady_clock::now () < admission_deadline) {
        (void) target->transport ().observe_routes ();
        (void) coordinator_node.observe_routes ();
        (void) await_task (target->transport ().pump_one (std::chrono::steady_clock::now ()));
        (void) await_task (coordinator_node.pump_one (std::chrono::steady_clock::now ()));
    }
    assert (ready_peer (target->transport ().topology (), remote.node_routing_id));
    const protocol::wire_operation_id_t original{0x2507, 0x77};
    constexpr std::uint64_t reply_route = 0x1786;
    const protocol::request_source_fence_t source{lease->owner_id, 1, local.node_routing_id,
                                                  local.lifecycle_generation};
    const protocol::relocation_coordinator_fence_t coordinator{
      "alias-coordinator-owner", 2, remote.node_routing_id, remote.lifecycle_generation, "v1"};
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "alias-actor", 1, 1, local.mesh_name, "previous-owner"};
    auto restored = owner;
    restored.node_id = "alias-source-target";
    ++restored.authority_owner_generation;
    const protocol::actor_route_fence_t old_route{
      owner.key, 1, remote.node_routing_id, remote.lifecycle_generation, 1, 1};
    std::atomic<int> completions{0};
    std::vector<std::uint8_t> completed_payload;
    assert (await_task (target->transport ().request_to_actor (
      remote.node_routing_id, std::nullopt, old_route,
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("request")}), 10s,
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto payload = std::move (completion_payload.value ());
          assert (terminal == foundation::operation_terminal_t::completed);
          const auto parts = protocol::decode_application_parts (
            protocol::decode_application_payload (payload.to_bytes ()));
          assert (parts.size () == 1);
          completed_payload = parts.front ().to_bytes ();
          ++completions;
      },
      original, std::nullopt, reply_route)));
    assert (!target->transport ().operation_pending ({original.high, original.low}));
    assert (target->transport ().operation_pending ({local.lifecycle_generation, reply_route}));
    assert (target->transport ().relocated_request_source (original, reply_route) == source);
    auto wrong_operation = original;
    ++wrong_operation.low;
    assert (!target->transport ().relocated_request_source (wrong_operation, reply_route));
    assert (!target->transport ().relocated_request_source (original, reply_route + 1));

    protocol::actor_message_header_t header;
    header.target = old_route;
    header.operation = original;
    header.correlation = reply_route;
    auto accepted = stateful::accepted_actor_record (
      header, source,
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("request")}), true);
    const auto request = protocol::encode_frozen_application_record (accepted);
    protocol::relocation_prepare_t prepare;
    prepare.relocation = {0x2507, 0x88};
    prepare.target_attempt_generation = 1;
    prepare.coordinator = coordinator;
    const stateful::relocation_restore_identity_t identity{"alias-relocation"};
    const auto dispatch = [] (const auto &, const auto &, auto, auto) {};
    const auto reply =
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("reply")});
    protocol::reply_relay_t incorrect{wrong_operation,
                                      reply_route,
                                      prepare.relocation,
                                      1,
                                      coordinator,
                                      1,
                                      original.low,
                                      0,
                                      protocol::framework_error_code::none};
    assert (
      await_task (target->transport ().send_reply_relay (local.node_routing_id, incorrect, reply)));
    (void) target->dispatch_ready (dispatch);
    assert (completions == 0);
    incorrect.operation = original;
    ++incorrect.reply_route_id;
    assert (
      await_task (target->transport ().send_reply_relay (local.node_routing_id, incorrect, reply)));
    (void) target->dispatch_ready (dispatch);
    assert (completions == 0);
    assert (target->transport ().operation_pending ({local.lifecycle_generation, reply_route}));
    assert (host::public_host_runtime_test_access_t::restore_saved (
      *target, prepare, {{owner, "actor", {}, {{1, {}, 0, {}, request}}, {}}}, {restored},
      identity));
    assert (target->objects ().commit_relocation_restore (restored, identity)
            == stateful::stateful_error_t::none);
    const bool handler_completed =
      host::public_host_runtime_test_access_t::complete_saved (*target, restored, reply);
    assert (handler_completed);
    assert (target->relocation_wire ().pending_terminal_relays () == 1);
    std::cerr << "restored-alias source=alias-source-target target=alias-source-target"
              << " coordinator=alias-coordinator original=" << original.high << '/' << original.low
              << " native=" << local.lifecycle_generation << '/' << reply_route
              << " alias=registered handler-completed=" << handler_completed << '\n';
    const auto deadline = std::chrono::steady_clock::now () + 1s;
    while ((completions != 1 || target->relocation_wire ().pending_terminal_relays () != 0)
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) await_task (coordinator_node.pump_one (std::chrono::steady_clock::now ()));
    }
    std::cerr << "restored-alias completions=" << completions << " pending-terminal-relays="
              << target->relocation_wire ().pending_terminal_relays () << '\n';
    assert (completions == 1);
    assert (completed_payload == bytes ("reply"));
    assert (target->relocation_wire ().pending_terminal_relays () == 0);
    assert (!target->transport ().operation_pending ({local.lifecycle_generation, reply_route}));
    target->close ();
    coordinator_node.close ();
}
void verify_public_host_dispatches_durable_reply_relay ()
{
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("host-terminal-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("host-terminal-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->start ();
    target->start ();
    const auto source_status = source->status ();
    const auto target_status = target->status ();
    assert (source->connect_peer (target->transport ().endpoint (), target_status.routing_id ()));

    const protocol::relocation_id_t relocation{601, 602};
    const protocol::wire_operation_id_t operation{603, 604};
    const protocol::relocation_coordinator_fence_t coordinator{
      "host-coordinator", 11, target_status.routing_id ().to_bytes (),
      target_status.lifecycle_generation (), "host-store"};
    const protocol::request_source_fence_t request_source{"host-source-owner", 12,
                                                          source_status.routing_id ().to_bytes (),
                                                          source_status.lifecycle_generation ()};
    const protocol::reply_relay_t relay{
      operation, 605, relocation, 13, coordinator, 14, 15, 0, protocol::framework_error_code::none};
    int completions = 0;
    int acknowledgements = 0;
    assert (source->relocation_wire ().register_terminal_source (
      {relocation, coordinator, operation, request_source, target_status.routing_id ().to_bytes (),
       target_status.lifecycle_generation (), 13, 14, 15, 605,
       [&] (const protocol::reply_relay_t &received,
            const std::optional<protocol::application_payload_t> &payload) {
           ++completions;
           return received == relay && !payload;
       }}));
    assert (target->relocation_wire ().register_terminal_target (
      {relay, request_source, std::nullopt,
       [&] (protocol::reply_relay_ack_status_t) {
           ++acknowledgements;
           return true;
       },
       [] { return true; }}));
    const auto dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                              std::vector<zlink::message_t>, auto) {};
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while ((completions != 1 || acknowledgements != 1)
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
        (void) target->dispatch_ready (dispatch);
    }
    assert (completions == 1);
    assert (acknowledgements == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    auto duplicate_relay = relay;
    duplicate_relay.operation = {606, 607};
    duplicate_relay.reply_route_id = 608;
    int duplicate_completions = 0;
    int duplicate_acknowledgements = 0;
    assert (source->relocation_wire ().register_terminal_source (
      {relocation, coordinator, duplicate_relay.operation, request_source,
       target_status.routing_id ().to_bytes (), target_status.lifecycle_generation (), 13, 14, 15,
       608,
       [&] (const protocol::reply_relay_t &received,
            const std::optional<protocol::application_payload_t> &payload) {
           ++duplicate_completions;
           return received == duplicate_relay && !payload;
       }}));
    assert (target->relocation_wire ().register_terminal_target (
      {duplicate_relay, request_source, std::nullopt,
       [&] (protocol::reply_relay_ack_status_t) {
           ++duplicate_acknowledgements;
           return true;
       },
       [] { return true; }}));

    const auto first = stateful::raw_relocation_replay_coordinator_t::clock_t::now ();
    assert (await_task (target->relocation_wire ().retry_terminal_relays (first)) == 1);
    while (await_task (
             source->transport ().pump_one (mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (source->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::terminal_received);
    assert (duplicate_completions == 1);

    bool dropped = false;
    while (!dropped) {
        (void) await_task (
          target->transport ().pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        auto claim = target->transport ().mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 64 * 1024);
        if (!claim)
            continue;
        dropped = protocol::decode_header (
                    protocol::message_bytes (claim->records.front ()->parts ().front ()))
                    .kind
                  == protocol::command::replyRelayAck;
        assert (target->transport ().mailbox ().release (*claim));
    }
    assert (target->relocation_wire ().pending_terminal_relays () == 1);
    assert (await_task (target->relocation_wire ().retry_terminal_relays (first + 2s)) == 1);
    while (await_task (
             source->transport ().pump_one (mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (source->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::terminal_duplicate);
    assert (duplicate_completions == 1);
    while (await_task (
             target->transport ().pump_one (mesh::service_liveness_registry_t::clock_t::now ()))
           == mesh::raw_mesh_pump_result_t::no_data) {
    }
    assert (await_task (target->relocation_wire ().pump_one ())
            == stateful::raw_relocation_replay_result_t::relay_acknowledged);
    assert (duplicate_acknowledgements == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    auto expiry_relay = relay;
    expiry_relay.operation = {609, 610};
    expiry_relay.reply_route_id = 611;
    int lease_expiry_persisted = 0;
    assert (target->relocation_wire ().register_terminal_target (
      {expiry_relay, request_source, std::nullopt,
       [] (protocol::reply_relay_ack_status_t) { return true; },
       [&] {
           ++lease_expiry_persisted;
           return true;
       }}));
    assert (target->relocation_wire ().confirm_terminal_source_lease_expired (
      relocation, expiry_relay.operation, request_source));
    assert (lease_expiry_persisted == 1);
    assert (target->relocation_wire ().pending_terminal_relays () == 0);

    source->close ();
    target->close ();
}
}

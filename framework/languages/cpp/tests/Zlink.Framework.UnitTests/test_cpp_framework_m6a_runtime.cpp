/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

namespace
{

template <typename T>
T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

void verify_actor_create_command_49_roundtrip ()
{
    const protocol::actor_create_header_t request{
      19,
      {20, 21},
      bytes ("source"),
      22,
      "actor-1",
      "player",
      {"reservation-1", "store-1", 23, 24,
       bytes ("target"), 25, "owner-1", 26, 1},
      27};
    assert (protocol::decode_actor_create_header (
              protocol::encode_actor_create_header (request))
            == request);

    const auto created = protocol::decode_actor_create_reply (
      protocol::encode_actor_create_reply (
        19, 0, 0, protocol::actor_create_result_t::created,
        bytes ("target"), "actor-1", 23));
    assert (created.header.correlation == 19);
    assert (created.result
            == protocol::actor_create_result_t::created);
    assert (created.node_routing_id == bytes ("target"));
    assert (created.actor_id == "actor-1");
    assert (created.object_generation == 23);

    const auto rejected = protocol::decode_actor_create_reply (
      protocol::encode_actor_create_reply (
        19, 0, 0, protocol::actor_create_result_t::rejected,
        {}, {}, 0));
    assert (rejected.result
            == protocol::actor_create_result_t::rejected);
    assert (rejected.node_routing_id.empty ());
}

mesh::service_node_descriptor_t descriptor (
  std::string rid,
  std::string endpoint = "tcp://127.0.0.1:0")
{
    return mesh::service_node_descriptor_t{
      "m6a-mesh", bytes (std::move (rid)), 1, 1, std::move (endpoint),
      {{"alpha", 100}, {"beta", 50}},
      mesh::service_node_state_t::preparing};
}

protocol::actor_create_header_t actor_create_request (
  const mesh::service_node_descriptor_t &source,
  const mesh::service_node_descriptor_t &target,
  std::string actor_id)
{
    return protocol::actor_create_header_t{
      0,
      {source.lifecycle_generation, 1},
      source.node_routing_id,
      source.lifecycle_generation,
      std::move (actor_id),
      "player",
      {"startup-reservation", "startup-store", 1, 1,
       target.node_routing_id, target.lifecycle_generation,
       "startup-owner", 1, 1},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch () + 2s)
          .count ())};
}

protocol::bound_session_bind_t bound_session_bind_request (
  const mesh::service_node_descriptor_t &target,
  std::string actor_id)
{
    return protocol::bound_session_bind_t{
      0,
      {std::move (actor_id), 1, target.node_routing_id,
       target.lifecycle_generation, 1, 1},
      bytes ("session-owner"),
      {protocol::bound_session_binding_state_t::active, 1}};
}

void admit_pair (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target,
                 const mesh::service_node_descriptor_t &target_descriptor)
{
    source.expect_peer (target_descriptor);
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!source.topology ().peer (target_descriptor.node_routing_id)
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
}

void complete_bound_session_bind (
  mesh::raw_mesh_node_owner_t &source,
  mesh::raw_mesh_node_owner_t &target,
  std::future<std::pair<foundation::operation_terminal_t,
                        std::vector<std::uint8_t>>> &completion,
  std::string_view expected_actor)
{
    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            const auto pumped = await_task (target.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
            assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();
    const auto decoded = protocol::decode_bound_session_bind (
      record.parts.front ());
    assert (decoded.actor.actor_id == expected_actor);
    assert (record.correlation && decoded.correlation == *record.correlation);
    assert (target.reply_bound_session_bind (record, 0, 0));
    assert (target.mailbox ().release (*claim));

    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        const auto pumped = await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    const auto settled = completion.get ();
    assert (settled.first == foundation::operation_terminal_t::completed);
    const auto reply = protocol::decode_reply_header (settled.second);
    assert (reply.terminal_result == 0 && reply.failure_code == 0);
}

void verify_bound_session_bind_retries_until_route_is_admitted ()
{
    using completion_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("bind-delay-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("bind-delay-target")});
    source.start ();
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "delayed-bind-actor"),
      2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    assert (completion.wait_for (0ms) == std::future_status::timeout);

    admit_pair (source, target, target_descriptor);
    complete_bound_session_bind (
      source, target, completion, "delayed-bind-actor");
    source.close ();
    target.close ();
}

void verify_bound_session_bind_permanent_absence_is_bounded ()
{
    using completion_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("bind-timeout-source")});
    source.start ();
    auto target_descriptor = descriptor (
      "bind-timeout-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    const auto started = std::chrono::steady_clock::now ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "missing-bind-actor"),
      50ms,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    assert (completion.wait_for (500ms) == std::future_status::ready);
    assert (std::chrono::steady_clock::now () - started >= 40ms);
    assert (completion.get ().first
            == foundation::operation_terminal_t::timed_out);
    source.close ();
}

void verify_bound_session_bind_reply_completes_registered_operation ()
{
    using completion_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("bind-reply-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("bind-reply-target")});
    source.start ();
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "reply-bind-actor"),
      2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    complete_bound_session_bind (
      source, target, completion, "reply-bind-actor");
    source.close ();
    target.close ();
}

void verify_actor_create_retries_until_route_is_admitted ()
{
    using completion_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("startup-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("startup-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    auto request = actor_create_request (
      source_descriptor, target_descriptor, "startup-actor");
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          completion_promise.set_value ({terminal, std::move (payload)});
      });
    assert (await_task (std::move (submitted)));
    assert (completion.wait_for (0ms) == std::future_status::timeout);

    source.expect_peer (target_descriptor);
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    const auto admission_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source.topology ().peer (target_descriptor.node_routing_id)
           && std::chrono::steady_clock::now () < admission_deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) target.drain_monitor_events (now);
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));

    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (claim)
            break;
        const auto received = await_task (target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (received != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();
    const auto decoded = protocol::decode_actor_create_header (
      record.parts.front ());
    assert (decoded.actor_id == "startup-actor");
    assert (target.reply_actor_create (
      record,
      protocol::actor_create_reply_t{
        {*record.correlation, 0, 0},
        protocol::actor_create_result_t::created,
        target_descriptor.node_routing_id,
        decoded.actor_id,
        1}));
    assert (target.mailbox ().release (*claim));
    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    assert (completion.get ().first
            == foundation::operation_terminal_t::completed);
    source.close ();
    target.close ();
}

void verify_actor_create_retry_timeout_is_unavailable ()
{
    using completion_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("timeout-source")});
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    auto target_descriptor = descriptor (
      "timeout-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    auto request = actor_create_request (
      source_descriptor, target_descriptor, "timeout-actor");
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    const auto submitted_at = std::chrono::steady_clock::now ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 50ms,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          completion_promise.set_value ({terminal, std::move (payload)});
      });
    assert (std::chrono::steady_clock::now () - submitted_at < 20ms);
    assert (await_task (std::move (submitted)));
    assert (std::chrono::steady_clock::now () - submitted_at < 20ms);
    assert (completion.wait_for (500ms) == std::future_status::ready);
    assert (std::chrono::steady_clock::now () - submitted_at >= 40ms);
    const auto terminal = completion.get ().first;
    assert (terminal == foundation::operation_terminal_t::transport_failed);
    assert (
      runtime::user_spot_terminal::map_user_spot_operation_failure (
        terminal, {}, true)
      == zlink::framework::framework_error_kind_t::unavailable);
    source.close ();
}

void verify_actor_create_from_dispatch_thread_does_not_block ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("dispatch-source")});
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    auto target_descriptor = descriptor (
      "dispatch-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    source.expect_peer (target_descriptor);
    auto request = actor_create_request (
      source_descriptor, target_descriptor, "dispatch-actor");
    std::atomic<bool> callback_called{false};
    std::atomic<foundation::operation_terminal_t> callback_terminal{
      foundation::operation_terminal_t::transport_failed};

    const auto submitted_at = std::chrono::steady_clock::now ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 500ms,
      [&callback_called, &callback_terminal] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t>) {
          callback_terminal.store (terminal, std::memory_order_release);
          callback_called.store (true, std::memory_order_release);
      });
    assert (await_task (std::move (submitted)));
    assert (std::chrono::steady_clock::now () - submitted_at < 20ms);
    assert (!callback_called.load (std::memory_order_acquire));
    source.close ();
    const auto callback_deadline = std::chrono::steady_clock::now () + 500ms;
    while (!callback_called.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < callback_deadline) {
        std::this_thread::yield ();
    }
    assert (callback_called.load (std::memory_order_acquire));
    assert (callback_terminal.load (std::memory_order_acquire)
            == foundation::operation_terminal_t::shutdown);
}

void verify_topology_snapshot_and_connection_fence ()
{
    mesh::service_topology_registry_t topology (descriptor ("local"));
    auto peer = descriptor ("peer", "tcp://127.0.0.1:7001");
    peer.state = mesh::service_node_state_t::serving;
    const auto first_connection = bytes ("connection-a");
    assert (topology.admit (peer, first_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.select ("alpha")->descriptor.node_routing_id == bytes ("peer"));

    auto older = peer;
    older.descriptor_revision = 0;
    assert (topology.admit (older, bytes ("ignored"))
            == mesh::peer_admission_result_t::invalid_descriptor);

    const auto replacement_connection = bytes ("connection-b");
    assert (topology.admit (peer, replacement_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.disconnect (bytes ("peer"), first_connection));
    assert (topology.peer (bytes ("peer"))->connection_id
            == replacement_connection);

    auto equal_revision_mutation = peer;
    equal_revision_mutation.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (equal_revision_mutation, replacement_connection)
            == mesh::peer_admission_result_t::stale_descriptor);

    auto immutable_mutation = peer;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.security_identity = "changed-security";
    assert (topology.admit (immutable_mutation, replacement_connection)
            == mesh::peer_admission_result_t::stale_descriptor);

    peer.descriptor_revision = 2;
    peer.state = mesh::service_node_state_t::retiring;
    assert (topology.admit (peer, replacement_connection)
            == mesh::peer_admission_result_t::admitted);
    assert (!topology.select ("alpha"));
}

// RouteMesh admission's capability is part of the wire descriptor contract.
// Keeping the default bound to the generated schema constant prevents a C++
// peer from sending a descriptor that a newer language rejects before its
// identity guard can run.
void verify_route_mesh_descriptor_uses_generated_capability ()
{
    const auto local = descriptor ("generated-capability-local");
    assert (local.protocol_capabilities.size () == 2);
    assert (local.protocol_capabilities.at (0) == "framework-service-v12");
    assert (local.protocol_capabilities.at (1)
            == protocol::required_capability);
    mesh::service_topology_registry_t topology (local);
    (void) topology;

    auto stale = local;
    stale.protocol_capabilities = {"framework-service-v12"};
    bool rejected = false;
    try {
        mesh::service_topology_registry_t invalid (std::move (stale));
        (void) invalid;
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);
}

void verify_duplicate_connection_survivor_is_symmetric ()
{
    auto lower = descriptor ("aa");
    auto higher = descriptor ("zz", "tcp://127.0.0.1:7003");
    higher.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t lower_topology (lower);

    assert (lower_topology.admit (
              higher, bytes ("inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.admit (
              higher, bytes ("outbound"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::admitted);
    assert (lower_topology.peer (bytes ("zz"))->connection_id
            == bytes ("outbound"));
    assert (lower_topology.admit (
              higher, bytes ("second-inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::duplicate_connection);
    assert (!lower_topology.disconnect (
      bytes ("zz"), bytes ("second-inbound")));
    assert (lower_topology.peer (bytes ("zz"))->connection_id
            == bytes ("outbound"));
    mesh::service_liveness_registry_t liveness;
    liveness.admit (
      bytes ("zz"), bytes ("outbound"),
      mesh::service_liveness_registry_t::clock_t::now ());
    assert (!liveness.disconnect (
      bytes ("zz"), bytes ("second-inbound")));
    assert (liveness.size () == 1);

    auto lower_peer = descriptor (
      "aa", "tcp://127.0.0.1:7004");
    lower_peer.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t higher_topology (
      descriptor ("zz"));
    assert (higher_topology.admit (
              lower_peer, bytes ("outbound"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::admitted);
    assert (higher_topology.admit (
              lower_peer, bytes ("inbound"),
              mesh::service_connection_direction_t::inbound)
            == mesh::peer_admission_result_t::admitted);
    assert (higher_topology.peer (bytes ("aa"))->connection_id
            == bytes ("inbound"));
}

void verify_lifecycle_token_requires_current_discovery_expectation ()
{
    mesh::service_topology_registry_t topology (
      descriptor ("local"));
    auto generation_99 = descriptor (
      "peer", "tcp://127.0.0.1:7099");
    generation_99.lifecycle_generation = 99;
    generation_99.state =
      mesh::service_node_state_t::serving;
    assert (topology.admit (
              generation_99, bytes ("generation-99"),
              mesh::service_connection_direction_t::outbound,
              generation_99)
            == mesh::peer_admission_result_t::admitted);

    auto generation_3 = generation_99;
    generation_3.lifecycle_generation = 3;
    generation_3.descriptor_revision = 1;
    assert (topology.admit (
              generation_3, bytes ("generation-3-without-expectation"),
              mesh::service_connection_direction_t::outbound)
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation
            == 99);

    auto wrong_expectation = generation_3;
    wrong_expectation.lifecycle_generation = 4;
    assert (topology.admit (
              generation_3, bytes ("generation-3-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.advertised_endpoint =
      "tcp://127.0.0.1:7100";
    assert (topology.admit (
              generation_3, bytes ("generation-3-endpoint-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);
    wrong_expectation = generation_3;
    wrong_expectation.security_identity =
      "different-security";
    assert (topology.admit (
              generation_3, bytes ("generation-3-security-mismatch"),
              mesh::service_connection_direction_t::outbound,
              wrong_expectation)
            == mesh::peer_admission_result_t::stale_descriptor);

    assert (topology.admit (
              generation_3, bytes ("generation-3"),
              mesh::service_connection_direction_t::outbound,
              generation_3)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peer (bytes ("peer"))->descriptor.lifecycle_generation
            == 3);
}

void verify_physical_candidates_preserve_survivor ()
{
    mesh::raw_mesh_connection_candidates_t candidates;
    const auto peer = bytes ("peer");
    const auto inbound = bytes ("inbound-physical");
    const auto outbound = bytes ("outbound-physical");
    const auto late_inbound = bytes ("late-inbound-physical");
    candidates.ready (
      peer, inbound,
      mesh::service_connection_direction_t::inbound,
      "tcp://127.0.0.1:7101");
    candidates.ready (
      peer, outbound,
      mesh::service_connection_direction_t::outbound,
      "tcp://127.0.0.1:7102");
    assert (candidates.size (peer) == 2);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == inbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->remote_endpoint
            == "tcp://127.0.0.1:7101");
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->connection_id
            == outbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->remote_endpoint
            == "tcp://127.0.0.1:7102");

    candidates.ready (
      peer, late_inbound,
      mesh::service_connection_direction_t::inbound,
      "tcp://127.0.0.1:7103");
    assert (candidates.size (peer) == 3);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == late_inbound);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::outbound)
              ->connection_id
            == outbound);

    assert (candidates.disconnect (peer, inbound));
    assert (!candidates.disconnect (peer, inbound));
    assert (candidates.disconnect (peer, late_inbound));
    assert (candidates.size (peer) == 1);
    assert (candidates.for_handshake (
              peer,
              mesh::service_connection_direction_t::inbound)
              ->connection_id
            == outbound);

    const auto second_peer = bytes ("second-peer");
    const auto shared_endpoint = "tcp://127.0.0.1:7110";
    candidates.ready (
      peer, bytes ("stale-peer-connection"),
      mesh::service_connection_direction_t::inbound, shared_endpoint);
    candidates.ready (
      second_peer, bytes ("stale-second-connection"),
      mesh::service_connection_direction_t::outbound, shared_endpoint);
    const auto removed = candidates.disconnect_by_endpoint (shared_endpoint);
    assert (removed.size () == 2);
    assert (candidates.size (peer) == 1);
    assert (candidates.size (second_peer) == 0);

    candidates.ready (
      peer, bytes ("old-peer-connection"),
      mesh::service_connection_direction_t::outbound, shared_endpoint);
    candidates.ready (
      second_peer, bytes ("replacement-peer-connection"),
      mesh::service_connection_direction_t::outbound, shared_endpoint);
    assert (candidates.endpoint_in_use_by_other (shared_endpoint, peer));
    const auto old_connections = candidates.disconnect_all (peer);
    assert (old_connections.size () == 2);
    assert (candidates.size (peer) == 0);
    assert (candidates.size (second_peer) == 1);
    assert (!candidates.endpoint_in_use_by_other (
      shared_endpoint, second_peer));
}

void verify_stale_rid_disconnect_preserves_same_endpoint_replacement ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("replacement-source")});
    auto old_target = std::make_unique<mesh::raw_mesh_node_owner_t> (
      mesh::raw_mesh_node_options_t{descriptor ("replacement-old")});
    source.start ();
    old_target->start ();
    const auto old_descriptor = old_target->topology ().local_descriptor ();
    const auto endpoint = old_target->endpoint ();
    admit_pair (source, *old_target, old_descriptor);

    old_target->close ();
    old_target.reset ();
    const auto disconnect_deadline = std::chrono::steady_clock::now () + 2s;
    while (source.topology ().peer (old_descriptor.node_routing_id)
           && std::chrono::steady_clock::now () < disconnect_deadline) {
        (void) source.drain_monitor_events (
          mesh::service_liveness_registry_t::clock_t::now ());
        std::this_thread::sleep_for (1ms);
    }

    auto replacement_options = descriptor ("replacement-new", endpoint);
    auto replacement = std::make_unique<mesh::raw_mesh_node_owner_t> (
      mesh::raw_mesh_node_options_t{std::move (replacement_options)});
    replacement->start ();
    const auto replacement_descriptor =
      replacement->topology ().local_descriptor ();
    admit_pair (source, *replacement, replacement_descriptor);

    assert (source.disconnect_peer (
      old_descriptor.node_routing_id, endpoint));
    assert (source.topology ().peer (
      replacement_descriptor.node_routing_id));

    const auto stable_deadline = std::chrono::steady_clock::now () + 500ms;
    while (std::chrono::steady_clock::now () < stable_deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.drain_monitor_events (now);
        (void) replacement->drain_monitor_events (now);
        (void) await_task (source.pump_one (now));
        (void) await_task (replacement->pump_one (now));
        assert (source.topology ().peer (
          replacement_descriptor.node_routing_id));
        std::this_thread::sleep_for (1ms);
    }

    source.close ();
    replacement->close ();
}

void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ()
{
    /* Public ROUTER receive exposes the peer RID but not the physical
     * connection ID. Exercise the runtime's deterministic direction rule
     * with both physical directions present, then drain late monitor and
     * handshake events to prove that they cannot replace the survivor. */
    mesh::raw_mesh_node_owner_t lower (
      mesh::raw_mesh_node_options_t{
        descriptor ("bilateral-aa")});
    mesh::raw_mesh_node_owner_t higher (
      mesh::raw_mesh_node_options_t{
        descriptor ("bilateral-zz")});
    lower.start ();
    higher.start ();
    const auto lower_descriptor =
      lower.topology ().local_descriptor ();
    const auto higher_descriptor =
      higher.topology ().local_descriptor ();
    lower.expect_peer (higher_descriptor);
    higher.expect_peer (lower_descriptor);
    assert (lower.connect_peer (
      higher.endpoint (), higher_descriptor));
    assert (higher.connect_peer (
      lower.endpoint (), lower_descriptor));

    const auto deadline =
      std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.drain_monitor_events (now);
        (void) higher.drain_monitor_events (now);
        (void) await_task (lower.pump_one (now));
        (void) await_task (higher.pump_one (now));
        const auto lower_peer =
          lower.topology ().peer (
            higher_descriptor.node_routing_id);
        const auto higher_peer =
          higher.topology ().peer (
            lower_descriptor.node_routing_id);
        if (lower_peer && higher_peer
            && lower_peer->direction
                 == mesh::service_connection_direction_t::outbound
            && higher_peer->direction
                 == mesh::service_connection_direction_t::inbound)
            break;
        std::this_thread::sleep_for (1ms);
    }

    const auto lower_survivor =
      lower.topology ().peer (
        higher_descriptor.node_routing_id);
    const auto higher_survivor =
      higher.topology ().peer (
        lower_descriptor.node_routing_id);
    assert (lower_survivor);
    assert (higher_survivor);
    assert (lower_survivor->direction
            == mesh::service_connection_direction_t::outbound);
    assert (higher_survivor->direction
            == mesh::service_connection_direction_t::inbound);

    const auto settle_deadline =
      std::chrono::steady_clock::now () + 100ms;
    while (std::chrono::steady_clock::now ()
           < settle_deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.drain_monitor_events (now);
        (void) higher.drain_monitor_events (now);
        (void) await_task (lower.pump_one (now));
        (void) await_task (higher.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (lower.topology ().peer (
              higher_descriptor.node_routing_id)
              ->connection_id
            == lower_survivor->connection_id);
    assert (higher.topology ().peer (
              lower_descriptor.node_routing_id)
              ->connection_id
            == higher_survivor->connection_id);
}

void verify_raw_admission_rejects_lifecycle_mismatch ()
{
    mesh::raw_mesh_node_owner_t first (
      mesh::raw_mesh_node_options_t{
        descriptor ("lifecycle-a")});
    mesh::raw_mesh_node_owner_t second (
      mesh::raw_mesh_node_options_t{
        descriptor ("lifecycle-b")});
    first.start ();
    second.start ();
    auto expected_second =
      second.topology ().local_descriptor ();
    ++expected_second.lifecycle_generation;
    assert (first.connect_peer (
      second.endpoint (), expected_second));

    const auto deadline =
      std::chrono::steady_clock::now () + 500ms;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (now);
        (void) second.drain_monitor_events (now);
        (void) await_task (first.pump_one (now));
        (void) await_task (second.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }
    assert (!first.topology ().peer (
      expected_second.node_routing_id));
}

void verify_object_client_connection_requirement ()
{
    auto local = descriptor ("client-a");
    local.object_role = mesh::service_object_role_t::client;
    local.channels.clear ();
    auto remote = descriptor (
      "client-b", "tcp://127.0.0.1:7002");
    remote.object_role = mesh::service_object_role_t::client;
    remote.channels.clear ();
    remote.state = mesh::service_node_state_t::serving;

    assert (mesh::route_mesh_connection_not_required (
      local, remote));
    mesh::service_topology_registry_t topology (local);
    assert (topology.admit (
              remote, bytes ("client-only-connection"))
            == mesh::peer_admission_result_t::not_required);
    assert (topology.peers ().empty ());
    assert (topology.not_required_peers ().size () == 1);

    auto zero_weight_server = remote;
    zero_weight_server.lifecycle_generation = 2;
    zero_weight_server.descriptor_revision = 1;
    zero_weight_server.channels = {{"audit", 0}};
    assert (!mesh::route_mesh_connection_not_required (
      local, zero_weight_server));
    assert (topology.admit (
              zero_weight_server, bytes ("required-connection"),
              mesh::service_connection_direction_t::inbound,
              zero_weight_server)
            == mesh::peer_admission_result_t::admitted);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    assert (topology.admit (
              remote, bytes ("stale-client-only-connection"))
            == mesh::peer_admission_result_t::stale_descriptor);
    assert (topology.peers ().size () == 1);
    assert (topology.not_required_peers ().empty ());

    auto local_server_membership = local;
    local_server_membership.descriptor_revision = 2;
    local_server_membership.channels = {{"commands", 0}};
    assert (!mesh::route_mesh_connection_not_required (
      local_server_membership, remote));
}

void verify_manual_object_client_pair_ends_not_required ()
{
    auto first_descriptor = descriptor (
      "manual-client-a", "tcp://127.0.0.1:0");
    first_descriptor.object_role =
      mesh::service_object_role_t::client;
    first_descriptor.channels.clear ();
    auto second_descriptor = descriptor (
      "manual-client-b", "tcp://127.0.0.1:0");
    second_descriptor.object_role =
      mesh::service_object_role_t::client;
    second_descriptor.channels.clear ();

    mesh::raw_mesh_node_owner_t first (
      {first_descriptor, 16, 1024, 16, 1024});
    mesh::raw_mesh_node_owner_t second (
      {second_descriptor, 16, 1024, 16, 1024});
    first.start ();
    second.start ();
    assert (first.connect_peer (second.endpoint ()));

    const auto deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while ((first.topology ().not_required_peers ().empty ()
            || second.topology ().not_required_peers ().empty ())
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (now);
        (void) second.drain_monitor_events (now);
        (void) await_task (first.pump_one (now));
        (void) await_task (second.pump_one (now));
        std::this_thread::sleep_for (1ms);
    }

    assert (first.topology ().peers ().empty ());
    assert (second.topology ().peers ().empty ());
    assert (first.topology ().not_required_peers ().size () == 1);
    assert (second.topology ().not_required_peers ().size () == 1);
    assert (await_task (first.tick_liveness (
              mesh::service_liveness_registry_t::clock_t::now ()
              + 5s)).probes.empty ());
    assert (await_task (second.tick_liveness (
              mesh::service_liveness_registry_t::clock_t::now ()
              + 5s)).probes.empty ());

    first.close ();
    second.close ();
}

void verify_signed_weight_contract ()
{
    auto local = descriptor ("weight-local");
    local.channels = {{"weighted", 100}};
    local.placement_weight = 100;
    mesh::service_topology_registry_t topology (local);

    auto weight_100 =
      descriptor ("weight-100", "tcp://127.0.0.1:7101");
    weight_100.channels = {{"weighted", 100}};
    weight_100.state = mesh::service_node_state_t::serving;
    auto weight_300 =
      descriptor ("weight-300", "tcp://127.0.0.1:7102");
    weight_300.channels = {{"weighted", 300}};
    weight_300.state = mesh::service_node_state_t::serving;
    auto weight_zero =
      descriptor ("weight-zero", "tcp://127.0.0.1:7103");
    weight_zero.channels = {{"weighted", 0}};
    weight_zero.state = mesh::service_node_state_t::serving;
    assert (
      topology.admit (weight_100, bytes ("weight-connection-100"))
      == mesh::peer_admission_result_t::admitted);
    assert (
      topology.admit (weight_300, bytes ("weight-connection-300"))
      == mesh::peer_admission_result_t::admitted);
    assert (
      topology.admit (weight_zero, bytes ("weight-connection-zero"))
      == mesh::peer_admission_result_t::admitted);

    std::size_t selected_100 = 0;
    std::size_t selected_300 = 0;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected = topology.select ("weighted");
        assert (selected);
        if (selected->descriptor.node_routing_id
            == bytes ("weight-100"))
            ++selected_100;
        else if (selected->descriptor.node_routing_id
                 == bytes ("weight-300"))
            ++selected_300;
        else
            assert (false);
    }
    assert (selected_100 == 100);
    assert (selected_300 == 300);

    const auto multicast =
      topology.multicast_targets ("weighted");
    assert (multicast.size () == 2);
    assert (std::count_if (
              multicast.begin (), multicast.end (),
              [] (const auto &peer) {
                  return peer.descriptor.node_routing_id
                         == bytes ("weight-100");
              })
            == 1);
    assert (std::count_if (
              multicast.begin (), multicast.end (),
              [] (const auto &peer) {
                  return peer.descriptor.node_routing_id
                         == bytes ("weight-300");
              })
            == 1);

    auto revision = weight_100;
    revision.descriptor_revision = 2;
    revision.channels.front ().weight = 0;
    assert (
      topology.admit (
        revision, bytes ("weight-connection-100"))
      == mesh::peer_admission_result_t::admitted);
    const auto after_revision =
      topology.multicast_targets ("weighted");
    assert (after_revision.size () == 1);
    assert (after_revision.front ().descriptor.node_routing_id
            == bytes ("weight-300"));

    auto invalid_negative = local;
    invalid_negative.channels.front ().weight = -1;
    bool rejected_negative = false;
    try {
        mesh::service_topology_registry_t invalid (
          invalid_negative);
    }
    catch (const std::invalid_argument &) {
        rejected_negative = true;
    }
    assert (rejected_negative);

    auto invalid_upper = local;
    invalid_upper.placement_weight = 10001;
    bool rejected_upper = false;
    try {
        mesh::service_topology_registry_t invalid (
          invalid_upper);
    }
    catch (const std::invalid_argument &) {
        rejected_upper = true;
    }
    assert (rejected_upper);

    std::vector<int> overflow_safe_weights (
      430000, 10000);
    assert (
      mesh::sum_service_weights (overflow_safe_weights)
      == 4'300'000'000ull);
}

void verify_independent_mailbox_domains_and_claim_fence ()
{
    constexpr auto fixed_work_cost = runtime::dispatch_limits::fixed_work_byte_cost;
    mesh::service_mailbox_t owner_scoped (1, 4096, 1, 4096);
    assert (owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{1}}}));
    auto owner_a_claim = owner_scoped.try_claim_owner (
      mesh::service_mailbox_domain_t::application, "owner-a", 1, 4096);
    assert (owner_a_claim && owner_a_claim->records.size () == 1);
    assert (!owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{2}}}));
    assert (owner_scoped.try_enqueue (
      {"owner-b", mesh::service_mailbox_domain_t::application,
       {{3}}}));
    assert (owner_scoped.release (*owner_a_claim));
    assert (owner_scoped.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{6}}}));

    mesh::service_mailbox_t mailbox (4, 4096, 2, 4096);
    assert (mailbox.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{1, 2, 3}}}));
    assert (mailbox.try_enqueue (
      {"owner-a", mesh::service_mailbox_domain_t::application,
       {{4, 5}}}));
    assert (mailbox.try_enqueue (
      {"peer-a", mesh::service_mailbox_domain_t::infrastructure,
       {{9}}}));
    assert (mailbox.try_enqueue (
      {"owner-b", mesh::service_mailbox_domain_t::application,
       {{7}}}));

    auto owner_b = mailbox.try_claim_owner (
      mesh::service_mailbox_domain_t::application, "owner-b", 1, 64);
    assert (owner_b && owner_b->records.size () == 1);
    assert (mailbox.release (*owner_b));

    auto application =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 64);
    assert (application && application->records.size () == 1);
    assert (!mailbox.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64));

    auto infrastructure =
      mailbox.try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 32);
    assert (infrastructure && infrastructure->records.size () == 1);
    assert (mailbox.release (*infrastructure));
    assert (!mailbox.release (*infrastructure));

    assert (mailbox.release (*application));
    auto remaining =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 2, 64);
    assert (remaining && remaining->records.size () == 1);
    assert (mailbox.pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 0);
    assert (mailbox.pending_bytes (
              mesh::service_mailbox_domain_t::application)
            == 0);
    assert (mailbox.release (*remaining));

    mesh::service_mailbox_t accounting (2, fixed_work_cost + 3, 1,
                                        fixed_work_cost + 3);
    assert (accounting.try_enqueue (
      {"accounted", mesh::service_mailbox_domain_t::application,
       {{1, 2, 3}}}));
    assert (accounting.pending_bytes (
              mesh::service_mailbox_domain_t::application)
            == fixed_work_cost + 3);
    assert (!accounting.try_enqueue (
      {"accounted", mesh::service_mailbox_domain_t::application,
       {{4}}}));
    auto accounted = accounting.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1);
    assert (accounted && accounted->records.size () == 1);
    assert (accounting.release (*accounted));

    assert (mailbox.try_enqueue (
      {"owner-large", mesh::service_mailbox_domain_t::application,
       {std::vector<std::uint8_t> (20, 7)}}));
    auto oversized =
      mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 1);
    assert (oversized && oversized->records.size () == 1);
    assert (mailbox.release (*oversized));

    mesh::service_mailbox_t saturated (1, 1024, 1, 1024);
    assert (saturated.try_enqueue (
      {"first", mesh::service_mailbox_domain_t::application,
       {{1}}}));
    mesh::service_mailbox_record_t retained{
      "first", mesh::service_mailbox_domain_t::application,
      {{2, 3, 4}}};
    assert (!saturated.try_enqueue (std::move (retained)));
    assert (retained.owner == "first");
    assert ((retained.parts
             == std::vector<std::vector<std::uint8_t>>{{2, 3, 4}}));
    auto first = saturated.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64);
    assert (first && saturated.release (*first));
    assert (saturated.try_enqueue (std::move (retained)));
    auto second = saturated.try_claim (
      mesh::service_mailbox_domain_t::application, 1, 64);
    assert (second && second->records.size () == 1);
    assert (second->records.front ().owner == "first");
    assert ((second->records.front ().parts
             == std::vector<std::vector<std::uint8_t>>{{2, 3, 4}}));
    assert (saturated.release (*second));
}

void verify_liveness_reuses_probe_and_fences_reconnect ()
{
    mesh::service_liveness_registry_t liveness (10ms, 30ms);
    const auto node = bytes ("peer");
    const auto first_connection = bytes ("connection-a");
    const auto replacement_connection = bytes ("connection-b");
    const auto start = mesh::service_liveness_registry_t::clock_t::now ();
    liveness.admit (node, first_connection, start);

    const auto first = liveness.tick (start + 10ms);
    assert (first.probes.size () == 1);
    const auto probe_id = first.probes.front ().probe_id;
    const auto retransmit = liveness.tick (start + 20ms);
    assert (retransmit.probes.size () == 1);
    assert (retransmit.probes.front ().probe_id == probe_id);
    assert (!liveness.acknowledge (
      node, first_connection, probe_id + 1, start + 21ms));
    assert (liveness.acknowledge (
      node, first_connection, probe_id, start + 21ms));

    liveness.admit (node, replacement_connection, start + 22ms);
    assert (!liveness.disconnect (node, first_connection));
    assert (!liveness.acknowledge (
      node, first_connection, probe_id, start + 23ms));
    assert (liveness.tick (start + 53ms).timed_out_nodes
            == std::vector<std::vector<std::uint8_t>>{node});
}

void verify_location_descriptor_cas_snapshot_and_watch ()
{
    locations::service_descriptor_registry_t registry;
    std::vector<locations::service_descriptor_event_t> events;
    const auto watch = registry.watch (
      {locations::service_descriptor_kind_t::client_server, "alpha"},
      [&events] (locations::service_descriptor_event_t event) {
          events.push_back (std::move (event));
      });
    locations::service_descriptor_record_t record{
      {locations::service_descriptor_kind_t::client_server, "alpha",
       bytes ("server-a")},
      11,
      1,
      "tcp://127.0.0.1:7001",
      "security-a",
      1024 * 1024,
      mesh::service_node_state_t::serving,
      100,
      "owner-a",
      17};
    assert (registry.publish (record, std::nullopt)
            == locations::service_descriptor_publish_status_t::inserted);
    auto stale = record;
    stale.descriptor_revision = 2;
    assert (registry.publish (stale, 9)
            == locations::service_descriptor_publish_status_t::conflict);
    auto immutable_mutation = record;
    immutable_mutation.descriptor_revision = 2;
    immutable_mutation.endpoint = "tcp://127.0.0.1:7002";
    assert (registry.publish (immutable_mutation, 1)
            == locations::service_descriptor_publish_status_t::conflict);
    auto updated = record;
    updated.descriptor_revision = 2;
    updated.weight = 0;
    updated.state = mesh::service_node_state_t::draining;
    assert (registry.publish (updated, 1)
            == locations::service_descriptor_publish_status_t::updated);
    const auto snapshot = registry.snapshot (
      {locations::service_descriptor_kind_t::client_server, "alpha"});
    assert (snapshot.change_stamp == 2);
    assert (snapshot.records == std::vector{updated});
    assert (!registry.remove (
      updated.key, 1, updated.owner_id, updated.owner_lease_generation));
    assert (registry.remove (
      updated.key, 2, updated.owner_id, updated.owner_lease_generation));
    assert (events.size () == 3);
    assert (events.back ().change
            == locations::service_descriptor_change_t::removed);
    assert (events.back ().change_stamp == 3);
    assert (registry.unwatch (watch));
    assert (!registry.unwatch (watch));
}

void verify_manual_and_automatic_classic_fanout ()
{
    fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
    publisher.start ();
    fanout::raw_fanout_subscriber_t manual;
    const auto publisher_id = bytes ("publisher-a");
    assert (manual.connect_manual (publisher_id, publisher.endpoint ()));

    auto receive_now = std::chrono::steady_clock::now ();
    bool beacon_received = false;
    const auto deadline = receive_now + 2s;
    std::size_t beacon_tick = 1;
    while (!beacon_received && std::chrono::steady_clock::now () < deadline) {
        (void) publisher.tick (
          receive_now
          + fanout::fanout_beacon_interval
              * static_cast<int> (beacon_tick++));
        const auto [status, received] = manual.try_receive (receive_now);
        static_cast<void> (received);
        beacon_received = status == fanout::fanout_receive_status_t::beacon;
        if (!beacon_received) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (beacon_received);
    assert (manual.ready (publisher_id));
    bool application_received = false;
    for (std::size_t attempt = 0; attempt < 100 && !application_received;
         ++attempt) {
        await_task (publisher.publish (
          "fanout-alpha",
          "topic-a",
          {"FanoutProbe", "application/json", bytes ("fanout")}));
        const auto [status, received] = manual.try_receive (receive_now);
        if (status == fanout::fanout_receive_status_t::application) {
            assert (received);
            assert (received->publisher_routing_id == publisher_id);
            assert (received->topic == "topic-a");
            const protocol::application_payload_t expected{
              "FanoutProbe", "application/json", bytes ("fanout")};
            assert (received->payload == expected);
            application_received = true;
        } else {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (application_received);
    assert (manual.tick (
              receive_now + fanout::fanout_receive_deadline)
            == std::vector<std::vector<std::uint8_t>>{publisher_id});
    assert (!manual.ready (publisher_id));

    fanout::raw_fanout_subscriber_t automatic;
    fanout::fanout_publisher_intent_t automatic_descriptor{
      publisher_id,
      1,
      publisher.endpoint (),
      mesh::service_node_state_t::serving};
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    bool automatic_ready = false;
    for (std::size_t attempt = 0; attempt < 100 && !automatic_ready;
         ++attempt) {
        (void) publisher.tick (
          receive_now
          + fanout::fanout_beacon_interval
              * static_cast<int> (beacon_tick++));
        const auto [status, received] =
          automatic.try_receive (receive_now);
        static_cast<void> (received);
        automatic_ready =
          status == fanout::fanout_receive_status_t::beacon;
        if (!automatic_ready) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (automatic_ready);
    assert (automatic.ready (publisher_id));

    automatic_descriptor.lifecycle_generation = 2;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    assert (!automatic.ready (publisher_id));

    automatic_descriptor.state = mesh::service_node_state_t::draining;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 0);

    bool reserved_rejected = false;
    try {
        await_task (publisher.publish (
          "fanout-alpha",
          fanout::raw_fanout_publisher_t::reserved_topic (),
          {"Reserved", "application/json", {}}));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        reserved_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::internal_failure;
    }
    assert (reserved_rejected);
}

void verify_client_server_stale_admission_reply_is_discarded ()
{
    /* Spec 51 §4 physical-connection replacement: an admission reply that
     * settles after its physical pair terminated must not admit the client
     * — the parked completion is fenced by the connection generation it
     * was requested on and discarded on mismatch. */
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-stale",
      bytes ("server-s"),
      41,
      1,
      100,
      mesh::service_node_state_t::serving,
      "security-s",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    auto server =
      std::make_unique<client_server::raw_client_server_server_t> (
        client_server::raw_client_server_server_options_t{
          {server_descriptor}});
    server->start ();
    auto expected_server = server->descriptor ();
    auto manual_server = expected_server;
    manual_server.server_routing_id.clear ();
    manual_server.lifecycle_generation = 0;
    manual_server.descriptor_revision = 0;
    client_server::raw_client_server_client_t client (
      {bytes ("client-s"),
       {expected_server.channel_name, "security-s", 16u * 1024u * 1024u},
       std::move (manual_server)});
    client.start ();

    /* Fire the hello request and let the server answer it, without ever
     * pumping the client — the admit completion parks unapplied. */
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    bool served_hello = false;
    while (!served_hello && std::chrono::steady_clock::now () < deadline) {
        (void) server->drain_monitor_events (std::chrono::steady_clock::now ());
        (void) client.drain_monitor_events (std::chrono::steady_clock::now ())
          .result ()
          .value ();
        const auto pump =
          server->pump_one (std::chrono::steady_clock::now ())
            .result ()
            .value ();
        served_hello =
          pump == client_server::client_server_pump_result_t::infrastructure;
        if (!served_hello)
            std::this_thread::sleep_for (1ms);
    }
    assert (served_hello);
    /* Give the admit reply time to settle into the parked state. */
    std::this_thread::sleep_for (300ms);

    /* Terminate the physical pair before the parked reply is applied, and
     * bring up a replacement server on the same endpoint. The client
     * reconnects (same connection identity bytes, new physical pair), but
     * the replacement server never answers the new hello — so the only
     * admission the client could apply is the stale parked one. */
    const auto endpoint = expected_server.advertised_endpoint;
    server->close ();
    server.reset ();
    auto replacement_descriptor = server_descriptor;
    replacement_descriptor.advertised_endpoint = endpoint;
    auto replacement =
      std::make_unique<client_server::raw_client_server_server_t> (
        client_server::raw_client_server_server_options_t{
          {replacement_descriptor}});
    replacement->start ();
    /* Drain the client so it observes the disconnect and the reconnect to
     * the replacement (new physical pair; fires a fresh hello the
     * replacement deliberately leaves unanswered). */
    const auto drain_until = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < drain_until) {
        (void) client.drain_monitor_events (std::chrono::steady_clock::now ())
          .result ()
          .value ();
        std::this_thread::sleep_for (10ms);
    }

    /* Applying the parked admit from the previous pair must discard it as
     * stale — the client stays unadmitted until the replacement answers. */
    const auto assert_deadline = std::chrono::steady_clock::now () + 800ms;
    while (std::chrono::steady_clock::now () < assert_deadline) {
        (void) client.pump_one (std::chrono::steady_clock::now ())
          .result ()
          .value ();
        assert (!client.ready ());
        std::this_thread::sleep_for (10ms);
    }
    assert (!client.ready ());
    replacement->close ();
}

void verify_client_server_plain_hello_is_rejected ()
{
    /* Spec 51 §4 (ClientServer direction): admission rides the Core
     * request/reply envelope — the server answers hello only on the reply
     * of the client's request. A plain routed hello cannot be answered
     * and must end as a protocol error instead of a raw routed admit. */
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-plain",
      bytes ("server-p"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-p",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      {{server_descriptor}});
    server.start ();

    auto context = std::make_shared<zlink::context_t> ();
    zlink::dealer_socket_t dealer (*context);
    dealer.set_routing_id (zlink::routing_id_t::from (bytes ("client-p")));
    dealer.connect (server.endpoint ());
    zlink::framework::detail::backend::raw_dealer_port_t port (dealer);
    const zlink::framework::detail::backend::raw_message_t hello_message{
      protocol::encode_client_server_client_admission (
        protocol::command::hello,
        {server_descriptor.channel_name,
         "security-p",
         16u * 1024u * 1024u})};
    const auto send_deadline = std::chrono::steady_clock::now () + 5s;
    bool sent = false;
    while (!sent && std::chrono::steady_clock::now () < send_deadline) {
        try {
            sent = await_task (port.send (hello_message));
        }
        catch (const std::exception &) {
            /* The physical connection may not be ready yet. */
        }
        if (!sent)
            std::this_thread::sleep_for (10ms);
    }
    assert (sent);

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    auto result = client_server::client_server_pump_result_t::no_data;
    while (result == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        (void) server.drain_monitor_events (
          std::chrono::steady_clock::now ());
        result = server.pump_one (std::chrono::steady_clock::now ())
                   .result ()
                   .value ();
        if (result == client_server::client_server_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (result
            == client_server::client_server_pump_result_t::protocol_error);
    port.close ();
}

void verify_client_server_independent_raw_path ()
{
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-alpha",
      bytes ("server-a"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-a",
      16u * 1024u * 1024u,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      {{server_descriptor}});
    server.start ();
    auto expected_server = server.descriptor ();
    auto manual_server = expected_server;
    manual_server.server_routing_id.clear ();
    manual_server.lifecycle_generation = 0;
    manual_server.descriptor_revision = 0;
    client_server::raw_client_server_client_options_t client_options{
      bytes ("client-a"),
      {expected_server.channel_name,
       "security-a",
       16u * 1024u * 1024u},
      std::move (manual_server)};
    client_server::raw_client_server_client_t client (
      std::move (client_options));
    client.start ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!client.ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) server.drain_monitor_events (now);
        (void) client.drain_monitor_events (now).result ().value ();
        const auto server_pump = server.pump_one (now).result ().value ();
        const auto client_pump = client.pump_one (now).result ().value ();
        assert (
          server_pump
          != client_server::client_server_pump_result_t::protocol_error);
        assert (
          client_pump
          != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (client.ready ());

    const auto liveness_base = std::chrono::steady_clock::now ();
    const auto first_probe =
      client.tick_liveness (liveness_base + 5s).result ().value ();
    assert (first_probe.probes.size () == 1);
    client_server::client_server_pump_result_t probe_pump =
      client_server::client_server_pump_result_t::no_data;
    while (probe_pump
             == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        probe_pump = server.pump_one (std::chrono::steady_clock::now ())
                       .result ().value ();
    }
    assert (probe_pump
            == client_server::client_server_pump_result_t::infrastructure);
    client_server::client_server_pump_result_t ack_pump =
      client_server::client_server_pump_result_t::no_data;
    while (ack_pump
             == client_server::client_server_pump_result_t::no_data
           && std::chrono::steady_clock::now () < deadline) {
        ack_pump = client.pump_one (std::chrono::steady_clock::now ())
                     .result ().value ();
    }
    assert (ack_pump
            == client_server::client_server_pump_result_t::infrastructure);
    const auto next_probe =
      client.tick_liveness (liveness_base + 10s).result ().value ();
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id
            != first_probe.probes.front ().probe_id);

    assert (client.send (
      {"ClientServerSend", "application/json", bytes ("send")}, 2s)
              .result ().value ()
            == zlink::submit_result_t::ok);
    client_server::client_server_pump_result_t send_pump =
      client_server::client_server_pump_result_t::no_data;
    while (send_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        send_pump = server.pump_one (std::chrono::steady_clock::now ())
                      .result ().value ();
    }
    assert (send_pump
            == client_server::client_server_pump_result_t::application);
    auto send_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (send_claim && send_claim->records.size () == 1);
    {
        const auto send_header =
          runtime::messaging::envelope_codec_t{}.decode_header (
            zlink::message_t::from (
              send_claim->records.front ().parts.front ()),
            false);
        assert (send_header);
        assert (send_header.value ().channel_name
                == expected_server.channel_name);
        assert (send_header.value ().kind
                == runtime::messaging::message_kind_t::command);
        assert (send_header.value ().message_name == "ClientServerSend");
    }
    assert (server.mailbox ().release (*send_claim));

    auto request_task = client.request (
      {"ClientServerRequest", "application/json", bytes ("request")},
      2s);
    client_server::client_server_pump_result_t request_pump =
      client_server::client_server_pump_result_t::no_data;
    while (request_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (std::chrono::steady_clock::now ())
                         .result ().value ();
    }
    assert (request_pump
            == client_server::client_server_pump_result_t::application);
    auto request_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (request_claim->records.front ().reply_token);
    assert (server.reply (
      request_claim->records.front (),
      {"ClientServerReply", "application/json", bytes ("reply")}));
    assert (server.mailbox ().release (*request_claim));
    while (!request_task.await_ready ()
           && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ())
                            .result ().value ();
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (request_task.await_ready ());
    const auto result = request_task.result ().value ();
    assert (result.terminal
            == foundation::operation_terminal_t::completed);
    assert (!result.error_code);
    assert (result.content_type == "application/json");
    assert (result.payload == bytes ("reply"));

    auto rejected_task = client.request (
      {"RejectedRequest", "application/json", bytes ("request")},
      2s);
    request_pump =
      client_server::client_server_pump_result_t::no_data;
    while (request_pump
             != client_server::client_server_pump_result_t::application
           && std::chrono::steady_clock::now () < deadline) {
        request_pump = server.pump_one (
          std::chrono::steady_clock::now ()).result ().value ();
    }
    assert (request_pump
            == client_server::client_server_pump_result_t::application);
    request_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    assert (server.reply (
      request_claim->records.front (),
      zlink::framework::framework_exception_t (
        zlink::framework::framework_error_kind_t::rejected,
        "ClientServer request was rejected.")));
    assert (server.mailbox ().release (*request_claim));
    while (!rejected_task.await_ready ()
           && std::chrono::steady_clock::now () < deadline) {
        const auto pump = client.pump_one (
          std::chrono::steady_clock::now ()).result ().value ();
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (rejected_task.await_ready ());
    const auto rejected = rejected_task.result ().value ();
    assert (rejected.terminal
            == foundation::operation_terminal_t::completed);
    assert (rejected.error_code && *rejected.error_code == "rejected");

    // The advertised ClientServer limit must remain usable for a frame that
    // is larger than the core automatic HWM default.
    const auto large_payload = std::vector<std::uint8_t> (
      1024u * 1024u, static_cast<std::uint8_t> ('p'));
    auto large_task = client.request (
      {"LargePayloadRequest", "application/json", large_payload},
      3s);
    const auto large_deadline = std::chrono::steady_clock::now () + 5s;
    auto large_claim = server.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 2u * 1024u * 1024u);
    while (!large_claim
           && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = server.pump_one (
          std::chrono::steady_clock::now ()).result ().value ();
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        large_claim = server.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application,
          1,
          2u * 1024u * 1024u);
        if (!large_claim)
            std::this_thread::sleep_for (1ms);
    }
    assert (large_claim && large_claim->records.size () == 1);
    const auto large_reply = bytes ("large payload accepted");
    assert (server.reply (
      large_claim->records.front (),
      {"LargePayloadReply", "application/json", large_reply}));
    assert (server.mailbox ().release (*large_claim));
    while (!large_task.await_ready ()
           && std::chrono::steady_clock::now () < large_deadline) {
        const auto pump = client.pump_one (std::chrono::steady_clock::now ())
                            .result ().value ();
        assert (
          pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (large_task.await_ready ());
    const auto large_result = large_task.result ().value ();
    assert (large_result.terminal
            == foundation::operation_terminal_t::completed);
    assert (!large_result.error_code);
    assert (large_result.payload == large_reply);
}

void verify_client_server_admits_before_monitor_drain ()
{
    protocol::client_server_server_admission_t server_descriptor{
      "client-server-monitor-order",
      bytes ("server-monitor-order"),
      41,
      1,
      100,
      mesh::service_node_state_t::preparing,
      "security-monitor-order",
      1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      {{server_descriptor}});
    server.start ();
    const auto expected_server = server.descriptor ();

    std::vector<std::unique_ptr<client_server::raw_client_server_client_t>>
      clients;
    for (const auto &client_id : {"client-monitor-a", "client-monitor-b"}) {
        clients.push_back (
          std::make_unique<client_server::raw_client_server_client_t> (
            client_server::raw_client_server_client_options_t{
              bytes (client_id),
              {expected_server.channel_name,
               expected_server.security_identity,
               expected_server.effective_max_message_bytes},
              expected_server}));
        clients.back ()->start ();
    }

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        for (auto &client : clients) {
            (void) client->drain_monitor_events (now).result ().value ();
            const auto client_pump =
              client->pump_one (now).result ().value ();
            assert (
              client_pump
              != client_server::client_server_pump_result_t::protocol_error);
        }

        // The server monitor queue is deliberately not drained here. A
        // received hello already identifies the route that must receive the
        // admission response.
        const auto server_pump =
          server.pump_one (now).result ().value ();
        assert (
          server_pump
          != client_server::client_server_pump_result_t::protocol_error);

        bool all_ready = true;
        for (auto &client : clients) {
            (void) client->pump_one (now).result ().value ();
            all_ready = all_ready && client->ready ();
        }
        if (all_ready)
            break;
        std::this_thread::sleep_for (1ms);
    }

    for (const auto &client : clients)
        assert (client->ready ());
    for (auto &client : clients)
        client->close ();
    server.close ();
}

void verify_client_server_weighted_selection ()
{
    client_server::smooth_weighted_selector_t selector;
    const std::vector<client_server::weighted_candidate_t> weighted{
      {"api-a", 300}, {"api-b", 100}, {"disabled", 0}};
    std::map<std::string, std::size_t> selected;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto key = selector.select (weighted);
        assert (key);
        ++selected[*key];
        assert (selector.state_size () == 2);
        assert (selector.maximum_absolute_credit () <= 400);
    }
    assert (selected["api-a"] == 300);
    assert (selected["api-b"] == 100);
    assert (!selected.contains ("disabled"));

    const std::vector<client_server::weighted_candidate_t>
      after_api_b_enters_draining{
      {"api-a", 300}};
    for (std::size_t index = 0; index < 32; ++index) {
        const auto key = selector.select (
          after_api_b_enters_draining);
        assert (key && *key == "api-a");
        assert (selector.state_size () == 1);
        assert (selector.maximum_absolute_credit () <= 300);
    }

    const std::vector<client_server::weighted_candidate_t> none{
      {"disabled", 0}};
    assert (!selector.select (none));
    assert (selector.state_size () == 0);

    client_server::smooth_weighted_selector_t rid_tiebreak;
    const std::vector<client_server::weighted_candidate_t> tied{
      {"connection-z", 100, "rid-a"},
      {"connection-a", 100, "rid-b"}};
    const auto tie_selected = rid_tiebreak.select (tied);
    assert (tie_selected && *tie_selected == "connection-z");

    mesh::service_topology_registry_t topology (descriptor ("route-local"));
    auto route_a = descriptor ("route-a");
    auto route_b = descriptor ("route-b");
    route_a.state = mesh::service_node_state_t::serving;
    route_b.state = mesh::service_node_state_t::serving;
    assert (topology.admit (route_a, bytes ("route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (topology.admit (route_b, bytes ("route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    const std::vector<std::vector<std::uint8_t>> expected_route_ids{
      bytes ("route-a"), bytes ("route-b"), bytes ("route-a"),
      bytes ("route-b")};
    for (const auto &expected : expected_route_ids) {
        const auto selected_route = topology.select ("alpha");
        assert (selected_route
                && selected_route->descriptor.node_routing_id == expected);
    }

    auto weighted_route_a = descriptor ("weighted-route-a");
    auto weighted_route_b = descriptor ("weighted-route-b");
    weighted_route_a.channels.front ().weight = 300;
    weighted_route_b.channels.front ().weight = 100;
    weighted_route_a.state = mesh::service_node_state_t::serving;
    weighted_route_b.state = mesh::service_node_state_t::serving;
    mesh::service_topology_registry_t weighted_topology (
      descriptor ("weighted-route-local"));
    assert (weighted_topology.admit (
              weighted_route_a, bytes ("weighted-route-connection-a"))
            == mesh::peer_admission_result_t::admitted);
    assert (weighted_topology.admit (
              weighted_route_b, bytes ("weighted-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    std::map<std::vector<std::uint8_t>, std::size_t> weighted_selected;
    for (std::size_t index = 0; index < 400; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route);
        ++weighted_selected[selected_route->descriptor.node_routing_id];
    }
    assert (weighted_selected[bytes ("weighted-route-a")] == 300);
    assert (weighted_selected[bytes ("weighted-route-b")] == 100);

    weighted_route_b.descriptor_revision = 2;
    weighted_route_b.state = mesh::service_node_state_t::retiring;
    assert (weighted_topology.admit (
              weighted_route_b, bytes ("weighted-route-connection-b"))
            == mesh::peer_admission_result_t::admitted);
    for (std::size_t index = 0; index < 32; ++index) {
        const auto selected_route = weighted_topology.select ("alpha");
        assert (selected_route
                && selected_route->descriptor.node_routing_id
                     == bytes ("weighted-route-a"));
    }
}

void verify_raw_owner_node_send_and_liveness ()
{
    mesh::raw_mesh_node_owner_t first (
      mesh::raw_mesh_node_options_t{descriptor ("raw-a")});
    mesh::raw_mesh_node_owner_t second (
      mesh::raw_mesh_node_options_t{
        descriptor ("raw-b"),
        1,
        16u * 1024u * 1024u,
        1024,
        4u * 1024u * 1024u});
    assert (first.topology ().local_descriptor ().state
            == mesh::service_node_state_t::preparing);
    assert (second.topology ().local_descriptor ().state
            == mesh::service_node_state_t::preparing);
    first.start ();
    second.start ();
    assert (first.started () && second.started ());
    assert (first.topology ().local_descriptor ().state
            == mesh::service_node_state_t::serving);
    assert (second.topology ().local_descriptor ().state
            == mesh::service_node_state_t::serving);

    auto first_descriptor = first.topology ().local_descriptor ();
    auto second_descriptor = second.topology ().local_descriptor ();
    // The higher RID does not dial, but it retains discovery data to
    // validate the inbound endpoint and security identity.
    second.expect_peer (first_descriptor);
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto deadline = now + 5s;
    assert (first.connect_peer (second.endpoint (), second_descriptor));
    while ((!first.topology ().peer (second_descriptor.node_routing_id)
            || !second.topology ().peer (first_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto progress_now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.drain_monitor_events (progress_now);
        (void) second.drain_monitor_events (progress_now);
        (void) await_task (first.pump_one (progress_now));
        (void) await_task (second.pump_one (progress_now));
        std::this_thread::sleep_for (1ms);
    }
    assert (first.topology ().peer (second_descriptor.node_routing_id));
    assert (second.topology ().peer (first_descriptor.node_routing_id));

    bool submitted = false;
    while (!submitted && mesh::service_liveness_registry_t::clock_t::now ()
                           < deadline) {
        try {
            submitted = await_task (first.send_to_node (
              second_descriptor.node_routing_id,
              {"Probe", "application/json", bytes ("payload")}));
        }
        catch (...) {
        }
        if (!submitted) {
            std::this_thread::sleep_for (5ms);
        }
    }
    assert (submitted);

    // A missing host-wide Application Job Queue permit fences the ordinary
    // ROUTER before Core receive.  Poll readiness alone must not consume or
    // retain the application record in the Framework owner.
    assert (second.wait_for_activity (1s, false));
    const auto without_shared_permit = await_task (second.pump_one (
      mesh::service_liveness_registry_t::clock_t::now (), false));
    assert (without_shared_permit == mesh::raw_mesh_pump_result_t::no_data);
    assert (second.last_pump_bytes () == 0);
    assert (second.mailbox ().pending_messages (
              mesh::service_mailbox_domain_t::application)
            == 0);

    mesh::raw_mesh_pump_result_t pumped =
      mesh::raw_mesh_pump_result_t::no_data;
    while (pumped != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        pumped = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped != mesh::raw_mesh_pump_result_t::application) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::application);

    bool retained_submitted = false;
    while (!retained_submitted
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_submitted = await_task (first.send_to_node (
          second_descriptor.node_routing_id,
          {"Probe", "application/json", bytes ("retained")}));
        if (!retained_submitted)
            std::this_thread::sleep_for (1ms);
    }
    assert (retained_submitted);
    mesh::raw_mesh_pump_result_t retained_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (retained_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (retained_pump
            == mesh::raw_mesh_pump_result_t::backpressured);

    // A full owner mailbox drops the second one-way payload. Liveness is a
    // finite ordinary control record: it shares the pre-receive permit, then
    // returns that permit as soon as its internal processing completes.
    const auto paused_liveness_base =
      mesh::service_liveness_registry_t::clock_t::now ();
    const auto paused_probe = await_task (
      first.tick_liveness (paused_liveness_base + 5s));
    assert (paused_probe.probes.size () == 1);
    assert (second.wait_for_activity (1s, false));
    const auto paused_without_shared_permit = await_task (second.pump_one (
      mesh::service_liveness_registry_t::clock_t::now (), false));
    assert (paused_without_shared_permit
            == mesh::raw_mesh_pump_result_t::no_data);
    assert (second.last_pump_bytes () == 0);
    mesh::raw_mesh_pump_result_t paused_probe_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (paused_probe_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        paused_probe_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (paused_probe_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_probe_pump
            != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_probe_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);
    mesh::raw_mesh_pump_result_t paused_ack_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (paused_ack_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        paused_ack_pump = await_task (first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (paused_ack_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_ack_pump
            != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_ack_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);

    auto claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (claim && claim->records.size () == 1);
    assert (protocol::decode_header (
              claim->records.front ().parts.front ())
              .kind
            == protocol::command::nodeSend);
    const protocol::application_payload_t expected_payload{
      "Probe", "application/json", bytes ("payload")};
    assert (protocol::decode_application_payload (
              claim->records.front ().parts.at (1))
            == expected_payload);
    assert (second.mailbox ().release (*claim));

    for (std::size_t attempt = 0; attempt < 10; ++attempt) {
        const auto after_release = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (after_release
                != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (after_release
                != mesh::raw_mesh_pump_result_t::application);
        if (after_release == mesh::raw_mesh_pump_result_t::no_data)
            break;
    }
    auto dropped_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (!dropped_claim);

    bool channel_submitted = false;
    while (!channel_submitted
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        try {
            channel_submitted = await_task (first.send_to_channel (
              "alpha", {"ChannelProbe", "application/json", bytes ("channel")}));
        }
        catch (...) {
        }
    }
    assert (channel_submitted);
    mesh::raw_mesh_pump_result_t channel_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (channel_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        channel_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (channel_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (channel_pump == mesh::raw_mesh_pump_result_t::application);
    auto channel_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (channel_claim && channel_claim->owner == "channel:alpha");
    assert (protocol::decode_channel_send_header (
              channel_claim->records.front ().parts.front ())
            == "alpha");
    assert (second.mailbox ().release (*channel_claim));

    using request_result_t =
      std::pair<foundation::operation_terminal_t,
                std::vector<std::uint8_t>>;
    std::promise<request_result_t> request_promise;
    auto request_future = request_promise.get_future ();
    assert (await_task (first.request_to_node (
      second_descriptor.node_routing_id,
      {"RequestProbe", "application/json", bytes ("request")},
      2s,
      [&request_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          request_promise.set_value (
            {terminal, std::move (payload)});
      })));
    mesh::raw_mesh_pump_result_t request_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (request_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        request_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (request_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (request_pump == mesh::raw_mesh_pump_result_t::application);
    auto request_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    const auto &request_record = request_claim->records.front ();
    assert (request_record.reply_token);
    assert (request_record.correlation);
    assert (protocol::decode_node_request_header (
              request_record.parts.front ())
            == *request_record.correlation);
    assert (second.reply (
      request_record,
      {"RequestReply", "application/json", bytes ("reply")}));
    assert (second.mailbox ().release (*request_claim));
    const auto request_deadline =
      mesh::service_liveness_registry_t::clock_t::now () + 2s;
    // Reply/error completion is pre-classified on the binding completion
    // path, so it must settle even while ordinary receive has no shared
    // Application Job Queue permit.
    while (request_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < request_deadline) {
        const auto client_pump = await_task (first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), false));
        assert (client_pump == mesh::raw_mesh_pump_result_t::no_data);
        std::this_thread::sleep_for (1ms);
    }
    assert (request_future.wait_for (0ms) == std::future_status::ready);
    auto request_result = request_future.get ();
    assert (request_result.first
            == foundation::operation_terminal_t::completed);
    const protocol::application_payload_t expected_reply{
      "RequestReply", "application/json", bytes ("reply")};
    assert (protocol::decode_application_payload (request_result.second)
            == expected_reply);

    std::promise<request_result_t> actor_create_promise;
    auto actor_create_future = actor_create_promise.get_future ();
    protocol::actor_create_header_t actor_create{
      0,
      {first_descriptor.lifecycle_generation, 91},
      first_descriptor.node_routing_id,
      first_descriptor.lifecycle_generation,
      "actor-remote",
      "player",
      {"reservation-remote", "store-remote", 1, 1,
       second_descriptor.node_routing_id,
       second_descriptor.lifecycle_generation,
       "owner-remote", 1, 1},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch () + 2s)
          .count ())};
    assert (await_task (first.request_actor_create (
      second_descriptor.node_routing_id, actor_create, 2s,
      [&actor_create_promise] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          actor_create_promise.set_value (
            {terminal, std::move (payload)});
      })));
    mesh::raw_mesh_pump_result_t actor_create_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (actor_create_pump
             != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        actor_create_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (actor_create_pump
                != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_pump
            == mesh::raw_mesh_pump_result_t::infrastructure);
    auto actor_create_claim = second.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
    assert (actor_create_claim
            && actor_create_claim->records.size () == 1);
    const auto &actor_create_record =
      actor_create_claim->records.front ();
    const auto decoded_actor_create =
      protocol::decode_actor_create_header (
        actor_create_record.parts.front ());
    assert (decoded_actor_create.operation == actor_create.operation);
    assert (decoded_actor_create.actor_id == "actor-remote");
    protocol::actor_create_reply_t actor_create_reply{
      {*actor_create_record.correlation, 0, 0},
      protocol::actor_create_result_t::created,
      second_descriptor.node_routing_id,
      "actor-remote",
      1};
    assert (second.reply_actor_create (
      actor_create_record, actor_create_reply));
    assert (second.mailbox ().release (*actor_create_claim));
    while (actor_create_future.wait_for (0ms)
             != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump = await_task (first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_future.wait_for (0ms)
            == std::future_status::ready);
    const auto actor_create_result = actor_create_future.get ();
    assert (actor_create_result.first
            == foundation::operation_terminal_t::completed);
    assert (!actor_create_result.second.empty ());

    constexpr std::size_t actor_create_burst_size = 8;
    std::vector<std::future<request_result_t>> actor_create_burst_futures;
    actor_create_burst_futures.reserve (actor_create_burst_size);
    for (std::size_t index = 0; index < actor_create_burst_size; ++index) {
        auto promise = std::make_shared<std::promise<request_result_t>> ();
        actor_create_burst_futures.push_back (promise->get_future ());
        auto burst_request = actor_create;
        burst_request.operation.low = 100 + index;
        burst_request.actor_id = "actor-burst-" + std::to_string (index);
        assert (await_task (first.request_actor_create (
          second_descriptor.node_routing_id, burst_request, 2s,
          [promise] (foundation::operation_terminal_t terminal,
                     std::vector<std::uint8_t> payload) mutable {
              promise->set_value ({terminal, std::move (payload)});
          })));
    }

    std::size_t burst_received = 0;
    while (burst_received < actor_create_burst_size
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        const auto pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pump == mesh::raw_mesh_pump_result_t::infrastructure)
            ++burst_received;
    }
    assert (burst_received == actor_create_burst_size);

    std::size_t burst_replied = 0;
    while (burst_replied < actor_create_burst_size) {
        auto claim = second.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure,
          actor_create_burst_size - burst_replied,
          256 * 1024);
        assert (claim && !claim->records.empty ());
        for (const auto &record : claim->records) {
            const auto decoded = protocol::decode_actor_create_header (
              record.parts.front ());
            protocol::actor_create_reply_t burst_reply{
              {*record.correlation, 0, 0},
              protocol::actor_create_result_t::created,
              second_descriptor.node_routing_id,
              decoded.actor_id,
              1};
            assert (second.reply_actor_create (
              record, burst_reply));
            ++burst_replied;
        }
        assert (second.mailbox ().release (*claim));
    }

    std::size_t burst_completed = 0;
    while (burst_completed < actor_create_burst_size
           && mesh::service_liveness_registry_t::clock_t::now ()
                < deadline) {
        burst_completed = static_cast<std::size_t> (std::count_if (
          actor_create_burst_futures.begin (),
          actor_create_burst_futures.end (),
          [] (std::future<request_result_t> &future) {
              return future.wait_for (0ms) == std::future_status::ready;
          }));
        if (burst_completed < actor_create_burst_size) {
            const auto pump = await_task (first.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
            assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        }
    }
    assert (burst_completed == actor_create_burst_size);
    for (auto &future : actor_create_burst_futures) {
        const auto result = future.get ();
        assert (result.first
                == foundation::operation_terminal_t::completed);
        assert (!result.second.empty ());
    }

    // The paused probe advanced the registry's logical next-probe time by
    // one interval. Continue from that same logical clock instead of mixing
    // the synthetic probe time with the wall clock used by the pump loop.
    const auto liveness_base = paused_liveness_base + 5s;
    const auto first_probe = await_task (first.tick_liveness (liveness_base + 5s));
    assert (first_probe.probes.size () == 1);
    mesh::raw_mesh_pump_result_t probe_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (probe_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        probe_pump = await_task (second.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (probe_pump == mesh::raw_mesh_pump_result_t::infrastructure);

    mesh::raw_mesh_pump_result_t ack_pump =
      mesh::raw_mesh_pump_result_t::no_data;
    while (ack_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        ack_pump = await_task (first.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (ack_pump == mesh::raw_mesh_pump_result_t::infrastructure);
    const auto next_probe = await_task (first.tick_liveness (liveness_base + 10s));
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id
            != first_probe.probes.front ().probe_id);

    first.close ();
    second.close ();
}

protocol::relocation_prepare_t relocation_prepare_request (
  const mesh::service_node_descriptor_t &source_descriptor,
  const mesh::service_node_descriptor_t &target_descriptor)
{
    return protocol::relocation_prepare_t{
      protocol::relocation_id_t{555, 777},
      3,
      protocol::relocation_coordinator_fence_t{
        "coord-owner", 1, bytes ("coord-node"), 1, "store-v1"},
      protocol::relocation_target_fence_t{
        target_descriptor.node_routing_id,
        target_descriptor.lifecycle_generation, "target-owner", 9},
      protocol::relocation_role_t::source,
      protocol::relocation_object_t{
        protocol::relocation_object_kind_t::actor, "player", "actor-1", 4, 6},
      source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation,
      1024, 1, 0xdeadbeefu, 1};
}

// Spec 15/28 + node's own cross-language audit: an explicit, identity-
// matched relocationFailed(53) reply must resolve request_relocation_prepare
// promptly with its own outcome, not be silently dropped into the same "no
// result" a genuine timeout produces. Before this fix, request_relocation_
// prepare only ever extracted relocation_ready_t from the decoded reply —
// a relocation_failed_t reply fell through untouched, discarding both the
// fast explicit rejection and its wire failure_code.
void verify_relocation_prepare_failed_reply_resolves_promptly_with_identity_fencing ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{
        descriptor ("relocation-fence-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{
        descriptor ("relocation-fence-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto prepare =
      relocation_prepare_request (source_descriptor, target_descriptor);

    mesh::relocation_prepare_response_t response;
    std::atomic_bool source_settled{false};
    const auto started = std::chrono::steady_clock::now ();
    std::thread prepare_thread ([&] {
        response = await_task (source.request_relocation_prepare (
          target_descriptor.node_routing_id, prepare, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (target.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
            (void) await_task (source.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();
    const auto control =
      protocol::decode_relocation_control (record.parts.front ());
    const auto *decoded_prepare =
      std::get_if<protocol::relocation_prepare_t> (&control);
    assert (decoded_prepare && *decoded_prepare == prepare);

    assert (target.reply_relocation_failed (
      record,
      protocol::relocation_failed_t{
        prepare.relocation, prepare.target_attempt_generation,
        prepare.coordinator, prepare.target, prepare.object,
        protocol::relocation_role_t::target,
        static_cast<std::uint32_t> (
          protocol::framework_error_code::relocationDataLost)}));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    prepare_thread.join ();
    const auto elapsed = std::chrono::steady_clock::now () - started;

    assert (source_settled.load (std::memory_order_acquire));
    assert (!response.ready);
    assert (response.failed);
    assert (response.failed->relocation == prepare.relocation);
    assert (response.failed->target_attempt_generation
            == prepare.target_attempt_generation);
    assert (response.failed->coordinator == prepare.coordinator);
    assert (response.failed->target == prepare.target);
    assert (response.failed->object == prepare.object);
    assert (response.failed->failure_code
            == static_cast<std::uint32_t> (
                 protocol::framework_error_code::relocationDataLost));
    // Resolved by the explicit reply well inside the 5s request timeout —
    // proves this is its own prompt outcome, not the same "no result" a
    // timeout would also produce.
    assert (elapsed < 2s);

    source.close ();
    target.close ();
}

// actorJoin(28) wire-level request/reply, mirroring the relocationPrepare
// harness pattern above: originate via request_actor_join, the target claims
// the infrastructure mailbox record (standing in for the real admission
// handler, which is a separate, not-yet-landed increment — see
// runtime/mesh/raw_mesh_node_owner.{hpp,cpp} and the C-5 report), and replies
// via reply_actor_join.
protocol::actor_join_request_t actor_join_request (
  const mesh::service_node_descriptor_t &source_descriptor,
  const mesh::service_node_descriptor_t &target_descriptor,
  std::uint64_t correlation,
  std::string actor_id,
  std::string spot_id,
  bool entry = false)
{
    return protocol::actor_join_request_t{
      correlation,
      protocol::actor_route_fence_t{
        std::move (actor_id), 4, source_descriptor.node_routing_id,
        source_descriptor.lifecycle_generation, 11, 12},
      entry,
      protocol::spot_route_fence_t{
        std::move (spot_id), 9, target_descriptor.node_routing_id,
        target_descriptor.lifecycle_generation, 21, 22}};
}

std::optional<mesh::service_mailbox_claim_t> claim_actor_join (
  mesh::raw_mesh_node_owner_t &target)
{
    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (target.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    return claim;
}

// Accepted case: the tail's spot ref, membershipEpoch, and
// receiveChunkLimitBytes must all thread through unchanged from the
// receiver's reply_actor_join call to the originate side's decoded tail —
// this is the exact information the ORIGINATE side's completion bookkeeping
// (dd23fb0b36's receiveChunkLimitBytes consumption) depends on.
void verify_actor_join_accepted_reply_threads_chunk_limit_and_epoch ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-accept-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-accept-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request = actor_join_request (
      source_descriptor, target_descriptor, 4242, "actor-1", "spot-1");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (
          target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();
    const auto decoded = protocol::decode_actor_join_28 (record.parts);
    assert (decoded.correlation == request.correlation);
    assert (decoded.actor.id == request.actor.actor_id);
    assert (decoded.actor.generation == request.actor.object_generation);
    assert (decoded.actor.target_node_rid
            == request.actor.target_node_routing_id);
    assert (decoded.actor.target_node_generation
            == request.actor.target_node_generation);
    assert (decoded.actor.expected_authority_owner_generation
            == request.actor.authority_owner_generation);
    assert (decoded.actor.expected_owner_lease_generation
            == request.actor.owner_lease_generation);
    assert (decoded.entry == request.entry);
    assert (decoded.target_spot.id == request.target_spot.spot_id);
    assert (decoded.target_spot.generation
            == request.target_spot.object_generation);
    assert (decoded.target_spot.target_node_rid
            == request.target_spot.target_node_routing_id);
    assert (decoded.target_spot.target_node_generation
            == request.target_spot.target_node_generation);
    assert (decoded.target_spot.expected_authority_owner_generation
            == request.target_spot.authority_owner_generation);
    assert (decoded.target_spot.expected_owner_lease_generation
            == request.target_spot.owner_lease_generation);
    assert (!decoded.payload);
    assert (record.correlation && *record.correlation == request.correlation);

    assert (target.reply_actor_join (
      record, protocol::actor_join_result_t::accepted,
      protocol::actor_join_reply_spot_ref_t{"spot-1", 9}, 3, 8192));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    const auto &response = outcome.reply;
    assert (response);
    assert (response->join_result == protocol::actor_join_result_t::accepted);
    assert (response->spot && response->spot->spot_id == "spot-1");
    assert (response->spot->object_generation == 9);
    assert (response->membership_epoch == 3);
    assert (response->receive_chunk_limit_bytes == 8192);

    source.close ();
    target.close ();
}

// Rejected case: a typed rejection (no spot) must resolve as rejected, not
// as a timeout/no-result and not as accepted.
void verify_actor_join_rejected_reply_completes_typed_failure ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-reject-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-reject-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request = actor_join_request (
      source_descriptor, target_descriptor, 4343, "actor-2", "spot-2");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (
          target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();
    const protocol::application_payload_t application_reply{
      "actor-join-rejected", "application/octet-stream", bytes ("not-approved")};
    bool terminal_payload_rejected = false;
    try {
        (void) target.reply_actor_join (record, protocol::actor_join_result_t::rejected,
                                        std::nullopt, 0, 0, 105, 17, application_reply);
    }
    catch (const std::invalid_argument &) {
        terminal_payload_rejected = true;
    }
    assert (terminal_payload_rejected);
    assert (target.reply_actor_join (record, protocol::actor_join_result_t::rejected, std::nullopt,
                                     0, 0, 0, 0, application_reply));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    const auto &response = outcome.reply;
    assert (response);
    assert (response->join_result == protocol::actor_join_result_t::rejected);
    assert (!response->spot);
    assert (outcome.application_reply == application_reply);

    source.close ();
    target.close ();
}

// Identity-fence case: the receiver must reject an inbound actorJoin(28)
// whose "actor" fence claims a source node lifecycle generation different
// from the one this transport connection was actually admitted under — a
// wrong-generation sender is a protocol error, exactly like the existing
// actorCreate/userSpotCreate/userSpotClose/relocationPrepare fence checks.
void verify_actor_join_wrong_source_generation_is_fenced ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-fence-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-fence-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    auto request = actor_join_request (
      source_descriptor, target_descriptor, 4444, "actor-3", "spot-3");
    // Claim a source node generation the connection was not actually
    // admitted under.
    request.actor.target_node_generation =
      source_descriptor.lifecycle_generation + 1;

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (
          target_descriptor.node_routing_id, request, std::nullopt, 300ms));
        source_settled.store (true, std::memory_order_release);
    });

    mesh::raw_mesh_pump_result_t pumped = mesh::raw_mesh_pump_result_t::no_data;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (pumped != mesh::raw_mesh_pump_result_t::protocol_error
           && std::chrono::steady_clock::now () < receive_deadline) {
        pumped = await_task (target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::protocol_error);
    // A fenced inbound frame must not have reached the infrastructure
    // mailbox at all.
    assert (!target.mailbox ().try_claim (
      mesh::service_mailbox_domain_t::infrastructure, 1, 4096));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    // Fenced on the receiver: the originate side never gets a reply and
    // times out — and the typed outcome must classify that timeout as
    // deadline_exceeded (spec 32 §5), not a false "accepted"/"rejected"
    // and not an unclassified transport failure.
    assert (!outcome.reply);
    assert (outcome.failure == mesh::actor_join_wire_failure_t::deadline_exceeded);

    source.close ();
    target.close ();
}

// Malformed-reply classification (spec 32 §5): a decodable reply whose
// correlation does not match the request this call sent is a stale or
// misrouted reply — the typed outcome must classify it as protocol_error,
// never as accepted/rejected and never as a plain timeout.
void verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-mismatch-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-mismatch-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request = actor_join_request (
      source_descriptor, target_descriptor, 4545, "actor-4", "spot-4");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (
          target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    // Reply through the raw route (so the frame reaches the pending
    // request), but stamp a correlation that does not identify it.
    auto misrouted = claim->records.front ();
    misrouted.correlation = request.correlation + 2;
    assert (target.reply_actor_join (
      misrouted, protocol::actor_join_result_t::accepted,
      protocol::actor_join_reply_spot_ref_t{"spot-4", 9}, 3, 8192));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    assert (!outcome.reply);
    assert (outcome.failure == mesh::actor_join_wire_failure_t::protocol_error);

    source.close ();
    target.close ();
}

// Canonical actorJoin(28)'s target Spot fence is intentionally not a
// transport decision. Once the source peer and its execution generation are
// authenticated, raw ingress routes this frame to the Store-backed admission,
// which owns the target/owner fence terminal.
void verify_actor_join_target_fence_reaches_admission ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-thin-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("actor-join-thin-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    auto request = actor_join_request (
      source_descriptor, target_descriptor, 4646, "actor-5", "spot-5");
    request.target_spot.target_node_routing_id = bytes ("stale-target");
    request.target_spot.target_node_generation = 99;

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (
          target_descriptor.node_routing_id, request, std::nullopt, 5s));
        settled.store (true, std::memory_order_release);
    });
    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    assert (claim->records.front ().correlation
            && *claim->records.front ().correlation == request.correlation);
    assert (target.reply_actor_join (
      claim->records.front (), protocol::actor_join_result_t::rejected,
      std::nullopt, 0, 0));
    assert (target.mailbox ().release (*claim));

    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();
    assert (settled.load (std::memory_order_acquire));
    assert (outcome.reply
            && outcome.reply->join_result == protocol::actor_join_result_t::rejected);
    source.close ();
    target.close ();
}

// The same exact-identity fencing that already protects relocation_ready_t
// must also protect relocation_failed_t: a reply whose identity does not
// match the sent prepare (a stale or wrong-attempt reply) must resolve
// neither ready nor failed.
void verify_relocation_prepare_failed_reply_with_mismatched_identity_is_fenced ()
{
    mesh::raw_mesh_node_owner_t source (
      mesh::raw_mesh_node_options_t{
        descriptor ("relocation-mismatch-source")});
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{
        descriptor ("relocation-mismatch-target")});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto prepare =
      relocation_prepare_request (source_descriptor, target_descriptor);

    mesh::relocation_prepare_response_t response;
    std::atomic_bool source_settled{false};
    std::thread prepare_thread ([&] {
        response = await_task (source.request_relocation_prepare (
          target_descriptor.node_routing_id, prepare, 300ms));
        source_settled.store (true, std::memory_order_release);
    });

    std::optional<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (target.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
            (void) await_task (source.pump_one (
              mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = claim->records.front ();

    // A different exact identity (target_attempt_generation) than the one
    // this prepare sent — the framework's own "newest attempt wins /
    // stale identity discarded" rule (spec 15 §4.2) applies here too.
    auto mismatched = prepare;
    ++mismatched.target_attempt_generation;
    assert (target.reply_relocation_failed (
      record,
      protocol::relocation_failed_t{
        mismatched.relocation, mismatched.target_attempt_generation,
        mismatched.coordinator, mismatched.target, mismatched.object,
        protocol::relocation_role_t::target,
        static_cast<std::uint32_t> (
          protocol::framework_error_code::relocationDataLost)}));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    prepare_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    assert (!response.ready);
    assert (!response.failed);

    source.close ();
    target.close ();
}

} // namespace

int main ()
{
    using zlink::framework::detail::backend::transient_route_errno;
    assert (transient_route_errno (EHOSTUNREACH));
    assert (transient_route_errno (ENETUNREACH));
    assert (transient_route_errno (ENOTCONN));
    assert (!transient_route_errno (EINVAL));
    verify_actor_create_command_49_roundtrip ();
    verify_bound_session_bind_retries_until_route_is_admitted ();
    verify_bound_session_bind_permanent_absence_is_bounded ();
    verify_bound_session_bind_reply_completes_registered_operation ();
    verify_actor_create_retries_until_route_is_admitted ();
    verify_actor_create_retry_timeout_is_unavailable ();
    verify_actor_create_from_dispatch_thread_does_not_block ();
    verify_topology_snapshot_and_connection_fence ();
    verify_route_mesh_descriptor_uses_generated_capability ();
    verify_duplicate_connection_survivor_is_symmetric ();
    verify_lifecycle_token_requires_current_discovery_expectation ();
    verify_physical_candidates_preserve_survivor ();
    verify_stale_rid_disconnect_preserves_same_endpoint_replacement ();
    verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ();
    verify_raw_admission_rejects_lifecycle_mismatch ();
    verify_object_client_connection_requirement ();
    verify_manual_object_client_pair_ends_not_required ();
    verify_signed_weight_contract ();
    verify_independent_mailbox_domains_and_claim_fence ();
    verify_liveness_reuses_probe_and_fences_reconnect ();
    verify_location_descriptor_cas_snapshot_and_watch ();
    verify_manual_and_automatic_classic_fanout ();
    verify_client_server_stale_admission_reply_is_discarded ();
    verify_client_server_plain_hello_is_rejected ();
    verify_client_server_independent_raw_path ();
    verify_client_server_admits_before_monitor_drain ();
    verify_client_server_weighted_selection ();
    verify_raw_owner_node_send_and_liveness ();
    verify_relocation_prepare_failed_reply_resolves_promptly_with_identity_fencing ();
    verify_relocation_prepare_failed_reply_with_mismatched_identity_is_fenced ();
    verify_actor_join_accepted_reply_threads_chunk_limit_and_epoch ();
    verify_actor_join_rejected_reply_completes_typed_failure ();
    verify_actor_join_wrong_source_generation_is_fenced ();
    verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ();
    verify_actor_join_target_fence_reaches_admission ();
    return 0;
}

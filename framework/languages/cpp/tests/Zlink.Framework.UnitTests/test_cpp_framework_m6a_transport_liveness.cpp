/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "../support/m6a_native_calls.hpp"

#include "mailbox_claim_fixture.hpp"

#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

void verify_liveness_shared_record_contract ()
{
    auto root = std::filesystem::path (__FILE__).parent_path ();
    for (int depth = 0; depth < 4; ++depth)
        root = root.parent_path ();
    std::ifstream fixture (root / "test/fixtures/liveness-any-record.tsv");
    assert (fixture.is_open ());
    std::string row;
    std::size_t count = 0;
    while (std::getline (fixture, row)) {
        if (row.empty () || row.front () == '#')
            continue;
        ++count;
        std::string kind;
        std::int64_t received_ms, alive_ms, expires_ms;
        std::size_t expected_probes;
        std::istringstream fields (row);
        assert (fields >> kind >> received_ms >> alive_ms >> expires_ms >> expected_probes);
        zlink::framework::runtime::state_lane_t owner_lane;
        mesh::service_liveness_registry_t owner (owner_lane);
        const auto start = mesh::service_liveness_registry_t::clock_t::time_point{};
        const auto peer = bytes ("peer");
        auto receipt = owner.admit (peer, bytes ("current"), start);
        (void) owner.tick (start + 5s);
        if (kind == "retired_connection")
            owner.admit (peer, bytes ("replacement"), start + 6s);
        else if (kind == "other_connection")
            receipt = owner.admit (bytes ("other"), bytes ("other"), start);
        if (kind == "previous_ack")
            assert (!owner.acknowledge (peer, bytes ("current"), 2,
                                        start + std::chrono::milliseconds (received_ms)));
        else
            receipt->record_received (start + std::chrono::milliseconds (received_ms));
        const auto alive = owner.tick (start + std::chrono::milliseconds (alive_ms));
        assert (std::find (alive.timed_out_nodes.begin (), alive.timed_out_nodes.end (), peer)
                == alive.timed_out_nodes.end ());
        assert (static_cast<std::size_t> (std::count_if (
                  alive.probes.begin (), alive.probes.end (),
                  [&peer] (const auto &probe) { return probe.node_routing_id == peer; }))
                == expected_probes);
        const auto expired = owner.tick (start + std::chrono::milliseconds (expires_ms));
        assert (std::find (expired.timed_out_nodes.begin (), expired.timed_out_nodes.end (), peer)
                != expired.timed_out_nodes.end ());
    }
    assert (count == 9);
}

void verify_liveness_reuses_probe_and_fences_reconnect ()
{
    {
        // Transport liveness §3: admission immediately makes the first probe due.
        zlink::framework::runtime::state_lane_t owner_lane;
        mesh::service_liveness_registry_t owner (owner_lane);
        const auto admitted = mesh::service_liveness_registry_t::clock_t::time_point{};
        owner.admit (bytes ("initial-peer"), bytes ("initial-pair"), admitted);
        assert (owner.tick (admitted).probes.size () == 1);
        assert (owner.tick (admitted + 5s - 1ns).probes.empty ());
        assert (owner.tick (admitted + 5s).probes.size () == 1);
    }
    {
        zlink::framework::runtime::state_lane_t owner_lane;
        mesh::service_liveness_registry_t owner (owner_lane);
        const auto start = mesh::service_liveness_registry_t::clock_t::time_point{};
        const auto peer = bytes ("peer");
        const auto connection = bytes ("pair");
        owner.admit (peer, connection, start);
        const auto probe = owner.tick (start + 5s).probes.front ().probe_id;
        assert (!owner.acknowledge (peer, connection, probe + 1, start + 14s));
        assert (owner.tick (start + 15s).timed_out_nodes.empty ());
        assert (owner.tick (start + 20s).probes.front ().probe_id == probe);
        assert (owner.tick (start + 29s).timed_out_nodes
                == std::vector<std::vector<std::uint8_t>>{peer});
    }
    zlink::framework::runtime::state_lane_t liveness_lane;
    mesh::service_liveness_registry_t liveness (liveness_lane, 10ms, 30ms);
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
    assert (!liveness.acknowledge (node, first_connection, probe_id + 1, start + 21ms));
    assert (liveness.acknowledge (node, first_connection, probe_id, start + 21ms));

    liveness.admit (node, replacement_connection, start + 22ms);
    assert (!liveness.disconnect (node, first_connection));
    assert (!liveness.acknowledge (node, first_connection, probe_id, start + 23ms));
    assert (liveness.tick (start + 53ms).timed_out_nodes
            == std::vector<std::vector<std::uint8_t>>{node});
}

void verify_liveness_physical_close (bool receive, bool replacement)
{
    auto context = std::make_shared<zlink::context_t> ();
    context->options ().auto_hwm_enabled (false);
    mesh::raw_mesh_node_owner_t source (
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("liveness-source")}),
      context);
    zlink::dealer_socket_t target (*context);
    const auto remote = descriptor ("liveness-target");
    target.set_routing_id (zlink::routing_id_t::from (remote.node_routing_id));
    constexpr std::size_t record_bytes = 1024 * 1024;
    target.options ().recv_hwm (zlink::byte_count_t::bytes (record_bytes + 256));
    target.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    source.start ();
    target.connect (source.endpoint ());
    const auto selected_by = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    std::size_t selected = 0;
    while (selected == 0 && mesh::service_liveness_registry_t::clock_t::now () < selected_by)
        selected += source.observe_routes ();
    assert (selected == 1);
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    assert (source.admit_peer (remote, bytes ("old-route"), now)
            == mesh::peer_admission_result_t::admitted);
    const auto old = source.liveness ().connection (remote.node_routing_id);
    const auto control_pending = source.pending_operation_count ();
    std::optional<zlink::framework::task_t<zlink::submit_result_t>> pending;
    std::vector<std::string> native_completions;
    const auto local = source.topology ().local_descriptor ();
    const protocol::bound_session_send_t session{
      {"liveness-actor", 1, local.node_routing_id, local.lifecycle_generation, 1, 1}, 1};
    const protocol::application_payload_t payload{"LivenessPressure", "application/json",
                                                  std::vector<std::uint8_t> (record_bytes)};
    for (std::size_t record = 0; record < 32 && !pending; ++record) {
        native_completions.clear ();
        auto send = source.send_bound_session_result (
          remote.node_routing_id, session, payload,
          [&native_completions] (std::string_view stage, std::string_view result) {
              if (stage == "router_admission_complete")
                  native_completions.emplace_back (result);
          });
        if (source.pending_operation_count () > control_pending)
            pending.emplace (std::move (send));
        else
            assert (await_task (std::move (send)) == zlink::submit_result_t::ok);
    }
    assert (pending && !pending->await_ready ());
    if (source.pending_operation_count () != control_pending + 1)
        throw std::runtime_error ("SEND pending before=" + std::to_string (control_pending)
                                  + " after=" + std::to_string (source.pending_operation_count ()));
    if (receive)
        old->record_received (now + 14s);
    if (replacement)
        assert (source.admit_peer (remote, bytes ("new-route"), now + 14s)
                == mesh::peer_admission_result_t::admitted);
    const auto expired = await_task (source.tick_liveness (now + 15s));
    assert (expired.timed_out_nodes.empty () == (receive || replacement));
    if (receive || replacement) {
        assert (source.topology ().peer (remote.node_routing_id));
        assert (source.observe_routes () == 0);
        assert (!pending->await_ready ());
        source.close ();
        return;
    } else {
        assert (!source.topology ().peer (remote.node_routing_id));
        assert (await_task (source.tick_liveness (now + 16s)).timed_out_nodes.empty ());
    }
    const auto settled_by = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while (!pending->await_ready ()
           && mesh::service_liveness_registry_t::clock_t::now () < settled_by)
        (void) await_task (
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false));
    assert (pending->await_ready ());
    assert (native_completions == std::vector<std::string>{"not_found"});
    assert (await_task (std::move (*pending)) != zlink::submit_result_t::ok);
    source.close ();
}

void verify_liveness_without_selected_route ()
{
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t source (
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("liveness-missing-source")}),
      context);
    source.start ();
    const auto remote = descriptor ("liveness-missing-target");
    zlink::dealer_socket_t departed (*context);
    departed.set_routing_id (zlink::routing_id_t::from (remote.node_routing_id));
    departed.connect (source.endpoint ());
    const auto selected_by = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    std::size_t selected = 0;
    while (!selected && mesh::service_liveness_registry_t::clock_t::now () < selected_by)
        selected += source.observe_routes ();
    assert (selected == 1);
    departed.close ();
    (void) source.disconnect_peer (remote.node_routing_id, remote.advertised_endpoint);
    while (source.peer_route (remote.node_routing_id).first != 0
           && mesh::service_liveness_registry_t::clock_t::now () < selected_by)
        (void) source.observe_routes ();
    assert (source.peer_route (remote.node_routing_id).first == 0);
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    // Preserve a stale logical admission after Core has ended the physical RID.
    assert (source.admit_peer (remote, bytes ("missing-route"), now)
            == mesh::peer_admission_result_t::admitted);
    const protocol::application_payload_t payload{"LivenessAfterCleanup", "application/json",
                                                  bytes ("delivered")};
    // Mandatory directed SEND reports an absent route as NotConnected (Core ROUTER section 5).
    // The logical admission remains for liveness to perform disconnect_rid cleanup.
    const auto absent_route =
      await_task (source.send_to_node_result (remote.node_routing_id, payload));
    assert (absent_route == zlink::submit_result_t::not_connected);
    assert (peer_ready (source, remote.node_routing_id));
    const auto expired = await_task (source.tick_liveness (now + 15s));
    assert (expired.timed_out_nodes
            == std::vector<std::vector<std::uint8_t>>{remote.node_routing_id});
    assert (!source.topology ().peer (remote.node_routing_id));
    assert (await_task (source.tick_liveness (now + 16s)).timed_out_nodes.empty ());
    assert (source.pending_operation_count () == 0);

    mesh::raw_mesh_node_owner_t survivor (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("liveness-survivor")}));
    survivor.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto live = survivor.topology ().local_descriptor ();
    survivor.expect_peer (local);
    assert (source.connect_peer (survivor.endpoint (), live));
    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    while (
      (!peer_ready (source, live.node_routing_id) || !peer_ready (survivor, local.node_routing_id))
      && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        (void) await_task (source.pump_one (now));
        (void) await_task (survivor.pump_one (now));
    }
    assert (peer_ready (source, live.node_routing_id));
    assert (peer_ready (survivor, local.node_routing_id));
    assert (await_task (source.send_to_node (live.node_routing_id, payload)));
    assert (await_task (survivor.send_to_node (local.node_routing_id, payload)));
    for (auto *receiver : {&source, &survivor}) {
        zlink::framework::test::mailbox_claim_t claim;
        while (!claim && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
            (void) await_task (receiver->pump_one (now));
            claim = zlink::framework::test::claim_mailbox (
              receiver->mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
        }
        assert (claim && claim->records.size () == 1);
        assert (protocol::decode_application_payload (
                  protocol::message_bytes (claim->records.front ()->parts ().at (1)))
                == payload);
        assert (receiver->mailbox ().release (*claim));
    }
    survivor.close ();
    source.close ();
}

#if !defined(_WIN32)
void verify_liveness_propagates_binding_failure (bool missing)
{
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t source (
      zlink::framework::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{descriptor ("liveness-terminated-source")}),
      context);
    source.start ();
    const auto remote = descriptor ("liveness-terminated-target");
    auto second = descriptor ("liveness-terminated-second");
    auto third = descriptor ("liveness-terminated-third");
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    std::vector<zlink::framework::task_t<bool>> pending;
    std::vector<std::future<foundation::operation_terminal_t>> terminals;
    for (const auto &peer : {remote, second, third}) {
        assert (
          source.topology ().admit (peer, bytes ("terminated-route"), now, protocol::command::hello)
          == mesh::peer_admission_result_t::admitted);
        auto terminal = std::make_shared<std::promise<foundation::operation_terminal_t>> ();
        terminals.push_back (terminal->get_future ());
        pending.push_back (source.request_to_node (
          peer.node_routing_id, {"PendingCleanup", "application/json", bytes ("request")}, 2s,
          [terminal] (auto result, auto) { terminal->set_value (result); }));
    }
    assert (source.pending_operation_count () == 3);
    int notifications = 0;
    source.topology ().set_change_handler ([&] {
        ++notifications;
        assert (source.topology ().peers ().empty ());
    });
    const auto timeout_at =
      source.liveness ().connection (remote.node_routing_id)->deadline.load ();
    const auto prior_injections = injected_disconnect_rid_calls.load (std::memory_order_relaxed);
    injected_disconnect_result.store (
      missing ? ZLINK_CONNECT_NOT_FOUND : ZLINK_CONNECT_INTERNAL_ERROR, std::memory_order_relaxed);
    fail_next_disconnect_rid.store (true, std::memory_order_release);
    bool observed = false;
    try {
        (void) await_task (source.tick_liveness (timeout_at));
    }
    catch (const zlink::connect_error_t &error) {
        observed = true;
        assert (error.result () == zlink::connect_result_t::internal_error);
        assert (error.internal_errno () == EIO);
    }
    assert (observed == !missing);
    assert (source.topology ().peers ().empty ());
    assert (notifications == 3);
    assert (source.pending_operation_count () == 0);
    for (auto &terminal : terminals)
        assert (terminal.get () == foundation::operation_terminal_t::route_unavailable);
    for (auto &request : pending)
        assert (!request.result ());
    assert (await_task (source.tick_liveness (timeout_at + 1s)).timed_out_nodes.empty ());
    assert (notifications == 3);
    source.topology ().set_change_handler ({});
    assert (injected_disconnect_rid_calls.load (std::memory_order_relaxed) == prior_injections + 1);
    source.close ();
}
#endif
}

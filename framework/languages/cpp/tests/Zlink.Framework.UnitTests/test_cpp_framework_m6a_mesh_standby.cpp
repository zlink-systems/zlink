/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"


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

// Opt-in Core boundary repro: send the first new handshake record after local
// promotion, with or without waiting for the peer to observe its promotion.
void verify_standby_promotion_public_send (bool observe_remote_first)
{
    zlink::context_t context;
    zlink::router_socket_t lower (context), higher (context);
    const auto setup = [] (auto &socket, const char *rid) {
        socket.set_routing_id (zlink::routing_id_t::from (rid));
        socket.options ().handover (true);
        socket.options ().mandatory (true);
        socket.options ().linger (0ms);
        socket.options ().reconnect_interval (0ms);
        socket.bind ("tcp://127.0.0.1:*");
    };
    setup (lower, "A-promotion");
    setup (higher, "Z-promotion");
    const auto wait_route = [] (auto &socket, const char *peer, std::uint64_t previous = 0) {
        zlink::poller_t poller;
        poller.add (socket, zlink::poll_event_flag_t::pollroute, 0);
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (std::chrono::steady_clock::now () < deadline) {
            for (const auto &route : socket.routes_snapshot ()) {
                if (route.routing_id == zlink::routing_id_t::from (peer)
                    && route.route_generation != previous)
                    return route.route_generation;
            }
            zlink::poll_event_t event;
            (void) poller.wait (&event, 1, 10ms);
        }
        std::fprintf (stderr, "selected route did not change: peer=%s previous=%llu\n", peer,
                      static_cast<unsigned long long> (previous));
        assert (false && "selected route did not change");
        return std::uint64_t{0};
    };
    const auto connect = [] (auto &socket, const char *peer, const auto &endpoint) {
        socket.options ().connect_routing_id (zlink::routing_id_t::from (peer));
        socket.connect (endpoint);
    };
    const auto higher_endpoint = higher.options ().last_endpoint ();
    connect (higher, "A-promotion", lower.options ().last_endpoint ());
    const auto lower_initial = wait_route (lower, "Z-promotion");
    const auto higher_initial = wait_route (higher, "A-promotion");
    connect (lower, "Z-promotion", higher_endpoint);
    const auto lower_selected = wait_route (lower, "Z-promotion", lower_initial);
    const auto higher_selected = wait_route (higher, "A-promotion", higher_initial);
    lower.disconnect (higher_endpoint);
    (void) wait_route (lower, "Z-promotion", lower_selected);
    if (observe_remote_first)
        (void) wait_route (higher, "A-promotion", higher_selected);
    lower.send (zlink::routing_id_t::from ("Z-promotion"))
      .message (zlink::message_t::from ("new-route-hello"))
      .submit ();
    (void) wait_route (higher, "A-promotion", higher_selected);
    higher.options ().recv_timeout (2s);
    zlink::received_t received;
    assert (higher.recv (received) == 0);
    assert (received.parts ().size () == 1);
    const auto &part = received.parts ().front ();
    assert (std::string_view (reinterpret_cast<const char *> (part.data ()), part.size ())
            == "new-route-hello");
}

void verify_standby_promotion_readmits_both_peers ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("Z-standby-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("A-standby-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    assert (source.connect_peer (target.endpoint (), remote));
    const auto pump = [&] {
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        (void) await_task (source.tick_liveness (now));
        (void) await_task (target.tick_liveness (now));
    };
    const auto await_replacement = [&] (const auto &source_previous, const auto &target_previous) {
        const auto ready = [&] {
            const auto a = source.topology ().peer (remote.node_routing_id);
            const auto b = target.topology ().peer (local.node_routing_id);
            return a && a->ready && b && b->ready && a->connection_id != source_previous
                   && b->connection_id != target_previous;
        };
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (!ready () && std::chrono::steady_clock::now () < deadline)
            pump ();
        assert (ready ());
    };
    await_replacement (std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{});
    auto source_previous = source.topology ().peer (remote.node_routing_id)->connection_id;
    auto target_previous = target.topology ().peer (local.node_routing_id)->connection_id;
    assert (target.connect_peer (source.endpoint (), local));
    await_replacement (source_previous, target_previous);
    source_previous = source.topology ().peer (remote.node_routing_id)->connection_id;
    target_previous = target.topology ().peer (local.node_routing_id)->connection_id;
    target.disconnect_peer (source.endpoint ());
    await_replacement (source_previous, target_previous);
    source.close ();
    target.close ();
}

void verify_stale_rid_disconnect_preserves_same_endpoint_replacement ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("replacement-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    auto old_target = std::make_unique<mesh::raw_mesh_node_owner_t> (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("replacement-old"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
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
        (void) source.observe_routes ();
        std::this_thread::sleep_for (1ms);
    }
    // Regression precondition: reconnect still owns the endpoint after the
    // old RID is gone, so connecting the replacement must retarget that
    // existing endpoint intent rather than keep publishing the old alias.
    assert (!source.topology ().peer (old_descriptor.node_routing_id));

    auto replacement_options = descriptor ("replacement-new", endpoint);
    auto replacement = std::make_unique<mesh::raw_mesh_node_owner_t> (mesh::raw_mesh_node_options_t{
      .descriptor = std::move (replacement_options),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    replacement->start ();
    const auto replacement_descriptor = replacement->topology ().local_descriptor ();
    admit_pair (source, *replacement, replacement_descriptor);

    assert (source.disconnect_peer (old_descriptor.node_routing_id, endpoint));
    assert (source.topology ().peer (replacement_descriptor.node_routing_id));

    const auto stable_deadline = std::chrono::steady_clock::now () + 500ms;
    while (std::chrono::steady_clock::now () < stable_deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) replacement->observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (replacement->pump_one (now));
        assert (source.topology ().peer (replacement_descriptor.node_routing_id));
        std::this_thread::sleep_for (1ms);
    }

    source.close ();
    replacement->close ();
}

void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor (bool lower_first)
{
    // Admit the first direction before creating the opposite pipe. Core keeps
    // the lower RID's outbound route; only the higher-first case replaces it.
    mesh::raw_mesh_node_owner_t lower (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bilateral-aa"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t higher (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bilateral-zz"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    lower.start ();
    higher.start ();
    const auto lower_descriptor = lower.topology ().local_descriptor ();
    const auto higher_descriptor = higher.topology ().local_descriptor ();
    lower.expect_peer (higher_descriptor);
    higher.expect_peer (lower_descriptor);
    auto &first = lower_first ? lower : higher;
    auto &second = lower_first ? higher : lower;
    assert (
      first.connect_peer (second.endpoint (), lower_first ? higher_descriptor : lower_descriptor));

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.observe_routes ();
        (void) higher.observe_routes ();
        (void) await_task (lower.pump_one (now));
        (void) await_task (higher.pump_one (now));
        const auto lower_peer = lower.topology ().peer (higher_descriptor.node_routing_id);
        const auto higher_peer = higher.topology ().peer (lower_descriptor.node_routing_id);
        if (lower_peer && lower_peer->ready && higher_peer && higher_peer->ready)
            break;
    }

    const auto lower_survivor = lower.topology ().peer (higher_descriptor.node_routing_id);
    const auto higher_survivor = higher.topology ().peer (lower_descriptor.node_routing_id);
    assert (lower_survivor);
    assert (higher_survivor);

    assert (lower_survivor->ready && higher_survivor->ready);
    assert (
      second.connect_peer (first.endpoint (), lower_first ? lower_descriptor : higher_descriptor));
    if (!lower_first) {
        bool replaced = false;
        while (!replaced && std::chrono::steady_clock::now () < deadline) {
            const auto now = mesh::service_liveness_registry_t::clock_t::now ();
            (void) lower.observe_routes ();
            (void) higher.observe_routes ();
            (void) await_task (lower.pump_one (now));
            (void) await_task (higher.pump_one (now));
            const auto lp = lower.topology ().peer (higher_descriptor.node_routing_id);
            const auto hp = higher.topology ().peer (lower_descriptor.node_routing_id);
            replaced = lp && lp->ready && hp && hp->ready
                       && lp->connection_id != lower_survivor->connection_id
                       && hp->connection_id != higher_survivor->connection_id;
        }
        assert (replaced);
    }
    const auto lower_selected = lower.topology ().peer (higher_descriptor.node_routing_id);
    const auto higher_selected = higher.topology ().peer (lower_descriptor.node_routing_id);
    const auto settle_deadline = std::chrono::steady_clock::now () + 100ms;
    while (std::chrono::steady_clock::now () < settle_deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) lower.observe_routes ();
        (void) higher.observe_routes ();
        (void) await_task (lower.pump_one (now));
        (void) await_task (higher.pump_one (now));
    }
    assert (lower.topology ().peer (higher_descriptor.node_routing_id)->connection_id
            == lower_selected->connection_id);
    assert (higher.topology ().peer (lower_descriptor.node_routing_id)->connection_id
            == higher_selected->connection_id);
    assert (lower.topology ().peers ().size () == 1);
    assert (higher.topology ().peers ().size () == 1);
    if (lower_first) {
        assert (lower_selected->connection_id == lower_survivor->connection_id);
        assert (higher_selected->connection_id == higher_survivor->connection_id);
    }
}

void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ()
{
    verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor (true);
    verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor (false);
}
}

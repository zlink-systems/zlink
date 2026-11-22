/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include <zlink.h>
#include <zlink.hpp>
#include <atomic>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

#if defined(ZLINK_TEST_CONNECT_COUNT)
#include <zlink.h>
#include <atomic>

std::atomic<std::size_t> native_connect_calls{0};
std::atomic<std::size_t> native_disconnect_calls{0};
std::atomic<std::size_t> native_call_sequence{0};
std::atomic<std::size_t> last_native_connect{0};
std::atomic<std::size_t> last_native_disconnect{0};
extern "C" zlink_connect_result_t __real_zlink_connect (void *, const char *);
extern "C" zlink_connect_result_t __real_zlink_disconnect (void *, const char *);
extern "C" zlink_connect_result_t __wrap_zlink_connect (void *socket, const char *endpoint)
{
    ++native_connect_calls;
    last_native_connect = ++native_call_sequence;
    return __real_zlink_connect (socket, endpoint);
}
extern "C" zlink_connect_result_t __wrap_zlink_disconnect (void *socket, const char *endpoint)
{
    ++native_disconnect_calls;
    last_native_disconnect = ++native_call_sequence;
    return __real_zlink_disconnect (socket, endpoint);
}
#endif

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void verify_automatic_intent_respects_manual_peer ()
{
    auto local_state = make_node ("tcp://127.0.0.1:0", "A-auto-local");
    auto remote_state = make_node ("tcp://127.0.0.1:0", "Z-auto-remote");
    zlink::framework::detail::mesh_node_runtime_t local (local_state), remote (remote_state);
    remote.start ().result ().value ();
    const auto rid = remote.status ().routing_id ();
    local_state->peer_connections.push_back ({1, rid, "tcp://127.0.0.1:1"});
    local.start ().result ().value ();
    // A manual intent at another endpoint owns the direction even before admission.
    assert (
      !local.native_node ().connect_peer (remote.listen_endpoint (), rid, 0, "default", true));
    local.disconnect_peer (rid, remote.listen_endpoint (), true);
    local.disconnect_peer (rid, "tcp://127.0.0.1:1", true);
    assert (
      !local.native_node ().connect_peer (remote.listen_endpoint (), rid, 0, "default", true));
    local_state->lane.run ([&] { local_state->peer_connections.clear (); }).get ();
    local.disconnect_peer (rid, "tcp://127.0.0.1:1");
    // Removing the manual intent permits the existing RID-order discovery direction.
    assert (local.native_node ().connect_peer (remote.listen_endpoint (), rid, 0, "default", true));
    local.stop ();
    remote.stop ();

    auto inbound_state = make_node ("tcp://127.0.0.1:0", "A-auto-inbound");
    auto outbound_state = make_node ("tcp://127.0.0.1:0", "Z-auto-outbound");
    zlink::framework::detail::mesh_node_runtime_t inbound (inbound_state),
      outbound (outbound_state);
    inbound.start ().result ().value ();
    outbound.start ().result ().value ();
    const auto outbound_rid = outbound.status ().routing_id ();
    assert (outbound.native_node ().connect_peer (inbound.listen_endpoint (),
                                                  inbound.status ().routing_id ()));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    std::size_t observed = 0;
    while (!observed && std::chrono::steady_clock::now () < deadline)
        observed += inbound.native_node ().transport ().observe_routes ();
    assert (observed != 0);
    // An inbound Core route suffices before its Framework admission completes.
    assert (!inbound.native_node ().connect_peer (outbound.listen_endpoint (), outbound_rid, 0,
                                                  "default", true));
    auto &inbound_transport = inbound.native_node ().transport ();
    auto &outbound_transport = outbound.native_node ().transport ();
    const auto inbound_rid = inbound.status ().routing_id ();
    const auto both_ready = [&] {
        return inbound_transport.peer_route (outbound_rid.to_bytes ()).second
               && outbound_transport.peer_route (inbound_rid.to_bytes ()).second;
    };
    while (!both_ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) inbound_transport.pump_one (now).result ().value ();
        (void) outbound_transport.pump_one (now).result ().value ();
        (void) inbound_transport.tick_liveness (now).result ().value ();
        (void) outbound_transport.tick_liveness (now).result ().value ();
    }
    assert (both_ready ());
    inbound.disconnect_peer (outbound_rid, outbound.listen_endpoint (), true);
    assert (both_ready ());
    inbound.stop ();
    outbound.stop ();
}

void verify_peer_intent_ids_and_callback_lifetime ()
{
    auto monitoring = zlink::framework::test::runtime_failure_monitoring ();
    zlink::framework::zlink_builder_t builder;
    zlink::framework::detail::bind_zlink_monitoring (builder, monitoring);
    auto mesh = builder.add_route_mesh ("intent-issuance");
    mesh.set_bind_host ("127.0.0.1")
      .listen (std::uint16_t{0})
      .set_routing_id (zlink::routing_id_t::from ("intent-issuance-local"));
    auto &connections = mesh.peer_connections ();
    const std::string first = "tcp://127.0.0.1:1";
    const std::string second = "tcp://127.0.0.1:2";
    const std::string host_only = "tcp://127.0.0.1:3";
    const std::string raw_only = "tcp://127.0.0.1:4";
    const std::string dynamic = "tcp://127.0.0.1:5";
    connections.connect (first);
    connections.connect (first);
    const auto configured = connections.list_connections ();
    assert (configured.size () == 2);
    assert (configured[1].intent_id == configured[0].intent_id + 1);
    const auto state =
      zlink::framework::detail::mesh_node_runtime_t::registrations (builder).front ();
    state->core_context = std::make_shared<zlink::context_t> ();
    state->spot_state->monitoring = monitoring;
    auto runtime = std::make_unique<zlink::framework::detail::mesh_node_runtime_t> (state);
    runtime->start ().result ().value ();
    assert (connections.list_connections ()[0].intent_id == configured[0].intent_id);
    assert (connections.list_connections ()[1].intent_id == configured[1].intent_id);

    // Both successful startup records issue an ID, even though their endpoint is the same.
    connections.connect (second);
    auto last = connections.list_connections ().back ().intent_id;
    assert (last == configured[1].intent_id + 3);
    // Only the first dynamic insertion issues a runtime ID after its public record's ID.
    connections.connect (second);
    auto next = connections.list_connections ().back ().intent_id;
    assert (next == last + 2);
    last = next;
    connections.connect (second);
    next = connections.list_connections ().back ().intent_id;
    assert (next == last + 1);
    last = next;
    assert (runtime->native_node ().connect_peer (host_only));
    assert (runtime->native_node ().transport ().connect_peer (raw_only));
    connections.connect (second);
    next = connections.list_connections ().back ().intent_id;
    assert (next == last + 1); // Neither standalone Host nor raw submission issues runtime IDs.
    last = next;
    runtime->connect_peer (dynamic);
    runtime->connect_peer (dynamic);
    connections.connect (second);
    next = connections.list_connections ().back ().intent_id;
    assert (next == last + 2);
    last = next;
    runtime->disconnect_peer (dynamic);
    runtime->connect_peer (dynamic);
    connections.connect (second);
    next = connections.list_connections ().back ().intent_id;
    assert (next == last + 2); // Erasing an endpoint permits a new runtime insertion ID.
    last = next;
    assert (!runtime->native_node ().connect_peer (""));
    runtime->connect_peer ("");
    connections.connect (second);
    next = connections.list_connections ().back ().intent_id;
    assert (next == last + 1);
    last = next;

    // The existing stop operation clears only Mesh intent membership, not Host suppression.
    runtime->native_node ().transport ().clear_peer_connection_intents ();
    const auto expected = zlink::routing_id_t::from ("intent-issuance-remote");
    runtime->disconnect_peer (expected, dynamic, true);
    assert (!runtime->native_node ().connect_peer (dynamic, expected, 0, "default", true));
    assert (!runtime->native_node ().connect_peer (host_only, expected, 0, "default", true));

    auto callbacks =
      state->lane
        .run (
          [&] { return std::pair{state->runtime_peer_connect, state->runtime_peer_disconnect}; })
        .get ();
    assert (callbacks.first && callbacks.second);
    auto retained_host = runtime->native_node ().shared_from_this ();
    const auto weak_host =
      std::weak_ptr<zlink::framework::runtime::host::public_host_runtime_t> (retained_host);
    runtime->stop ();
    runtime.reset ();
    const zlink::framework::mesh_peer_connection_t late{0, {}, dynamic};
    callbacks.first (late);
    callbacks.second (late);
    retained_host.reset ();
    assert (weak_host.expired ()); // Copied peer callbacks do not retain a Host cycle.
    callbacks.first (late);
    callbacks.second (late);
    connections.connect (second);
    assert (connections.list_connections ().back ().intent_id == last + 1);
    state->spot_state->runtime_failures ()->rethrow_if_failed ();
}

void verify_repeated_mesh_connect_keeps_one_physical_connection ()
{
    namespace mesh = zlink::framework::runtime::mesh;
    for (const bool typed : {false, true}) {
        auto context = std::make_shared<zlink::context_t> ();
        zlink::router_socket_t peer (*context);
        peer.options ().handover (true);
        peer.set_routing_id (zlink::routing_id_t::from (std::string ("intent-peer")));
        const auto endpoint = typed ? "inproc://typed-connect-intent" : "inproc://connect-intent";
        peer.bind (endpoint);
        auto monitor = peer.monitor_open (zlink::monitor_event::connection_ready);
        mesh::raw_mesh_node_options_t options;
        options.descriptor.mesh_name = "connect-intent";
        options.descriptor.node_routing_id = {'l', 'o', 'c', 'a', 'l'};
        options.descriptor.lifecycle_generation = 1;
        options.descriptor.descriptor_revision = 1;
        options.descriptor.advertised_endpoint = std::string (endpoint) + "-local";
        options.runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
        mesh::raw_mesh_node_owner_t local (std::move (options), context);
        local.start ();
        auto expected = local.topology ().local_descriptor ();
        expected.node_routing_id = {'i', 'n', 't', 'e', 'n', 't', '-', 'p', 'e', 'e', 'r'};
        expected.advertised_endpoint = endpoint;
        const auto connect = [&] {
            return typed ? local.connect_peer (endpoint, expected) : local.connect_peer (endpoint);
        };
        const auto wait_ready = [&] (std::chrono::milliseconds timeout) {
            zlink::poller_t poller;
            poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
            zlink::poll_event_t event;
            if (poller.wait (&event, 1, timeout) != 1)
                return false;
            return monitor.recv (zlink::recv_flags_t::dontwait).has_value ();
        };
        assert (connect ());
        assert (wait_ready (2s));
#if defined(ZLINK_TEST_CONNECT_COUNT)
        const auto first_connects = native_connect_calls.load ();
#endif
        assert (connect ());
        assert (!wait_ready (100ms));
#if defined(ZLINK_TEST_CONNECT_COUNT)
        assert (native_connect_calls == first_connects);
        if (typed) {
            for (int change = 0; change != 3; ++change) {
                if (change == 0)
                    expected.security_identity = "replacement-security";
                else if (change == 1)
                    ++expected.lifecycle_generation;
                else
                    expected.node_routing_id.push_back ('2');
                const auto connects = native_connect_calls.load ();
                const auto disconnects = native_disconnect_calls.load ();
                assert (connect ());
                assert (native_connect_calls == connects + 1);
                assert (native_disconnect_calls == disconnects + 1);
                assert (last_native_disconnect < last_native_connect);
                assert (connect ());
                assert (native_connect_calls == connects + 1);
            }
        }
#endif
        local.disconnect_peer (endpoint);
        assert (connect ());
        assert (wait_ready (2s));
        local.close ();
    }
}
} // namespace framework_mesh_node_vertical_tests

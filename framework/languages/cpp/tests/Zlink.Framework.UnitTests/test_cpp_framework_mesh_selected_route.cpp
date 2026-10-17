/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

// RouteMesh admission follows the Core selected route (#1087 FW01):
// Core ROUTER §10.1 owns which pipe carries a RID; the framework observes the
// selected-route snapshot and admits only that route
// (server 05-transport-liveness §5). A replaced route loses its admission and
// the new route admits through a new handshake, even when Core keeps the old
// pipe as a standby and never reports it disconnected.

#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <gtest/gtest.h>
#include <zlink.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace backend = zlink::framework::detail::backend;

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

mesh::raw_mesh_node_options_t options (const std::string &rid, std::uint64_t lifecycle_generation)
{
    mesh::raw_mesh_node_options_t result;
    result.runtime_failures =
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
    result.descriptor = mesh::service_node_descriptor_t{"selected-route-mesh",
                                                        bytes (rid),
                                                        1,
                                                        1,
                                                        "tcp://127.0.0.1:0",
                                                        {{"alpha", 100}},
                                                        mesh::service_node_state_t::preparing};
    result.descriptor.lifecycle_generation = lifecycle_generation;
    return result;
}

// One host ingress pass observes the Core selected routes and pumps one record.
void progress (mesh::raw_mesh_node_owner_t &owner)
{
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) owner.observe_routes ();
    (void) std::move (owner.pump_one (now)).result ();
}

bool pump_until (const std::vector<mesh::raw_mesh_node_owner_t *> &owners,
                 const std::function<bool ()> &condition,
                 std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (!condition () && std::chrono::steady_clock::now () < deadline) {
        for (auto *owner : owners)
            progress (*owner);
    }
    return condition ();
}

std::uint64_t admitted_generation (mesh::raw_mesh_node_owner_t &owner,
                                   const std::vector<std::uint8_t> &peer)
{
    const auto admitted = owner.topology ().peer (peer);
    return admitted ? admitted->descriptor.lifecycle_generation : 0;
}

TEST (CppFrameworkMeshSelectedRoute, AdmissionFollowsCoreSelectedRouteAcrossHandoverAndPromotion)
{
    const auto target_rid = bytes ("selected-route-target");
    mesh::raw_mesh_node_owner_t source (options ("selected-route-source", 101));
    auto first =
      std::make_unique<mesh::raw_mesh_node_owner_t> (options ("selected-route-target", 201));
    auto second =
      std::make_unique<mesh::raw_mesh_node_owner_t> (options ("selected-route-target", 202));
    source.start ();
    first->start ();
    second->start ();

    // Endpoint-only manual intent: the admitted identity is whatever process
    // answers the handshake on the Core selected route.
    ASSERT_TRUE (source.connect_peer (first->endpoint ()));
    ASSERT_TRUE (pump_until ({&source, first.get (), second.get ()},
                             [&] { return admitted_generation (source, target_rid) == 201; }));

    // Same RID, new process: Core hands the selected route to the new pipe
    // and keeps the first pipe as a standby without a disconnect.
    ASSERT_TRUE (source.connect_peer (second->endpoint ()));
    EXPECT_TRUE (pump_until ({&source, first.get (), second.get ()},
                             [&] { return admitted_generation (source, target_rid) == 202; }))
      << "admitted lifecycle generation stayed " << admitted_generation (source, target_rid);

    // The selected pipe ends; Core promotes the standby to a new route
    // generation. The first process answers the new handshake on it.
    second->close ();
    EXPECT_TRUE (pump_until ({&source, first.get ()},
                             [&] { return admitted_generation (source, target_rid) == 201; }))
      << "admitted lifecycle generation is " << admitted_generation (source, target_rid);

    source.close ();
    first->close ();
}

enum class remote_hello_t
{
    none,
    before_update,
    after_update
};

class CppFrameworkHelloExchange : public testing::TestWithParam<remote_hello_t>
{
};

TEST_P (CppFrameworkHelloExchange, RetainsFirstDescriptorUntilAdmit)
{
    mesh::raw_mesh_node_owner_t source (options ("hello-source", 101));
    source.start ();
    const auto initial = source.topology ().local_descriptor ();
    zlink::router_socket_t remote (source.context ());
    remote.set_routing_id (zlink::routing_id_t::from ("hello-target"));
    remote.options ().linger (0ms);
    remote.bind ("tcp://127.0.0.1:0");
    backend::raw_route_port_t port (remote);
    ASSERT_TRUE (source.connect_peer (remote.options ().last_endpoint ()));
    std::optional<backend::raw_received_t> received;
    const auto receive = [&] {
        received.reset ();
        return pump_until ({&source}, [&] {
            if (!received)
                received = port.try_receive ();
            return received.has_value ();
        });
    };
    ASSERT_TRUE (receive ());
    ASSERT_EQ (1u, received->parts.size ());
    const auto first = received->parts.front ().to_bytes ();
    EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::hello,
                                                      source.topology ().local_descriptor ()),
               first);
    auto descriptor = options ("hello-target", 201).descriptor;
    descriptor.advertised_endpoint = remote.options ().last_endpoint ();
    const auto receive_remote_hello = [&] {
        ASSERT_TRUE (port
                       .send (bytes ("hello-source"), {protocol::encode_route_mesh_admission (
                                                        protocol::command::hello, descriptor)})
                       .result ()
                       .value ());
        ASSERT_TRUE (receive ());
        EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::admit, initial),
                   received->parts.front ().to_bytes ());
        ASSERT_TRUE (source.topology ().peer (descriptor.node_routing_id));
        EXPECT_FALSE (source.topology ().peer (descriptor.node_routing_id)->ready);
    };
    if (GetParam () == remote_hello_t::before_update)
        ASSERT_NO_FATAL_FAILURE (receive_remote_hello ());
    const auto latest = source.publish_descriptor_update ([] (auto &descriptor) {
        ++descriptor.descriptor_revision;
        descriptor.state = mesh::service_node_state_t::serving;
    });
    ASSERT_EQ (initial.descriptor_revision + 1, latest.descriptor_revision);
    // The crossed HELLO must retain our first descriptor after publication too.
    if (GetParam () == remote_hello_t::after_update)
        ASSERT_NO_FATAL_FAILURE (receive_remote_hello ());
    // A successful send has completed; only the remote ADMIT remains withheld.
    const auto next_activity = source.liveness ().next_activity ();
    ASSERT_TRUE (next_activity);
    const auto tick = source.tick_liveness (*next_activity).result ().value ();
    ASSERT_TRUE (receive ());
    EXPECT_EQ (first, received->parts.front ().to_bytes ());
    if (GetParam () != remote_hello_t::none) {
        ASSERT_EQ (1u, tick.probes.size ());
        ASSERT_TRUE (receive ());
        EXPECT_EQ (protocol::encode_liveness (protocol::command::livenessProbe,
                                              tick.probes.front ().probe_id),
                   received->parts.front ().to_bytes ());
        ASSERT_TRUE (port
                       .send (bytes ("hello-source"),
                              {protocol::encode_liveness (protocol::command::livenessAck,
                                                          tick.probes.front ().probe_id)})
                       .result ()
                       .value ());
    }

    ASSERT_TRUE (port
                   .send (bytes ("hello-source"), {protocol::encode_route_mesh_admission (
                                                    protocol::command::admit, descriptor)})
                   .result ()
                   .value ());
    ASSERT_TRUE (receive ());
    EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::update, latest),
               received->parts.front ().to_bytes ());
    source.close ();
}

INSTANTIATE_TEST_SUITE_P (WithAndWithoutRemoteHello,
                          CppFrameworkHelloExchange,
                          testing::Values (remote_hello_t::none,
                                           remote_hello_t::before_update,
                                           remote_hello_t::after_update));

TEST (CppFrameworkMeshSelectedRoute, AdmissionRevisionIntegrity)
{
    auto baseline = options ("guard-peer", 201).descriptor;
    baseline.descriptor_revision = 23;
    auto higher = baseline;
    ++higher.descriptor_revision;
    higher.state = mesh::service_node_state_t::serving;
    auto equal_mutated = higher;
    equal_mutated.descriptor_revision = baseline.descriptor_revision;
    auto lower = baseline;
    --lower.descriptor_revision;
    auto immutable = higher;
    immutable.advertised_endpoint = "tcp://127.0.0.1:7071";
    for (const auto &[command, incoming, expected] :
         std::vector<std::tuple<protocol::command, mesh::service_node_descriptor_t,
                                mesh::peer_admission_result_t>>{
           {protocol::command::hello, higher, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::admit, higher, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::update, higher, mesh::peer_admission_result_t::admitted},
           {protocol::command::hello, baseline,
            mesh::peer_admission_result_t::duplicate_connection},
           {protocol::command::hello, equal_mutated,
            mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::hello, lower, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::hello, immutable,
            mesh::peer_admission_result_t::stale_descriptor}}) {
        SCOPED_TRACE (static_cast<int> (command));
        mesh::service_topology_registry_t topology (options ("guard-local", 101).descriptor);
        ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (baseline, {1}));
        EXPECT_EQ (expected,
                   topology.admit (incoming, {1},
                                   mesh::service_liveness_registry_t::clock_t::now (), command));
    }
}

TEST (CppFrameworkMeshSelectedRoute, UpdateRequiresAdmittedLifecycle)
{
    const auto baseline = options ("guard-peer", 201).descriptor;
    auto incoming = baseline;
    incoming.lifecycle_generation = 202;
    ++incoming.descriptor_revision;
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    for (const bool has_current : {false, true}) {
        for (const auto command :
             {protocol::command::hello, protocol::command::admit, protocol::command::update}) {
            SCOPED_TRACE (has_current);
            SCOPED_TRACE (static_cast<int> (command));
            mesh::service_topology_registry_t topology (options ("guard-local", 101).descriptor);
            if (has_current)
                ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (baseline, {1}));
            EXPECT_EQ (command == protocol::command::update
                         ? mesh::peer_admission_result_t::stale_descriptor
                         : mesh::peer_admission_result_t::admitted,
                       topology.admit (incoming, {2}, incoming, now, command));
            const auto peer = topology.peer (incoming.node_routing_id);
            if (command == protocol::command::update) {
                EXPECT_EQ (has_current, peer != nullptr);
                if (has_current && peer) {
                    EXPECT_EQ (baseline, peer->descriptor);
                    EXPECT_EQ (std::vector<std::uint8_t> ({1}), peer->connection_id);
                }
            } else {
                ASSERT_TRUE (peer);
                EXPECT_EQ (incoming, peer->descriptor);
            }
        }
    }
}

} // namespace

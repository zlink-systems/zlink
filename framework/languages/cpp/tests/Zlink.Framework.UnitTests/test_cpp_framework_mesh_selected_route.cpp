/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// RouteMesh admission follows the Core selected route (#1087 FW01):
// Core ROUTER §10.1 owns which pipe carries a RID; the framework observes the
// selected-route snapshot and admits only that route
// (server 05-transport-liveness §5). A replaced route loses its admission and
// the new route admits through a new handshake, even when Core keeps the old
// pipe as a standby and never reports it disconnected.

#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace mesh = zlink::framework::runtime::mesh;

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

mesh::raw_mesh_node_options_t options (const std::string &rid, std::uint64_t lifecycle_generation)
{
    mesh::raw_mesh_node_options_t result;
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

// One host ingress pass per owner: the route observation step and one pump.
// Before #1087 the observation step was the monitor drain; the fixture
// accepts either so the same scenario shows the behavior on both sides.
template <typename Owner> void progress (Owner &owner)
{
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    if constexpr (requires { owner.observe_routes (); })
        (void) owner.observe_routes ();
    else
        (void) std::move (owner.drain_monitor_events (now)).result ();
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

} // namespace

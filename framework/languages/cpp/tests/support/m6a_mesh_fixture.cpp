/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_mesh_fixture.hpp"
#include <cassert>
#include <thread>
#include <utility>

namespace zlink::framework::test::m6a
{
using namespace std::chrono_literals;

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

mesh::service_node_descriptor_t descriptor (std::string rid, std::string endpoint)
{
    return mesh::service_node_descriptor_t{"m6a-mesh",
                                           bytes (std::move (rid)),
                                           1,
                                           1,
                                           std::move (endpoint),
                                           {{"alpha", 100}, {"beta", 50}},
                                           mesh::service_node_state_t::preparing};
}

bool peer_ready (mesh::raw_mesh_node_owner_t &node, const std::vector<std::uint8_t> &routing_id)
{
    const auto peer = node.topology ().peer (routing_id);
    return peer && peer->ready;
}

void admit_pair (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target,
                 const mesh::service_node_descriptor_t &target_descriptor)
{
    source.expect_peer (target_descriptor);
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    const auto ready = [&] {
        return peer_ready (source, target_descriptor.node_routing_id)
               && peer_ready (target, source.topology ().local_descriptor ().node_routing_id);
    };
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        std::this_thread::yield ();
    }
    assert (ready ());
}
}

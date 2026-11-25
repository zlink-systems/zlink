/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6b_mesh_fixture.hpp"
#include <utility>

namespace zlink::framework::test::m6b
{
bool ready_peer (const mesh::service_topology_registry_t &topology,
                 const std::vector<std::uint8_t> &routing_id)
{
    const auto peer = topology.peer (routing_id);
    return peer && peer->ready;
}

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

mesh::service_node_descriptor_t descriptor (std::string rid)
{
    return {"m6b-mesh",
            bytes (std::move (rid)),
            1,
            1,
            "tcp://127.0.0.1:0",
            {},
            mesh::service_node_state_t::preparing};
}
}

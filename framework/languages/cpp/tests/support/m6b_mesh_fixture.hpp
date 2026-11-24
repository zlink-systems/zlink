/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace zlink::framework::test::m6b
{
namespace mesh = zlink::framework::runtime::mesh;

bool ready_peer (const mesh::service_topology_registry_t &topology,
                 const std::vector<std::uint8_t> &routing_id);
std::vector<std::uint8_t> bytes (std::string value);
mesh::service_node_descriptor_t descriptor (std::string rid);
}

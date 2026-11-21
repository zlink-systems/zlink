/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = runtime::mesh;

template <typename T> T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (task_t<void> task);
std::vector<std::uint8_t> bytes (std::string value);
mesh::service_node_descriptor_t descriptor (std::string rid,
                                            std::string endpoint = "tcp://127.0.0.1:0");
bool peer_ready (mesh::raw_mesh_node_owner_t &node, const std::vector<std::uint8_t> &routing_id);
void admit_pair (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target,
                 const mesh::service_node_descriptor_t &target_descriptor);
}

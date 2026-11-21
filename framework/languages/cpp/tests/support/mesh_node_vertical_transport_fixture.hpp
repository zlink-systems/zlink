/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <memory>
#include <string>

namespace zlink::framework::detail
{
struct mesh_node_builder_state_t;
}

namespace framework_mesh_node_vertical_fixture
{
std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_node (std::string endpoint, std::string routing_id);

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_named_node (std::string mesh_name, std::string routing_id);
} // namespace framework_mesh_node_vertical_fixture

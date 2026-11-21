/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "m6a_mesh_fixture.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <future>
#include <string_view>

namespace zlink::framework::test::m6a
{
namespace protocol = runtime::protocol;
namespace foundation = runtime::foundation;

protocol::bound_session_bind_t
bound_session_bind_request (const mesh::service_node_descriptor_t &target, std::string actor_id);

void complete_bound_session_bind (
  mesh::raw_mesh_node_owner_t &source,
  mesh::raw_mesh_node_owner_t &target,
  std::future<std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>> &completion,
  std::string_view expected_actor);
}

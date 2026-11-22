/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "m6a_mesh_fixture.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

namespace zlink::framework::test::m6a
{
namespace protocol = runtime::protocol;

protocol::actor_join_request_t
actor_join_request (const mesh::service_node_descriptor_t &source_descriptor,
                    const mesh::service_node_descriptor_t &target_descriptor,
                    std::uint64_t correlation,
                    std::string actor_id,
                    std::string spot_id,
                    bool entry = false);
}

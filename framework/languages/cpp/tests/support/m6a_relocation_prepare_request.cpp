/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_relocation_prepare_request.hpp"

namespace zlink::framework::test::m6a
{
protocol::relocation_prepare_t
relocation_prepare_request (const mesh::service_node_descriptor_t &source_descriptor,
                            const mesh::service_node_descriptor_t &target_descriptor)
{
    return protocol::relocation_prepare_t{
      protocol::relocation_id_t{555, 777},
      3,
      protocol::relocation_coordinator_fence_t{"coord-owner", 1, bytes ("coord-node"), 1,
                                               "store-v1"},
      protocol::relocation_target_fence_t{target_descriptor.node_routing_id,
                                          target_descriptor.lifecycle_generation, "target-owner",
                                          9},
      protocol::relocation_role_t::source,
      protocol::relocation_object_t{protocol::relocation_object_kind_t::actor, "player", "actor-1",
                                    4, 6},
      source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation,
      1024,
      1,
      0xdeadbeefu,
      1,
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())};
}
}

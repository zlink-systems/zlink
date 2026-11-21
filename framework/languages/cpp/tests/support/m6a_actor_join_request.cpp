/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_actor_join_request.hpp"
#include <utility>

namespace zlink::framework::test::m6a
{
protocol::actor_join_request_t
actor_join_request (const mesh::service_node_descriptor_t &source_descriptor,
                    const mesh::service_node_descriptor_t &target_descriptor,
                    std::uint64_t correlation,
                    std::string actor_id,
                    std::string spot_id,
                    bool entry)
{
    return protocol::actor_join_request_t{
      correlation,
      protocol::actor_route_fence_t{std::move (actor_id), 4, source_descriptor.node_routing_id,
                                    source_descriptor.lifecycle_generation, 11, 12},
      entry,
      protocol::spot_route_fence_t{std::move (spot_id), 9, target_descriptor.node_routing_id,
                                   target_descriptor.lifecycle_generation, 21, 22}};
}
}

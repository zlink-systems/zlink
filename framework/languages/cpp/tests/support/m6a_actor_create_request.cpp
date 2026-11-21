/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_actor_create_request.hpp"

namespace zlink::framework::test::m6a
{
using namespace std::chrono_literals;

protocol::actor_create_header_t actor_create_request (const mesh::service_node_descriptor_t &source,
                                                      const mesh::service_node_descriptor_t &target,
                                                      std::string actor_id)
{
    return protocol::actor_create_header_t{
      0,
      {source.lifecycle_generation, 1},
      source.node_routing_id,
      source.lifecycle_generation,
      std::move (actor_id),
      "player",
      {"startup-reservation", "startup-store", 1, 1, target.node_routing_id,
       target.lifecycle_generation, "startup-owner", 1, 1},
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 2s)
                                    .count ())};
}
}

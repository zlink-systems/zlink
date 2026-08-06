/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/mesh_node_runtime.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <memory>
#include <vector>

namespace zlink::framework::runtime
{

class actor_location_observer_t;
class live_location_reader_t;

std::shared_ptr<actor_client_t>
make_actor_client (live_location_reader_t &store,
                   serializer_registry_t &serializers,
                   std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes,
                   std::shared_ptr<actor_location_observer_t> actor_locations,
                   location_options_t options,
                   route_mesh_runtime_t *route_runtime = nullptr);

} // namespace zlink::framework::runtime

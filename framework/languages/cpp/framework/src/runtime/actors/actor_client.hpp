/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/locations/options.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <memory>
#include <string>
#include <vector>

namespace zlink::framework::detail
{
class mesh_node_runtime_t;
}

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

// Internal transport fixture used by relocation process tests. It preserves
// the supplied ActorRef route while still applying the current authority
// fence. Public Actor calls continue to resolve by Actor ID.
task_t<void> send_to_actor_ref (actor_client_t &client,
                                actor_ref_t actor,
                                std::string packet_name,
                                message_t message);

task_t<message_t> request_to_actor_ref (
  actor_client_t &client,
  actor_ref_t actor,
  std::string packet_name,
  message_t message,
  std::chrono::milliseconds timeout);

} // namespace zlink::framework::runtime

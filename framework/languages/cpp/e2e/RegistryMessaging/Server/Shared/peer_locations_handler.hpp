/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/registry_messaging_contracts.hpp"

#include <zlink/framework.hpp>

namespace zlink::framework::e2e::registry_messaging
{

class peer_locations_handler_t
{
  public:
    using dependency_types = dependency_list_t<location_runtime_query_t>;

    explicit peer_locations_handler_t (location_runtime_query_t &locations) :
        _locations (locations)
    {
    }

    http_response_t handle (const http_request_t &request)
    {
        auto mesh_name = std::string (api_channel);
        const auto requested_mesh = request.query_values.find ("mesh");
        if (requested_mesh != request.query_values.end () && !requested_mesh->second.empty ()) {
            mesh_name = requested_mesh->second;
        }
        const auto peers =
          _locations
            .list_topology (
              location_topology_filter_t{.mesh_name = mesh_name},
              location_page_request_t{.page_size = 1000})
            .result ()
            .value ();
        auto payload = nlohmann::json::array ();
        for (const auto &peer : peers.items) {
            payload.push_back (nlohmann::json{
              {"mesh_name", peer.mesh_name},
              {"role", "router"},
              {"node_rid", peer.node_rid.to_string ()},
              {"endpoint", peer.endpoint},
              {"ready", peer.state == location_topology_state_t::ready}});
        }
        http_response_t response;
        response.body = payload.dump ();
        return response;
    }

  private:
    location_runtime_query_t &_locations;
};

} // namespace zlink::framework::e2e::registry_messaging

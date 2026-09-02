/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/Endpoints/evidence_endpoint.hpp"
#include "../../Shared/Handlers/channel_control_ping_handler.hpp"

#include <zlink/framework.hpp>

namespace
{

class spot_locations_handler_t
{
  public:

    explicit spot_locations_handler_t (zlink::framework::location_runtime_query_t &locations) :
        _locations (locations)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        try {
            const auto page =
              _locations
                .list_topology (
                  zlink::framework::location_topology_filter_t{
                    .mesh_name = e2e::spot_mesh},
                  zlink::framework::location_page_request_t{.page_size = 100})
                .result ()
                .value ();
            auto rows = nlohmann::json::array ();
            for (const auto &row : page.items) {
                rows.push_back (nlohmann::json{
                  {"mesh_name", row.mesh_name},
                  {"node_rid", row.node_rid.to_string ()},
                  {"endpoint", row.endpoint},
                  {"draining", row.draining},
                  {"state", static_cast<int> (row.state)},
                  {"ready", row.state == zlink::framework::location_topology_state_t::ready}});
            }
            response.body = rows.dump ();
        }
        catch (const std::exception &error) {
            response.status = 503;
            response.body = nlohmann::json{{"error", error.what ()}}.dump ();
        }
        return response;
    }

  private:
    zlink::framework::location_runtime_query_t &_locations;
};

} // namespace

inline void map_operational_endpoints (zlink::framework::http_options_builder_t &http)
{
    http.map_health ("/health")
      .map_get<spot_locations_handler_t> ("/locations/spots")
      .map_get<evidence_handler_t> ("/evidence")
      .map_post<evidence_wait_handler_t> ("/evidence/wait")
      .map_post<channel_control_ping_route_handler_t> ("/channel/control-ping")
      .map_post<shutdown_handler_t> ("/shutdown")
      .map_post<crash_handler_t> ("/crash");
}

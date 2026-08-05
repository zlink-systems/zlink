/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "api_server_framework.hpp"

#include "../Configuration/sample_configuration.hpp"
#include "../host_support.hpp"

namespace zlink::samples::bingo
{

using namespace framework;

class api_server_host_factory_t
{
  public:
    static app_t build (const sample_topology_t &topology, bool auto_stop = true);
    static app_t &configure (app_t &app, const sample_topology_t &topology, bool auto_stop = true);
};

inline app_t api_server_host_factory_t::build (const sample_topology_t &topology, bool auto_stop)
{
    auto app = app_t::create ();
    configure (app, topology, auto_stop);
    return app;
}

inline app_t &
api_server_host_factory_t::configure (app_t &app, const sample_topology_t &topology, bool auto_stop)
{
    app.logging ().use_console ().set_level ("info");
    if (auto_stop) {
        app.add_hosted_service (std::make_unique<stop_after_start_service_t> (app));
    }
    add_bingo_api_server (app, topology);
    if (!auto_stop) {
        app.add_hosted_service (std::make_unique<route_mesh_readiness_service_t> (
          "api-" + topology.api_node,
          sample_names_t::matchmaking_mesh,
          "matchmaking"));
        app.add_hosted_service (std::make_unique<route_mesh_readiness_service_t> (
          "api-" + topology.api_node,
          sample_names_t::room_spot_mesh,
          "room"));
    }
    return app;
}

} // namespace zlink::samples::bingo

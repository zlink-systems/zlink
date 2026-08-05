/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>

namespace e2e = zlink::framework::e2e::spot_service;

class channel_control_ping_route_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t>;

    explicit channel_control_ping_route_handler_t (zlink::framework::route_client_t &routes) :
        _routes (routes)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::channel_control_ping_req_t> ();
        const auto mesh_name =
          request.mesh_name.empty () ? std::string (e2e::route_channel) : request.mesh_name;
        if (mesh_name != e2e::route_channel && mesh_name != e2e::spot_mesh) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::protocol_error,
              "control ping mesh is not configured");
        }
        auto reply =
          _routes
            .request_to_node (mesh_name, zlink::routing_id_t::from (request.target_node_rid),
                      e2e::channel_echo_req_t{request.value})
            .timeout (std::chrono::milliseconds (3000))
            .submit<e2e::channel_echo_res_t> ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "ChannelEchoReq failed");
        }

        zlink::framework::http_response_t response;
        response.body = nlohmann::json (e2e::channel_control_ping_res_t{
                                         .node_rid = reply.value ().handled_by,
                                         .value = request.value})
                          .dump ();
        return response;
    }

  private:
    zlink::framework::route_client_t &_routes;
};

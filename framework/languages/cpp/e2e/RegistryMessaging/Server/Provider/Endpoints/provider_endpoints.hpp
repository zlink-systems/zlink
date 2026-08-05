/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"
#include "../../Shared/public_error_type.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::registry_messaging::provider
{

inline profile_res_t request_profile (zlink::framework::channel_client_t &channels,
                                      const std::string &channel_name,
                                      const profile_req_t &request)
{
    auto reply = channels.request (channel_name, request)
                   .timeout (std::chrono::seconds (5))
                   .submit<profile_res_t> ()
                   .result ();
    if (!reply) {
        if (reply.error ()) {
            throw *reply.error ();
        }
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure,
          "profile request failed");
    }
    return reply.value ();
}

inline scenario_route_res_t request_route (zlink::framework::route_client_t &routes,
                                           const zlink::routing_id_t &target,
                                           const scenario_route_req_t &request)
{
    auto reply = routes.request_to_node (route_channel, target, request)
                   .timeout (std::chrono::seconds (5))
                   .submit<scenario_route_res_t> ()
                   .result ();
    if (!reply) {
        if (reply.error ()) {
            throw *reply.error ();
        }
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure,
          "route request failed");
    }
    return reply.value ();
}

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;

    explicit evidence_handler_t (scenario_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_state.snapshot ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
};

class http_profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit http_profile_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        return request_profile (_channels, api_channel, request);
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class http_profile_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_msg_t;
    using reply_type = operation_status_t;

    explicit http_profile_command_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    operation_status_t handle (const profile_msg_t &command)
    {
        _channels.send (api_channel, command).submit ();
        return {.status = "sent"};
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class http_route_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::route_client_t>;
    using request_type = scenario_route_req_t;
    using reply_type = scenario_route_res_t;

    explicit http_route_request_handler_t (zlink::framework::route_client_t &routes) :
        _routes (routes)
    {
    }

    scenario_route_res_t handle (const scenario_route_req_t &request)
    {
        return request_route (_routes, zlink::routing_id_t::from (std::string ("api-b")), request);
    }

  private:
    zlink::framework::route_client_t &_routes;
};

class http_route_missing_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::route_client_t>;
    using request_type = scenario_route_req_t;
    using reply_type = request_failure_res_t;

    explicit http_route_missing_handler_t (zlink::framework::route_client_t &routes) :
        _routes (routes)
    {
    }

    request_failure_res_t handle (const scenario_route_req_t &request)
    {
        auto call = _routes.request_to_node (route_channel,
                                     zlink::routing_id_t::from (std::string ("missing-rid")),
                                     request)
                      .timeout (std::chrono::milliseconds (300))
                      .submit<scenario_route_res_t> ();
        return {.failed = !call.result ().has_value (),
                .error_type = public_error_type (call.result ())};
    }

  private:
    zlink::framework::route_client_t &_routes;
};

class server_weight_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<
      zlink::framework::channel_runtime_options_t>;

    explicit server_weight_handler_t (zlink::framework::channel_runtime_options_t &options) :
        _options (options)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto found = request.query_values.find ("weight");
        if (found == request.query_values.end ()) {
            zlink::framework::http_response_t response;
            response.status = 400;
            response.body = R"({"error":"weight is required"})";
            return response;
        }
        const auto weight = static_cast<std::uint32_t> (std::stoul (found->second));
        _options.client_server_channel (api_channel)
          .configure_server_socket ()
          .peer_weight (zlink::peer_weight_t::value (weight));
        zlink::framework::http_response_t response;
        response.body = nlohmann::json{{"weight", weight}}.dump ();
        return response;
    }

  private:
    zlink::framework::channel_runtime_options_t &_options;
};

} // namespace zlink::framework::e2e::registry_messaging::provider

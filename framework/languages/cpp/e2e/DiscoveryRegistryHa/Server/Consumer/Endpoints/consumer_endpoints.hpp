/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/store_failure_contracts.hpp"
#include "../../Shared/runtime_status_projection.hpp"
#include "../Infrastructure/socket_evidence_store.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <csignal>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace zlink::framework::e2e::store_failure::consumer
{

class profile_request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit profile_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        auto call = _channels.request (api_channel, request)
                      .timeout (std::chrono::milliseconds (5000))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        if (!reply) {
            throw std::runtime_error (
              reply.error () ? reply.error ()->what () : "profile request failed");
        }
        return reply.value ();
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class profile_request_timeout_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit profile_request_timeout_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body);
        const auto profile = body.get<profile_req_t> ();
        int timeout_ms = 3000;
        if (auto found = request.route_values.find ("milliseconds");
            found != request.route_values.end ()) {
            timeout_ms = std::stoi (found->second);
        }
        auto call = _channels.request (api_channel, profile)
                      .timeout (std::chrono::milliseconds (timeout_ms))
                      .submit<profile_res_t> ();
        const auto &reply = call.result ();
        zlink::framework::http_response_t response;
        if (!reply) {
            response.status = 408;
            response.body = R"({"status":"timeout"})";
            return response;
        }
        response.body = nlohmann::json (reply.value ()).dump ();
        return response;
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class query_status_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::location_runtime_query_t>;

    explicit query_status_handler_t (zlink::framework::location_runtime_query_t &query) :
        _query (query)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        const auto status = _query.get_status ().result ().value ();
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (server::project_runtime_status (status)).dump ();
        return response;
    }

  private:
    zlink::framework::location_runtime_query_t &_query;
};

class query_peers_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::location_runtime_query_t>;

    explicit query_peers_handler_t (zlink::framework::location_runtime_query_t &query) :
        _query (query)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        try {
            auto peers = _query.list_topology ({}).result ().value ();
            std::vector<peer_row_res_t> rows;
            rows.reserve (peers.items.size ());
            for (const auto &peer : peers.items) {
                rows.push_back (
                  peer_row_res_t{.rid = peer.node_rid.to_string (),
                                 .endpoint = peer.endpoint,
                                 .owner_id = {},
                                 .draining = peer.draining});
            }
            response.body = nlohmann::json (rows).dump ();
        }
        catch (const std::exception &error) {
            response.status = 503;
            response.body = nlohmann::json{{"error", error.what ()}}.dump ();
        }
        return response;
    }

  private:
    zlink::framework::location_runtime_query_t &_query;
};

class query_connections_handler_t
{
  public:
    using dependency_types = dependency_list_t<socket_evidence_store_t>;

    explicit query_connections_handler_t (socket_evidence_store_t &evidence) :
        _evidence (evidence)
    {
    }

    http_response_t handle (const http_request_t &)
    {
        http_response_t response;
        response.body = nlohmann::json (_evidence.snapshot ()).dump ();
        return response;
    }

  private:
    socket_evidence_store_t &_evidence;
};

class shutdown_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
            std::raise (SIGTERM);
        }).detach ();

        zlink::framework::http_response_t response;
        response.body = R"({"status":"stopping"})";
        return response;
    }
};

} // namespace zlink::framework::e2e::store_failure::consumer

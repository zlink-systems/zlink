/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/registry_messaging_contracts.hpp"

#include <zlink/framework.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace zlink::framework::e2e::registry_messaging
{

inline const char *client_server_state_name (
  client_server_server_state_t state)
{
    switch (state) {
        case client_server_server_state_t::configured: return "configured";
        case client_server_server_state_t::connecting: return "connecting";
        case client_server_server_state_t::ready: return "ready";
        case client_server_server_state_t::draining: return "draining";
        case client_server_server_state_t::disconnected: return "disconnected";
        case client_server_server_state_t::rejected: return "rejected";
    }
    return "rejected";
}

class client_server_status_handler_t
{
  public:
    using dependency_types = dependency_list_t<client_server_runtime_t>;

    explicit client_server_status_handler_t (client_server_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    http_response_t handle (const http_request_t &request)
    {
        auto channel = std::string (api_channel);
        const auto requested = request.query_values.find ("channel");
        if (requested != request.query_values.end () && !requested->second.empty ())
            channel = requested->second;

        const auto snapshot = _runtime.snapshot (channel);
        auto servers = nlohmann::json::array ();
        for (const auto &server : snapshot.servers) {
            servers.push_back (nlohmann::json{
              {"serverRid", server.server_rid.to_string ()},
              {"lifecycleGeneration", server.lifecycle_generation},
              {"weight", server.weight},
              {"ready", server.ready},
              {"state", client_server_state_name (server.state)},
              {"descriptorSource", server.descriptor_source}});
        }
        http_response_t response;
        response.body = nlohmann::json{
          {"channelName", snapshot.channel_name},
          {"selectable", snapshot.selectable},
          {"readyServerCount", snapshot.ready_server_count},
          {"connectionIntentCount", snapshot.connection_intent_count},
          {"pendingRequestCount", snapshot.pending_request_count},
          {"sequence", snapshot.sequence},
          {"servers", std::move (servers)}}.dump ();
        return response;
    }

  private:
    client_server_runtime_t &_runtime;
};

} // namespace zlink::framework::e2e::registry_messaging

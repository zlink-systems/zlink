/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/sample_names.hpp"

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace zlink::samples::deliverydispatch
{

using namespace framework;

class route_ready_handler_t
{
  public:
    using dependency_types = dependency_list_t<route_mesh_runtime_t>;

    explicit route_ready_handler_t (route_mesh_runtime_t &runtime) : _runtime (runtime) {}

    http_response_t handle (const http_request_t &request)
    {
        const auto found = request.query_values.find ("targetRid");
        if (found == request.query_values.end () || found->second.empty ())
            return {.status = 400,
                    .body = R"({"error":"targetRid is required"})"};

        const auto snapshot = _runtime.snapshot (sample_names_t::courier_actor_discovery);
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == found->second
                && peer.state == peer_state_t::ready)
                return {.body = nlohmann::json{{"ready", true},
                                               {"targetRid", found->second}}
                                  .dump ()};
        }

        nlohmann::json peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers)
            peers.push_back ({{"rid", peer.node_rid.to_string ()},
                              {"state", static_cast<int> (peer.state)}});
        return {.status = 503,
                .body = nlohmann::json{{"ready", false},
                                       {"targetRid", found->second},
                                       {"peers", std::move (peers)}}
                          .dump ()};
    }

  private:
    route_mesh_runtime_t &_runtime;
};

} // namespace zlink::samples::deliverydispatch

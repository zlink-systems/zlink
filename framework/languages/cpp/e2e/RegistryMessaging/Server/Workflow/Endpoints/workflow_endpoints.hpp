/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::registry_messaging::workflow
{

inline workflow_res_t request_workflow (zlink::framework::channel_client_t &channels,
                                        const workflow_req_t &request)
{
    auto reply = channels.request (workflow_channel, request)
                   .timeout (std::chrono::seconds (5))
                   .submit<workflow_res_t> ()
                   .result ();
    if (!reply) {
        if (reply.error ()) {
            throw *reply.error ();
        }
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure,
          "workflow request failed");
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

class http_workflow_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = workflow_req_t;
    using reply_type = workflow_res_t;

    explicit http_workflow_request_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    workflow_res_t handle (const workflow_req_t &request)
    {
        return request_workflow (_channels, request);
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

} // namespace zlink::framework::e2e::registry_messaging::workflow

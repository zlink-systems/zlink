/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"

#include <zlink/framework.hpp>

namespace zlink::framework::e2e::registry_messaging::workflow
{

class workflow_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = workflow_req_t;
    using reply_type = workflow_res_t;

    explicit workflow_request_handler_t (scenario_state_t &state) : _state (state) {}

    workflow_res_t handle (const workflow_req_t &request)
    {
        _state.record ("WorkflowReq", request.value);
        return {.value = "workflow:" + request.value, .provider_rid = _state.provider_rid};
    }

  private:
    scenario_state_t &_state;
};

} // namespace zlink::framework::e2e::registry_messaging::workflow

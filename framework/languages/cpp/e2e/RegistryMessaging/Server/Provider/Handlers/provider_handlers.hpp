/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <thread>

namespace zlink::framework::e2e::registry_messaging::provider
{

class profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    explicit profile_request_handler_t (scenario_state_t &state) : _state (state) {}

    profile_res_t handle (const profile_req_t &request)
    {
        if (request.value == "slow") {
            std::this_thread::sleep_for (std::chrono::seconds (1));
        } else if (request.value == "very-slow") {
            std::this_thread::sleep_for (std::chrono::seconds (10));
        }
        _state.record ("ProfileReq", request.value);
        return {.value = "profile:" + request.value,
                .provider_rid = _state.provider_rid,
                .instance_id = _state.instance_id};
    }

  private:
    scenario_state_t &_state;
};

class profile_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using message_type = profile_msg_t;

    explicit profile_command_handler_t (scenario_state_t &state) : _state (state) {}

    void handle (const profile_msg_t &command)
    {
        if (command.command_id.rfind ("rm-c9-slow-", 0) == 0) {
            std::this_thread::sleep_for (std::chrono::seconds (1));
        }
        _state.record ("ProfileMsg", command.command_id);
    }

  private:
    scenario_state_t &_state;
};

class payload_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = payload_req_t;
    using reply_type = payload_res_t;

    explicit payload_request_handler_t (scenario_state_t &state) : _state (state) {}

    payload_res_t handle (const payload_req_t &request)
    {
        const auto hash = sha256_hex (request.payload);
        _state.record ("PayloadReq",
                       request.marker + ":length=" + std::to_string (request.payload.size ())
                         + ":sha256=" + hash);
        return {.marker = request.marker,
                .length = static_cast<int> (request.payload.size ()),
                .sha256 = hash};
    }

  private:
    scenario_state_t &_state;
};

class route_ping_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = scenario_route_req_t;
    using reply_type = scenario_route_res_t;

    explicit route_ping_handler_t (scenario_state_t &state) : _state (state) {}

    scenario_route_res_t handle (const scenario_route_req_t &request,
                                 const zlink::framework::route_message_context_t &context)
    {
        _state.record ("ScenarioRouteReq", request.value);
        return {.value = "route:" + request.value,
                .target_rid = _state.provider_rid,
                .source_rid = context.source_node_rid.to_string ()};
    }

  private:
    scenario_state_t &_state;
};

} // namespace zlink::framework::e2e::registry_messaging::provider

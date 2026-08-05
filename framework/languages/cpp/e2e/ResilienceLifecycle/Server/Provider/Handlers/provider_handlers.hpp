/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/evidence_store.hpp"
#include "../Infrastructure/fault_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

inline const std::string &profile_marker_or_value (const profile_req_t &request)
{
    return request.marker.empty () ? request.value : request.marker;
}

inline const std::string &profile_marker_or_command (const profile_msg_t &command)
{
    return command.marker.empty () ? command.command_id : command.marker;
}

class profile_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t, fault_state_t>;
    using request_type = profile_req_t;
    using reply_type = profile_res_t;

    profile_request_handler_t (evidence_store_t &evidence, fault_state_t &fault) :
        _evidence (evidence), _fault (fault)
    {
    }

    profile_res_t handle (const profile_req_t &request)
    {
        const auto &marker = profile_marker_or_value (request);
        if (_fault.mode () == "gray" && request.value == "gray") {
            _evidence.record ("ProfileFault", marker);
            throw std::runtime_error ("gray failure");
        }
        if (request.value == "slow") {
            _evidence.record ("ProfileStart", marker);
            std::this_thread::sleep_for (std::chrono::seconds (1));
            _evidence.record ("ProfileReq", marker);
            return {.value = "profile:" + request.value,
                    .provider_rid = _evidence.provider_rid (),
                    .instance_id = _evidence.instance_id (),
                    .marker = marker};
        } else if (request.value == "very-slow") {
            _evidence.record ("ProfileReq", marker);
            std::this_thread::sleep_for (std::chrono::seconds (10));
            return {.value = "profile:" + request.value,
                    .provider_rid = _evidence.provider_rid (),
                    .instance_id = _evidence.instance_id (),
                    .marker = marker};
        }
        _evidence.record ("ProfileReq", marker);
        return {.value = "profile:" + request.value,
                .provider_rid = _evidence.provider_rid (),
                .instance_id = _evidence.instance_id (),
                .marker = marker};
    }

  private:
    evidence_store_t &_evidence;
    fault_state_t &_fault;
};

class profile_command_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using message_type = profile_msg_t;

    explicit profile_command_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    void handle (const profile_msg_t &command)
    {
        _evidence.record ("ProfileMsg", profile_marker_or_command (command));
    }

  private:
    evidence_store_t &_evidence;
};

class payload_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using request_type = payload_req_t;
    using reply_type = payload_res_t;

    explicit payload_request_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    payload_res_t handle (const payload_req_t &request)
    {
        const auto hash = sha256_hex (request.payload);
        _evidence.record ("PayloadReq",
                          request.marker + ":length=" + std::to_string (request.payload.size ())
                            + ":sha256=" + hash);
        return {.marker = request.marker,
                .length = static_cast<int> (request.payload.size ()),
                .sha256 = hash};
    }

  private:
    evidence_store_t &_evidence;
};

class route_ping_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using request_type = scenario_route_req_t;
    using reply_type = scenario_route_res_t;

    explicit route_ping_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    scenario_route_res_t handle (const scenario_route_req_t &request,
                                 const zlink::framework::route_message_context_t &context)
    {
        _evidence.record ("ScenarioRouteReq", request.value);
        return {.value = "route:" + request.value,
                .target_rid = _evidence.provider_rid (),
                .source_rid = context.source_node_rid.to_string ()};
    }

  private:
    evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::resilience_lifecycle::provider

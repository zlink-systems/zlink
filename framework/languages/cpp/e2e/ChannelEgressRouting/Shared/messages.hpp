/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::e2e::channel_egress
{

inline constexpr std::string_view game_mesh = "channel-egress-game";
inline constexpr std::string_view audit_mesh = "channel-egress-audit";
inline constexpr std::string_view session_channel = "game.session";
inline constexpr std::string_view play_channel = "game.play";
inline constexpr std::string_view api_channel = "game.api";
inline constexpr std::string_view audit_channel = "audit.record";
inline constexpr std::string_view workflow_channel = "workflow.command";
inline constexpr std::string_view spot_type = "channel-egress.workflow-spot";

struct channel_probe_req_t
{
    static constexpr const char *packet_name = "ChannelProbeReq";
    std::string id;
    std::string mode = "echo";
};

struct channel_probe_msg_t
{
    static constexpr const char *packet_name = "ChannelProbeMsg";
    std::string id;
};

struct channel_probe_res_t
{
    std::string id;
    std::string role;
    std::string lifecycle;
    std::string channel;
    std::vector<std::string> downstream;
};

struct spot_workflow_req_t
{
    static constexpr const char *packet_name = "SpotWorkflowReq";
    std::string id;
    std::string timer_name;
};

struct spot_workflow_res_t
{
    std::string id;
    std::vector<std::string> sequence;
};

inline void to_json (nlohmann::json &json, const channel_probe_req_t &value)
{
    json = { {"id", value.id}, {"mode", value.mode} };
}

inline void from_json (const nlohmann::json &json, channel_probe_req_t &value)
{
    json.at ("id").get_to (value.id);
    value.mode = json.value ("mode", "echo");
}

inline void to_json (nlohmann::json &json, const channel_probe_msg_t &value)
{
    json = { {"id", value.id} };
}

inline void from_json (const nlohmann::json &json, channel_probe_msg_t &value)
{
    json.at ("id").get_to (value.id);
}

inline void to_json (nlohmann::json &json, const channel_probe_res_t &value)
{
    json = { {"id", value.id},
             {"role", value.role},
             {"lifecycle", value.lifecycle},
             {"channel", value.channel},
             {"downstream", value.downstream} };
}

inline void from_json (const nlohmann::json &json, channel_probe_res_t &value)
{
    json.at ("id").get_to (value.id);
    json.at ("role").get_to (value.role);
    value.lifecycle = json.value ("lifecycle", "");
    value.channel = json.value ("channel", "");
    value.downstream = json.value ("downstream", std::vector<std::string>{});
}

inline void to_json (nlohmann::json &json, const spot_workflow_req_t &value)
{
    json = { {"id", value.id}, {"timer_name", value.timer_name} };
}

inline void from_json (const nlohmann::json &json, spot_workflow_req_t &value)
{
    json.at ("id").get_to (value.id);
    value.timer_name = json.value ("timer_name", value.id + "-timer");
}

inline void to_json (nlohmann::json &json, const spot_workflow_res_t &value)
{
    json = { {"id", value.id}, {"sequence", value.sequence} };
}

inline void from_json (const nlohmann::json &json, spot_workflow_res_t &value)
{
    json.at ("id").get_to (value.id);
    value.sequence = json.value ("sequence", std::vector<std::string>{});
}

} // namespace zlink::framework::e2e::channel_egress

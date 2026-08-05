/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring
{

inline constexpr const char *profile_channel = "monitor.profile";
inline constexpr const char *channel_server_source = "monitor.profile.server";
inline constexpr const char *channel_client_source = "monitor.profile.client";
inline constexpr const char *spot_channel = "monitor.spot";
inline constexpr const char *spot_node = "monitor.spot";
inline constexpr const char *handler_group = "runtime-monitoring";
inline constexpr const char *route_mesh_name = "runtime.monitoring.mesh";
inline constexpr const char *route_mesh_channel = route_mesh_name;
inline constexpr const char *monitoring_subject_spot = "monitoring-subject";

struct profile_req_t
{
    std::string value;
    std::string marker;
};

struct profile_res_t
{
    std::string value;
    std::string provider_rid;
    std::string marker;
};

struct application_gate_req_t
{
    std::string marker;
};

struct application_gate_res_t
{
    std::string marker;
    std::string provider_rid;
};

struct evidence_wait_req_t
{
    std::vector<std::string> contains_all;
    std::vector<std::vector<std::string>> contains_any_groups;
    int timeout_milliseconds = 10000;
};

inline void to_json (nlohmann::json &json, const profile_req_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, profile_req_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const profile_res_t &value)
{
    json = nlohmann::json{
      {"value", value.value}, {"provider_rid", value.provider_rid}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, profile_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("provider_rid").get_to (value.provider_rid);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const application_gate_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, application_gate_req_t &value)
{
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const application_gate_res_t &value)
{
    json = nlohmann::json{
      {"marker", value.marker}, {"provider_rid", value.provider_rid}};
}

inline void from_json (const nlohmann::json &json, application_gate_res_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("provider_rid").get_to (value.provider_rid);
}

inline void to_json (nlohmann::json &json, const evidence_wait_req_t &value)
{
    json = nlohmann::json{{"contains_all", value.contains_all},
                          {"contains_any_groups", value.contains_any_groups},
                          {"timeout_milliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, evidence_wait_req_t &value)
{
    if (json.contains ("contains_all")) {
        json.at ("contains_all").get_to (value.contains_all);
    }
    if (json.contains ("contains_any_groups")) {
        json.at ("contains_any_groups").get_to (value.contains_any_groups);
    }
    if (json.contains ("timeout_milliseconds")) {
        json.at ("timeout_milliseconds").get_to (value.timeout_milliseconds);
    }
}

} // namespace zlink::framework::e2e::runtime_monitoring

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace zlink::framework::e2e::registry_messaging
{

inline constexpr const char *api_channel = "registry.messaging.api";
inline constexpr const char *workflow_channel = "registry.messaging.workflow";
inline constexpr const char *route_channel = "registry.messaging.route";
inline constexpr const char *dealer_channel = "registry.messaging.dealer";
inline constexpr const char *handler_group = "registry-messaging";

struct profile_req_t
{
    std::string value;
};

struct profile_res_t
{
    std::string value;
    std::string provider_rid;
    std::string instance_id;
};

struct profile_msg_t
{
    std::string command_id;
};

/* Negative-path wire packets: the names deliberately have no handler on
 * the provider, so senders use dedicated DTOs instead of a per-call name
 * override (CPP-G0-DISPATCH-001). */
struct missing_profile_req_t : profile_req_t
{
    static constexpr const char *packet_name = "MissingProfileReq";
};

struct missing_profile_msg_t : profile_msg_t
{
    static constexpr const char *packet_name = "MissingProfileMsg";
};

struct payload_req_t
{
    std::string marker;
    std::string payload;
};

struct payload_res_t
{
    std::string marker;
    int length = 0;
    std::string sha256;
};

struct request_failure_res_t
{
    bool failed = false;
    std::string error_type;
};

struct operation_status_t
{
    std::string status;
};

struct backpressure_send_res_t
{
    std::string outcome;
};

struct workflow_req_t
{
    std::string value;
};

struct workflow_res_t
{
    std::string value;
    std::string provider_rid;
};

struct scenario_route_req_t
{
    static constexpr const char *packet_name = "ScenarioRouteReq";
    std::string value;
};

struct scenario_route_res_t
{
    std::string value;
    std::string target_rid;
    std::string source_rid;
};

struct evidence_entry_t
{
    std::string marker;
    std::string provider_rid;
    std::string value;
};

struct evidence_snapshot_t
{
    std::string provider_rid;
    std::vector<evidence_entry_t> entries;
};

inline std::string sha256_hex (const std::string &value)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256 (reinterpret_cast<const unsigned char *> (value.data ()), value.size (), digest);
    std::ostringstream output;
    output << std::hex << std::setfill ('0');
    for (const auto byte : digest) {
        output << std::setw (2) << static_cast<int> (byte);
    }
    return output.str ();
}

inline void to_json (nlohmann::json &json, const profile_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, profile_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const profile_res_t &value)
{
    json = nlohmann::json{{"value", value.value},
                          {"provider_rid", value.provider_rid},
                          {"instance_id", value.instance_id}};
}

inline void from_json (const nlohmann::json &json, profile_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("provider_rid").get_to (value.provider_rid);
    if (json.contains ("instance_id")) {
        json.at ("instance_id").get_to (value.instance_id);
    }
}

inline void to_json (nlohmann::json &json, const profile_msg_t &value)
{
    json = nlohmann::json{{"command_id", value.command_id}};
}

inline void from_json (const nlohmann::json &json, profile_msg_t &value)
{
    json.at ("command_id").get_to (value.command_id);
}

inline void to_json (nlohmann::json &json, const missing_profile_req_t &value)
{
    to_json (json, static_cast<const profile_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, missing_profile_req_t &value)
{
    from_json (json, static_cast<profile_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const missing_profile_msg_t &value)
{
    to_json (json, static_cast<const profile_msg_t &> (value));
}

inline void from_json (const nlohmann::json &json, missing_profile_msg_t &value)
{
    from_json (json, static_cast<profile_msg_t &> (value));
}

inline void to_json (nlohmann::json &json, const payload_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}, {"payload", value.payload}};
}

inline void from_json (const nlohmann::json &json, payload_req_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("payload").get_to (value.payload);
}

inline void to_json (nlohmann::json &json, const payload_res_t &value)
{
    json = nlohmann::json{
      {"marker", value.marker}, {"length", value.length}, {"sha256", value.sha256}};
}

inline void from_json (const nlohmann::json &json, payload_res_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("length").get_to (value.length);
    json.at ("sha256").get_to (value.sha256);
}

inline void to_json (nlohmann::json &json, const request_failure_res_t &value)
{
    json = nlohmann::json{{"failed", value.failed}, {"error_type", value.error_type}};
}

inline void from_json (const nlohmann::json &json, request_failure_res_t &value)
{
    json.at ("failed").get_to (value.failed);
    json.at ("error_type").get_to (value.error_type);
}

inline void to_json (nlohmann::json &json, const operation_status_t &value)
{
    json = nlohmann::json{{"status", value.status}};
}

inline void from_json (const nlohmann::json &json, operation_status_t &value)
{
    json.at ("status").get_to (value.status);
}

inline void to_json (nlohmann::json &json, const backpressure_send_res_t &value)
{
    json = nlohmann::json{{"outcome", value.outcome}};
}

inline void from_json (const nlohmann::json &json, backpressure_send_res_t &value)
{
    json.at ("outcome").get_to (value.outcome);
}

inline void to_json (nlohmann::json &json, const workflow_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, workflow_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const workflow_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"provider_rid", value.provider_rid}};
}

inline void from_json (const nlohmann::json &json, workflow_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("provider_rid").get_to (value.provider_rid);
}

inline void to_json (nlohmann::json &json, const scenario_route_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, scenario_route_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const scenario_route_res_t &value)
{
    json = nlohmann::json{
      {"value", value.value}, {"target_rid", value.target_rid}, {"source_rid", value.source_rid}};
}

inline void from_json (const nlohmann::json &json, scenario_route_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("target_rid").get_to (value.target_rid);
    json.at ("source_rid").get_to (value.source_rid);
}

inline void to_json (nlohmann::json &json, const evidence_entry_t &value)
{
    json = nlohmann::json{
      {"marker", value.marker}, {"provider_rid", value.provider_rid}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, evidence_entry_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("provider_rid").get_to (value.provider_rid);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const evidence_snapshot_t &value)
{
    json = nlohmann::json{{"provider_rid", value.provider_rid}, {"entries", value.entries}};
}

inline void from_json (const nlohmann::json &json, evidence_snapshot_t &value)
{
    json.at ("provider_rid").get_to (value.provider_rid);
    json.at ("entries").get_to (value.entries);
}

} // namespace zlink::framework::e2e::registry_messaging

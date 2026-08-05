/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace zlink::framework::e2e::store_failure
{

inline constexpr const char *api_channel = "storefailure.profile";
inline constexpr const char *handler_group = "store-failure";

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

struct evidence_wait_req_t
{
    std::string contains;
    int timeout_milliseconds = 10000;
};

struct operation_status_t
{
    std::string status;
};

struct store_delay_req_t
{
    int milliseconds = 0;
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

struct runtime_status_res_t
{
    bool store_healthy = false;
    bool watch_enabled = false;
    bool owner_lease_healthy = false;
    std::int64_t owner_lease_renewed_at_unix_ms = 0;
    std::int64_t last_refresh_at_unix_ms = 0;
    std::string last_error;
};

struct peer_row_res_t
{
    std::string rid;
    std::string endpoint;
    std::string owner_id;
    bool draining = false;
};

struct socket_evidence_entry_t
{
    std::string kind;
    std::string remote_address;
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

inline void to_json (nlohmann::json &json, const evidence_wait_req_t &value)
{
    json = nlohmann::json{{"contains", value.contains},
                          {"timeout_milliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, evidence_wait_req_t &value)
{
    json.at ("contains").get_to (value.contains);
    if (json.contains ("timeout_milliseconds")) {
        json.at ("timeout_milliseconds").get_to (value.timeout_milliseconds);
    }
}

inline void to_json (nlohmann::json &json, const operation_status_t &value)
{
    json = nlohmann::json{{"status", value.status}};
}

inline void from_json (const nlohmann::json &json, operation_status_t &value)
{
    json.at ("status").get_to (value.status);
}

inline void to_json (nlohmann::json &json, const store_delay_req_t &value)
{
    json = nlohmann::json{{"milliseconds", value.milliseconds}};
}

inline void from_json (const nlohmann::json &json, store_delay_req_t &value)
{
    json.at ("milliseconds").get_to (value.milliseconds);
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

inline void to_json (nlohmann::json &json, const runtime_status_res_t &value)
{
    json = nlohmann::json{{"store_healthy", value.store_healthy},
                          {"watch_enabled", value.watch_enabled},
                          {"owner_lease_healthy", value.owner_lease_healthy},
                          {"owner_lease_renewed_at_unix_ms",
                           value.owner_lease_renewed_at_unix_ms},
                          {"last_refresh_at_unix_ms", value.last_refresh_at_unix_ms},
                          {"last_error", value.last_error}};
}

inline void from_json (const nlohmann::json &json, runtime_status_res_t &value)
{
    json.at ("store_healthy").get_to (value.store_healthy);
    json.at ("watch_enabled").get_to (value.watch_enabled);
    json.at ("owner_lease_healthy").get_to (value.owner_lease_healthy);
    json.at ("owner_lease_renewed_at_unix_ms")
      .get_to (value.owner_lease_renewed_at_unix_ms);
    json.at ("last_refresh_at_unix_ms").get_to (value.last_refresh_at_unix_ms);
    if (json.contains ("last_error") && !json.at ("last_error").is_null ()) {
        json.at ("last_error").get_to (value.last_error);
    }
}

inline void to_json (nlohmann::json &json, const peer_row_res_t &value)
{
    json = nlohmann::json{{"rid", value.rid},
                          {"endpoint", value.endpoint},
                          {"owner_id", value.owner_id},
                          {"draining", value.draining}};
}

inline void from_json (const nlohmann::json &json, peer_row_res_t &value)
{
    if (json.contains ("rid") && !json.at ("rid").is_null ()) {
        json.at ("rid").get_to (value.rid);
    }
    json.at ("endpoint").get_to (value.endpoint);
    json.at ("owner_id").get_to (value.owner_id);
    json.at ("draining").get_to (value.draining);
}

inline void to_json (nlohmann::json &json, const socket_evidence_entry_t &value)
{
    json = nlohmann::json{{"kind", value.kind}, {"remote_address", value.remote_address}};
}

inline void from_json (const nlohmann::json &json, socket_evidence_entry_t &value)
{
    json.at ("kind").get_to (value.kind);
    json.at ("remote_address").get_to (value.remote_address);
}

} // namespace zlink::framework::e2e::store_failure

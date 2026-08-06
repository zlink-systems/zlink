/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace zlink::framework::e2e::instance_spot
{

inline constexpr const char *mesh_name = "instance-spot.mesh";
inline constexpr const char *spot_type = "ScenarioInstanceSpot";

struct probe_request_t
{
    static constexpr const char *packet_name = "InstanceProbeReq";
    std::string spot_id;
    std::string operation_id;
    std::string action;
};

struct probe_message_t
{
    static constexpr const char *packet_name = "InstanceProbeMsg";
    std::string spot_id;
    std::string operation_id;
    std::string action;
};

struct probe_reply_t
{
    std::string spot_id;
    std::string operation_id;
    std::string action;
    bool instance_spot = true;
};

struct operation_evidence_t
{
    std::uint32_t entered = 0;
    std::uint32_t completed = 0;
    std::uint32_t instance_count = 0;
    std::string spot_id;
    std::string owner_rid;
};

struct lifecycle_snapshot_t
{
    std::uint32_t materialized_instances = 0;
    std::uint32_t closed_instances = 0;
};

inline void to_json (nlohmann::json &json, const probe_request_t &value)
{
    json = {{"spotId", value.spot_id}, {"operationId", value.operation_id},
            {"action", value.action}};
}

inline void from_json (const nlohmann::json &json, probe_request_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.operation_id = json.value ("operationId", "");
    value.action = json.value ("action", "");
}

inline void to_json (nlohmann::json &json, const probe_message_t &value)
{
    json = {{"spotId", value.spot_id}, {"operationId", value.operation_id},
            {"action", value.action}};
}

inline void from_json (const nlohmann::json &json, probe_message_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.operation_id = json.value ("operationId", "");
    value.action = json.value ("action", "");
}

inline void to_json (nlohmann::json &json, const probe_reply_t &value)
{
    json = {{"status", "completed"}, {"spotId", value.spot_id},
            {"operationId", value.operation_id}, {"action", value.action},
            {"instanceSpot", value.instance_spot}};
}

inline void from_json (const nlohmann::json &json, probe_reply_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.operation_id = json.value ("operationId", "");
    value.action = json.value ("action", "");
    value.instance_spot = json.value ("instanceSpot", false);
}

inline void to_json (nlohmann::json &json, const operation_evidence_t &value)
{
    json = {{"entered", value.entered}, {"completed", value.completed},
            {"instanceCount", value.instance_count}, {"spotId", value.spot_id},
            {"ownerRid", value.owner_rid}};
}

inline void from_json (const nlohmann::json &json, operation_evidence_t &value)
{
    value.entered = json.value ("entered", std::uint32_t {0});
    value.completed = json.value ("completed", std::uint32_t {0});
    value.instance_count = json.value ("instanceCount", std::uint32_t {0});
    value.spot_id = json.value ("spotId", "");
    value.owner_rid = json.value ("ownerRid", "");
}

inline void to_json (nlohmann::json &json, const lifecycle_snapshot_t &value)
{
    json = {{"materializedInstances", value.materialized_instances},
            {"closedInstances", value.closed_instances}};
}

inline void from_json (const nlohmann::json &json, lifecycle_snapshot_t &value)
{
    value.materialized_instances =
      json.value ("materializedInstances", std::uint32_t {0});
    value.closed_instances = json.value ("closedInstances", std::uint32_t {0});
}

} // namespace zlink::framework::e2e::instance_spot

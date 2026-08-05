/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace zlink::framework::e2e::observability_ops
{

inline constexpr const char *spot_mesh = "obs.play";
inline constexpr const char *spot_route_channel = "obs.play.route";
inline constexpr const char *room_spot = "obs.room";
inline constexpr const char *workflow_spot_mesh = "obs.workflow";
inline constexpr const char *workflow_route_channel = "obs.workflow.route";
inline constexpr const char *order_workflow_spot = "OrderWorkflowSpot";
inline constexpr const char *spot_id_metadata = "obs-spot-rid";

struct obs_action_req_t
{
    static constexpr const char *packet_name = "ObsActionReq";
    std::string spot_id;
    std::string marker;
    int value = 0;
};

inline void to_json (nlohmann::json &json, const obs_action_req_t &value)
{
    json = {{"spotId", value.spot_id}, {"marker", value.marker}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, obs_action_req_t &value)
{
    json.at ("spotId").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("value").get_to (value.value);
}

struct obs_action_res_t
{
    static constexpr const char *packet_name = "ObsActionRes";
    std::string spot_id;
    std::string marker;
    int value = 0;
};

inline void to_json (nlohmann::json &json, const obs_action_res_t &value)
{
    json = {{"spotId", value.spot_id}, {"marker", value.marker}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, obs_action_res_t &value)
{
    json.at ("spotId").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("value").get_to (value.value);
}

/* OBS-A2: intentionally has no registered handler on the room spot. */
struct obs_unknown_req_t
{
    static constexpr const char *packet_name = "ObsUnknownReq";
    std::string marker;
};

inline void to_json (nlohmann::json &json, const obs_unknown_req_t &value)
{
    json = {{"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, obs_unknown_req_t &value)
{
    json.at ("marker").get_to (value.marker);
}

struct create_room_req_t
{
    std::string spot_id;
};

inline void to_json (nlohmann::json &json, const create_room_req_t &value)
{
    json = {{"spotId", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, create_room_req_t &value)
{
    json.at ("spotId").get_to (value.spot_id);
}

struct create_room_res_t
{
    std::string spot_id;
    std::string state;
};

inline void to_json (nlohmann::json &json, const create_room_res_t &value)
{
    json = {{"spotId", value.spot_id}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, create_room_res_t &value)
{
    json.at ("spotId").get_to (value.spot_id);
    json.at ("state").get_to (value.state);
}

struct drain_req_t
{
    int deadline_ms = 30000;
};

inline void to_json (nlohmann::json &json, const drain_req_t &value)
{
    json = {{"deadlineMs", value.deadline_ms}};
}

inline void from_json (const nlohmann::json &json, drain_req_t &value)
{
    if (json.contains ("deadlineMs")) {
        json.at ("deadlineMs").get_to (value.deadline_ms);
    }
}

inline constexpr const char *actor_type = "obs-player";
inline constexpr const char *projection_topic = "obs.projection";

/* OBS-C2/C5: joins (or finds) the player actor on the serving node. */
struct join_actor_req_t
{
    std::string actor_id;
};

inline void to_json (nlohmann::json &json, const join_actor_req_t &value)
{
    json = {{"actorId", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, join_actor_req_t &value)
{
    json.at ("actorId").get_to (value.actor_id);
}

struct join_actor_res_t
{
    std::string actor_id;
    std::string node_rid;
    bool accepted = false;
    std::string error;
};

inline void to_json (nlohmann::json &json, const join_actor_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"nodeRid", value.node_rid},
            {"accepted", value.accepted},
            {"error", value.error}};
}

inline void from_json (const nlohmann::json &json, join_actor_res_t &value)
{
    json.at ("actorId").get_to (value.actor_id);
    json.at ("nodeRid").get_to (value.node_rid);
    json.at ("accepted").get_to (value.accepted);
    json.at ("error").get_to (value.error);
}

/* Actor request packet: accumulates value so post-move continuity shows the
 * request still reaches the (moved) actor. */
struct actor_ping_req_t
{
    static constexpr const char *packet_name = "ObsActorPingReq";
    std::string actor_id;
    int value = 0;
};

inline void to_json (nlohmann::json &json, const actor_ping_req_t &value)
{
    json = {{"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_ping_req_t &value)
{
    json.at ("actorId").get_to (value.actor_id);
    json.at ("value").get_to (value.value);
}

struct actor_ping_res_t
{
    static constexpr const char *packet_name = "ObsActorPingRes";
    std::string actor_id;
    std::string node_rid;
    int total = 0;
};

inline void to_json (nlohmann::json &json, const actor_ping_res_t &value)
{
    json = {{"actorId", value.actor_id}, {"nodeRid", value.node_rid}, {"total", value.total}};
}

inline void from_json (const nlohmann::json &json, actor_ping_res_t &value)
{
    json.at ("actorId").get_to (value.actor_id);
    json.at ("nodeRid").get_to (value.node_rid);
    json.at ("total").get_to (value.total);
}

/* OBS-A4/B3: projection event fanned out to every mesh subscriber. */
struct projection_event_t
{
    static constexpr const char *packet_name = "ObsProjectionEvent";
    std::string spot_id;
    std::string marker;
    int applied = 0;
};

inline void to_json (nlohmann::json &json, const projection_event_t &value)
{
    json = {{"spotId", value.spot_id}, {"marker", value.marker}, {"applied", value.applied}};
}

inline void from_json (const nlohmann::json &json, projection_event_t &value)
{
    json.at ("spotId").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("applied").get_to (value.applied);
}

} // namespace zlink::framework::e2e::observability_ops

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zlink::e2e::spot_actor_transfer
{

inline constexpr const char *mesh_name = "spot-actor-transfer";
inline constexpr const char *actor_type_stateful = "transfer-stateful";
inline constexpr const char *actor_type_empty_state = "transfer-empty-state";
inline constexpr const char *actor_type_no_adapter = "transfer-no-adapter";
inline constexpr const char *actor_type_fail_transfer_out = "transfer-fail-out";
inline constexpr const char *actor_type_fail_leave = "transfer-fail-leave";
inline constexpr const char *actor_type_fail_transfer_in = "transfer-fail-in";
inline constexpr const char *entry_spot_id = "spot-actor-transfer-entry";

struct actor_create_req_t
{
    std::string actor_id;
    std::string actor_type;
    int state_version = 0;
};

struct actor_create_res_t
{
    std::string actor_id;
    std::string actor_type;
    std::string node_rid;
    std::int64_t generation = 0;
};

struct create_spot_req_t
{
    std::string spot_id;
    std::string mode = "accept";
};

struct create_spot_res_t
{
    std::string spot_id;
    std::string node_rid;
    std::string state;
};

struct gate_release_res_t
{
    std::string spot_id;
    bool released = false;
};

struct join_target_req_t
{
    static constexpr const char *packet_name = "JoinTargetReq";
    std::string scenario;
    std::string target_spot_id;
    std::string expected_mode = "accept";
};

struct join_target_res_t
{
    std::string scenario;
    std::string actor_id;
    bool accepted = false;
    std::string source_node_rid;
    std::string target_spot_id;
    int state_version = 0;
    std::string error_kind;
};

struct probe_req_t
{
    static constexpr const char *packet_name = "ProbeReq";
    std::string scenario;
    std::string marker;
};

struct handoff_packet_msg_t
{
    static constexpr const char *packet_name = "HandoffPacket";
    std::string scenario;
    std::string marker;
};

struct probe_res_t
{
    std::string scenario;
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    int state_version = 0;
    std::string marker;
};

struct actor_ref_probe_req_t
{
    std::string scenario;
    std::string marker;
    std::string node_rid;
    std::int64_t generation = 0;
    int timeout_ms = 5000;
};

struct actor_ref_probe_res_t
{
    bool succeeded = false;
    std::optional<probe_res_t> reply;
    std::string error_kind;
};

struct bind_actor_session_req_t
{
    static constexpr const char *packet_name = "BindActorSessionReq";
    std::string scenario;
    std::string actor_id;
    std::string node_rid;
    std::optional<std::int64_t> generation;
};

struct bind_actor_session_res_t
{
    static constexpr const char *packet_name = "BindActorSessionRes";
    std::string scenario;
    std::string actor_id;
    std::string node_rid;
    std::int64_t generation = 0;
};

struct bound_push_req_t
{
    static constexpr const char *packet_name = "BoundPushReq";
    std::string scenario;
    std::string marker;
};

struct bound_push_res_t
{
    std::string scenario;
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    std::string marker;
    int state_version = 0;
};

struct bound_push_notify_t
{
    static constexpr const char *packet_name = "BoundPushNotify";
    std::string scenario;
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    std::string marker;
    int state_version = 0;
};

struct evidence_wait_req_t
{
    std::vector<std::string> contains_all;
    int timeout_milliseconds = 10000;
};

struct actor_ref_snapshot_res_t
{
    std::string actor_id;
    std::string node_rid;
    std::int64_t generation = 0;
};

struct transfer_state_dto_t
{
    std::string actor_id;
    int state_version = 0;
};

struct actor_evidence_t
{
    std::string scenario;
    std::string actor_id;
    std::string kind;
    std::string value;
    std::string node_rid;
    std::string transfer_id;
    std::string correlation_id;
    std::string flow_id;
};

inline void to_json (nlohmann::json &json, const actor_create_req_t &value)
{
    json = {{"actorId", value.actor_id},
            {"actorType", value.actor_type},
            {"stateVersion", value.state_version}};
}

inline void from_json (const nlohmann::json &json, actor_create_req_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.actor_type = json.value ("actorType", "");
    value.state_version = json.value ("stateVersion", 0);
}

inline void to_json (nlohmann::json &json, const actor_create_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"actorType", value.actor_type},
            {"nodeRid", value.node_rid},
            {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_create_res_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.actor_type = json.value ("actorType", "");
    value.node_rid = json.value ("nodeRid", "");
    value.generation = json.value ("generation", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const create_spot_req_t &value)
{
    json = {{"spotId", value.spot_id}, {"mode", value.mode}};
}

inline void from_json (const nlohmann::json &json, create_spot_req_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.mode = json.value ("mode", "accept");
}

inline void to_json (nlohmann::json &json, const create_spot_res_t &value)
{
    json = {{"spotId", value.spot_id}, {"nodeRid", value.node_rid}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, create_spot_res_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.state = json.value ("state", "");
}

inline void to_json (nlohmann::json &json, const gate_release_res_t &value)
{
    json = {{"spotId", value.spot_id}, {"released", value.released}};
}

inline void from_json (const nlohmann::json &json, gate_release_res_t &value)
{
    value.spot_id = json.value ("spotId", "");
    value.released = json.value ("released", false);
}

inline void to_json (nlohmann::json &json, const join_target_req_t &value)
{
    json = {{"scenario", value.scenario},
            {"targetSpotId", value.target_spot_id},
            {"expectedMode", value.expected_mode}};
}

inline void from_json (const nlohmann::json &json, join_target_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.target_spot_id = json.value ("targetSpotId", "");
    value.expected_mode = json.value ("expectedMode", "accept");
}

inline void to_json (nlohmann::json &json, const join_target_res_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"accepted", value.accepted},
            {"sourceNodeRid", value.source_node_rid},
            {"targetSpotId", value.target_spot_id},
            {"stateVersion", value.state_version}};
    if (!value.error_kind.empty ()) {
        json["errorKind"] = value.error_kind;
    }
}

inline void from_json (const nlohmann::json &json, join_target_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.accepted = json.value ("accepted", false);
    value.source_node_rid = json.value ("sourceNodeRid", "");
    value.target_spot_id = json.value ("targetSpotId", "");
    value.state_version = json.value ("stateVersion", 0);
    value.error_kind = json.value ("errorKind", "");
}

inline void to_json (nlohmann::json &json, const probe_req_t &value)
{
    json = {{"scenario", value.scenario}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, probe_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const handoff_packet_msg_t &value)
{
    json = {{"scenario", value.scenario}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, handoff_packet_msg_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const probe_res_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"spotId", value.spot_id},
            {"nodeRid", value.node_rid},
            {"stateVersion", value.state_version},
            {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, probe_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.spot_id = json.value ("spotId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.state_version = json.value ("stateVersion", 0);
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const actor_ref_probe_req_t &value)
{
    json = {{"scenario", value.scenario},
            {"marker", value.marker},
            {"nodeRid", value.node_rid},
            {"generation", value.generation},
            {"timeoutMs", value.timeout_ms}};
}

inline void from_json (const nlohmann::json &json, actor_ref_probe_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.marker = json.value ("marker", "");
    value.node_rid = json.value ("nodeRid", "");
    value.generation = json.value ("generation", std::int64_t{0});
    value.timeout_ms = json.value ("timeoutMs", 5000);
}

inline void to_json (nlohmann::json &json, const actor_ref_probe_res_t &value)
{
    json = {{"succeeded", value.succeeded}};
    if (value.reply) {
        json["reply"] = *value.reply;
    }
    if (!value.error_kind.empty ()) {
        json["errorKind"] = value.error_kind;
    }
}

inline void from_json (const nlohmann::json &json, actor_ref_probe_res_t &value)
{
    value.succeeded = json.value ("succeeded", false);
    if (json.contains ("reply") && !json["reply"].is_null ()) {
        value.reply = json["reply"].get<probe_res_t> ();
    }
    value.error_kind = json.contains ("errorKind") && !json["errorKind"].is_null ()
                         ? json.value ("errorKind", "")
                         : "";
}

inline void to_json (nlohmann::json &json, const bind_actor_session_req_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}};
    if (!value.node_rid.empty ()) {
        json["nodeRid"] = value.node_rid;
    }
    if (value.generation) {
        json["generation"] = *value.generation;
    }
}

inline void from_json (const nlohmann::json &json, bind_actor_session_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.node_rid =
      json.contains ("nodeRid") && !json["nodeRid"].is_null () ? json.value ("nodeRid", "") : "";
    if (json.contains ("generation") && !json["generation"].is_null ()) {
        value.generation = json["generation"].get<std::int64_t> ();
    }
}

inline void to_json (nlohmann::json &json, const bind_actor_session_res_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"nodeRid", value.node_rid},
            {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, bind_actor_session_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.generation = json.value ("generation", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const bound_push_req_t &value)
{
    json = {{"scenario", value.scenario}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, bound_push_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const bound_push_res_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"spotId", value.spot_id},
            {"nodeRid", value.node_rid},
            {"marker", value.marker},
            {"stateVersion", value.state_version}};
}

inline void from_json (const nlohmann::json &json, bound_push_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.spot_id = json.value ("spotId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.marker = json.value ("marker", "");
    value.state_version = json.value ("stateVersion", 0);
}

inline void to_json (nlohmann::json &json, const bound_push_notify_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"spotId", value.spot_id},
            {"nodeRid", value.node_rid},
            {"marker", value.marker},
            {"stateVersion", value.state_version}};
}

inline void from_json (const nlohmann::json &json, bound_push_notify_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.spot_id = json.value ("spotId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.marker = json.value ("marker", "");
    value.state_version = json.value ("stateVersion", 0);
}

inline void to_json (nlohmann::json &json, const evidence_wait_req_t &value)
{
    json = {{"containsAll", value.contains_all},
            {"timeoutMilliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, evidence_wait_req_t &value)
{
    value.contains_all = json.value ("containsAll", std::vector<std::string>{});
    value.timeout_milliseconds = json.value ("timeoutMilliseconds", 10000);
}

inline void to_json (nlohmann::json &json, const actor_ref_snapshot_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"nodeRid", value.node_rid},
            {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_ref_snapshot_res_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.node_rid = json.value ("nodeRid", "");
    value.generation = json.value ("generation", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const transfer_state_dto_t &value)
{
    json = {{"actorId", value.actor_id}, {"stateVersion", value.state_version}};
}

inline void from_json (const nlohmann::json &json, transfer_state_dto_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.state_version = json.value ("stateVersion", 0);
}

inline void to_json (nlohmann::json &json, const actor_evidence_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"kind", value.kind},
            {"value", value.value},
            {"nodeRid", value.node_rid},
            {"transferId", value.transfer_id},
            {"correlationId", value.correlation_id},
            {"flowId", value.flow_id}};
}

inline void from_json (const nlohmann::json &json, actor_evidence_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.kind = json.value ("kind", "");
    value.value = json.value ("value", "");
    value.node_rid = json.value ("nodeRid", "");
    value.transfer_id = json.value ("transferId", "");
    value.correlation_id = json.value ("correlationId", "");
    value.flow_id = json.value ("flowId", "");
}

inline std::string evidence_text (const actor_evidence_t &evidence)
{
    return evidence.scenario + "|" + evidence.actor_id + "|" + evidence.kind + "|" + evidence.value
           + "|" + evidence.node_rid + "|" + evidence.transfer_id + "|"
           + evidence.correlation_id + "|" + evidence.flow_id;
}

// Bridges the nlohmann to_json/from_json contracts above into the stream payload
// hooks the connector uses for typed request/reply and push decoding. Without
// these the connector's response decode falls through to a no-op and yields an
// empty message (see stream_payload.hpp apply_packet_payload fallback).
template <typename T> zlink::message_t to_stream_payload (const T &value)
{
    return zlink::message_t::from_json (value);
}

template <typename T> void from_stream_payload (const zlink::message_t &payload, T &value)
{
    value = payload.parse_json<T> ();
}

} // namespace zlink::e2e::spot_actor_transfer

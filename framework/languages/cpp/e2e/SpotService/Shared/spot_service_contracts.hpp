/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <map>
#include <vector>

namespace zlink::framework::e2e::spot_service
{

inline constexpr const char *route_channel = "spot.service.play";
inline constexpr const char *api_channel = "spot.service.api";
inline constexpr const char *spot_mesh = "spot.service.mesh";
inline constexpr const char *spot_only_mesh = "spot-only.mesh";
inline constexpr const char *publisher_channel = "spot.service.publisher";
inline constexpr const char *handler_group = "spot-service";
inline constexpr const char *actor_type = "scenario-player";
inline constexpr const char *user_spot = "user";
inline constexpr const char *alternate_spot = "alternate-user";
inline constexpr const char *mesh_topic = "spot-service-topic";
inline constexpr const char *multi_route_channel_a = "spot.service.multi.a";
inline constexpr const char *multi_route_channel_b = "spot.service.multi.b";
inline constexpr const char *multi_spot_a = "multi-a";
inline constexpr const char *multi_spot_b = "multi-b";

inline std::string owner_node_rid_for_key (const std::string &key)
{
    if (key.rfind ("b-", 0) == 0 || key.rfind ("remote-", 0) == 0) {
        return "play-b";
    }
    return "play-a";
}

inline std::string user_spot_id_for_key (const std::string &key)
{
    return "user:" + owner_node_rid_for_key (key) + ":" + key;
}

struct actor_ref_dto_t
{
    std::string node_rid;
    std::string actor_type;
    std::string actor_id;
    std::uint64_t generation = 0;
};

struct ensure_actor_req_t
{
    static constexpr const char *packet_name = "EnsureActor";
    std::string actor_id;
    std::string display_name;
};

struct ensure_actor_res_t
{
    actor_ref_dto_t actor;
};

struct join_req_t
{
    std::string key;
    std::string actor_id;
    std::string display_name;
    int level = 0;
    std::vector<std::string> tags;
};

struct join_res_t
{
    std::string spot_id;
    std::string owner_node_rid;
    std::string actor_id;
    std::string display_name;
    int level = 0;
    std::vector<std::string> tags;
    actor_ref_dto_t actor;
};

struct state_req_t
{
    static constexpr const char *packet_name = "StateReq";
    std::string op;
    int amount = 0;
};

struct spot_state_req_t
{
    std::string actor_id;
    state_req_t state;
};

struct remote_actor_flow_req_t
{
    join_req_t join;
    state_req_t state;
};

struct remote_actor_request_req_t
{
    join_req_t join;
    state_req_t state;
};

struct missing_actor_req_t
{
    join_req_t join;
    std::string packet_name;
};

struct missing_actor_res_t
{
    bool failed = false;
    std::string error_kind;
};

struct actor_ping_req_t
{
    std::string value;
};

struct slow_actor_ping_req_t
{
    std::string value;
    int delay_ms = 0;
};

struct actor_ping_res_t
{
    std::string actor_id;
    std::string node_rid;
    std::string spot_id;
    std::string value;
    int seen = 0;
};

struct spot_state_route_req_t
{
    std::string key;
    std::string target_node_rid;
    std::string spot_id;
    state_req_t state;
};

struct stage_probe_req_t
{
    static constexpr const char *packet_name = "StageProbeReq";
    std::string marker;
    int delta = 0;
};

struct stage_timer_start_msg_t
{
    static constexpr const char *packet_name = "StageTimerStartMsg";
    std::string name;
    int period_ms = 0;
};

struct spot_stage_probe_req_t
{
    std::string spot_id;
    std::string marker;
    int delta = 0;
};

struct spot_stage_timer_req_t
{
    std::string spot_id;
    std::string name;
    int period_ms = 0;
};

struct state_res_t
{
    std::string spot_id;
    std::string owner_node_rid;
    int value = 0;
    int sequence = 0;
};

struct multi_node_create_spot_req_t
{
    std::string spot_id;
    int delta = 0;
};

struct multi_node_create_spot_res_t
{
    std::string spot_id;
    std::string node_rid;
    std::string state;
    int value = 0;
};

struct multi_node_state_route_req_t
{
    static constexpr const char *packet_name = "MultiNodeStateReq";
    std::string spot_id;
    int delta = 0;
};

struct join_admitted_user_spot_actor_req_t
{
    std::string spot_id;
    std::string actor_id;
    bool allow = false;
    std::string reason;
};

struct join_admitted_user_spot_actor_res_t
{
    std::string spot_id;
    std::string actor_id;
    bool accepted = false;
    std::uint64_t generation = 0;
    std::string error_kind;
};

struct remote_actor_flow_res_t
{
    join_res_t join;
    state_res_t state;
};

struct complex_actor_req_t
{
    std::string display_name;
    int level = 0;
    std::vector<std::string> tags;
    std::map<std::string, std::string> attributes;
};

struct complex_actor_res_t
{
    std::string actor_id;
    std::string display_name;
    int level = 0;
    std::vector<std::string> tags;
    std::map<std::string, std::string> attributes;
};

struct spot_complex_actor_req_t
{
    join_req_t join;
    complex_actor_req_t complex;
};

struct spot_complex_actor_res_t
{
    join_res_t join;
    complex_actor_res_t complex;
};

struct leave_req_t
{
    std::string actor_id;
    std::string reason;
};

struct leave_res_t
{
    bool left = false;
    std::string actor_id;
};

struct destroy_actor_req_t
{
    std::string reason;
};

struct destroy_actor_res_t
{
    bool destroyed = false;
    std::string actor_id;
};

struct disconnect_req_t
{
    std::string reason;
};

struct disconnect_res_t
{
    bool disconnected = false;
    std::string actor_id;
};

struct channel_echo_req_t
{
    static constexpr const char *packet_name = "ChannelEchoReq";
    std::string value;
};

struct channel_echo_res_t
{
    std::string value;
    std::string handled_by;
};

struct channel_control_ping_req_t
{
    static constexpr const char *packet_name = "MultiNodeRoutePing";
    std::string target_node_rid;
    std::string value;
    std::string mesh_name;
};

struct channel_control_ping_res_t
{
    std::string node_rid;
    std::string value;
};

struct channel_msg_t
{
    std::string command_id;
};

/* Negative-path wire packets: these names deliberately have no handler on
 * the api channel, so senders pick the variant type instead of a per-call
 * name override (CPP-G0-DISPATCH-001). */
struct missing_channel_req_t : channel_echo_req_t
{
    static constexpr const char *packet_name = "MissingChannelReq";
};

struct missing_channel_msg_t : channel_msg_t
{
    static constexpr const char *packet_name = "MissingChannelMsg";
};

struct channel_slow_req_t
{
    std::string value;
    int delay_ms = 0;
};

struct channel_slow_res_t
{
    std::string value;
};

struct mesh_msg_t
{
    std::string event_id;
    std::string value;
};

struct outbound_req_t
{
    static constexpr const char *packet_name = "SpotOutboundReq";
    std::string value;
};

/* Wire variant: the negative path targets its own handler registration
 * name (CPP-G0-DISPATCH-001). */
struct outbound_negative_req_t : outbound_req_t
{
    static constexpr const char *packet_name = "SpotOutboundNegativeReq";
};

struct outbound_res_t
{
    std::string channel_reply;
    bool command_sent = false;
    bool published = false;
};

struct spot_outbound_route_req_t
{
    std::string target_node_rid;
    std::string spot_id;
    std::string marker;
};

struct spot_outbound_route_res_t
{
    std::string spot_id;
    std::string marker;
    bool accepted = false;
};

struct worker_req_t
{
    int delta = 0;
    int delay_ms = 0;
};

struct spot_worker_req_t
{
    std::string actor_id;
    worker_req_t worker;
};

struct spot_worker_start_req_t
{
    static constexpr const char *packet_name = "WorkerStartReq";
    std::string spot_id;
    std::string marker;
    int delay_ms = 0;
};

struct spot_worker_start_res_t
{
    std::string spot_id;
    std::string owner_node_rid;
    std::string marker;
};

struct spot_worker_complete_req_t
{
    std::string spot_id;
    std::string marker;
};

struct worker_res_t
{
    int snapshot = 0;
    int worker_result = 0;
    int final_value = 0;
    int sequence = 0;
};

struct direct_spot_req_t
{
    static constexpr const char *packet_name = "DirectSpotReq";
    std::string source_actor_id;
    std::string value;
};

struct direct_spot_route_req_t
{
    std::string target_node_rid;
    std::string spot_id;
    std::string value;
    std::string source_actor_id = "sm-a3-client";
};

struct create_spot_req_t
{
    std::string spot_id;
};

struct create_spot_res_t
{
    std::string spot_id;
    std::string owner_node_rid;
    bool created = false;
};

struct direct_spot_res_t
{
    std::string spot_id;
    std::string owner_node_rid;
    std::string value;
};

struct direct_spot_msg_t
{
    static constexpr const char *packet_name = "DirectSpotMsg";
    std::string source_actor_id;
    std::string value;
};

struct spot_state_command_route_req_t
{
    std::string target_node_rid;
    std::string spot_id;
    std::string marker;
};

struct spot_state_command_route_res_t
{
    bool accepted = false;
};

struct spot_publish_route_req_t
{
    std::string spot_id;
    std::string marker;
};

struct spot_publish_route_res_t
{
    bool accepted = false;
};

struct slow_spot_req_t
{
    static constexpr const char *packet_name = "SlowSpotReq";
    std::string value;
};

struct spot_slow_route_req_t
{
    std::string target_node_rid;
    std::string spot_id;
    std::string value;
    int timeout_ms = 0;
};

struct spot_slow_route_res_t
{
    bool timed_out = false;
};

struct unhandled_spot_req_t
{
    static constexpr const char *packet_name = "MissingSpotReq";
    std::string value;
};

/* One-way negative variant: the drop path asserts its own packet name
 * (CPP-G0-DISPATCH-001). */
struct unhandled_spot_msg_t : unhandled_spot_req_t
{
    static constexpr const char *packet_name = "MissingSpotMsg";
};

struct spot_missing_route_req_t
{
    std::string target_node_rid;
    std::string spot_id;
    std::string value;
};

struct spot_missing_route_res_t
{
    bool request_failed = false;
    bool command_sent = false;
};

struct spot_missing_handler_req_t
{
    std::string spot_id;
};

struct spot_missing_command_req_t
{
    std::string spot_id;
    std::string marker;
};

struct spot_missing_target_req_t
{
    std::string spot_id;
};

struct spot_missing_target_res_t
{
    std::string spot_id;
    bool failed = false;
};

struct spot_to_spot_req_t
{
    std::string target_key;
    std::string value;
};

struct spot_to_spot_res_t
{
    std::string request_reply;
    bool command_sent = false;
    bool published = false;
    bool missing_rejected = false;
    bool timeout_rejected = false;
};

struct spot_to_spot_route_req_t
{
    std::string source_node_rid;
    std::string source_spot_id;
    std::string target_node_rid;
    std::string target_spot_id;
    std::string marker;
};

/* Wire variants: each registered handler name is its own packet, so the
 * sender picks the variant type instead of a per-call name override
 * (CPP-G0-DISPATCH-001). */
struct spot_to_spot_direct_req_t : spot_to_spot_route_req_t
{
    static constexpr const char *packet_name = "SpotToSpotDirectReq";
};

struct spot_to_spot_timeout_req_t : spot_to_spot_route_req_t
{
    static constexpr const char *packet_name = "SpotToSpotTimeoutReq";
};

struct spot_to_spot_negative_req_t : spot_to_spot_route_req_t
{
    static constexpr const char *packet_name = "SpotToSpotNegativeReq";
};

struct spot_to_spot_route_res_t
{
    std::string source_spot_id;
    std::string target_spot_id;
    std::string target_value;
};

struct spot_to_spot_timeout_route_res_t
{
    std::string source_spot_id;
    std::string target_spot_id;
    bool failed = false;
};

struct spot_to_spot_negative_route_res_t
{
    std::string source_spot_id;
    std::string target_spot_id;
    bool request_failed = false;
};

struct type_mismatch_req_t
{
    std::string probe;
};

struct type_mismatch_res_t
{
    bool rejected = false;
    std::string error_kind;
    std::string spot_name;
    int value = 0;
};

struct lifecycle_req_t
{
    std::string key;
};

struct lifecycle_res_t
{
    std::string spot_id;
    bool created = false;
    bool closed = false;
};

struct close_spot_req_t
{
    std::string key;
};

struct close_spot_res_t
{
    std::string spot_id;
    bool closed = false;
};

struct idle_close_msg_t
{
    std::string name;
    int period_ms = 0;
};

struct spot_idle_close_req_t
{
    std::string spot_id;
    std::string name;
    int period_ms = 0;
};

struct overrun_timer_msg_t
{
    std::string name;
    std::string policy;
    int period_ms = 0;
};

struct spot_overrun_start_req_t
{
    std::string spot_id;
    std::string name;
    std::string policy;
    int period_ms = 0;
};

struct stream_auth_req_t
{
    std::string target_node_rid;
    std::string actor_id;
    std::string display_name;
    actor_ref_dto_t actor;
};

struct stream_ensure_auth_req_t
{
    std::string target_node_rid;
    std::string actor_id;
    std::string display_name;
};

struct stream_auth_res_t
{
    actor_ref_dto_t actor;
    std::string session_node_rid;
};

struct actor_push_req_t
{
    static constexpr const char *packet_name = "PushReq";
    std::string value;
};

struct actor_push_by_actor_req_t
{
    std::string actor_id;
    std::string value;
};

struct actor_push_by_actor_res_t
{
    std::string actor_id;
    std::string value;
    bool delivered = false;
    std::string error_kind;
};

struct snapshot_req_t
{
    std::string actor_id;
};

struct snapshot_res_t
{
    std::string actor_id;
    int seen = 0;
};

struct bound_session_push_req_t
{
    std::string actor_id;
    actor_push_req_t push;
};

struct actor_push_res_t
{
    bool pushed = false;
    std::string actor_id;
    int seen = 0;
};

struct actor_push_notify_t
{
    std::string actor_id;
    std::string value;
    int seen = 0;
};

struct spot_only_mesh_req_t
{
    std::string source_spot_id;
    std::string target_spot_id;
    std::string marker;
};

struct spot_only_mesh_res_t
{
    std::string source_spot_id;
    std::string target_spot_id;
    int target_value = 0;
    std::string marker;
};

struct spot_only_join_req_t
{
    std::string target_spot_id;
    std::string actor_id;
    std::string marker;
};

struct spot_only_join_res_t
{
    std::string target_spot_id;
    std::string actor_id;
    bool accepted = false;
    std::string marker;
};

struct evidence_entry_t
{
    std::string marker;
    std::string node_rid;
    std::string actor_id;
    std::string spot_id;
    std::string value;
};

struct evidence_snapshot_t
{
    std::string node_rid;
    std::vector<evidence_entry_t> entries;
};

struct spot_stage_timer_res_t
{
    std::string spot_id;
    std::string name;
    bool started = false;
    evidence_snapshot_t evidence;
};

struct spot_worker_complete_res_t
{
    std::string spot_id;
    std::string marker;
    bool completed = false;
    evidence_snapshot_t evidence;
};

struct spot_idle_close_res_t
{
    std::string spot_id;
    std::string name;
    bool closed = false;
    evidence_snapshot_t evidence;
};

struct spot_overrun_start_res_t
{
    std::string spot_id;
    std::string name;
    bool started = false;
    evidence_snapshot_t evidence;
};

struct spot_publish_observe_res_t
{
    std::string operation;
    std::string spot_id;
    std::string marker;
    bool received = false;
    evidence_snapshot_t evidence;
};

struct spot_missing_handler_res_t
{
    std::string spot_id;
    bool failed = false;
    evidence_snapshot_t evidence;
};

struct spot_missing_command_res_t
{
    std::string spot_id;
    std::string marker;
    bool sent = false;
    evidence_snapshot_t evidence;
};

struct evidence_wait_req_t
{
    std::vector<std::string> contains_all;
    int timeout_milliseconds = 10000;
};

inline void to_json (nlohmann::json &json, const actor_ref_dto_t &value)
{
    json = nlohmann::json{{"node_rid", value.node_rid},
                          {"actor_type", value.actor_type},
                          {"actor_id", value.actor_id},
                          {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_ref_dto_t &value)
{
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("actor_type").get_to (value.actor_type);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("generation").get_to (value.generation);
}

inline void to_json (nlohmann::json &json, const ensure_actor_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"display_name", value.display_name}};
}

inline void from_json (const nlohmann::json &json, ensure_actor_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
}

inline void to_json (nlohmann::json &json, const ensure_actor_res_t &value)
{
    json = nlohmann::json{{"actor", value.actor}};
}

inline void from_json (const nlohmann::json &json, ensure_actor_res_t &value)
{
    json.at ("actor").get_to (value.actor);
}

inline void to_json (nlohmann::json &json, const join_req_t &value)
{
    json = nlohmann::json{{"key", value.key},
                          {"actor_id", value.actor_id},
                          {"display_name", value.display_name},
                          {"level", value.level},
                          {"tags", value.tags}};
}

inline void from_json (const nlohmann::json &json, join_req_t &value)
{
    json.at ("key").get_to (value.key);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
    json.at ("level").get_to (value.level);
    json.at ("tags").get_to (value.tags);
}

inline void to_json (nlohmann::json &json, const join_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"owner_node_rid", value.owner_node_rid},
                          {"actor_id", value.actor_id}, {"display_name", value.display_name},
                          {"level", value.level},       {"tags", value.tags},
                          {"actor", value.actor}};
}

inline void from_json (const nlohmann::json &json, join_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("owner_node_rid").get_to (value.owner_node_rid);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
    json.at ("level").get_to (value.level);
    json.at ("tags").get_to (value.tags);
    value.actor = json.value ("actor", actor_ref_dto_t{});
}

inline void to_json (nlohmann::json &json, const state_req_t &value)
{
    json = nlohmann::json{{"op", value.op}, {"amount", value.amount}};
}

inline void from_json (const nlohmann::json &json, state_req_t &value)
{
    json.at ("op").get_to (value.op);
    json.at ("amount").get_to (value.amount);
}

inline void to_json (nlohmann::json &json, const spot_state_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, spot_state_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("state").get_to (value.state);
}

inline void to_json (nlohmann::json &json, const remote_actor_flow_req_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, remote_actor_flow_req_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("state").get_to (value.state);
}

inline void to_json (nlohmann::json &json, const remote_actor_request_req_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, remote_actor_request_req_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("state").get_to (value.state);
}

inline void to_json (nlohmann::json &json, const missing_actor_req_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"packet_name", value.packet_name}};
}

inline void from_json (const nlohmann::json &json, missing_actor_req_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("packet_name").get_to (value.packet_name);
}

inline void to_json (nlohmann::json &json, const missing_actor_res_t &value)
{
    json = nlohmann::json{{"failed", value.failed}, {"error_kind", value.error_kind}};
}

inline void from_json (const nlohmann::json &json, missing_actor_res_t &value)
{
    json.at ("failed").get_to (value.failed);
    json.at ("error_kind").get_to (value.error_kind);
}

inline void to_json (nlohmann::json &json, const actor_ping_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_ping_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const slow_actor_ping_req_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, slow_actor_ping_req_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("delay_ms").get_to (value.delay_ms);
}

inline void to_json (nlohmann::json &json, const actor_ping_res_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"node_rid", value.node_rid},
                          {"spot_id", value.spot_id},
                          {"value", value.value},
                          {"seen", value.seen}};
}

inline void from_json (const nlohmann::json &json, actor_ping_res_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("value").get_to (value.value);
    json.at ("seen").get_to (value.seen);
}

inline void to_json (nlohmann::json &json, const spot_state_route_req_t &value)
{
    json = nlohmann::json{{"key", value.key},
                          {"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, spot_state_route_req_t &value)
{
    if (json.contains ("key")) {
        json.at ("key").get_to (value.key);
    }
    if (json.contains ("target_node_rid")) {
        json.at ("target_node_rid").get_to (value.target_node_rid);
    }
    if (json.contains ("spot_id")) {
        json.at ("spot_id").get_to (value.spot_id);
    }
    if (json.contains ("state")) {
        json.at ("state").get_to (value.state);
    } else {
        if (json.contains ("op")) {
            json.at ("op").get_to (value.state.op);
        }
        if (json.contains ("amount")) {
            json.at ("amount").get_to (value.state.amount);
        }
    }
}

inline void to_json (nlohmann::json &json, const stage_probe_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}, {"delta", value.delta}};
}

inline void from_json (const nlohmann::json &json, stage_probe_req_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("delta").get_to (value.delta);
}

inline void to_json (nlohmann::json &json, const stage_timer_start_msg_t &value)
{
    json = nlohmann::json{{"name", value.name}, {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, stage_timer_start_msg_t &value)
{
    json.at ("name").get_to (value.name);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const spot_stage_probe_req_t &value)
{
    json = nlohmann::json{
      {"spot_id", value.spot_id}, {"marker", value.marker}, {"delta", value.delta}};
}

inline void from_json (const nlohmann::json &json, spot_stage_probe_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("delta").get_to (value.delta);
}

inline void to_json (nlohmann::json &json, const spot_stage_timer_req_t &value)
{
    json = nlohmann::json{
      {"spot_id", value.spot_id}, {"name", value.name}, {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, spot_stage_timer_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const state_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"owner_node_rid", value.owner_node_rid},
                          {"value", value.value},
                          {"sequence", value.sequence}};
}

inline void from_json (const nlohmann::json &json, state_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("owner_node_rid").get_to (value.owner_node_rid);
    json.at ("value").get_to (value.value);
    json.at ("sequence").get_to (value.sequence);
}

inline void to_json (nlohmann::json &json, const multi_node_create_spot_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"delta", value.delta}};
}

inline void from_json (const nlohmann::json &json, multi_node_create_spot_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("delta").get_to (value.delta);
}

inline void to_json (nlohmann::json &json, const multi_node_create_spot_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"node_rid", value.node_rid},
                          {"state", value.state},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, multi_node_create_spot_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("state").get_to (value.state);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const multi_node_state_route_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"delta", value.delta}};
}

inline void from_json (const nlohmann::json &json, multi_node_state_route_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("delta").get_to (value.delta);
}

inline void to_json (nlohmann::json &json, const join_admitted_user_spot_actor_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"actor_id", value.actor_id},
                          {"allow", value.allow},
                          {"reason", value.reason}};
}

inline void from_json (const nlohmann::json &json, join_admitted_user_spot_actor_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("allow").get_to (value.allow);
    json.at ("reason").get_to (value.reason);
}

inline void to_json (nlohmann::json &json, const join_admitted_user_spot_actor_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"actor_id", value.actor_id},
                          {"accepted", value.accepted},
                          {"generation", value.generation},
                          {"error_kind", value.error_kind}};
}

inline void from_json (const nlohmann::json &json, join_admitted_user_spot_actor_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("accepted").get_to (value.accepted);
    if (json.contains ("generation")) {
        json.at ("generation").get_to (value.generation);
    }
    if (json.contains ("error_kind")) {
        json.at ("error_kind").get_to (value.error_kind);
    }
}

inline void to_json (nlohmann::json &json, const remote_actor_flow_res_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, remote_actor_flow_res_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("state").get_to (value.state);
}

inline void to_json (nlohmann::json &json, const complex_actor_req_t &value)
{
    json = nlohmann::json{{"display_name", value.display_name},
                          {"level", value.level},
                          {"tags", value.tags},
                          {"attributes", value.attributes}};
}

inline void from_json (const nlohmann::json &json, complex_actor_req_t &value)
{
    json.at ("display_name").get_to (value.display_name);
    json.at ("level").get_to (value.level);
    json.at ("tags").get_to (value.tags);
    json.at ("attributes").get_to (value.attributes);
}

inline void to_json (nlohmann::json &json, const complex_actor_res_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"display_name", value.display_name},
                          {"level", value.level},
                          {"tags", value.tags},
                          {"attributes", value.attributes}};
}

inline void from_json (const nlohmann::json &json, complex_actor_res_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
    json.at ("level").get_to (value.level);
    json.at ("tags").get_to (value.tags);
    json.at ("attributes").get_to (value.attributes);
}

inline void to_json (nlohmann::json &json, const spot_complex_actor_req_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"complex", value.complex}};
}

inline void from_json (const nlohmann::json &json, spot_complex_actor_req_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("complex").get_to (value.complex);
}

inline void to_json (nlohmann::json &json, const spot_complex_actor_res_t &value)
{
    json = nlohmann::json{{"join", value.join}, {"complex", value.complex}};
}

inline void from_json (const nlohmann::json &json, spot_complex_actor_res_t &value)
{
    json.at ("join").get_to (value.join);
    json.at ("complex").get_to (value.complex);
}

inline void to_json (nlohmann::json &json, const leave_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"reason", value.reason}};
}

inline void from_json (const nlohmann::json &json, leave_req_t &value)
{
    if (json.contains ("actor_id")) {
        json.at ("actor_id").get_to (value.actor_id);
    }
    if (json.contains ("reason")) {
        json.at ("reason").get_to (value.reason);
    }
}

inline void to_json (nlohmann::json &json, const leave_res_t &value)
{
    json = nlohmann::json{{"left", value.left}, {"actor_id", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, leave_res_t &value)
{
    json.at ("left").get_to (value.left);
    json.at ("actor_id").get_to (value.actor_id);
}

inline void to_json (nlohmann::json &json, const destroy_actor_req_t &value)
{
    json = nlohmann::json{{"reason", value.reason}};
}

inline void from_json (const nlohmann::json &json, destroy_actor_req_t &value)
{
    json.at ("reason").get_to (value.reason);
}

inline void to_json (nlohmann::json &json, const destroy_actor_res_t &value)
{
    json = nlohmann::json{{"destroyed", value.destroyed}, {"actor_id", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, destroy_actor_res_t &value)
{
    json.at ("destroyed").get_to (value.destroyed);
    json.at ("actor_id").get_to (value.actor_id);
}

inline void to_json (nlohmann::json &json, const disconnect_req_t &value)
{
    json = nlohmann::json{{"reason", value.reason}};
}

inline void from_json (const nlohmann::json &json, disconnect_req_t &value)
{
    json.at ("reason").get_to (value.reason);
}

inline void to_json (nlohmann::json &json, const disconnect_res_t &value)
{
    json = nlohmann::json{{"disconnected", value.disconnected}, {"actor_id", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, disconnect_res_t &value)
{
    json.at ("disconnected").get_to (value.disconnected);
    json.at ("actor_id").get_to (value.actor_id);
}

inline void to_json (nlohmann::json &json, const channel_echo_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, channel_echo_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const channel_echo_res_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"handled_by", value.handled_by}};
}

inline void from_json (const nlohmann::json &json, channel_echo_res_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("handled_by").get_to (value.handled_by);
}

inline void to_json (nlohmann::json &json, const channel_control_ping_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"value", value.value},
                          {"mesh_name", value.mesh_name}};
}

inline void from_json (const nlohmann::json &json, channel_control_ping_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("value").get_to (value.value);
    value.mesh_name = json.value ("mesh_name", "");
}

inline void to_json (nlohmann::json &json, const channel_control_ping_res_t &value)
{
    json = nlohmann::json{{"node_rid", value.node_rid}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, channel_control_ping_res_t &value)
{
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const channel_msg_t &value)
{
    json = nlohmann::json{{"command_id", value.command_id}};
}

inline void from_json (const nlohmann::json &json, channel_msg_t &value)
{
    json.at ("command_id").get_to (value.command_id);
}

inline void to_json (nlohmann::json &json, const missing_channel_req_t &value)
{
    to_json (json, static_cast<const channel_echo_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, missing_channel_req_t &value)
{
    from_json (json, static_cast<channel_echo_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const missing_channel_msg_t &value)
{
    to_json (json, static_cast<const channel_msg_t &> (value));
}

inline void from_json (const nlohmann::json &json, missing_channel_msg_t &value)
{
    from_json (json, static_cast<channel_msg_t &> (value));
}

inline void to_json (nlohmann::json &json, const channel_slow_req_t &value)
{
    json = nlohmann::json{{"value", value.value}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, channel_slow_req_t &value)
{
    json.at ("value").get_to (value.value);
    json.at ("delay_ms").get_to (value.delay_ms);
}

inline void to_json (nlohmann::json &json, const channel_slow_res_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, channel_slow_res_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const mesh_msg_t &value)
{
    json = nlohmann::json{{"event_id", value.event_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, mesh_msg_t &value)
{
    json.at ("event_id").get_to (value.event_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const outbound_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, outbound_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const outbound_negative_req_t &value)
{
    to_json (json, static_cast<const outbound_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, outbound_negative_req_t &value)
{
    from_json (json, static_cast<outbound_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const outbound_res_t &value)
{
    json = nlohmann::json{{"channel_reply", value.channel_reply},
                          {"command_sent", value.command_sent},
                          {"published", value.published}};
}

inline void from_json (const nlohmann::json &json, outbound_res_t &value)
{
    json.at ("channel_reply").get_to (value.channel_reply);
    json.at ("command_sent").get_to (value.command_sent);
    json.at ("published").get_to (value.published);
}

inline void to_json (nlohmann::json &json, const spot_outbound_route_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_outbound_route_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_outbound_route_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"marker", value.marker},
                          {"accepted", value.accepted}};
}

inline void from_json (const nlohmann::json &json, spot_outbound_route_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("accepted").get_to (value.accepted);
}

inline void to_json (nlohmann::json &json, const worker_req_t &value)
{
    json = nlohmann::json{{"delta", value.delta}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, worker_req_t &value)
{
    json.at ("delta").get_to (value.delta);
    json.at ("delay_ms").get_to (value.delay_ms);
}

inline void to_json (nlohmann::json &json, const spot_worker_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"worker", value.worker}};
}

inline void from_json (const nlohmann::json &json, spot_worker_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("worker").get_to (value.worker);
}

inline void to_json (nlohmann::json &json, const spot_worker_start_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"marker", value.marker},
                          {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, spot_worker_start_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("delay_ms").get_to (value.delay_ms);
}

inline void to_json (nlohmann::json &json, const spot_worker_start_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"owner_node_rid", value.owner_node_rid},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_worker_start_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("owner_node_rid").get_to (value.owner_node_rid);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_worker_complete_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_worker_complete_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const worker_res_t &value)
{
    json = nlohmann::json{{"snapshot", value.snapshot},
                          {"worker_result", value.worker_result},
                          {"final_value", value.final_value},
                          {"sequence", value.sequence}};
}

inline void from_json (const nlohmann::json &json, worker_res_t &value)
{
    json.at ("snapshot").get_to (value.snapshot);
    json.at ("worker_result").get_to (value.worker_result);
    json.at ("final_value").get_to (value.final_value);
    json.at ("sequence").get_to (value.sequence);
}

inline void to_json (nlohmann::json &json, const direct_spot_req_t &value)
{
    json = nlohmann::json{{"source_actor_id", value.source_actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, direct_spot_req_t &value)
{
    json.at ("source_actor_id").get_to (value.source_actor_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const direct_spot_route_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"value", value.value},
                          {"source_actor_id", value.source_actor_id}};
}

inline void from_json (const nlohmann::json &json, direct_spot_route_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("value").get_to (value.value);
    if (json.contains ("source_actor_id")) {
        json.at ("source_actor_id").get_to (value.source_actor_id);
    }
}

inline void to_json (nlohmann::json &json, const create_spot_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, create_spot_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
}

inline void to_json (nlohmann::json &json, const create_spot_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"owner_node_rid", value.owner_node_rid},
                          {"created", value.created}};
}

inline void from_json (const nlohmann::json &json, create_spot_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("owner_node_rid").get_to (value.owner_node_rid);
    json.at ("created").get_to (value.created);
}

inline void to_json (nlohmann::json &json, const direct_spot_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"owner_node_rid", value.owner_node_rid},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, direct_spot_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("owner_node_rid").get_to (value.owner_node_rid);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const direct_spot_msg_t &value)
{
    json = nlohmann::json{{"source_actor_id", value.source_actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, direct_spot_msg_t &value)
{
    json.at ("source_actor_id").get_to (value.source_actor_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const spot_state_command_route_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_state_command_route_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_state_command_route_res_t &value)
{
    json = nlohmann::json{{"accepted", value.accepted}};
}

inline void from_json (const nlohmann::json &json, spot_state_command_route_res_t &value)
{
    json.at ("accepted").get_to (value.accepted);
}

inline void to_json (nlohmann::json &json, const spot_publish_route_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_publish_route_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_publish_route_res_t &value)
{
    json = nlohmann::json{{"accepted", value.accepted}};
}

inline void from_json (const nlohmann::json &json, spot_publish_route_res_t &value)
{
    json.at ("accepted").get_to (value.accepted);
}

inline void to_json (nlohmann::json &json, const slow_spot_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, slow_spot_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const spot_slow_route_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"value", value.value},
                          {"timeout_ms", value.timeout_ms}};
}

inline void from_json (const nlohmann::json &json, spot_slow_route_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("value").get_to (value.value);
    json.at ("timeout_ms").get_to (value.timeout_ms);
}

inline void to_json (nlohmann::json &json, const spot_slow_route_res_t &value)
{
    json = nlohmann::json{{"timed_out", value.timed_out}};
}

inline void from_json (const nlohmann::json &json, spot_slow_route_res_t &value)
{
    json.at ("timed_out").get_to (value.timed_out);
}

inline void to_json (nlohmann::json &json, const unhandled_spot_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, unhandled_spot_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const unhandled_spot_msg_t &value)
{
    to_json (json, static_cast<const unhandled_spot_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, unhandled_spot_msg_t &value)
{
    from_json (json, static_cast<unhandled_spot_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const spot_missing_route_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"spot_id", value.spot_id},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, spot_missing_route_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const spot_missing_route_res_t &value)
{
    json = nlohmann::json{{"request_failed", value.request_failed},
                          {"command_sent", value.command_sent}};
}

inline void from_json (const nlohmann::json &json, spot_missing_route_res_t &value)
{
    json.at ("request_failed").get_to (value.request_failed);
    json.at ("command_sent").get_to (value.command_sent);
}

inline void to_json (nlohmann::json &json, const spot_missing_handler_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, spot_missing_handler_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
}

inline void to_json (nlohmann::json &json, const spot_missing_command_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_missing_command_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_missing_target_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, spot_missing_target_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
}

inline void to_json (nlohmann::json &json, const spot_missing_target_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"failed", value.failed}};
}

inline void from_json (const nlohmann::json &json, spot_missing_target_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("failed").get_to (value.failed);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_req_t &value)
{
    json = nlohmann::json{{"target_key", value.target_key}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_req_t &value)
{
    json.at ("target_key").get_to (value.target_key);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_res_t &value)
{
    json = nlohmann::json{{"request_reply", value.request_reply},
                          {"command_sent", value.command_sent},
                          {"published", value.published},
                          {"missing_rejected", value.missing_rejected},
                          {"timeout_rejected", value.timeout_rejected}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_res_t &value)
{
    json.at ("request_reply").get_to (value.request_reply);
    json.at ("command_sent").get_to (value.command_sent);
    json.at ("published").get_to (value.published);
    json.at ("missing_rejected").get_to (value.missing_rejected);
    json.at ("timeout_rejected").get_to (value.timeout_rejected);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_route_req_t &value)
{
    json = nlohmann::json{{"source_node_rid", value.source_node_rid},
                          {"source_spot_id", value.source_spot_id},
                          {"target_node_rid", value.target_node_rid},
                          {"target_spot_id", value.target_spot_id},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_route_req_t &value)
{
    json.at ("source_node_rid").get_to (value.source_node_rid);
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_direct_req_t &value)
{
    to_json (json, static_cast<const spot_to_spot_route_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, spot_to_spot_direct_req_t &value)
{
    from_json (json, static_cast<spot_to_spot_route_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const spot_to_spot_timeout_req_t &value)
{
    to_json (json, static_cast<const spot_to_spot_route_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, spot_to_spot_timeout_req_t &value)
{
    from_json (json, static_cast<spot_to_spot_route_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const spot_to_spot_negative_req_t &value)
{
    to_json (json, static_cast<const spot_to_spot_route_req_t &> (value));
}

inline void from_json (const nlohmann::json &json, spot_to_spot_negative_req_t &value)
{
    from_json (json, static_cast<spot_to_spot_route_req_t &> (value));
}

inline void to_json (nlohmann::json &json, const spot_to_spot_route_res_t &value)
{
    json = nlohmann::json{{"source_spot_id", value.source_spot_id},
                          {"target_spot_id", value.target_spot_id},
                          {"target_value", value.target_value}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_route_res_t &value)
{
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("target_value").get_to (value.target_value);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_timeout_route_res_t &value)
{
    json = nlohmann::json{{"source_spot_id", value.source_spot_id},
                          {"target_spot_id", value.target_spot_id},
                          {"failed", value.failed}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_timeout_route_res_t &value)
{
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("failed").get_to (value.failed);
}

inline void to_json (nlohmann::json &json, const spot_to_spot_negative_route_res_t &value)
{
    json = nlohmann::json{{"source_spot_id", value.source_spot_id},
                          {"target_spot_id", value.target_spot_id},
                          {"request_failed", value.request_failed}};
}

inline void from_json (const nlohmann::json &json, spot_to_spot_negative_route_res_t &value)
{
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("request_failed").get_to (value.request_failed);
}

inline void to_json (nlohmann::json &json, const type_mismatch_req_t &value)
{
    json = nlohmann::json{{"probe", value.probe}};
}

inline void from_json (const nlohmann::json &json, type_mismatch_req_t &value)
{
    json.at ("probe").get_to (value.probe);
}

inline void to_json (nlohmann::json &json, const type_mismatch_res_t &value)
{
    json = nlohmann::json{{"rejected", value.rejected},
                          {"error_kind", value.error_kind},
                          {"spot_name", value.spot_name},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, type_mismatch_res_t &value)
{
    json.at ("rejected").get_to (value.rejected);
    json.at ("error_kind").get_to (value.error_kind);
    json.at ("spot_name").get_to (value.spot_name);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const lifecycle_req_t &value)
{
    json = nlohmann::json{{"key", value.key}};
}

inline void from_json (const nlohmann::json &json, lifecycle_req_t &value)
{
    json.at ("key").get_to (value.key);
}

inline void to_json (nlohmann::json &json, const lifecycle_res_t &value)
{
    json = nlohmann::json{
      {"spot_id", value.spot_id}, {"created", value.created}, {"closed", value.closed}};
}

inline void from_json (const nlohmann::json &json, lifecycle_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("created").get_to (value.created);
    json.at ("closed").get_to (value.closed);
}

inline void to_json (nlohmann::json &json, const close_spot_req_t &value)
{
    json = nlohmann::json{{"key", value.key}};
}

inline void from_json (const nlohmann::json &json, close_spot_req_t &value)
{
    json.at ("key").get_to (value.key);
}

inline void to_json (nlohmann::json &json, const close_spot_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"closed", value.closed}};
}

inline void from_json (const nlohmann::json &json, close_spot_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("closed").get_to (value.closed);
}

inline void to_json (nlohmann::json &json, const idle_close_msg_t &value)
{
    json = nlohmann::json{{"name", value.name}, {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, idle_close_msg_t &value)
{
    json.at ("name").get_to (value.name);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const spot_idle_close_req_t &value)
{
    json = nlohmann::json{
      {"spot_id", value.spot_id}, {"name", value.name}, {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, spot_idle_close_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const overrun_timer_msg_t &value)
{
    json = nlohmann::json{
      {"name", value.name}, {"policy", value.policy}, {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, overrun_timer_msg_t &value)
{
    json.at ("name").get_to (value.name);
    json.at ("policy").get_to (value.policy);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const spot_overrun_start_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"name", value.name},
                          {"policy", value.policy},
                          {"period_ms", value.period_ms}};
}

inline void from_json (const nlohmann::json &json, spot_overrun_start_req_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("policy").get_to (value.policy);
    json.at ("period_ms").get_to (value.period_ms);
}

inline void to_json (nlohmann::json &json, const stream_auth_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"actor_id", value.actor_id},
                          {"display_name", value.display_name},
                          {"actor", value.actor}};
}

inline void from_json (const nlohmann::json &json, stream_auth_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
    json.at ("actor").get_to (value.actor);
}

inline void to_json (nlohmann::json &json, const stream_ensure_auth_req_t &value)
{
    json = nlohmann::json{{"target_node_rid", value.target_node_rid},
                          {"actor_id", value.actor_id},
                          {"display_name", value.display_name}};
}

inline void from_json (const nlohmann::json &json, stream_ensure_auth_req_t &value)
{
    json.at ("target_node_rid").get_to (value.target_node_rid);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("display_name").get_to (value.display_name);
}

inline void to_json (nlohmann::json &json, const stream_auth_res_t &value)
{
    json = nlohmann::json{{"actor", value.actor}, {"session_node_rid", value.session_node_rid}};
}

inline void from_json (const nlohmann::json &json, stream_auth_res_t &value)
{
    json.at ("actor").get_to (value.actor);
    json.at ("session_node_rid").get_to (value.session_node_rid);
}

inline void to_json (nlohmann::json &json, const actor_push_req_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_push_req_t &value)
{
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const actor_push_by_actor_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_push_by_actor_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const actor_push_by_actor_res_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"value", value.value},
                          {"delivered", value.delivered},
                          {"error_kind", value.error_kind}};
}

inline void from_json (const nlohmann::json &json, actor_push_by_actor_res_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("value").get_to (value.value);
    json.at ("delivered").get_to (value.delivered);
    if (json.contains ("error_kind")) {
        json.at ("error_kind").get_to (value.error_kind);
    }
}

inline void to_json (nlohmann::json &json, const snapshot_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, snapshot_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
}

inline void to_json (nlohmann::json &json, const snapshot_res_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"seen", value.seen}};
}

inline void from_json (const nlohmann::json &json, snapshot_res_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("seen").get_to (value.seen);
}

inline void to_json (nlohmann::json &json, const bound_session_push_req_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id}, {"push", value.push}};
}

inline void from_json (const nlohmann::json &json, bound_session_push_req_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("push").get_to (value.push);
}

inline void to_json (nlohmann::json &json, const actor_push_res_t &value)
{
    json =
      nlohmann::json{{"pushed", value.pushed}, {"actor_id", value.actor_id}, {"seen", value.seen}};
}

inline void from_json (const nlohmann::json &json, actor_push_res_t &value)
{
    json.at ("pushed").get_to (value.pushed);
    json.at ("actor_id").get_to (value.actor_id);
    if (json.contains ("seen")) {
        json.at ("seen").get_to (value.seen);
    }
}

template <typename T> inline zlink::message_t to_stream_payload (const T &value)
{
    return zlink::message_t::from_json (value);
}

template <typename T>
inline void from_stream_payload (const zlink::message_t &payload, T &value)
{
    value = payload.parse_json<T> ();
}

inline void to_json (nlohmann::json &json, const actor_push_notify_t &value)
{
    json = nlohmann::json{
      {"actor_id", value.actor_id}, {"value", value.value}, {"seen", value.seen}};
}

inline std::string to_stream_payload (const actor_push_notify_t &value)
{
    return nlohmann::json (value).dump ();
}

inline void from_stream_payload (const zlink::message_t &payload, actor_push_notify_t &value)
{
    const auto json = nlohmann::json::parse (payload.to_string ());
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("value").get_to (value.value);
    if (json.contains ("seen")) {
        json.at ("seen").get_to (value.seen);
    }
}

inline void from_json (const nlohmann::json &json, actor_push_notify_t &value)
{
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("value").get_to (value.value);
    if (json.contains ("seen")) {
        json.at ("seen").get_to (value.seen);
    }
}

inline void to_json (nlohmann::json &json, const spot_only_mesh_req_t &value)
{
    json = nlohmann::json{{"source_spot_id", value.source_spot_id},
                          {"target_spot_id", value.target_spot_id},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_only_mesh_req_t &value)
{
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_only_mesh_res_t &value)
{
    json = nlohmann::json{{"source_spot_id", value.source_spot_id},
                          {"target_spot_id", value.target_spot_id},
                          {"target_value", value.target_value},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_only_mesh_res_t &value)
{
    json.at ("source_spot_id").get_to (value.source_spot_id);
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("target_value").get_to (value.target_value);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_only_join_req_t &value)
{
    json = nlohmann::json{{"target_spot_id", value.target_spot_id},
                          {"actor_id", value.actor_id},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_only_join_req_t &value)
{
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const spot_only_join_res_t &value)
{
    json = nlohmann::json{{"target_spot_id", value.target_spot_id},
                          {"actor_id", value.actor_id},
                          {"accepted", value.accepted},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, spot_only_join_res_t &value)
{
    json.at ("target_spot_id").get_to (value.target_spot_id);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("accepted").get_to (value.accepted);
    json.at ("marker").get_to (value.marker);
}

inline void to_json (nlohmann::json &json, const evidence_entry_t &value)
{
    json = nlohmann::json{{"marker", value.marker},
                          {"node_rid", value.node_rid},
                          {"actor_id", value.actor_id},
                          {"spot_id", value.spot_id},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, evidence_entry_t &value)
{
    json.at ("marker").get_to (value.marker);
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("actor_id").get_to (value.actor_id);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("value").get_to (value.value);
}

inline void to_json (nlohmann::json &json, const evidence_snapshot_t &value)
{
    json = nlohmann::json{{"node_rid", value.node_rid}, {"entries", value.entries}};
}

inline void from_json (const nlohmann::json &json, evidence_snapshot_t &value)
{
    json.at ("node_rid").get_to (value.node_rid);
    json.at ("entries").get_to (value.entries);
}

inline void to_json (nlohmann::json &json, const spot_stage_timer_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"name", value.name},
                          {"started", value.started},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_stage_timer_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("started").get_to (value.started);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_worker_complete_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"marker", value.marker},
                          {"completed", value.completed},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_worker_complete_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("completed").get_to (value.completed);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_idle_close_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"name", value.name},
                          {"closed", value.closed},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_idle_close_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("closed").get_to (value.closed);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_overrun_start_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"name", value.name},
                          {"started", value.started},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_overrun_start_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("name").get_to (value.name);
    json.at ("started").get_to (value.started);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_publish_observe_res_t &value)
{
    json = nlohmann::json{{"operation", value.operation},
                          {"spot_id", value.spot_id},
                          {"marker", value.marker},
                          {"received", value.received},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_publish_observe_res_t &value)
{
    json.at ("operation").get_to (value.operation);
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("received").get_to (value.received);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_missing_handler_res_t &value)
{
    json = nlohmann::json{
      {"spot_id", value.spot_id}, {"failed", value.failed}, {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_missing_handler_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("failed").get_to (value.failed);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const spot_missing_command_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id},
                          {"marker", value.marker},
                          {"sent", value.sent},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, spot_missing_command_res_t &value)
{
    json.at ("spot_id").get_to (value.spot_id);
    json.at ("marker").get_to (value.marker);
    json.at ("sent").get_to (value.sent);
    json.at ("evidence").get_to (value.evidence);
}

inline void to_json (nlohmann::json &json, const evidence_wait_req_t &value)
{
    json = nlohmann::json{{"contains_all", value.contains_all},
                          {"timeout_milliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, evidence_wait_req_t &value)
{
    if (json.contains ("contains_all")) {
        json.at ("contains_all").get_to (value.contains_all);
    }
    if (json.contains ("timeout_milliseconds")) {
        json.at ("timeout_milliseconds").get_to (value.timeout_milliseconds);
    }
}

} // namespace zlink::framework::e2e::spot_service

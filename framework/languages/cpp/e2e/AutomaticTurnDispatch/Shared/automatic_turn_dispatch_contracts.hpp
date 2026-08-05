/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <zlink/framework/codecs/json.hpp>

#include <nlohmann/json.hpp>

namespace zlink::stream_connector
{
enum class codec_t : std::uint8_t;
}

namespace zlink::framework::e2e::automatic_turn_dispatch
{

inline constexpr const char *control_channel = "await.control";
inline constexpr const char *delay_channel = "await.delay";
inline constexpr const char *spot_channel = "await.spot";
inline constexpr const char *spot_route_channel = "await.spot.route";
inline constexpr const char *stream_node = "await.stream";
inline constexpr const char *actor_type = "await.actor";
inline constexpr const char *handler_group = "automatic-turn-dispatch";
inline constexpr const char *actor_id_metadata = "actor-id";
inline constexpr const char *spot_id_metadata = "spot-rid";
inline constexpr const char *target_node_rid_metadata = "target-node-rid";
inline constexpr const char *probe_spot_name = "YieldProbeSpot";

struct ensure_spot_req_t
{
    static constexpr const char *packet_name = "EnsureSpotReq";
    std::string spot_id;
};

struct ensure_spot_res_t
{
    static constexpr const char *packet_name = "EnsureSpotRes";
    std::string spot_id;
    std::string node_rid;
};

struct await_evidence_req_t
{
    static constexpr const char *packet_name = "YieldEvidenceReq";
    std::string request_id;
};

struct await_evidence_res_t
{
    static constexpr const char *packet_name = "YieldEvidenceRes";
    std::string request_id;
    std::vector<std::string> evidence;
};

struct await_evidence_wait_req_t
{
    static constexpr const char *packet_name = "YieldEvidenceWaitReq";
    std::string request_id;
    std::string marker;
    int timeout_milliseconds = 3000;
};

struct await_shutdown_scenario_req_t
{
    static constexpr const char *packet_name = "YieldShutdownScenarioReq";
    std::string request_id;
    std::string spot_id;
    int delay_ms = 0;
};

struct await_shutdown_recovery_req_t
{
    static constexpr const char *packet_name = "YieldShutdownRecoveryReq";
    std::string request_id;
    std::string spot_id;
};

struct delay_req_t
{
    static constexpr const char *packet_name = "DelayReq";
    std::string request_id;
    int delay_ms = 0;
    std::string marker;
};

struct delay_res_t
{
    static constexpr const char *packet_name = "DelayRes";
    std::string request_id;
    std::string marker;
    std::string node_rid;
};

struct await_actor_binding_t
{
    std::string actor_id;
    std::string node_rid;
    std::uint64_t generation = 0;
};

struct bind_await_actors_req_t
{
    static constexpr const char *packet_name = "BindYieldActorsReq";
    std::string spot_id;
    std::vector<std::string> actor_ids;
};

struct bind_await_actors_res_t
{
    static constexpr const char *packet_name = "BindYieldActorsRes";
    std::string spot_id;
    std::vector<await_actor_binding_t> actors;
};

struct hold_req_t
{
    static constexpr const char *packet_name = "HoldReq";
    std::string request_id;
    int delay_ms = 0;
};

struct hold_msg_t
{
    static constexpr const char *packet_name = "HoldMsg";
    std::string request_id;
    int delay_ms = 0;
};

struct await_req_t
{
    static constexpr const char *packet_name = "YieldReq";
    std::string request_id;
    int delay_ms = 0;
    std::string correlation_id;
};

struct await_msg_t
{
    static constexpr const char *packet_name = "YieldMsg";
    std::string request_id;
    int delay_ms = 0;
    std::string correlation_id;
};

struct worker_await_req_t
{
    static constexpr const char *packet_name = "WorkerYieldReq";
    std::string request_id;
    int delay_ms = 0;
};

struct worker_await_msg_t
{
    static constexpr const char *packet_name = "WorkerYieldMsg";
    std::string request_id;
    int delay_ms = 0;
};

struct http_await_msg_t
{
    static constexpr const char *packet_name = "HttpAwaitMsg";
    std::string request_id;
    int delay_ms = 0;
    std::string terminator;
};

struct io_worker_await_msg_t
{
    static constexpr const char *packet_name = "IoWorkerAwaitMsg";
    std::string request_id;
    std::string operation_id;
    int delay_ms = 0;
};

struct external_delay_res_t
{
    std::string request_id;
    std::string marker;
};

struct await_timeout_req_t
{
    static constexpr const char *packet_name = "YieldTimeoutReq";
    std::string request_id;
    int delay_ms = 0;
    int timeout_ms = 0;
};

struct await_timeout_msg_t
{
    static constexpr const char *packet_name = "YieldTimeoutMsg";
    std::string request_id;
    int delay_ms = 0;
    int timeout_ms = 0;
};

struct remote_spot_await_req_t
{
    static constexpr const char *packet_name = "RemoteSpotYieldReq";
    std::string request_id;
    std::string target_spot_id;
    int delay_ms = 0;
};

struct timer_start_msg_t
{
    static constexpr const char *packet_name = "TimerStartMsg";
    std::string request_id;
    std::string timer_name;
    std::string mode;
    int period_ms = 0;
    int delay_ms = 0;
};

struct timer_stop_msg_t
{
    static constexpr const char *packet_name = "TimerStopMsg";
    std::string request_id;
};

struct probe_req_t
{
    static constexpr const char *packet_name = "ProbeReq";
    std::string request_id;
    std::string marker;
};

struct probe_msg_t
{
    static constexpr const char *packet_name = "ProbeMsg";
    std::string request_id;
    std::string marker;
};

struct actor_await_req_t
{
    static constexpr const char *packet_name = "ActorYieldReq";
    std::string request_id;
    int delay_ms = 0;
};

struct actor_fast_req_t
{
    static constexpr const char *packet_name = "ActorFastReq";
    std::string request_id;
    std::string marker;
};

struct actor_join_await_req_t
{
    static constexpr const char *packet_name = "ActorJoinYieldReq";
    std::string request_id;
    std::string target_node_rid;
};

struct actor_join_spot_req_t
{
    static constexpr const char *packet_name = "ActorJoinSpotReq";
    std::string request_id;
    std::string target_spot_id;
    int admission_delay_ms = 0;
};

struct actor_push_await_req_t
{
    static constexpr const char *packet_name = "ActorPushYieldReq";
    std::string request_id;
    int delay_ms = 0;
    std::string value;
};

struct actor_push_notify_t
{
    static constexpr const char *packet_name = "ActorPushNotify";
    std::string actor_id;
    std::string request_id;
    std::string value;
    std::string node_rid;
};

struct actor_await_res_t
{
    static constexpr const char *packet_name = "ActorYieldRes";
    std::string scenario_id;
    std::string request_id;
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    std::string marker;
};

struct automatic_turn_dispatch_res_t
{
    static constexpr const char *packet_name = "AutomaticTurnDispatchRes";
    std::string scenario_id;
    std::string request_id;
    std::string spot_id;
    std::string node_rid;
    std::string marker;
};

struct await_timeout_res_t
{
    static constexpr const char *packet_name = "YieldTimeoutRes";
    std::string scenario_id;
    std::string request_id;
    std::string spot_id;
    std::string node_rid;
    bool timed_out = false;
    std::string error;
};

struct await_scenario_res_t
{
    static constexpr const char *packet_name = "YieldScenarioRes";
    std::string operation;
    std::string spot_id;
    std::vector<std::string> evidence;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch

namespace zlink::framework::e2e::automatic_turn_dispatch
{

inline void to_json (nlohmann::json &json, const ensure_spot_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, ensure_spot_req_t &value)
{
    value.spot_id = json.value ("spot_id", "");
}

inline void to_json (nlohmann::json &json, const ensure_spot_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"node_rid", value.node_rid}};
}

inline void from_json (const nlohmann::json &json, ensure_spot_res_t &value)
{
    value.spot_id = json.value ("spot_id", "");
    value.node_rid = json.value ("node_rid", "");
}

inline void to_json (nlohmann::json &json, const await_evidence_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}};
}

inline void from_json (const nlohmann::json &json, await_evidence_req_t &value)
{
    value.request_id = json.value ("request_id", "");
}

inline void to_json (nlohmann::json &json, const await_evidence_res_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, await_evidence_res_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.evidence = json.value ("evidence", std::vector<std::string>{});
}

inline void to_json (nlohmann::json &json, const await_evidence_wait_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"marker", value.marker},
                          {"timeout_milliseconds", value.timeout_milliseconds}};
}

inline void from_json (const nlohmann::json &json, await_evidence_wait_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
    value.timeout_milliseconds = json.value ("timeout_milliseconds", 3000);
}

inline void to_json (nlohmann::json &json, const await_shutdown_scenario_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"spot_id", value.spot_id},
                          {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, await_shutdown_scenario_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.spot_id = json.value ("spot_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const await_shutdown_recovery_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"spot_id", value.spot_id}};
}

inline void from_json (const nlohmann::json &json, await_shutdown_recovery_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.spot_id = json.value ("spot_id", "");
}

inline void to_json (nlohmann::json &json, const delay_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, delay_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const delay_res_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"marker", value.marker},
                          {"node_rid", value.node_rid}};
}

inline void from_json (const nlohmann::json &json, delay_res_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
    value.node_rid = json.value ("node_rid", "");
}

inline void to_json (nlohmann::json &json, const await_actor_binding_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"node_rid", value.node_rid},
                          {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, await_actor_binding_t &value)
{
    value.actor_id = json.value ("actor_id", "");
    value.node_rid = json.value ("node_rid", "");
    value.generation = json.value ("generation", std::uint64_t{0});
}

inline void to_json (nlohmann::json &json, const bind_await_actors_req_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"actor_ids", value.actor_ids}};
}

inline void from_json (const nlohmann::json &json, bind_await_actors_req_t &value)
{
    value.spot_id = json.value ("spot_id", "");
    value.actor_ids = json.value ("actor_ids", std::vector<std::string>{});
}

inline void to_json (nlohmann::json &json, const bind_await_actors_res_t &value)
{
    json = nlohmann::json{{"spot_id", value.spot_id}, {"actors", value.actors}};
}

inline void from_json (const nlohmann::json &json, bind_await_actors_res_t &value)
{
    value.spot_id = json.value ("spot_id", "");
    value.actors = json.value ("actors", std::vector<await_actor_binding_t>{});
}

inline void to_json (nlohmann::json &json, const actor_join_spot_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"target_spot_id", value.target_spot_id},
                          {"admission_delay_ms", value.admission_delay_ms}};
}

inline void from_json (const nlohmann::json &json, actor_join_spot_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.target_spot_id = json.value ("target_spot_id", "");
    value.admission_delay_ms = json.value ("admission_delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const hold_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, hold_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const hold_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, hold_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"correlation_id", value.correlation_id}};
}

inline void from_json (const nlohmann::json &json, await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.correlation_id = json.value ("correlation_id", "");
}

inline void to_json (nlohmann::json &json, const await_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"correlation_id", value.correlation_id}};
}

inline void from_json (const nlohmann::json &json, await_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.correlation_id = json.value ("correlation_id", "");
}

inline void to_json (nlohmann::json &json, const worker_await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, worker_await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const worker_await_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, worker_await_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const http_await_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"terminator", value.terminator}};
}

inline void from_json (const nlohmann::json &json, http_await_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.terminator = json.value ("terminator", "");
}

inline void to_json (nlohmann::json &json, const io_worker_await_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"operation_id", value.operation_id},
                          {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, io_worker_await_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.operation_id = json.value ("operation_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const external_delay_res_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, external_delay_res_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const await_timeout_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"timeout_ms", value.timeout_ms}};
}

inline void from_json (const nlohmann::json &json, await_timeout_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.timeout_ms = json.value ("timeout_ms", 0);
}

inline void to_json (nlohmann::json &json, const await_timeout_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"timeout_ms", value.timeout_ms}};
}

inline void from_json (const nlohmann::json &json, await_timeout_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.timeout_ms = json.value ("timeout_ms", 0);
}

inline void to_json (nlohmann::json &json, const remote_spot_await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"target_spot_id", value.target_spot_id},
                          {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, remote_spot_await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.target_spot_id = json.value ("target_spot_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const timer_start_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"timer_name", value.timer_name},
                          {"mode", value.mode},
                          {"period_ms", value.period_ms},
                          {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, timer_start_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.timer_name = json.value ("timer_name", "");
    value.mode = json.value ("mode", "");
    value.period_ms = json.value ("period_ms", 0);
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const timer_stop_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}};
}

inline void from_json (const nlohmann::json &json, timer_stop_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
}

inline void to_json (nlohmann::json &json, const probe_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, probe_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const probe_msg_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, probe_msg_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const actor_await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"delay_ms", value.delay_ms}};
}

inline void from_json (const nlohmann::json &json, actor_await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
}

inline void to_json (nlohmann::json &json, const actor_fast_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id}, {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, actor_fast_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const actor_join_await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"target_node_rid", value.target_node_rid}};
}

inline void from_json (const nlohmann::json &json, actor_join_await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.target_node_rid = json.value ("target_node_rid", "");
}

inline void to_json (nlohmann::json &json, const actor_push_await_req_t &value)
{
    json = nlohmann::json{{"request_id", value.request_id},
                          {"delay_ms", value.delay_ms},
                          {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_push_await_req_t &value)
{
    value.request_id = json.value ("request_id", "");
    value.delay_ms = json.value ("delay_ms", 0);
    value.value = json.value ("value", "");
}

inline zlink::message_t to_stream_payload (const actor_push_await_req_t &value)
{
    return zlink::message_t::from_json (value);
}

inline void from_stream_payload (const zlink::message_t &payload, actor_push_await_req_t &value)
{
    value = payload.parse_json<actor_push_await_req_t> ();
}

inline void from_stream_payload (zlink::stream_connector::codec_t,
                                 const zlink::message_t &payload,
                                 actor_push_await_req_t &value)
{
    from_stream_payload (payload, value);
}

inline void to_json (nlohmann::json &json, const actor_push_notify_t &value)
{
    json = nlohmann::json{{"actor_id", value.actor_id},
                          {"request_id", value.request_id},
                          {"value", value.value},
                          {"node_rid", value.node_rid}};
}

inline void from_json (const nlohmann::json &json, actor_push_notify_t &value)
{
    value.actor_id = json.value ("actor_id", "");
    value.request_id = json.value ("request_id", "");
    value.value = json.value ("value", "");
    value.node_rid = json.value ("node_rid", "");
}

inline zlink::message_t to_stream_payload (const actor_push_notify_t &value)
{
    return zlink::message_t::from_json (value);
}

inline void from_stream_payload (const zlink::message_t &payload, actor_push_notify_t &value)
{
    value = payload.parse_json<actor_push_notify_t> ();
}

inline void from_stream_payload (zlink::stream_connector::codec_t,
                                 const zlink::message_t &payload,
                                 actor_push_notify_t &value)
{
    from_stream_payload (payload, value);
}

inline void to_json (nlohmann::json &json, const actor_await_res_t &value)
{
    json = nlohmann::json{{"scenario_id", value.scenario_id},
                          {"request_id", value.request_id},
                          {"actor_id", value.actor_id},
                          {"spot_id", value.spot_id},
                          {"node_rid", value.node_rid},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, actor_await_res_t &value)
{
    value.scenario_id = json.value ("scenario_id", "");
    value.request_id = json.value ("request_id", "");
    value.actor_id = json.value ("actor_id", "");
    value.spot_id = json.value ("spot_id", "");
    value.node_rid = json.value ("node_rid", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const automatic_turn_dispatch_res_t &value)
{
    json = nlohmann::json{{"scenario_id", value.scenario_id},
                          {"request_id", value.request_id},
                          {"spot_id", value.spot_id},
                          {"node_rid", value.node_rid},
                          {"marker", value.marker}};
}

inline void from_json (const nlohmann::json &json, automatic_turn_dispatch_res_t &value)
{
    value.scenario_id = json.value ("scenario_id", "");
    value.request_id = json.value ("request_id", "");
    value.spot_id = json.value ("spot_id", "");
    value.node_rid = json.value ("node_rid", "");
    value.marker = json.value ("marker", "");
}

inline void to_json (nlohmann::json &json, const await_timeout_res_t &value)
{
    json = nlohmann::json{{"scenario_id", value.scenario_id},
                          {"request_id", value.request_id},
                          {"spot_id", value.spot_id},
                          {"node_rid", value.node_rid},
                          {"timed_out", value.timed_out},
                          {"error", value.error}};
}

inline void from_json (const nlohmann::json &json, await_timeout_res_t &value)
{
    value.scenario_id = json.value ("scenario_id", "");
    value.request_id = json.value ("request_id", "");
    value.spot_id = json.value ("spot_id", "");
    value.node_rid = json.value ("node_rid", "");
    value.timed_out = json.value ("timed_out", false);
    value.error = json.value ("error", "");
}

inline void to_json (nlohmann::json &json, const await_scenario_res_t &value)
{
    json = nlohmann::json{{"operation", value.operation},
                          {"spot_id", value.spot_id},
                          {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, await_scenario_res_t &value)
{
    value.operation = json.value ("operation", "");
    value.spot_id = json.value ("spot_id", "");
    value.evidence = json.value ("evidence", std::vector<std::string>{});
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

} // namespace zlink::framework::e2e::automatic_turn_dispatch

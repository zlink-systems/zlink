/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace zlink::e2e::to_actor_messaging
{

inline constexpr const char *spot_mesh_name = "to-actor-messaging";
inline constexpr const char *actor_type_name = "TestActor";

struct actor_notify_t
{
    static constexpr const char *packet_name = "ActorNotify";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct actor_ask_t
{
    static constexpr const char *packet_name = "ActorAsk";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct actor_reply_t
{
    static constexpr const char *packet_name = "ActorReply";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct bind_actor_session_req_t
{
    static constexpr const char *packet_name = "BindActorSessionReq";
    std::string scenario;
    std::string actor_id;
};

struct bind_actor_session_res_t
{
    static constexpr const char *packet_name = "BindActorSessionRes";
    std::string scenario;
    std::string actor_id;
};

struct actor_push_notify_t
{
    static constexpr const char *packet_name = "ActorPushNotify";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct caller_request_t
{
    static constexpr const char *packet_name = "CallerRequest";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct caller_reply_t
{
    static constexpr const char *packet_name = "CallerReply";
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct actor_call_request_t
{
    std::string scenario;
    std::string actor_id;
    std::string value;
};

struct actor_call_response_t
{
    std::string scenario;
    std::string actor_id;
    std::string result;
    std::string error_kind;
};

struct actor_evidence_t
{
    std::string scenario;
    std::string actor_id;
    std::string kind;
    std::string value;
};

inline void to_json (nlohmann::json &json, const actor_notify_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_notify_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const actor_ask_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_ask_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const actor_reply_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_reply_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const bind_actor_session_req_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, bind_actor_session_req_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
}

inline void to_json (nlohmann::json &json, const bind_actor_session_res_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, bind_actor_session_res_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
}

inline void to_json (nlohmann::json &json, const actor_push_notify_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_push_notify_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.value ("actorId", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const caller_request_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, caller_request_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const caller_reply_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, caller_reply_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const actor_call_request_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_call_request_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const actor_call_response_t &value)
{
    json = {{"scenario", value.scenario}, {"actorId", value.actor_id}, {"result", value.result}};
    if (!value.error_kind.empty ()) {
        json["errorKind"] = value.error_kind;
    }
}

inline void from_json (const nlohmann::json &json, actor_call_response_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.result = json.value ("result", "");
    value.error_kind = json.contains ("errorKind") ? json.value ("errorKind", "")
                                                   : json.value ("error_kind", "");
}

inline void to_json (nlohmann::json &json, const actor_evidence_t &value)
{
    json = {{"scenario", value.scenario},
            {"actorId", value.actor_id},
            {"kind", value.kind},
            {"value", value.value}};
}

inline void from_json (const nlohmann::json &json, actor_evidence_t &value)
{
    value.scenario = json.value ("scenario", "");
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", "")
                                                : json.value ("actor_id", "");
    value.kind = json.value ("kind", "");
    value.value = json.value ("value", "");
}

template<typename T>
zlink::message_t to_stream_payload (const T &value)
{
    return zlink::message_t::from_json (value);
}

template<typename T>
void from_stream_payload (const zlink::message_t &payload, T &value)
{
    value = payload.parse_json<T> ();
}

} // namespace zlink::e2e::to_actor_messaging

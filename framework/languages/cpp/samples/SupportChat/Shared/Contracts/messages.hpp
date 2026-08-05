/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::samples::supportchat
{

using zlink::framework::actor_id_t;
using zlink::framework::actor_ref_t;
using zlink::framework::node_rid_t;

struct actor_location_t
{
    node_rid_t node_rid;
    std::string actor_id;
    std::uint64_t generation = 0;

    static actor_location_t from (const actor_ref_t &actor_ref)
    {
        return actor_location_t{actor_ref.node_rid (),
                                std::string (actor_ref.actor_id ().value ()),
                                actor_ref.object_generation ()};
    }

    actor_ref_t to_actor_ref (std::string mesh_name) const
    {
        return actor_ref_t (actor_id_t (actor_id), generation, std::move (mesh_name), node_rid);
    }
};

inline void to_json (nlohmann::json &json, const actor_location_t &value)
{
    json = {{"nodeRid", std::string (value.node_rid.value ())},
            {"actorId", value.actor_id},
            {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, actor_location_t &value)
{
    const auto node_rid = json.contains ("nodeRid") ? json.value ("nodeRid", std::string{})
                                                    : json.value ("node_rid", std::string{});
    value.node_rid = node_rid_t::from_string (node_rid);
    value.actor_id = json.contains ("actorId") ? json.value ("actorId", std::string{})
                                               : json.value ("actor_id", std::string{});
    value.generation = json.value ("generation", std::uint64_t{0});
}

struct role_t
{
    static constexpr const char *customer = "Customer";
    static constexpr const char *agent = "Agent";
};

struct conversation_status_t
{
    static constexpr const char *waiting_for_agent = "WaitingForAgent";
    static constexpr const char *active = "Active";
    static constexpr const char *waiting_for_close = "WaitingForClose";
    static constexpr const char *closed = "Closed";
};

struct chat_message_t
{
    std::string conversation_id;
    std::uint64_t message_seq{0};
    std::string sender_actor_id;
    std::string text;
    std::int64_t sent_at_unix_ms{0};
};

struct conversation_state_t
{
    std::string conversation_id;
    std::string subject;
    std::string status;
    std::string customer_actor_id;
    std::optional<std::string> agent_actor_id;
    std::uint64_t last_message_seq{0};
    std::optional<std::int64_t> last_message_at_unix_ms;
    std::optional<std::int64_t> idle_deadline_unix_ms;
};

struct authenticate_req_t
{
    static constexpr const char *packet_name = "AuthenticateReq";
    std::string access_token;
};

struct authenticate_res_t
{
    static constexpr const char *packet_name = "AuthenticateRes";
    std::string actor_id;
    std::string display_name;
    std::string role;
};

/* Internal role-to-role messages. They create and locate application actors and conversation
 * state; they are not messages that a sample user sends through the public client contract. */
struct authenticate_user_req_t
{
    static constexpr const char *packet_name = "AuthenticateUserReq";
    std::string access_token;
};

struct authenticate_user_res_t
{
    static constexpr const char *packet_name = "AuthenticateUserRes";
    bool accepted{false};
    std::optional<std::string> actor_id;
    std::optional<std::string> display_name;
    std::optional<std::string> role;
    std::optional<std::string> reason;
};

struct open_conversation_api_req_t
{
    static constexpr const char *packet_name = "OpenConversationApiReq";
    std::string customer_actor_id;
    std::string customer_display_name;
    std::string subject;
};

struct open_conversation_api_res_t
{
    static constexpr const char *packet_name = "OpenConversationApiRes";
    conversation_state_t state;
};

struct conversation_create_req_t
{
    std::string customer_actor_id;
    std::string customer_display_name;
    std::string subject;
    std::int64_t created_at_unix_ms{0};
};

struct conversation_create_res_t
{
    conversation_state_t state;
};

struct ensure_support_user_actor_req_t
{
    static constexpr const char *packet_name = "EnsureSupportUserActorReq";
    std::string actor_id;
    std::string display_name;
    std::string role;
    std::string participant_id;
};

struct ensure_support_user_actor_res_t
{
    static constexpr const char *packet_name = "EnsureSupportUserActorRes";
    actor_location_t actor;
};

struct ensure_agent_conversation_req_t
{
    static constexpr const char *packet_name = "EnsureAgentConversationReq";
    std::string roster_actor_id;
    std::string display_name;
    std::string conversation_id;
};

struct ensure_agent_conversation_res_t
{
    static constexpr const char *packet_name = "EnsureAgentConversationRes";
    actor_location_t actor;
    bool scheduled{false};
    conversation_state_t state;
};

struct open_conversation_req_t
{
    static constexpr const char *packet_name = "OpenConversationReq";
    std::string subject;
};

struct open_conversation_res_t
{
    static constexpr const char *packet_name = "OpenConversationRes";
    std::string conversation_id;
    conversation_state_t state;
};

struct set_agent_available_req_t
{
    static constexpr const char *packet_name = "SetAgentAvailableReq";
    bool is_available{false};
};

struct set_agent_available_res_t
{
    static constexpr const char *packet_name = "SetAgentAvailableRes";
    bool is_available{false};
};

struct join_conversation_req_t
{
    static constexpr const char *packet_name = "JoinConversationReq";
    std::string participant_id;
    std::string role;
    std::string display_name;
};

struct join_conversation_res_t
{
    static constexpr const char *packet_name = "JoinConversationRes";
    bool scheduled{false};
    conversation_state_t state;
};

struct join_conversation_failed_notify_t
{
    static constexpr const char *packet_name = "JoinConversationFailedNotify";
    std::string conversation_id;
    std::string error;
};

struct send_chat_message_req_t
{
    static constexpr const char *packet_name = "SendChatMessageReq";
    std::string text;
};

struct send_chat_message_res_t
{
    static constexpr const char *packet_name = "SendChatMessageRes";
    chat_message_t message;
    conversation_state_t state;
};

struct set_typing_msg_t
{
    static constexpr const char *packet_name = "SetTypingMsg";
    bool is_typing{false};
};

struct close_conversation_req_t
{
    static constexpr const char *packet_name = "CloseConversationReq";
    std::optional<std::string> reason;
};

struct close_conversation_res_t
{
    static constexpr const char *packet_name = "CloseConversationRes";
    conversation_state_t state;
};

struct participant_joined_notify_t
{
    static constexpr const char *packet_name = "ParticipantJoinedNotify";
    std::string conversation_id;
    std::string actor_id;
    std::string role;
    conversation_state_t state;
};

struct conversation_assigned_notify_t
{
    static constexpr const char *packet_name = "ConversationAssignedNotify";
    std::string conversation_id;
    conversation_state_t state;
};

struct chat_message_notify_t
{
    static constexpr const char *packet_name = "ChatMessageNotify";
    std::string conversation_id;
    chat_message_t message;
    conversation_state_t state;
};

struct typing_changed_notify_t
{
    static constexpr const char *packet_name = "TypingChangedNotify";
    std::string conversation_id;
    std::string actor_id;
    bool is_typing{false};
    conversation_state_t state;
};

struct conversation_idle_notify_t
{
    static constexpr const char *packet_name = "ConversationIdleNotify";
    std::string conversation_id;
    conversation_state_t state;
};

struct conversation_closed_notify_t
{
    static constexpr const char *packet_name = "ConversationClosedNotify";
    std::string conversation_id;
    conversation_state_t state;
};

template <typename T>
inline void set_optional (nlohmann::json &json, const char *name, const std::optional<T> &value)
{
    if (value) {
        json[name] = *value;
    }
}

inline std::optional<std::string>
json_optional_string (const nlohmann::json &json, const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::string> ();
}

inline std::optional<std::int64_t>
json_optional_i64 (const nlohmann::json &json, const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::int64_t> ();
}

inline void to_json (nlohmann::json &json, const chat_message_t &value)
{
    json = {{"conversationId", value.conversation_id},
            {"messageSeq", value.message_seq},
            {"senderActorId", value.sender_actor_id},
            {"text", value.text},
            {"sentAtUnixMs", value.sent_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, chat_message_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.message_seq = json.value ("messageSeq", std::uint64_t{0});
    value.sender_actor_id = json.value ("senderActorId", "");
    value.text = json.value ("text", "");
    value.sent_at_unix_ms = json.value ("sentAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const conversation_state_t &value)
{
    json = {{"conversationId", value.conversation_id},
            {"subject", value.subject},
            {"status", value.status},
            {"customerActorId", value.customer_actor_id},
            {"lastMessageSeq", value.last_message_seq}};
    set_optional (json, "agentActorId", value.agent_actor_id);
    set_optional (json, "lastMessageAtUnixMs", value.last_message_at_unix_ms);
    set_optional (json, "idleDeadlineUnixMs", value.idle_deadline_unix_ms);
}

inline void from_json (const nlohmann::json &json, conversation_state_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.subject = json.value ("subject", "");
    value.status = json.value ("status", "");
    value.customer_actor_id = json.value ("customerActorId", "");
    value.agent_actor_id = json_optional_string (json, "agentActorId");
    value.last_message_seq = json.value ("lastMessageSeq", std::uint64_t{0});
    value.last_message_at_unix_ms = json_optional_i64 (json, "lastMessageAtUnixMs");
    value.idle_deadline_unix_ms = json_optional_i64 (json, "idleDeadlineUnixMs");
}

#define SUPPORTCHAT_JSON_STRING_REQ(type, field_name, json_name) \
inline void to_json (nlohmann::json &json, const type &value) { json = {{json_name, value.field_name}}; } \
inline void from_json (const nlohmann::json &json, type &value) { value.field_name = json.value (json_name, ""); }

SUPPORTCHAT_JSON_STRING_REQ (authenticate_req_t, access_token, "accessToken")
SUPPORTCHAT_JSON_STRING_REQ (authenticate_user_req_t, access_token, "accessToken")
inline void to_json (nlohmann::json &json, const open_conversation_req_t &value)
{
    json = {{"subject", value.subject}};
}
inline void from_json (const nlohmann::json &json, open_conversation_req_t &value)
{
    value.subject = json.value ("subject", "");
}
SUPPORTCHAT_JSON_STRING_REQ (send_chat_message_req_t, text, "text")

#undef SUPPORTCHAT_JSON_STRING_REQ

inline void to_json (nlohmann::json &json, const authenticate_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"role", value.role}};
}

inline void from_json (const nlohmann::json &json, authenticate_res_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.role = json.value ("role", "");
}

inline void to_json (nlohmann::json &json, const authenticate_user_res_t &value)
{
    json = {{"accepted", value.accepted}};
    set_optional (json, "actorId", value.actor_id);
    set_optional (json, "displayName", value.display_name);
    set_optional (json, "role", value.role);
    set_optional (json, "reason", value.reason);
}

inline void from_json (const nlohmann::json &json, authenticate_user_res_t &value)
{
    value.accepted = json.value ("accepted", false);
    value.actor_id = json_optional_string (json, "actorId");
    value.display_name = json_optional_string (json, "displayName");
    value.role = json_optional_string (json, "role");
    value.reason = json_optional_string (json, "reason");
}

inline void to_json (nlohmann::json &json, const open_conversation_api_req_t &value)
{
    json = {{"customerActorId", value.customer_actor_id},
            {"customerDisplayName", value.customer_display_name},
            {"subject", value.subject}};
}

inline void from_json (const nlohmann::json &json, open_conversation_api_req_t &value)
{
    value.customer_actor_id = json.value ("customerActorId", "");
    value.customer_display_name = json.value ("customerDisplayName", "");
    value.subject = json.value ("subject", "");
}

inline void to_json (nlohmann::json &json, const open_conversation_api_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, open_conversation_api_res_t &value)
{
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const conversation_create_req_t &value)
{
    json = {{"customerActorId", value.customer_actor_id},
            {"customerDisplayName", value.customer_display_name},
            {"subject", value.subject},
            {"createdAtUnixMs", value.created_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, conversation_create_req_t &value)
{
    value.customer_actor_id = json.value ("customerActorId", "");
    value.customer_display_name = json.value ("customerDisplayName", "");
    value.subject = json.value ("subject", "");
    value.created_at_unix_ms = json.value ("createdAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const conversation_create_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, conversation_create_res_t &value)
{
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const ensure_support_user_actor_req_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"role", value.role},
            {"participantId", value.participant_id}};
}

inline void from_json (const nlohmann::json &json, ensure_support_user_actor_req_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.role = json.value ("role", "");
    value.participant_id = json.value ("participantId", "");
}

inline void to_json (nlohmann::json &json, const ensure_support_user_actor_res_t &value)
{
    json = {{"actor", value.actor}};
}

inline void from_json (const nlohmann::json &json, ensure_support_user_actor_res_t &value)
{
    value.actor = json.value ("actor", actor_location_t{});
}

inline void to_json (nlohmann::json &json, const ensure_agent_conversation_req_t &value)
{
    json = {{"rosterActorId", value.roster_actor_id},
            {"displayName", value.display_name},
            {"conversationId", value.conversation_id}};
}

inline void from_json (const nlohmann::json &json, ensure_agent_conversation_req_t &value)
{
    value.roster_actor_id = json.value ("rosterActorId", "");
    value.display_name = json.value ("displayName", "");
    value.conversation_id = json.value ("conversationId", "");
}

inline void to_json (nlohmann::json &json, const ensure_agent_conversation_res_t &value)
{
    json = {{"actor", value.actor},
            {"scheduled", value.scheduled},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, ensure_agent_conversation_res_t &value)
{
    value.actor = json.value ("actor", actor_location_t{});
    value.scheduled = json.value ("scheduled", false);
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const open_conversation_res_t &value)
{
    json = {{"conversationId", value.conversation_id}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, open_conversation_res_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const set_agent_available_req_t &value)
{
    json = {{"isAvailable", value.is_available}};
}

inline void from_json (const nlohmann::json &json, set_agent_available_req_t &value)
{
    value.is_available = json.value ("isAvailable", false);
}

inline void to_json (nlohmann::json &json, const set_agent_available_res_t &value)
{
    json = {{"isAvailable", value.is_available}};
}

inline void from_json (const nlohmann::json &json, set_agent_available_res_t &value)
{
    value.is_available = json.value ("isAvailable", false);
}

inline void to_json (nlohmann::json &json, const join_conversation_req_t &value)
{
    json = {{"participantId", value.participant_id},
            {"role", value.role},
            {"displayName", value.display_name}};
}

inline void from_json (const nlohmann::json &json, join_conversation_req_t &value)
{
    value.participant_id = json.value ("participantId", "");
    value.role = json.value ("role", "");
    value.display_name = json.value ("displayName", "");
}

inline void to_json (nlohmann::json &json, const join_conversation_res_t &value)
{
    json = {{"scheduled", value.scheduled}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, join_conversation_res_t &value)
{
    value.scheduled = json.value ("scheduled", false);
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json,
                     const join_conversation_failed_notify_t &value)
{
    json = {{"conversationId", value.conversation_id}, {"error", value.error}};
}

inline void from_json (const nlohmann::json &json,
                       join_conversation_failed_notify_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.error = json.value ("error", "");
}

inline void to_json (nlohmann::json &json, const send_chat_message_res_t &value)
{
    json = {{"message", value.message}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, send_chat_message_res_t &value)
{
    value.message = json.value ("message", chat_message_t{});
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const set_typing_msg_t &value)
{
    json = {{"isTyping", value.is_typing}};
}

inline void from_json (const nlohmann::json &json, set_typing_msg_t &value)
{
    value.is_typing = json.value ("isTyping", false);
}

inline void to_json (nlohmann::json &json, const close_conversation_req_t &value)
{
    json = nlohmann::json::object ();
    set_optional (json, "reason", value.reason);
}

inline void from_json (const nlohmann::json &json, close_conversation_req_t &value)
{
    value.reason = json_optional_string (json, "reason");
}

inline void to_json (nlohmann::json &json, const close_conversation_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, close_conversation_res_t &value)
{
    value.state = json.value ("state", conversation_state_t{});
}

/* 공통 sample spec §12: notify wire shape는 `{ConversationId, State}`다. state를 루트로
 * 펼치지 않는다. */
#define SUPPORTCHAT_NOTIFY_JSON(type) \
inline void to_json (nlohmann::json &json, const type &value) { \
    json = {{"conversationId", value.conversation_id}, {"state", value.state}}; \
} \
inline void from_json (const nlohmann::json &json, type &value) { \
    value.conversation_id = json.value ("conversationId", ""); \
    value.state = json.value ("state", conversation_state_t{}); \
}

SUPPORTCHAT_NOTIFY_JSON (conversation_assigned_notify_t)
SUPPORTCHAT_NOTIFY_JSON (conversation_idle_notify_t)
SUPPORTCHAT_NOTIFY_JSON (conversation_closed_notify_t)

#undef SUPPORTCHAT_NOTIFY_JSON

inline void to_json (nlohmann::json &json, const participant_joined_notify_t &value)
{
    json = {{"conversationId", value.conversation_id},
            {"actorId", value.actor_id},
            {"role", value.role},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, participant_joined_notify_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.actor_id = json.value ("actorId", "");
    value.role = json.value ("role", "");
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const chat_message_notify_t &value)
{
    json = {{"conversationId", value.conversation_id},
            {"message", value.message},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, chat_message_notify_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.message = json.value ("message", chat_message_t{});
    value.state = json.value ("state", conversation_state_t{});
}

inline void to_json (nlohmann::json &json, const typing_changed_notify_t &value)
{
    json = {{"conversationId", value.conversation_id},
            {"actorId", value.actor_id},
            {"isTyping", value.is_typing},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, typing_changed_notify_t &value)
{
    value.conversation_id = json.value ("conversationId", "");
    value.actor_id = json.value ("actorId", "");
    value.is_typing = json.value ("isTyping", false);
    value.state = json.value ("state", conversation_state_t{});
}

/* Test/evidence-only HTTP assertion messages. The runner uses them to inspect server-owned
 * evidence; they are not a public SupportChat application API. */
struct supportchat_server_assertion_req_t
{
};

struct supportchat_server_assertion_res_t
{
    bool ok{false};
    std::vector<std::string> evidence;
};

inline void to_json (nlohmann::json &json, const supportchat_server_assertion_req_t &)
{
    json = nlohmann::json::object ();
}

inline void from_json (const nlohmann::json &, supportchat_server_assertion_req_t &) {}

inline void to_json (nlohmann::json &json, const supportchat_server_assertion_res_t &value)
{
    json = {{"ok", value.ok}, {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, supportchat_server_assertion_res_t &value)
{
    value.ok = json.value ("ok", false);
    value.evidence = json.value ("evidence", std::vector<std::string>{});
}

} // namespace zlink::samples::supportchat

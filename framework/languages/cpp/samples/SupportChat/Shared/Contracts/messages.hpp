/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <zlink/json_profile.hpp>
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
    std::string node_rid;
    std::string actor_id;
    std::uint64_t generation = 0;

    static actor_location_t from (const actor_ref_t &actor_ref)
    {
        return actor_location_t{std::string (actor_ref.node_rid ().value ()),
                                std::string (actor_ref.actor_id ().value ()),
                                actor_ref.object_generation ()};
    }

    actor_ref_t to_actor_ref (std::string mesh_name) const
    {
        return actor_ref_t (actor_id_t (actor_id),
                            generation,
                            std::move (mesh_name),
                            node_rid_t::from_string (node_rid));
    }
};

}

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::actor_location_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("nodeRid", &zlink::samples::supportchat::actor_location_t::node_rid),
      zlink::json::field ("actorId", &zlink::samples::supportchat::actor_location_t::actor_id),
      zlink::json::field ("generation",
                          &zlink::samples::supportchat::actor_location_t::generation)};
};

namespace zlink::samples::supportchat
{

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
    std::string conversation_id;
    std::string participant_id;
    std::string role;
    std::string display_name;
};

struct join_conversation_res_t
{
    static constexpr const char *packet_name = "JoinConversationRes";
    bool scheduled{false};
    std::string actor_id;
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

inline std::optional<std::string> json_optional_string (const nlohmann::json &json,
                                                        const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::string> ();
}

inline std::optional<std::int64_t> json_optional_i64 (const nlohmann::json &json, const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::int64_t> ();
}

}

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::chat_message_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::chat_message_t::conversation_id),
      zlink::json::field ("messageSeq", &zlink::samples::supportchat::chat_message_t::message_seq),
      zlink::json::field ("senderActorId",
                          &zlink::samples::supportchat::chat_message_t::sender_actor_id),
      zlink::json::field ("text", &zlink::samples::supportchat::chat_message_t::text),
      zlink::json::field ("sentAtUnixMs",
                          &zlink::samples::supportchat::chat_message_t::sent_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_state_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::conversation_state_t::conversation_id),
      zlink::json::field ("subject", &zlink::samples::supportchat::conversation_state_t::subject),
      zlink::json::field ("status", &zlink::samples::supportchat::conversation_state_t::status),
      zlink::json::field ("customerActorId",
                          &zlink::samples::supportchat::conversation_state_t::customer_actor_id),
      zlink::json::field ("lastMessageSeq",
                          &zlink::samples::supportchat::conversation_state_t::last_message_seq),
      zlink::json::field ("agentActorId",
                          &zlink::samples::supportchat::conversation_state_t::agent_actor_id),
      zlink::json::field (
        "lastMessageAtUnixMs",
        &zlink::samples::supportchat::conversation_state_t::last_message_at_unix_ms),
      zlink::json::field (
        "idleDeadlineUnixMs",
        &zlink::samples::supportchat::conversation_state_t::idle_deadline_unix_ms)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::authenticate_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "accessToken", &zlink::samples::supportchat::authenticate_req_t::access_token)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::authenticate_user_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "accessToken", &zlink::samples::supportchat::authenticate_user_req_t::access_token)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::open_conversation_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "subject", &zlink::samples::supportchat::open_conversation_req_t::subject)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::send_chat_message_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("text", &zlink::samples::supportchat::send_chat_message_req_t::text)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::authenticate_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("actorId", &zlink::samples::supportchat::authenticate_res_t::actor_id),
      zlink::json::field ("displayName",
                          &zlink::samples::supportchat::authenticate_res_t::display_name),
      zlink::json::field ("role", &zlink::samples::supportchat::authenticate_res_t::role)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::authenticate_user_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("accepted",
                          &zlink::samples::supportchat::authenticate_user_res_t::accepted),
      zlink::json::field ("actorId",
                          &zlink::samples::supportchat::authenticate_user_res_t::actor_id),
      zlink::json::field ("displayName",
                          &zlink::samples::supportchat::authenticate_user_res_t::display_name),
      zlink::json::field ("role", &zlink::samples::supportchat::authenticate_user_res_t::role),
      zlink::json::field ("reason", &zlink::samples::supportchat::authenticate_user_res_t::reason)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::open_conversation_api_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "customerActorId",
        &zlink::samples::supportchat::open_conversation_api_req_t::customer_actor_id),
      zlink::json::field (
        "customerDisplayName",
        &zlink::samples::supportchat::open_conversation_api_req_t::customer_display_name),
      zlink::json::field ("subject",
                          &zlink::samples::supportchat::open_conversation_api_req_t::subject)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::open_conversation_api_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "state", &zlink::samples::supportchat::open_conversation_api_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_create_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "customerActorId",
        &zlink::samples::supportchat::conversation_create_req_t::customer_actor_id),
      zlink::json::field (
        "customerDisplayName",
        &zlink::samples::supportchat::conversation_create_req_t::customer_display_name),
      zlink::json::field ("subject",
                          &zlink::samples::supportchat::conversation_create_req_t::subject),
      zlink::json::field (
        "createdAtUnixMs",
        &zlink::samples::supportchat::conversation_create_req_t::created_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_create_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::supportchat::conversation_create_res_t::state)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::ensure_support_user_actor_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("actorId",
                          &zlink::samples::supportchat::ensure_support_user_actor_req_t::actor_id),
      zlink::json::field (
        "displayName", &zlink::samples::supportchat::ensure_support_user_actor_req_t::display_name),
      zlink::json::field ("role",
                          &zlink::samples::supportchat::ensure_support_user_actor_req_t::role),
      zlink::json::field (
        "participantId",
        &zlink::samples::supportchat::ensure_support_user_actor_req_t::participant_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::ensure_support_user_actor_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "actor", &zlink::samples::supportchat::ensure_support_user_actor_res_t::actor)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::ensure_agent_conversation_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "rosterActorId",
        &zlink::samples::supportchat::ensure_agent_conversation_req_t::roster_actor_id),
      zlink::json::field (
        "displayName", &zlink::samples::supportchat::ensure_agent_conversation_req_t::display_name),
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::ensure_agent_conversation_req_t::conversation_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::ensure_agent_conversation_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "actor", &zlink::samples::supportchat::ensure_agent_conversation_res_t::actor)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::open_conversation_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::open_conversation_res_t::conversation_id),
      zlink::json::field ("state", &zlink::samples::supportchat::open_conversation_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::set_agent_available_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "isAvailable", &zlink::samples::supportchat::set_agent_available_req_t::is_available)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::set_agent_available_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "isAvailable", &zlink::samples::supportchat::set_agent_available_res_t::is_available)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::join_conversation_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::join_conversation_req_t::conversation_id),
      zlink::json::field ("participantId",
                          &zlink::samples::supportchat::join_conversation_req_t::participant_id),
      zlink::json::field ("role", &zlink::samples::supportchat::join_conversation_req_t::role),
      zlink::json::field ("displayName",
                          &zlink::samples::supportchat::join_conversation_req_t::display_name)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::join_conversation_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("scheduled",
                          &zlink::samples::supportchat::join_conversation_res_t::scheduled),
      zlink::json::field ("actorId",
                          &zlink::samples::supportchat::join_conversation_res_t::actor_id),
      zlink::json::field ("state", &zlink::samples::supportchat::join_conversation_res_t::state)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::join_conversation_failed_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::join_conversation_failed_notify_t::conversation_id),
      zlink::json::field ("error",
                          &zlink::samples::supportchat::join_conversation_failed_notify_t::error)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::send_chat_message_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("message",
                          &zlink::samples::supportchat::send_chat_message_res_t::message),
      zlink::json::field ("state", &zlink::samples::supportchat::send_chat_message_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::set_typing_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("isTyping", &zlink::samples::supportchat::set_typing_msg_t::is_typing)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::close_conversation_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "reason", &zlink::samples::supportchat::close_conversation_req_t::reason)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::close_conversation_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::supportchat::close_conversation_res_t::state)};
};

namespace zlink::samples::supportchat
{

/* 공통 sample spec §12: notify wire shape는 `{ConversationId, State}`다. state를 루트로
 * 펼치지 않는다. */

}

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_assigned_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::conversation_assigned_notify_t::conversation_id),
      zlink::json::field ("state",
                          &zlink::samples::supportchat::conversation_assigned_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_idle_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::conversation_idle_notify_t::conversation_id),
      zlink::json::field ("state",
                          &zlink::samples::supportchat::conversation_idle_notify_t::state)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::supportchat::conversation_closed_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::conversation_closed_notify_t::conversation_id),
      zlink::json::field ("state",
                          &zlink::samples::supportchat::conversation_closed_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::participant_joined_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "conversationId",
        &zlink::samples::supportchat::participant_joined_notify_t::conversation_id),
      zlink::json::field ("actorId",
                          &zlink::samples::supportchat::participant_joined_notify_t::actor_id),
      zlink::json::field ("role", &zlink::samples::supportchat::participant_joined_notify_t::role),
      zlink::json::field ("state",
                          &zlink::samples::supportchat::participant_joined_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::chat_message_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::chat_message_notify_t::conversation_id),
      zlink::json::field ("message", &zlink::samples::supportchat::chat_message_notify_t::message),
      zlink::json::field ("state", &zlink::samples::supportchat::chat_message_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::supportchat::typing_changed_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("conversationId",
                          &zlink::samples::supportchat::typing_changed_notify_t::conversation_id),
      zlink::json::field ("actorId",
                          &zlink::samples::supportchat::typing_changed_notify_t::actor_id),
      zlink::json::field ("isTyping",
                          &zlink::samples::supportchat::typing_changed_notify_t::is_typing),
      zlink::json::field ("state", &zlink::samples::supportchat::typing_changed_notify_t::state)};
};

namespace zlink::samples::supportchat
{

} // namespace zlink::samples::supportchat

// Explicit JSON state-store conversion shares the typed messaging profile.
namespace zlink::samples::supportchat
{
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void to_json (nlohmann::json &out, const T &value)
{
    out = zlink::detail::json_profile::encode_value (value);
}
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void from_json (const nlohmann::json &in, T &value)
{
    value = zlink::detail::json_profile::decode_value<T> (in);
}
}

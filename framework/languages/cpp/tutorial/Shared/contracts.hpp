#pragma once

#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <string>
#include <vector>

// Message contracts shared by both processes. They are plain structs: the
// Framework serializes them, and nothing here is registered or annotated.
// `packet_name` is the name that travels on the wire; without it the Framework
// falls back to the compiler's own type name, which differs between compilers.

// --8<-- [start:channel-contracts]
struct get_player_profile_t
{
    static constexpr const char *packet_name = "GetPlayerProfile";
    std::string player_id;
};

struct player_profile_t
{
    static constexpr const char *packet_name = "PlayerProfile";
    std::string player_id;
    std::string nickname;
    int level = 0;
};

// One-way: the caller does not wait, so this message has no reply struct.
struct record_login_t
{
    static constexpr const char *packet_name = "RecordLogin";
    std::string player_id;
};
// --8<-- [end:channel-contracts]

// --8<-- [start:clientserver-contracts]
struct issue_session_ticket_t
{
    static constexpr const char *packet_name = "IssueSessionTicket";
    std::string player_id;
};

struct session_ticket_t
{
    static constexpr const char *packet_name = "SessionTicket";
    std::string value;
};
// --8<-- [end:clientserver-contracts]

// --8<-- [start:fanout-contracts]
// Published without naming a recipient. Every subscribed node receives it.
struct maintenance_notice_t
{
    static constexpr const char *packet_name = "MaintenanceNotice";
    std::string message;
};
// --8<-- [end:fanout-contracts]

// --8<-- [start:spot-contracts]
// Reaches the room's create callback rather than a handler, so it carries what
// the room needs in order to exist.
struct open_room_t
{
    static constexpr const char *packet_name = "OpenRoom";
    std::string title;
};

// One-way: the caller does not wait for the room to record the line.
struct post_chat_t
{
    static constexpr const char *packet_name = "PostChat";
    std::string player_id;
    std::string text;
};

struct get_room_state_t
{
    static constexpr const char *packet_name = "GetRoomState";
};

struct room_state_t
{
    static constexpr const char *packet_name = "RoomState";
    std::string title;
    std::vector<std::string> chat;
};
// --8<-- [end:spot-contracts]

// --8<-- [start:instance-spot-contracts]
// A match queue has no create call, so nothing here corresponds to open_room_t.
struct join_match_queue_t
{
    static constexpr const char *packet_name = "JoinMatchQueue";
    std::string player_id;
};

struct match_queue_status_t
{
    static constexpr const char *packet_name = "MatchQueueStatus";
    int waiting;
};
// --8<-- [end:instance-spot-contracts]

// --8<-- [start:actor-contracts]
// Reaches the player's create callback rather than a handler.
struct create_player_t
{
    static constexpr const char *packet_name = "CreatePlayer";
    std::string nickname;
};

// One-way: the caller does not wait for the rename to be recorded.
struct change_nickname_t
{
    static constexpr const char *packet_name = "ChangeNickname";
    std::string nickname;
};

struct get_player_t
{
    static constexpr const char *packet_name = "GetPlayer";
};

struct player_info_t
{
    static constexpr const char *packet_name = "PlayerInfo";
    std::string player_id;
    std::string nickname;
};
// --8<-- [end:actor-contracts]

// --8<-- [start:stream-contracts]
// Exchanged over the external TCP connection, not between mesh nodes. The
// stream codec wants 64-bit integers as decimal strings, so the timestamp is
// carried as text rather than as an integer.
struct ping_t
{
    static constexpr const char *packet_name = "Ping";
    std::string sent_at_unix_ms;
};

struct pong_t
{
    static constexpr const char *packet_name = "Pong";
    std::string sent_at_unix_ms;
};
// --8<-- [end:stream-contracts]

// --8<-- [start:session-actor-contracts]
struct authenticate_t
{
    static constexpr const char *packet_name = "Authenticate";
    std::string player_id;
};

struct authenticated_t
{
    static constexpr const char *packet_name = "Authenticated";
    std::string player_id;
};

// Pushed by the player to its own connection, with no request to answer.
struct nickname_changed_t
{
    static constexpr const char *packet_name = "NicknameChanged";
    std::string nickname;
};
// --8<-- [end:session-actor-contracts]

// --8<-- [start:node-direct-contracts]
// Answered by the node itself rather than by a channel, so the reply describes
// that one process.
struct get_node_status_t
{
    static constexpr const char *packet_name = "GetNodeStatus";
};

struct node_status_t
{
    static constexpr const char *packet_name = "NodeStatus";
    std::string mesh_name;
    std::string channel_name;
    std::string called_by;
    std::string uptime;
};
// --8<-- [end:node-direct-contracts]

// The default JSON codec reads the DTO traits below. Wire names remain camelCase.

template <> struct zlink::json::dto_traits<get_player_profile_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &get_player_profile_t::player_id)};
};

template <> struct zlink::json::dto_traits<player_profile_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &player_profile_t::player_id),
      zlink::json::field ("nickname", &player_profile_t::nickname),
      zlink::json::field ("level", &player_profile_t::level)};
};

template <> struct zlink::json::dto_traits<record_login_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &record_login_t::player_id)};
};

template <> struct zlink::json::dto_traits<issue_session_ticket_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &issue_session_ticket_t::player_id)};
};

template <> struct zlink::json::dto_traits<session_ticket_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &session_ticket_t::value)};
};

template <> struct zlink::json::dto_traits<maintenance_notice_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("message", &maintenance_notice_t::message)};
};

template <> struct zlink::json::dto_traits<get_node_status_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<node_status_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("meshName", &node_status_t::mesh_name),
      zlink::json::field ("channelName", &node_status_t::channel_name),
      zlink::json::field ("calledBy", &node_status_t::called_by),
      zlink::json::field ("uptime", &node_status_t::uptime)};
};

template <> struct zlink::json::dto_traits<open_room_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("title", &open_room_t::title)};
};

template <> struct zlink::json::dto_traits<post_chat_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &post_chat_t::player_id),
      zlink::json::field ("text", &post_chat_t::text)};
};

template <> struct zlink::json::dto_traits<get_room_state_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<room_state_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("title", &room_state_t::title),
                                              zlink::json::field ("chat", &room_state_t::chat)};
};

template <> struct zlink::json::dto_traits<join_match_queue_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &join_match_queue_t::player_id)};
};

template <> struct zlink::json::dto_traits<match_queue_status_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("waiting", &match_queue_status_t::waiting)};
};

template <> struct zlink::json::dto_traits<create_player_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("nickname", &create_player_t::nickname)};
};

template <> struct zlink::json::dto_traits<change_nickname_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("nickname", &change_nickname_t::nickname)};
};

template <> struct zlink::json::dto_traits<get_player_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<player_info_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &player_info_t::player_id),
      zlink::json::field ("nickname", &player_info_t::nickname)};
};

template <> struct zlink::json::dto_traits<ping_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sentAtUnixMs", &ping_t::sent_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<pong_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sentAtUnixMs", &pong_t::sent_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<authenticate_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &authenticate_t::player_id)};
};

template <> struct zlink::json::dto_traits<authenticated_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &authenticated_t::player_id)};
};

template <> struct zlink::json::dto_traits<nickname_changed_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("nickname", &nickname_changed_t::nickname)};
};

#pragma once

#include <nlohmann/json.hpp>
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

// The JSON codec is the default one, and it reaches these structs through
// argument-dependent lookup. A struct with no to_json/from_json pair is
// rejected when the serializer registry is asked for it, so every contract
// above needs an entry here. Writing the pair by hand rather than using
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE is what lets the wire keep camelCase
// names while the C++ members stay snake_case.

inline void to_json (nlohmann::json &json, const get_player_profile_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, get_player_profile_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const player_profile_t &value)
{
    json = {{"playerId", value.player_id}, {"nickname", value.nickname}, {"level", value.level}};
}

inline void from_json (const nlohmann::json &json, player_profile_t &value)
{
    value.player_id = json.value ("playerId", "");
    value.nickname = json.value ("nickname", "");
    value.level = json.value ("level", 0);
}

inline void to_json (nlohmann::json &json, const record_login_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, record_login_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const issue_session_ticket_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, issue_session_ticket_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const session_ticket_t &value)
{
    json = {{"value", value.value}};
}

inline void from_json (const nlohmann::json &json, session_ticket_t &value)
{
    value.value = json.value ("value", "");
}

inline void to_json (nlohmann::json &json, const maintenance_notice_t &value)
{
    json = {{"message", value.message}};
}

inline void from_json (const nlohmann::json &json, maintenance_notice_t &value)
{
    value.message = json.value ("message", "");
}

inline void to_json (nlohmann::json &json, const get_node_status_t &)
{
    json = nlohmann::json::object ();
}

inline void from_json (const nlohmann::json &, get_node_status_t &)
{
}

inline void to_json (nlohmann::json &json, const node_status_t &value)
{
    json = {{"meshName", value.mesh_name},
            {"channelName", value.channel_name},
            {"calledBy", value.called_by},
            {"uptime", value.uptime}};
}

inline void from_json (const nlohmann::json &json, node_status_t &value)
{
    value.mesh_name = json.value ("meshName", "");
    value.channel_name = json.value ("channelName", "");
    value.called_by = json.value ("calledBy", "");
    value.uptime = json.value ("uptime", "");
}

inline void to_json (nlohmann::json &json, const open_room_t &value)
{
    json = {{"title", value.title}};
}

inline void from_json (const nlohmann::json &json, open_room_t &value)
{
    value.title = json.value ("title", "");
}

inline void to_json (nlohmann::json &json, const post_chat_t &value)
{
    json = {{"playerId", value.player_id}, {"text", value.text}};
}

inline void from_json (const nlohmann::json &json, post_chat_t &value)
{
    value.player_id = json.value ("playerId", "");
    value.text = json.value ("text", "");
}

inline void to_json (nlohmann::json &json, const get_room_state_t &)
{
    json = nlohmann::json::object ();
}

inline void from_json (const nlohmann::json &, get_room_state_t &)
{
}

inline void to_json (nlohmann::json &json, const room_state_t &value)
{
    json = {{"title", value.title}, {"chat", value.chat}};
}

inline void from_json (const nlohmann::json &json, room_state_t &value)
{
    value.title = json.value ("title", "");
    value.chat = json.value ("chat", std::vector<std::string>{});
}

inline void to_json (nlohmann::json &json, const join_match_queue_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, join_match_queue_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const match_queue_status_t &value)
{
    json = {{"waiting", value.waiting}};
}

inline void from_json (const nlohmann::json &json, match_queue_status_t &value)
{
    value.waiting = json.value ("waiting", 0);
}

inline void to_json (nlohmann::json &json, const create_player_t &value)
{
    json = {{"nickname", value.nickname}};
}

inline void from_json (const nlohmann::json &json, create_player_t &value)
{
    value.nickname = json.value ("nickname", "");
}

inline void to_json (nlohmann::json &json, const change_nickname_t &value)
{
    json = {{"nickname", value.nickname}};
}

inline void from_json (const nlohmann::json &json, change_nickname_t &value)
{
    value.nickname = json.value ("nickname", "");
}

inline void to_json (nlohmann::json &json, const get_player_t &)
{
    json = nlohmann::json::object ();
}

inline void from_json (const nlohmann::json &, get_player_t &)
{
}

inline void to_json (nlohmann::json &json, const player_info_t &value)
{
    json = {{"playerId", value.player_id}, {"nickname", value.nickname}};
}

inline void from_json (const nlohmann::json &json, player_info_t &value)
{
    value.player_id = json.value ("playerId", "");
    value.nickname = json.value ("nickname", "");
}

inline void to_json (nlohmann::json &json, const ping_t &value)
{
    json = {{"sentAtUnixMs", value.sent_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, ping_t &value)
{
    value.sent_at_unix_ms = json.value ("sentAtUnixMs", "");
}

inline void to_json (nlohmann::json &json, const pong_t &value)
{
    json = {{"sentAtUnixMs", value.sent_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, pong_t &value)
{
    value.sent_at_unix_ms = json.value ("sentAtUnixMs", "");
}

inline void to_json (nlohmann::json &json, const authenticate_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, authenticate_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const authenticated_t &value)
{
    json = {{"playerId", value.player_id}};
}

inline void from_json (const nlohmann::json &json, authenticated_t &value)
{
    value.player_id = json.value ("playerId", "");
}

inline void to_json (nlohmann::json &json, const nickname_changed_t &value)
{
    json = {{"nickname", value.nickname}};
}

inline void from_json (const nlohmann::json &json, nickname_changed_t &value)
{
    value.nickname = json.value ("nickname", "");
}

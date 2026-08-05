/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::samples::tictactoe
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

template <typename T>
void write_nullable (nlohmann::json &json, const char *name, const std::optional<T> &value)
{
    json[name] = value ? nlohmann::json (*value) : nlohmann::json (nullptr);
}

template <typename T>
std::optional<T> read_nullable (const nlohmann::json &json, const char *name)
{
    const auto found = json.find (name);
    if (found == json.end () || found->is_null ()) {
        return std::nullopt;
    }
    return found->template get<T> ();
}

struct tictactoe_marks_t
{
    static constexpr const char *x = "X";
    static constexpr const char *o = "O";
};

struct tictactoe_status_t
{
    static constexpr const char *waiting_for_players = "WaitingForPlayers";
    static constexpr const char *in_progress = "InProgress";
    static constexpr const char *won = "Won";
    static constexpr const char *draw = "Draw";
    static constexpr const char *turn_timed_out = "TurnTimedOut";
};

struct authenticate_req_t
{
    static constexpr const char *packet_name = "AuthenticateReq";
    std::string access_token;
};

struct player_info_t
{
    std::string actor_id;
    std::string display_name;
    int level = 0;
    int wins = 0;
};

struct play_node_info_t
{
    std::string stream_endpoint;
};

struct authenticate_res_t
{
    static constexpr const char *packet_name = "AuthenticateRes";
    player_info_t player;
};

struct authenticate_player_req_t
{
    static constexpr const char *packet_name = "AuthenticatePlayerReq";
    std::string access_token;
};

struct authenticate_player_res_t
{
    static constexpr const char *packet_name = "AuthenticatePlayerRes";
    bool accepted = false;
    player_info_t player;
    std::string reason;
};

/* Internal Play-role request. The public client uses CreateGameHttpReq; this message crosses the
 * API-to-Play application boundary and must not be treated as an additional client API. */
struct create_game_req_t
{
    static constexpr const char *packet_name = "CreateGameReq";
    std::string game_name;
    int required_level = 0;
};

struct create_game_http_req_t
{
    static constexpr const char *packet_name = "CreateGameHttpReq";
    std::optional<std::string> game_name;
};

struct create_game_http_res_t
{
    static constexpr const char *packet_name = "CreateGameHttpRes";
    std::string room_id;
    std::string game_name;
    std::vector<std::string> play_endpoints;
    std::vector<play_node_info_t> play_nodes;
    int required_level = 0;
};

/* 공통 sample spec §11: JoinGameReq에는 RoomId만 담는다. PlayerInfo는 인증 때 actor에
 * 설정되고, room join payload(TicTacToeGameJoinReq)에 actor가 직접 실어 보낸다. */
struct join_game_req_t
{
    static constexpr const char *packet_name = "JoinGameReq";
    std::string room_id;
};

/* Internal Play-role messages. The Entry Spot uses them to pass the authenticated player to the
 * room Spot; the public client sees JoinGameReq/Res only. */
struct tictactoe_game_join_req_t
{
    std::string room_id;
    player_info_t player;
};

struct tictactoe_state_t
{
    std::string room_id;
    std::string board = ".........";
    std::string status = tictactoe_status_t::waiting_for_players;
    std::string next_turn;
    std::optional<std::string> winner;
    std::optional<std::string> x_actor_id;
    std::optional<std::string> o_actor_id;
    std::optional<std::string> last_move_actor_id;
    std::optional<int> last_move_cell;
};

struct tictactoe_game_join_res_t
{
    tictactoe_state_t state;
};

struct join_game_res_t
{
    static constexpr const char *packet_name = "JoinGameRes";
    tictactoe_state_t state;
};

struct join_game_failed_notify_t
{
    static constexpr const char *packet_name = "JoinGameFailedNotify";
    std::string room_id;
    std::string error;
};

struct place_mark_req_t
{
    static constexpr const char *packet_name = "PlaceMarkReq";
    int cell = 0;
};

struct place_mark_res_t
{
    static constexpr const char *packet_name = "PlaceMarkRes";
    tictactoe_state_t state;
};

struct player_joined_notify_t
{
    static constexpr const char *packet_name = "PlayerJoinedNotify";
    std::string room_id;
    std::string actor_id;
    std::string display_name;
    int level = 0;
    std::string mark;
    tictactoe_state_t state;
};

struct observe_milestone_req_t
{
    static constexpr const char *packet_name = "ObserveMilestoneReq";
};

struct observe_milestone_res_t
{
    static constexpr const char *packet_name = "ObserveMilestoneRes";
    bool subscribed = false;
};

struct leave_game_msg_t
{
    static constexpr const char *packet_name = "LeaveGameMsg";
    std::string room_id;
};

struct player_win_milestone_event_t
{
    static constexpr const char *packet_name = "PlayerWinMilestoneEvent";
    std::string room_id;
    std::string actor_id;
    std::string display_name;
    int wins = 0;
};

struct win_milestone_notify_t
{
    static constexpr const char *packet_name = "WinMilestoneNotify";
    std::string room_id;
    std::string actor_id;
    std::string display_name;
    int wins = 0;
};

struct game_state_notify_t
{
    static constexpr const char *packet_name = "GameStateNotify";
    std::string room_id;
    std::string next_turn;
    tictactoe_state_t state;
};

inline void to_json (nlohmann::json &json, const authenticate_req_t &value)
{
    json = {{"accessToken", value.access_token}};
}

inline void from_json (const nlohmann::json &json, authenticate_req_t &value)
{
    value.access_token = json.value ("accessToken", "");
}

inline void to_json (nlohmann::json &json, const player_info_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"level", value.level},
            {"wins", value.wins}};
}

inline void from_json (const nlohmann::json &json, player_info_t &value)
{
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.level = json.value ("level", 0);
    value.wins = json.value ("wins", 0);
}

inline void to_json (nlohmann::json &json, const play_node_info_t &value)
{
    json = {{"streamEndpoint", value.stream_endpoint}};
}

inline void from_json (const nlohmann::json &json, play_node_info_t &value)
{
    value.stream_endpoint = json.value ("streamEndpoint", "");
}

inline void to_json (nlohmann::json &json, const authenticate_player_req_t &value)
{
    json = {{"accessToken", value.access_token}};
}

inline void from_json (const nlohmann::json &json, authenticate_player_req_t &value)
{
    value.access_token = json.value ("accessToken", "");
}

inline void to_json (nlohmann::json &json, const authenticate_player_res_t &value)
{
    json = {{"accepted", value.accepted}, {"player", value.player}, {"reason", value.reason}};
}

inline void from_json (const nlohmann::json &json, authenticate_player_res_t &value)
{
    value.accepted = json.value ("accepted", false);
    value.player = json.value ("player", player_info_t{});
    value.reason = json.value ("reason", "");
}

inline void to_json (nlohmann::json &json, const create_game_req_t &value)
{
    json = {{"gameName", value.game_name}, {"requiredLevel", value.required_level}};
}

inline void from_json (const nlohmann::json &json, create_game_req_t &value)
{
    value.game_name = json.value ("gameName", "");
    value.required_level = json.value ("requiredLevel", 0);
}

inline void to_json (nlohmann::json &json, const create_game_http_req_t &value)
{
    write_nullable (json, "gameName", value.game_name);
}

inline void from_json (const nlohmann::json &json, create_game_http_req_t &value)
{
    value.game_name = read_nullable<std::string> (json, "gameName");
}

inline void to_json (nlohmann::json &json, const join_game_req_t &value)
{
    json = {{"roomId", value.room_id}};
}

inline void from_json (const nlohmann::json &json, join_game_req_t &value)
{
    value.room_id = json.value ("roomId", "");
}

inline void to_json (nlohmann::json &json, const tictactoe_game_join_req_t &value)
{
    json = {{"roomId", value.room_id}, {"player", value.player}};
}

inline void from_json (const nlohmann::json &json, tictactoe_game_join_req_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.player = json.value ("player", player_info_t{});
}

inline void to_json (nlohmann::json &json, const place_mark_req_t &value)
{
    json = {{"cell", value.cell}};
}

inline void from_json (const nlohmann::json &json, place_mark_req_t &value)
{
    value.cell = json.value ("cell", 0);
}

inline void to_json (nlohmann::json &json, const tictactoe_state_t &value)
{
    json = {{"roomId", value.room_id},
            {"board", value.board},
            {"status", value.status},
            {"nextTurn", value.next_turn}};
    write_nullable (json, "winner", value.winner);
    write_nullable (json, "xActorId", value.x_actor_id);
    write_nullable (json, "oActorId", value.o_actor_id);
    write_nullable (json, "lastMoveActorId", value.last_move_actor_id);
    write_nullable (json, "lastMoveCell", value.last_move_cell);
}

inline void from_json (const nlohmann::json &json, tictactoe_state_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.board = json.value ("board", ".........");
    value.status = json.value ("status", "");
    value.next_turn = json.value ("nextTurn", "");
    value.winner = read_nullable<std::string> (json, "winner");
    value.x_actor_id = read_nullable<std::string> (json, "xActorId");
    value.o_actor_id = read_nullable<std::string> (json, "oActorId");
    value.last_move_actor_id = read_nullable<std::string> (json, "lastMoveActorId");
    value.last_move_cell = read_nullable<int> (json, "lastMoveCell");
}

inline void to_json (nlohmann::json &json, const tictactoe_game_join_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, tictactoe_game_join_res_t &value)
{
    value.state = json.value ("state", tictactoe_state_t{});
}

inline void to_json (nlohmann::json &json, const authenticate_res_t &value)
{
    json = {{"player", value.player}};
}

inline void from_json (const nlohmann::json &json, authenticate_res_t &value)
{
    value.player = json.value ("player", player_info_t{});
}

inline void to_json (nlohmann::json &json, const create_game_http_res_t &value)
{
    json = {{"roomId", value.room_id},
            {"gameName", value.game_name},
            {"playEndpoints", value.play_endpoints},
            {"playNodes", value.play_nodes},
            {"requiredLevel", value.required_level}};
}

inline void from_json (const nlohmann::json &json, create_game_http_res_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.game_name = json.value ("gameName", "");
    value.play_endpoints = json.value ("playEndpoints", std::vector<std::string>{});
    value.play_nodes = json.value ("playNodes", std::vector<play_node_info_t>{});
    value.required_level = json.value ("requiredLevel", 0);
}

inline void to_json (nlohmann::json &json, const join_game_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, join_game_res_t &value)
{
    value.state = json.value ("state", tictactoe_state_t{});
}

inline void to_json (nlohmann::json &json, const join_game_failed_notify_t &value)
{
    json = {{"roomId", value.room_id}, {"error", value.error}};
}

inline void from_json (const nlohmann::json &json, join_game_failed_notify_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.error = json.value ("error", "");
}

inline void to_json (nlohmann::json &json, const place_mark_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, place_mark_res_t &value)
{
    value.state = json.value ("state", tictactoe_state_t{});
}

inline void to_json (nlohmann::json &json, const player_joined_notify_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"level", value.level},
            {"mark", value.mark},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, player_joined_notify_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.level = json.value ("level", 0);
    value.mark = json.value ("mark", "");
    value.state = json.value ("state", tictactoe_state_t{});
}

inline void to_json (nlohmann::json &json, const observe_milestone_req_t &)
{
    json = nlohmann::json::object ();
}

inline void from_json (const nlohmann::json &, observe_milestone_req_t &) {}

inline void to_json (nlohmann::json &json, const observe_milestone_res_t &value)
{
    json = {{"subscribed", value.subscribed}};
}

inline void from_json (const nlohmann::json &json, observe_milestone_res_t &value)
{
    value.subscribed = json.value ("subscribed", false);
}

inline void to_json (nlohmann::json &json, const leave_game_msg_t &value)
{
    json = {{"roomId", value.room_id}};
}

inline void from_json (const nlohmann::json &json, leave_game_msg_t &value)
{
    value.room_id = json.value ("roomId", "");
}

inline void to_json (nlohmann::json &json, const player_win_milestone_event_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"wins", value.wins}};
}

inline void from_json (const nlohmann::json &json, player_win_milestone_event_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.wins = json.value ("wins", 0);
}

inline void to_json (nlohmann::json &json, const win_milestone_notify_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"wins", value.wins}};
}

inline void from_json (const nlohmann::json &json, win_milestone_notify_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.actor_id = json.value ("actorId", "");
    value.display_name = json.value ("displayName", "");
    value.wins = json.value ("wins", 0);
}

inline void to_json (nlohmann::json &json, const game_state_notify_t &value)
{
    json = {{"roomId", value.room_id}, {"nextTurn", value.next_turn}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, game_state_notify_t &value)
{
    value.room_id = json.value ("roomId", "");
    value.next_turn = json.value ("nextTurn", "");
    value.state = json.value ("state", tictactoe_state_t{});
}

} // namespace zlink::samples::tictactoe

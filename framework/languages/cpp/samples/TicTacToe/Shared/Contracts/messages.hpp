/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <zlink/json_profile.hpp>
#include <optional>
#include <string>
#include <vector>

namespace zlink::samples::tictactoe
{

template <typename T>
void write_nullable (nlohmann::json &json, const char *name, const std::optional<T> &value)
{
    json[name] = value ? nlohmann::json (*value) : nlohmann::json (nullptr);
}

template <typename T> std::optional<T> read_nullable (const nlohmann::json &json, const char *name)
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

struct player_actor_create_req_t
{
    static constexpr const char *packet_name = "PlayerActorCreateReq";
    player_info_t player;
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
    player_info_t player;
};

/* Internal Play-role request. The public client uses CreateGameHttpReq; this message crosses the
 * API-to-Play application boundary and must not be treated as an additional client API. */
struct tictactoe_game_create_req_t
{
    static constexpr const char *packet_name = "TicTacToeGameCreateReq";
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

/* 공통 sample spec §11: JoinGameMsg에는 RoomId만 담는다. PlayerInfo는 인증 때 actor에
 * 설정되고, room join payload(TicTacToeGameJoinReq)에 actor가 직접 실어 보낸다. */
struct join_game_msg_t
{
    static constexpr const char *packet_name = "JoinGameMsg";
    std::string room_id;
};

/* Internal Play-role messages. The Entry Spot uses them to pass the authenticated player to the
 * room Spot; the public client sends JoinGameMsg and receives JoinGameNotify. */
struct tictactoe_game_join_req_t
{
    static constexpr const char *packet_name = "TicTacToeGameJoinReq";
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
    static constexpr const char *packet_name = "TicTacToeGameJoinRes";
    tictactoe_state_t state;
};

struct join_game_notify_t
{
    static constexpr const char *packet_name = "JoinGameNotify";
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
    tictactoe_state_t state;
};

}

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::authenticate_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "accessToken", &zlink::samples::tictactoe::authenticate_req_t::access_token)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::player_info_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("actorId", &zlink::samples::tictactoe::player_info_t::actor_id),
      zlink::json::field ("displayName", &zlink::samples::tictactoe::player_info_t::display_name),
      zlink::json::field ("level", &zlink::samples::tictactoe::player_info_t::level),
      zlink::json::field ("wins", &zlink::samples::tictactoe::player_info_t::wins)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::player_actor_create_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("player", &zlink::samples::tictactoe::player_actor_create_req_t::player)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::play_node_info_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "streamEndpoint", &zlink::samples::tictactoe::play_node_info_t::stream_endpoint)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::authenticate_player_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "accessToken", &zlink::samples::tictactoe::authenticate_player_req_t::access_token)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::authenticate_player_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("player", &zlink::samples::tictactoe::authenticate_player_res_t::player)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::tictactoe_game_create_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("gameName",
                          &zlink::samples::tictactoe::tictactoe_game_create_req_t::game_name),
      zlink::json::field ("requiredLevel",
                          &zlink::samples::tictactoe::tictactoe_game_create_req_t::required_level)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::create_game_http_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "gameName", &zlink::samples::tictactoe::create_game_http_req_t::game_name)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::join_game_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::join_game_msg_t::room_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::tictactoe_game_join_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::tictactoe_game_join_req_t::room_id),
      zlink::json::field ("player", &zlink::samples::tictactoe::tictactoe_game_join_req_t::player)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::place_mark_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("cell", &zlink::samples::tictactoe::place_mark_req_t::cell)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::tictactoe_state_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::tictactoe_state_t::room_id),
      zlink::json::field ("board", &zlink::samples::tictactoe::tictactoe_state_t::board),
      zlink::json::field ("status", &zlink::samples::tictactoe::tictactoe_state_t::status),
      zlink::json::field ("nextTurn", &zlink::samples::tictactoe::tictactoe_state_t::next_turn),
      zlink::json::field ("winner", &zlink::samples::tictactoe::tictactoe_state_t::winner),
      zlink::json::field ("xActorId", &zlink::samples::tictactoe::tictactoe_state_t::x_actor_id),
      zlink::json::field ("oActorId", &zlink::samples::tictactoe::tictactoe_state_t::o_actor_id),
      zlink::json::field ("lastMoveActorId",
                          &zlink::samples::tictactoe::tictactoe_state_t::last_move_actor_id),
      zlink::json::field ("lastMoveCell",
                          &zlink::samples::tictactoe::tictactoe_state_t::last_move_cell)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::tictactoe_game_join_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::tictactoe::tictactoe_game_join_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::authenticate_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("player", &zlink::samples::tictactoe::authenticate_res_t::player)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::create_game_http_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::create_game_http_res_t::room_id),
      zlink::json::field ("gameName",
                          &zlink::samples::tictactoe::create_game_http_res_t::game_name),
      zlink::json::field ("playEndpoints",
                          &zlink::samples::tictactoe::create_game_http_res_t::play_endpoints),
      zlink::json::field ("playNodes",
                          &zlink::samples::tictactoe::create_game_http_res_t::play_nodes),
      zlink::json::field ("requiredLevel",
                          &zlink::samples::tictactoe::create_game_http_res_t::required_level)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::join_game_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::tictactoe::join_game_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::join_game_failed_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::join_game_failed_notify_t::room_id),
      zlink::json::field ("error", &zlink::samples::tictactoe::join_game_failed_notify_t::error)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::place_mark_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::tictactoe::place_mark_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::player_joined_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::player_joined_notify_t::room_id),
      zlink::json::field ("actorId", &zlink::samples::tictactoe::player_joined_notify_t::actor_id),
      zlink::json::field ("displayName",
                          &zlink::samples::tictactoe::player_joined_notify_t::display_name),
      zlink::json::field ("level", &zlink::samples::tictactoe::player_joined_notify_t::level),
      zlink::json::field ("mark", &zlink::samples::tictactoe::player_joined_notify_t::mark),
      zlink::json::field ("state", &zlink::samples::tictactoe::player_joined_notify_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::observe_milestone_req_t>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::observe_milestone_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "subscribed", &zlink::samples::tictactoe::observe_milestone_res_t::subscribed)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::leave_game_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::leave_game_msg_t::room_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::player_win_milestone_event_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId",
                          &zlink::samples::tictactoe::player_win_milestone_event_t::room_id),
      zlink::json::field ("actorId",
                          &zlink::samples::tictactoe::player_win_milestone_event_t::actor_id),
      zlink::json::field ("displayName",
                          &zlink::samples::tictactoe::player_win_milestone_event_t::display_name),
      zlink::json::field ("wins", &zlink::samples::tictactoe::player_win_milestone_event_t::wins)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::win_milestone_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &zlink::samples::tictactoe::win_milestone_notify_t::room_id),
      zlink::json::field ("actorId", &zlink::samples::tictactoe::win_milestone_notify_t::actor_id),
      zlink::json::field ("displayName",
                          &zlink::samples::tictactoe::win_milestone_notify_t::display_name),
      zlink::json::field ("wins", &zlink::samples::tictactoe::win_milestone_notify_t::wins)};
};

template <> struct zlink::json::dto_traits<zlink::samples::tictactoe::game_state_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::tictactoe::game_state_notify_t::state)};
};

namespace zlink::samples::tictactoe
{

} // namespace zlink::samples::tictactoe

// Explicit JSON state-store conversion shares the typed messaging profile.
namespace zlink::samples::tictactoe
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

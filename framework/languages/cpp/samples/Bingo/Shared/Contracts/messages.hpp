/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/codecs/json.hpp>
#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <array>

namespace zlink::samples::bingo
{

inline std::string json_string (const nlohmann::json &json,
                                const char *camel,
                                const char *snake,
                                std::string fallback = {})
{
    if (json.contains (camel)) {
        return json.value (camel, fallback);
    }
    return json.value (snake, fallback);
}

template <typename T>
inline T json_value (const nlohmann::json &json, const char *camel, const char *snake, T fallback)
{
    if (json.contains (camel)) {
        return json.value (camel, fallback);
    }
    return json.value (snake, fallback);
}

inline std::optional<std::string>
json_optional_string (const nlohmann::json &json, const char *name)
{
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::string> ();
}

template <typename T>
inline void set_optional (nlohmann::json &json,
                          const char *name,
                          const std::optional<T> &value)
{
    if (value) {
        json[name] = *value;
    }
}

struct bingo_sample_modes_t
{
    static constexpr const char *two_player = "two-player";
};

struct bingo_sample_players_t
{
    static constexpr const char *player1 = "player-1";
    static constexpr const char *player2 = "player-2";
    static constexpr const char *observer = "observer";
};

struct bingo_reward_items_t
{
    static constexpr const char *golden_dauber_id = "rare-golden-dauber";
    static constexpr const char *golden_dauber_name = "Golden Dauber";
    static constexpr const char *legendary_rarity = "Legendary";
};

struct bingo_room_status_t
{
    static constexpr const char *waiting = "WaitingForPlayers";
    static constexpr const char *running = "Running";
    static constexpr const char *finished = "Finished";
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
    std::optional<std::string> actor_id;
    std::optional<std::string> display_name;
    std::optional<std::string> reason;
};

struct get_player_record_req_t
{
    static constexpr const char *packet_name = "GetPlayerRecordReq";
    std::string actor_id;
};

struct get_player_record_res_t
{
    static constexpr const char *packet_name = "GetPlayerRecordRes";
    std::string actor_id;
    int wins = 0;
    int losses = 0;
};

struct report_bingo_result_req_t
{
    static constexpr const char *packet_name = "ReportBingoResultReq";
    std::string room_id;
    std::string actor_id;
    bool won = false;
    int final_draw_seq = 0;
};

struct report_bingo_result_res_t
{
    static constexpr const char *packet_name = "ReportBingoResultRes";
    std::string actor_id;
    int wins = 0;
    int losses = 0;
};

struct ensure_player_actor_req_t
{
    static constexpr const char *packet_name = "EnsurePlayerActorReq";
    std::string actor_id;
    std::string display_name;
};

struct match_bingo_req_t
{
    static constexpr const char *packet_name = "MatchBingoReq";
    std::string mode;
};

struct match_bingo_api_req_t
{
    static constexpr const char *packet_name = "MatchBingoApiReq";
    std::string actor_id;
    std::string display_name;
    std::string mode;
};

struct match_bingo_api_res_t
{
    static constexpr const char *packet_name = "MatchBingoApiRes";
    std::string room_id;
};

struct reserve_bingo_room_req_t
{
    static constexpr const char *packet_name = "ReserveBingoRoomReq";
    std::string mode;
    std::string actor_id;
    std::string level_bucket;
};

struct bingo_room_settings_payload_t
{
    static constexpr const char *packet_name = "BingoRoomSettingsPayload";
    std::string room_name;
    std::string mode;
    int required_players = 2;
    int max_draw_number = 75;
    std::string purpose = "Game";
    std::optional<std::string> observed_room_id;
};

struct reserve_bingo_room_res_t
{
    static constexpr const char *packet_name = "ReserveBingoRoomRes";
    std::string room_id;
    bingo_room_settings_payload_t settings;
};

struct bingo_player_state_t
{
    std::string actor_id;
    std::string display_name;
    int seat = 0;
    bool is_host = false;
    std::vector<int> card;
    std::vector<bool> marks;
    int completed_lines = 0;
    int wins = 0;
    int losses = 0;
};

struct bingo_room_state_t
{
    std::string room_id;
    std::string status = bingo_room_status_t::waiting;
    std::string host_actor_id;
    bool can_start = false;
    int draw_seq = 0;
    std::optional<int> last_drawn_number;
    std::vector<int> drawn_numbers;
    std::vector<bingo_player_state_t> players;
    std::vector<std::string> winners;
};

struct match_bingo_res_t
{
    static constexpr const char *packet_name = "MatchBingoRes";
    std::string room_id;
    bingo_room_state_t state;
};

struct bingo_room_join_req_t
{
    static constexpr const char *packet_name = "BingoRoomJoinReq";
    std::string room_id;
    std::string actor_id;
    std::string display_name;
    bool observe_only = false;
};

struct bingo_room_join_res_t
{
    static constexpr const char *packet_name = "BingoRoomJoinRes";
    bingo_room_state_t state;
};

struct submit_bingo_card_req_t
{
    static constexpr const char *packet_name = "SubmitBingoCardReq";
    std::string room_id;
    std::vector<int> card;
};

struct submit_bingo_card_res_t
{
    static constexpr const char *packet_name = "SubmitBingoCardRes";
    bingo_room_state_t state;
};

struct observe_bingo_events_req_t
{
    static constexpr const char *packet_name = "ObserveBingoEventsReq";
    std::string room_id;
};

struct observe_bingo_events_res_t
{
    static constexpr const char *packet_name = "ObserveBingoEventsRes";
    bool subscribed = false;
};

struct stop_observing_bingo_events_req_t
{
    static constexpr const char *packet_name = "StopObservingBingoEventsReq";
    std::string room_id;
};

struct stop_observing_bingo_events_res_t
{
    static constexpr const char *packet_name = "StopObservingBingoEventsRes";
    bool stopped = false;
};

struct player_joined_notify_t
{
    static constexpr const char *packet_name = "PlayerJoinedNotify";
    std::string room_id;
    std::string actor_id;
    std::string display_name;
    int seat = 0;
    bool is_host = false;
    bingo_room_state_t state;
};

struct game_started_notify_t
{
    static constexpr const char *packet_name = "BingoGameStartedNotify";
    bingo_room_state_t state;
};

struct number_drawn_notify_t
{
    static constexpr const char *packet_name = "BingoNumberDrawnNotify";
    std::string room_id;
    int draw_seq = 0;
    int number = 0;
    bingo_room_state_t state;
};

struct game_ended_notify_t
{
    static constexpr const char *packet_name = "BingoGameEndedNotify";
    bingo_room_state_t state;
};

struct bingo_reward_announced_notify_t
{
    static constexpr const char *packet_name = "BingoRewardAnnouncedNotify";
    std::string room_id;
    std::string actor_id;
    int draw_seq = 0;
    std::string item_id;
    std::string item_name;
    std::string rarity;
};

struct bingo_reward_acquired_event_t
{
    static constexpr const char *packet_name = "BingoRewardAcquiredEvent";
    std::string room_id;
    std::string actor_id;
    int draw_seq = 0;
    std::string item_id;
    std::string item_name;
    std::string rarity;
};

inline void to_json (nlohmann::json &json, const authenticate_req_t &value)
{
    json = {{"accessToken", value.access_token}};
}

inline void from_json (const nlohmann::json &json, authenticate_req_t &value)
{
    value.access_token = json_string (json, "accessToken", "access_token");
}

inline void to_json (nlohmann::json &json, const authenticate_player_req_t &value)
{
    json = {{"accessToken", value.access_token}};
}

inline void from_json (const nlohmann::json &json, authenticate_player_req_t &value)
{
    value.access_token = json_string (json, "accessToken", "access_token");
}

inline void to_json (nlohmann::json &json, const authenticate_player_res_t &value)
{
    json = {{"accepted", value.accepted}};
    set_optional (json, "actorId", value.actor_id);
    set_optional (json, "displayName", value.display_name);
    set_optional (json, "reason", value.reason);
}

inline void from_json (const nlohmann::json &json, authenticate_player_res_t &value)
{
    value.accepted = json.value ("accepted", false);
    value.actor_id = json_optional_string (json, "actorId");
    value.display_name = json_optional_string (json, "displayName");
    value.reason = json_optional_string (json, "reason");
}

inline void to_json (nlohmann::json &json, const get_player_record_req_t &value)
{
    json = {{"actorId", value.actor_id}};
}

inline void from_json (const nlohmann::json &json, get_player_record_req_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
}

inline void to_json (nlohmann::json &json, const get_player_record_res_t &value)
{
    json = {{"actorId", value.actor_id}, {"wins", value.wins}, {"losses", value.losses}};
}

inline void from_json (const nlohmann::json &json, get_player_record_res_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.wins = json.value ("wins", 0);
    value.losses = json.value ("losses", 0);
}

inline void to_json (nlohmann::json &json, const report_bingo_result_req_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"won", value.won},
            {"finalDrawSeq", value.final_draw_seq}};
}

inline void from_json (const nlohmann::json &json, report_bingo_result_req_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.won = json.value ("won", false);
    value.final_draw_seq = json_value (json, "finalDrawSeq", "final_draw_seq", 0);
}

inline void to_json (nlohmann::json &json, const report_bingo_result_res_t &value)
{
    json = {{"actorId", value.actor_id}, {"wins", value.wins}, {"losses", value.losses}};
}

inline void from_json (const nlohmann::json &json, report_bingo_result_res_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.wins = json.value ("wins", 0);
    value.losses = json.value ("losses", 0);
}

inline void to_json (nlohmann::json &json, const ensure_player_actor_req_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name}};
}

inline void from_json (const nlohmann::json &json, ensure_player_actor_req_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.display_name = json_string (json, "displayName", "display_name");
}

inline void to_json (nlohmann::json &json, const match_bingo_req_t &value)
{
    json = {{"mode", value.mode}};
}

inline void from_json (const nlohmann::json &json, match_bingo_req_t &value)
{
    value.mode = json.value ("mode", "");
}

inline void to_json (nlohmann::json &json, const match_bingo_api_req_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"mode", value.mode}};
}

inline void from_json (const nlohmann::json &json, match_bingo_api_req_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.display_name = json_string (json, "displayName", "display_name");
    value.mode = json.value ("mode", "");
}

inline void to_json (nlohmann::json &json, const match_bingo_api_res_t &value)
{
    json = {{"roomId", value.room_id}};
}

inline void from_json (const nlohmann::json &json, match_bingo_api_res_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
}

inline void to_json (nlohmann::json &json, const reserve_bingo_room_req_t &value)
{
    json = {{"mode", value.mode},
            {"actorId", value.actor_id},
            {"levelBucket", value.level_bucket}};
}

inline void from_json (const nlohmann::json &json, reserve_bingo_room_req_t &value)
{
    value.mode = json.value ("mode", "");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.level_bucket = json_string (json, "levelBucket", "level_bucket");
}

inline void to_json (nlohmann::json &json, const bingo_room_settings_payload_t &value)
{
    json = {{"roomName", value.room_name},
            {"mode", value.mode},
            {"requiredPlayers", value.required_players},
            {"maxDrawNumber", value.max_draw_number},
            {"purpose", value.purpose}};
    set_optional (json, "observedRoomId", value.observed_room_id);
}

inline void from_json (const nlohmann::json &json, bingo_room_settings_payload_t &value)
{
    value.room_name = json_string (json, "roomName", "room_name");
    value.mode = json.value ("mode", "");
    value.required_players = json_value (json, "requiredPlayers", "required_players", 2);
    value.max_draw_number = json_value (json, "maxDrawNumber", "max_draw_number", 75);
    value.purpose = json.value ("purpose", "Game");
    value.observed_room_id = json_optional_string (json, "observedRoomId");
}

inline void to_json (nlohmann::json &json, const reserve_bingo_room_res_t &value)
{
    json = {{"roomId", value.room_id}, {"settings", value.settings}};
}

inline void from_json (const nlohmann::json &json, reserve_bingo_room_res_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.settings = json.value ("settings", bingo_room_settings_payload_t{});
}

inline void to_json (nlohmann::json &json, const bingo_room_join_req_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"observeOnly", value.observe_only}};
}

inline void from_json (const nlohmann::json &json, bingo_room_join_req_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.display_name = json_string (json, "displayName", "display_name");
    value.observe_only = json_value (json, "observeOnly", "observe_only", false);
}

inline void to_json (nlohmann::json &json, const submit_bingo_card_req_t &value)
{
    json = {{"roomId", value.room_id}, {"card", value.card}};
}

inline void from_json (const nlohmann::json &json, submit_bingo_card_req_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.card = json.value ("card", std::vector<int>{});
}

inline void to_json (nlohmann::json &json, const bingo_player_state_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name},
            {"seat", value.seat},
            {"isHost", value.is_host},
            {"card", value.card},
            {"marks", value.marks},
            {"completedLines", value.completed_lines},
            {"wins", value.wins},
            {"losses", value.losses}};
}

inline void from_json (const nlohmann::json &json, bingo_player_state_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.display_name = json_string (json, "displayName", "display_name");
    value.seat = json.value ("seat", 0);
    value.is_host = json_value (json, "isHost", "is_host", false);
    value.card = json.value ("card", std::vector<int>{});
    value.marks = json.value ("marks", std::vector<bool>{});
    value.completed_lines = json_value (json, "completedLines", "completed_lines", 0);
    value.wins = json.value ("wins", 0);
    value.losses = json.value ("losses", 0);
}

inline void to_json (nlohmann::json &json, const bingo_room_state_t &value)
{
    json = {{"roomId", value.room_id},
            {"status", value.status},
            {"hostActorId", value.host_actor_id},
            {"canStart", value.can_start},
            {"drawSeq", value.draw_seq},
            {"drawnNumbers", value.drawn_numbers},
            {"players", value.players},
            {"winners", value.winners}};
    set_optional (json, "lastDrawnNumber", value.last_drawn_number);
}

inline void from_json (const nlohmann::json &json, bingo_room_state_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.status = json.value ("status", "");
    value.host_actor_id = json_string (json, "hostActorId", "host_actor_id");
    value.can_start = json_value (json, "canStart", "can_start", false);
    value.draw_seq = json_value (json, "drawSeq", "draw_seq", 0);
    if (json.contains ("lastDrawnNumber")) {
        value.last_drawn_number = json.at ("lastDrawnNumber").is_null ()
                                     ? std::nullopt
                                     : std::optional<int> (json.at ("lastDrawnNumber").get<int> ());
    } else if (json.contains ("last_drawn_number")) {
        value.last_drawn_number = json.at ("last_drawn_number").is_null ()
                                     ? std::nullopt
                                     : std::optional<int> (json.at ("last_drawn_number").get<int> ());
    } else {
        value.last_drawn_number = std::nullopt;
    }
    value.drawn_numbers =
      json_value (json, "drawnNumbers", "drawn_numbers", std::vector<int>{});
    value.players = json.value ("players", std::vector<bingo_player_state_t>{});
    value.winners = json.value ("winners", std::vector<std::string>{});
}

inline void to_json (nlohmann::json &json, const authenticate_res_t &value)
{
    json = {{"actorId", value.actor_id},
            {"displayName", value.display_name}};
}

inline void from_json (const nlohmann::json &json, authenticate_res_t &value)
{
    value.actor_id = json_string (json, "actorId", "actor_id", json.value ("ActorId", ""));
    value.display_name =
      json_string (json, "displayName", "display_name", json.value ("DisplayName", ""));
}

inline void to_json (nlohmann::json &json, const match_bingo_res_t &value)
{
    json = {{"roomId", value.room_id},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, match_bingo_res_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const bingo_room_join_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, bingo_room_join_res_t &value)
{
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const submit_bingo_card_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, submit_bingo_card_res_t &value)
{
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const observe_bingo_events_req_t &value)
{
    json = {{"roomId", value.room_id}};
}

inline void from_json (const nlohmann::json &json, observe_bingo_events_req_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
}

inline void to_json (nlohmann::json &json, const observe_bingo_events_res_t &value)
{
    json = {{"subscribed", value.subscribed}};
}

inline void from_json (const nlohmann::json &json, observe_bingo_events_res_t &value)
{
    value.subscribed = json.value ("subscribed", false);
}

inline void to_json (nlohmann::json &json, const stop_observing_bingo_events_req_t &value)
{
    json = {{"roomId", value.room_id}};
}

inline void from_json (const nlohmann::json &json, stop_observing_bingo_events_req_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
}

inline void to_json (nlohmann::json &json, const stop_observing_bingo_events_res_t &value)
{
    json = {{"stopped", value.stopped}};
}

inline void from_json (const nlohmann::json &json, stop_observing_bingo_events_res_t &value)
{
    value.stopped = json.value ("stopped", false);
}

inline void to_json (nlohmann::json &json, const player_joined_notify_t &value)
{
    json = {
      {"roomId", value.room_id}, {"actorId", value.actor_id}, {"displayName", value.display_name},
      {"seat", value.seat},      {"isHost", value.is_host},   {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, player_joined_notify_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.display_name = json_string (json, "displayName", "display_name");
    value.seat = json.value ("seat", 0);
    value.is_host = json_value (json, "isHost", "is_host", false);
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const game_started_notify_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, game_started_notify_t &value)
{
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const number_drawn_notify_t &value)
{
    json = {{"roomId", value.room_id},
            {"drawSeq", value.draw_seq},
            {"number", value.number},
            {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, number_drawn_notify_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.draw_seq = json_value (json, "drawSeq", "draw_seq", 0);
    value.number = json.value ("number", 0);
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const game_ended_notify_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, game_ended_notify_t &value)
{
    value.state = json.value ("state", bingo_room_state_t{});
}

inline void to_json (nlohmann::json &json, const bingo_reward_announced_notify_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"drawSeq", value.draw_seq},
            {"itemId", value.item_id},
            {"itemName", value.item_name},
            {"rarity", value.rarity}};
}

inline void from_json (const nlohmann::json &json, bingo_reward_announced_notify_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.draw_seq = json_value (json, "drawSeq", "draw_seq", 0);
    value.item_id = json_string (json, "itemId", "item_id");
    value.item_name = json_string (json, "itemName", "item_name");
    value.rarity = json.value ("rarity", "");
}

inline void to_json (nlohmann::json &json, const bingo_reward_acquired_event_t &value)
{
    json = {{"roomId", value.room_id},
            {"actorId", value.actor_id},
            {"drawSeq", value.draw_seq},
            {"itemId", value.item_id},
            {"itemName", value.item_name},
            {"rarity", value.rarity}};
}

inline void from_json (const nlohmann::json &json, bingo_reward_acquired_event_t &value)
{
    value.room_id = json_string (json, "roomId", "room_id");
    value.actor_id = json_string (json, "actorId", "actor_id");
    value.draw_seq = json_value (json, "drawSeq", "draw_seq", 0);
    value.item_id = json_string (json, "itemId", "item_id");
    value.item_name = json_string (json, "itemName", "item_name");
    value.rarity = json.value ("rarity", "");
}

} // namespace zlink::samples::bingo

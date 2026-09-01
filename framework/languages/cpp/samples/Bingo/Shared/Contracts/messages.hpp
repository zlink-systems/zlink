/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "bingo_messages.pb.h"

namespace zlink::samples::bingo
{

namespace pb = ::bingo::samples;

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

using authenticate_req_t = pb::AuthenticateReq;
using authenticate_res_t = pb::AuthenticateRes;
using authenticate_player_req_t = pb::AuthenticatePlayerReq;
using authenticate_player_res_t = pb::AuthenticatePlayerRes;
using get_player_record_req_t = pb::GetPlayerRecordReq;
using get_player_record_res_t = pb::GetPlayerRecordRes;
using report_bingo_result_req_t = pb::ReportBingoResultReq;
using report_bingo_result_res_t = pb::ReportBingoResultRes;
using ensure_player_actor_req_t = pb::EnsurePlayerActorReq;
using match_bingo_req_t = pb::MatchBingoReq;
using match_bingo_res_t = pb::MatchBingoRes;
using match_bingo_api_req_t = pb::MatchBingoApiReq;
using match_bingo_api_res_t = pb::MatchBingoApiRes;
using reserve_bingo_room_req_t = pb::ReserveBingoRoomReq;
using reserve_bingo_room_res_t = pb::ReserveBingoRoomRes;
using bingo_room_settings_payload_t = pb::BingoRoomSettingsPayload;
using bingo_room_create_req_t = pb::BingoRoomCreateReq;
using bingo_room_join_req_t = pb::BingoRoomJoinReq;
using bingo_room_join_res_t = pb::BingoRoomJoinRes;
using bingo_player_state_message_t = pb::BingoPlayerState;
using bingo_room_state_message_t = pb::BingoRoomState;
using submit_bingo_card_req_t = pb::SubmitBingoCardReq;
using submit_bingo_card_res_t = pb::SubmitBingoCardRes;
using observe_bingo_events_req_t = pb::ObserveBingoEventsReq;
using observe_bingo_events_res_t = pb::ObserveBingoEventsRes;
using stop_observing_bingo_events_req_t = pb::StopObservingBingoEventsReq;
using stop_observing_bingo_events_res_t = pb::StopObservingBingoEventsRes;
using observer_returned_to_entry_spot_notify_t = pb::ObserverReturnedToEntrySpotNotify;
using player_joined_notify_t = pb::PlayerJoinedNotify;
using game_started_notify_t = pb::BingoGameStartedNotify;
using number_drawn_notify_t = pb::BingoNumberDrawnNotify;
using game_ended_notify_t = pb::BingoGameEndedNotify;
using bingo_reward_announced_notify_t = pb::BingoRewardAnnouncedNotify;
using bingo_reward_acquired_event_t = pb::BingoRewardAcquiredEvent;

} // namespace zlink::samples::bingo

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Bingo의 wire codec은 Protobuf다(공통 sample spec §Bingo). 도메인 타입은 이 헤더의 변환을
 * 거쳐 `bingo_messages.proto`가 만든 message로 옮겨 실린다 — 그래서 wire 바이트가 같은 `.proto`를
 * 쓰는 다른 언어 구현과 그대로 통한다. */

#include "messages.hpp"

#include "bingo_messages.pb.h"

#include <stdexcept>
#include <string>

namespace zlink::samples::bingo
{

namespace pb = ::bingo::samples;

inline void to_protobuf (const bingo_player_state_t &value, pb::BingoPlayerState &message)
{
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
    message.set_seat (value.seat);
    message.set_is_host (value.is_host);
    for (const auto number : value.card) {
        message.add_card (number);
    }
    for (const auto mark : value.marks) {
        message.add_marks (mark);
    }
    message.set_completed_lines (value.completed_lines);
    message.set_wins (value.wins);
    message.set_losses (value.losses);
}

inline void from_protobuf (const pb::BingoPlayerState &message, bingo_player_state_t &value)
{
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
    value.seat = message.seat ();
    value.is_host = message.is_host ();
    value.card.assign (message.card ().begin (), message.card ().end ());
    value.marks.assign (message.marks ().begin (), message.marks ().end ());
    value.completed_lines = message.completed_lines ();
    value.wins = message.wins ();
    value.losses = message.losses ();
}

inline void to_protobuf (const bingo_room_state_t &value, pb::BingoRoomState &message)
{
    message.set_room_id (value.room_id);
    message.set_status (value.status);
    message.set_host_actor_id (value.host_actor_id);
    message.set_can_start (value.can_start);
    message.set_draw_seq (value.draw_seq);
    if (value.last_drawn_number) {
        message.set_last_drawn_number (*value.last_drawn_number);
    }
    for (const auto number : value.drawn_numbers) {
        message.add_drawn_numbers (number);
    }
    for (const auto &player : value.players) {
        to_protobuf (player, *message.add_players ());
    }
    for (const auto &winner : value.winners) {
        message.add_winners (winner);
    }
}

inline void from_protobuf (const pb::BingoRoomState &message, bingo_room_state_t &value)
{
    value.room_id = message.room_id ();
    value.status = message.status ();
    value.host_actor_id = message.host_actor_id ();
    value.can_start = message.can_start ();
    value.draw_seq = message.draw_seq ();
    value.last_drawn_number = message.has_last_drawn_number ()
                                ? std::optional<int> (message.last_drawn_number ())
                                : std::nullopt;
    value.drawn_numbers.assign (message.drawn_numbers ().begin (), message.drawn_numbers ().end ());
    value.players.clear ();
    value.players.reserve (message.players_size ());
    for (const auto &player : message.players ()) {
        bingo_player_state_t state;
        from_protobuf (player, state);
        value.players.push_back (std::move (state));
    }
    value.winners.assign (message.winners ().begin (), message.winners ().end ());
}

inline void to_protobuf (const authenticate_req_t &value, pb::AuthenticateReq &message)
{
    message.set_access_token (value.access_token);
}

inline void from_protobuf (const pb::AuthenticateReq &message, authenticate_req_t &value)
{
    value.access_token = message.access_token ();
}

inline void to_protobuf (const authenticate_res_t &value, pb::AuthenticateRes &message)
{
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
}

inline void from_protobuf (const pb::AuthenticateRes &message, authenticate_res_t &value)
{
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
}

inline void to_protobuf (const authenticate_player_req_t &value, pb::AuthenticatePlayerReq &message)
{
    message.set_access_token (value.access_token);
}

inline void from_protobuf (const pb::AuthenticatePlayerReq &message,
                           authenticate_player_req_t &value)
{
    value.access_token = message.access_token ();
}

inline void to_protobuf (const authenticate_player_res_t &value, pb::AuthenticatePlayerRes &message)
{
    message.set_accepted (value.accepted);
    if (value.actor_id) message.set_actor_id (*value.actor_id);
    if (value.display_name) message.set_display_name (*value.display_name);
    if (value.reason) message.set_reason (*value.reason);
}

inline void from_protobuf (const pb::AuthenticatePlayerRes &message,
                           authenticate_player_res_t &value)
{
    value.accepted = message.accepted ();
    value.actor_id = message.has_actor_id ()
                       ? std::optional<std::string> (message.actor_id ())
                       : std::nullopt;
    value.display_name = message.has_display_name ()
                            ? std::optional<std::string> (message.display_name ())
                            : std::nullopt;
    value.reason = message.has_reason ()
                     ? std::optional<std::string> (message.reason ())
                     : std::nullopt;
}

inline void to_protobuf (const get_player_record_req_t &value,
                         pb::GetPlayerRecordReq &message)
{
    message.set_actor_id (value.actor_id);
}

inline void from_protobuf (const pb::GetPlayerRecordReq &message,
                           get_player_record_req_t &value)
{
    value.actor_id = message.actor_id ();
}

inline void to_protobuf (const get_player_record_res_t &value,
                         pb::GetPlayerRecordRes &message)
{
    message.set_actor_id (value.actor_id);
    message.set_wins (value.wins);
    message.set_losses (value.losses);
}

inline void from_protobuf (const pb::GetPlayerRecordRes &message,
                           get_player_record_res_t &value)
{
    value.actor_id = message.actor_id ();
    value.wins = message.wins ();
    value.losses = message.losses ();
}

inline void to_protobuf (const report_bingo_result_req_t &value,
                         pb::ReportBingoResultReq &message)
{
    message.set_room_id (value.room_id);
    message.set_actor_id (value.actor_id);
    message.set_won (value.won);
    message.set_final_draw_seq (value.final_draw_seq);
}

inline void from_protobuf (const pb::ReportBingoResultReq &message,
                           report_bingo_result_req_t &value)
{
    value.room_id = message.room_id ();
    value.actor_id = message.actor_id ();
    value.won = message.won ();
    value.final_draw_seq = message.final_draw_seq ();
}

inline void to_protobuf (const report_bingo_result_res_t &value,
                         pb::ReportBingoResultRes &message)
{
    message.set_actor_id (value.actor_id);
    message.set_wins (value.wins);
    message.set_losses (value.losses);
}

inline void from_protobuf (const pb::ReportBingoResultRes &message,
                           report_bingo_result_res_t &value)
{
    value.actor_id = message.actor_id ();
    value.wins = message.wins ();
    value.losses = message.losses ();
}

inline void to_protobuf (const ensure_player_actor_req_t &value, pb::EnsurePlayerActorReq &message)
{
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
}

inline void from_protobuf (const pb::EnsurePlayerActorReq &message,
                           ensure_player_actor_req_t &value)
{
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
}

inline void to_protobuf (const match_bingo_req_t &value, pb::MatchBingoReq &message)
{
    message.set_mode (value.mode);
}

inline void from_protobuf (const pb::MatchBingoReq &message, match_bingo_req_t &value)
{
    value.mode = message.mode ();
}

inline void to_protobuf (const match_bingo_res_t &value, pb::MatchBingoRes &message)
{
    message.set_room_id (value.room_id);
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::MatchBingoRes &message, match_bingo_res_t &value)
{
    value.room_id = message.room_id ();
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const match_bingo_api_req_t &value, pb::MatchBingoApiReq &message)
{
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
    message.set_mode (value.mode);
}

inline void from_protobuf (const pb::MatchBingoApiReq &message, match_bingo_api_req_t &value)
{
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
    value.mode = message.mode ();
}

inline void to_protobuf (const match_bingo_api_res_t &value, pb::MatchBingoApiRes &message)
{
    message.set_room_id (value.room_id);
}

inline void from_protobuf (const pb::MatchBingoApiRes &message, match_bingo_api_res_t &value)
{
    value.room_id = message.room_id ();
}

inline void to_protobuf (const reserve_bingo_room_req_t &value, pb::ReserveBingoRoomReq &message)
{
    message.set_mode (value.mode);
    message.set_actor_id (value.actor_id);
    message.set_level_bucket (value.level_bucket);
}

inline void from_protobuf (const pb::ReserveBingoRoomReq &message,
                           reserve_bingo_room_req_t &value)
{
    value.mode = message.mode ();
    value.actor_id = message.actor_id ();
    value.level_bucket = message.level_bucket ();
}

inline void to_protobuf (const bingo_room_settings_payload_t &value,
                         pb::BingoRoomSettingsPayload &message)
{
    message.set_room_name (value.room_name);
    message.set_mode (value.mode);
    message.set_required_players (value.required_players);
    message.set_max_draw_number (value.max_draw_number);
    message.set_purpose (value.purpose);
    if (value.observed_room_id) {
        message.set_observed_room_id (*value.observed_room_id);
    }
}

inline void from_protobuf (const pb::BingoRoomSettingsPayload &message,
                           bingo_room_settings_payload_t &value)
{
    value.room_name = message.room_name ();
    value.mode = message.mode ();
    value.required_players = message.required_players ();
    value.max_draw_number = message.max_draw_number ();
    value.purpose = message.purpose ();
    value.observed_room_id = message.has_observed_room_id ()
                               ? std::optional<std::string> (message.observed_room_id ())
                               : std::nullopt;
}

inline void to_protobuf (const reserve_bingo_room_res_t &value, pb::ReserveBingoRoomRes &message)
{
    message.set_room_id (value.room_id);
    to_protobuf (value.settings, *message.mutable_settings ());
}

inline void from_protobuf (const pb::ReserveBingoRoomRes &message,
                           reserve_bingo_room_res_t &value)
{
    value.room_id = message.room_id ();
    from_protobuf (message.settings (), value.settings);
}

inline void to_protobuf (const bingo_room_join_req_t &value, pb::BingoRoomJoinReq &message)
{
    message.set_room_id (value.room_id);
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
    message.set_observe_only (value.observe_only);
}

inline void from_protobuf (const pb::BingoRoomJoinReq &message, bingo_room_join_req_t &value)
{
    value.room_id = message.room_id ();
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
    value.observe_only = message.observe_only ();
}

inline void to_protobuf (const bingo_room_join_res_t &value, pb::BingoRoomJoinRes &message)
{
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::BingoRoomJoinRes &message, bingo_room_join_res_t &value)
{
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const submit_bingo_card_req_t &value, pb::SubmitBingoCardReq &message)
{
    message.set_room_id (value.room_id);
    for (const auto number : value.card) {
        message.add_card (number);
    }
}

inline void from_protobuf (const pb::SubmitBingoCardReq &message, submit_bingo_card_req_t &value)
{
    value.room_id = message.room_id ();
    value.card.assign (message.card ().begin (), message.card ().end ());
}

inline void to_protobuf (const submit_bingo_card_res_t &value, pb::SubmitBingoCardRes &message)
{
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::SubmitBingoCardRes &message, submit_bingo_card_res_t &value)
{
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const observe_bingo_events_req_t &value, pb::ObserveBingoEventsReq &message)
{
    message.set_room_id (value.room_id);
}

inline void from_protobuf (const pb::ObserveBingoEventsReq &message,
                           observe_bingo_events_req_t &value)
{
    value.room_id = message.room_id ();
}

inline void to_protobuf (const observe_bingo_events_res_t &value, pb::ObserveBingoEventsRes &message)
{
    message.set_subscribed (value.subscribed);
}

inline void from_protobuf (const pb::ObserveBingoEventsRes &message,
                           observe_bingo_events_res_t &value)
{
    value.subscribed = message.subscribed ();
}

inline void to_protobuf (const stop_observing_bingo_events_req_t &value,
                         pb::StopObservingBingoEventsReq &message)
{
    message.set_room_id (value.room_id);
}

inline void from_protobuf (const pb::StopObservingBingoEventsReq &message,
                           stop_observing_bingo_events_req_t &value)
{
    value.room_id = message.room_id ();
}

inline void to_protobuf (const stop_observing_bingo_events_res_t &value,
                         pb::StopObservingBingoEventsRes &message)
{
    message.set_stopped (value.stopped);
}

inline void from_protobuf (const pb::StopObservingBingoEventsRes &message,
                           stop_observing_bingo_events_res_t &value)
{
    value.stopped = message.stopped ();
}

inline void to_protobuf (const player_joined_notify_t &value, pb::PlayerJoinedNotify &message)
{
    message.set_room_id (value.room_id);
    message.set_actor_id (value.actor_id);
    message.set_display_name (value.display_name);
    message.set_seat (value.seat);
    message.set_is_host (value.is_host);
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::PlayerJoinedNotify &message, player_joined_notify_t &value)
{
    value.room_id = message.room_id ();
    value.actor_id = message.actor_id ();
    value.display_name = message.display_name ();
    value.seat = message.seat ();
    value.is_host = message.is_host ();
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const game_started_notify_t &value, pb::BingoGameStartedNotify &message)
{
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::BingoGameStartedNotify &message, game_started_notify_t &value)
{
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const number_drawn_notify_t &value, pb::BingoNumberDrawnNotify &message)
{
    message.set_room_id (value.room_id);
    message.set_draw_seq (value.draw_seq);
    message.set_number (value.number);
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::BingoNumberDrawnNotify &message, number_drawn_notify_t &value)
{
    value.room_id = message.room_id ();
    value.draw_seq = message.draw_seq ();
    value.number = message.number ();
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const game_ended_notify_t &value, pb::BingoGameEndedNotify &message)
{
    to_protobuf (value.state, *message.mutable_state ());
}

inline void from_protobuf (const pb::BingoGameEndedNotify &message, game_ended_notify_t &value)
{
    from_protobuf (message.state (), value.state);
}

inline void to_protobuf (const bingo_reward_announced_notify_t &value,
                         pb::BingoRewardAnnouncedNotify &message)
{
    message.set_room_id (value.room_id);
    message.set_actor_id (value.actor_id);
    message.set_draw_seq (value.draw_seq);
    message.set_item_id (value.item_id);
    message.set_item_name (value.item_name);
    message.set_rarity (value.rarity);
}

inline void from_protobuf (const pb::BingoRewardAnnouncedNotify &message,
                           bingo_reward_announced_notify_t &value)
{
    value.room_id = message.room_id ();
    value.actor_id = message.actor_id ();
    value.draw_seq = message.draw_seq ();
    value.item_id = message.item_id ();
    value.item_name = message.item_name ();
    value.rarity = message.rarity ();
}

inline void to_protobuf (const bingo_reward_acquired_event_t &value,
                         pb::BingoRewardAcquiredEvent &message)
{
    message.set_room_id (value.room_id);
    message.set_actor_id (value.actor_id);
    message.set_draw_seq (value.draw_seq);
    message.set_item_id (value.item_id);
    message.set_item_name (value.item_name);
    message.set_rarity (value.rarity);
}

inline void from_protobuf (const pb::BingoRewardAcquiredEvent &message,
                           bingo_reward_acquired_event_t &value)
{
    value.room_id = message.room_id ();
    value.actor_id = message.actor_id ();
    value.draw_seq = message.draw_seq ();
    value.item_id = message.item_id ();
    value.item_name = message.item_name ();
    value.rarity = message.rarity ();
}

/* stream connector는 typed payload를 실을 때 ADL로 아래 훅을 찾는다. 훅이 있으면 JSON 대신 이
 * 인코딩을 쓰므로, client stream의 wire도 서버 간 wire와 같은 protobuf가 된다. */
#define ZLINK_BINGO_STREAM_PAYLOAD(payload_type, message_type)                                    \
    inline zlink::message_t to_stream_payload (const payload_type &value)                         \
    {                                                                                             \
        ::bingo::samples::message_type message;                                                   \
        to_protobuf (value, message);                                                             \
        return zlink::message_t::from (message.SerializeAsString ());                             \
    }                                                                                             \
    inline void from_stream_payload (const zlink::message_t &payload, payload_type &value)        \
    {                                                                                             \
        ::bingo::samples::message_type message;                                                   \
        if (!message.ParseFromString (payload.to_string ())) {                                    \
            throw std::runtime_error ("bingo protobuf payload is invalid: " #message_type);       \
        }                                                                                         \
        from_protobuf (message, value);                                                           \
    }

ZLINK_BINGO_STREAM_PAYLOAD (authenticate_req_t, AuthenticateReq)
ZLINK_BINGO_STREAM_PAYLOAD (authenticate_res_t, AuthenticateRes)
ZLINK_BINGO_STREAM_PAYLOAD (authenticate_player_req_t, AuthenticatePlayerReq)
ZLINK_BINGO_STREAM_PAYLOAD (authenticate_player_res_t, AuthenticatePlayerRes)
ZLINK_BINGO_STREAM_PAYLOAD (get_player_record_req_t, GetPlayerRecordReq)
ZLINK_BINGO_STREAM_PAYLOAD (get_player_record_res_t, GetPlayerRecordRes)
ZLINK_BINGO_STREAM_PAYLOAD (report_bingo_result_req_t, ReportBingoResultReq)
ZLINK_BINGO_STREAM_PAYLOAD (report_bingo_result_res_t, ReportBingoResultRes)
ZLINK_BINGO_STREAM_PAYLOAD (ensure_player_actor_req_t, EnsurePlayerActorReq)
ZLINK_BINGO_STREAM_PAYLOAD (match_bingo_req_t, MatchBingoReq)
ZLINK_BINGO_STREAM_PAYLOAD (match_bingo_res_t, MatchBingoRes)
ZLINK_BINGO_STREAM_PAYLOAD (match_bingo_api_req_t, MatchBingoApiReq)
ZLINK_BINGO_STREAM_PAYLOAD (match_bingo_api_res_t, MatchBingoApiRes)
ZLINK_BINGO_STREAM_PAYLOAD (reserve_bingo_room_req_t, ReserveBingoRoomReq)
ZLINK_BINGO_STREAM_PAYLOAD (reserve_bingo_room_res_t, ReserveBingoRoomRes)
ZLINK_BINGO_STREAM_PAYLOAD (bingo_room_settings_payload_t, BingoRoomSettingsPayload)
ZLINK_BINGO_STREAM_PAYLOAD (bingo_room_join_req_t, BingoRoomJoinReq)
ZLINK_BINGO_STREAM_PAYLOAD (bingo_room_join_res_t, BingoRoomJoinRes)
ZLINK_BINGO_STREAM_PAYLOAD (submit_bingo_card_req_t, SubmitBingoCardReq)
ZLINK_BINGO_STREAM_PAYLOAD (submit_bingo_card_res_t, SubmitBingoCardRes)
ZLINK_BINGO_STREAM_PAYLOAD (observe_bingo_events_req_t, ObserveBingoEventsReq)
ZLINK_BINGO_STREAM_PAYLOAD (observe_bingo_events_res_t, ObserveBingoEventsRes)
ZLINK_BINGO_STREAM_PAYLOAD (stop_observing_bingo_events_req_t, StopObservingBingoEventsReq)
ZLINK_BINGO_STREAM_PAYLOAD (stop_observing_bingo_events_res_t, StopObservingBingoEventsRes)
ZLINK_BINGO_STREAM_PAYLOAD (player_joined_notify_t, PlayerJoinedNotify)
ZLINK_BINGO_STREAM_PAYLOAD (game_started_notify_t, BingoGameStartedNotify)
ZLINK_BINGO_STREAM_PAYLOAD (number_drawn_notify_t, BingoNumberDrawnNotify)
ZLINK_BINGO_STREAM_PAYLOAD (game_ended_notify_t, BingoGameEndedNotify)
ZLINK_BINGO_STREAM_PAYLOAD (bingo_reward_announced_notify_t, BingoRewardAnnouncedNotify)
ZLINK_BINGO_STREAM_PAYLOAD (bingo_reward_acquired_event_t, BingoRewardAcquiredEvent)
#undef ZLINK_BINGO_STREAM_PAYLOAD

} // namespace zlink::samples::bingo

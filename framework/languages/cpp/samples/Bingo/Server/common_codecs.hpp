/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Shared/Contracts/messages.hpp"
#include "../Shared/Contracts/protobuf_conversions.hpp"

#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>

namespace zlink::samples::bingo
{

/* Bingo의 payload codec은 Protobuf다. 도메인 타입은 `bingo_messages.proto`가 만든 message로
 * 옮겨 실리므로 wire 바이트가 같은 스키마를 쓰는 다른 언어 구현과 그대로 통한다. */
struct bingo_protobuf_codecs_t
{
    template <typename TRegistrar> void register_framework_codecs (TRegistrar &registrar) const
    {
#define ZLINK_BINGO_REGISTER_PROTOBUF(payload_type, message_type)                                 \
    zlink::framework_codecs::protobuf_codec_extension_t::register_payload_serializer<             \
      payload_type, ::bingo::samples::message_type> (registrar)
        ZLINK_BINGO_REGISTER_PROTOBUF (authenticate_req_t, AuthenticateReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (authenticate_res_t, AuthenticateRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (authenticate_player_req_t, AuthenticatePlayerReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (authenticate_player_res_t, AuthenticatePlayerRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (get_player_record_req_t, GetPlayerRecordReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (get_player_record_res_t, GetPlayerRecordRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (report_bingo_result_req_t, ReportBingoResultReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (report_bingo_result_res_t, ReportBingoResultRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (ensure_player_actor_req_t, EnsurePlayerActorReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (match_bingo_req_t, MatchBingoReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (match_bingo_res_t, MatchBingoRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (match_bingo_api_req_t, MatchBingoApiReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (match_bingo_api_res_t, MatchBingoApiRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (reserve_bingo_room_req_t, ReserveBingoRoomReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (reserve_bingo_room_res_t, ReserveBingoRoomRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (bingo_room_settings_payload_t, BingoRoomSettingsPayload);
        ZLINK_BINGO_REGISTER_PROTOBUF (bingo_room_join_req_t, BingoRoomJoinReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (bingo_room_join_res_t, BingoRoomJoinRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (submit_bingo_card_req_t, SubmitBingoCardReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (submit_bingo_card_res_t, SubmitBingoCardRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (observe_bingo_events_req_t, ObserveBingoEventsReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (observe_bingo_events_res_t, ObserveBingoEventsRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (stop_observing_bingo_events_req_t,
                                       StopObservingBingoEventsReq);
        ZLINK_BINGO_REGISTER_PROTOBUF (stop_observing_bingo_events_res_t,
                                       StopObservingBingoEventsRes);
        ZLINK_BINGO_REGISTER_PROTOBUF (player_joined_notify_t, PlayerJoinedNotify);
        ZLINK_BINGO_REGISTER_PROTOBUF (game_started_notify_t, BingoGameStartedNotify);
        ZLINK_BINGO_REGISTER_PROTOBUF (number_drawn_notify_t, BingoNumberDrawnNotify);
        ZLINK_BINGO_REGISTER_PROTOBUF (game_ended_notify_t, BingoGameEndedNotify);
        ZLINK_BINGO_REGISTER_PROTOBUF (bingo_reward_announced_notify_t,
                                       BingoRewardAnnouncedNotify);
        ZLINK_BINGO_REGISTER_PROTOBUF (bingo_reward_acquired_event_t, BingoRewardAcquiredEvent);
#undef ZLINK_BINGO_REGISTER_PROTOBUF
    }
};

inline void use_default_bingo_codecs (framework::codec_options_builder_t codecs)
{
    codecs.use (bingo_protobuf_codecs_t{});
}

} // namespace zlink::samples::bingo

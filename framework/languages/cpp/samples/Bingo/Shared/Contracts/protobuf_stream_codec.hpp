/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* client(stream connector)도 Bingo payload를 Protobuf로 싣는다. connector의 typed 경로는
 * `codec_traits<T>`를 보므로, 도메인 타입마다 protobuf 인코딩으로 특수화한다. */

#include "protobuf_conversions.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>

namespace zlink::stream_connector::codecs
{

#define ZLINK_BINGO_PROTOBUF_CODEC_TRAITS(payload_type)                                           \
    template <> struct codec_traits<payload_type>                                                 \
    {                                                                                             \
        static constexpr codec_t codec = codec_t::protobuf;                                       \
                                                                                                  \
        static zlink::message_t encode (const payload_type &value)                                \
        {                                                                                         \
            return to_stream_payload (value);                                                     \
        }                                                                                         \
                                                                                                  \
        static payload_type decode (const zlink::message_t &message)                              \
        {                                                                                         \
            payload_type value;                                                                   \
            from_stream_payload (message, value);                                                 \
            return value;                                                                         \
        }                                                                                         \
    };

ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::authenticate_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::authenticate_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::authenticate_player_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::authenticate_player_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::ensure_player_actor_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::match_bingo_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::match_bingo_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::match_bingo_api_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::match_bingo_api_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::reserve_bingo_room_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::reserve_bingo_room_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::bingo_room_settings_payload_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::bingo_room_join_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::bingo_room_join_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::submit_bingo_card_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::submit_bingo_card_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::observe_bingo_events_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::observe_bingo_events_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::stop_observing_bingo_events_req_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::stop_observing_bingo_events_res_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::player_joined_notify_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::game_started_notify_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::number_drawn_notify_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::game_ended_notify_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::bingo_reward_announced_notify_t)
ZLINK_BINGO_PROTOBUF_CODEC_TRAITS (zlink::samples::bingo::bingo_reward_acquired_event_t)
#undef ZLINK_BINGO_PROTOBUF_CODEC_TRAITS

} // namespace zlink::stream_connector::codecs

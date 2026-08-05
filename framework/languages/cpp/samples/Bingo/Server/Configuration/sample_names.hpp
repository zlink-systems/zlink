/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::samples::bingo
{

struct sample_names_t
{
    static constexpr const char *api_channel = "bingo.api";
    static constexpr const char *play_channel = "bingo.play";
    static constexpr const char *router_channel = "bingo.gateway";
    static constexpr const char *stream_node = "bingo.client.stream";
    static constexpr const char *session_spot_node = "bingo.session.node";
    static constexpr const char *player_actor_type = "bingo.player";
    static constexpr const char *room_spot_node = "bingo.room.node";
    static constexpr const char *room_spot = "bingo.room";
    static constexpr const char *room_spot_mesh = "bingo.play";
    static constexpr const char *matchmaking_mesh = "bingo.matchmaking";
    static constexpr const char *matchmaker_spot = "bingo.matchmaker";
    static constexpr const char *reward_topic = "bingo.room.reward";
    static constexpr const char *player_joined_packet = "PlayerJoinedNotify";
    static constexpr const char *game_started_packet = "BingoGameStartedNotify";
    static constexpr const char *number_drawn_packet = "BingoNumberDrawnNotify";
    static constexpr const char *game_ended_packet = "BingoGameEndedNotify";
    static constexpr const char *reward_announced_packet = "BingoRewardAnnouncedNotify";
};

} // namespace zlink::samples::bingo

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::samples::gamequest
{

struct sample_names_t
{
    static constexpr const char *quest_owner_channel = "gamequest.quest.owner";
    static constexpr const char *quest_spot_route_channel_prefix =
      "gamequest.quest.spot.route.";
    static constexpr const char *quest_spot_discovery = "gamequest.quest.spot";
    static constexpr const char *quest_spot_route = "gamequest.quest.spot.route";
    static constexpr const char *player_quest_spot = "gamequest.player.quest";
    static constexpr const char *stream_node = "gamequest.stream";
    static constexpr const char *mission_a_rid = "gamequest-mission-a";
    static constexpr const char *mission_b_rid = "gamequest-mission-b";
};

/* API 노드마다 자기 이름의 spot mesh를 연다. owner spot이 notify를 route하려면 이 mesh 이름에
 * route 채널을 매핑해야 한다. */
inline std::string api_spot_mesh_for (const std::string &api_name)
{
    return std::string (sample_names_t::quest_spot_discovery) + "." + api_name;
}

inline std::string quest_spot_route_channel_for (const std::string &instance_id)
{
    return std::string (sample_names_t::quest_spot_route_channel_prefix) + instance_id;
}

} // namespace zlink::samples::gamequest

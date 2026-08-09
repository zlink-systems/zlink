/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Contracts/messages.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace zlink::samples::zoneworld
{

struct names_t
{
    static constexpr const char *mesh = "zoneworld.mesh";
    static constexpr const char *zone_channel = "zoneworld.zones";
    static constexpr const char *broadcast_channel = "zoneworld.broadcast";
    static constexpr const char *report_channel = "zoneworld.report";
    static constexpr const char *player_actor = "zoneworld.player";
    static constexpr const char *zone_spot = "zoneworld.zone";
    static constexpr const char *gateway_stream = "zoneworld.gateway";
    static constexpr const char *ops_stream = "zoneworld.ops";
    static constexpr const char *announce_topic = "world.announce";
    static constexpr const char *maintenance_topic = "world.maintenance";
};

struct reject_reason_t
{
    static constexpr const char *out_of_range = "OutOfRange";
    static constexpr const char *too_far = "TooFar";
    static constexpr const char *diagonal_crossing = "DiagonalCrossing";
    static constexpr const char *zone_maintenance = "ZoneMaintenance";
};

inline std::string zone_of (int x, int y)
{
    return y < 50 ? (x < 50 ? "zone-nw" : "zone-ne")
                  : (x < 50 ? "zone-sw" : "zone-se");
}

inline std::optional<std::string> validate_move (int from_x, int from_y, int x, int y,
                                                 bool target_maintenance)
{
    if (x < 0 || x >= 100 || y < 0 || y >= 100) return reject_reason_t::out_of_range;
    if (std::abs (x - from_x) > 5 || std::abs (y - from_y) > 5)
        return reject_reason_t::too_far;
    if (zone_of (from_x, from_y) != zone_of (x, y)
        && (from_x < 50) != (x < 50) && (from_y < 50) != (y < 50))
        return reject_reason_t::diagonal_crossing;
    if (target_maintenance) return reject_reason_t::zone_maintenance;
    return std::nullopt;
}

inline std::vector<std::string> adjacent_zones (const std::string &zone)
{
    if (zone == "zone-nw") return {"zone-ne", "zone-sw"};
    if (zone == "zone-ne") return {"zone-nw", "zone-se"};
    if (zone == "zone-sw") return {"zone-nw", "zone-se"};
    return {"zone-ne", "zone-sw"};
}

inline std::string border_topic (const std::string &from, const std::string &to)
{
    return "zone.border." + from + "." + to;
}

inline void sort_players (std::vector<player_view_t> &players)
{
    std::sort (players.begin (), players.end (), [] (const auto &left, const auto &right) {
        return left.player_id < right.player_id;
    });
}

} // namespace zlink::samples::zoneworld

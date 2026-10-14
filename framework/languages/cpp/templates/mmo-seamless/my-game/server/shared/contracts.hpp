#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto zone = "Zone";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto border = "border";
    static constexpr auto stream = "players";
    static constexpr auto timer = "border-tick";
    static constexpr auto zone_prefix = "zone-";
    static constexpr int zone_width = 100;
    static constexpr int zone_count = 6;
    static constexpr int border_radius = 10;
    static constexpr auto tick_period = std::chrono::milliseconds (100);
    static std::string zone_id (int x)
    {
        if (x < 0 || x >= zone_width * zone_count)
            throw std::out_of_range ("Position outside world");
        return zone_prefix + std::to_string (x / zone_width + 1);
    }
    static std::vector<std::string> adjacent (const std::string &zone_id)
    {
        auto n = std::stoi (zone_id.substr (std::string (zone_prefix).size ()));
        std::vector<std::string> result;
        if (n > 1)
            result.push_back (zone_prefix + std::to_string (n - 1));
        if (n < zone_count)
            result.push_back (zone_prefix + std::to_string (n + 1));
        return result;
    }
    static std::string topic (const std::string &from, const std::string &to)
    {
        return from + ":" + to;
    }
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
struct Control
{
    static constexpr auto ready = "READY ";
    static constexpr auto drain = "drain";
    static constexpr auto shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED";
    static constexpr auto relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN";
    static constexpr auto stopped = "STOPPED";
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};
inline void to_json (nlohmann::json &j, const SessionInfoRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, SessionInfoRequest &)
{
}
struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    int x = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinWorldRequest, playerId, x)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, x, sequence)
};
struct EnterZone
{
    static constexpr auto packet_name = "EnterZone";
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterZone, x, sequence)
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string zoneId, nodeId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (WorldState, zoneId, nodeId, x, sequence)
};
struct BorderPresence
{
    static constexpr auto packet_name = "BorderPresence";
    std::string sourceZoneId, actorId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (BorderPresence, sourceZoneId, actorId, x, sequence)
};
struct BorderVisible
{
    static constexpr auto packet_name = "BorderVisible";
    std::string observerZoneId, sourceZoneId, actorId;
    int x = 0, sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (
      BorderVisible, observerZoneId, sourceZoneId, actorId, x, sequence)
};
struct OpenDungeonRequest
{
    static constexpr auto packet_name = "OpenDungeonRequest";
    std::string dungeonId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (OpenDungeonRequest, dungeonId)
};
struct DungeonOpened
{
    static constexpr auto packet_name = "DungeonOpened";
    std::string dungeonId, nodeId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonOpened, dungeonId, nodeId)
};
}

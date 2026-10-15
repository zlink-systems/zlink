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
    static constexpr auto region = "Region";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto stream = "players";
    static constexpr auto region_full = "region-full";
    static constexpr int first_index = 1;
    static constexpr int default_capacity = 2;
    static constexpr int default_region_limit = 2;
    static std::string region_id (const std::string &name, int index)
    {
        if (name.find_first_not_of (" \t\r\n") == std::string::npos || index < first_index)
            throw std::invalid_argument ("Invalid region selection");
        return name + "-" + std::to_string (index);
    }
};
struct Limits
{
    static constexpr auto isolation = std::chrono::milliseconds (300);
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
struct EnterRegionRequest
{
    static constexpr auto packet_name = "EnterRegionRequest";
    std::string playerId, region;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterRegionRequest, playerId, region)
};
struct JoinRegion
{
    static constexpr auto packet_name = "JoinRegion";
    std::string region;
    int index = Names::first_index;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinRegion, region, index)
};
struct RegionFull
{
    static constexpr auto packet_name = "RegionFull";
    std::string reason, region;
    int index = Names::first_index;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RegionFull, reason, region, index)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    int x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, x, sequence)
};
struct PlayerPosition
{
    std::string playerId;
    int x = 0;
    bool operator== (const PlayerPosition &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerPosition, playerId, x)
};
struct RegionView
{
    static constexpr auto packet_name = "RegionView";
    std::string regionId, nodeId;
    std::vector<PlayerPosition> players;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RegionView, regionId, nodeId, players)
};
struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};
inline void to_json (nlohmann::json &j, const ViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, ViewRequest &)
{
}
struct DungeonInfoRequest
{
    static constexpr auto packet_name = "DungeonInfoRequest";
};
inline void to_json (nlohmann::json &j, const DungeonInfoRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, DungeonInfoRequest &)
{
}
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

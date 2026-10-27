#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
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
    static constexpr int max_replicas_per_region = 2;
    static constexpr int max_step = 10;
    static constexpr int region_min_x = 0;
    static constexpr int region_max_x = 100;
    static constexpr double max_speed = max_step * 5.0;
    static constexpr std::int64_t max_safe_integer = 9'007'199'254'740'991LL;
    static constexpr std::array<std::string_view, 2> allowed_regions{"forest", "desert"};
    static constexpr int first_index = 1;
    static constexpr int default_capacity = 2;
    static constexpr int default_region_limit = 2;
    static std::string region_id (const std::string &name, int index)
    {
        if (name.find_first_not_of (" \t\r\n") == std::string::npos || index < first_index)
            throw std::invalid_argument ("Invalid region selection");
        return name + "-" + std::to_string (index);
    }
    static bool is_allowed_region (std::string_view name)
    {
        return std::find (allowed_regions.begin (), allowed_regions.end (), name)
               != allowed_regions.end ();
    }
};
struct Rejection
{
    static constexpr auto packet_not_allowed = "packet-not-allowed";
    static constexpr auto non_finite = "non-finite";
    static constexpr auto step_too_large = "step-too-large";
    static constexpr auto outside_region = "outside-region";
    static constexpr auto move_budget = "move-budget";
    static constexpr auto stale_sequence = "stale-sequence";
    static constexpr auto region_unknown = "region-unknown";
    static constexpr auto capacity_exhausted = "capacity-exhausted";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
};
struct Limits
{
    static constexpr auto isolation = std::chrono::milliseconds (300);
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe_observation = message_follow + relocation;
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
    std::string SessionId, ActorId;
    std::uint64_t ObjectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    friend void to_json (nlohmann::json &j, const SessionInfo &value)
    {
        j = nlohmann::json{{"sessionId", value.SessionId}, {"actorId", value.ActorId}, {"objectGeneration", value.ObjectGeneration}};
    }
    friend void from_json (const nlohmann::json &j, SessionInfo &value)
    {
        j.at ("sessionId").get_to (value.SessionId);
        j.at ("actorId").get_to (value.ActorId);
        j.at ("objectGeneration").get_to (value.ObjectGeneration);
    }
};
struct EnterRegionRequest
{
    static constexpr auto packet_name = "EnterRegionRequest";
    std::string PlayerId, Region;
    friend void to_json (nlohmann::json &j, const EnterRegionRequest &value)
    {
        j = nlohmann::json{{"playerId", value.PlayerId}, {"region", value.Region}};
    }
    friend void from_json (const nlohmann::json &j, EnterRegionRequest &value)
    {
        j.at ("playerId").get_to (value.PlayerId);
        j.at ("region").get_to (value.Region);
    }
};
struct JoinRegion
{
    static constexpr auto packet_name = "JoinRegion";
    std::string Region;
    int Index = Names::first_index;
    friend void to_json (nlohmann::json &j, const JoinRegion &value)
    {
        j = nlohmann::json{{"region", value.Region}, {"index", value.Index}};
    }
    friend void from_json (const nlohmann::json &j, JoinRegion &value)
    {
        j.at ("region").get_to (value.Region);
        j.at ("index").get_to (value.Index);
    }
};
struct RegionFull
{
    static constexpr auto packet_name = "RegionFull";
    std::string Reason, Region;
    int Index = Names::first_index;
    friend void to_json (nlohmann::json &j, const RegionFull &value)
    {
        j = nlohmann::json{{"reason", value.Reason}, {"region", value.Region}, {"index", value.Index}};
    }
    friend void from_json (const nlohmann::json &j, RegionFull &value)
    {
        j.at ("reason").get_to (value.Reason);
        j.at ("region").get_to (value.Region);
        j.at ("index").get_to (value.Index);
    }
};
struct RegionUnavailable
{
    static constexpr auto packet_name = "RegionUnavailable";
    std::string Region, Reason;
    friend void to_json (nlohmann::json &j, const RegionUnavailable &value)
    {
        j = nlohmann::json{{"region", value.Region}, {"reason", value.Reason}};
    }
    friend void from_json (const nlohmann::json &j, RegionUnavailable &value)
    {
        j.at ("region").get_to (value.Region);
        j.at ("reason").get_to (value.Reason);
    }
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double Delta = 0;
    std::int64_t Sequence = 0;
    friend void to_json (nlohmann::json &j, const MoveRequest &value)
    {
        j = nlohmann::json{{"delta", value.Delta}, {"sequence", value.Sequence}};
    }
    friend void from_json (const nlohmann::json &j, MoveRequest &value)
    {
        j.at ("delta").get_to (value.Delta);
        j.at ("sequence").get_to (value.Sequence);
    }
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string Reason;
    std::int32_t RetryAfterMs = 0;
    bool operator== (const ActionRejected &) const = default;
    friend void to_json (nlohmann::json &j, const ActionRejected &value)
    {
        j = nlohmann::json{{"reason", value.Reason}, {"retryAfterMs", value.RetryAfterMs}};
    }
    friend void from_json (const nlohmann::json &j, ActionRejected &value)
    {
        j.at ("reason").get_to (value.Reason);
        j.at ("retryAfterMs").get_to (value.RetryAfterMs);
    }
};
struct PlayerPosition
{
    std::string PlayerId;
    double X = 0;
    std::int64_t Sequence = 0;
    bool operator== (const PlayerPosition &) const = default;
    friend void to_json (nlohmann::json &j, const PlayerPosition &value)
    {
        j = nlohmann::json{{"playerId", value.PlayerId}, {"x", value.X}, {"sequence", value.Sequence}};
    }
    friend void from_json (const nlohmann::json &j, PlayerPosition &value)
    {
        j.at ("playerId").get_to (value.PlayerId);
        j.at ("x").get_to (value.X);
        j.at ("sequence").get_to (value.Sequence);
    }
};
struct RegionView
{
    static constexpr auto packet_name = "RegionView";
    std::string RegionId, NodeId;
    std::vector<PlayerPosition> Players;
    friend void to_json (nlohmann::json &j, const RegionView &value)
    {
        j = nlohmann::json{{"regionId", value.RegionId}, {"nodeId", value.NodeId}, {"players", value.Players}};
    }
    friend void from_json (const nlohmann::json &j, RegionView &value)
    {
        j.at ("regionId").get_to (value.RegionId);
        j.at ("nodeId").get_to (value.NodeId);
        j.at ("players").get_to (value.Players);
    }
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
    std::string DungeonId;
    friend void to_json (nlohmann::json &j, const OpenDungeonRequest &value)
    {
        j = nlohmann::json{{"dungeonId", value.DungeonId}};
    }
    friend void from_json (const nlohmann::json &j, OpenDungeonRequest &value)
    {
        j.at ("dungeonId").get_to (value.DungeonId);
    }
};
struct DungeonOpened
{
    static constexpr auto packet_name = "DungeonOpened";
    std::string DungeonId, NodeId;
    friend void to_json (nlohmann::json &j, const DungeonOpened &value)
    {
        j = nlohmann::json{{"dungeonId", value.DungeonId}, {"nodeId", value.NodeId}};
    }
    friend void from_json (const nlohmann::json &j, DungeonOpened &value)
    {
        j.at ("dungeonId").get_to (value.DungeonId);
        j.at ("nodeId").get_to (value.NodeId);
    }
};
}

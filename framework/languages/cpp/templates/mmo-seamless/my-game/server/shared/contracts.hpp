#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
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
    static constexpr auto zone_prefix = "zone-";
    static constexpr int zone_width = 100;
    static constexpr int zone_count = 6;
    static constexpr int border_radius = 10;
    static constexpr int world_width = zone_width * zone_count;
    static std::string zone_id (double x)
    {
        return zone_prefix + std::to_string (static_cast<int> (x / zone_width) + 1);
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
    static constexpr std::int64_t max_safe_integer = 9'007'199'254'740'991;
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
struct Control
{
    static constexpr int replacement_x = 0;
    static constexpr auto vacate_zone = "vacate-zone";
    static constexpr auto zone_vacated = "ZONE_VACATED";
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
    std::string sessionId;
    std::string actorId;
    std::int64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    double x = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinWorldRequest, playerId, x)
};
struct Position
{
    double x = 0;
    bool operator== (const Position &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Position, x)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double delta = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, delta, sequence)
};
struct EnterZone
{
    static constexpr auto packet_name = "EnterZone";
    Position position;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterZone, position, sequence, remainingMs)
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string zoneId, nodeId;
    double x = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (WorldState, zoneId, nodeId, x, sequence)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason, retryAfterMs)
};
enum class PlayerLeftReason
{
    left_zone,
    disconnected,
};
NLOHMANN_JSON_SERIALIZE_ENUM (PlayerLeftReason,
                              {{PlayerLeftReason::left_zone, "left-zone"},
                               {PlayerLeftReason::disconnected, "disconnected"}})
enum class BorderPresenceKind
{
    appeared,
    moved,
    left,
};
NLOHMANN_JSON_SERIALIZE_ENUM (BorderPresenceKind,
                              {{BorderPresenceKind::appeared, "appeared"},
                               {BorderPresenceKind::moved, "moved"},
                               {BorderPresenceKind::left, "left"}})
struct PlayerAppeared
{
    static constexpr auto packet_name = "PlayerAppeared";
    std::string playerId;
    Position position;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerAppeared, playerId, position)
};
struct PlayerMoved
{
    static constexpr auto packet_name = "PlayerMoved";
    std::string playerId;
    Position position;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerMoved, playerId, position, sequence)
};
struct PlayerLeft
{
    static constexpr auto packet_name = "PlayerLeft";
    std::string playerId;
    PlayerLeftReason reason = PlayerLeftReason::left_zone;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerLeft, playerId, reason)
};
struct BorderPresence
{
    static constexpr auto packet_name = "BorderPresence";
    std::string sourceZoneId, playerId;
    Position position;
    std::int64_t sequence = 0;
    BorderPresenceKind kind = BorderPresenceKind::appeared;
    std::optional<PlayerLeftReason> reason;
};
inline void to_json (nlohmann::json &j, const BorderPresence &value)
{
    j = { {"sourceZoneId", value.sourceZoneId},
          {"playerId", value.playerId},
          {"position", value.position},
          {"sequence", value.sequence},
          {"kind", value.kind},
          {"reason", value.reason ? nlohmann::json (*value.reason) : nlohmann::json (nullptr)} };
}
inline void from_json (const nlohmann::json &j, BorderPresence &value)
{
    j.at ("sourceZoneId").get_to (value.sourceZoneId);
    j.at ("playerId").get_to (value.playerId);
    j.at ("position").get_to (value.position);
    j.at ("sequence").get_to (value.sequence);
    j.at ("kind").get_to (value.kind);
    const auto &reason = j.at ("reason");
    value.reason = reason.is_null () ? std::nullopt
                                    : std::optional<PlayerLeftReason> (
                                        reason.get<PlayerLeftReason> ());
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

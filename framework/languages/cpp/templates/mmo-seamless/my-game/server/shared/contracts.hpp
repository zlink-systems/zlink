#pragma once
#include <zlink/json.hpp>
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

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId;
    std::string actorId;
    std::int64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
};
struct JoinWorldRequest
{
    static constexpr auto packet_name = "JoinWorldRequest";
    std::string playerId;
    double x = 0;
};
struct Position
{
    double x = 0;
    bool operator== (const Position &) const = default;
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double delta = 0;
    std::int64_t sequence = 0;
};
struct EnterZone
{
    static constexpr auto packet_name = "EnterZone";
    Position position;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
};
struct WorldState
{
    static constexpr auto packet_name = "WorldState";
    std::string zoneId, nodeId;
    double x = 0;
    std::int64_t sequence = 0;
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
};
enum class PlayerLeftReason
{
    left_zone,
    disconnected,
    out_of_range,
};

enum class BorderPresenceKind
{
    appeared,
    moved,
    left,
};

struct PlayerAppeared
{
    static constexpr auto packet_name = "PlayerAppeared";
    std::string playerId;
    Position position;
    std::string sourceSpotId;
};
struct PlayerMoved
{
    static constexpr auto packet_name = "PlayerMoved";
    std::string playerId;
    Position position;
    std::int64_t sequence = 0;
    std::string sourceSpotId;
};
struct PlayerLeft
{
    static constexpr auto packet_name = "PlayerLeft";
    std::string playerId;
    PlayerLeftReason reason = PlayerLeftReason::left_zone;
    std::string sourceSpotId;
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

struct OpenDungeonRequest
{
    static constexpr auto packet_name = "OpenDungeonRequest";
    std::string dungeonId;
};
struct DungeonOpened
{
    static constexpr auto packet_name = "DungeonOpened";
    std::string dungeonId, nodeId;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::JoinWorldRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinWorldRequest::playerId),
      zlink::json::field ("x", &::MyGame::JoinWorldRequest::x)
    };
};

template <> struct dto_traits<::MyGame::Position>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("x", &::MyGame::Position::x)
    };
};

template <> struct dto_traits<::MyGame::MoveRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("delta", &::MyGame::MoveRequest::delta),
      zlink::json::field ("sequence", &::MyGame::MoveRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::EnterZone>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("position", &::MyGame::EnterZone::position),
      zlink::json::field ("sequence", &::MyGame::EnterZone::sequence),
      zlink::json::field ("remainingMs", &::MyGame::EnterZone::remainingMs)
    };
};

template <> struct dto_traits<::MyGame::WorldState>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("zoneId", &::MyGame::WorldState::zoneId),
      zlink::json::field ("nodeId", &::MyGame::WorldState::nodeId),
      zlink::json::field ("x", &::MyGame::WorldState::x),
      zlink::json::field ("sequence", &::MyGame::WorldState::sequence)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::PlayerAppeared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerAppeared::playerId),
      zlink::json::field ("position", &::MyGame::PlayerAppeared::position),
      zlink::json::field ("sourceSpotId", &::MyGame::PlayerAppeared::sourceSpotId)
    };
};

template <> struct dto_traits<::MyGame::PlayerMoved>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerMoved::playerId),
      zlink::json::field ("position", &::MyGame::PlayerMoved::position),
      zlink::json::field ("sequence", &::MyGame::PlayerMoved::sequence),
      zlink::json::field ("sourceSpotId", &::MyGame::PlayerMoved::sourceSpotId)
    };
};

template <> struct dto_traits<::MyGame::PlayerLeft>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerLeft::playerId),
      zlink::json::field ("reason", &::MyGame::PlayerLeft::reason),
      zlink::json::field ("sourceSpotId", &::MyGame::PlayerLeft::sourceSpotId)
    };
};

template <> struct dto_traits<::MyGame::OpenDungeonRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::OpenDungeonRequest::dungeonId)
    };
};

template <> struct dto_traits<::MyGame::DungeonOpened>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonOpened::dungeonId),
      zlink::json::field ("nodeId", &::MyGame::DungeonOpened::nodeId)
    };
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::BorderPresence>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sourceZoneId", &::MyGame::BorderPresence::sourceZoneId),
      zlink::json::field ("playerId", &::MyGame::BorderPresence::playerId),
      zlink::json::field ("position", &::MyGame::BorderPresence::position),
      zlink::json::field ("sequence", &::MyGame::BorderPresence::sequence),
      zlink::json::field ("kind", &::MyGame::BorderPresence::kind),
      zlink::json::field ("reason", &::MyGame::BorderPresence::reason)
    };
};

template <> struct enum_traits<::MyGame::PlayerLeftReason>
{
    static constexpr auto values = std::array{
      zlink::json::enum_value (::MyGame::PlayerLeftReason::left_zone, "left-zone"),
      zlink::json::enum_value (::MyGame::PlayerLeftReason::disconnected, "disconnected"),
      zlink::json::enum_value (::MyGame::PlayerLeftReason::out_of_range, "out-of-range")
    };
};

template <> struct enum_traits<::MyGame::BorderPresenceKind>
{
    static constexpr auto values = std::array{
      zlink::json::enum_value (::MyGame::BorderPresenceKind::appeared, "appeared"),
      zlink::json::enum_value (::MyGame::BorderPresenceKind::moved, "moved"),
      zlink::json::enum_value (::MyGame::BorderPresenceKind::left, "left")
    };
};
} // namespace zlink::json

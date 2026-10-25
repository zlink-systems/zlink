#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
namespace detail
{
template <class T> nlohmann::json to_nullable_json (const std::optional<T> &value)
{
    return value ? nlohmann::json (*value) : nlohmann::json (nullptr);
}
template <class T> void from_nullable_json (const nlohmann::json &source, std::optional<T> &value)
{
    value = source.is_null () ? std::nullopt : std::optional<T> (source.get<T> ());
}
}

struct Names
{
    static constexpr auto mesh = "my-game-arena", stream = "players", player = "Player";
    static constexpr auto room = "Room", matchmaking = "Matchmaking",
                          matchmaking_id = "matchmaking";
    static constexpr auto default_mode = "duel", room_prefix = "room-";
};

struct Game
{
    static constexpr std::size_t players_per_room = 2;
    static constexpr int initial_hp = 100, shot_damage = 25, fire_interval_ms = 250;
    static constexpr double shot_range = 80, max_step = 2, max_speed = 10, eye_height = 1.6,
                            player_half_width = 0.5, player_height = 2;
};

struct Rejection
{
    static constexpr auto blocked_by_wall = "blocked-by-wall";
    static constexpr auto admission = "admission-rejected";
    static constexpr auto vertical_move = "vertical-move", non_finite = "non-finite";
    static constexpr auto step_too_large = "step-too-large", outside_map = "outside-map";
    static constexpr auto move_blocked_by_wall = "move-blocked-by-wall";
    static constexpr auto move_budget = "move-budget", invalid_aim = "invalid-aim";
    static constexpr auto fire_too_soon = "fire-too-soon", stale = "stale-sequence";
    static constexpr auto finished = "match-finished", packet_not_allowed = "packet-not-allowed";
};

struct ShotOutcome
{
    static constexpr auto hit = "hit", miss = "miss", blocked_by_wall = Rejection::blocked_by_wall;
};

enum class MatchPhase
{
    waiting,
    playing,
    finished
};

struct Vec3
{
    double x{}, y{}, z{};
    bool operator== (const Vec3 &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Vec3, x, y, z)
};

struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
    bool operator== (const SessionInfoRequest &) const = default;
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
    std::string sessionId{};
    std::string actorId{};
    std::uint64_t objectGeneration{};
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};

struct QueueRequest
{
    static constexpr auto packet_name = "QueueRequest";
    std::string playerId{};
    std::string mode = Names::default_mode;
    bool operator== (const QueueRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT (QueueRequest, playerId, mode)
};

struct RoomAdmission
{
    static constexpr auto packet_name = "RoomAdmission";
    std::string playerId{};
    std::string roomId{};
    bool operator== (const RoomAdmission &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomAdmission, playerId, roomId)
};

struct RoomAssignment
{
    static constexpr auto packet_name = "RoomAssignment";
    std::string roomId{};
    bool operator== (const RoomAssignment &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomAssignment, roomId)
};

struct MatchFound
{
    static constexpr auto packet_name = "MatchFound";
    std::string roomId{};
    bool operator== (const MatchFound &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchFound, roomId)
};

struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    Vec3 delta{};
    std::int64_t sequence{};
    bool operator== (const MoveRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, delta, sequence)
};

struct MoveResult
{
    static constexpr auto packet_name = "MoveResult";
    Vec3 position{};
    std::int64_t sequence{};
    bool operator== (const MoveResult &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveResult, position, sequence)
};

struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    Vec3 aimDirection{};
    std::int64_t sequence{};
    bool operator== (const ShootRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShootRequest, aimDirection, sequence)
};

struct RoomViewRequest
{
    static constexpr auto packet_name = "RoomViewRequest";
    bool operator== (const RoomViewRequest &) const = default;
};
inline void to_json (nlohmann::json &j, const RoomViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, RoomViewRequest &)
{
}

struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string playerId{};
    Vec3 position{};
    int hp{};
    std::int64_t lastSequence{};
    bool operator== (const PlayerView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerView, playerId, position, hp, lastSequence)
};

struct RoomView
{
    static constexpr auto packet_name = "RoomView";
    std::string roomId{};
    std::string nodeId{};
    std::vector<PlayerView> players{};
    MatchPhase phase = MatchPhase::waiting;
    std::optional<std::string> winner{};
    bool operator== (const RoomView &) const = default;
};
inline void to_json (nlohmann::json &j, const RoomView &value)
{
    j = nlohmann::json::object ();
    j["roomId"] = value.roomId;
    j["nodeId"] = value.nodeId;
    j["players"] = value.players;
    j["phase"] = value.phase;
    j["winner"] = detail::to_nullable_json (value.winner);
}
inline void from_json (const nlohmann::json &j, RoomView &value)
{
    j.at ("roomId").get_to (value.roomId);
    j.at ("nodeId").get_to (value.nodeId);
    j.at ("players").get_to (value.players);
    j.at ("phase").get_to (value.phase);
    detail::from_nullable_json (j.at ("winner"), value.winner);
}

struct ShotResult
{
    static constexpr auto packet_name = "ShotResult";
    std::string outcome{};
    std::optional<std::string> targetId{};
    std::int64_t sequence{};
    bool operator== (const ShotResult &) const = default;
};
inline void to_json (nlohmann::json &j, const ShotResult &value)
{
    j = nlohmann::json::object ();
    j["outcome"] = value.outcome;
    j["targetId"] = detail::to_nullable_json (value.targetId);
    j["sequence"] = value.sequence;
}
inline void from_json (const nlohmann::json &j, ShotResult &value)
{
    j.at ("outcome").get_to (value.outcome);
    detail::from_nullable_json (j.at ("targetId"), value.targetId);
    j.at ("sequence").get_to (value.sequence);
}

struct ShotRejected
{
    static constexpr auto packet_name = "ShotRejected";
    std::string reason{};
    int retryAfterMs{};
    bool operator== (const ShotRejected &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShotRejected, reason, retryAfterMs)
};

struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason{};
    int retryAfterMs{};
    bool operator== (const ActionRejected &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason, retryAfterMs)
};

struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner{};
    bool operator== (const MatchEnded &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchEnded, winner)
};

struct Limits
{
    static constexpr int room_capacity = std::numeric_limits<int>::max ();
    static constexpr auto packet = std::chrono::seconds (10), startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30),
                          shutdown = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe_observation = message_follow + relocation;
    static constexpr std::size_t observation_capacity = 128;
};

struct Control
{
    static constexpr auto ready = "READY ", drain = "drain", shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED", relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN", stopped = "STOPPED";
};
}

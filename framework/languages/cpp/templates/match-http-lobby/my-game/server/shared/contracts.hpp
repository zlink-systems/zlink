#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

NLOHMANN_JSON_NAMESPACE_BEGIN
template <typename T>
struct adl_serializer<std::optional<T>>
{
    static void to_json (json &json_, const std::optional<T> &value_)
    {
        if (value_)
            json_ = *value_;
        else
            json_ = nullptr;
    }
    static void from_json (const json &json_, std::optional<T> &value_)
    {
        if (json_.is_null ())
            value_.reset ();
        else
            value_ = json_.template get<T> ();
    }
};
NLOHMANN_JSON_NAMESPACE_END

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-arena";
    static constexpr auto room = "Room";
    static constexpr auto player = "Player";
    static constexpr auto stream = "players";
    static constexpr auto default_mode = "duel";
    static constexpr auto room_prefix = "room-";
    static constexpr auto match_path = "/match";
};
struct Game
{
    static constexpr std::size_t players_per_room = 2;
    static constexpr int initial_hp = 100, shot_damage = 25;
    static constexpr std::int64_t max_safe_integer = 9007199254740991LL;
    static constexpr double shot_range = 80;
    static constexpr double map_min_x = 0, map_max_x = 100, map_min_y = 0, map_max_y = 100;
    static constexpr double max_step = 2.0, max_speed = 10.0;
    static constexpr std::int64_t fire_interval_ms = 250;
    static constexpr double first_spawn_x = 40, first_spawn_y = 10;
    static constexpr double second_spawn_x = 60, second_spawn_y = 10;
    static constexpr double wall_x = 50, wall_min_y = 20, wall_max_y = 60;
    static constexpr auto playing_phase = "Playing", finished_phase = "Finished";
};
struct Rejection
{
    static constexpr auto out_of_range = "out-of-range", blocked = "blocked-by-wall";
    static constexpr auto no_target = "no-target";
    static constexpr auto invalid_move = "invalid-move";
    static constexpr auto stale = "stale-sequence", finished = "match-finished";
    static constexpr auto step_too_large = "step-too-large", outside_map = "outside-map";
    static constexpr auto move_blocked = "move-blocked-by-wall", move_budget = "move-budget";
    static constexpr auto fire_too_soon = "fire-too-soon";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
    static constexpr auto admission_rejected = "admission-rejected";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
};
struct Limits
{
    static constexpr int room_capacity = std::numeric_limits<int>::max ();
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto safe_shutdown = message_follow + relocation;
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
struct MatchRequest
{
    static constexpr auto packet_name = "MatchRequest";
    std::string playerId;
    std::string mode = Names::default_mode;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchRequest, playerId, mode)
};
struct MatchResponse
{
    static constexpr auto packet_name = "MatchResponse";
    std::string roomId, sessionEndpoint;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchResponse, roomId, sessionEndpoint)
};
struct JoinRoomRequest
{
    static constexpr auto packet_name = "JoinRoomRequest";
    std::string playerId, roomId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinRoomRequest, playerId, roomId)
};
struct PrepareRoom
{
    static constexpr auto packet_name = "PrepareRoom";
    std::string matchId;
    std::vector<std::string> members;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareRoom, matchId, members)
};
struct RoomPrepared
{
    static constexpr auto packet_name = "RoomPrepared";
    std::string matchId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomPrepared, matchId)
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double dx{};
    double dy{};
    std::int64_t sequence{};
    bool operator== (const MoveRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, dx, dy, sequence)
};
struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    std::string targetId{};
    std::int64_t sequence{};
    bool operator== (const ShootRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShootRequest, targetId, sequence)
};
struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string playerId;
    double x = 0, y = 0;
    int hp = Game::initial_hp;
    std::int64_t lastSequence = 0;
    bool operator== (const PlayerView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerView, playerId, x, y, hp, lastSequence)
};
struct RoomView
{
    static constexpr auto packet_name = "RoomView";
    std::string roomId{};
    std::string nodeId{};
    std::string phase = Game::playing_phase;
    std::optional<std::string> winner{};
    std::vector<PlayerView> players{};
    bool operator== (const RoomView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomView, roomId, nodeId, phase, winner, players)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs{};
    bool operator== (const ActionRejected &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason, retryAfterMs)
};
struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchEnded, winner)
};
struct MoveResult
{
    static constexpr auto packet_name = "MoveResult";
    double x{};
    double y{};
    std::int64_t sequence{};
    bool operator== (const MoveResult &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveResult, x, y, sequence)
};
struct ShotResult
{
    static constexpr auto packet_name = "ShotResult";
    std::string outcome{};
    std::string targetId{};
    std::int64_t sequence{};
    bool operator== (const ShotResult &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShotResult, outcome, targetId, sequence)
};
struct RoomViewRequest
{
    static constexpr auto packet_name = "RoomViewRequest";
};
inline void to_json (nlohmann::json &j, const RoomViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, RoomViewRequest &)
{
}
}

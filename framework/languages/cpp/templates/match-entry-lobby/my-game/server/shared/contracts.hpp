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
    static constexpr auto mesh = "my-game-arena", stream = "players", player = "Player";
    static constexpr auto room = "Room", matchmaking = "Matchmaking",
                          matchmaking_id = "matchmaking";
    static constexpr auto default_mode = "duel", room_prefix = "room-";
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
    static constexpr auto out_of_range = "out-of-range", blocked = "blocked-by-wall",
                          no_target = "no-target";
    static constexpr auto admission = "admission-rejected", invalid_move = "invalid-move";
    static constexpr auto stale = "stale-sequence", finished = "match-finished";
    static constexpr auto step_too_large = "step-too-large", outside_map = "outside-map";
    static constexpr auto move_blocked = "move-blocked-by-wall", move_budget = "move-budget";
    static constexpr auto fire_too_soon = "fire-too-soon";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
};
struct Limits
{
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
    double x{};
    double y{};
    int hp{};
    std::int64_t lastSequence{};
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
};
inline void to_json (nlohmann::json &j, const RoomView &view)
{
    j = nlohmann::json{{"roomId", view.roomId}, {"nodeId", view.nodeId},
                       {"phase", view.phase},
                       {"winner", view.winner ? nlohmann::json (*view.winner)
                                               : nlohmann::json (nullptr)},
                       {"players", view.players}};
}
inline void from_json (const nlohmann::json &j, RoomView &view)
{
    j.at ("roomId").get_to (view.roomId);
    j.at ("nodeId").get_to (view.nodeId);
    j.at ("phase").get_to (view.phase);
    view.winner = j.at ("winner").is_null ()
                    ? std::nullopt
                    : std::optional<std::string> (j.at ("winner").get<std::string> ());
    j.at ("players").get_to (view.players);
}
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason{};
    std::int32_t retryAfterMs{};
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
}

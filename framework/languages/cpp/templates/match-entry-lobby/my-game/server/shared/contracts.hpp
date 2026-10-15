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
    static constexpr auto mesh = "my-game-arena", stream = "players", player = "Player";
    static constexpr auto room = "Room", matchmaking = "Matchmaking",
                          matchmaking_id = "matchmaking";
    static constexpr auto default_mode = "duel", room_prefix = "room-";
};
struct Game
{
    static constexpr std::size_t players_per_room = 2;
    static constexpr int initial_hp = 100, shot_damage = 25;
    static constexpr double shot_range = 80, wall_x = 50, wall_min_y = 0, wall_max_y = 100;
};
struct Rejection
{
    static constexpr auto out_of_range = "out-of-range", blocked = "blocked-by-wall",
                          no_target = "no-target";
    static constexpr auto admission = "admission-rejected", invalid_move = "invalid-move";
    static constexpr auto stale = "stale-sequence", finished = "match-finished";
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10), startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30),
                          shutdown = std::chrono::seconds (30);
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
    double x{};
    double y{};
    std::int64_t sequence{};
    bool operator== (const MoveRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, x, y, sequence)
};
struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    std::string targetId{};
    std::int64_t sequence{};
    bool operator== (const ShootRequest &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShootRequest, targetId, sequence)
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
    bool operator== (const PlayerView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerView, playerId, x, y, hp)
};
struct RoomView
{
    static constexpr auto packet_name = "RoomView";
    std::string roomId{};
    std::string nodeId{};
    std::vector<PlayerView> players{};
    bool operator== (const RoomView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomView, roomId, nodeId, players)
};
struct ShotRejected
{
    static constexpr auto packet_name = "ShotRejected";
    std::string reason{};
    bool operator== (const ShotRejected &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShotRejected, reason)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason{};
    bool operator== (const ActionRejected &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason)
};
struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner{};
    bool operator== (const MatchEnded &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchEnded, winner)
};
}

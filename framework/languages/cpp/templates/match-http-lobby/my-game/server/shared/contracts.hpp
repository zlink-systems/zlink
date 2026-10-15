#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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
    static constexpr int players_per_room = 2, initial_hp = 100, damage = 25;
    static constexpr double range = 80, wall_x = 50, wall_min_y = 0, wall_max_y = 100;
};
struct Rejection
{
    static constexpr auto stale = "stale-sequence";
    static constexpr auto finished = "match-finished";
    static constexpr auto invalid_move = "invalid-move";
    static constexpr auto no_target = "no-target";
    static constexpr auto range = "out-of-range";
    static constexpr auto wall = "blocked-by-wall";
    static constexpr auto admission = "admission-rejected";
};
struct Limits
{
    static constexpr int room_capacity = std::numeric_limits<int>::max ();
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
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double x = 0, y = 0;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MoveRequest, x, y, sequence)
};
struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    std::string targetId;
    std::int64_t sequence = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShootRequest, targetId, sequence)
};
struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string playerId;
    double x = 0, y = 0;
    int hp = Game::initial_hp;
    bool operator== (const PlayerView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerView, playerId, x, y, hp)
};
struct RoomView
{
    static constexpr auto packet_name = "RoomView";
    std::string roomId, nodeId;
    std::vector<PlayerView> players;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomView, roomId, nodeId, players)
};
struct ShotRejected
{
    static constexpr auto packet_name = "ShotRejected";
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ShotRejected, reason)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason)
};
struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchEnded, winner)
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

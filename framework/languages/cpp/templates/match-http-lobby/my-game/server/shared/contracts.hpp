#pragma once
#include <zlink/json.hpp>
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

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
};
struct MatchRequest
{
    static constexpr auto packet_name = "MatchRequest";
    std::string playerId;
    std::string mode = Names::default_mode;
};
struct MatchResponse
{
    static constexpr auto packet_name = "MatchResponse";
    std::string roomId, sessionEndpoint;
};
struct JoinRoomRequest
{
    static constexpr auto packet_name = "JoinRoomRequest";
    std::string playerId, roomId;
};
struct PrepareRoom
{
    static constexpr auto packet_name = "PrepareRoom";
    std::string matchId;
    std::vector<std::string> members;
};
struct RoomPrepared
{
    static constexpr auto packet_name = "RoomPrepared";
    std::string matchId;
};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double dx{};
    double dy{};
    std::int64_t sequence{};
    bool operator== (const MoveRequest &) const = default;
};
struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    std::string targetId{};
    std::int64_t sequence{};
    bool operator== (const ShootRequest &) const = default;
};
struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string playerId;
    double x = 0, y = 0;
    int hp = Game::initial_hp;
    std::int64_t lastSequence = 0;
    bool operator== (const PlayerView &) const = default;
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
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs{};
    bool operator== (const ActionRejected &) const = default;
};
struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner;
};
struct MoveResult
{
    static constexpr auto packet_name = "MoveResult";
    double x{};
    double y{};
    std::int64_t sequence{};
    bool operator== (const MoveResult &) const = default;
};
struct ShotResult
{
    static constexpr auto packet_name = "ShotResult";
    std::string outcome{};
    std::string targetId{};
    std::int64_t sequence{};
    bool operator== (const ShotResult &) const = default;
};
struct RoomViewRequest
{
    static constexpr auto packet_name = "RoomViewRequest";
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

template <> struct dto_traits<::MyGame::MatchRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::MatchRequest::playerId),
      zlink::json::field ("mode", &::MyGame::MatchRequest::mode)
    };
};

template <> struct dto_traits<::MyGame::MatchResponse>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::MatchResponse::roomId),
      zlink::json::field ("sessionEndpoint", &::MyGame::MatchResponse::sessionEndpoint)
    };
};

template <> struct dto_traits<::MyGame::JoinRoomRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinRoomRequest::playerId),
      zlink::json::field ("roomId", &::MyGame::JoinRoomRequest::roomId)
    };
};

template <> struct dto_traits<::MyGame::PrepareRoom>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("matchId", &::MyGame::PrepareRoom::matchId),
      zlink::json::field ("members", &::MyGame::PrepareRoom::members)
    };
};

template <> struct dto_traits<::MyGame::RoomPrepared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("matchId", &::MyGame::RoomPrepared::matchId)
    };
};

template <> struct dto_traits<::MyGame::MoveRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dx", &::MyGame::MoveRequest::dx),
      zlink::json::field ("dy", &::MyGame::MoveRequest::dy),
      zlink::json::field ("sequence", &::MyGame::MoveRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::ShootRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("targetId", &::MyGame::ShootRequest::targetId),
      zlink::json::field ("sequence", &::MyGame::ShootRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::PlayerView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerView::playerId),
      zlink::json::field ("x", &::MyGame::PlayerView::x),
      zlink::json::field ("y", &::MyGame::PlayerView::y),
      zlink::json::field ("hp", &::MyGame::PlayerView::hp),
      zlink::json::field ("lastSequence", &::MyGame::PlayerView::lastSequence)
    };
};

template <> struct dto_traits<::MyGame::RoomView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::RoomView::roomId),
      zlink::json::field ("nodeId", &::MyGame::RoomView::nodeId),
      zlink::json::field ("phase", &::MyGame::RoomView::phase),
      zlink::json::field ("winner", &::MyGame::RoomView::winner),
      zlink::json::field ("players", &::MyGame::RoomView::players)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::MatchEnded>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("winner", &::MyGame::MatchEnded::winner)
    };
};

template <> struct dto_traits<::MyGame::MoveResult>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("x", &::MyGame::MoveResult::x),
      zlink::json::field ("y", &::MyGame::MoveResult::y),
      zlink::json::field ("sequence", &::MyGame::MoveResult::sequence)
    };
};

template <> struct dto_traits<::MyGame::ShotResult>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("outcome", &::MyGame::ShotResult::outcome),
      zlink::json::field ("targetId", &::MyGame::ShotResult::targetId),
      zlink::json::field ("sequence", &::MyGame::ShotResult::sequence)
    };
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::RoomViewRequest>
{
    static constexpr auto fields = std::tuple{};
};
} // namespace zlink::json

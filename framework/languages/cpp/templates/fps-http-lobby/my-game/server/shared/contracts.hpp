#pragma once
#include <zlink/json.hpp>
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
    static constexpr auto mesh = "my-game-arena", stream = "players", player = "Player";
    static constexpr auto room = "Room", matchmaking = "Matchmaking",
                          matchmaking_id = "matchmaking";
    static constexpr auto default_mode = "duel", room_prefix = "room-";
};

struct Game
{
    static constexpr std::size_t players_per_room = 2;
    static constexpr std::int64_t max_sequence = 9007199254740991LL;
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
    static constexpr auto integer_out_of_range = "integer-out-of-range";
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
};

struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
    bool operator== (const SessionInfoRequest &) const = default;
};

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId{};
    std::string actorId{};
    std::uint64_t objectGeneration{};
    bool operator== (const SessionInfo &) const = default;
};

struct QueueRequest
{
    static constexpr auto packet_name = "QueueRequest";
    std::string playerId{};
    std::string mode = Names::default_mode;
    bool operator== (const QueueRequest &) const = default;
};

struct RoomAdmission
{
    static constexpr auto packet_name = "RoomAdmission";
    std::string playerId{};
    std::string roomId{};
    bool operator== (const RoomAdmission &) const = default;
};

struct RoomAssignment
{
    static constexpr auto packet_name = "RoomAssignment";
    std::string roomId{};
    bool operator== (const RoomAssignment &) const = default;
};

struct MatchFound
{
    static constexpr auto packet_name = "MatchFound";
    std::string roomId{};
    bool operator== (const MatchFound &) const = default;
};

struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    Vec3 delta{};
    std::int64_t sequence{};
    bool operator== (const MoveRequest &) const = default;
};

struct MoveResult
{
    static constexpr auto packet_name = "MoveResult";
    Vec3 position{};
    std::int64_t sequence{};
    bool operator== (const MoveResult &) const = default;
};

struct ShootRequest
{
    static constexpr auto packet_name = "ShootRequest";
    Vec3 aimDirection{};
    std::int64_t sequence{};
    bool operator== (const ShootRequest &) const = default;
};

struct RoomViewRequest
{
    static constexpr auto packet_name = "RoomViewRequest";
    bool operator== (const RoomViewRequest &) const = default;
};

struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string playerId{};
    Vec3 position{};
    int hp{};
    std::int64_t lastSequence{};
    bool operator== (const PlayerView &) const = default;
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

struct ShotResult
{
    static constexpr auto packet_name = "ShotResult";
    std::string outcome{};
    std::optional<std::string> targetId{};
    std::int64_t sequence{};
    bool operator== (const ShotResult &) const = default;
};

struct ShotRejected
{
    static constexpr auto packet_name = "ShotRejected";
    std::string reason{};
    int retryAfterMs{};
    bool operator== (const ShotRejected &) const = default;
};

struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason{};
    int retryAfterMs{};
    bool operator== (const ActionRejected &) const = default;
};

struct MatchEnded
{
    static constexpr auto packet_name = "MatchEnded";
    std::string winner{};
    bool operator== (const MatchEnded &) const = default;
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

namespace zlink::json
{
template <> struct dto_traits<::MyGame::Vec3>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("x", &::MyGame::Vec3::x),
      zlink::json::field ("y", &::MyGame::Vec3::y),
      zlink::json::field ("z", &::MyGame::Vec3::z)
    };
};

template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::QueueRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::QueueRequest::playerId),
      zlink::json::field ("mode", &::MyGame::QueueRequest::mode)
    };
};

template <> struct dto_traits<::MyGame::RoomAdmission>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::RoomAdmission::playerId),
      zlink::json::field ("roomId", &::MyGame::RoomAdmission::roomId)
    };
};

template <> struct dto_traits<::MyGame::RoomAssignment>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::RoomAssignment::roomId)
    };
};

template <> struct dto_traits<::MyGame::MatchFound>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::MatchFound::roomId)
    };
};

template <> struct dto_traits<::MyGame::MoveRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("delta", &::MyGame::MoveRequest::delta),
      zlink::json::field ("sequence", &::MyGame::MoveRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::MoveResult>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("position", &::MyGame::MoveResult::position),
      zlink::json::field ("sequence", &::MyGame::MoveResult::sequence)
    };
};

template <> struct dto_traits<::MyGame::ShootRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("aimDirection", &::MyGame::ShootRequest::aimDirection),
      zlink::json::field ("sequence", &::MyGame::ShootRequest::sequence)
    };
};

template <> struct dto_traits<::MyGame::PlayerView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerView::playerId),
      zlink::json::field ("position", &::MyGame::PlayerView::position),
      zlink::json::field ("hp", &::MyGame::PlayerView::hp),
      zlink::json::field ("lastSequence", &::MyGame::PlayerView::lastSequence)
    };
};

template <> struct dto_traits<::MyGame::ShotRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ShotRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ShotRejected::retryAfterMs)
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

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::RoomViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::RoomView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::RoomView::roomId),
      zlink::json::field ("nodeId", &::MyGame::RoomView::nodeId),
      zlink::json::field ("players", &::MyGame::RoomView::players),
      zlink::json::field ("phase", &::MyGame::RoomView::phase),
      zlink::json::field ("winner", &::MyGame::RoomView::winner)
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

template <> struct enum_traits<::MyGame::MatchPhase>
{
    static constexpr auto values = std::array{
      zlink::json::enum_value (::MyGame::MatchPhase::waiting, "Waiting"),
      zlink::json::enum_value (::MyGame::MatchPhase::playing, "Playing"),
      zlink::json::enum_value (::MyGame::MatchPhase::finished, "Finished")
    };
};
} // namespace zlink::json

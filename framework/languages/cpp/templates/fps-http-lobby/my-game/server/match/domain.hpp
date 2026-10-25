#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>

namespace MyGame
{
inline Vec3 operator+ (const Vec3 &a, const Vec3 &b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator- (const Vec3 &a, const Vec3 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator* (const Vec3 &a, double scale)
{
    return {a.x * scale, a.y * scale, a.z * scale};
}
inline bool is_finite (const Vec3 &value)
{
    return std::isfinite (value.x) && std::isfinite (value.y) && std::isfinite (value.z);
}
inline double length (const Vec3 &value)
{
    const auto scale = std::max ({std::abs (value.x), std::abs (value.y), std::abs (value.z)});
    if (scale == 0 || !std::isfinite (scale))
        return scale;
    const auto x = value.x / scale, y = value.y / scale, z = value.z / scale;
    return scale * std::sqrt (x * x + y * y + z * z);
}
inline bool try_normalize (const Vec3 &value, Vec3 &direction)
{
    const auto scale = std::max ({std::abs (value.x), std::abs (value.y), std::abs (value.z)});
    if (!is_finite (value) || scale == 0) {
        direction = {};
        return false;
    }
    const Vec3 scaled{value.x / scale, value.y / scale, value.z / scale};
    direction = scaled * (1 / length (scaled));
    return true;
}

struct Aabb
{
    Vec3 min{}, max{};
    bool contains (const Vec3 &point) const
    {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y
               && point.z >= min.z && point.z <= max.z;
    }
    std::optional<double> intersect (const Vec3 &origin, const Vec3 &direction, double limit) const
    {
        auto near = 0.0, far = limit;
        if (!slab (origin.x, direction.x, min.x, max.x, near, far)
            || !slab (origin.y, direction.y, min.y, max.y, near, far)
            || !slab (origin.z, direction.z, min.z, max.z, near, far))
            return std::nullopt;
        return near;
    }

  private:
    static bool slab (double origin, double direction, double low, double high,
                      double &near, double &far)
    {
        if (direction == 0)
            return origin >= low && origin <= high;
        const auto a = (low - origin) / direction;
        const auto b = (high - origin) / direction;
        near = std::max (near, std::min (a, b));
        far = std::min (far, std::max (a, b));
        return near <= far;
    }
};

struct ArenaMap
{
    static const Aabb &bounds ()
    {
        static const Aabb value{{0, 0, 0}, {100, 10, 100}};
        return value;
    }
    static const std::array<Aabb, 2> &walls ()
    {
        static const std::array<Aabb, 2> value{
          Aabb{{49, 0, 20}, {51, 10, 60}}, Aabb{{80, 0, 0}, {82, 10, 20}}};
        return value;
    }
    static const std::array<Vec3, 2> &spawns ()
    {
        static const std::array<Vec3, 2> value{Vec3{48, 0, 18}, Vec3{52, 0, 18}};
        return value;
    }
    static Aabb player_box (const Vec3 &feet)
    {
        return {feet + Vec3{-Game::player_half_width, 0, -Game::player_half_width},
                feet + Vec3{Game::player_half_width, Game::player_height,
                            Game::player_half_width}};
    }
};

struct Combatant
{
    std::string playerId;
    Vec3 position{};
    int hp = Game::initial_hp;
    std::int64_t sequence{};
    std::optional<double> lastMoveMs{};
    std::optional<double> nextFireMs{};
};

struct CombatantSnapshot
{
    std::string playerId{};
    Vec3 position{};
    int hp{};
    std::int64_t sequence{};
    double fireRemainingMs{};
    double moveRemainingMs{};
    bool operator== (const CombatantSnapshot &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (CombatantSnapshot, playerId, position, hp, sequence,
                                    fireRemainingMs, moveRemainingMs)
};

struct RoomSnapshot
{
    std::vector<CombatantSnapshot> players{};
    MatchPhase phase = MatchPhase::waiting;
    std::optional<std::string> winner{};
    bool operator== (const RoomSnapshot &) const = default;
};
inline void to_json (nlohmann::json &j, const RoomSnapshot &value)
{
    j = nlohmann::json::object ();
    j["players"] = value.players;
    j["phase"] = value.phase;
    j["winner"] = detail::to_nullable_json (value.winner);
}
inline void from_json (const nlohmann::json &j, RoomSnapshot &value)
{
    j.at ("players").get_to (value.players);
    j.at ("phase").get_to (value.phase);
    detail::from_nullable_json (j.at ("winner"), value.winner);
}

struct ShotDecision
{
    std::optional<ShotRejected> rejected{};
    std::optional<ShotResult> result{};
};

class RoomState
{
  public:
    using Clock = std::function<double ()>;
    explicit RoomState (Clock clock = {}) : nowMs_ (clock ? std::move (clock) : Clock{monotonic_now_ms}) {}

    bool admit (const std::string &playerId)
    {
        if (std::any_of (players_.begin (), players_.end (), [&] (const auto &player) {
                return player.playerId == playerId;
            }))
            return true;
        if (players_.size () >= Game::players_per_room)
            return false;
        players_.push_back ({playerId, ArenaMap::spawns ()[players_.size ()]});
        if (players_.size () == Game::players_per_room)
            phase_ = MatchPhase::playing;
        return true;
    }

    std::optional<ActionRejected> move (std::string_view id, const MoveRequest &request)
    {
        auto &combatant = player (id);
        if (const auto *reason = validate_action (combatant, request.sequence))
            return ActionRejected{reason};
        if (!is_finite (request.delta))
            return ActionRejected{Rejection::non_finite};
        if (request.delta.y != 0)
            return ActionRejected{Rejection::vertical_move};
        const auto distance = length (request.delta);
        if (distance > Game::max_step)
            return ActionRejected{Rejection::step_too_large};
        const auto next = combatant.position + request.delta;
        if (!ArenaMap::bounds ().contains (next))
            return ActionRejected{Rejection::outside_map};
        for (const auto &wall : ArenaMap::walls ())
            if (wall.intersect (combatant.position, request.delta, 1))
                return ActionRejected{Rejection::move_blocked_by_wall};

        const auto now = nowMs_ ();
        if (combatant.lastMoveMs) {
            const auto elapsed = std::max (0.0, now - *combatant.lastMoveMs);
            const auto budget = std::min (Game::max_step, Game::max_speed * elapsed / 1000);
            if (distance > budget) {
                const auto retry = std::max (1.0, std::ceil (distance * 1000 / Game::max_speed
                                                             - elapsed));
                return ActionRejected{Rejection::move_budget, static_cast<int> (retry)};
            }
        }
        combatant.position = next;
        combatant.sequence = request.sequence;
        combatant.lastMoveMs = now;
        return std::nullopt;
    }

    ShotDecision shoot (std::string_view id, const ShootRequest &request)
    {
        auto &shooter = player (id);
        if (const auto *reason = validate_action (shooter, request.sequence))
            return {ShotRejected{reason}, std::nullopt};
        Vec3 direction;
        if (!try_normalize (request.aimDirection, direction))
            return {ShotRejected{Rejection::invalid_aim}, std::nullopt};
        const auto now = nowMs_ ();
        if (shooter.nextFireMs && now < *shooter.nextFireMs) {
            const auto retry = std::max (1.0, std::ceil (*shooter.nextFireMs - now));
            return {ShotRejected{Rejection::fire_too_soon, static_cast<int> (retry)},
                    std::nullopt};
        }

        const auto origin = shooter.position + Vec3{0, Game::eye_height, 0};
        auto wallDistance = std::numeric_limits<double>::infinity ();
        for (const auto &wall : ArenaMap::walls ())
            if (const auto distance = wall.intersect (origin, direction, Game::shot_range))
                wallDistance = std::min (wallDistance, *distance);
        Combatant *target = nullptr;
        auto targetDistance = std::numeric_limits<double>::infinity ();
        for (auto &candidate : players_) {
            if (&candidate == &shooter || candidate.hp == 0)
                continue;
            if (const auto distance = ArenaMap::player_box (candidate.position)
                                        .intersect (origin, direction, Game::shot_range);
                distance && *distance < targetDistance) {
                target = &candidate;
                targetDistance = *distance;
            }
        }

        std::string outcome = ShotOutcome::miss;
        std::optional<std::string> targetId;
        if (wallDistance <= targetDistance && std::isfinite (wallDistance))
            outcome = ShotOutcome::blocked_by_wall;
        else if (target) {
            outcome = ShotOutcome::hit;
            target->hp = std::max (0, target->hp - Game::shot_damage);
            targetId = target->playerId;
            if (target->hp == 0) {
                phase_ = MatchPhase::finished;
                winner_ = std::string (id);
            }
        }
        shooter.sequence = request.sequence;
        shooter.nextFireMs = now + Game::fire_interval_ms;
        return {std::nullopt, ShotResult{std::move (outcome), std::move (targetId), request.sequence}};
    }

    std::vector<PlayerView> view () const
    {
        std::vector<PlayerView> result;
        result.reserve (players_.size ());
        for (const auto &player : players_)
            result.push_back ({player.playerId, player.position, player.hp, player.sequence});
        return result;
    }

    RoomSnapshot capture () const
    {
        const auto now = nowMs_ ();
        std::vector<CombatantSnapshot> snapshots;
        snapshots.reserve (players_.size ());
        const auto moveWindow = Game::max_step * 1000 / Game::max_speed;
        for (const auto &player : players_)
            snapshots.push_back ({player.playerId, player.position, player.hp, player.sequence,
                                  player.nextFireMs ? std::max (0.0, *player.nextFireMs - now) : 0,
                                  player.lastMoveMs
                                    ? std::max (0.0, moveWindow - (now - *player.lastMoveMs))
                                    : 0});
        return {std::move (snapshots), phase_, winner_};
    }

    void restore (const RoomSnapshot &snapshot)
    {
        const auto now = nowMs_ ();
        players_.clear ();
        for (const auto &snapshotPlayer : snapshot.players) {
            Combatant player{snapshotPlayer.playerId, snapshotPlayer.position};
            player.hp = snapshotPlayer.hp;
            player.sequence = snapshotPlayer.sequence;
            player.nextFireMs = now + snapshotPlayer.fireRemainingMs;
            const auto moveWindow = Game::max_step * 1000 / Game::max_speed;
            if (snapshotPlayer.moveRemainingMs > 0)
                player.lastMoveMs = now - (moveWindow - snapshotPlayer.moveRemainingMs);
            players_.push_back (std::move (player));
        }
        phase_ = snapshot.phase;
        winner_ = snapshot.winner;
    }

    const std::vector<Combatant> &players () const noexcept { return players_; }
    MatchPhase phase () const noexcept { return phase_; }
    const std::optional<std::string> &winner () const noexcept { return winner_; }

  private:
    static double monotonic_now_ms ()
    {
        using namespace std::chrono;
        return duration<double, std::milli> (steady_clock::now ().time_since_epoch ()).count ();
    }
    const char *validate_action (const Combatant &player, std::int64_t sequence) const
    {
        if (sequence <= player.sequence)
            return Rejection::stale;
        if (phase_ == MatchPhase::finished)
            return Rejection::finished;
        return nullptr;
    }
    Combatant &player (std::string_view id)
    {
        const auto found = std::find_if (players_.begin (), players_.end (), [&] (const auto &p) {
            return p.playerId == id;
        });
        if (found == players_.end ())
            throw std::invalid_argument ("Unknown room player");
        return *found;
    }

    Clock nowMs_;
    std::vector<Combatant> players_;
    MatchPhase phase_ = MatchPhase::waiting;
    std::optional<std::string> winner_;
};
}

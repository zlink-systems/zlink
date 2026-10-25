#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
namespace MyGame
{
struct MatchmakingState
{
    std::vector<std::pair<std::string, std::vector<QueueRequest>>> waiting;
    std::int64_t roomSequence = 0;
    void enqueue (const QueueRequest &request)
    {
        if (request.mode.find_first_not_of (" \t\r\n") == std::string::npos)
            throw std::invalid_argument ("Mode required");
        for (const auto &[mode, queue] : waiting)
            for (const auto &p : queue)
                if (p.playerId == request.playerId)
                    return;
        auto found = std::find_if (waiting.begin (), waiting.end (),
                                   [&] (const auto &q) { return q.first == request.mode; });
        if (found == waiting.end ())
            waiting.push_back ({request.mode, {request}});
        else
            found->second.push_back (request);
    }
    std::vector<QueueRequest> pair () const
    {
        for (const auto &[mode, queue] : waiting)
            if (queue.size () >= Game::players_per_room)
                return {queue.begin (), queue.begin () + Game::players_per_room};
        return {};
    }
    std::string allocate_room ()
    {
        if (roomSequence == std::numeric_limits<std::int64_t>::max ())
            throw std::overflow_error ("Room sequence exhausted");
        return Names::room_prefix + std::to_string (++roomSequence);
    }
    void remove (const std::vector<QueueRequest> &pair)
    {
        auto q = std::find_if (waiting.begin (), waiting.end (), [&] (const auto &entry) {
            return entry.first == pair.front ().mode;
        });
        q->second.erase (q->second.begin (), q->second.begin () + Game::players_per_room);
        if (q->second.empty ())
            waiting.erase (q);
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchmakingState, waiting, roomSequence)
};
struct Combatant
{
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    std::string playerId;
    double x = 0, y = 0;
    int hp = Game::initial_hp;
    std::int64_t lastSequence = 0;
    std::optional<TimePoint> lastMoveAt;
    std::optional<TimePoint> lastShotAt;
};
struct RoomState
{
    using Clock = Combatant::Clock;
    using TimePoint = Combatant::TimePoint;
    using ClockSource = std::function<TimePoint ()>;

    explicit RoomState (ClockSource clock = {}) :
        clock_ (clock ? std::move (clock) : ClockSource{[] { return Clock::now (); }})
    {
    }

    std::vector<Combatant> players;
    std::optional<std::string> winner;

    std::string phase () const
    {
        return winner ? Game::finished_phase : Game::playing_phase;
    }

    bool admit (const std::string &id)
    {
        if (std::any_of (players.begin (), players.end (),
                         [&] (const auto &p) { return p.playerId == id; }))
            return true;
        if (players.size () >= Game::players_per_room)
            return false;
        const auto spawn = players.empty ()
                             ? std::pair{Game::first_spawn_x, Game::first_spawn_y}
                             : std::pair{Game::second_spawn_x, Game::second_spawn_y};
        players.push_back (Combatant{.playerId = id, .x = spawn.first, .y = spawn.second});
        return true;
    }
    Combatant &player (std::string_view id)
    {
        for (auto &p : players)
            if (p.playerId == id)
                return p;
        throw std::invalid_argument ("Unknown room player");
    }

    const Combatant &player (std::string_view id) const
    {
        for (const auto &p : players)
            if (p.playerId == id)
                return p;
        throw std::invalid_argument ("Unknown room player");
    }

    std::optional<ActionRejected> validate (const Combatant &p, std::int64_t sequence) const
    {
        if (sequence > Game::max_safe_integer)
            return ActionRejected{Rejection::integer_out_of_range};
        if (sequence <= p.lastSequence)
            return ActionRejected{Rejection::stale};
        if (winner)
            return ActionRejected{Rejection::finished};
        return std::nullopt;
    }

    std::optional<ActionRejected> move (std::string_view id, const MoveRequest &request)
    {
        auto &p = player (id);
        if (auto rejection = validate (p, request.sequence))
            return rejection;
        if (!std::isfinite (request.dx) || !std::isfinite (request.dy))
            return ActionRejected{Rejection::invalid_move};
        const auto distance = std::hypot (request.dx, request.dy);
        if (distance > Game::max_step)
            return ActionRejected{Rejection::step_too_large};
        const auto target_x = p.x + request.dx;
        const auto target_y = p.y + request.dy;
        if (target_x < Game::map_min_x || target_x > Game::map_max_x
            || target_y < Game::map_min_y || target_y > Game::map_max_y)
            return ActionRejected{Rejection::outside_map};
        if (intersects_wall (p.x, p.y, target_x, target_y))
            return ActionRejected{Rejection::move_blocked};

        const auto now = clock_ ();
        if (p.lastMoveAt) {
            const auto elapsed = std::max (
              0.0, std::chrono::duration<double> (now - *p.lastMoveAt).count ());
            const auto budget = std::min (Game::max_step, Game::max_speed * elapsed);
            if (distance > budget) {
                const auto retry_after_ms = bounded_retry_after_ms (
                  (distance - budget) * 1000.0 / Game::max_speed);
                return ActionRejected{Rejection::move_budget, retry_after_ms};
            }
        }

        p.x = target_x;
        p.y = target_y;
        p.lastSequence = request.sequence;
        p.lastMoveAt = now;
        return std::nullopt;
    }

    std::optional<ActionRejected> shoot (std::string_view id, const ShootRequest &request)
    {
        auto &p = player (id);
        if (auto rejection = validate (p, request.sequence))
            return rejection;
        auto target = std::find_if (players.begin (), players.end (), [&] (const auto &t) {
            return t.playerId == request.targetId && t.playerId != id && t.hp > 0;
        });
        if (target == players.end ())
            return ActionRejected{Rejection::no_target};
        const double dx = target->x - p.x, dy = target->y - p.y;
        if (dx * dx + dy * dy > Game::shot_range * Game::shot_range)
            return ActionRejected{Rejection::out_of_range};
        if (intersects_wall (p.x, p.y, target->x, target->y))
            return ActionRejected{Rejection::blocked};

        const auto now = clock_ ();
        if (p.lastShotAt) {
            const auto elapsed_ms = std::max (
              0.0, std::chrono::duration<double, std::milli> (now - *p.lastShotAt).count ());
            const auto retry_after_ms =
              static_cast<double> (Game::fire_interval_ms) - elapsed_ms;
            if (retry_after_ms > 0)
                return ActionRejected{Rejection::fire_too_soon,
                                      bounded_retry_after_ms (retry_after_ms)};
        }

        target->hp = std::max (0, target->hp - Game::shot_damage);
        if (target->hp == 0)
            winner = std::string (id);
        p.lastSequence = request.sequence;
        p.lastShotAt = now;
        return std::nullopt;
    }

    std::vector<PlayerView> view () const
    {
        std::vector<PlayerView> result;
        result.reserve (players.size ());
        for (const auto &p : players)
            result.push_back ({p.playerId, p.x, p.y, p.hp, p.lastSequence});
        return result;
    }

    friend void to_json (nlohmann::json &j, const RoomState &state)
    {
        const auto now = state.clock_ ();
        j = nlohmann::json{{"phase", state.phase ()},
                           {"winner", state.winner ? nlohmann::json (*state.winner)
                                                    : nlohmann::json (nullptr)},
                           {"players", nlohmann::json::array ()}};
        const auto move_budget_ms = Game::max_step / Game::max_speed * 1000.0;
        for (const auto &p : state.players) {
            const auto move_remaining = state.remaining_ms (p.lastMoveAt, move_budget_ms, now);
            const auto fire_remaining =
              state.remaining_ms (p.lastShotAt, Game::fire_interval_ms, now);
            j["players"].push_back (
              {{"playerId", p.playerId}, {"x", p.x}, {"y", p.y}, {"hp", p.hp},
               {"lastSequence", p.lastSequence},
               {"moveBudgetRemainingMs", move_remaining ? nlohmann::json (*move_remaining)
                                                        : nlohmann::json (nullptr)},
               {"fireCooldownRemainingMs", fire_remaining ? nlohmann::json (*fire_remaining)
                                                           : nlohmann::json (nullptr)}});
        }
    }

    friend void from_json (const nlohmann::json &j, RoomState &state)
    {
        const auto phase = j.at ("phase").get<std::string> ();
        const auto winner = j.at ("winner").is_null ()
                              ? std::optional<std::string>{}
                              : std::optional<std::string> (
                                  j.at ("winner").get<std::string> ());
        if (phase != (winner ? Game::finished_phase : Game::playing_phase))
            throw std::invalid_argument ("Room snapshot phase differs from winner");
        const auto &snapshot_players = j.at ("players");
        if (!snapshot_players.is_array () || snapshot_players.size () > Game::players_per_room)
            throw std::invalid_argument ("Room snapshot player count is invalid");

        const auto now = state.clock_ ();
        const auto move_budget_ms = Game::max_step / Game::max_speed * 1000.0;
        std::vector<Combatant> players;
        players.reserve (snapshot_players.size ());
        for (const auto &item : snapshot_players) {
            Combatant p;
            p.playerId = item.at ("playerId").get<std::string> ();
            p.x = item.at ("x").get<double> ();
            p.y = item.at ("y").get<double> ();
            p.hp = item.at ("hp").get<int> ();
            p.lastSequence = item.at ("lastSequence").get<std::int64_t> ();
            const auto move_remaining = item.at ("moveBudgetRemainingMs").is_null ()
                                          ? std::optional<std::int64_t>{}
                                          : std::optional<std::int64_t> (
                                              item.at ("moveBudgetRemainingMs")
                                                .get<std::int64_t> ());
            const auto fire_remaining = item.at ("fireCooldownRemainingMs").is_null ()
                                          ? std::optional<std::int64_t>{}
                                          : std::optional<std::int64_t> (
                                              item.at ("fireCooldownRemainingMs")
                                                .get<std::int64_t> ());
            p.lastMoveAt = state.restore_timestamp (move_remaining, move_budget_ms, now);
            p.lastShotAt = state.restore_timestamp (
              fire_remaining, static_cast<double> (Game::fire_interval_ms), now);
            players.push_back (std::move (p));
        }
        state.players = std::move (players);
        state.winner = winner;
    }

  private:
    ClockSource clock_;

    static std::int32_t bounded_retry_after_ms (double milliseconds)
    {
        if (!(milliseconds > 1.0))
            return 1;
        const auto rounded = std::ceil (milliseconds);
        const auto max_retry_after_ms =
          static_cast<double> (std::numeric_limits<std::int32_t>::max ());
        if (rounded >= max_retry_after_ms)
            return std::numeric_limits<std::int32_t>::max ();
        return static_cast<std::int32_t> (rounded);
    }

    bool intersects_wall (double ax, double ay, double bx, double by) const
    {
        if (ax == bx)
            return ax == Game::wall_x && std::max (ay, by) >= Game::wall_min_y
                   && std::min (ay, by) <= Game::wall_max_y;
        const auto t = (Game::wall_x - ax) / (bx - ax);
        const auto y = ay + t * (by - ay);
        return t >= 0 && t <= 1 && y >= Game::wall_min_y && y <= Game::wall_max_y;
    }

    std::optional<std::int64_t> remaining_ms (std::optional<TimePoint> timestamp,
                                              double duration_ms,
                                              TimePoint now) const
    {
        if (!timestamp)
            return std::nullopt;
        const auto elapsed_ms = std::max (
          0.0, std::chrono::duration<double, std::milli> (now - *timestamp).count ());
        return std::max<std::int64_t> (
          0, static_cast<std::int64_t> (std::ceil (duration_ms - elapsed_ms)));
    }

    std::optional<TimePoint> restore_timestamp (std::optional<std::int64_t> remaining,
                                                double duration_ms,
                                                TimePoint now) const
    {
        if (!remaining)
            return std::nullopt;
        const auto bounded_remaining = std::clamp (*remaining, std::int64_t{0},
                                                   static_cast<std::int64_t> (duration_ms));
        const auto elapsed_ms = duration_ms - static_cast<double> (bounded_remaining);
        const auto elapsed = std::chrono::ceil<Clock::duration> (
          std::chrono::duration<double, std::milli> (elapsed_ms));
        return now - elapsed;
    }
};
}

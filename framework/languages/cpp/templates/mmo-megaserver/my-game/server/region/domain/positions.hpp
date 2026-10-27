#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

namespace MyGame
{
struct PositionSnapshot
{
    std::string PlayerId;
    double X = 0;
    std::int64_t Sequence = 0;
    std::int64_t RemainingMs = 0;
    bool operator== (const PositionSnapshot &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PositionSnapshot, PlayerId, X, Sequence, RemainingMs)
};
struct PositionsSnapshot
{
    std::vector<PositionSnapshot> Players;
    bool operator== (const PositionsSnapshot &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PositionsSnapshot, Players)
};

class Positions
{
    using Clock = std::function<std::chrono::steady_clock::time_point ()>;
    using TimePoint = std::chrono::steady_clock::time_point;

    struct State
    {
        double x = Names::region_min_x;
        std::int64_t sequence = 0;
        double budget = Names::max_step;
        TimePoint updated_at;
    };

    Clock clock_;
    std::map<std::string, State> players_;

    static double available_budget (const State &state, TimePoint now)
    {
        const auto elapsed = std::chrono::duration<double> (now - state.updated_at).count ();
        if (elapsed <= 0)
            return state.budget;
        return std::min (static_cast<double> (Names::max_step),
                         state.budget + elapsed * Names::max_speed);
    }

  public:
    explicit Positions (Clock clock = [] { return std::chrono::steady_clock::now (); }) :
        clock_ (std::move (clock))
    {
    }

    std::size_t count () const noexcept { return players_.size (); }
    bool contains (const std::string &id) const { return players_.contains (id); }
    bool remove (const std::string &id) { return players_.erase (id) != 0; }

    void add (const std::string &id)
    {
        players_.try_emplace (id, State{Names::region_min_x, 0, Names::max_step, clock_ ()});
    }

    std::optional<ActionRejected> move (const std::string &id, const MoveRequest &request)
    {
        auto &state = players_.at (id);
        if (request.Sequence > Names::max_safe_integer)
            return ActionRejected{Rejection::integer_out_of_range, 0};
        if (request.Sequence <= state.sequence)
            return ActionRejected{Rejection::stale_sequence, 0};
        if (!std::isfinite (request.Delta))
            return ActionRejected{Rejection::non_finite, 0};

        const auto distance = std::abs (request.Delta);
        if (distance > Names::max_step)
            return ActionRejected{Rejection::step_too_large, 0};

        const auto next_x = state.x + request.Delta;
        if (next_x < Names::region_min_x || next_x > Names::region_max_x)
            return ActionRejected{Rejection::outside_region, 0};

        const auto now = clock_ ();
        const auto available = available_budget (state, now);
        if (distance > available) {
            const auto retry_after_ms = static_cast<std::int32_t> (
              std::ceil ((distance - available) / Names::max_speed * 1000.0));
            return ActionRejected{Rejection::move_budget, std::max<std::int32_t> (1, retry_after_ms)};
        }

        state.x = next_x;
        state.sequence = request.Sequence;
        state.budget = available - distance;
        state.updated_at = now;
        return std::nullopt;
    }

    std::vector<PlayerPosition> view () const
    {
        std::vector<PlayerPosition> result;
        result.reserve (players_.size ());
        for (const auto &[id, state] : players_)
            result.push_back ({id, state.x, state.sequence});
        return result;
    }

    PositionsSnapshot capture () const
    {
        const auto now = clock_ ();
        PositionsSnapshot snapshot;
        snapshot.Players.reserve (players_.size ());
        const auto maximum_remaining_ms = static_cast<std::int64_t> (
          std::ceil (Names::max_step / Names::max_speed * 1000.0));
        for (const auto &[id, state] : players_) {
            const auto available = available_budget (state, now);
            const auto remaining_ms = static_cast<std::int64_t> (
              std::ceil ((Names::max_step - available) / Names::max_speed * 1000.0));
            snapshot.Players.push_back ({id, state.x, state.sequence,
                                         std::clamp<std::int64_t> (remaining_ms, 0,
                                                                   maximum_remaining_ms)});
        }
        return snapshot;
    }

    void restore (const PositionsSnapshot &snapshot)
    {
        const auto maximum_remaining_ms = static_cast<std::int64_t> (
          std::ceil (Names::max_step / Names::max_speed * 1000.0));
        const auto now = clock_ ();
        std::map<std::string, State> restored;
        for (const auto &player : snapshot.Players) {
            if (player.PlayerId.empty () || !std::isfinite (player.X)
                || player.X < Names::region_min_x || player.X > Names::region_max_x
                || player.Sequence < 0 || player.Sequence > Names::max_safe_integer
                || player.RemainingMs < 0 || player.RemainingMs > maximum_remaining_ms)
                throw std::invalid_argument ("Invalid Region position snapshot");
            const auto budget = Names::max_step
                                - player.RemainingMs * Names::max_speed / 1000.0;
            if (!restored
                   .emplace (player.PlayerId,
                             State{player.X, player.Sequence, budget, now})
                   .second)
                throw std::invalid_argument ("Duplicate player in Region position snapshot");
        }
        players_.swap (restored);
    }
};
}

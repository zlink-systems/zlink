#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <functional>
#include <map>

namespace MyGame
{
struct Position
{
    double x = 0;
    bool operator== (const Position &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Position, x)
};
struct FieldPlayerSnapshot
{
    Position position;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
    bool connected = true;
    bool operator== (const FieldPlayerSnapshot &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldPlayerSnapshot, position, sequence, remainingMs, connected)
};
struct FieldPositionSnapshot
{
    std::string fieldId;
    std::map<std::string, FieldPlayerSnapshot> players;
    std::map<std::string, EnterField> arrivals;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldPositionSnapshot, fieldId, players, arrivals)
};
struct FieldSnapshot
{
    std::map<std::string, EnterField> visiblePlayers;
    FieldPositionSnapshot positions;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldSnapshot, visiblePlayers, positions)
};
struct FieldState
{
    // Visibility is an application projection; PlayerState owns movement and party membership.
    std::map<std::string, EnterField> visiblePlayers;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (FieldState, visiblePlayers)
};
struct MovementResult
{
    bool accepted = false;
    Position position;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
    std::optional<ActionRejected> rejection;
    std::string fieldId;
    bool relocates = false;
};
struct FieldLocationResolution
{
    std::optional<std::string> fieldId;
    std::optional<ActionRejected> rejection;
};
struct PlayerStateView
{
    Position position;
    std::int64_t sequence = 0;
    std::int32_t remainingMs = 0;
    bool connected = true;
    bool operator== (const PlayerStateView &) const = default;
};

class FieldPositionDomain
{
  public:
    static constexpr double max_step = 10;
    static constexpr double max_speed = max_step * 5;
    static constexpr std::int32_t full_budget_after_ms = 200;
    using monotonic_milliseconds_t = std::function<std::int64_t ()>;

  private:
    struct PlayerPositionState
    {
        Position position;
        std::int64_t sequence = 0;
        std::int64_t last_accepted_milliseconds = 0;
        bool connected = true;
    };

    std::string field_id_;
    monotonic_milliseconds_t clock_;
    std::map<std::string, PlayerPositionState> players_;
    std::map<std::string, EnterField> arrivals_;

    static std::int64_t read_monotonic_milliseconds ()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                 std::chrono::steady_clock::now ().time_since_epoch ())
          .count ();
    }
    static std::int32_t remaining_ms (std::int64_t accepted_at, std::int64_t now)
    {
        const auto elapsed = std::max<std::int64_t> (0, now - accepted_at);
        const auto remaining = static_cast<std::int64_t> (full_budget_after_ms) - elapsed;
        return static_cast<std::int32_t> (
          std::clamp<std::int64_t> (remaining, 0, full_budget_after_ms));
    }
    MovementResult reject (const std::string &actor_id,
                           const std::string &reason,
                           std::int32_t retry_after_ms = 0) const
    {
        const auto &player = players_.at (actor_id);
        return {false,
                player.position,
                player.sequence,
                remaining_ms (player.last_accepted_milliseconds, clock_ ()),
                ActionRejected{reason, retry_after_ms},
                field_id_,
                false};
    }

  public:
    explicit FieldPositionDomain (std::string field_id,
                                  monotonic_milliseconds_t monotonic_milliseconds = {}) :
        field_id_ (std::move (field_id)),
        clock_ (monotonic_milliseconds ? std::move (monotonic_milliseconds)
                                      : &FieldPositionDomain::read_monotonic_milliseconds)
    {
    }

    static FieldLocationResolution resolve_location (double x)
    {
        if (!std::isfinite (x))
            return {std::nullopt, ActionRejected{Rejection::non_finite}};
        if (x < 0 || x >= Names::field_width * Names::field_count)
            return {std::nullopt, ActionRejected{Rejection::outside_world}};
        return {Names::field_id (x), std::nullopt};
    }

    std::optional<ActionRejected> queue_arrival (const std::string &actor_id,
                                                 const EnterField &arrival)
    {
        const auto location = resolve_location (arrival.x);
        if (location.rejection)
            return location.rejection;
        if (arrival.sequence > Names::max_safe_integer)
            return ActionRejected{Rejection::integer_out_of_range};
        if (arrival.sequence < 0)
            return ActionRejected{Rejection::stale_sequence};
        if (arrival.remainingMs < 0 || arrival.remainingMs > full_budget_after_ms)
            return ActionRejected{Rejection::integer_out_of_range};
        if (*location.fieldId != field_id_)
            return ActionRejected{Rejection::outside_world};
        arrivals_[actor_id] = arrival;
        return std::nullopt;
    }

    bool try_take_arrival (const std::string &actor_id, EnterField &arrival)
    {
        const auto found = arrivals_.find (actor_id);
        if (found == arrivals_.end ())
            return false;
        arrival = found->second;
        arrivals_.erase (found);
        return true;
    }

    void add_player (const std::string &actor_id, const EnterField &arrival, bool connected = true)
    {
        const auto accepted_at = clock_ () - (full_budget_after_ms - arrival.remainingMs);
        players_[actor_id] = {{arrival.x}, arrival.sequence, accepted_at, connected};
    }

    MovementResult move (const std::string &actor_id, const MoveRequest &request)
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return {false,
                    {0},
                    0,
                    0,
                    ActionRejected{Rejection::stale_sequence},
                    field_id_,
                    false};
        const auto &player = found->second;
        if (!std::isfinite (request.delta))
            return reject (actor_id, Rejection::non_finite);
        if (std::abs (request.delta) > max_step)
            return reject (actor_id, Rejection::step_too_large);
        if (request.sequence > Names::max_safe_integer)
            return reject (actor_id, Rejection::integer_out_of_range);
        if (request.sequence < 0 || player.sequence >= Names::max_safe_integer
            || request.sequence <= player.sequence)
            return reject (actor_id, Rejection::stale_sequence);

        const auto next_x = player.position.x + request.delta;
        const auto location = resolve_location (next_x);
        if (location.rejection)
            return reject (actor_id, location.rejection->reason);

        const auto now = clock_ ();
        const auto elapsed_ms = std::max<std::int64_t> (0, now - player.last_accepted_milliseconds);
        const auto budget =
          std::min (max_step, max_speed * static_cast<double> (elapsed_ms) / 1000.0);
        const auto distance = std::abs (request.delta);
        if (distance > budget) {
            const auto retry_after_ms = static_cast<std::int32_t> (
              std::max (1.0, std::ceil ((distance - budget) * 1000.0 / max_speed)));
            return reject (actor_id, Rejection::move_budget, retry_after_ms);
        }

        const Position position{next_x};
        const auto next_field_id = *location.fieldId;
        const bool relocates = next_field_id != field_id_;
        if (!relocates)
            players_[actor_id] = {position, request.sequence, now, player.connected};
        return {true,
                position,
                request.sequence,
                full_budget_after_ms,
                std::nullopt,
                next_field_id,
                relocates};
    }

    PlayerStateView state (const std::string &actor_id) const
    {
        const auto &player = players_.at (actor_id);
        return {player.position,
                player.sequence,
                remaining_ms (player.last_accepted_milliseconds, clock_ ()),
                player.connected};
    }

    std::optional<PlayerStateView> remove_player (const std::string &actor_id)
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return std::nullopt;
        const auto result = PlayerStateView{
          found->second.position,
          found->second.sequence,
          remaining_ms (found->second.last_accepted_milliseconds, clock_ ()),
          found->second.connected};
        players_.erase (found);
        return result;
    }

    std::optional<PlayerStateView> set_connected (const std::string &actor_id, bool connected)
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return std::nullopt;
        found->second.connected = connected;
        return state (actor_id);
    }

    FieldPositionSnapshot capture () const
    {
        const auto now = clock_ ();
        FieldPositionSnapshot result;
        result.fieldId = field_id_;
        for (const auto &[actor_id, player] : players_)
            result.players.emplace (
              actor_id,
              FieldPlayerSnapshot{player.position,
                                  player.sequence,
                                  remaining_ms (player.last_accepted_milliseconds, now),
                                  player.connected});
        result.arrivals = arrivals_;
        return result;
    }

    void restore (const FieldPositionSnapshot &snapshot)
    {
        if (snapshot.fieldId != field_id_)
            throw std::invalid_argument ("Field relocation state belongs to a different Field.");
        players_.clear ();
        const auto now = clock_ ();
        for (const auto &[actor_id, state] : snapshot.players) {
            const auto accepted_at = now - (full_budget_after_ms - state.remainingMs);
            players_.emplace (actor_id,
                              PlayerPositionState{state.position,
                                                  state.sequence,
                                                  accepted_at,
                                                  state.connected});
        }
        arrivals_ = snapshot.arrivals;
    }
};
}

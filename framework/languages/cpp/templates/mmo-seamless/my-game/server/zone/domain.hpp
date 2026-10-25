#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace MyGame
{
class ZonePositionDomain
{
  public:
    static constexpr double MaxStep = 10;
    static constexpr double MaxSpeed = MaxStep * 5;
    static constexpr std::int32_t FullBudgetAfterMs = 200;
    using Clock = std::function<std::int64_t ()>;

    struct Location
    {
        std::optional<std::string> zoneId;
        std::optional<ActionRejected> rejection;
    };
    struct PlayerView
    {
        Position position;
        std::int64_t sequence = 0;
        std::int32_t remainingMs = 0;
        bool connected = false;
        bool operator== (const PlayerView &) const = default;
    };
    struct Movement
    {
        bool accepted = false;
        Position position;
        std::int64_t sequence = 0;
        std::int32_t remainingMs = 0;
        std::optional<ActionRejected> rejection;
        std::string zoneId;
        bool relocates = false;
    };
    struct PlayerSnapshot
    {
        Position position;
        std::int64_t sequence = 0;
        std::int32_t remainingMs = 0;
        bool connected = false;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerSnapshot, position, sequence, remainingMs, connected)
    };
    struct Snapshot
    {
        std::string zoneId;
        std::map<std::string, PlayerSnapshot> players;
        std::map<std::string, EnterZone> arrivals;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE (Snapshot, zoneId, players, arrivals)
    };

  private:
    struct Player
    {
        Position position;
        std::int64_t sequence = 0;
        std::int64_t lastAcceptedMilliseconds = 0;
        bool connected = true;
    };
    std::string zone_id_;
    Clock clock_;
    std::map<std::string, Player> players_;
    std::map<std::string, EnterZone> arrivals_;

    static std::int64_t read_monotonic_milliseconds ()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                 std::chrono::steady_clock::now ().time_since_epoch ())
          .count ();
    }
    static std::int32_t remaining_ms (std::int64_t accepted_at, std::int64_t now)
    {
        const auto elapsed = std::max<std::int64_t> (0, now - accepted_at);
        return static_cast<std::int32_t> (
          std::clamp<std::int64_t> (FullBudgetAfterMs - elapsed, 0, FullBudgetAfterMs));
    }
    Movement reject (const Player &player,
                     std::string reason,
                     std::int32_t retry_after_ms = 0) const
    {
        return {false,
                player.position,
                player.sequence,
                remaining_ms (player.lastAcceptedMilliseconds, clock_ ()),
                ActionRejected{std::move (reason), retry_after_ms},
                zone_id_,
                false};
    }

  public:
    explicit ZonePositionDomain (std::string zone_id, Clock clock = {}) :
        zone_id_ (std::move (zone_id)),
        clock_ (clock ? std::move (clock) : Clock{&ZonePositionDomain::read_monotonic_milliseconds})
    {
    }

    static Location resolve_location (double x)
    {
        if (!std::isfinite (x))
            return {std::nullopt, ActionRejected{"non-finite", 0}};
        if (x < 0 || x >= Names::world_width)
            return {std::nullopt, ActionRejected{"outside-world", 0}};
        return {Names::zone_id (x), std::nullopt};
    }

    std::optional<ActionRejected> queue_arrival (const std::string &actor_id,
                                                 const EnterZone &arrival)
    {
        const auto location = resolve_location (arrival.position.x);
        if (location.rejection)
            return location.rejection;
        if (arrival.sequence > Limits::max_safe_integer)
            return ActionRejected{"integer-out-of-range", 0};
        if (arrival.sequence < 0)
            return ActionRejected{"stale-sequence", 0};
        if (arrival.remainingMs < 0 || arrival.remainingMs > FullBudgetAfterMs)
            return ActionRejected{"integer-out-of-range", 0};
        if (*location.zoneId != zone_id_)
            return ActionRejected{"outside-world", 0};
        if (!arrivals_.emplace (actor_id, arrival).second)
            throw std::runtime_error ("Duplicate zone admission");
        return std::nullopt;
    }

    bool take_arrival (const std::string &actor_id, EnterZone &arrival)
    {
        const auto found = arrivals_.find (actor_id);
        if (found == arrivals_.end ())
            return false;
        arrival = found->second;
        arrivals_.erase (found);
        return true;
    }

    void add_player (const std::string &actor_id, const EnterZone &arrival, bool connected = true)
    {
        const auto accepted_at = clock_ () - (FullBudgetAfterMs - arrival.remainingMs);
        players_[actor_id] = {arrival.position, arrival.sequence, accepted_at, connected};
    }

    Movement move (const std::string &actor_id, const MoveRequest &request)
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return {false,
                    {},
                    0,
                    0,
                    ActionRejected{"stale-sequence", 0},
                    zone_id_,
                    false};
        auto &player = found->second;
        if (!std::isfinite (request.delta))
            return reject (player, "non-finite");
        if (std::abs (request.delta) > MaxStep)
            return reject (player, "step-too-large");
        if (request.sequence > Limits::max_safe_integer)
            return reject (player, "integer-out-of-range");
        if (request.sequence < 0 || player.sequence >= Limits::max_safe_integer
            || request.sequence <= player.sequence)
            return reject (player, "stale-sequence");

        const auto next_x = player.position.x + request.delta;
        const auto location = resolve_location (next_x);
        if (location.rejection)
            return reject (player, location.rejection->reason);

        const auto now = clock_ ();
        const auto elapsed_ms = std::max<std::int64_t> (0, now - player.lastAcceptedMilliseconds);
        const auto budget = std::min (MaxStep, MaxSpeed * elapsed_ms / 1000.0);
        const auto distance = std::abs (request.delta);
        if (distance > budget) {
            const auto retry_after_ms = static_cast<std::int32_t> (
              std::max (1.0, std::ceil ((distance - budget) * 1000 / MaxSpeed)));
            return reject (player, "move-budget", retry_after_ms);
        }

        const Position position{next_x};
        const auto next_zone_id = *location.zoneId;
        const auto relocates = next_zone_id != zone_id_;
        if (!relocates)
            player = {position, request.sequence, now, player.connected};
        return {true,
                position,
                request.sequence,
                FullBudgetAfterMs,
                std::nullopt,
                next_zone_id,
                relocates};
    }

    PlayerView state (const std::string &actor_id) const
    {
        const auto &player = players_.at (actor_id);
        return {player.position,
                player.sequence,
                remaining_ms (player.lastAcceptedMilliseconds, clock_ ()),
                player.connected};
    }

    bool try_state (const std::string &actor_id, PlayerView &state) const
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return false;
        const auto &player = found->second;
        state = {player.position,
                 player.sequence,
                 remaining_ms (player.lastAcceptedMilliseconds, clock_ ()),
                 player.connected};
        return true;
    }

    auto connected_players () const
    {
        const auto now = clock_ ();
        auto connected = std::views::filter (
          players_, [] (const auto &entry) { return entry.second.connected; });
        return std::views::transform (connected, [this, now] (const auto &entry) {
            return std::pair<const std::string &, PlayerView>{
              entry.first,
              PlayerView{entry.second.position,
                         entry.second.sequence,
                         remaining_ms (entry.second.lastAcceptedMilliseconds, now),
                         true}};
        });
    }

    std::optional<PlayerView> set_connected (const std::string &actor_id, bool connected)
    {
        const auto found = players_.find (actor_id);
        if (found == players_.end ())
            return std::nullopt;
        found->second.connected = connected;
        return state (actor_id);
    }

    std::optional<PlayerView> remove_player (const std::string &actor_id)
    {
        PlayerView previous;
        if (!try_state (actor_id, previous))
            return std::nullopt;
        players_.erase (actor_id);
        return previous;
    }

    Snapshot capture () const
    {
        Snapshot snapshot;
        snapshot.zoneId = zone_id_;
        const auto now = clock_ ();
        for (const auto &[id, player] : players_)
            snapshot.players.emplace (
              id,
              PlayerSnapshot{player.position,
                             player.sequence,
                             remaining_ms (player.lastAcceptedMilliseconds, now),
                             player.connected});
        snapshot.arrivals = arrivals_;
        return snapshot;
    }

    void restore (const Snapshot &snapshot)
    {
        if (snapshot.zoneId != zone_id_)
            throw std::invalid_argument ("Zone relocation state belongs to a different Zone");
        players_.clear ();
        arrivals_ = snapshot.arrivals;
        const auto now = clock_ ();
        for (const auto &[id, state] : snapshot.players) {
            const auto accepted_at = now - (FullBudgetAfterMs - state.remainingMs);
            players_.emplace (id,
                              Player{state.position, state.sequence, accepted_at, state.connected});
        }
    }
};
}

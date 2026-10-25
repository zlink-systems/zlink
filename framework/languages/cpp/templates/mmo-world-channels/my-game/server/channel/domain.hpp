#pragma once
#include "server/shared/contracts.hpp"
#include <chrono>
#include <functional>
#include <optional>

namespace MyGame
{
struct GoldEarnDecision
{
    std::optional<std::int64_t> newValue;
    std::string reason;
    std::int32_t retryAfterMs = 0;
    bool accepted () const noexcept { return reason.empty (); }
};

struct GoldSnapshot
{
    std::int64_t gold = 0;
    std::int64_t earnCooldownRemainingMs = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (GoldSnapshot, gold, earnCooldownRemainingMs)
};

class GoldBalance
{
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Now = std::function<TimePoint ()>;
    static constexpr std::chrono::milliseconds earn_interval{GameRules::earn_interval_ms};
    std::int64_t value_ = 0;
    Now now_;
    std::optional<TimePoint> last_earned_;

    static std::int64_t validate (std::int64_t value)
    {
        if (value < 0 || value > GameRules::max_integer)
            throw std::invalid_argument ("Gold is outside the supported range");
        return value;
    }

  public:
    explicit GoldBalance (std::int64_t value = 0, Now now = [] { return Clock::now (); }) :
        value_ (validate (value)), now_ (std::move (now))
    {
    }

    std::int64_t value () const noexcept { return value_; }

    std::int64_t earn_cooldown_remaining_ms () const
    {
        if (!last_earned_)
            return 0;
        const auto remaining = earn_interval - (now_ () - *last_earned_);
        return remaining <= Clock::duration::zero ()
          ? 0
          : std::chrono::ceil<std::chrono::milliseconds> (remaining).count ();
    }

    GoldEarnDecision evaluate_earn () const
    {
        const auto retry = earn_cooldown_remaining_ms ();
        if (retry > 0)
            return {std::nullopt, Rejection::earn_too_soon, static_cast<std::int32_t> (retry)};
        if (value_ > GameRules::max_integer - GameRules::earn_amount)
            return {std::nullopt, Rejection::gold_limit, 0};
        return {value_ + GameRules::earn_amount, {}, 0};
    }

    void commit_earn (const GoldEarnDecision &decision)
    {
        if (!decision.accepted () || decision.newValue != value_ + GameRules::earn_amount)
            throw std::invalid_argument ("Only an accepted gold decision can be committed");
        value_ = *decision.newValue;
        last_earned_ = now_ ();
    }

    GoldSnapshot capture () const { return {value_, earn_cooldown_remaining_ms ()}; }

    void restore (const GoldSnapshot &snapshot)
    {
        const auto value = validate (snapshot.gold);
        if (snapshot.earnCooldownRemainingMs < 0
            || snapshot.earnCooldownRemainingMs > GameRules::earn_interval_ms)
            throw std::invalid_argument ("Gold cooldown snapshot is outside the supported range");
        value_ = value;
        if (snapshot.earnCooldownRemainingMs == 0) {
            last_earned_.reset ();
            return;
        }
        const auto elapsed = earn_interval -
          std::chrono::milliseconds (snapshot.earnCooldownRemainingMs);
        last_earned_ = now_ () - std::chrono::duration_cast<Clock::duration> (elapsed);
    }
};
}

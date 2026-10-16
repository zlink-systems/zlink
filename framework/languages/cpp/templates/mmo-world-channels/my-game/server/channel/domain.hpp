#pragma once
#include "server/shared/contracts.hpp"

namespace MyGame
{
class GoldBalance
{
    static constexpr std::int64_t maximum_gold = 9007199254740991;
    std::int64_t value_ = 0;

  public:
    explicit GoldBalance (std::int64_t value = 0) { restore (value); }
    std::int64_t value () const noexcept { return value_; }
    std::int64_t after_earning (std::int64_t amount) const
    {
        if (amount <= 0 || amount > maximum_gold - value_)
            throw std::invalid_argument ("Amount must be positive and gold must not overflow");
        return value_ + amount;
    }
    void restore (std::int64_t value)
    {
        if (value < 0 || value > maximum_gold)
            throw std::invalid_argument ("Gold must be nonnegative");
        value_ = value;
    }
};
}

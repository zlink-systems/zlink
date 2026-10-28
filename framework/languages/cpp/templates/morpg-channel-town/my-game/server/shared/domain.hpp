#pragma once
#include "contracts.hpp"
#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <vector>
namespace MyGame
{
inline bool insert_sorted_unique (std::vector<std::string> &values, std::string_view value)
{
    const auto position = std::lower_bound (values.begin (), values.end (), value);
    if (position != values.end () && *position == value)
        return false;
    values.insert (position, std::string (value));
    return true;
}

inline std::string server_id (std::string_view prefix)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device source;
    std::uniform_int_distribution<unsigned int> byte (0, 255);
    std::string result (prefix);
    for (int i = 0; i < 16; ++i) {
        const auto value = byte (source);
        result.push_back (digits[value >> 4]);
        result.push_back (digits[value & 0x0f]);
    }
    return result;
}
inline bool blank (const std::string &value)
{
    return value.find_first_not_of (" \t\r\n") == std::string::npos;
}
struct ChannelRules
{
    static std::optional<std::string> validate (std::int64_t channel)
    {
        if (channel > Names::maximum_integer || channel < -Names::maximum_integer)
            return "integer-out-of-range";
        if (channel < 1 || channel > Names::channel_count)
            return "invalid-channel";
        return std::nullopt;
    }
};
}

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <array>
#include <limits>
#include <string_view>

namespace zlink::detail::stream_wire
{
inline constexpr std::string_view reserved_packet_name_prefix = "$zlink.";
enum class packet_name_error_t
{
    none,
    invalid,
    reserved
};

inline packet_name_error_t validate_packet_name (std::string_view name, bool allow_reserved = false)
{
    if (name.size () > std::numeric_limits<std::uint8_t>::max ())
        return packet_name_error_t::invalid;
    // Unicode White_Space, encoded as UTF-8. ASCII members use the fast scan below.
    static constexpr std::array<std::string_view, 19> unicode_whitespace = {
      "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81",
      "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86",
      "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
      "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
    auto remainder = name;
    while (!remainder.empty ()) {
        const auto non_ascii_space = remainder.find_first_not_of (" \t\r\n\f\v");
        if (non_ascii_space == std::string_view::npos) {
            remainder = {};
            break;
        }
        remainder.remove_prefix (non_ascii_space);
        if (static_cast<unsigned char> (remainder.front ()) < 0x80)
            break;
        bool matched = false;
        for (auto whitespace : unicode_whitespace) {
            if (remainder.starts_with (whitespace)) {
                remainder.remove_prefix (whitespace.size ());
                matched = true;
                break;
            }
        }
        if (!matched)
            break;
    }
    if (remainder.empty ()) {
        return packet_name_error_t::invalid;
    }
    if (!allow_reserved && name.starts_with (reserved_packet_name_prefix)) {
        return packet_name_error_t::reserved;
    }
    return packet_name_error_t::none;
}
} // namespace zlink::detail::stream_wire

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
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
    if (name.size () > std::numeric_limits<std::uint8_t>::max ()
        || name.find_first_not_of (" \t\r\n\f\v") == std::string_view::npos) {
        return packet_name_error_t::invalid;
    }
    if (!allow_reserved && name.starts_with (reserved_packet_name_prefix)) {
        return packet_name_error_t::reserved;
    }
    return packet_name_error_t::none;
}
} // namespace zlink::detail::stream_wire

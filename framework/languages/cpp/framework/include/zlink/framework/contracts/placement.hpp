/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/detail/utf8.hpp>

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework
{

namespace detail
{

inline constexpr std::size_t identifier_max_bytes = 255;

inline std::string placement_value (std::string value, std::string_view name)
{
    if (value.empty () || value.size () > identifier_max_bytes || !is_valid_non_nul_utf8 (value))
        throw std::invalid_argument (std::string (name)
                                     + " must contain 1..255 non-NUL UTF-8 bytes");
    return value;
}

} // namespace detail

} // namespace zlink::framework

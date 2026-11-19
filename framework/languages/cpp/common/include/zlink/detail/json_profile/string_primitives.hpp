/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace zlink::detail::json_profile
{
// The caller has consumed the lead byte. Consume its continuation bytes in
// the same traversal that recognizes JSON escapes and string boundaries.
inline bool consume_utf8 (std::string_view value, std::size_t &index, unsigned char lead) noexcept
{
    const auto remaining = lead >= 0xc2 && lead <= 0xdf   ? 1
                           : lead >= 0xe0 && lead <= 0xef ? 2
                           : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                          : 0;
    if (remaining == 0 || value.size () - index < static_cast<std::size_t> (remaining))
        return false;
    const auto second = static_cast<unsigned char> (value[index]);
    if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)
        || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
        return false;
    for (int part = 0; part < remaining; ++part) {
        const auto byte = static_cast<unsigned char> (value[index++]);
        if (byte < 0x80 || byte > 0xbf)
            return false;
    }
    return true;
}

inline std::size_t plain_ascii_prefix (std::uint64_t word)
{
    constexpr std::uint64_t high = 0x8080808080808080ULL, ones = 0x0101010101010101ULL,
                            spaces = 0x2020202020202020ULL;
    const auto quotes = word ^ 0x2222222222222222ULL, slashes = word ^ 0x5c5c5c5c5c5c5c5cULL;
    const auto special = (word & high) | ((word - spaces) & ~word & high)
                         | ((quotes - ones) & ~quotes & high)
                         | ((slashes - ones) & ~slashes & high);
    const auto leading = std::endian::native == std::endian::little ? std::countr_zero (special)
                                                                    : std::countl_zero (special);
    return static_cast<std::size_t> (leading / 8);
}
inline std::size_t plain_ascii_prefix (const char *text, std::size_t size)
{
    std::size_t offset = 0;
    while (size - offset >= 8) {
        std::uint64_t word;
        std::memcpy (&word, text + offset, sizeof word);
        const auto count = plain_ascii_prefix (word);
        offset += count;
        if (count != 8)
            return offset;
    }
    if (offset != size) {
        // Zero padding supplies a control-byte sentinel without reading past size.
        std::uint64_t word = 0;
        std::memcpy (&word, text + offset, size - offset);
        offset += plain_ascii_prefix (word);
    }
    return offset;
}

}

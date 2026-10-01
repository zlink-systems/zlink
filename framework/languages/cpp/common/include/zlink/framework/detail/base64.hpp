/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace zlink::framework::detail
{
inline constexpr std::string_view base64_alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode (std::span<const std::byte> bytes)
{
    constexpr auto alphabet = base64_alphabet;
    std::string encoded;
    encoded.reserve (((bytes.size () + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size (); offset += 3) {
        const auto remaining = bytes.size () - offset;
        const auto first = std::to_integer<unsigned> (bytes[offset]);
        const auto second = remaining > 1 ? std::to_integer<unsigned> (bytes[offset + 1]) : 0u;
        const auto third = remaining > 2 ? std::to_integer<unsigned> (bytes[offset + 2]) : 0u;
        encoded.push_back (alphabet[first >> 2]);
        encoded.push_back (alphabet[((first & 0x03u) << 4) | (second >> 4)]);
        encoded.push_back (remaining > 1 ? alphabet[((second & 0x0fu) << 2) | (third >> 6)] : '=');
        encoded.push_back (remaining > 2 ? alphabet[third & 0x3fu] : '=');
    }
    return encoded;
}

} // namespace zlink::framework::detail

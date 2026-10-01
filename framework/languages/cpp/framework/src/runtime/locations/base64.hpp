/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// RFC 4648 standard base64 (with '+'/'/' and '=' padding) -- NOT the
// URL-safe alphabet. Used by the store record layer (21-location-runtime.md
// #2.4) to encode the authority record's opaque `payload` field.

#include <zlink/framework/detail/base64.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

using zlink::framework::detail::base64_encode;

inline std::vector<std::byte> base64_decode (std::string_view encoded)
{
    constexpr auto alphabet = zlink::framework::detail::base64_alphabet;
    if (encoded.size () % 4 != 0)
        throw std::invalid_argument ("base64 payload has an invalid length");
    const auto symbol = [&] (char character) -> unsigned {
        const auto position = alphabet.find (character);
        if (position == std::string_view::npos)
            throw std::invalid_argument ("base64 payload contains an invalid symbol");
        return static_cast<unsigned> (position);
    };
    std::vector<std::byte> result;
    result.reserve ((encoded.size () / 4) * 3);
    for (std::size_t offset = 0; offset < encoded.size (); offset += 4) {
        const bool final_group = offset + 4 == encoded.size ();
        const bool pad_two = encoded[offset + 2] == '=';
        const bool pad_one = encoded[offset + 3] == '=';
        if (encoded[offset] == '=' || encoded[offset + 1] == '='
            || (pad_two && (!pad_one || !final_group)) || (pad_one && !final_group))
            throw std::invalid_argument ("base64 payload has invalid padding");
        const auto first = symbol (encoded[offset]);
        const auto second = symbol (encoded[offset + 1]);
        const auto third = pad_two ? 0u : symbol (encoded[offset + 2]);
        const auto fourth = pad_one ? 0u : symbol (encoded[offset + 3]);
        result.push_back (static_cast<std::byte> ((first << 2) | (second >> 4)));
        if (!pad_two)
            result.push_back (static_cast<std::byte> ((second << 4) | (third >> 2)));
        if (!pad_one)
            result.push_back (static_cast<std::byte> ((third << 6) | fourth));
    }
    return result;
}

} // namespace zlink::framework::runtime

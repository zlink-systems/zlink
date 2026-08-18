/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// RFC 4648 standard base64 (with '+'/'/' and '=' padding) -- NOT the
// URL-safe alphabet. Used by the store record layer (21-location-runtime.md
// #2.4) to encode the authority record's opaque `payload` field.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

inline std::string base64_encode (const std::vector<std::byte> &bytes)
{
    static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve (((bytes.size () + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size (); offset += 3) {
        const auto remaining = bytes.size () - offset;
        const auto first = std::to_integer<unsigned> (bytes[offset]);
        const auto second =
          remaining > 1 ? std::to_integer<unsigned> (bytes[offset + 1]) : 0u;
        const auto third =
          remaining > 2 ? std::to_integer<unsigned> (bytes[offset + 2]) : 0u;
        encoded.push_back (alphabet[first >> 2]);
        encoded.push_back (alphabet[((first & 0x03u) << 4) | (second >> 4)]);
        encoded.push_back (
          remaining > 1 ? alphabet[((second & 0x0fu) << 2) | (third >> 6)] : '=');
        encoded.push_back (remaining > 2 ? alphabet[third & 0x3fu] : '=');
    }
    return encoded;
}

inline std::vector<std::byte> base64_decode (std::string_view encoded)
{
    static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

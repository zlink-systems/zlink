/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{

// 21-location-runtime.md#2.4: requestContentReference is `inline-v1:{base64url}`
// over `A-Z a-z 0-9 - _` with no `=` padding, and no other form is recognized.
// The record's requestEncodedSize and requestSha256 decide the content's
// integrity, so the reference carries no checksum segment of its own.
inline constexpr std::string_view inline_creation_content_prefix = "inline-v1:";

inline std::string encode_inline_creation_content (
  const std::vector<std::byte> &payload)
{
    constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result{inline_creation_content_prefix};
    result.reserve (
      result.size () + (payload.size () * 4u + 2u) / 3u);
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const auto value : payload) {
        accumulator =
          (accumulator << 8u) | std::to_integer<std::uint8_t> (value);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result.push_back (alphabet[(accumulator >> bits) & 0x3fu]);
        }
    }
    if (bits != 0)
        result.push_back (alphabet[(accumulator << (6u - bits)) & 0x3fu]);
    return result;
}

// Decodes the reference and verifies it against the same atomic record's
// requestEncodedSize and requestSha256. A reference outside the specified
// form, a length mismatch or a digest mismatch all return nullopt, and the
// caller must record the creation as failed without running the factory.
inline std::optional<std::vector<std::byte>>
decode_inline_creation_content (std::string_view reference,
                                const std::array<std::byte, 32> &expected_sha256,
                                std::uint64_t expected_encoded_size)
{
    if (!reference.starts_with (inline_creation_content_prefix))
        return std::nullopt;
    const auto encoded =
      reference.substr (inline_creation_content_prefix.size ());
    if (encoded.size () % 4u == 1u)
        return std::nullopt;
    std::vector<std::byte> payload;
    payload.reserve ((encoded.size () * 3u) / 4u + 2u);
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const auto value : encoded) {
        std::uint32_t decoded = 0;
        if (value >= 'A' && value <= 'Z')
            decoded = static_cast<std::uint32_t> (value - 'A');
        else if (value >= 'a' && value <= 'z')
            decoded = static_cast<std::uint32_t> (value - 'a' + 26);
        else if (value >= '0' && value <= '9')
            decoded = static_cast<std::uint32_t> (value - '0' + 52);
        else if (value == '-')
            decoded = 62;
        else if (value == '_')
            decoded = 63;
        else
            return std::nullopt;
        accumulator = (accumulator << 6u) | decoded;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            payload.push_back (static_cast<std::byte> (
              (accumulator >> bits) & 0xffu));
        }
    }
    if (payload.size () != expected_encoded_size)
        return std::nullopt;
    if (sha256 (payload) != expected_sha256)
        return std::nullopt;
    return payload;
}

} // namespace zlink::framework::runtime

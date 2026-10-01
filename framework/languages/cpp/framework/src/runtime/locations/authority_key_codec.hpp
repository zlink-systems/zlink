/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/detail/utf8.hpp>

#include <runtime/locations/location_repository.hpp>

#include "../../../../../../runtime/protocol/generated/cpp/service_wire_constants.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::runtime
{

namespace authority_key_codec_detail
{

inline constexpr std::string_view prefix = "zla1:";
inline constexpr std::size_t identity_byte_limit = protocol::shortTextBytes;
inline constexpr std::size_t percent_encoded_byte_width = 3;
inline constexpr std::size_t identity_length_digits =
  std::numeric_limits<unsigned char>::digits10 + 1;
inline constexpr std::size_t encoded_key_byte_limit =
  prefix.size () + 2 + identity_length_digits + 1
  + identity_byte_limit * percent_encoded_byte_width;

struct decoded_authority_key_t
{
    char kind = 0;
    std::string object_id;
};

inline bool is_unreserved (unsigned char byte) noexcept
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
           || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_'
           || byte == '~';
}


inline authority_key_t encode_authority_key (char kind, std::string_view object_id)
{
    if (object_id.empty () || object_id.size () > identity_byte_limit
        || !zlink::framework::detail::is_valid_non_nul_utf8 (object_id)) {
        throw std::invalid_argument (
          "authority identity must contain 1..255 valid UTF-8 bytes without NUL");
    }
    std::string encoded;
    encoded.reserve (object_id.size ());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto character : object_id) {
        const auto byte = static_cast<unsigned char> (character);
        if (is_unreserved (byte)) {
            encoded.push_back (static_cast<char> (byte));
        } else {
            encoded.push_back ('%');
            encoded.push_back (hex[byte >> 4]);
            encoded.push_back (hex[byte & 0x0f]);
        }
    }
    return authority_key_t{std::string (prefix) + std::string (1, kind) + ":"
                           + std::to_string (object_id.size ()) + ":" + encoded};
}

inline std::optional<decoded_authority_key_t> decode_authority_key (std::string_view value)
{
    if (!value.starts_with (prefix) || value.size () < prefix.size () + 4
        || value.size () > encoded_key_byte_limit)
        return std::nullopt;
    const auto kind = value[prefix.size ()];
    if ((kind != 'a' && kind != 's') || value[prefix.size () + 1] != ':')
        return std::nullopt;
    const auto length_end = value.find (':', prefix.size () + 2);
    if (length_end == std::string_view::npos)
        return std::nullopt;
    std::size_t expected_size = 0;
    const auto length = value.substr (prefix.size () + 2, length_end - prefix.size () - 2);
    if (length.empty () || (length.size () > 1 && length.front () == '0'))
        return std::nullopt;
    for (const auto digit : length) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        if (expected_size > (identity_byte_limit - static_cast<std::size_t> (digit - '0')) / 10)
            return std::nullopt;
        expected_size = expected_size * 10 + static_cast<std::size_t> (digit - '0');
    }
    if (expected_size == 0 || expected_size > identity_byte_limit)
        return std::nullopt;
    const auto hex = [] (char digit) -> int {
        if (digit >= '0' && digit <= '9')
            return digit - '0';
        if (digit >= 'A' && digit <= 'F')
            return digit - 'A' + 10;
        return -1;
    };
    std::string decoded;
    const auto encoded = value.substr (length_end + 1);
    decoded.reserve (expected_size);
    for (std::size_t index = 0; index < encoded.size (); ++index) {
        if (encoded[index] != '%') {
            if (!is_unreserved (static_cast<unsigned char> (encoded[index])))
                return std::nullopt;
            decoded.push_back (encoded[index]);
            continue;
        }
        if (index + 2 >= encoded.size ())
            return std::nullopt;
        const auto high = hex (encoded[index + 1]);
        const auto low = hex (encoded[index + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;
        const auto byte = static_cast<unsigned char> ((high << 4) | low);
        if (is_unreserved (byte))
            return std::nullopt;
        decoded.push_back (static_cast<char> (byte));
        index += 2;
    }
    if (decoded.size () != expected_size
        || !zlink::framework::detail::is_valid_non_nul_utf8 (decoded))
        return std::nullopt;
    return decoded_authority_key_t{kind, std::move (decoded)};
}

} // namespace authority_key_codec_detail

inline authority_key_t actor_authority_key (std::string_view actor_id)
{
    return authority_key_codec_detail::encode_authority_key ('a', actor_id);
}

inline authority_key_t spot_authority_key (std::string_view spot_id)
{
    return authority_key_codec_detail::encode_authority_key ('s', spot_id);
}

} // namespace zlink::framework::runtime

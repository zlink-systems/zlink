/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace zlink::framework::runtime
{

namespace authority_key_codec_detail
{

struct decoded_authority_key_t
{
    char kind = 0;
    std::string object_id;
};

inline authority_key_t encode_authority_key (
  char kind,
  std::string_view object_id)
{
    std::string encoded;
    encoded.reserve (object_id.size ());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto character : object_id) {
        const auto byte = static_cast<unsigned char> (character);
        const auto unreserved =
          (byte >= 'A' && byte <= 'Z')
          || (byte >= 'a' && byte <= 'z')
          || (byte >= '0' && byte <= '9')
          || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (unreserved) {
            encoded.push_back (static_cast<char> (byte));
        }
        else {
            encoded.push_back ('%');
            encoded.push_back (hex[byte >> 4]);
            encoded.push_back (hex[byte & 0x0f]);
        }
    }
    return authority_key_t{
      "zla1:" + std::string (1, kind) + ":"
      + std::to_string (object_id.size ()) + ":" + encoded};
}

inline std::optional<decoded_authority_key_t>
decode_authority_key (std::string_view value)
{
    constexpr std::string_view prefix = "zla1:";
    if (!value.starts_with (prefix) || value.size () < prefix.size () + 4)
        return std::nullopt;
    const auto kind = value[prefix.size ()];
    if ((kind != 'a' && kind != 's') || value[prefix.size () + 1] != ':')
        return std::nullopt;
    const auto length_end = value.find (':', prefix.size () + 2);
    if (length_end == std::string_view::npos)
        return std::nullopt;
    std::size_t expected_size = 0;
    const auto length = value.substr (prefix.size () + 2,
                                      length_end - prefix.size () - 2);
    if (length.empty ())
        return std::nullopt;
    for (const auto digit : length) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        if (expected_size > (255 - static_cast<std::size_t> (digit - '0')) / 10)
            return std::nullopt;
        expected_size = expected_size * 10 + static_cast<std::size_t> (digit - '0');
    }
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
            decoded.push_back (encoded[index]);
            continue;
        }
        if (index + 2 >= encoded.size ())
            return std::nullopt;
        const auto high = hex (encoded[index + 1]);
        const auto low = hex (encoded[index + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;
        decoded.push_back (static_cast<char> ((high << 4) | low));
        index += 2;
    }
    if (decoded.size () != expected_size)
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

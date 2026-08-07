/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>

#include <string>
#include <string_view>

namespace zlink::framework::runtime
{

namespace authority_key_codec_detail
{

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

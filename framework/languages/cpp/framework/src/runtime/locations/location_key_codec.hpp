/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/locations/legacy_location_rows.hpp"

#include <zlink/framework/contracts/locations/keys.hpp>
#include <zlink/framework/contracts/spots/spot_identity.hpp>

#include <string>

namespace zlink::framework::runtime
{

class location_key_codec_t
{
  public:
    static std::string encode_spot_key (const spot_location_key_t &key)
    {
        detail::require_spot_id (key.spot_id);
        return encode (key.spot_id);
    }

    static std::string encode_actor_key (const actor_location_key_t &key)
    {
        return encode (key.mesh_name, key.actor_id);
    }

    static std::string encode_route_key (const route_location_key_t &key)
    {
        return encode (std::to_string (static_cast<int> (key.route_kind)), key.route_key);
    }

    static std::optional<std::string> normalize_actor_type (
      std::optional<std::string> actor_type)
    {
        return actor_type;
    }

  private:
    template <typename... TSegments> static std::string encode (const TSegments &...segments)
    {
        std::string result;
        (append_segment (result, segments), ...);
        return result;
    }

    static void append_segment (std::string &result, const std::string &segment)
    {
        result += std::to_string (segment.size ());
        result += ':';
        result += segment;
    }
};

} // namespace zlink::framework::runtime

/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/spots/spot.hpp>
#include "runtime/utils/uuid.hpp"
#include <zlink/framework/detail/utf8.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace zlink::framework
{

namespace detail
{
namespace
{
constexpr std::string_view entry_spot_marker = "-entry-";

} // namespace

bool valid_spot_id (std::string_view value) noexcept
{
    return !value.empty () && value.size () <= identifier_max_bytes
           && is_valid_non_nul_utf8 (value);
}

void require_spot_id (std::string_view value)
{
    if (!valid_spot_id (value))
        throw std::invalid_argument ("SpotId must contain 1..255 bytes of valid UTF-8");
}

spot_id_t new_user_spot_id ()
{
    return new_uuid_v4 ();
}

spot_id_t new_entry_spot_id (std::string_view diagnostic_prefix)
{
    std::string value (diagnostic_prefix);
    value += entry_spot_marker;
    value += new_uuid_v4 ();
    require_spot_id (value);
    return value;
}

bool is_framework_entry_spot_id (std::string_view value) noexcept
{
    const auto marker = value.rfind (entry_spot_marker);
    if (marker == std::string_view::npos
        || value.size () - marker - entry_spot_marker.size () != uuid_text_length)
        return false;
    const auto uuid = value.substr (marker + entry_spot_marker.size ());
    if (!is_lowercase_uuid_text (uuid))
        return false;
    return uuid[14] == '4'
           && (uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
}
} // namespace detail

} // namespace zlink::framework

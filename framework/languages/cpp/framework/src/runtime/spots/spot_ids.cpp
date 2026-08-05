/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/spots/spot.hpp>

#include <array>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace zlink::framework
{

namespace detail
{
namespace
{
bool valid_utf8 (std::string_view value) noexcept
{
    for (std::size_t index = 0; index < value.size ();) {
        const auto first =
          static_cast<std::uint8_t> (value[index]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7f) {
            if (first == 0)
                return false;
            ++index;
            continue;
        }
        if ((first & 0xe0u) == 0xc0u) {
            continuation = 1;
            codepoint = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation = 2;
            codepoint = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (value.size () - index - 1 < continuation)
            return false;
        for (std::size_t part = 0; part < continuation; ++part) {
            const auto next =
              static_cast<std::uint8_t> (value[index + part + 1]);
            if ((next & 0xc0u) != 0x80u)
                return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 1 && codepoint < 0x80)
            || (continuation == 2 && codepoint < 0x800)
            || (continuation == 3 && codepoint < 0x10000)
            || codepoint > 0x10ffff
            || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
        index += continuation + 1;
    }
    return true;
}

std::string uuid_v4 ()
{
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto &byte : bytes)
        byte = static_cast<std::uint8_t> (random ());
    bytes[6] = static_cast<std::uint8_t> (
      (bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<std::uint8_t> (
      (bytes[8] & 0x3fu) | 0x80u);

    std::ostringstream text;
    text << std::hex << std::nouppercase << std::setfill ('0');
    for (std::size_t index = 0; index < bytes.size (); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text << '-';
        text << std::setw (2) << static_cast<unsigned int> (bytes[index]);
    }
    return text.str ();
}
} // namespace

bool valid_spot_id (std::string_view value) noexcept
{
    return !value.empty () && value.size () <= 255 && valid_utf8 (value);
}

void require_spot_id (std::string_view value)
{
    if (!valid_spot_id (value))
        throw std::invalid_argument (
          "SpotId must contain 1..255 bytes of valid UTF-8");
}

spot_id_t new_user_spot_id ()
{
    return uuid_v4 ();
}

spot_id_t new_entry_spot_id (std::string_view diagnostic_prefix)
{
    std::string value (diagnostic_prefix);
    value += "-entry-";
    value += uuid_v4 ();
    require_spot_id (value);
    return value;
}

bool is_framework_entry_spot_id (std::string_view value) noexcept
{
    const auto marker = value.rfind ("-entry-");
    if (marker == std::string_view::npos
        || value.size () - marker - 7 != 36)
        return false;
    const auto uuid = value.substr (marker + 7);
    for (std::size_t index = 0; index < uuid.size (); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (uuid[index] != '-')
                return false;
            continue;
        }
        const auto ch = uuid[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return uuid[14] == '4'
           && (uuid[19] == '8' || uuid[19] == '9'
               || uuid[19] == 'a' || uuid[19] == 'b');
}
} // namespace detail

} // namespace zlink::framework

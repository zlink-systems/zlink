/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/utils/uuid.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace zlink::framework::detail
{

std::string new_uuid_v4 ()
{
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto &byte : bytes)
        byte = static_cast<std::uint8_t> (random ());
    bytes[6] = static_cast<std::uint8_t> ((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<std::uint8_t> ((bytes[8] & 0x3fu) | 0x80u);

    std::ostringstream text;
    text << std::hex << std::nouppercase << std::setfill ('0');
    for (std::size_t index = 0; index < bytes.size (); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text << '-';
        text << std::setw (2) << static_cast<unsigned int> (bytes[index]);
    }
    return text.str ();
}

} // namespace zlink::framework::detail

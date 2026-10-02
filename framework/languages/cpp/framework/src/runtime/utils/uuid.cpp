/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/utils/uuid.hpp"

#include <array>
#include <cstdint>
#include <random>

namespace zlink::framework::detail
{

std::string new_uuid_v4 ()
{
    std::array<std::uint8_t, uuid_byte_length> bytes{};
    std::random_device random;
    for (auto &byte : bytes)
        byte = static_cast<std::uint8_t> (random ());
    bytes[6] = static_cast<std::uint8_t> ((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<std::uint8_t> ((bytes[8] & 0x3fu) | 0x80u);

    return encode_uuid (bytes);
}

} // namespace zlink::framework::detail

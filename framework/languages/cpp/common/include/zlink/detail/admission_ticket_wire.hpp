/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>

namespace zlink::detail
{

constexpr std::size_t unpadded_base64url_size (std::size_t byte_count) noexcept
{
    return (byte_count * 4 + 2) / 3;
}

inline constexpr std::size_t admission_ticket_header_size = 1 + 8 + 8;
inline constexpr std::size_t admission_ticket_mac_size = 32;
inline constexpr std::size_t admission_ticket_size =
  admission_ticket_header_size + admission_ticket_mac_size;
inline constexpr std::size_t admission_ticket_encoded_size =
  unpadded_base64url_size (admission_ticket_size);

} // namespace zlink::detail

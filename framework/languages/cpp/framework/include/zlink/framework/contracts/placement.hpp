/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework
{

namespace detail
{

inline bool is_valid_contract_utf8 (std::string_view value) noexcept
{
    const auto *bytes =
      reinterpret_cast<const unsigned char *> (value.data ());
    std::size_t index = 0;
    while (index < value.size ()) {
        const auto first = bytes[index++];
        if (first <= 0x7f) {
            if (first == 0)
                return false;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0) == 0xc0) {
            continuation_count = 1;
            code_point = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            continuation_count = 2;
            code_point = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            continuation_count = 3;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (index + continuation_count > value.size ())
            return false;
        for (std::size_t offset = 0; offset < continuation_count; ++offset) {
            const auto next = bytes[index++];
            if ((next & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if ((continuation_count == 1 && code_point < 0x80)
            || (continuation_count == 2 && code_point < 0x800)
            || (continuation_count == 3 && code_point < 0x10000)
            || code_point > 0x10ffff
            || (code_point >= 0xd800 && code_point <= 0xdfff))
            return false;
    }
    return true;
}

inline std::string placement_value (std::string value,
                                    std::string_view name)
{
    if (value.empty () || value.size () > 255
        || !is_valid_contract_utf8 (value))
        throw std::invalid_argument (
          std::string (name)
          + " must contain 1..255 non-NUL UTF-8 bytes");
    return value;
}

} // namespace detail

} // namespace zlink::framework

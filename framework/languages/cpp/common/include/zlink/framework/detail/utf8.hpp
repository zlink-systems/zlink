/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zlink::framework::detail
{

inline bool is_valid_utf8 (std::string_view value) noexcept
{
    const auto *bytes = reinterpret_cast<const unsigned char *> (value.data ());
    std::size_t index = 0;
    while (index < value.size ()) {
        const auto first = bytes[index++];
        if (first <= 0x7f)
            continue;

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            code_point = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            code_point = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            code_point = first & 0x07;
            minimum = 0x10000;
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
        if (code_point < minimum || code_point > 0x10ffff
            || (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

inline bool is_valid_non_nul_utf8 (std::string_view value) noexcept
{
    return value.find ('\0') == std::string_view::npos && is_valid_utf8 (value);
}

} // namespace zlink::framework::detail

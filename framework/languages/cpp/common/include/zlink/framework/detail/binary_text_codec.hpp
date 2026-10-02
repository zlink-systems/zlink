/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace zlink::framework::detail
{

inline constexpr std::size_t uuid_byte_length = 16;
inline constexpr std::size_t uuid_hex_length = uuid_byte_length * 2;
inline constexpr std::size_t uuid_text_length = uuid_hex_length + 4;
inline constexpr std::array<std::size_t, 4> uuid_byte_separators{4, 6, 8, 10};
inline constexpr auto uuid_text_separators = [] {
    auto result = uuid_byte_separators;
    for (std::size_t index = 0; index < result.size (); ++index)
        result[index] = result[index] * 2 + index;
    return result;
}();

inline constexpr char lowercase_hex_digits[] = "0123456789abcdef";
template <typename T> inline std::string encode_hex_bytes (std::span<const T> bytes)
{
    std::string result;
    result.reserve (bytes.size () * 2);
    for (const auto input : bytes) {
        const auto byte = static_cast<std::uint8_t> (input);
        result.push_back (lowercase_hex_digits[byte >> 4u]);
        result.push_back (lowercase_hex_digits[byte & 0x0fu]);
    }
    return result;
}

inline std::string encode_hex (std::span<const std::uint8_t> bytes)
{
    return encode_hex_bytes (bytes);
}
inline std::string encode_hex (std::span<const std::byte> bytes)
{
    return encode_hex_bytes (bytes);
}

inline std::string encode_uuid (std::span<const std::uint8_t, uuid_byte_length> bytes)
{
    std::string result;
    result.reserve (uuid_text_length);
    for (std::size_t index = 0; index < bytes.size (); ++index) {
        if (std::find (uuid_byte_separators.begin (), uuid_byte_separators.end (), index)
            != uuid_byte_separators.end ())
            result.push_back ('-');
        result.push_back (lowercase_hex_digits[bytes[index] >> 4u]);
        result.push_back (lowercase_hex_digits[bytes[index] & 0x0fu]);
    }
    return result;
}

inline bool is_lowercase_uuid_text (std::string_view value) noexcept
{
    if (value.size () != uuid_text_length)
        return false;
    for (std::size_t index = 0; index < value.size (); ++index) {
        const auto ch = value[index];
        if (std::find (uuid_text_separators.begin (), uuid_text_separators.end (), index)
            != uuid_text_separators.end ()) {
            if (ch != '-')
                return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace zlink::framework::detail
